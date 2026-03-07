/*
 * F1NetWork.cpp  –  SignalR client for F1 live-timing.
 *
 * *** CRITICAL: DO NOT include wled.h or any header that includes     ***
 * *** ESPAsyncWebServer.h in this translation unit.                   ***
 * *** ESPAsyncWebServer defines HTTP_GET/DELETE/etc. as C enum values ***
 * *** that conflict with the same identifiers in esp-idf/http_parser. ***
 *
 * Implementation uses the ESP-IDF esp_tls API for HTTPS/WSS connections
 * to the F1 live-timing SignalR service.
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

#include <WiFi.h>           /* Arduino WiFi – brings in WiFiClient     */
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <Arduino.h>        /* millis(), Serial                        */

/* ESP-IDF TLS */
extern "C" {
#include "esp_tls.h"
#include "esp_crt_bundle.h"
}

#include "F1NetWork.h"
#include "F1Sessions.h"

/* For select() / fd_set on ESP-IDF */
#include <sys/select.h>

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

static esp_tls_t*  s_tls         = nullptr;   /* persistent WSS connection    */
static bool        s_wsOpen       = false;
static uint32_t    s_reconnDelay  = RECONNECT_INIT_MS;
static unsigned long s_lastConnect = 0;
static unsigned long s_lastPing    = 0;
static unsigned long s_lastData    = 0;  /* last ws frame received */
static bool        s_sessionActive = false;
static int         s_connFails    = 2;   /* start disabled – esp_tls has TLS compiled out, crashes on connect */
static constexpr int MAX_CONN_FAILS = 2; /* stop retrying after this many    */

/* AWS ALB cookies captured during negotiate, forwarded to WS upgrade */
static char s_cookies[512] = {};

/* WS frame reassembly */
static char s_wsBuf[8192];
static int  s_wsBufLen = 0;

/* ----------------------------------------------------------------
   Helpers
   ---------------------------------------------------------------- */

/* URL-encode src into dst.  Returns dst. */
static char* url_encode(const char* src, char* dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; src[i] && j + 4 < dst_size; ++i) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c) || c=='_' || c=='-' || c=='.' || c=='~') {
            dst[j++] = (char)c;
        } else {
            dst[j++] = '%';
            dst[j++] = hex[c >> 4];
            dst[j++] = hex[c & 0xF];
        }
    }
    dst[j] = '\0';
    return dst;
}

/* Extract JSON string value: {"key":"<value>",...} */
static bool json_str(const char* json, const char* key,
                     char* out, size_t out_size)
{
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return false;
    p += strlen(search);
    while (*p == ' ' || *p == ':') ++p;
    if (*p != '"') return false;
    ++p;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < out_size) out[n++] = *p++;
    out[n] = '\0';
    return n > 0;
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

/* Schedule reconnect with back-off */
static void scheduleReconnect()
{
    s_wsOpen     = false;
    s_connected  = false;
    if (s_tls) {
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
    }
    s_lastConnect = millis();
    Serial.printf("[F1Net] Reconnect in %ums\n", s_reconnDelay);
    if (s_reconnDelay < RECONNECT_MAX_MS)
        s_reconnDelay = (s_reconnDelay < RECONNECT_MAX_MS / 2)
                        ? s_reconnDelay * 2 : RECONNECT_MAX_MS;
}

/* ----------------------------------------------------------------
   TLS I/O helpers
   ---------------------------------------------------------------- */

/* Write all bytes over TLS (handles partial writes) */
static bool tls_write_all(esp_tls_t* tls, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    size_t sent = 0;
    unsigned long t0 = millis();
    while (sent < len && millis() - t0 < TCP_TIMEOUT_MS) {
        ssize_t r = esp_tls_conn_write(tls, p + sent, len - sent);
        if (r > 0) {
            sent += (size_t)r;
        } else if (r == 0) {
            delay(1);
        } else {
            Serial.printf("[F1Net] TLS write error: %d\n", (int)r);
            return false;
        }
    }
    return sent == len;
}

/* Check if socket has data available (non-blocking) */
static bool tls_data_available(esp_tls_t* tls)
{
    if (!tls) return false;
    int sockfd = -1;
    if (esp_tls_get_conn_sockfd(tls, &sockfd) != ESP_OK || sockfd < 0)
        return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sockfd, &fds);
    struct timeval tv = {0, 0};
    return select(sockfd + 1, &fds, nullptr, nullptr, &tv) > 0;
}

