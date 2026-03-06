/*
 * Config.cpp  –  LittleFS-backed JSON configuration
 */

#include "Config.h"
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Arduino.h>

AppConfig g_cfg;
static bool s_mounted = false;

bool cfg_isMounted() { return s_mounted; }

/* ── factory defaults ─────────────────────────────────────────────────────── */
void cfg_defaults() {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.led_count  = 22;
    g_cfg.f_count    = 17;    /* 17 LEDs = "F" letter, remaining 5 = "1" letter */
    g_cfg.brightness = 128;
    g_cfg.power      = true;
    g_cfg.feat_winner       = true;
    g_cfg.feat_fastest_lap  = true;
    g_cfg.feat_drs          = false;  /* fires frequently in practice – off by default */
    g_cfg.feat_start_lights = true;
    g_cfg.deep_sleep        = false;  /* opt-in – off by default                        */

    /* StateEffect fields: {effect, r, g, b, speed, r2, g2, b2, revert_s}
     *
     * Effect 4 (alt_letters) alternates the F-segment and 1-segment between
     * two colors.  Phase A: F=(r,g,b) / 1=(r2,g2,b2).  Phase B: swapped.
     */

    /* 0  F1ST_IDLE          – dim red, solid (race running / no session) */
    g_cfg.states[0] = { 0,  80,   0,   0,  50,   0,   0,   0,  0 };
    /* 1  F1ST_SESSION_START – formation lap: F green↔yellow, 1 yellow↔green */
    g_cfg.states[1] = { 4,   0, 180,   0,  70, 180, 150,   0,  0 };
    /* 2  F1ST_GREEN         – green strobe, auto-reverts to IDLE after 10 s */
    g_cfg.states[2] = { 3,   0, 200,   0, 180,   0,   0,   0, 10 };
    /* 3  F1ST_YELLOW        – yellow pulse */
    g_cfg.states[3] = { 1, 200, 180,   0,  80,   0,   0,   0,  0 };
    /* 4  F1ST_SAFETY_CAR   – alternating yellow: F=yellow / 1=off, swapping */
    g_cfg.states[4] = { 4, 255, 200,   0,  80,   0,   0,   0,  0 };
    /* 5  F1ST_VIRTUAL_SC   – orange pulse (slower than SC) */
    g_cfg.states[5] = { 1, 255, 100,   0,  60,   0,   0,   0,  0 };
    /* 6  F1ST_RED_FLAG     – fast red strobe */
    g_cfg.states[6] = { 3, 220,   0,   0, 180,   0,   0,   0,  0 };
    /* 7  F1ST_CHEQUERED    – F white / 1 off, alternating */
    g_cfg.states[7] = { 4, 255, 255, 255, 140,   0,   0,   0,  0 };
    /* 8  F1ST_VSC_ENDING   – fast yellow alt_letters: "prepare to race" */
    g_cfg.states[8] = { 4, 255, 200,   0, 160,   0,   0,   0,  0 };
    /* 9  F1ST_SC_ENDING    – fast yellow alt_letters: "SC coming in" (force only, no live feed code) */
    g_cfg.states[9] = { 4, 255, 200,   0, 160,   0,   0,   0,  0 };
}

/* ── init ─────────────────────────────────────────────────────────────────── */
void cfg_init() {
    cfg_defaults();
    if (!LittleFS.begin(true)) {          /* true = format on failure */
        Serial.println("[Config] LittleFS mount failed – using defaults");
        return;
    }
    s_mounted = true;
    cfg_load();
}

