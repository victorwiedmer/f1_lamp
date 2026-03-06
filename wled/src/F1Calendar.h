#pragma once
/*
 * F1Calendar.h  –  Fetches the F1 race calendar and computes a
 *                  race-week idle-brightness ramp.
 *
 * Source : Jolpica API (Ergast-compatible) at api.jolpi.ca
 *
 * Ramp logic (applies only when LEDs are in the IDLE state):
 *   7 days before race  →  5 % of configured brightness  (previous Sunday)
 *   0 days (race day)   → 100 % of configured brightness
 *   All other times     →   0 % override (normal brightness used)
 */

#include <Arduino.h>

/*
 * Fetch the 2026 race calendar from the Jolpica API.
 * Must be called after WiFi STA is connected and NTP has synced.
 * Non-blocking within a task; call from setup() after WiFi connects,
 * or schedule a periodic refresh (once per day is enough).
 * Returns true if a future race was found.
 */
bool f1cal_update();

/*
 * Returns an idle-brightness scale factor:
 *   0.0  = not in race week → caller uses normal g_cfg.brightness
 *   0.05 = 7 days out  (Sunday before race week)
 *   1.0  = race day
 */
float f1cal_idleFactor();

/* true once a successful calendar fetch has been done. */
bool f1cal_hasData();

/* Human label, e.g.  "Australian GP  in 5d" */
const char* f1cal_nextRaceLabel();

/* ISO date string of next race, e.g. "2026-03-15"  (or "" if no data) */
const char* f1cal_nextRaceDate();

/* Days until race day (0 = today, negative = past) — INT16_MIN if no data */
int f1cal_daysUntilRace();

/*
 * Deep-sleep helpers.
 *
 * weekendActive – true if now falls inside the race weekend window:
 *   (FP1/SprintQ start − 30 min)  →  (race-day midnight + 27 h)
 *   Use to decide whether to stay fully awake.
 *
 * sleepSeconds – recommended deep-sleep duration based on proximity to
 *   the next FP1/SprintQ start.  Call only when !weekendActive.
 *     > 7 days away  → 3 600 s (1 h)
 *     1 – 7 days     → 1 800 s (30 min)
 *     < 24 h         →   900 s (15 min)
 *     < 2 h          →   300 s (5 min)
 */
bool     f1cal_weekendActive();
uint32_t f1cal_sleepSeconds();
