/*
 * F1NetWork.cpp  –  SignalR client for F1 live-timing.
 *
 * *** CRITICAL: DO NOT include wled.h or any header that includes     ***
 * *** ESPAsyncWebServer.h in this translation unit.                   ***
 * *** ESPAsyncWebServer defines HTTP_GET/DELETE/etc. as C enum values ***
 * *** that conflict with the same identifiers in esp-idf/http_parser. ***
 *
 * Implementation uses only WiFiClient (Arduino TCP socket) so that we
 * avoid pulling in libesp_websocket_client.a / libesp-tls.a, which
 * reference mbedtls_ssl_* symbols stripped from the WLED framework.
 *
 * Protocol flow:
 *   HTTP GET /signalr/negotiate  →  ConnectionToken (JSON)
 *   HTTP/1.1 upgrade  →  ws://  WebSocket (RFC 6455, plain TCP)
 *   Send SignalR subscribe frame
 *   Receive TrackStatus / SessionStatus push frames
 *   Ping every 30 s (SignalR keepalive)
 *   Reconnect with exponential back-off on disconnect
 */

#include <WiFi.h>           /* Arduino WiFi – brings in WiFiClient     */
#include <WiFiClient.h>     /* plain TCP socket, no TLS                */
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <Arduino.h>        /* millis(), Serial                        */

#include "F1NetWork.h"

/* ----------------------------------------------------------------
   Tunables
   ---------------------------------------------------------------- */
static constexpr uint32_t RECONNECT_INIT_MS  =   5000;
static constexpr uint32_t RECONNECT_MAX_MS   = 120000;
static constexpr uint32_t PING_INTERVAL_MS   =  30000;   /* SignalR {} ping  */
static constexpr uint32_t TCP_TIMEOUT_MS     =  10000;   /* connect timeout  */
static constexpr uint32_t READ_TIMEOUT_MS    =  15000;   /* idle WS timeout  */

/* ----------------------------------------------------------------
   SignalR endpoints (plain HTTP, port 80)
   ---------------------------------------------------------------- */
static constexpr const char* SR_HOST      = "livetiming.formula1.com";
static constexpr int         SR_PORT      = 80;
static constexpr const char* SR_NEGOTIATE = "/signalr/negotiate";
static constexpr const char* SR_CONNECT   = "/signalr/connect";
static constexpr const char* HUB_DATA_ENC =
    "%5B%7B%22name%22%3A%22Streaming%22%7D%5D";
static constexpr const char* SUBSCRIBE_MSG =
    "{\"H\":\"Streaming\",\"M\":\"Subscribe\","
    "\"A\":[[\"TrackStatus\",\"SessionStatus\",\"Heartbeat\"]],"
    "\"I\":1}";

/* ----------------------------------------------------------------
   Module state
   ---------------------------------------------------------------- */
static volatile F1NetState  s_state       = F1ST_IDLE;
static volatile bool        s_connected   = false;
static F1NetStateCB         s_callback    = nullptr;

static WiFiClient  s_tcp;
static bool        s_wsOpen       = false;
static uint32_t    s_reconnDelay  = RECONNECT_INIT_MS;
static unsigned long s_lastConnect = 0;
static unsigned long s_lastPing    = 0;
static unsigned long s_lastData    = 0;  /* last ws frame received */
static bool        s_sessionActive = false;

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
        case '7': return F1ST_VIRTUAL_SC; /* VSC ending */
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
    s_tcp.stop();
    s_lastConnect = millis();
    Serial.printf("[F1Net] Reconnect in %ums\n", s_reconnDelay);
    if (s_reconnDelay < RECONNECT_MAX_MS)
        s_reconnDelay = (s_reconnDelay < RECONNECT_MAX_MS / 2)
                        ? s_reconnDelay * 2 : RECONNECT_MAX_MS;
}

/* ----------------------------------------------------------------
   WebSocket framing (RFC 6455, client-side only, no fragmentation)
   ---------------------------------------------------------------- */

