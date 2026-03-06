/*
 * F1Calendar.cpp  –  Race calendar (static 2026) + race-week brightness ramp
 *
 * NOTE: This build strips TLS/mbedtls, so HTTPS fetches are not possible.
 * The 2026 F1 race calendar is hard-coded below.  Update each season or
 * when schedule changes are announced.
 */

#include "F1Calendar.h"
#include <Arduino.h>
#include <time.h>

/* ── 2026 F1 static calendar  ────────────────────────────────────────────── */
/* Dates are UTC race day.  firstSessDate is FP1 for standard weekends,
   Sprint Qualifying session date for sprint weekends.
   Update sprint flag if the sprint format weekend differs.             */
struct CalEntry {
    const char* name;
    const char* raceDate;     /* "YYYY-MM-DD"                              */
    const char* raceTime;     /* "HH:MM:SSZ"  UTC start                   */
    const char* firstSessDate;
    const char* firstSessTime;
};

/* Source: Jolpica/Ergast API https://api.jolpi.ca/ergast/f1/2026.json
   firstSessDate = SprintQualifying date for sprint weekends, else FP1 date.
   Sprint weekends 2026: Chinese, Miami, Canadian, British, Dutch, Singapore. */
static const CalEntry s_cal2026[] = {
    { "Australian",  "2026-03-08", "04:00:00Z", "2026-03-06", "01:30:00Z" }, /* FP1 */
    { "Chinese",     "2026-03-15", "07:00:00Z", "2026-03-13", "07:30:00Z" }, /* SprintQ */
    { "Japanese",    "2026-03-29", "05:00:00Z", "2026-03-27", "02:30:00Z" }, /* FP1 */
    { "Bahrain",     "2026-04-12", "15:00:00Z", "2026-04-10", "11:30:00Z" }, /* FP1 */
    { "Saudi",       "2026-04-19", "17:00:00Z", "2026-04-17", "13:30:00Z" }, /* FP1 */
    { "Miami",       "2026-05-03", "20:00:00Z", "2026-05-01", "20:30:00Z" }, /* SprintQ */
    { "Canadian",    "2026-05-24", "20:00:00Z", "2026-05-22", "20:30:00Z" }, /* SprintQ */
    { "Monaco",      "2026-06-07", "13:00:00Z", "2026-06-05", "11:30:00Z" }, /* FP1 */
    { "Barcelona",   "2026-06-14", "13:00:00Z", "2026-06-12", "11:30:00Z" }, /* FP1 */
    { "Austrian",    "2026-06-28", "13:00:00Z", "2026-06-26", "11:30:00Z" }, /* FP1 */
    { "British",     "2026-07-05", "14:00:00Z", "2026-07-03", "15:30:00Z" }, /* SprintQ */
    { "Belgian",     "2026-07-19", "13:00:00Z", "2026-07-17", "11:30:00Z" }, /* FP1 */
    { "Hungarian",   "2026-07-26", "13:00:00Z", "2026-07-24", "11:30:00Z" }, /* FP1 */
    { "Dutch",       "2026-08-23", "13:00:00Z", "2026-08-21", "14:30:00Z" }, /* SprintQ */
    { "Italian",     "2026-09-06", "13:00:00Z", "2026-09-04", "10:30:00Z" }, /* FP1 */
    { "Spanish",     "2026-09-13", "13:00:00Z", "2026-09-11", "11:30:00Z" }, /* FP1 (Madrid) */
    { "Azerbaijan",  "2026-09-26", "11:00:00Z", "2026-09-24", "08:30:00Z" }, /* FP1 */
    { "Singapore",   "2026-10-11", "12:00:00Z", "2026-10-09", "12:30:00Z" }, /* SprintQ */
    { "US",          "2026-10-25", "20:00:00Z", "2026-10-23", "17:30:00Z" }, /* FP1 */
    { "Mexico City", "2026-11-01", "20:00:00Z", "2026-10-30", "18:30:00Z" }, /* FP1 */
    { "São Paulo",   "2026-11-08", "17:00:00Z", "2026-11-06", "15:30:00Z" }, /* FP1 */
    { "Las Vegas",   "2026-11-22", "04:00:00Z", "2026-11-20", "00:30:00Z" }, /* FP1 */
    { "Qatar",       "2026-11-29", "16:00:00Z", "2026-11-27", "13:30:00Z" }, /* FP1 */
    { "Abu Dhabi",   "2026-12-06", "13:00:00Z", "2026-12-04", "09:30:00Z" }, /* FP1 */
};

/* ── module state ────────────────────────────────────────────────────────── */
static bool   s_hasData       = false;
static time_t s_raceEpoch     = 0;     /* race day  00:00 UTC                */
static time_t s_firstSessEpoch= 0;     /* FP1 / first on-track session UTC   */
static char   s_raceName[48]  = {};
static char   s_raceDate[11]  = {};    /* "YYYY-MM-DD"                       */

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* Parse "YYYY-MM-DD" + "HH:MM:SSZ" into a UTC epoch.
   If timeStr is nullptr the time part defaults to 00:00:00. */
