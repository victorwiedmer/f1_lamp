/*
 * F1Sessions.cpp  –  Fetch the F1 live-timing session index over HTTPS,
 *                    trim it to { meetings: [ { name, location, sessions:
 *                    [{name,type,date,path}] } ] }, and cache the result.
 *
 * Uses raw mbedtls API for TLS + lwIP sockets for TCP, bypassing esp_tls
 * which has TLS compiled out (CONFIG_MBEDTLS_TLS_DISABLED=1 in the SDK).
 *
 * Must be called from the f1net FreeRTOS task (not the web-server callback).
 */

#include "F1Sessions.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>

/* Replay engine – for replay_startFromEvents() */
#include "Replay.h"

/* mbedtls */
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/debug.h"

/* lwIP for raw sockets */
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/inet.h>

/* FreeRTOS for vTaskDelay */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* ── Config ───────────────────────────────────────────────────────────── */
static constexpr const char* HOST = "livetiming.formula1.com";
static constexpr const char* PORT_STR = "443";
static constexpr uint32_t    TIMEOUT_MS = 20000;

/* ── Cached result ────────────────────────────────────────────────────── */
static String  s_json;
static bool    s_hasData  = false;
static volatile bool s_fetching = false;
static volatile bool s_fetchRequested = false;
static int     s_retries  = 0;
static constexpr int MAX_RETRIES = 3;
static String  s_lastError;   /* diagnostic – readable via API */

/* ── BIO callbacks for mbedtls ←→ lwIP socket ────────────────────────── */

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
    return r;  /* 0 = EOF, >0 = bytes read */
}

/* ── TLS connection helper ───────────────────────────────────────────── */

struct TlsConn {
    int                     sock;
    mbedtls_ssl_context     ssl;
    mbedtls_ssl_config      conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    bool                    connected;
};

static void tls_cleanup(TlsConn& c)
{
    if (c.connected) {
        mbedtls_ssl_close_notify(&c.ssl);
    }
    mbedtls_ssl_free(&c.ssl);
    mbedtls_ssl_config_free(&c.conf);
    mbedtls_ctr_drbg_free(&c.ctr_drbg);
    mbedtls_entropy_free(&c.entropy);
    if (c.sock >= 0) {
        lwip_close(c.sock);
        c.sock = -1;
    }
    c.connected = false;
}

