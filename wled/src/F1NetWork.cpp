/*
 * F1NetWork.cpp  –  SignalR client for F1 live-timing.
 *
 * *** CRITICAL: DO NOT include wled.h or any header that includes     ***
 * *** ESPAsyncWebServer.h in this translation unit.                   ***
 * *** ESPAsyncWebServer defines HTTP_GET/DELETE/etc. as C enum values ***
 * *** that conflict with the same identifiers in esp-idf/http_parser. ***
 *
 * Uses raw mbedTLS + lwIP sockets (same approach as F1Sessions.cpp)
 * because esp_tls has TLS compiled out in this SDK build
 * (CONFIG_MBEDTLS_TLS_DISABLED=1).
 *
 * Protocol flow:
 *   HTTPS GET /signalr/negotiate  →  ConnectionToken + ALB cookies
 *   HTTPS upgrade  →  wss://  WebSocket (RFC 6455, TLS)
 *                   (forwarding ALB cookies from negotiate)
 *   Send SignalR subscribe frame
 *   Receive TrackStatus / SessionStatus push frames
 *   Ping every 15 s (SignalR keepalive)
 *   Reconnect with exponential back-off on disconnect
 */

#include <WiFi.h>           /* Arduino WiFi                            */
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <Arduino.h>        /* millis(), Serial                        */

/* raw mbedTLS */
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"

/* lwIP raw sockets */
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>

/* FreeRTOS */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "F1NetWork.h"
#include "F1Sessions.h"
#include "F1Calendar.h"
#include "F1StringUtils.h"  /* f1_url_encode, f1_json_str */
#include "F1TimeUtils.h"    /* f1_clock() – clean 32-bit wall clock */
#include "Config.h"         /* g_cfg.react_* – which events the LED reacts to */

/* ----------------------------------------------------------------
   Tunables
   ---------------------------------------------------------------- */
static constexpr uint32_t RECONNECT_INIT_MS  =   5000;
static constexpr uint32_t RECONNECT_MAX_MS   =  30000;
/* Server KeepAliveTimeout = 20 s (from /signalr/negotiate).
   Send our ping every 15 s so we stay within that window.
   Declare idle after 30 s (1.5× server timeout) – faster failover. */
static constexpr uint32_t PING_INTERVAL_MS   =  15000;   /* SignalR {} ping  */
static constexpr uint32_t TCP_TIMEOUT_MS     =  10000;   /* connect timeout  */
static constexpr uint32_t READ_TIMEOUT_MS    =  30000;   /* idle WS timeout  */

/* ----------------------------------------------------------------
   SignalR endpoints (HTTPS, port 443)
   ---------------------------------------------------------------- */
static constexpr const char* SR_HOST      = "livetiming.formula1.com";
static constexpr int         SR_PORT      = 443;
/* SignalR Core (/signalrcore/) replaced classic SignalR (/signalr/) in 2026.
   Classic endpoint now returns 401.  Core endpoint is open (no auth needed).
   Protocol differences:
     - Negotiate: POST /signalrcore/negotiate?negotiateVersion=1
     - WS URL: /signalrcore?id=<connectionToken>
     - After WS upgrade: send handshake {"protocol":"json","version":1}\x1e
     - Subscribe: {"type":1,...}\x1e
     - Snapshot: type:3 with "result" field (same structure as old "R" field)
     - Push: type:1 with "target":"feed"
     - Ping: {"type":6}\x1e
     - All messages terminated with \x1e (0x1E record separator) */
static constexpr const char* SR_NEGOTIATE = "/signalrcore/negotiate";
static constexpr const char* SR_CONNECT   = "/signalrcore";
static constexpr const char* HANDSHAKE_MSG =
    "{\"protocol\":\"json\",\"version\":1}\x1e";
static constexpr const char* SUBSCRIBE_MSG =
    "{\"type\":1,\"invocationId\":\"0\",\"target\":\"Subscribe\","
    "\"arguments\":[[\"TrackStatus\",\"SessionStatus\",\"Heartbeat\",\"RaceControlMessages\"]]}"
    "\x1e";

/* ----------------------------------------------------------------
   Module state
   ---------------------------------------------------------------- */
static volatile F1NetState  s_state       = F1ST_IDLE;
static volatile bool        s_connected   = false;
static F1NetStateCB         s_callback    = nullptr;
static F1EventCB            s_eventCallback = nullptr;
static F1SnapshotCB         s_snapshotCb   = nullptr;

/* ----------------------------------------------------------------
   TlsConn – mbedTLS + raw lwIP socket (same struct as F1Sessions.cpp)
   ---------------------------------------------------------------- */
struct TlsConn {
    int                      sock;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;
    bool                     connected;
};

static TlsConn*    s_tls         = nullptr;   /* persistent WSS connection    */
static bool        s_wsOpen       = false;
static uint32_t    s_reconnDelay  = RECONNECT_INIT_MS;
static unsigned long s_lastConnect = 0;
static unsigned long s_lastPing    = 0;
static unsigned long s_lastData    = 0;  /* last ws frame received */
static bool        s_sessionActive = false;
static const char* s_connectPhase  = "wait";  /* human-readable phase */
static char        s_lastErr[128]  = {};      /* last connect failure reason */
static bool        s_handshakeAcked = false;  /* server confirmed {}\x1e ack */
static bool        s_handshakeErr   = false;  /* server rejected handshake   */

/* Record the most recent connection failure reason (for /api/status + logs) */
static void setErr(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_lastErr, sizeof(s_lastErr), fmt, ap);
    va_end(ap);
    Serial.printf("[F1Net] ERR: %s\n", s_lastErr);
}

/* ── Live event ring buffer ──────────────────────────────────────────── */
static F1LiveEvent s_eventLog[F1_EVENT_LOG_MAX];
static int         s_evHead    = 0;   /* next write position              */
static int         s_evCount   = 0;   /* total events stored              */
static uint32_t    s_sessEndEpoch = 0; /* epoch when session ended (0=active) */

static void logEvent(const char* category, const char* message) {
    /* Auto-clear if >1h past session end */
    if (s_sessEndEpoch > 0) {
        long now = (long)f1_clock();
        if (now > (long)s_sessEndEpoch + 3600L) {
            s_evCount = 0;
            s_evHead  = 0;
            s_sessEndEpoch = 0;
            return;  /* discard stale events */
        }
    }
    F1LiveEvent& e = s_eventLog[s_evHead];
    e.epoch = (uint32_t)time(nullptr);
    strlcpy(e.category, category, sizeof(e.category));
    strlcpy(e.message, message, sizeof(e.message));
    s_evHead = (s_evHead + 1) % F1_EVENT_LOG_MAX;
    if (s_evCount < F1_EVENT_LOG_MAX) s_evCount++;
}

