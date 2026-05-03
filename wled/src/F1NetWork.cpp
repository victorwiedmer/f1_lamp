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

/* ----------------------------------------------------------------
   Tunables
   ---------------------------------------------------------------- */
static constexpr uint32_t RECONNECT_INIT_MS  =   5000;
static constexpr uint32_t RECONNECT_MAX_MS   = 120000;
/* Server KeepAliveTimeout = 20 s (from /signalr/negotiate).
   Send our ping every 15 s so we stay within that window.
   Only declare idle after 45 s – well beyond one server heartbeat cycle. */
static constexpr uint32_t PING_INTERVAL_MS   =  15000;   /* SignalR {} ping  */
static constexpr uint32_t TCP_TIMEOUT_MS     =  10000;   /* connect timeout  */
static constexpr uint32_t READ_TIMEOUT_MS    =  45000;   /* idle WS timeout  */

/* ----------------------------------------------------------------
   SignalR endpoints (HTTPS, port 443)
   ---------------------------------------------------------------- */
static constexpr const char* SR_HOST      = "livetiming.formula1.com";
static constexpr int         SR_PORT      = 443;
static constexpr const char* SR_NEGOTIATE = "/signalr/negotiate";
static constexpr const char* SR_CONNECT   = "/signalr/connect";
static constexpr const char* HUB_DATA_ENC =
    "%5B%7B%22name%22%3A%22Streaming%22%7D%5D";
static constexpr const char* SUBSCRIBE_MSG =
    "{\"H\":\"Streaming\",\"M\":\"Subscribe\","
    "\"A\":[[\"TrackStatus\",\"SessionStatus\",\"Heartbeat\",\"RaceControlMessages\"]],"
    "\"I\":1}";

/* ----------------------------------------------------------------
   Module state
   ---------------------------------------------------------------- */
static volatile F1NetState  s_state       = F1ST_IDLE;
static volatile bool        s_connected   = false;
static F1NetStateCB         s_callback    = nullptr;
static F1EventCB            s_eventCallback = nullptr;

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

/* ── Live event ring buffer ──────────────────────────────────────────── */
static F1LiveEvent s_eventLog[F1_EVENT_LOG_MAX];
static int         s_evHead    = 0;   /* next write position              */
static int         s_evCount   = 0;   /* total events stored              */
static uint32_t    s_sessEndEpoch = 0; /* epoch when session ended (0=active) */