static bool tls_connect(TlsConn& c, const char* host, const char* port)
{
    char errbuf[128];
    c.sock = -1;
    c.connected = false;

    /* Init all contexts */
    mbedtls_ssl_init(&c.ssl);
    mbedtls_ssl_config_init(&c.conf);
    mbedtls_ctr_drbg_init(&c.ctr_drbg);
    mbedtls_entropy_init(&c.entropy);

    /* Seed DRBG */
    int ret = mbedtls_ctr_drbg_seed(&c.ctr_drbg, mbedtls_entropy_func,
                                     &c.entropy, nullptr, 0);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        s_lastError = "DRBG seed: " + String(errbuf);
        tls_cleanup(c);
        return false;
    }

    /* SSL config */
    ret = mbedtls_ssl_config_defaults(&c.conf,
            MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        s_lastError = "SSL config: " + String(errbuf);
        tls_cleanup(c);
        return false;
    }

    mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c.conf, mbedtls_ctr_drbg_random, &c.ctr_drbg);

    ret = mbedtls_ssl_setup(&c.ssl, &c.conf);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        s_lastError = "SSL setup: " + String(errbuf);
        tls_cleanup(c);
        return false;
    }

    ret = mbedtls_ssl_set_hostname(&c.ssl, host);
    if (ret != 0) {
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        s_lastError = "Set hostname: " + String(errbuf);
        tls_cleanup(c);
        return false;
    }

    /* TCP connect with retry */
    Serial.printf("[F1Sess] DNS resolve + TCP connect to %s:%s ...\n", host, port);
    struct addrinfo hints = {}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ret = lwip_getaddrinfo(host, port, &hints, &res);
    if (ret != 0 || !res) {
        s_lastError = "DNS failed: " + String(ret);
        tls_cleanup(c);
        return false;
    }

    /* Log resolved IP for diagnostics */
    {
        struct sockaddr_in* sa = (struct sockaddr_in*)res->ai_addr;
        Serial.printf("[F1Sess] Resolved %s -> %s\n", host,
                      inet_ntoa(sa->sin_addr));
    }

    bool tcpOk = false;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            Serial.printf("[F1Sess] TCP retry %d/3 after 3s...\n", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(3000));
        }

        c.sock = lwip_socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (c.sock < 0) {
            s_lastError = "socket() failed: errno=" + String(errno);
            continue;
        }

        /* Set socket timeout */
        struct timeval tv;
        tv.tv_sec = TIMEOUT_MS / 1000;
        tv.tv_usec = (TIMEOUT_MS % 1000) * 1000;
        lwip_setsockopt(c.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        lwip_setsockopt(c.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        ret = lwip_connect(c.sock, res->ai_addr, res->ai_addrlen);
        if (ret == 0) { tcpOk = true; break; }

        Serial.printf("[F1Sess] TCP connect errno=%d\n", errno);
        lwip_close(c.sock);
        c.sock = -1;
    }
    lwip_freeaddrinfo(res);
    if (!tcpOk) {
        s_lastError = "TCP connect failed after 3 tries: errno=" + String(errno);
        tls_cleanup(c);
        return false;
    }
    Serial.println("[F1Sess] TCP connected, starting TLS handshake...");

    /* Set BIO */
    mbedtls_ssl_set_bio(&c.ssl, &c.sock, bio_send, bio_recv, nullptr);

    /* TLS handshake */
    unsigned long t0 = millis();
    while ((ret = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf2[128];
            mbedtls_strerror(ret, errbuf2, sizeof(errbuf2));
            s_lastError = "TLS HS: " + String(errbuf2)
                        + " (0x" + String(-ret, HEX) + ")"
                        + " state=" + String(c.ssl.state)
                        + " heap=" + String(ESP.getFreeHeap());
            Serial.println("[F1Sess] " + s_lastError);
            tls_cleanup(c);
            return false;
        }
        if (millis() - t0 > TIMEOUT_MS) {
            s_lastError = "TLS handshake timeout";
            tls_cleanup(c);
            return false;
        }
        delay(1);
    }

    c.connected = true;
    Serial.println("[F1Sess] TLS handshake complete!");
    return true;
}

static bool tls_write_all(TlsConn& c, const char* data, size_t len)
{
    size_t sent = 0;
    unsigned long t0 = millis();
    while (sent < len && millis() - t0 < TIMEOUT_MS) {
        int r = mbedtls_ssl_write(&c.ssl,
                    (const unsigned char*)data + sent, len - sent);
        if (r > 0) {
            sent += (size_t)r;
        } else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) {
            delay(1);
        } else {
            return false;
        }
    }
    return sent == len;
}

static String tls_read_all(TlsConn& c)
{
    String out;
    out.reserve(8192);
    unsigned long t0 = millis();
    while (millis() - t0 < TIMEOUT_MS) {
        unsigned char buf[512];
        int r = mbedtls_ssl_read(&c.ssl, buf, sizeof(buf));
        if (r > 0) {
            out.concat((const char*)buf, (unsigned int)r);
            t0 = millis();  /* reset timeout on data */
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;  /* connection closed */
        } else if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            delay(1);
        } else {
            break;  /* error */
        }
    }
    return out;
}

/* ── Public API ───────────────────────────────────────────────────────── */