/* AWS ALB cookies captured during negotiate, forwarded to WS upgrade */
static char s_cookies[512] = {};

/* WS frame reassembly */
static char s_wsBuf[4096];

/* Diagnostics: last frame opcode seen + frame counter (for drop forensics) */
static uint8_t  s_lastOp     = 0xFF;
static uint32_t s_frameCount = 0;

/* ----------------------------------------------------------------
   Helpers
   ---------------------------------------------------------------- */

/* Thin wrappers – implementations live in F1StringUtils.h for testability */
static char* url_encode(const char* src, char* dst, size_t dst_size)
{
    return f1_url_encode(src, dst, dst_size);
}

static bool json_str(const char* json, const char* key,
                     char* out, size_t out_size)
{
    return f1_json_str(json, key, out, out_size);
}

/* Case-insensitive substring test (avoids strcasestr availability issues) */
static bool f1_contains_ci(const char* hay, const char* needle)
{
    if (!hay || !needle || !needle[0]) return false;
    size_t nl = strlen(needle);
    for (const char* p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i]))
            ++i;
        if (i == nl) return true;
    }
    return false;
}

/* Decide which LED state a RaceControl message drives, honouring the
   user's g_cfg.react_* switches.  Returns F1ST_UNKNOWN when the message
   should not change the LED.  Green/clear is deliberately left to the
   TrackStatus code 1, because RC "TRACK CLEAR"-style notes proved
   ambiguous in the live feed. */
static F1NetState rcEventToState(const char* cat, const char* flag,
                                 const char* scope, const char* text)
{
    if (!g_cfg.react_global && !g_cfg.react_sector) return F1ST_UNKNOWN;

    /* Global announcements (these are what the TV mirrors) */
    if (g_cfg.react_global && text && text[0]) {
        if (f1_contains_ci(text, "RED FLAG"))     return F1ST_RED_FLAG;
        if (f1_contains_ci(text, "VIRTUAL SAFETY CAR")
                || f1_contains_ci(text, "VSC"))
            return F1ST_VIRTUAL_SC;
        if (f1_contains_ci(text, "SAFETY CAR"))   return F1ST_SAFETY_CAR;
        if (f1_contains_ci(text, "CHEQUERED"))    return F1ST_CHEQUERED;
    }

    bool isFlag = (cat && cat[0] && strcasecmp(cat, "Flag") == 0);
    if (!isFlag) return F1ST_UNKNOWN;

    bool isSector = (scope && strcasecmp(scope, "Sector") == 0);
    if (isSector) {
        /* Sector-local yellows only when the user opted in */
        if (g_cfg.react_sector && flag && f1_contains_ci(flag, "YELLOW"))
            return F1ST_YELLOW;
        return F1ST_UNKNOWN;
    }
    /* Track-scope flags */
    if (g_cfg.react_global) {
        if (flag && f1_contains_ci(flag, "RED"))    return F1ST_RED_FLAG;
        if (flag && f1_contains_ci(flag, "YELLOW")) return F1ST_YELLOW;
    }
    return F1ST_UNKNOWN;
}

/* Map TrackStatus code → F1NetState */
static F1NetState trackCodeToState(const char* code)
{
    if (!code || !code[0]) return F1ST_UNKNOWN;
    switch (code[0]) {
        case '1': return F1ST_GREEN;
        case '2': return F1ST_YELLOW;
        case '3': return F1ST_YELLOW;      /* Flag / yellow variant           */
        case '4': return F1ST_SAFETY_CAR;
        case '5': return F1ST_RED_FLAG;
        case '6': return F1ST_VIRTUAL_SC;
        case '7': return F1ST_VSC_ENDING;  /* VSC ending – brief transition to green */
        default:  return F1ST_UNKNOWN;
    }
}

/* Push new state to WLED layer */
static void applyState(F1NetState ns)
{
    if (ns == F1ST_UNKNOWN) return;
    if (ns == s_state)      return;
    s_state = ns;
    if (s_callback) s_callback(ns);
}

/* forward declaration – defined in TLS helpers block below */
static void tls_cleanup(TlsConn& c);

/* Schedule reconnect with back-off */
static void scheduleReconnect(const char* reason = "unknown")
{
    Serial.printf("[F1Net] scheduleReconnect reason=%s t=%lu lastOp=%u "
                  "frames=%lu dataAge=%lums\n",
                  reason, (unsigned long)millis(),
                  (unsigned)s_lastOp, (unsigned long)s_frameCount,
                  s_lastData ? (unsigned long)(millis() - s_lastData) : 0UL);
    s_wsOpen     = false;
    s_connected  = false;
    if (s_tls) {
        tls_cleanup(*s_tls);
        delete s_tls;
        s_tls = nullptr;
    }
    s_lastConnect = millis();
    if (s_reconnDelay < RECONNECT_MAX_MS)
        s_reconnDelay = (s_reconnDelay < RECONNECT_MAX_MS / 2)
                        ? s_reconnDelay * 2 : RECONNECT_MAX_MS;
    Serial.printf("[F1Net] Reconnect in %ums\n", s_reconnDelay);
}

/* ----------------------------------------------------------------
   TLS I/O helpers  (mbedTLS + lwIP sockets)
   ---------------------------------------------------------------- */

/* BIO callbacks */
static int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    int fd = *(int*)ctx;
    int r = lwip_send(fd, buf, len, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return r;
}

static int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    int fd = *(int*)ctx;
    int r = lwip_recv(fd, buf, len, 0);
    if (r < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return r;
}

static void tls_cleanup(TlsConn& c)
{
    if (c.connected) mbedtls_ssl_close_notify(&c.ssl);
    mbedtls_ssl_free(&c.ssl);
    mbedtls_ssl_config_free(&c.conf);
    mbedtls_ctr_drbg_free(&c.ctr_drbg);
    mbedtls_entropy_free(&c.entropy);
    if (c.sock >= 0) { lwip_close(c.sock); c.sock = -1; }
    c.connected = false;
}

