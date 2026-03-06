#pragma once
/*
 * Config.h  –  Application configuration stored in LittleFS /config.json
 *
 * StateEffect  : visual settings for one F1 track state (matches F1NetState index)
 * AppConfig    : root config (WiFi, LED settings, per-state effects)
 *
 * Effects (StateEffect::effect):
 *   0 = solid        – fill both segments with primary color
 *   1 = pulse        – breathing sine-wave brightness modulation
 *   2 = spinner      – rotating half-strip segment
 *   3 = strobe       – fast on/off flash
 *   4 = alt_letters  – alternate F-letter and 1-letter between two colors;
 *                      segment-F gets (r,g,b) then (r2,g2,b2) alternating,
 *                      segment-1 gets the opposite phase
 *
 * revert_s: when > 0 the effect auto-reverts to F1ST_IDLE after that many
 *           seconds (used for green flag: blink then back to idle).
 */

#include <stdint.h>

#define CFG_NUM_STATES 10  /* F1ST_IDLE (0) .. F1ST_SC_ENDING (9) */

struct StateEffect {
    uint8_t effect;          /* 0=solid 1=pulse 2=spinner 3=strobe 4=alt_letters */
    uint8_t r,  g,  b;      /* primary / segment-A color                         */
    uint8_t speed;           /* 1–255                                             */
    uint8_t r2, g2, b2;     /* secondary / segment-B color (effect 4 only)       */
    uint8_t revert_s;        /* auto-revert to IDLE after N seconds (0 = never)  */
};

struct AppConfig {
    char     ssid[33];
    char     pass[65];
    uint16_t led_count;
    uint16_t f_count;         /* LEDs in the "F" letter segment (default 17)     */
    uint8_t  brightness;      /* global FastLED brightness 1–255                 */
    bool     power;           /* LED output on/off (persisted)                   */

    /* ── Live event feature flags (persisted, UI-togglable) ────────────── */
    bool     feat_winner;        /* rainbow spin when chequered flag fires         */
    bool     feat_fastest_lap;   /* purple flash on fastest lap                    */
    bool     feat_drs;           /* brief white pulse when DRS zones open          */
    bool     feat_start_lights;  /* 5-red-lights countdown on session start        */
    bool     deep_sleep;         /* enter deep sleep between sessions              */

    StateEffect states[CFG_NUM_STATES];
};

extern AppConfig g_cfg;

void cfg_init();        /* mount LittleFS, load config (or create defaults) */
void cfg_save();        /* persist g_cfg to /config.json                    */
void cfg_load();        /* (re)load from /config.json into g_cfg            */
void cfg_defaults();    /* fill g_cfg with factory defaults                 */
bool cfg_isMounted();   /* true when LittleFS is up                         */
