/*
 * F1Calendar.cpp  –  Race calendar + race-week brightness ramp
 *
 * On startup, fetches the current season calendar from the Jolpica API
 * (Ergast-compatible) via HTTPS.  Falls back to a custom calendar from
 * LittleFS, then to a built-in 2026 season table.
 *
 * The next race weekend is stored with ALL sessions and UTC times,
 * served to the browser via /api/nextrace.  JavaScript converts to
 * the user's local timezone.
 */

#include "F1Calendar.h"
#include <Arduino.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include "F1Sessions.h"   /* for f1sessions_httpsGet() */

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
static bool   s_customLoaded  = false; /* true if loaded from LittleFS       */

/* ── Next-race session schedule (from API or fallback) ───────────────────── */
#define MAX_SESSIONS 7
struct NextRaceSession {
    char name[20];          /* "FP1", "Qualifying", "Race", etc.          */
    char dateTime[22];      /* "YYYY-MM-DDTHH:MM:SSZ"  (ISO 8601, UTC)   */
};
static struct {
    char raceName[64];
    char circuitName[64];
    char locality[32];
    char country[32];
    int  round;
    NextRaceSession sessions[MAX_SESSIONS];
    int sessionCount;
    bool fromApi;           /* true = fetched from Jolpica API            */
} s_nextRace = {};

static String s_nextRaceJson;          /* cached JSON for /api/nextrace   */

/* ── API fetch state ─────────────────────────────────────────────────────── */
static volatile bool s_apiFetchRequested = false;
static volatile bool s_apiFetching       = false;
static bool          s_apiFetched        = false;
static String        s_apiError;

/* ── dynamic calendar storage (from LittleFS JSON) ──────────────────────── */
#define MAX_DYN_RACES 30
static struct DynEntry {
    char name[40];
    char raceDate[11];        /* "YYYY-MM-DD"  */
    char raceTime[10];        /* "HH:MM:SSZ"   */
    char firstSessDate[11];
    char firstSessTime[10];
} s_dynCal[MAX_DYN_RACES];
static int s_dynCount = 0;

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



/* ── Build the cached /api/nextrace JSON from s_nextRace ─────────────────── */
static void buildNextRaceJson() {
    JsonDocument doc;
    doc["raceName"]    = s_nextRace.raceName;
    doc["circuit"]     = s_nextRace.circuitName;
    doc["locality"]    = s_nextRace.locality;
    doc["country"]     = s_nextRace.country;
    doc["round"]       = s_nextRace.round;
    doc["fromApi"]     = s_nextRace.fromApi;
    doc["daysUntil"]   = f1cal_daysUntilRace();

    JsonArray sess = doc["sessions"].to<JsonArray>();
    for (int i = 0; i < s_nextRace.sessionCount; i++) {
        JsonObject s = sess.add<JsonObject>();
        s["name"]     = s_nextRace.sessions[i].name;
        s["dateTime"] = s_nextRace.sessions[i].dateTime;
    }
    s_nextRaceJson = String();
    serializeJson(doc, s_nextRaceJson);
}

/* ── public ──────────────────────────────────────────────────────────────── */

/*
 * Try to load a custom calendar from LittleFS.
 * Expected JSON: {"races":[ {"name":"...", "raceDate":"YYYY-MM-DD",
 *   "raceTime":"HH:MM:SSZ", "firstSessDate":"YYYY-MM-DD",
 *   "firstSessTime":"HH:MM:SSZ"}, ... ]}
 * Returns number of races loaded, or 0 on failure.
 */
static int loadCustomCalendar() {
    if (!LittleFS.exists("/calendar_custom.json")) return 0;

    File f = LittleFS.open("/calendar_custom.json", "r");
    if (!f) return 0;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[F1Cal] Custom calendar parse error: %s\n", err.c_str());
        return 0;
    }

    JsonArray races = doc["races"];
    if (races.isNull()) return 0;

    s_dynCount = 0;
    for (JsonObject race : races) {
        if (s_dynCount >= MAX_DYN_RACES) break;
        DynEntry& d = s_dynCal[s_dynCount];
        strlcpy(d.name,          race["name"] | "Race",       sizeof(d.name));
        strlcpy(d.raceDate,      race["raceDate"] | "",       sizeof(d.raceDate));
        strlcpy(d.raceTime,      race["raceTime"] | "13:00:00Z", sizeof(d.raceTime));
        strlcpy(d.firstSessDate, race["firstSessDate"] | "",  sizeof(d.firstSessDate));
        strlcpy(d.firstSessTime, race["firstSessTime"] | "11:30:00Z", sizeof(d.firstSessTime));
        /* If firstSessDate is empty, default to raceDate - 2 days */
        if (d.firstSessDate[0] == '\0') {
            strlcpy(d.firstSessDate, d.raceDate, sizeof(d.firstSessDate));
        }
        s_dynCount++;
    }
    Serial.printf("[F1Cal] Loaded %d races from custom calendar\n", s_dynCount);
    return s_dynCount;
}