/* Open a fresh TLS connection to SR_HOST:SR_PORT */
static bool tls_connect(TlsConn& c)
{
    char errbuf[128];
    c.sock = -1;
    c.connected = false;
    Serial.printf("[F1Net] tls_connect() %s:%d heap=%u\n",
                  SR_HOST, SR_PORT, (unsigned)esp_get_free_heap_size());

    mbedtls_ssl_init(&c.ssl);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_ctr_drbg_init(&c.ctr_drbg);
    mbedtls_entropy_init(&c.entropy);

    int ret = mbedtls_ctr_drbg_seed(&c.ctr_drbg, mbedtls_entropy_func,
                                      &c.entropy, nullptr, 0);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        Serial.printf("[F1Net] DRBG seed: %s\n", errbuf);
        tls_cleanup(c); return false;
    }
    ret = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        Serial.printf("[F1Net] SSL config: %s\n", errbuf);
        tls_cleanup(c); return false;
    }
    mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.ctr_drbg);

    ret = mbedtls_ssl_setup(&c.ssl, &c.conf);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        Serial.printf("[F1Net] SSL setup: %s\n", errbuf);
        tls_cleanup(c); return false;
    }
    ret = mbedtls_ssl_set_hostname(&c.ssl, SR_HOST);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        Serial.printf("[F1Net] set_hostname: %s\n", errbuf);
        tls_cleanup(c); return false;
    }

    /* TCP connect */
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", SR_PORT);
    ret = lwip_getaddrinfo(SR_HOST, portStr, &hints, &res);
    if (ret != 0 || !res) {
        setErr("DNS failed for %s: %d", SR_HOST, ret);
        tls_cleanup(c); return false;
    }
    {
        struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
        Serial.printf("[F1Net] Resolved %s -> %s\n", SR_HOST, inet_ntoa(sa->sin_addr));
    }
    bool tcpOk = false;
    for (int attempt = 0; attempt < 3 && !tcpOk; attempt++) {
        if (attempt > 0) {
            Serial.printf("[F1Net] TCP retry %d/3...\n", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        c.sock = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (c.sock < 0) continue;
        struct timeval tv;
        tv.tv_sec  = TCP_TIMEOUT_MS / 1000;
        tv.tv_usec = (TCP_TIMEOUT_MS % 1000) * 1000;
        lwip_setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(c.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (lwip_connect(c.sock, res->ai_addr, res->ai_addrlen) == 0) {
            tcpOk = true;
            Serial.printf("[F1Net] TCP connect OK (fd=%d)\n", c.sock);
        } else {
            setErr("TCP connect failed: %d", errno);
            lwip_close(c.sock); c.sock = -1;
        }
    }
    lwip_freeaddrinfo(res);
    if (!tcpOk) {
        setErr("TCP connect failed after retries");
        tls_cleanup(c); return false;
    }

    mbedtls_ssl_set_bio(&c.ssl, &c.sock, bio_send, bio_recv, nullptr);

    unsigned long t0 = millis();
    while ((ret = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            Serial.printf("[F1Net] TLS handshake failed: %s (0x%x)\n", errbuf, -ret);
            tls_cleanup(c); return false;
        }
        if (millis() - t0 > TCP_TIMEOUT_MS) {
            Serial.println("[F1Net] TLS handshake timeout");
            tls_cleanup(c); return false;
        }
        delay(1);
    }
    c.connected = true;
    Serial.printf("[F1Net] TLS handshake OK  ver=%s cipher=%s\n",
                  mbedtls_ssl_get_version(&c.ssl),
                  mbedtls_ssl_get_ciphersuite(&c.ssl));
    s_lastErr[0] = '\0';   /* clear last error on success */
    return true;
}

/* Write all bytes over TLS */
static bool tls_write_all(TlsConn& c, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    unsigned long t0 = millis();
    while (sent < len && millis() - t0 < TCP_TIMEOUT_MS) {
        int r = mbedtls_ssl_write(&c.ssl, p + sent, len - sent);
        if (r > 0) {
            sent += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            delay(1);
        } else {
            Serial.printf("[F1Net] TLS write error: 0x%x\n", -r);
            return false;
        }
    }
    return sent == len;
}

/* Put the socket in non-blocking mode so mbedtls_ssl_read() returns
 * MBEDTLS_ERR_SSL_WANT_READ immediately when no data has arrived.  We
 * deliberately avoid select()/ioctl(FIONREAD) readiness checks: the
 * descriptors here are large (fd ~ 48) and lwIP's fd_set is small, so
 * those checks never report readability on this build. */
static void tls_set_nonblocking(TlsConn& c)
{
    if (c.sock < 0) return;
    int fl = lwip_fcntl(c.sock, F_GETFL, 0);
    if (fl >= 0) lwip_fcntl(c.sock, F_SETFL, fl | O_NONBLOCK);
}

/* Read exactly 'want' bytes from the TLS stream.
 *
 * Returns:
 *    1  - success (all 'want' bytes read)
 *    0  - would block with NO bytes read yet (soft; caller retries later;
 *         only legal when allowSoft is true, i.e. before the first byte
 *         of a WS frame has been consumed)
 *   -1  - fatal: peer closed / TLS error / mid-frame stall longer than
 *         stall_ms with allowSoft=false
 */
static int wsReadFull(TlsConn& c, uint8_t* buf, size_t want,
                      uint32_t stall_ms, bool allowSoft)
{
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < want) {
        int r = mbedtls_ssl_read(&c.ssl, buf + got, want - got);
        if (r > 0) {
            got += (size_t)r;
            t0 = millis();            /* reset stall timer on progress */
            continue;
        }
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -1;
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (got == 0 && allowSoft) return 0;   /* nothing yet - retry later */
            if ((int32_t)(millis() - t0) > (int32_t)stall_ms) return -1;
            delay(1);
            continue;
        }
        return -1;                    /* real TLS error */
    }
    return 1;
}

/* Read exactly 'len' bytes with timeout (used by the HTTP negotiate path,
   which expects a full body).  Returns got bytes or -1 on error/timeout. */
static int tls_read_exact(TlsConn& c, uint8_t* buf, size_t len,
                          uint32_t timeout_ms)
{
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < len && millis() - t0 < timeout_ms) {
        int r = mbedtls_ssl_read(&c.ssl, buf + got, len - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return -1;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            delay(1);
        } else {
            return -1;
        }
    }
    return (int)got;
}