/* Read exactly 'len' bytes with timeout */
static int tls_read_exact(esp_tls_t* tls, uint8_t* buf, size_t len,
                          uint32_t timeout_ms)
{
    size_t got = 0;
    unsigned long t0 = millis();
    while (got < len && millis() - t0 < timeout_ms) {
        ssize_t r = esp_tls_conn_read(tls, buf + got, len - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return -1;  /* connection closed */
        } else {
            /* WANT_READ / WANT_WRITE → retry after tiny delay */
            delay(1);
        }
    }
    return (int)got;
}

/* Open a fresh TLS connection to SR_HOST:SR_PORT */
static esp_tls_t* tls_connect()
{
    esp_tls_cfg_t cfg = {};
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = TCP_TIMEOUT_MS;
    cfg.non_block  = false;

    esp_tls_t* tls = esp_tls_init();
    if (!tls) {
        Serial.println("[F1Net] esp_tls_init failed");
        return nullptr;
    }
    int ret = esp_tls_conn_new_sync(SR_HOST, strlen(SR_HOST),
                                     SR_PORT, &cfg, tls);
    if (ret != 1) {
        Serial.printf("[F1Net] TLS connect failed: %d\n", ret);
        esp_tls_conn_destroy(tls);
        return nullptr;
    }
    return tls;
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
static void ws_send_text(esp_tls_t* tls, const char* payload)
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

    tls_write_all(tls, hdr, hdrLen);

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
        tls_write_all(tls, masked, chunk);
        p += chunk;
        remaining -= chunk;
    }
}

/* Send WS ping frame */
static void ws_send_ping(esp_tls_t* tls)
{
    uint8_t frame[6] = {0x89, 0x80, 0x00, 0x00, 0x00, 0x00};  /* FIN+ping, masked, len=0 */
    tls_write_all(tls, frame, sizeof(frame));
}

/* ----------------------------------------------------------------
   HTTPS negotiate  (captures ALB cookies for WebSocket upgrade)
   ---------------------------------------------------------------- */