bool f1sessions_fetch(int year)
{
    s_fetchRequested = false;
    s_fetching = true;
    s_retries++;
    s_lastError = "";
    Serial.printf("[F1Sess] Fetching %d/Index.json (attempt %d/%d) heap=%u\n",
                  year, s_retries, MAX_RETRIES, ESP.getFreeHeap());

    /* Heap-allocate TlsConn – the struct is ~3KB (entropy_context alone
       is ~1.6KB) and stack space is precious on FreeRTOS tasks. */
    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) {
        s_lastError = "OOM: TlsConn alloc";
        s_fetching = false;
        return false;
    }

    /* Use a lambda-style cleanup so every exit path frees the heap block */
    #define FAIL_RET(msg) do { s_lastError = (msg); \
        Serial.println("[F1Sess] " + s_lastError); \
        tls_cleanup(*cp); delete cp; s_fetching = false; return false; } while(0)

    if (!tls_connect(*cp, HOST, PORT_STR)) {
        Serial.println("[F1Sess] FAIL: " + s_lastError);
        delete cp;   /* tls_connect already cleaned up internals */
        s_fetching = false;
        return false;
    }

    /* Send HTTP GET */
    char path[64];
    snprintf(path, sizeof(path), "/static/%d/Index.json", year);

    String req = String("GET ") + path + " HTTP/1.1\r\n"
               + "Host: " + HOST + "\r\n"
               + "Accept: application/json\r\n"
               + "Connection: close\r\n\r\n";

    if (!tls_write_all(*cp, req.c_str(), req.length())) {
        FAIL_RET("Write failed");
    }

    /* Read entire response (Connection: close) */
    String raw = tls_read_all(*cp);
    tls_cleanup(*cp);
    delete cp;
    cp = nullptr;

    if (raw.length() == 0) {
        s_lastError = "Empty response (0 bytes)";
        s_fetching = false;
        return false;
    }

    /* Split headers from body */
    int bodyStart = raw.indexOf("\r\n\r\n");
    if (bodyStart < 0) {
        s_lastError = "No header/body separator";
        s_fetching = false;
        return false;
    }
    String headers = raw.substring(0, bodyStart);
    String body = raw.substring(bodyStart + 4);
    int rawLen = raw.length();
    raw = String();  /* free */

    Serial.printf("[F1Sess] rawLen=%d bodyLen=%d\n", rawLen, (int)body.length());

    if (headers.indexOf("200") < 0) {
        s_lastError = "HTTP: " + headers.substring(0, 60);
        s_fetching = false;
        return false;
    }

    if (body.length() < 10) {
        s_lastError = "Body too short: " + String(body.length()) + "b";
        s_fetching = false;
        return false;
    }

    /* Handle chunked transfer-encoding: dechunk manually */
    if (headers.indexOf("chunked") >= 0) {
        String dechunked;
        dechunked.reserve(body.length());
        int pos = 0;
        while (pos < (int)body.length()) {
            int nl = body.indexOf("\r\n", pos);
            if (nl < 0) break;
            String hexLen = body.substring(pos, nl);
            hexLen.trim();
            unsigned long chunkLen = strtoul(hexLen.c_str(), nullptr, 16);
            if (chunkLen == 0) break;  /* final chunk */
            int dataStart = nl + 2;
            if (dataStart + (int)chunkLen > (int)body.length()) break;
            dechunked.concat(body.c_str() + dataStart, (unsigned int)chunkLen);
            pos = dataStart + (int)chunkLen + 2; /* skip chunk data + \r\n */
        }
        body = dechunked;
    }

    /* Strip UTF-8 BOM if present */
    if (body.length() >= 3 &&
        (unsigned char)body[0] == 0xEF &&
        (unsigned char)body[1] == 0xBB &&
        (unsigned char)body[2] == 0xBF) {
        body = body.substring(3);
    }

    /* Parse and trim to a compact JSON for the WebUI */
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
        s_lastError = "JSON parse: " + String(err.c_str())
                    + " bodyLen=" + String(body.length());
        Serial.println("[F1Sess] " + s_lastError);
        s_fetching = false;
        return false;
    }
    body = String();  /* free raw body now */

    /* Build trimmed output: array of meetings with sessions */
    JsonDocument out;
    JsonArray meetings = out["meetings"].to<JsonArray>();

    for (JsonObject m : doc["Meetings"].as<JsonArray>()) {
        JsonObject mo = meetings.add<JsonObject>();
        mo["name"]     = m["Name"];
        mo["location"] = m["Location"];
        mo["code"]     = m["Code"];

        JsonArray sess = mo["sessions"].to<JsonArray>();
        for (JsonObject s : m["Sessions"].as<JsonArray>()) {
            JsonObject so = sess.add<JsonObject>();
            so["name"] = s["Name"];
            so["type"] = s["Type"];
            /* StartDate: "2026-03-06T12:30:00" → "2026-03-06 12:30" */
            String sd = s["StartDate"] | "";
            sd.replace("T", " ");
            if (sd.length() > 16) sd = sd.substring(0, 16);
            so["date"] = sd;
            /* Path present only for sessions with available data */
            const char* path = s["Path"] | "";
            if (path[0]) so["path"] = path;
        }
    }

    s_json = String();
    serializeJson(out, s_json);
    s_hasData = true;
    s_fetching = false;
    #undef FAIL_RET
    Serial.printf("[F1Sess] Cached %u bytes, %d meetings\n",
                  s_json.length(), meetings.size());
    return true;
}