/* ----------------------------------------------------------------
   Extract Set-Cookie headers into "name=val; name2=val2" string
   ---------------------------------------------------------------- */
static void extractCookies(const char* headers, char* out, size_t outLen)
{
    out[0] = '\0';
    size_t pos = 0;
    const char* p = headers;

    while (p && *p) {
        /* Case-insensitive search for "Set-Cookie:" */
        const char* sc = strstr(p, "Set-Cookie:");
        if (!sc) sc = strstr(p, "set-cookie:");
        if (!sc) break;
        sc += 11;  /* skip "Set-Cookie:" */
        while (*sc == ' ') ++sc;

        /* Copy name=value until ';' or end-of-line */
        const char* end = sc;
        while (*end && *end != ';' && *end != '\r' && *end != '\n') ++end;
        size_t cookieLen = (size_t)(end - sc);

        if (pos + cookieLen + 3 < outLen) {
            if (pos > 0) { out[pos++] = ';'; out[pos++] = ' '; }
            memcpy(out + pos, sc, cookieLen);
            pos += cookieLen;
        }
        out[pos] = '\0';
        p = end;
    }
    if (pos > 0) Serial.printf("[F1Net] Cookies: %s\n", out);
}

/* ----------------------------------------------------------------
   WebSocket framing (RFC 6455, client-side only, no fragmentation)
   ---------------------------------------------------------------- */

/* Send a text frame from client (mask bit set, 4-byte mask).
   Returns true when the entire frame reached the TLS stream. */
static bool ws_send_text(TlsConn& c, const char* payload)
{
    size_t len = strlen(payload);
    uint8_t hdr[14];
    size_t hdrLen = 0;

    hdr[hdrLen++] = 0x81;  /* FIN + opcode=1 (text) */
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};  /* fixed mask is fine */

    if (len <= 125) {
        hdr[hdrLen++] = (uint8_t)(0x80 | len);
    } else if (len <= 65535) {
        hdr[hdrLen++] = 0x80 | 126;
        hdr[hdrLen++] = (uint8_t)(len >> 8);
        hdr[hdrLen++] = (uint8_t)(len & 0xFF);
    } else {
        hdr[hdrLen++] = 0x80 | 127;
        for (int i = 7; i >= 0; --i)
            hdr[hdrLen++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    }
    memcpy(hdr + hdrLen, mask, 4);
    hdrLen += 4;

    if (!tls_write_all(c, hdr, hdrLen)) return false;

    /* Write masked payload */
    char masked[256];
    const char* p = payload;
    size_t remaining = len;
    size_t mi = 0;
    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(masked)) ? sizeof(masked) : remaining;
        for (size_t i = 0; i < chunk; ++i)
            masked[i] = p[i] ^ mask[mi++ & 3];
        if (!tls_write_all(c, masked, chunk)) return false;
        p += chunk;
        remaining -= chunk;
    }
    return true;
}

/* ----------------------------------------------------------------
   HTTPS negotiate  (SignalR Core: POST, captures ALB cookie)
   ---------------------------------------------------------------- */
static bool negotiate(char* outToken, size_t tokenLen)
{
    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) { Serial.println("[F1Net] OOM negotiate"); return false; }

    if (!tls_connect(*cp)) {
        Serial.println("[F1Net] Neg TLS connect fail");
        delete cp;
        return false;
    }

    /* SignalR Core negotiate: POST with empty body */
    static char req[512];
    snprintf(req, sizeof(req),
        "POST %s?negotiateVersion=1 HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BestHTTP\r\n"
        "Content-Length: 0\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        SR_NEGOTIATE, SR_HOST);

    if (!tls_write_all(*cp, req, strlen(req))) {
        Serial.println("[F1Net] Neg write fail");
        tls_cleanup(*cp); delete cp;
        return false;
    }

    /* Read entire response (Connection: close → server closes when done) */
    static char resp[2048];
    int respLen = 0;
    unsigned long t0 = millis();
    while (respLen < (int)sizeof(resp) - 1 && millis() - t0 < TCP_TIMEOUT_MS) {
        int r = mbedtls_ssl_read(&cp->ssl,
                    (unsigned char*)resp + respLen,
                    sizeof(resp) - 1 - respLen);
        if (r > 0) {
            respLen += r;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            delay(1);
        } else {
            break;
        }
    }
    resp[respLen] = '\0';
    tls_cleanup(*cp); delete cp;

    /* Parse status line */
    int statusCode = 0;
    if (sscanf(resp, "HTTP/1.1 %d", &statusCode) != 1 || statusCode != 200) {
        setErr("Neg HTTP %d: %.80s", statusCode, resp);
        return false;
    }

    /* Capture ALB cookies from response headers */
    extractCookies(resp, s_cookies, sizeof(s_cookies));

    /* Find body (after \r\n\r\n) */
    const char* body = strstr(resp, "\r\n\r\n");
    if (!body) { Serial.println("[F1Net] No body"); return false; }
    body += 4;

    /* SignalR Core uses lowercase "connectionToken" */
    if (!json_str(body, "connectionToken", outToken, tokenLen)) {
        Serial.println("[F1Net] No connectionToken");
        return false;
    }
    Serial.printf("[F1Net] Token len=%d\n", (int)strlen(outToken));
    return true;
}

/* ----------------------------------------------------------------
   WebSocket connect + upgrade  (SignalR Core: /signalrcore?id=<token>)
   ---------------------------------------------------------------- */