/* Send a text frame from client (mask bit set, 4-byte mask) */
static void ws_send_text(WiFiClient& tcp, const char* payload)
{
    size_t len = strlen(payload);
    uint8_t hdr[10];
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

    tcp.write(hdr, hdrLen);

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
        tcp.write((uint8_t*)masked, chunk);
        p += chunk;
        remaining -= chunk;
    }
}

/* Send WS ping frame */
static void ws_send_ping(WiFiClient& tcp)
{
    uint8_t frame[6] = {0x89, 0x80, 0x00, 0x00, 0x00, 0x00};  /* FIN+ping, masked, len=0 */
    tcp.write(frame, sizeof(frame));
}

/* ----------------------------------------------------------------
   HTTP negotiate
   ---------------------------------------------------------------- */
static bool negotiate(char* outToken, size_t tokenLen)
{
    WiFiClient http;
    http.setTimeout(TCP_TIMEOUT_MS / 1000);
    if (!http.connect(SR_HOST, SR_PORT)) {
        Serial.println("[F1Net] Neg connect fail");
        return false;
    }

    char req[512];
    snprintf(req, sizeof(req),
        "GET %s?clientProtocol=1.5&connectionData=%s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: BestHTTP\r\n"
        "Accept-Encoding: identity\r\n"
        "Connection: close\r\n"
        "\r\n",
        SR_NEGOTIATE, HUB_DATA_ENC, SR_HOST);
    http.print(req);

    /* Read response headers + body */
    unsigned long t0 = millis();
    static char resp[3072];
    int respLen = 0;
    bool headersEnded = false;
    int statusCode = 0;

    while (millis() - t0 < TCP_TIMEOUT_MS) {
        while (http.available() && respLen < (int)sizeof(resp) - 1) {
            resp[respLen++] = (char)http.read();
        }
        if (!http.connected() && !http.available()) break;
        delay(1);
    }
    resp[respLen] = '\0';
    http.stop();

    /* Parse status line */
    if (sscanf(resp, "HTTP/1.1 %d", &statusCode) != 1 || statusCode != 200) {
        Serial.printf("[F1Net] Neg HTTP %d\n", statusCode);
        return false;
    }

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
   WebSocket connect + upgrade
   ---------------------------------------------------------------- */
static bool wsConnect(const char* tokenEnc)
{
    s_tcp.setTimeout(TCP_TIMEOUT_MS / 1000);
    if (!s_tcp.connect(SR_HOST, SR_PORT)) {
        Serial.println("[F1Net] WS connect fail");
        return false;
    }

    /* Build upgrade path */
    char path[2048];
    snprintf(path, sizeof(path),
        "%s?transport=webSockets&clientProtocol=1.5"
        "&connectionToken=%s&connectionData=%s",
        SR_CONNECT, tokenEnc, HUB_DATA_ENC);

    /* RFC 6455 upgrade request */
    char req[3072];
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
    s_tcp.print(req);

    /* Wait for 101 Switching Protocols */
    unsigned long t0 = millis();
    static char resp[1024];
    int rlen = 0;
    while (millis() - t0 < TCP_TIMEOUT_MS) {
        while (s_tcp.available() && rlen < (int)sizeof(resp) - 1) {
            resp[rlen++] = (char)s_tcp.read();
        }
        if (rlen >= 12 && strstr(resp, "\r\n\r\n")) break;
        delay(1);
    }
    resp[rlen] = '\0';

    if (!strstr(resp, "101")) {
        Serial.printf("[F1Net] WS upgrade failed: %.100s\n", resp);
        s_tcp.stop();
        return false;
    }
    Serial.println("[F1Net] WS connected");
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
    while (s_tcp.available()) {
        /* Read WebSocket frame header (at least 2 bytes) */
        if (s_tcp.available() < 2) break;

        uint8_t b0 = (uint8_t)s_tcp.read();
        uint8_t b1 = (uint8_t)s_tcp.read();

        bool fin    = (b0 & 0x80) != 0;
        uint8_t op  =  b0 & 0x0F;
        bool masked = (b1 & 0x80) != 0;
        uint64_t payLen = b1 & 0x7F;

        /* Extended length */
        if (payLen == 126) {
            unsigned long t0 = millis();
            while (s_tcp.available() < 2 && millis()-t0 < 500) delay(1);
            if (s_tcp.available() < 2) { scheduleReconnect(); return; }
            uint8_t hi = (uint8_t)s_tcp.read();
            uint8_t lo = (uint8_t)s_tcp.read();
            payLen = ((uint64_t)hi << 8) | lo;
        } else if (payLen == 127) {
            unsigned long t0 = millis();
            while (s_tcp.available() < 8 && millis()-t0 < 500) delay(1);
            payLen = 0;
            for (int i = 0; i < 8; ++i)
                payLen = (payLen << 8) | (uint8_t)s_tcp.read();
        }

        /* Server frames shouldn't be masked, but handle anyway */
        uint8_t mask[4] = {};
        if (masked) {
            unsigned long t0 = millis();
            while (s_tcp.available() < 4 && millis()-t0 < 500) delay(1);
            for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)s_tcp.read();
        }

        /* Read payload */
        if (payLen > sizeof(s_wsBuf) - 1) {
            /* Too large — skip */
            uint64_t skip = payLen;
            unsigned long t0 = millis();
            while (skip > 0 && millis()-t0 < 5000) {
                if (s_tcp.available()) { s_tcp.read(); --skip; }
                else delay(1);
            }
        } else {
            uint64_t toRead = payLen;
            unsigned long t0 = millis();
            while (toRead > 0 && millis()-t0 < 5000) {
                if (s_tcp.available()) {
                    uint8_t ch = (uint8_t)s_tcp.read();
                    if (masked) ch ^= mask[(payLen - toRead) & 3];
                    s_wsBuf[payLen - toRead] = (char)ch;
                    --toRead;
                } else delay(1);
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
            s_tcp.write(pong, sizeof(pong));
            continue;
        }
        if (op == 0xA) {
            /* Pong – ignore */
            continue;
        }
        if ((op == 0x1 || op == 0x0) && payLen > 0) {
            /* Text / continuation frame */
            processMessage(s_wsBuf, (int)payLen);
        }
    }
}

/* ----------------------------------------------------------------
   Connect SignalR: negotiate + ws upgrade + subscribe
   ---------------------------------------------------------------- */
static void connectSignalR()
{
    if (WiFi.status() != WL_CONNECTED) return;

    char token[1024] = {};
    if (!negotiate(token, sizeof(token))) {
        scheduleReconnect();
        return;
    }

    char tokenEnc[2048] = {};
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
    ws_send_text(s_tcp, SUBSCRIBE_MSG);
    Serial.println("[F1Net] Subscribed");
}

/* ================================================================
   Public C API
   ================================================================ */

void f1net_setCallback(F1NetStateCB cb) { s_callback = cb; }

void f1net_setup(void)
{
    Serial.println("[F1Net] setup");
    s_lastConnect = millis() - RECONNECT_INIT_MS;  /* connect immediately */
}

void f1net_loop(void)
{
    if (WiFi.status() != WL_CONNECTED) return;

    if (!s_wsOpen) {
        unsigned long now = millis();
        if (now - s_lastConnect >= s_reconnDelay) {
            s_lastConnect = now;
            connectSignalR();
        }
        return;
    }

    /* Check connection alive */
    if (!s_tcp.connected()) {
        Serial.println("[F1Net] TCP dropped");
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
        ws_send_text(s_tcp, "{}");
        s_lastPing = millis();
    }
}

bool       f1net_isConnected(void) { return s_wsOpen && s_tcp.connected(); }
F1NetState f1net_getState(void)    { return s_state; }

void f1net_disconnect(void)
{
    s_wsOpen    = false;
    s_connected = false;
    s_tcp.stop();
}