/* ── Generic HTTPS GET helper (reusable by other modules) ─────────── */

String f1sessions_httpsGet(const char* path, String& outError,
                          const char* host, const char* port)
{
    const char* h = host ? host : HOST;
    const char* p = port ? port : PORT_STR;

    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) { outError = "OOM: TlsConn"; return String(); }

    if (!tls_connect(*cp, h, p)) {
        outError = s_lastError;
        delete cp;
        return String();
    }

    String req = String("GET ") + path + " HTTP/1.1\r\n"
               + "Host: " + h + "\r\n"
               + "Accept: application/json\r\n"
               + "Connection: close\r\n\r\n";

    if (!tls_write_all(*cp, req.c_str(), req.length())) {
        outError = "Write failed";
        tls_cleanup(*cp); delete cp;
        return String();
    }

    String raw = tls_read_all(*cp);
    tls_cleanup(*cp);
    delete cp;

    if (raw.length() == 0) { outError = "Empty response"; return String(); }

    int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) { outError = "No header separator"; return String(); }
    String headers = raw.substring(0, sep);
    String body = raw.substring(sep + 4);
    raw = String();

    if (headers.indexOf("200") < 0) {
        outError = "HTTP: " + headers.substring(0, 60);
        return String();
    }

    /* Dechunk if needed */
    if (headers.indexOf("chunked") >= 0) {
        String dc; dc.reserve(body.length());
        int pos = 0;
        while (pos < (int)body.length()) {
            int nl = body.indexOf("\r\n", pos);
            if (nl < 0) break;
            unsigned long cl = strtoul(body.c_str() + pos, nullptr, 16);
            if (cl == 0) break;
            int ds = nl + 2;
            if (ds + (int)cl > (int)body.length()) break;
            dc.concat(body.c_str() + ds, (unsigned int)cl);
            pos = ds + (int)cl + 2;
        }
        body = dc;
    }

    /* Strip UTF-8 BOM */
    if (body.length() >= 3 &&
        (uint8_t)body[0] == 0xEF && (uint8_t)body[1] == 0xBB && (uint8_t)body[2] == 0xBF)
        body = body.substring(3);

    return body;
}

bool f1sessions_hasData()       { return s_hasData;  }
bool f1sessions_isFetching()    { return s_fetching || s_fetchRequested; }
bool f1sessions_fetchRequested() { return s_fetchRequested; }

const String& f1sessions_json() {
    static String empty = "{}";
    return s_hasData ? s_json : empty;
}

void f1sessions_clear() {
    s_json = String();
    s_hasData = false;
}

/* ── Async fetch request (picked up by the f1net task) ────────────────── */
void f1sessions_requestFetch(int year)
{
    if (s_fetching || s_hasData || s_fetchRequested) return;
    if (s_retries >= MAX_RETRIES) return;   /* give up after N failures */
    s_fetchRequested = true;
    Serial.printf("[F1Sess] Fetch requested (year %d)\n", year);
}

const String& f1sessions_lastError() { return s_lastError; }

void f1sessions_resetRetries() { s_retries = 0; s_lastError = ""; }

/* ═════════════════════════════════════════════════════════════════════════
   Session Replay – fetch .jsonStream files, parse events, start replay
   ═════════════════════════════════════════════════════════════════════════ */