/* Helper: scan a list of entries for the next upcoming race */
static bool scanEntries(
    const char* const* names,     /* array of name pointers  */
    const char* const* raceDates, /* array of raceDate ptrs  */
    const char* const* raceTimes, /* array of raceTime ptrs  */
    const char* const* fssDates,  /* firstSessDate ptrs      */
    const char* const* fssTimes,  /* firstSessTime ptrs      */
    int count, time_t now)
{
    for (int i = 0; i < count; i++) {
        time_t raceEpoch = parseUtc(raceDates[i], raceTimes[i]);
        if (raceEpoch == 0) continue;
        if (raceEpoch + 6 * 3600 < now) continue;

        strlcpy(s_raceName, names[i], sizeof(s_raceName));
        strlcpy(s_raceDate, raceDates[i], sizeof(s_raceDate));

        struct tm raceDayTm;
        gmtime_r(&raceEpoch, &raceDayTm);
        raceDayTm.tm_hour = 0; raceDayTm.tm_min = 0; raceDayTm.tm_sec = 0;
        s_raceEpoch = mktime(&raceDayTm);

        s_firstSessEpoch = parseUtc(fssDates[i], fssTimes[i]);
        s_hasData = true;

        Serial.printf("[F1Cal] Next: %s on %s  firstSess=%lu\n",
                      s_raceName, s_raceDate, (unsigned long)s_firstSessEpoch);
        return true;
    }
    return false;
}