static void logEvent(const char* category, const char* message) {
    /* Auto-clear if >1h past session end */
    if (s_sessEndEpoch > 0) {
        time_t now = time(nullptr);
        if (now > (time_t)s_sessEndEpoch + 3600) {
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
static int  s_wsBufLen = 0;

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

/* Map TrackStatus code → F1NetState */
static F1NetState trackCodeToState(const char* code)
{
    if (!code || !code[0]) return F1ST_UNKNOWN;
    switch (code[0]) {
        case '1': return F1ST_GREEN;
        case '2': return F1ST_YELLOW;
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
static void scheduleReconnect()
{
    s_wsOpen     = false;
    s_connected  = false;
    if (s_tls) {
        tls_cleanup(*s_tls);
        delete s_tls;
        s_tls = nullptr;
    }
    s_lastConnect = millis();
    Serial.printf("[F1Net] Reconnect in %ums\n", s_reconnDelay);
    if (s_reconnDelay < RECONNECT_MAX_MS)
        s_reconnDelay = (s_reconnDelay < RECONNECT_MAX_MS / 2)
                        ? s_reconnDelay * 2 : RECONNECT_MAX_MS;
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
        Serial.printf("[F1Net] DNS failed: %d\n", ret);
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
        } else {
            lwip_close(c.sock); c.sock = -1;
        }
    }
    lwip_freeaddrinfo(res);
    if (!tcpOk) {
        Serial.println("[F1Net] TCP connect failed");
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
    Serial.println("[F1Net] TLS handshake OK");
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

/* Check if socket has data available without blocking */
static bool tls_data_available(TlsConn& c)
{
    if (c.sock < 0) return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(c.sock, &fds);
    struct timeval tv = {0, 0};
    return select(c.sock + 1, &fds, nullptr, nullptr, &tv) > 0;
}

/* Read exactly 'len' bytes with timeout */
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

/* Send a text frame from client (mask bit set, 4-byte mask) */
static void ws_send_text(TlsConn& c, const char* payload)
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

    tls_write_all(c, hdr, hdrLen);

    /* Write masked payload */
    char masked[256];
    const char* p = payload;
    size_t remaining = len;
    size_t mi = 0;
    while (remaining > 0) {
        size_t chunk = (remaining > sizeof(masked)) ? sizeof(masked) : remaining;
        for (size_t i = 0; i < chunk; ++i) {
            masked[i] = p[i] ^ mask[mi++ & 3];
        }
        tls_write_all(c, masked, chunk);
        p += chunk;
        remaining -= chunk;
    }
}

/* Send WS ping frame */
static void ws_send_ping(TlsConn& c)
{
    uint8_t frame[6] = {0x89, 0x80, 0x00, 0x00, 0x00, 0x00};  /* FIN+ping, masked, len=0 */
    tls_write_all(c, frame, sizeof(frame));
}

/* ----------------------------------------------------------------
   HTTPS negotiate  (captures ALB cookies for WebSocket upgrade)
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

    static char req[512];
    snprintf(req, sizeof(req),
        "GET %s?clientProtocol=1.5&connectionData=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BestHTTP\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        SR_NEGOTIATE, HUB_DATA_ENC, SR_HOST);

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
        Serial.printf("[F1Net] Neg HTTP %d\n", statusCode);
        return false;
    }

    /* Capture ALB cookies from response headers */
    extractCookies(resp, s_cookies, sizeof(s_cookies));

    /* Find body (after \r\n\r\n) */
    const char* body = strstr(resp, "\r\n\r\n");
    if (!body) { Serial.println("[F1Net] No body"); return false; }
    body += 4;

    if (!json_str(body, "ConnectionToken", outToken, tokenLen)) {
        Serial.println("[F1Net] No ConnectionToken");
        return false;
    }
    Serial.printf("[F1Net] Token len=%d\n", (int)strlen(outToken));
    return true;
}

/* ----------------------------------------------------------------
   WebSocket connect + upgrade  (WSS, forwarding ALB cookies)
   ---------------------------------------------------------------- */
static bool wsConnect(const char* tokenEnc)
{
    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) { Serial.println("[F1Net] OOM wsConnect"); return false; }

    if (!tls_connect(*cp)) {
        Serial.println("[F1Net] WS TLS connect fail");
        delete cp;
        return false;
    }

    /* Build upgrade path */
    static char path[1024];
    snprintf(path, sizeof(path),
        "%s?transport=webSockets&clientProtocol=1.5"
        "&connectionToken=%s&connectionData=%s",
        SR_CONNECT, tokenEnc, HUB_DATA_ENC);

    /* RFC 6455 upgrade request – include ALB cookies from negotiate */
    static char req[1536];
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
        Serial.printf("[F1Net] WS upgrade failed: %.100s\n", resp);
        tls_cleanup(*cp); delete cp;
        return false;
    }

    /* Persist connection for ongoing WS use */
    s_tls = cp;
    Serial.println("[F1Net] WSS connected");
    return true;
}

/* ----------------------------------------------------------------
   Process one complete SignalR message
   ---------------------------------------------------------------- */
static void processMessage(const char* msg, int len)
{
    /* Ignore keepalive {} */
    if (len <= 2) return;

    /* Look for "M" array (push messages) */
    const char* mp = strstr(msg, "\"M\":");
    if (!mp) return;
    mp += 4;
    while (*mp == ' ') ++mp;
    if (*mp != '[') return;

    /* Iterate messages in array */
    const char* cursor = mp + 1;
    while (*cursor && *cursor != ']') {
        if (*cursor != '{') { ++cursor; continue; }

        /* Get "H" hub name */
        char hub[64] = {};
        json_str(cursor, "H", hub, sizeof(hub));

        /* Get "M" method name */
        char method[64] = {};
        json_str(cursor, "M", method, sizeof(method));

        if (strcasecmp(hub, "Streaming") == 0 &&
            strcasecmp(method, "feed") == 0)
        {
            /* Extract "A" args array content */
            const char* ap = strstr(cursor, "\"A\":");
            if (ap) {
                ap += 4;
                while (*ap == ' ') ++ap;
                if (*ap == '[') {
                    ++ap;
                    /* First arg is topic name */
                    if (*ap == '"') {
                        char topic[64] = {};
                        size_t ti = 0;
                        ++ap;
                        while (*ap && *ap != '"' && ti < sizeof(topic)-1)
                            topic[ti++] = *ap++;
                        topic[ti] = '\0';

                        if (strcasecmp(topic, "TrackStatus") == 0) {
                            /* Skip to second arg (status object) */
                            if (*ap == '"') { ++ap; } /* closing quote */
                            while (*ap == ',' || *ap == ' ') ++ap;
                            char code[8] = {};
                            json_str(ap, "Status", code, sizeof(code));
                            if (code[0]) {
                                F1NetState ns = trackCodeToState(code);
                                Serial.printf("[F1Net] TrackStatus=%s\n", code);
                                /* Log the track status change */
                                static const char* TRK_NAMES[] = {
                                    "Idle","Green","Yellow","?","Safety Car",
                                    "Red Flag","VSC","VSC Ending"};
                                int ci = code[0] - '0';
                                const char* tn = (ci>=0 && ci<=7) ? TRK_NAMES[ci] : code;
                                char msg[F1_EVENT_MSG_LEN];
                                snprintf(msg, sizeof(msg), "Track: %s", tn);
                                logEvent("Track", msg);
                                applyState(ns);
                            }
                        }
                        else if (strcasecmp(topic, "SessionStatus") == 0) {
                            if (*ap == '"') { ++ap; }
                            while (*ap == ',' || *ap == ' ') ++ap;
                            char status[32] = {};
                            json_str(ap, "Status", status, sizeof(status));
                            Serial.printf("[F1Net] SessionStatus=%s\n", status);
                            /* Log session status */
                            char smsg[F1_EVENT_MSG_LEN];
                            snprintf(smsg, sizeof(smsg), "Session: %s", status);
                            logEvent("Session", smsg);
                            if (strstr(status, "Started")) {
                                s_sessionActive = true;
                                s_sessEndEpoch = 0;
                                applyState(F1ST_SESSION_START);
                            } else if (strstr(status, "Finished") ||
                                       strstr(status, "Ends")) {
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
                            if (*ap == '"') { ++ap; }
                            while (*ap == ',' || *ap == ' ') ++ap;
                            /* Extract the Message text for the event log */
                            char rcMsg[F1_EVENT_MSG_LEN] = {};
                            json_str(ap, "Message", rcMsg, sizeof(rcMsg));
                            if (rcMsg[0]) {
                                logEvent("RaceCtrl", rcMsg);
                                Serial.printf("[F1Net] RaceCtrl: %s\n", rcMsg);
                            }
                            /* Simple substring search – messages are small and well-known */
                            if (strstr(ap, "FASTEST LAP") || strstr(ap, "Fastest Lap")) {
                                Serial.println("[F1Net] ↯ Fastest lap detected");
                                if (s_eventCallback) s_eventCallback(F1EVT_FASTEST_LAP);
                            }
                            /* DRS: look for both strings to avoid false positives */
                            if ((strstr(ap, "DRS") || strstr(ap, "Drs"))
                                    && (strstr(ap, "ENABLED") || strstr(ap, "Enabled"))) {
                                Serial.println("[F1Net] ↯ DRS enabled");
                                if (s_eventCallback) s_eventCallback(F1EVT_DRS_ENABLED);
                            }
                        }
                    }
                }
            }
        }

        /* Advance to next object */
        while (*cursor && *cursor != '}') ++cursor;
        if (*cursor == '}') ++cursor;
        while (*cursor == ',' || *cursor == ' ') ++cursor;
    }
}

/* ----------------------------------------------------------------
   Read + process pending WS frames (non-blocking, called from loop)
   ---------------------------------------------------------------- */
static void wsRead()
{
    if (!s_tls) return;
    if (!tls_data_available(*s_tls)) return;

    /* Process frames while data keeps arriving */
    while (true) {
        /* Read 2-byte WebSocket frame header */
        uint8_t hdr2[2];
        if (tls_read_exact(*s_tls, hdr2, 2, 2000) != 2) {
            scheduleReconnect(); return;
        }

        bool fin    = (hdr2[0] & 0x80) != 0;
        uint8_t op  =  hdr2[0] & 0x0F;
        bool masked = (hdr2[1] & 0x80) != 0;
        uint64_t payLen = hdr2[1] & 0x7F;

        /* Extended length */
        if (payLen == 126) {
            uint8_t ext[2];
            if (tls_read_exact(*s_tls, ext, 2, 500) != 2) {
                scheduleReconnect(); return;
            }
            payLen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (payLen == 127) {
            uint8_t ext[8];
            if (tls_read_exact(*s_tls, ext, 8, 500) != 8) {
                scheduleReconnect(); return;
            }
            payLen = 0;
            for (int i = 0; i < 8; ++i) payLen = (payLen << 8) | ext[i];
        }

        /* Server frames shouldn't be masked, but handle anyway */
        uint8_t mask[4] = {};
        if (masked) {
            if (tls_read_exact(*s_tls, mask, 4, 500) != 4) {
                scheduleReconnect(); return;
            }
        }

        /* Read payload */
        if (payLen > sizeof(s_wsBuf) - 1) {
            /* Too large — skip by reading and discarding */
            uint64_t skip = payLen;
            uint8_t discard[256];
            while (skip > 0) {
                size_t chunk = (skip > sizeof(discard))
                               ? sizeof(discard) : (size_t)skip;
                int r = tls_read_exact(*s_tls, discard, chunk, 5000);
                if (r <= 0) { scheduleReconnect(); return; }
                skip -= (size_t)r;
            }
        } else if (payLen > 0) {
            int r = tls_read_exact(*s_tls, (uint8_t*)s_wsBuf,
                                    (size_t)payLen, 5000);
            if (r != (int)payLen) { scheduleReconnect(); return; }
            if (masked) {
                for (uint64_t i = 0; i < payLen; ++i)
                    s_wsBuf[i] ^= mask[i & 3];
            }
            s_wsBuf[payLen] = '\0';
        }

        s_lastData = millis();

        /* Handle opcodes */
        if (op == 0x8) {
            /* Close */
            Serial.println("[F1Net] WS close recv");
            scheduleReconnect();
            return;
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

        /* Check if more frames are waiting */
        if (!tls_data_available(*s_tls)) break;
    }
}

/* ----------------------------------------------------------------
   Connect SignalR: negotiate + ws upgrade + subscribe
   ---------------------------------------------------------------- */
static void connectSignalR()
{
    if (WiFi.status() != WL_CONNECTED) return;

    /* Static: avoids 3 KB of stack. Safe – only called from single f1net task. */
    static char token[512];
    static char tokenEnc[1280];
    token[0] = '\0';
    if (!negotiate(token, sizeof(token))) {
        scheduleReconnect();
        return;
    }

    url_encode(token, tokenEnc, sizeof(tokenEnc));

    if (!wsConnect(tokenEnc)) {
        scheduleReconnect();
        return;
    }

    s_wsOpen    = true;
    s_connected = true;
    s_lastPing  = millis();
    s_lastData  = millis();
    s_reconnDelay = RECONNECT_INIT_MS;  /* reset back-off on success */

    /* SignalR subscribe */
    ws_send_text(*s_tls, SUBSCRIBE_MSG);
    Serial.println("[F1Net] Subscribed");
}

/* ================================================================
   Public C API
   ================================================================ */

void f1net_setCallback(F1NetStateCB cb)      { s_callback      = cb; }
void f1net_setEventCallback(F1EventCB cb)    { s_eventCallback = cb; }

void f1net_setup(void)
{
    Serial.println("[F1Net] setup (TLS)");
    s_lastConnect = millis() - RECONNECT_INIT_MS;  /* connect immediately */
}

void f1net_loop(void)
{
    if (WiFi.status() != WL_CONNECTED) return;

    /* ── Sessions fetch request ──────────────────────────────────────────
       The WebUI sets a flag via f1sessions_requestFetch().  We handle it
       here so there is only ONE TLS connection at a time – opening a
       second one would exhaust the lwIP socket pool on ESP32-C3 and
       block the AsyncWebServer from sending any responses.              */
    if (f1sessions_fetchRequested()) {
        Serial.println("[F1Net] Sessions fetch requested – tearing down WS");
        if (s_tls) {
            tls_cleanup(*s_tls);
            delete s_tls;
            s_tls = nullptr;
        }
        s_wsOpen    = false;
        s_connected = false;
        /* Blocking fetch – reuses this task's stack, one TLS conn at a time */
        f1sessions_fetch();
        /* Schedule reconnect after a short delay */
        s_lastConnect = millis();
        s_reconnDelay = RECONNECT_INIT_MS;
        return;
    }

    /* ── Session replay fetch request ────────────────────────────────── */
    if (f1sessions_replayRequested()) {
        Serial.println("[F1Net] Replay fetch requested – tearing down WS");
        if (s_tls) {
            tls_cleanup(*s_tls);
            delete s_tls;
            s_tls = nullptr;
        }
        s_wsOpen    = false;
        s_connected = false;
        f1sessions_fetchAndReplay();
        s_lastConnect = millis();
        s_reconnDelay = RECONNECT_INIT_MS;
        return;
    }

    /* ── Calendar API fetch request ──────────────────────────────────── */
    if (f1cal_apiFetchRequested()) {
        Serial.println("[F1Net] Calendar API fetch requested – tearing down WS");
        if (s_tls) {
            tls_cleanup(*s_tls);
            delete s_tls;
            s_tls = nullptr;
        }
        s_wsOpen    = false;
        s_connected = false;
        f1cal_fetchApi();
        s_lastConnect = millis();
        s_reconnDelay = RECONNECT_INIT_MS;
        return;
    }

    if (!s_wsOpen) {
        unsigned long now = millis();
        if (now - s_lastConnect >= s_reconnDelay) {
            s_lastConnect = now;
            connectSignalR();
        }
        return;
    }

    /* Check connection still alive */
    if (!s_tls) {
        Serial.println("[F1Net] TLS handle lost");
        scheduleReconnect();
        return;
    }

    /* Read incoming frames */
    wsRead();

    /* Idle timeout */
    if (millis() - s_lastData > READ_TIMEOUT_MS) {
        Serial.println("[F1Net] WS idle timeout");
        scheduleReconnect();
        return;
    }

    /* Send SignalR keepalive {} ping */
    if (millis() - s_lastPing > PING_INTERVAL_MS) {
        ws_send_text(*s_tls, "{}");
        s_lastPing = millis();
    }
}

bool       f1net_isConnected(void) { return s_wsOpen && s_tls != nullptr; }
F1NetState f1net_getState(void)    { return s_state; }

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
        time_t now = time(nullptr);
        if (now > (time_t)s_sessEndEpoch + 3600) {
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
