/*
 * Replay.cpp  –  Offline session replay from livetiming.formula1.com/static/
 *
 * *** Do NOT include ESPAsyncWebServer.h or wled.h here. ***
 */

#include "Replay.h"
#include <WiFiClient.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstring>
#include <cstdlib>

static constexpr const char* HOST    = "livetiming.formula1.com";
static constexpr int         PORT    = 80;

/* ──────────────────────────────────────────────────────────────────────────
   Event store
   ────────────────────────────────────────────────────────────────────────── */
static ReplayEvent* s_events     = nullptr;
static volatile int s_eventCount = 0;
static int          s_eventIdx   = 0;

/* ──────────────────────────────────────────────────────────────────────────
   Playback state
   ────────────────────────────────────────────────────────────────────────── */
static volatile bool s_loading   = false;
static volatile bool s_active    = false;
static float         s_speed     = 5.0f;
static uint32_t      s_startMs   = 0;    /* millis() when replay started */

/* ──────────────────────────────────────────────────────────────────────────
   Callbacks – same as live F1 network so features (start lights, etc.) work
   ────────────────────────────────────────────────────────────────────────── */
static F1NetStateCB s_stateCB  = nullptr;
static F1EventCB    s_eventCB  = nullptr;

/* Pending session path for load task */
static char s_pendingPath[192] = {};
static float s_pendingSpeed    = 5.0f;

/* Sessions JSON cache */
static char* s_sessionsJson    = nullptr;

/* ──────────────────────────────────────────────────────────────────────────
   HTTP GET helper – plain TCP, returns heap-alloc'd body (caller frees)
   ────────────────────────────────────────────────────────────────────────── */
static char* httpGet(const char* path, size_t* outLen)
{
    *outLen = 0;
    WiFiClient c;
    c.setTimeout(12);
    if (!c.connect(HOST, PORT)) {
        Serial.printf("[Replay] connect fail: %s\n", path);
        return nullptr;
    }

    char req[512];
    snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\n"
        "User-Agent: F1Lamp\r\nAccept-Encoding: identity\r\n"
        "Connection: close\r\n\r\n",
        path, HOST);
    c.print(req);

    /* Read up to 32 KB.  Largest jsonStream for a race is ~25 KB. */
    const size_t CAP = 32 * 1024;
    char* buf = (char*)malloc(CAP);
    if (!buf) { c.stop(); return nullptr; }

    size_t len = 0;
    unsigned long t0 = millis();
    while (millis() - t0 < 18000) {
        while (c.available() && len < CAP - 1)
            buf[len++] = (char)c.read();
        if (!c.connected() && !c.available()) break;
        delay(1);
    }
    buf[len] = '\0';
    c.stop();

    /* Check HTTP status */
    int status = 0;
    sscanf(buf, "HTTP/1.1 %d", &status);
    if (status != 200) {
        Serial.printf("[Replay] HTTP %d: %s\n", status, path);
        free(buf); return nullptr;
    }

    /* Locate body */
    const char* body = strstr(buf, "\r\n\r\n");
    if (!body) { free(buf); return nullptr; }
    body += 4;

    size_t blen = len - (size_t)(body - buf);
    char* result = (char*)malloc(blen + 1);
    if (!result) { free(buf); return nullptr; }

    /* Handle chunked or plain body */
    bool chunked = (strstr(buf, "ransfer-Encoding: chunked") != nullptr);
    if (!chunked) {
        memcpy(result, body, blen);
        result[blen] = '\0';
        *outLen = blen;
    } else {
        /* Minimal chunk decoder */
        const char* src = body;
        char*       dst = result;
        size_t      dl  = 0;
        while (*src) {
            char* end;
            unsigned long csz = strtoul(src, &end, 16);
            if (end == src || csz == 0) break;
            while (*end == '\r' || *end == '\n') end++;
            if (dl + csz > blen) break;
            memcpy(dst, end, csz);
            dst += csz; dl += csz;
            src = end + csz;
            while (*src == '\r' || *src == '\n') src++;
        }
        *dst = '\0';
        *outLen = dl;
    }

    free(buf);
    return result;
}

/* ──────────────────────────────────────────────────────────────────────────
   Timestamp: "HH:MM:SS.mmm" → milliseconds
   ────────────────────────────────────────────────────────────────────────── */