static bool wsConnect(const char* token)
{
    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) { Serial.println("[F1Net] OOM wsConnect"); return false; }

    if (!tls_connect(*cp)) {
        Serial.println("[F1Net] WS TLS connect fail");
        delete cp;
        return false;
    }

    /* SignalR Core WS URL: /signalrcore?id=<connectionToken>
       Token is short alphanumeric (URL-safe), no encoding needed */
    static char path[256];
    snprintf(path, sizeof(path), "%s?id=%s", SR_CONNECT, token);

    /* RFC 6455 upgrade request – include ALB sticky-session cookie */
    static char req[1024];
    if (s_cookies[0]) {
        snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: BestHTTP\r\n"
            "Cookie: %s\r\n"
            "\r\n",
            path, SR_HOST, s_cookies);
    } else {
        snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "User-Agent: BestHTTP\r\n"
            "\r\n",
            path, SR_HOST);
    }

    if (!tls_write_all(*cp, req, strlen(req))) {
        Serial.println("[F1Net] WS upgrade write fail");
        tls_cleanup(*cp); delete cp;
        return false;
    }

    /* Wait for 101 Switching Protocols */
    static char resp[1024];
    int rlen = 0;
    unsigned long t0 = millis();
    while (rlen < (int)sizeof(resp) - 1 && millis() - t0 < TCP_TIMEOUT_MS) {
        int r = mbedtls_ssl_read(&cp->ssl,
                    (unsigned char*)resp + rlen,
                    sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            break;
        } else {
            delay(1);
        }
    }
    resp[rlen] = '\0';

    if (!strstr(resp, "101")) {
        setErr("WS upgrade: %.100s", resp);
        tls_cleanup(*cp); delete cp;
        return false;
    }
    Serial.println("[F1Net] WS upgrade 101 OK");

    /* SignalR Core protocol handshake: must be sent as a proper masked
       WebSocket text frame (RFC 6455), exactly like every other protocol
       message.  Sending it as raw TLS bytes makes the server reply with a
       WS close frame 1002 (protocol error) and drop the connection. */
    if (!ws_send_text(*cp, HANDSHAKE_MSG)) {
        Serial.println("[F1Net] Handshake write fail");
        tls_cleanup(*cp); delete cp;
        return false;
    }

    /* Persistent socket must be non-blocking so wsRead() can poll for
       frames without select()/FIONREAD (unreliable on this build). */
    tls_set_nonblocking(*cp);

    /* Persist connection for ongoing WS use.  The {}\x1e handshake ack is
       verified by connectSignalR() through the normal frame reader. */
    s_tls = cp;
    s_handshakeAcked = false;
    s_handshakeErr   = false;
    Serial.println("[F1Net] WSS connected (handshake sent)");
    return true;
}

/* Walk the RaceControlMessages backlog carried inside a (re)connect
   type:3 snapshot and apply LED reactions from the most recent
   messages.  This lets a device that boots (or reconnects) in the
   middle of an incident show the correct flag state (e.g. red) even
   though TrackStatus is stuck on code "2" (Yellow).  The messages are
   processed oldest → newest so the LAST one wins; nothing is written
   to the event log (that would replay "all past events"). */
static void processSnapshotRcBacklog(const char* msg)
{
    if (!g_cfg.react_global && !g_cfg.react_sector) return;
    const char* rc = strstr(msg, "\"RaceControlMessages\"");
    if (!rc) return;
    const char* arr = strchr(rc, '[');
    if (!arr) return;

    struct Back { char cat[16], flag[16], scope[16], text[F1_EVENT_MSG_LEN]; };
    static Back list[8];
    int n = 0;   /* total messages seen */
    const char* p = arr + 1;
    while (p && *p && *p != ']') {
        p = strchr(p, '{');
        if (!p) break;
        /* find matching closing brace (messages contain no nested braces) */
        const char* q = p;
        int depth = 0;
        while (*q) {
            if (*q == '{') ++depth;
            else if (*q == '}') { if (--depth == 0) { ++q; break; } }
            ++q;
        }
        if (depth != 0) break;
        size_t sl = (size_t)(q - p);
        if (sl > 700) sl = 700;
        static char buf[704];
        memcpy(buf, p, sl);
        buf[sl] = '\0';

        Back b;
        b.cat[0] = b.flag[0] = b.scope[0] = b.text[0] = '\0';
        json_str(buf, "Category", b.cat, sizeof(b.cat));
        json_str(buf, "Flag",     b.flag, sizeof(b.flag));
        json_str(buf, "Scope",    b.scope, sizeof(b.scope));
        json_str(buf, "Message",  b.text, sizeof(b.text));
        list[n % 8] = b;      /* rolling window – keeps the newest 8 */
        ++n;
        p = q;
    }

    Serial.printf("[F1Net] Snapshot RC backlog: %d messages\n", n);
    int start = (n > 8) ? (n - 8) : 0;
    for (int i = start; i < n; i++) {
        const Back* bi = &list[i % 8];
        F1NetState st = rcEventToState(bi->cat, bi->flag,
                                       bi->scope, bi->text);
        if (st != F1ST_UNKNOWN) {
            Serial.printf("[F1Net] backlog -> state %d (%s)\n",
                          (int)st, bi->text);
            applyState(st);
        }
    }
}

/* ----------------------------------------------------------------
   Process one complete SignalR message
   ---------------------------------------------------------------- */
