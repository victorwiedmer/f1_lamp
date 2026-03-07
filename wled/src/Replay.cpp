/*
 * Replay.cpp  –  Static session replay from LittleFS
 *
 * Reads /replay.json from flash, parses the pre-built event array,
 * and plays back through the same callback system as live F1 timing.
 */

#include "Replay.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <cstring>
#include <cstdlib>

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
static uint32_t      s_startMs   = 0;

/* ──────────────────────────────────────────────────────────────────────────
   Callbacks
   ────────────────────────────────────────────────────────────────────────── */
static F1NetStateCB s_stateCB  = nullptr;
static F1EventCB    s_eventCB  = nullptr;

/* ──────────────────────────────────────────────────────────────────────────
   Topic names (for logging)
   ────────────────────────────────────────────────────────────────────────── */
static const char* TOPIC_NAMES[] = {
    "TrackStatus", "SessionStatus", "RaceControlMessages"
};

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
        static const struct { char code; F1NetState st; } kMap[] = {
            {'1', F1ST_GREEN},
            {'2', F1ST_YELLOW},
            {'4', F1ST_SAFETY_CAR},
            {'5', F1ST_RED_FLAG},
            {'6', F1ST_VIRTUAL_SC},
            {'7', F1ST_VSC_ENDING},
        };
        for (auto& mp : kMap) {
            if (ev.data[0] == mp.code) {
                if (s_stateCB) s_stateCB(mp.st);
                break;
            }
        }
    } else if (ev.topic == 1) {
        if (strstr(ev.data, "Started")) {
            if (s_stateCB) s_stateCB(F1ST_SESSION_START);
        } else if (strstr(ev.data, "Finished") || strstr(ev.data, "Ends")) {
            if (s_stateCB) s_stateCB(F1ST_CHEQUERED);
        } else if (strstr(ev.data, "Inactive")) {
            if (s_stateCB) s_stateCB(F1ST_IDLE);
        }
    } else {
        if (strcmp(ev.data, "FL")  == 0 && s_eventCB) s_eventCB(F1EVT_FASTEST_LAP);
        if (strcmp(ev.data, "DRS") == 0 && s_eventCB) s_eventCB(F1EVT_DRS_ENABLED);
    }
}

/* ══════════════════════════════════════════════════════════════════════════
   Public API
   ══════════════════════════════════════════════════════════════════════════ */

void replay_setCallbacks(F1NetStateCB stateCB, F1EventCB eventCB)
{
    s_stateCB = stateCB;
    s_eventCB = eventCB;
}

/* ──────────────────────────────────────────────────────────────────────────
   Load /replay.json from LittleFS and begin playback
   ────────────────────────────────────────────────────────────────────────── */
void replay_start(float speed)
{
    replay_stop();

    if (!LittleFS.begin(true)) {
        Serial.println("[Replay] LittleFS mount failed");
        return;
    }

    File f = LittleFS.open("/replay.json", "r");
    if (!f) {
        Serial.println("[Replay] /replay.json not found");
        return;
    }

    s_events = (ReplayEvent*)malloc(REPLAY_MAX_EVENTS * sizeof(ReplayEvent));
    if (!s_events) {
        Serial.println("[Replay] OOM allocating event array");
        f.close();
        return;
    }
    s_eventCount = 0;
    s_loading = true;

    /* Parse JSON */
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[Replay] JSON parse error: %s\n", err.c_str());
        s_loading = false;
        free(s_events); s_events = nullptr;
        return;
    }

    JsonArray events = doc["events"].as<JsonArray>();
    for (JsonObject ev : events) {
        if (s_eventCount >= REPLAY_MAX_EVENTS) break;
        ReplayEvent& re = s_events[s_eventCount];
        re.ts_ms = ev["ms"] | (uint32_t)0;
        re.topic = ev["t"]  | (uint8_t)0;
        strlcpy(re.data, ev["d"] | "", sizeof(re.data));
        s_eventCount++;
    }

    if (s_eventCount > 0) {
        qsort(s_events, s_eventCount, sizeof(ReplayEvent), cmpEvent);
        s_eventIdx = 0;
        s_speed    = (speed < 0.1f) ? 1.0f : speed;
        s_startMs  = millis();
        s_loading  = false;
        s_active   = true;
        Serial.printf("[Replay] Ready: %d events  %.0fx\n",
                      (int)s_eventCount, s_speed);
    } else {
        Serial.println("[Replay] No events found in replay.json");
        s_loading = false;
        free(s_events); s_events = nullptr;
    }
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

    uint32_t simMs = (uint32_t)((uint64_t)(millis() - s_startMs) * (uint32_t)(s_speed * 100) / 100);

    while (s_eventIdx < s_eventCount &&
           s_events[s_eventIdx].ts_ms <= simMs) {
        dispatchEvent(s_events[s_eventIdx]);
        s_eventIdx++;
    }
}