/* ── load ─────────────────────────────────────────────────────────────────── */
void cfg_load() {
    cfg_defaults();
    if (!s_mounted || !LittleFS.exists("/config.json")) {
        Serial.println("[Config] No saved config – using defaults");
        return;
    }

    File f = LittleFS.open("/config.json", "r");
    if (!f) { return; }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[Config] JSON error: %s – using defaults\n", err.c_str());
        return;
    }

    strlcpy(g_cfg.ssid, doc["ssid"] | "",  sizeof(g_cfg.ssid));
    strlcpy(g_cfg.pass, doc["pass"] | "",  sizeof(g_cfg.pass));
    g_cfg.led_count  = doc["led_count"]  | 22;
    g_cfg.f_count    = doc["f_count"]    | 17;
    g_cfg.brightness = doc["brightness"] | 128;
    g_cfg.power             = doc["power"]             | true;
    g_cfg.feat_winner       = doc["feat_winner"]       | true;
    g_cfg.feat_fastest_lap  = doc["feat_fastest_lap"]  | true;
    g_cfg.feat_drs          = doc["feat_drs"]          | false;
    g_cfg.feat_start_lights = doc["feat_start_lights"] | true;
    g_cfg.deep_sleep        = doc["deep_sleep"]        | false;

    JsonArray arr = doc["states"].as<JsonArray>();
    for (int i = 0; i < CFG_NUM_STATES && i < (int)arr.size(); i++) {
        JsonObject s     = arr[i];
        g_cfg.states[i].effect   = s["effect"]   | (uint8_t)0;
        g_cfg.states[i].r        = s["r"]        | (uint8_t)0;
        g_cfg.states[i].g        = s["g"]        | (uint8_t)0;
        g_cfg.states[i].b        = s["b"]        | (uint8_t)0;
        g_cfg.states[i].speed    = s["speed"]    | (uint8_t)50;
        g_cfg.states[i].r2       = s["r2"]       | (uint8_t)0;
        g_cfg.states[i].g2       = s["g2"]       | (uint8_t)0;
        g_cfg.states[i].b2       = s["b2"]       | (uint8_t)0;
        g_cfg.states[i].revert_s = s["revert_s"] | (uint8_t)0;
    }
    Serial.printf("[Config] Loaded SSID=%s LEDs=%d Bri=%d\n",
                  g_cfg.ssid, g_cfg.led_count, g_cfg.brightness);
}

/* ── save ─────────────────────────────────────────────────────────────────── */
void cfg_save() {
    if (!s_mounted) {
        Serial.println("[Config] FS not mounted – cannot save");
        return;
    }

    JsonDocument doc;
    doc["ssid"]       = g_cfg.ssid;
    doc["pass"]       = g_cfg.pass;
    doc["led_count"]  = g_cfg.led_count;
    doc["f_count"]    = g_cfg.f_count;
    doc["brightness"] = g_cfg.brightness;
    doc["power"]             = g_cfg.power;
    doc["feat_winner"]       = g_cfg.feat_winner;
    doc["feat_fastest_lap"]  = g_cfg.feat_fastest_lap;
    doc["feat_drs"]          = g_cfg.feat_drs;
    doc["feat_start_lights"] = g_cfg.feat_start_lights;
    doc["deep_sleep"]        = g_cfg.deep_sleep;

    JsonArray arr = doc["states"].to<JsonArray>();
    for (int i = 0; i < CFG_NUM_STATES; i++) {
        JsonObject s = arr.add<JsonObject>();
        s["effect"]   = g_cfg.states[i].effect;
        s["r"]        = g_cfg.states[i].r;
        s["g"]        = g_cfg.states[i].g;
        s["b"]        = g_cfg.states[i].b;
        s["speed"]    = g_cfg.states[i].speed;
        s["r2"]       = g_cfg.states[i].r2;
        s["g2"]       = g_cfg.states[i].g2;
        s["b2"]       = g_cfg.states[i].b2;
        s["revert_s"] = g_cfg.states[i].revert_s;
    }

    File f = LittleFS.open("/config.json", "w");
    if (!f) {
        Serial.println("[Config] Cannot open for writing");
        return;
    }
    serializeJson(doc, f);
    f.close();
    Serial.println("[Config] Saved");
}