static uint32_t parseTs(const char* s)
{
    unsigned h = 0, m = 0, sec = 0, ms = 0;
    sscanf(s, "%u:%u:%u.%u", &h, &m, &sec, &ms);
    return (h * 3600u + m * 60u + sec) * 1000u + ms;
}

/* ──────────────────────────────────────────────────────────────────────────
   Load one topic stream into s_events array
   ────────────────────────────────────────────────────────────────────────── */
static const char* TOPIC_NAMES[] = {
    "TrackStatus", "SessionStatus", "RaceControlMessages"
};

static void loadStream(const char* sessionPath, uint8_t topic)
{
    char url[256];
    snprintf(url, sizeof(url),
             "/static/%s%s.jsonStream", sessionPath, TOPIC_NAMES[topic]);
    Serial.printf("[Replay] Fetching %s\n", url);

    size_t blen = 0;
    char* body = httpGet(url, &blen);
    if (!body) { Serial.printf("[Replay] Failed: %s\n", url); return; }

    /* Parse line-by-line: "HH:MM:SS.mmm{json...}\n" */
    char* line = body;
    while (*line) {
        char* nl = line;
        while (*nl && *nl != '\n') nl++;

        size_t lineLen = (size_t)(nl - line);
        if (lineLen > 13) {
            /* Find opening brace */
            char* brace = line;
            while (brace < nl && *brace != '{') brace++;

            if (brace < nl && s_eventCount < REPLAY_MAX_EVENTS) {
                uint32_t ts = parseTs(line);
                const char* payload = brace;

                ReplayEvent& ev = s_events[s_eventCount];
                ev.ts_ms  = ts;
                ev.topic  = topic;
                ev.data[0] = '\0';

                bool stored = false;

                if (topic == 0) {
                    /* TrackStatus: extract Status code */
                    const char* sp = strstr(payload, "\"Status\":\"");
                    if (sp) {
                        sp += 10;
                        ev.data[0] = *sp; ev.data[1] = '\0';
                        stored = (ev.data[0] >= '1' && ev.data[0] <= '9');
                    }

                } else if (topic == 1) {
                    /* SessionStatus: extract Status string */
                    const char* sp = strstr(payload, "\"Status\":\"");
                    if (sp) {
                        sp += 10;
                        int i = 0;
                        while (*sp && *sp != '"' && i < (int)sizeof(ev.data) - 1)
                            ev.data[i++] = *sp++;
                        ev.data[i] = '\0';
                        stored = (i > 0);
                    }

                } else {
                    /* RaceControlMessages: only keep FL and DRS events */
                    const char* msg = payload;
                    bool isFl = strstr(msg, "FASTEST LAP") || strstr(msg, "Fastest Lap");
                    bool isDrs = (strstr(msg, "DRS") || strstr(msg, "Drs"))
                              && (strstr(msg, "ENABLED") || strstr(msg, "Enabled"));
                    if (isFl) {
                        strlcpy(ev.data, "FL", sizeof(ev.data));
                        stored = true;
                    } else if (isDrs) {
                        strlcpy(ev.data, "DRS", sizeof(ev.data));
                        stored = true;
                    }
                }

                if (stored) s_eventCount++;
            }
        }

        line = (*nl == '\n') ? nl + 1 : nl;
        if (!*line) break;
    }

    free(body);
    Serial.printf("[Replay] %s: loaded (total events so far: %d)\n",
                  TOPIC_NAMES[topic], (int)s_eventCount);
}

/* ──────────────────────────────────────────────────────────────────────────
   qsort comparator
   ────────────────────────────────────────────────────────────────────────── */
static int cmpEvent(const void* a, const void* b)
{
    uint32_t ta = ((const ReplayEvent*)a)->ts_ms;
    uint32_t tb = ((const ReplayEvent*)b)->ts_ms;
    return (ta > tb) - (ta < tb);
}

/* ──────────────────────────────────────────────────────────────────────────
   Dispatch one event through the registered callbacks
   ────────────────────────────────────────────────────────────────────────── */