bool f1cal_update() {
    /* NTP must be synced (epoch > year 2020) */
    time_t now = time(nullptr);
    if (now < 1577836800UL) {
        Serial.println("[F1Cal] NTP not synced – skipping calendar scan");
        return false;
    }

    /* Try custom calendar from LittleFS first */
    s_customLoaded = false;
    if (loadCustomCalendar() > 0) {
        /* Build pointer arrays from dynamic storage */
        const char* names[MAX_DYN_RACES];
        const char* rDates[MAX_DYN_RACES];
        const char* rTimes[MAX_DYN_RACES];
        const char* fDates[MAX_DYN_RACES];
        const char* fTimes[MAX_DYN_RACES];
        for (int i = 0; i < s_dynCount; i++) {
            names[i]  = s_dynCal[i].name;
            rDates[i] = s_dynCal[i].raceDate;
            rTimes[i] = s_dynCal[i].raceTime;
            fDates[i] = s_dynCal[i].firstSessDate;
            fTimes[i] = s_dynCal[i].firstSessTime;
        }
        if (scanEntries(names, rDates, rTimes, fDates, fTimes, s_dynCount, now)) {
            s_customLoaded = true;
            return true;
        }
        Serial.println("[F1Cal] Custom calendar has no upcoming races, trying built-in");
    }

    /* Fall back to built-in 2026 calendar */
    int n = (int)(sizeof(s_cal2026) / sizeof(s_cal2026[0]));
    const char* names[24];
    const char* rDates[24];
    const char* rTimes[24];
    const char* fDates[24];
    const char* fTimes[24];
    for (int i = 0; i < n && i < 24; i++) {
        names[i]  = s_cal2026[i].name;
        rDates[i] = s_cal2026[i].raceDate;
        rTimes[i] = s_cal2026[i].raceTime;
        fDates[i] = s_cal2026[i].firstSessDate;
        fTimes[i] = s_cal2026[i].firstSessTime;
    }
    if (scanEntries(names, rDates, rTimes, fDates, fTimes, n, now)) {
        return true;
    }

    Serial.println("[F1Cal] No upcoming race in any calendar");
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
    if (secsToWake < 7200.0)    return   300;   /* < 2 h   → wake every 5 min   */
    if (secsToWake < 86400.0)   return   900;   /* < 24 h  → every 15 min       */
    if (secsToWake < 604800.0)  return  1800;   /* < 7 d   → every 30 min       */
    if (secsToWake < 1209600.0) return  3600;   /* < 14 d  → every 1 h          */
    return 10800;                               /* > 14 d  → every 3 h          */
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

/* ── custom calendar management ──────────────────────────────────────────── */

bool f1cal_hasCustomCalendar() {
    return LittleFS.exists("/calendar_custom.json");
}

bool f1cal_deleteCustomCalendar() {
    if (LittleFS.exists("/calendar_custom.json")) {
        LittleFS.remove("/calendar_custom.json");
        s_customLoaded = false;
        s_dynCount = 0;
        Serial.println("[F1Cal] Custom calendar deleted");
        return true;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════════════
   Online calendar fetch – uses f1calendar.com GitHub data
   (raw.githubusercontent.com/sportstimes/f1/main/_db/f1/{year}.json)
   Contains all races with full session schedule in UTC.
   ══════════════════════════════════════════════════════════════════════════ */

/* CDN mirrors for the f1calendar data – try in order until one works */
struct CalMirror {
    const char* host;
    const char* pathFmt;   /* printf format with %d for year */
};
static const CalMirror CAL_MIRRORS[] = {
    { "cdn.jsdelivr.net",            "/gh/sportstimes/f1@main/_db/f1/%d.json" },
    { "raw.githubusercontent.com",   "/sportstimes/f1/main/_db/f1/%d.json" },
};
static constexpr int NUM_MIRRORS = sizeof(CAL_MIRRORS) / sizeof(CAL_MIRRORS[0]);

void f1cal_requestApiFetch() {
    if (s_apiFetching || s_apiFetchRequested) return;
    s_apiFetchRequested = true;
    Serial.println("[F1Cal] API fetch requested");
}

bool f1cal_apiFetchRequested() { return s_apiFetchRequested; }
bool f1cal_isApiFetching()     { return s_apiFetching; }
bool f1cal_apiFetched()        { return s_apiFetched; }

/* Helper to add a session entry.  dateTime is already ISO 8601 UTC with Z. */
static void addSession(const char* name, const char* dateTime) {
    if (!dateTime || !dateTime[0]) return;
    if (s_nextRace.sessionCount >= MAX_SESSIONS) return;
    NextRaceSession& s = s_nextRace.sessions[s_nextRace.sessionCount];
    strlcpy(s.name, name, sizeof(s.name));
    strlcpy(s.dateTime, dateTime, sizeof(s.dateTime));
    s_nextRace.sessionCount++;
}

bool f1cal_fetchApi() {
    s_apiFetchRequested = false;
    s_apiFetching = true;
    s_apiError = "";

    /* Determine current year from NTP */
    time_t now = time(nullptr);
    struct tm tmNow;
    gmtime_r(&now, &tmNow);
    int year = tmNow.tm_year + 1900;

    Serial.printf("[F1Cal] Fetching f1calendar %d  heap=%u\n",
                  year, ESP.getFreeHeap());

    /* Try each CDN mirror in order */
    String body;
    for (int m = 0; m < NUM_MIRRORS; m++) {
        char path[96];
        snprintf(path, sizeof(path), CAL_MIRRORS[m].pathFmt, year);
        Serial.printf("[F1Cal] Trying %s%s\n", CAL_MIRRORS[m].host, path);
        s_apiError = "";
        body = f1sessions_httpsGet(path, s_apiError, CAL_MIRRORS[m].host);
        if (body.length() > 0) break;
        Serial.printf("[F1Cal] Mirror %d failed: %s\n", m, s_apiError.c_str());
    }
    if (body.length() == 0) {
        Serial.println("[F1Cal] Fetch failed: " + s_apiError);
        s_apiFetching = false;
        return false;
    }

    Serial.printf("[F1Cal] f1calendar body: %u bytes  heap=%u\n",
                  body.length(), ESP.getFreeHeap());

    /* Parse JSON */
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    body = String(); /* free */

    if (err) {
        s_apiError = "JSON parse: " + String(err.c_str());
        Serial.println("[F1Cal] " + s_apiError);
        s_apiFetching = false;
        return false;
    }

    JsonArray races = doc["races"];
    if (races.isNull() || races.size() == 0) {
        s_apiError = "No races in f1calendar JSON";
        s_apiFetching = false;
        return false;
    }

    /* Session key → friendly label mapping */
    struct { const char* key; const char* label; } sessMap[] = {
        {"fp1",              "FP1"},
        {"fp2",              "FP2"},
        {"fp3",              "FP3"},
        {"sprintQualifying", "Sprint Qual"},
        {"sprint",           "Sprint"},
        {"qualifying",       "Qualifying"},
        {"gp",               "Race"},
        {nullptr,             nullptr}
    };

    /* Find the next race whose GP hasn't finished */
    bool found = false;
    for (JsonObject race : races) {
        JsonObject sess = race["sessions"];
        if (sess.isNull()) continue;

        const char* gpTime = sess["gp"] | "";
        if (!gpTime[0]) continue;

        /* Parse ISO 8601 date "YYYY-MM-DDTHH:MM:SSZ" */
        char rDate[11] = {}, rTime[10] = {};
        if (strlen(gpTime) >= 20) {
            memcpy(rDate, gpTime, 10);
            memcpy(rTime, gpTime + 11, 9); /* includes 'Z' */
        }
        time_t gpEpoch = parseUtc(rDate, rTime);
        if (gpEpoch == 0) continue;
        if (gpEpoch + 6 * 3600 < now) continue; /* finished > 6h ago */

        /* Found the next race! */
        found = true;
        const char* rName = race["name"] | "Unknown";
        const char* rLoc  = race["location"] | "";
        int rRound        = race["round"] | 0;

        snprintf(s_nextRace.raceName, sizeof(s_nextRace.raceName),
                 "%s Grand Prix", rName);
        strlcpy(s_nextRace.circuitName, rLoc, sizeof(s_nextRace.circuitName));
        strlcpy(s_nextRace.locality,    rLoc, sizeof(s_nextRace.locality));
        s_nextRace.country[0] = '\0'; /* not in this API */
        s_nextRace.round = rRound;
        s_nextRace.sessionCount = 0;
        s_nextRace.fromApi = true;

        /* Add all sessions present */
        for (int i = 0; sessMap[i].key; i++) {
            const char* dt = sess[sessMap[i].key] | "";
            if (dt[0]) addSession(sessMap[i].label, dt);
        }

        /* Sort sessions by dateTime (bubble sort, max 7 items) */
        for (int i = 0; i < s_nextRace.sessionCount - 1; i++) {
            for (int j = 0; j < s_nextRace.sessionCount - 1 - i; j++) {
                if (strcmp(s_nextRace.sessions[j].dateTime,
                           s_nextRace.sessions[j+1].dateTime) > 0) {
                    NextRaceSession tmp = s_nextRace.sessions[j];
                    s_nextRace.sessions[j] = s_nextRace.sessions[j+1];
                    s_nextRace.sessions[j+1] = tmp;
                }
            }
        }

        /* Update main calendar state for ramp/sleep logic */
        strlcpy(s_raceName, s_nextRace.raceName, sizeof(s_raceName));
        strlcpy(s_raceDate, rDate, sizeof(s_raceDate));

        struct tm raceDayTm;
        gmtime_r(&gpEpoch, &raceDayTm);
        raceDayTm.tm_hour = 0; raceDayTm.tm_min = 0; raceDayTm.tm_sec = 0;
        s_raceEpoch = mktime(&raceDayTm);

        /* firstSessEpoch from first session */
        if (s_nextRace.sessionCount > 0) {
            char d[11] = {}, t[10] = {};
            const char* dt = s_nextRace.sessions[0].dateTime;
            if (strlen(dt) >= 20) {
                memcpy(d, dt, 10);
                memcpy(t, dt + 11, 9);
            }
            s_firstSessEpoch = parseUtc(d, t);
        }
        s_hasData = true;

        Serial.printf("[F1Cal] Next race = %s (Rd %d), %d sessions, on %s\n",
                      s_nextRace.raceName, rRound,
                      s_nextRace.sessionCount, rDate);
        break;
    }

    if (!found) {
        s_apiError = "No upcoming race found";
        s_apiFetching = false;
        return false;
    }

    /* Build cached JSON for /api/nextrace */
    buildNextRaceJson();

    s_apiFetched = true;
    s_apiFetching = false;
    Serial.printf("[F1Cal] API fetch complete, cached %u bytes JSON\n",
                  s_nextRaceJson.length());
    return true;
}

const String& f1cal_nextRaceJson() {
    static String empty = "{}";
    return s_nextRaceJson.length() > 0 ? s_nextRaceJson : empty;
}

const String& f1cal_apiError() { return s_apiError; }