static char     s_replayPath[160]  = "";
static float    s_replaySpeed      = 5.0f;
static volatile bool s_replayRequested = false;
static volatile bool s_replayFetching  = false;

void f1sessions_requestReplay(const char* sessionPath, float speed)
{
    if (s_fetching || s_replayRequested || s_replayFetching) return;
    strlcpy(s_replayPath, sessionPath, sizeof(s_replayPath));
    s_replaySpeed = speed;
    s_replayRequested = true;
    Serial.printf("[F1Sess] Replay requested: %s @ %.0fx\n", sessionPath, speed);
}

bool f1sessions_replayRequested()   { return s_replayRequested; }
bool f1sessions_isReplayFetching()  { return s_replayFetching || s_replayRequested; }

/* ── Timestamp parser: "HH:MM:SS.fff" → milliseconds ─────────────────── */
static uint32_t parseStreamTs(const char* p, int len)
{
    int h = 0, m = 0, s = 0, ms = 0;
    char buf[18];
    int n = (len < 17) ? len : 17;
    memcpy(buf, p, n);
    buf[n] = '\0';
    /* sscanf handles "12:34:56.789" and also "0:00:00.0" gracefully */
    sscanf(buf, "%d:%d:%d.%d", &h, &m, &s, &ms);
    /* normalise fractional part to 3 digits */
    if      (ms > 0 && ms < 10)  ms *= 100;
    else if (ms >= 10 && ms < 100) ms *= 10;
    return (uint32_t)(h * 3600000UL + m * 60000UL + s * 1000UL + ms);
}

/* ── Parse one .jsonStream body into ReplayEvents ─────────────────────── */
static int parseStreamLines(const String& body, uint8_t topic,
                            ReplayEvent* events, int maxEvents, int startIdx)
{
    int count = startIdx;
    const char* p   = body.c_str();
    const char* end = p + body.length();

    while (p < end && count < maxEvents) {
        /* find end of line */
        const char* eol = (const char*)memchr(p, '\n', end - p);
        if (!eol) eol = end;
        int lineLen = eol - p;
        if (lineLen < 5) { p = eol + 1; continue; }

        /* find opening brace (start of JSON payload) */
        const char* brace = (const char*)memchr(p, '{', lineLen);
        if (!brace) { p = eol + 1; continue; }

        uint32_t ts = parseStreamTs(p, brace - p);
        int payloadLen = eol - brace;

        if (topic <= 1) {
            /* TrackStatus (0) / SessionStatus (1) – extract "Status":"value" */
            const char* marker = "\"Status\":\"";
            const int  mLen = 10;
            const char* found = nullptr;
            for (const char* s = brace; s + mLen < eol; s++) {
                if (memcmp(s, marker, mLen) == 0) { found = s; break; }
            }
            if (found) {
                const char* vs = found + mLen;
                const char* ve = (const char*)memchr(vs, '"', eol - vs);
                if (ve && (ve - vs) < (int)sizeof(events[0].data)) {
                    events[count].ts_ms = ts;
                    events[count].topic = topic;
                    int vlen = ve - vs;
                    memcpy(events[count].data, vs, vlen);
                    events[count].data[vlen] = '\0';
                    count++;
                }
            }
        } else {
            /* RaceControlMessages – keyword search, no JSON parse needed */
            char buf[512];
            int n = (payloadLen < 511) ? payloadLen : 511;
            memcpy(buf, brace, n);
            buf[n] = '\0';
            for (int i = 0; i < n; i++)
                if (buf[i] >= 'a' && buf[i] <= 'z') buf[i] -= 32;

            if (strstr(buf, "FASTEST LAP") && count < maxEvents) {
                events[count].ts_ms = ts;
                events[count].topic = 2;
                strlcpy(events[count].data, "FL", sizeof(events[0].data));
                count++;
            }
            if (strstr(buf, "DRS") && strstr(buf, "ENABLED") && count < maxEvents) {
                events[count].ts_ms = ts;
                events[count].topic = 2;
                strlcpy(events[count].data, "DRS", sizeof(events[0].data));
                count++;
            }
        }
        p = eol + 1;
    }
    return count;
}