static void processMessage(const char* msg, int len)
{
    /* Ignore empty or whitespace-only (keepalive) */
    if (len <= 2) return;

    /* Handshake acknowledgement: the server answers the handshake with a
       short "{}\x1e" text frame.  Record it so connectSignalR() can
       subscribe only after the protocol handshake completed. */
    if (strstr(msg, "{}")) {
        s_handshakeAcked = true;
        Serial.println("[F1Net] Handshake ack received");
        return;
    }
    /* Server-side handshake rejection ({"error":...}) */
    if (strstr(msg, "\"error\"")) {
        s_handshakeErr = true;
        setErr("WS handshake rejected: %.80s", msg);
        return;
    }

    /* ── SignalR Core type:6 ping → reply with ping ─────────────────── */
    if (strstr(msg, "\"type\":6")) {
        ws_send_text(*s_tls, "{\"type\":6}\x1e");
        return;
    }

    /* ── SignalR Core type:7 close ───────────────────────────────────── */
    if (strstr(msg, "\"type\":7")) {
        Serial.println("[F1Net] WS close recv (type:7)");
        scheduleReconnect("type7");
        return;
    }


    /* ── SignalR Core type:3  (Completion / initial snapshot) ─────────
       {"type":3,"invocationId":"0","result":{"TrackStatus":{...},"SessionStatus":{...},...}}
       The "result" field has the same structure as the old "R" field.     */
    const char* resultPtr = strstr(msg, "\"result\":");
    if (resultPtr && strstr(msg, "\"type\":3")) {
        resultPtr += 9;
        while (*resultPtr == ' ') ++resultPtr;
        if (*resultPtr == '{') {
            /* Fresh (re)connect snapshot: tell the consumer to drop any
               stale queued states first, so only the CURRENT on-track
               state is applied after a boot/reconnect. */
            if (s_snapshotCb) s_snapshotCb();

            const char* tsp = strstr(resultPtr, "\"TrackStatus\"");
            if (tsp) {
                char code[8] = {};
                json_str(tsp, "Status", code, sizeof(code));
                if (code[0]) {
                    F1NetState ns = trackCodeToState(code);
                    char tmsg[24] = {};
                    json_str(tsp, "Message", tmsg, sizeof(tmsg));
                    Serial.printf("[F1Net] Snapshot TrackStatus=%s (%s)\n",
                                  code, tmsg[0] ? tmsg : "?");
                    /* NOTE: not written to the event log on purpose – a
                       snapshot reflects the state AT BOOT/CONNECT, not a
                       new event, and showing it with the boot timestamp
                       misleads (e.g. "Green" at 10:45 when the flag
                       actually changed an hour earlier). */
                    if (g_cfg.react_track) applyState(ns);
                }
            }
            const char* ssp = strstr(resultPtr, "\"SessionStatus\"");
            if (ssp) {
                char status[32] = {};
                json_str(ssp, "Status", status, sizeof(status));
                if (status[0]) {
                    Serial.printf("[F1Net] Snapshot SessionStatus=%s\n", status);
                    if (strstr(status, "Started")) {
                        s_sessionActive = true;
                        s_sessEndEpoch  = 0;
                    } else if (strstr(status, "Finished") || strstr(status, "Ends")) {
                        s_sessEndEpoch = (uint32_t)time(nullptr);
                        applyState(F1ST_CHEQUERED);
                    } else if (strstr(status, "Inactive")) {
                        s_sessionActive = false;
                        if (s_sessEndEpoch == 0)
                            s_sessEndEpoch = (uint32_t)time(nullptr);
                        applyState(F1ST_IDLE);
                    }
                }
            }
        }
        /* Re-derive the flag state from the snapshot's race-control
           backlog (most recent messages win), so booting mid-incident
           shows the correct colour. */
        processSnapshotRcBacklog(msg);
        return;  /* snapshot fully handled */
    }

    /* ── SignalR Core type:1 with target "feed" (live push) ───────────
       {"type":1,"target":"feed","arguments":["TopicName",<timestamp>,<data>]}
       arguments[0] = topic string
       arguments[1..] = timestamp string + data object (order may vary)     */
    if (!strstr(msg, "\"type\":1") || !strstr(msg, "\"feed\"")) return;

    const char* ap = strstr(msg, "\"arguments\":");
    if (!ap) return;
    ap += 12;
    while (*ap == ' ') ++ap;
    if (*ap != '[') return;
    ++ap;  /* skip '[' */
    while (*ap == ' ') ++ap;

    /* First element: topic name string */
    if (*ap != '"') return;
    char topic[64] = {};
    size_t ti = 0;
    ++ap;  /* skip opening '"' */
    while (*ap && *ap != '"' && ti < sizeof(topic)-1) topic[ti++] = *ap++;
    topic[ti] = '\0';
    if (*ap == '"') ++ap;  /* skip closing '"' */
    /* Skip comma + optional timestamp arg to reach data object/string */
    while (*ap == ',' || *ap == ' ') ++ap;
    /* Skip the timestamp string if present (starts with '"') */
    if (*ap == '"') {
        ++ap;
        while (*ap && *ap != '"') ++ap;
        if (*ap == '"') ++ap;
        while (*ap == ',' || *ap == ' ') ++ap;
    }
    /* ap now points to the data argument (object, string, or end of array) */

    if (strcasecmp(topic, "TrackStatus") == 0) {
        char code[8] = {};
        char tmsg[24] = {};
        json_str(ap, "Status", code, sizeof(code));
        if (code[0]) {
            json_str(ap, "Message", tmsg, sizeof(tmsg));
            F1NetState ns = trackCodeToState(code);
            Serial.printf("[F1Net] TrackStatus=%s (%s)\n", code,
                          tmsg[0] ? tmsg : "?");
            static const char* TRK_NAMES[] = {
                "Idle","Green","Yellow","?","Safety Car",
                "Red Flag","VSC","VSC Ending"};
            int ci = code[0] - '0';
            const char* tn = (ci>=0 && ci<=7) ? TRK_NAMES[ci] : code;
            char lmsg[F1_EVENT_MSG_LEN];
            snprintf(lmsg, sizeof(lmsg), "Track: %s", tn);
            logEvent("Track", lmsg);
            /* LED reacts to TrackStatus codes only when enabled */
            if (g_cfg.react_track) applyState(ns);
        }
    }
    else if (strcasecmp(topic, "SessionStatus") == 0) {
        char status[32] = {};
        json_str(ap, "Status", status, sizeof(status));
        Serial.printf("[F1Net] SessionStatus=%s\n", status);
        char smsg[F1_EVENT_MSG_LEN];
        snprintf(smsg, sizeof(smsg), "Session: %s", status);
        logEvent("Session", smsg);
        if (strstr(status, "Started")) {
            s_sessionActive = true;
            s_sessEndEpoch = 0;
            applyState(F1ST_SESSION_START);
        } else if (strstr(status, "Finished") || strstr(status, "Ends")) {
            s_sessEndEpoch = (uint32_t)time(nullptr);
            applyState(F1ST_CHEQUERED);
        } else if (strstr(status, "Inactive")) {
            s_sessionActive = false;
            if (s_sessEndEpoch == 0)
                s_sessEndEpoch = (uint32_t)time(nullptr);
            applyState(F1ST_IDLE);
        }
    }
    else if (strcasecmp(topic, "RaceControlMessages") == 0) {
        /* Structured race-control message.  Live pushes carry a single new
           message:
             {"Messages":{"<id>":{"Category":"Flag","Flag":"YELLOW",
              "Scope":"Sector","Message":"YELLOW IN TRACK SECTOR 13",...}}} */
        char cat[16] = {}, flag[16] = {}, scope[16] = {};
        char rcMsg[F1_EVENT_MSG_LEN] = {};
        json_str(ap, "Category", cat, sizeof(cat));
        json_str(ap, "Flag",     flag, sizeof(flag));
        json_str(ap, "Scope",    scope, sizeof(scope));
        json_str(ap, "Message",  rcMsg, sizeof(rcMsg));

        /* Log every race-control message (user wants the full picture) */
        if (rcMsg[0]) {
            logEvent("RaceCtrl", rcMsg);
            Serial.printf("[F1Net] RaceCtrl: %s\n", rcMsg);
        }

        /* ── LED flag-state transitions ─────────────────────────────────
           The TrackStatus topic in this feed can stay on code 2 ("Yellow")
           even through safety-car / red-flag periods, so the flag the TV
           mirrors mostly comes from RaceControlMessages.  Derive state
           changes via the user-configurable rcEventToState(). */
        F1NetState rcState = rcEventToState(cat, flag, scope, rcMsg);
        if (rcState != F1ST_UNKNOWN) {
            Serial.printf("[F1Net] RC flag -> state %d\n", (int)rcState);
            applyState(rcState);
        }

        /* Auxiliary event flashes (fastest lap / DRS) – text based */
        if (f1_contains_ci(rcMsg, "FASTEST LAP")) {
            Serial.println("[F1Net] ↯ Fastest lap detected");
            if (s_eventCallback) s_eventCallback(F1EVT_FASTEST_LAP);
        }
        if (f1_contains_ci(rcMsg, "DRS") && f1_contains_ci(rcMsg, "ENABLED")) {
            Serial.println("[F1Net] ↯ DRS enabled");
            if (s_eventCallback) s_eventCallback(F1EVT_DRS_ENABLED);
        }
    }
}