static bool negotiate(char* outToken, size_t tokenLen)
{
    esp_tls_t* tls = tls_connect();
    if (!tls) {
        Serial.println("[F1Net] Neg TLS connect fail");
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

    if (!tls_write_all(tls, req, strlen(req))) {
        Serial.println("[F1Net] Neg write fail");
        esp_tls_conn_destroy(tls);
        return false;
    }

    /* Read entire response (Connection: close → server closes when done) */
    static char resp[4096];
    int respLen = 0;
    unsigned long t0 = millis();
    while (respLen < (int)sizeof(resp) - 1 && millis() - t0 < TCP_TIMEOUT_MS) {
        ssize_t r = esp_tls_conn_read(tls, resp + respLen,
                                       sizeof(resp) - 1 - respLen);
        if (r > 0) {
            respLen += (int)r;
        } else if (r == 0) {
            break;  /* server closed connection */
        } else {
            delay(1);  /* WANT_READ → retry */
        }
    }
    resp[respLen] = '\0';
    esp_tls_conn_destroy(tls);

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
    s_tls = tls_connect();
    if (!s_tls) {
        Serial.println("[F1Net] WS TLS connect fail");
        return false;
    }

    /* Build upgrade path */
    static char path[2048];
    snprintf(path, sizeof(path),
        "%s?transport=webSockets&clientProtocol=1.5"
        "&connectionToken=%s&connectionData=%s",
        SR_CONNECT, tokenEnc, HUB_DATA_ENC);

    /* RFC 6455 upgrade request – include ALB cookies from negotiate */
    static char req[3072];
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

    if (!tls_write_all(s_tls, req, strlen(req))) {
        Serial.println("[F1Net] WS upgrade write fail");
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
        return false;
    }

    /* Wait for 101 Switching Protocols */
    static char resp[1024];
    int rlen = 0;
    unsigned long t0 = millis();
    while (rlen < (int)sizeof(resp) - 1 && millis() - t0 < TCP_TIMEOUT_MS) {
        ssize_t r = esp_tls_conn_read(s_tls, resp + rlen,
                                       sizeof(resp) - 1 - rlen);
        if (r > 0) {
            rlen += (int)r;
            resp[rlen] = '\0';
            if (strstr(resp, "\r\n\r\n")) break;
        } else if (r == 0) {
            break;
        } else {
            delay(1);
        }
    }
    resp[rlen] = '\0';

    if (!strstr(resp, "101")) {
        Serial.printf("[F1Net] WS upgrade failed: %.100s\n", resp);
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
        return false;
    }
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
                                applyState(ns);
                            }
                        }
                        else if (strcasecmp(topic, "SessionStatus") == 0) {
                            if (*ap == '"') { ++ap; }
                            while (*ap == ',' || *ap == ' ') ++ap;
                            char status[32] = {};
                            json_str(ap, "Status", status, sizeof(status));
                            Serial.printf("[F1Net] SessionStatus=%s\n", status);
                            if (strstr(status, "Started")) {
                                s_sessionActive = true;
                                applyState(F1ST_SESSION_START);
                            } else if (strstr(status, "Finished") ||
                                       strstr(status, "Ends")) {
                                applyState(F1ST_CHEQUERED);
                            } else if (strstr(status, "Inactive")) {
                                s_sessionActive = false;
                                applyState(F1ST_IDLE);
                            }
                        }
                        else if (strcasecmp(topic, "RaceControlMessages") == 0) {
                            if (*ap == '"') { ++ap; }
                            while (*ap == ',' || *ap == ' ') ++ap;
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
    if (!tls_data_available(s_tls)) return;

    /* Process frames while data keeps arriving */
    while (true) {
        /* Read 2-byte WebSocket frame header */
        uint8_t hdr2[2];
        if (tls_read_exact(s_tls, hdr2, 2, 2000) != 2) {
            scheduleReconnect(); return;
        }

        bool fin    = (hdr2[0] & 0x80) != 0;
        uint8_t op  =  hdr2[0] & 0x0F;
        bool masked = (hdr2[1] & 0x80) != 0;
        uint64_t payLen = hdr2[1] & 0x7F;

        /* Extended length */
        if (payLen == 126) {
            uint8_t ext[2];
            if (tls_read_exact(s_tls, ext, 2, 500) != 2) {
                scheduleReconnect(); return;
            }
            payLen = ((uint64_t)ext[0] << 8) | ext[1];
        } else if (payLen == 127) {
            uint8_t ext[8];
            if (tls_read_exact(s_tls, ext, 8, 500) != 8) {
                scheduleReconnect(); return;
            }
            payLen = 0;
            for (int i = 0; i < 8; ++i) payLen = (payLen << 8) | ext[i];
        }

        /* Server frames shouldn't be masked, but handle anyway */
        uint8_t mask[4] = {};
        if (masked) {
            if (tls_read_exact(s_tls, mask, 4, 500) != 4) {
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
                int r = tls_read_exact(s_tls, discard, chunk, 5000);
                if (r <= 0) { scheduleReconnect(); return; }
                skip -= (size_t)r;
            }
        } else if (payLen > 0) {
            int r = tls_read_exact(s_tls, (uint8_t*)s_wsBuf,
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
            tls_write_all(s_tls, pong, sizeof(pong));
        }
        if (op == 0xA) {
            /* Pong – ignore */
        }
        if ((op == 0x1 || op == 0x0) && payLen > 0) {
            /* Text / continuation frame */
            processMessage(s_wsBuf, (int)payLen);
        }

        /* Check if more frames are waiting */
        if (!tls_data_available(s_tls)) break;
    }
}

/* ----------------------------------------------------------------
   Connect SignalR: negotiate + ws upgrade + subscribe
   ---------------------------------------------------------------- */
static void connectSignalR()
{
    if (WiFi.status() != WL_CONNECTED) return;

    /* esp_tls has TLS compiled out (CONFIG_MBEDTLS_TLS_DISABLED).  Each failed
       attempt blocks TCP for ~10 s, starving the web server.  After a couple of
       failures give up so the UI stays responsive.  Will be removed when
       F1NetWork moves to raw mbedtls.  */
    if (s_connFails >= MAX_CONN_FAILS) return;

    /* Static: avoids 3 KB of stack. Safe – only called from single f1net task. */
    static char token[1024];
    static char tokenEnc[2048];
    token[0] = '\0';
    if (!negotiate(token, sizeof(token))) {
        s_connFails++;
        if (s_connFails >= MAX_CONN_FAILS)
            Serial.println("[F1Net] TLS appears broken – suspending retries");
        scheduleReconnect();
        return;
    }

    url_encode(token, tokenEnc, sizeof(tokenEnc));

    if (!wsConnect(tokenEnc)) {
        s_connFails++;
        if (s_connFails >= MAX_CONN_FAILS)
            Serial.println("[F1Net] TLS appears broken – suspending retries");
        scheduleReconnect();
        return;
    }

    s_connFails = 0;  /* reset on success */
    s_wsOpen    = true;
    s_connected = true;
    s_lastPing  = millis();
    s_lastData  = millis();
    s_reconnDelay = RECONNECT_INIT_MS;  /* reset back-off on success */

    /* SignalR subscribe */
    ws_send_text(s_tls, SUBSCRIBE_MSG);
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
            esp_tls_conn_destroy(s_tls);
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
        ws_send_text(s_tls, "{}");
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
        esp_tls_conn_destroy(s_tls);
        s_tls = nullptr;
    }
}