/* ── Fetch one .jsonStream file and extract its body ──────────────────── */
static String fetchOneStream(const char* streamPath)
{
    TlsConn* cp = new (std::nothrow) TlsConn;
    if (!cp) return String();
    if (!tls_connect(*cp, HOST, PORT_STR)) {
        delete cp;
        return String();
    }
    String req = String("GET /static/") + streamPath + " HTTP/1.1\r\n"
               + "Host: " + HOST + "\r\n"
               + "Accept: */*\r\n"
               + "Connection: close\r\n\r\n";
    if (!tls_write_all(*cp, req.c_str(), req.length())) {
        tls_cleanup(*cp); delete cp;
        return String();
    }
    String raw = tls_read_all(*cp);
    tls_cleanup(*cp); delete cp;

    if (raw.length() == 0) return String();

    /* Split headers / body */
    int sep = raw.indexOf("\r\n\r\n");
    if (sep < 0) return String();
    String headers = raw.substring(0, sep);
    String body    = raw.substring(sep + 4);
    raw = String();

    if (headers.indexOf("200") < 0) return String();   /* 404 = no data */

    /* Dechunk if needed */
    if (headers.indexOf("chunked") >= 0) {
        String dc; dc.reserve(body.length());
        int pos = 0;
        while (pos < (int)body.length()) {
            int nl = body.indexOf("\r\n", pos);
            if (nl < 0) break;
            unsigned long cl = strtoul(body.c_str() + pos, nullptr, 16);
            if (cl == 0) break;
            int ds = nl + 2;
            if (ds + (int)cl > (int)body.length()) break;
            dc.concat(body.c_str() + ds, (unsigned int)cl);
            pos = ds + (int)cl + 2;
        }
        body = dc;
    }
    /* Strip BOM */
    if (body.length() >= 3 &&
        (uint8_t)body[0] == 0xEF && (uint8_t)body[1] == 0xBB && (uint8_t)body[2] == 0xBF)
        body = body.substring(3);

    return body;
}

/* ── Public: fetch all 3 streams, parse, start replay ─────────────────── */
bool f1sessions_fetchAndReplay()
{
    s_replayRequested = false;
    s_replayFetching  = true;
    s_lastError       = "";

    Serial.printf("[F1Sess] Fetching session streams: %s  heap=%u\n",
                  s_replayPath, ESP.getFreeHeap());

    ReplayEvent* events = (ReplayEvent*)malloc(REPLAY_MAX_EVENTS * sizeof(ReplayEvent));
    if (!events) {
        s_lastError = "OOM: event array";
        s_replayFetching = false;
        return false;
    }
    int eventCount = 0;

    static const char* TOPICS[]   = {"TrackStatus", "SessionStatus", "RaceControlMessages"};
    static const uint8_t TIDS[]   = {0, 1, 2};

    for (int t = 0; t < 3; t++) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s.jsonStream", s_replayPath, TOPICS[t]);
        Serial.printf("[F1Sess] GET %s ...\n", TOPICS[t]);

        String body = fetchOneStream(path);
        if (body.length() == 0) {
            Serial.printf("[F1Sess] %s: no data / fetch failed\n", TOPICS[t]);
            continue;
        }
        int before = eventCount;
        eventCount = parseStreamLines(body, TIDS[t], events, REPLAY_MAX_EVENTS, eventCount);
        Serial.printf("[F1Sess] %s: %d events (body %u bytes)\n",
                      TOPICS[t], eventCount - before, body.length());
    }

    s_replayFetching = false;

    if (eventCount == 0) {
        free(events);
        s_lastError = "No events in session streams";
        Serial.println("[F1Sess] " + s_lastError);
        return false;
    }

    /* Hand ownership to the replay engine (it will free on stop) */
    replay_startFromEvents(events, eventCount, s_replaySpeed);
    Serial.printf("[F1Sess] Replay started: %d events @ %.0fx\n", eventCount, s_replaySpeed);
    return true;
}