/* ----------------------------------------------------------------
   Read + process pending WS frames (non-blocking, called from loop)
   ---------------------------------------------------------------- */
/* Read + process one complete WS frame from the (non-blocking) stream.
   Returns:
      1  - one frame fully read & dispatched
      0  - nothing available right now (soft; retry next tick)
     -1  - fatal error / close; scheduleReconnect() already issued
 */
static int wsReadOneFrame()
{
    TlsConn& c = *s_tls;
    uint8_t hdr2[2];
    int s = wsReadFull(c, hdr2, 2, 2000, true);
    if (s <= 0) return s;                  /* 0 = no data yet, -1 = fatal */

    uint8_t op  = hdr2[0] & 0x0F;
    bool masked = (hdr2[1] & 0x80) != 0;
    uint64_t payLen = hdr2[1] & 0x7F;

    /* Extended length */
    if (payLen == 126) {
        uint8_t ext[2];
        if (wsReadFull(c, ext, 2, 2000, false) != 1) {
            scheduleReconnect("ext-read"); return -1;
        }
        payLen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (payLen == 127) {
        uint8_t ext[8];
        if (wsReadFull(c, ext, 8, 2000, false) != 1) {
            scheduleReconnect("ext8-read"); return -1;
        }
        payLen = 0;
        for (int i = 0; i < 8; ++i) payLen = (payLen << 8) | ext[i];
    }

    /* Server frames shouldn't be masked, but handle anyway */
    uint8_t mask[4] = {};
    if (masked) {
        if (wsReadFull(c, mask, 4, 2000, false) != 1) {
            scheduleReconnect("mask-read"); return -1;
        }
    }

    /* Read payload */
    if (payLen > sizeof(s_wsBuf) - 1) {
        /* Frame too large for buffer – read what fits, parse it for
           state/session info (TrackStatus & SessionStatus appear early
           in the snapshot), then drain and discard the remainder. */
        size_t toRead = sizeof(s_wsBuf) - 1;
        int r = wsReadFull(c, (uint8_t*)s_wsBuf, toRead, 5000, false);
        if (r != 1) { scheduleReconnect("big-read"); return -1; }
        if (masked) {
            for (size_t i = 0; i < toRead; ++i) s_wsBuf[i] ^= mask[i & 3];
        }
        s_wsBuf[toRead] = '\0';
        Serial.printf("[F1Net] Oversized frame %llu B - parsing first %u B\n",
                      (unsigned long long)payLen, (unsigned)toRead);
        processMessage(s_wsBuf, (int)toRead);
        uint64_t skip = payLen - (uint64_t)toRead;
        uint8_t discard[256];
        while (skip > 0) {
            size_t chunk = (skip > sizeof(discard)) ? sizeof(discard) : (size_t)skip;
            int r2 = wsReadFull(c, discard, chunk, 5000, false);
            if (r2 != 1) { scheduleReconnect("drain-read"); return -1; }
            skip -= (size_t)chunk;
        }
    } else if (payLen > 0) {
        int r = wsReadFull(c, (uint8_t*)s_wsBuf, (size_t)payLen, 5000, false);
        if (r != 1) { scheduleReconnect("payload-read"); return -1; }
        if (masked) {
            for (uint64_t i = 0; i < payLen; ++i) s_wsBuf[i] ^= mask[i & 3];
        }
        s_wsBuf[payLen] = '\0';
    }

    s_lastData = millis();
    s_lastOp   = op;
    ++s_frameCount;

    /* Handle opcodes */
    if (op == 0x8) {
        /* Close */
        Serial.println("[F1Net] WS close recv");
        scheduleReconnect("ws-close");
        return -1;
    }
    if (op == 0x9) {
        /* Ping – send pong */
        uint8_t pong[6] = {0x8A, 0x80, 0x00, 0x00, 0x00, 0x00};
        tls_write_all(*s_tls, pong, sizeof(pong));
    }
    if (op == 0xA) {
        /* Pong – ignore */
    }
    if ((op == 0x1 || op == 0x0) && payLen > 0) {
        /* Text / continuation frame */
        processMessage(s_wsBuf, (int)payLen);
    }
    return 1;
}

/* Read + dispatch pending WS frames.  Non-blocking when the socket is
   idle (mbedtls returns WANT_READ immediately); never spins. */
static void wsRead()
{
    if (!s_tls) return;
    for (int frames = 0; frames < 8; ++frames) {
        int r = wsReadOneFrame();
        if (r < 0) return;    /* fatal – reconnect already scheduled */
        if (r == 0) return;   /* no data right now */
    }
}

/* ----------------------------------------------------------------
   Connect SignalR: negotiate + ws upgrade + subscribe
   ---------------------------------------------------------------- */
static void connectSignalR()
{
    if (WiFi.status() != WL_CONNECTED) return;

    static uint32_t s_attempt = 0;
    Serial.printf("[F1Net] connectSignalR attempt #%u (backoff=%ums) heap=%u\n",
                  ++s_attempt, s_reconnDelay, (unsigned)esp_get_free_heap_size());

    /* Static: avoids stack. Safe – only called from single f1net task.
       SignalR Core token is short alphanumeric, no URL-encoding needed. */
    static char token[128];
    token[0] = '\0';
    if (!negotiate(token, sizeof(token))) {
        scheduleReconnect("neg-fail");
        return;
    }

    if (!wsConnect(token)) {
        scheduleReconnect("ws-fail");
        return;
    }

    s_wsOpen    = true;
    s_connected = true;
    s_lastPing  = millis();
    s_lastData  = millis();
    s_reconnDelay = RECONNECT_INIT_MS;  /* reset back-off on success */

    /* Pump frames until the server acknowledges the protocol handshake
       ({}\x1e) or rejects it.  Subscribe must not go out before the ack:
       the server otherwise treats the stream as not-yet-handshaken. */
    {
        unsigned long t0 = millis();
        while (!s_handshakeAcked && !s_handshakeErr
                && millis() - t0 < 4000) {
            wsRead();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    if (s_handshakeErr || !s_handshakeAcked) {
        setErr("WS handshake not confirmed");
        scheduleReconnect("hs-ack");
        return;
    }
    Serial.println("[F1Net] Handshake OK");

    /* SignalR subscribe */
    if (!ws_send_text(*s_tls, SUBSCRIBE_MSG)) {
        setErr("Subscribe write failed");
        scheduleReconnect("sub-write");
        return;
    }
    Serial.println("[F1Net] Subscribed");
}

/* ================================================================
   Public C API
   ================================================================ */

void f1net_setCallback(F1NetStateCB cb)      { s_callback      = cb; }
void f1net_setEventCallback(F1EventCB cb)    { s_eventCallback = cb; }
void f1net_setSnapshotCallback(F1SnapshotCB cb) { s_snapshotCb = cb; }

void f1net_setup(void)
{
    Serial.println("[F1Net] setup (TLS)");
    /* Request the season session list NOW, before the WSS connects:
       the CDN fetch needs ~40 kB of TLS buffers and the live connection
       would otherwise have taken them (observed "SSL setup: Memory
       allocation failed" whenever it ran after the WSS was up). */
    if (!f1sessions_hasData()) f1sessions_requestFetch();
    s_lastConnect = millis() - RECONNECT_INIT_MS;  /* connect immediately */
}

void f1net_loop(void)
{
    if (WiFi.status() != WL_CONNECTED) {
        s_connectPhase = "wifi";
        return;
    }

    /* ── Fetch requests (sessions / replay / calendar) ──────────────────
       These open their OWN heap-allocated TlsConn in F1Sessions.cpp, so
       they never touch this file's s_tls WSS handle. Instead of returning
       immediately (blocking this task entirely and stalling wsRead() in
       the live connection), we perform the fetch here, then refresh s_lastData
       to ensure the live connection doesn't time out while we were busy. */
    if (f1sessions_fetchRequested()) {
        s_connectPhase = "sess";
        Serial.println("[F1Net] Sessions fetch");
        f1sessions_fetch();
        if (s_wsOpen) s_lastData = millis();
        /* Do NOT return here */
    }

    /* ── Session replay fetch request ────────────────────────────────── */
    if (f1sessions_replayRequested()) {
        Serial.println("[F1Net] Replay fetch");
        f1sessions_fetchAndReplay();
        if (s_wsOpen) s_lastData = millis();
        /* Do NOT return here */
    }

    /* ── Calendar API fetch request ──────────────────────────────────── */
    if (f1cal_apiFetchRequested()) {
        s_connectPhase = "cal";
        Serial.println("[F1Net] Calendar API fetch");
        f1cal_fetchApi();
        if (s_wsOpen) s_lastData = millis();
        /* Do NOT return here */
    }

    if (!s_wsOpen) {
        unsigned long now = millis();
        if (now - s_lastConnect >= s_reconnDelay) {
            s_connectPhase = "connecting";
            s_lastConnect = now;
            connectSignalR();
        } else {
            s_connectPhase = "wait";
        }
        return;
    }
    s_connectPhase = "live";

    /* Check connection still alive */
    if (!s_tls) {
        Serial.println("[F1Net] TLS handle lost");
        scheduleReconnect("tls-lost");
        return;
    }

    /* Read incoming frames */
    wsRead();

    /* Idle timeout */
    if (millis() - s_lastData > READ_TIMEOUT_MS) {
        Serial.println("[F1Net] WS idle timeout");
        scheduleReconnect("idle-timeout");
        return;
    }

    /* Send SignalR Core keepalive ping (type:6) */
    if (millis() - s_lastPing > PING_INTERVAL_MS) {
        ws_send_text(*s_tls, "{\"type\":6}\x1e");
        s_lastPing = millis();
    }
}

bool        f1net_isConnected(void)   { return s_wsOpen && s_tls != nullptr; }
const char* f1net_connectPhase(void)  { return s_connectPhase; }
const char* f1net_lastError(void)     { return s_lastErr[0] ? s_lastErr : ""; }
F1NetState f1net_getState(void)    { return s_state; }

void f1net_forceReconnect(void)
{
    /* Reset back-off and fire reconnect on the next f1net_loop() tick */
    s_reconnDelay = RECONNECT_INIT_MS;
    s_lastConnect = millis() - RECONNECT_INIT_MS;
    Serial.println("[F1Net] Force reconnect requested");
}

void f1net_disconnect(void)
{
    s_wsOpen    = false;
    s_connected = false;
    if (s_tls) {
        tls_cleanup(*s_tls);
        delete s_tls;
        s_tls = nullptr;
    }
}

/* ── Live event log public API ──────────────────────────────────────── */

int f1net_eventCount(void) {
    /* Auto-clear check */
    if (s_sessEndEpoch > 0 && s_evCount > 0) {
        long now = (long)f1_clock();
        if (now > (long)s_sessEndEpoch + 3600L) {
            s_evCount = 0;
            s_evHead  = 0;
            s_sessEndEpoch = 0;
        }
    }
    return s_evCount;
}

bool f1net_getEvent(int idx, F1LiveEvent* out) {
    if (idx < 0 || idx >= s_evCount || !out) return false;
    int start;
    if (s_evCount < F1_EVENT_LOG_MAX)
        start = 0;
    else
        start = s_evHead;  /* oldest is at head in a full ring */
    int pos = (start + idx) % F1_EVENT_LOG_MAX;
    *out = s_eventLog[pos];
    return true;
}

bool     f1net_sessionActive(void)    { return s_sessionActive; }
uint32_t f1net_sessionEndEpoch(void)  { return s_sessEndEpoch; }

void f1net_clearEvents(void) {
    s_evCount = 0;
    s_evHead  = 0;
}