static void dispatchEvent(const ReplayEvent& ev)
{
    unsigned m = ev.ts_ms / 60000;
    unsigned s = (ev.ts_ms % 60000) / 1000;
    Serial.printf("[Replay] %02u:%02u  %s  \"%s\"\n",
                  m, s, TOPIC_NAMES[ev.topic], ev.data);

    if (ev.topic == 0) {
        /* TrackStatus → state */
        static const struct { char code; F1NetState st; } kMap[] = {
            {'1', F1ST_GREEN},
            {'2', F1ST_YELLOW},
            {'4', F1ST_SAFETY_CAR},
            {'5', F1ST_RED_FLAG},
            {'6', F1ST_VIRTUAL_SC},
            {'7', F1ST_VSC_ENDING},
        };
        for (auto& m : kMap) {
            if (ev.data[0] == m.code) {
                if (s_stateCB) s_stateCB(m.st);
                break;
            }
        }

    } else if (ev.topic == 1) {
        /* SessionStatus → state */
        if (strstr(ev.data, "Started")) {
            if (s_stateCB) s_stateCB(F1ST_SESSION_START);
        } else if (strstr(ev.data, "Finished") || strstr(ev.data, "Ends")) {
            if (s_stateCB) s_stateCB(F1ST_CHEQUERED);
        } else if (strstr(ev.data, "Inactive")) {
            if (s_stateCB) s_stateCB(F1ST_IDLE);
        }

    } else {
        /* RaceControlMessages → event */
        if (strcmp(ev.data, "FL")  == 0 && s_eventCB) s_eventCB(F1EVT_FASTEST_LAP);
        if (strcmp(ev.data, "DRS") == 0 && s_eventCB) s_eventCB(F1EVT_DRS_ENABLED);
    }
}

/* ──────────────────────────────────────────────────────────────────────────
   Background load task
   ────────────────────────────────────────────────────────────────────────── */
static void replayLoadTask(void*)
{
    Serial.printf("[Replay] Load task start: %s  %.0fx\n",
                  s_pendingPath, s_pendingSpeed);

    loadStream(s_pendingPath, 0);
    loadStream(s_pendingPath, 1);
    loadStream(s_pendingPath, 2);

    qsort(s_events, s_eventCount, sizeof(ReplayEvent), cmpEvent);

    Serial.printf("[Replay] Ready: %d events\n", (int)s_eventCount);

    s_eventIdx  = 0;
    s_speed     = s_pendingSpeed;
    s_startMs   = millis();
    s_loading   = false;
    s_active    = true;

    vTaskDelete(nullptr);
}

/* ══════════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════════ */

void replay_setCallbacks(F1NetStateCB stateCB, F1EventCB eventCB)
{
    s_stateCB = stateCB;
    s_eventCB = eventCB;
}

void replay_start(const char* sessionPath, float speed)
{
    replay_stop();

    s_events = (ReplayEvent*)malloc(REPLAY_MAX_EVENTS * sizeof(ReplayEvent));
    if (!s_events) {
        Serial.println("[Replay] OOM allocating event array");
        return;
    }
    s_eventCount = 0;

    strlcpy(s_pendingPath, sessionPath, sizeof(s_pendingPath));
    /* Ensure trailing slash */
    size_t pl = strlen(s_pendingPath);
    if (pl > 0 && s_pendingPath[pl - 1] != '/') {
        s_pendingPath[pl] = '/'; s_pendingPath[pl + 1] = '\0';
    }
    s_pendingSpeed = (speed < 0.1f) ? 1.0f : speed;

    s_loading = true;
    s_active  = false;

    xTaskCreate(replayLoadTask, "replay_ld", 6144, nullptr, 1, nullptr);
}

void replay_stop()
{
    s_active  = false;
    s_loading = false;
    if (s_events) { free(s_events); s_events = nullptr; }
    s_eventCount = 0;
    s_eventIdx   = 0;
}

bool  replay_isLoading()   { return s_loading; }
bool  replay_isActive()    { return s_active;  }
int   replay_eventCount()  { return s_eventCount; }
int   replay_currentIdx()  { return s_eventIdx;   }
float replay_speed()       { return s_speed; }

void replay_tick()
{
    if (!s_active || !s_events) return;

    if (s_eventIdx >= s_eventCount) {
        Serial.println("[Replay] Complete.");
        replay_stop();
        return;
    }

    /* How far through the simulated session are we? */
    uint32_t simMs = (uint32_t)((uint64_t)(millis() - s_startMs) * (uint32_t)(s_speed * 100) / 100);

    while (s_eventIdx < s_eventCount &&
           s_events[s_eventIdx].ts_ms <= simMs) {
        dispatchEvent(s_events[s_eventIdx]);
        s_eventIdx++;
    }
}