static time_t parseUtc(const char* dateStr, const char* timeStr) {
    if (!dateStr || !dateStr[0]) return 0;
    struct tm t = {};
    int year, mon, day;
    if (sscanf(dateStr, "%d-%d-%d", &year, &mon, &day) != 3) return 0;
    t.tm_year = year - 1900;
    t.tm_mon  = mon  - 1;
    t.tm_mday = day;
    if (timeStr && timeStr[0]) {
        int h, m, s;
        sscanf(timeStr, "%d:%d:%d", &h, &m, &s);
        t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    }
    t.tm_isdst = -1;
    /* NTP sets TZ=UTC (configTime offset=0,0), so mktime treats as UTC */
    return mktime(&t);
}

/* Shorten a long race name to ≤ 24 chars for the label. */
static void shortName(const char* src, char* dst, size_t dstLen) {
    strlcpy(dst, src, dstLen);
    /* Remove common suffix words to fit in the badge */
    static const char* strips[] = {
        " Grand Prix", " GP", nullptr
    };
    for (int i = 0; strips[i]; i++) {
        char* p = strstr(dst, strips[i]);
        if (p) { *p = '\0'; break; }
    }
}

/* ── public ──────────────────────────────────────────────────────────────── */

bool f1cal_update() {
    /* NTP must be synced (epoch > year 2020) */
    time_t now = time(nullptr);
    if (now < 1577836800UL) {
        Serial.println("[F1Cal] NTP not synced – skipping calendar scan");
        return false;
    }

    int n = (int)(sizeof(s_cal2026) / sizeof(s_cal2026[0]));
    for (int i = 0; i < n; i++) {
        const CalEntry& e = s_cal2026[i];
        time_t raceEpoch = parseUtc(e.raceDate, e.raceTime);
        if (raceEpoch == 0) continue;

        /* Skip races that finished more than 6 h ago */
        if (raceEpoch + 6 * 3600 < now) continue;

        /* Found next/current race */
        strlcpy(s_raceName, e.name, sizeof(s_raceName));
        strlcpy(s_raceDate, e.raceDate, sizeof(s_raceDate));

        /* Race-day midnight UTC for ramp comparison */
        struct tm raceDayTm;
        gmtime_r(&raceEpoch, &raceDayTm);
        raceDayTm.tm_hour = 0; raceDayTm.tm_min = 0; raceDayTm.tm_sec = 0;
        s_raceEpoch = mktime(&raceDayTm);

        s_firstSessEpoch = parseUtc(e.firstSessDate, e.firstSessTime);
        s_hasData = true;

        Serial.printf("[F1Cal] Next: %s on %s  firstSess=%lu\n",
                      s_raceName, s_raceDate, (unsigned long)s_firstSessEpoch);
        return true;
    }

    Serial.println("[F1Cal] No upcoming race in static 2026 calendar");
    return false;
}

float f1cal_idleFactor() {
    if (!s_hasData) return 0.0f;

    time_t now = time(nullptr);

    /* Use first session time as the "100 % brightness" target if available,
       otherwise race day midnight */
    time_t target = (s_firstSessEpoch > 0) ? s_firstSessEpoch : s_raceEpoch;

    double diffSec  = difftime(target, now);
    double diffDays = diffSec / 86400.0;

    /* After first session start → race is live, normal brightness rules apply */
    if (diffDays < 0.0) return 0.0f;

    /* More than 7 days away → no ramp yet */
    if (diffDays > 7.0) return 0.0f;

    /* Linear: 7 days out = 5 %, 0 days = 100 % */
    float factor = 1.0f - (float)(diffDays / 7.0) * 0.95f;
    if (factor < 0.05f) factor = 0.05f;
    if (factor > 1.0f)  factor = 1.0f;
    return factor;
}

bool f1cal_hasData() { return s_hasData; }

bool f1cal_weekendActive() {
    if (!s_hasData) return false;
    time_t now = time(nullptr);
    /* Allow 30 min before FP1; end of window = race day midnight + 27 h
       (covers any race regardless of local start time + ~3 h duration). */
    time_t winStart = s_firstSessEpoch - 1800;
    time_t winEnd   = s_raceEpoch + 97200;   /* 27 h after race-day midnight */
    return (now >= winStart && now < winEnd);
}

uint32_t f1cal_sleepSeconds() {
    if (!s_hasData) return 3600;
    time_t now = time(nullptr);
    double secsToWake = difftime(s_firstSessEpoch - 1800, now);
    if (secsToWake < 7200.0)   return  300;   /* < 2 h  → wake every 5 min  */
    if (secsToWake < 86400.0)  return  900;   /* < 24 h → every 15 min      */
    if (secsToWake < 604800.0) return 1800;   /* < 7 d  → every 30 min      */
    return 3600;                              /* > 7d   → every 1 h         */
}

const char* f1cal_nextRaceDate() { return s_raceDate; }

int f1cal_daysUntilRace() {
    if (!s_hasData) return INT16_MIN;
    time_t now = time(nullptr);
    double diffSec = difftime(s_raceEpoch, now);
    return (int)(diffSec / 86400.0);
}

const char* f1cal_nextRaceLabel() {
    static char label[64];
    if (!s_hasData) { strlcpy(label, "—", sizeof(label)); return label; }

    char short_name[28];
    shortName(s_raceName, short_name, sizeof(short_name));

    int d = f1cal_daysUntilRace();
    if (d < 0)       snprintf(label, sizeof(label), "%s  (this week)", short_name);
    else if (d == 0) snprintf(label, sizeof(label), "%s  TODAY",       short_name);
    else             snprintf(label, sizeof(label), "%s  in %dd",      short_name, d);
    return label;
}