/* ──────────────────────────────────────────────────────────────────────────
   Session index fetcher
   Turn "2026/2026-03-08_Australian_Grand_Prix/2026-03-06_Practice_1/"
   into "Australian Grand Prix - Practice 1"
   ────────────────────────────────────────────────────────────────────────── */
static void pathToLabel(const char* path, char* out, size_t outLen)
{
    /* Skip year segment: "2026/" */
    const char* p1 = strchr(path, '/');
    if (!p1 || !*++p1) { strlcpy(out, path, outLen); return; }

    /* Meeting segment: "2026-03-08_Australian_Grand_Prix" */
    const char* p2 = strchr(p1, '/');
    if (!p2) { strlcpy(out, path, outLen); return; }

    /* Session segment: "2026-03-06_Practice_1" */
    const char* p3 = p2 + 1;
    const char* p4 = strchr(p3, '/');   /* may be nullptr for malformed paths */

    /* Skip date prefix (e.g. "2026-03-08_") in each segment */
    auto afterDate = [](const char* s) -> const char* {
        /* find 3rd '-' then '_' after it */
        int d = 0;
        while (*s && d < 3) { if (*s == '-') d++; s++; }
        return s;   /* points to name after "YYYY-MM-DD-" compat */
    };

    const char* meetName = strchr(p1, '_');
    if (!meetName || meetName >= p2) meetName = p1; else meetName++;

    const char* sessName = strchr(p3, '_');
    if (!sessName || (p4 && sessName >= p4)) sessName = p3; else sessName++;

    char meet[48] = {}, sess[32] = {};
    int mi = 0;
    for (const char* c = meetName; c < p2 && mi < 47; c++, mi++)
        meet[mi] = (*c == '_') ? ' ' : *c;

    int si = 0;
    const char* sessEnd = p4 ? p4 : sessName + strlen(sessName);
    for (const char* c = sessName; c < sessEnd && si < 31; c++, si++)
        sess[si] = (*c == '_') ? ' ' : *c;

    snprintf(out, outLen, "%s - %s", meet, sess);
}

const char* replay_fetchSessionsJson(int year)
{
    if (s_sessionsJson) { free(s_sessionsJson); s_sessionsJson = nullptr; }

    char url[64];
    snprintf(url, sizeof(url), "/static/%d/Index.json", year);

    size_t blen = 0;
    char* body = httpGet(url, &blen);
    if (!body) return "[{\"l\":\"Fetch failed\",\"p\":\"\"}]";

    const size_t OUT_CAP = 6144;
    char* out = (char*)malloc(OUT_CAP);
    if (!out) { free(body); return "[]"; }

    size_t pos = 0;
    auto append = [&](const char* s) {
        size_t l = strlen(s);
        if (pos + l < OUT_CAP - 2) { memcpy(out + pos, s, l); pos += l; }
    };

    append("[");
    bool first = true;
    const char* cur = body;

    while (*cur) {
        const char* pp = strstr(cur, "\"Path\":\"2026/");
        if (!pp) break;
        pp += 8;  /* skip "Path":" */

        char sessPath[128] = {};
        const char* vp = pp;
        int pi = 0;
        while (*vp && *vp != '"' && pi < 127) sessPath[pi++] = *vp++;
        sessPath[pi] = '\0';

        /* Count '/' – session paths have ≥ 3 (year/meeting/session/) */
        int slashes = 0;
        for (int i = 0; sessPath[i]; i++) if (sessPath[i] == '/') slashes++;

        if (slashes >= 3) {
            char label[80] = {};
            pathToLabel(sessPath, label, sizeof(label));

            char entry[256];
            /* Escape potential quotes in label (shouldn't exist, but safe) */
            snprintf(entry, sizeof(entry),
                     "%s{\"l\":\"%s\",\"p\":\"%s\"}",
                     first ? "" : ",", label, sessPath);
            append(entry);
            first = false;
        }

        cur = *vp ? vp + 1 : vp;
    }

    append("]");
    out[pos] = '\0';

    free(body);
    s_sessionsJson = out;
    Serial.printf("[Replay] sessions JSON: %d bytes\n", (int)pos);
    return s_sessionsJson;
}
