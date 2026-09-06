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
#include "F1TimeUtils.h"   /* f1_parseUtc, f1_weekendWindowActive, f1_idleBrightnessFactor */
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
static bool          s_nextRaceFallbackBuilt = false;  /* built-in fallback done */
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

/* Forward declaration (defined later in this file). */
static void buildFallbackNextRace(const char* name,
                                  const char* raceDate, const char* raceTime,
                                  const char* fssDate,  const char* fssTime);

/* ── helpers ─────────────────────────────────────────────────────────────── */

/* parseUtc is now provided by F1TimeUtils.h as f1_parseUtc.  Thin wrapper
   keeps existing call sites unchanged. */
static inline time_t parseUtc(const char* dateStr, const char* timeStr)
{
    return f1_parseUtc(dateStr, timeStr);
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

    String body = f.readString();
    f.close();

    /* Sanitize: remove ALL control characters (0-31).  Raw CR/LF/TAB
       inside JSON string literals are invalid JSON and would make
       deserializeJson() fail (or leak into string values). */
    String sanitized;
    sanitized.reserve(body.length());
    for (int i = 0; i < (int)body.length(); i++) {
        unsigned char c = (unsigned char)body[i];
        if (c >= 32) sanitized.concat((char)c);
    }
    body = sanitized;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
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

/* Helper: scan a list of entries for the next upcoming race.
   Returns the matching index, or -1 when none qualifies. */
static int scanEntries(
    const char* const* names,     /* array of name pointers  */
    const char* const* raceDates, /* array of raceDate ptrs  */
    const char* const* raceTimes, /* array of raceTime ptrs  */
    const char* const* fssDates,  /* firstSessDate ptrs      */
    const char* const* fssTimes,  /* firstSessTime ptrs      */
    int count, time_t now)
{
    long nw = (long)now;
    for (int i = 0; i < count; i++) {
        long raceEpoch = (long)parseUtc(raceDates[i], raceTimes[i]);
        if (raceEpoch == 0) continue;
        if (raceEpoch + 21600L < nw) continue;   /* finished > 6 h ago */

        strlcpy(s_raceName, names[i], sizeof(s_raceName));
        strlcpy(s_raceDate, raceDates[i], sizeof(s_raceDate));

        /* UTC midnight of the race day — pure 32-bit arithmetic (the
           hybrid newlib/picolibc toolchain corrupts time_t maths). */
        s_raceEpoch = parseUtc(raceDates[i], "00:00:00");

        s_firstSessEpoch = parseUtc(fssDates[i], fssTimes[i]);
        s_hasData = true;

        Serial.printf("[F1Cal] Next: %s on %s  firstSess=%lu\n",
                      s_raceName, s_raceDate, (unsigned long)s_firstSessEpoch);
        return i;
    }
    return -1;
}

bool f1cal_update() {
    /* NTP must be synced (epoch > year 2020).  f1_clock() masks the 64-bit
       time_t upper word that the hybrid libc leaves full of garbage. */
    long now = (long)f1_clock();
    if (now < 1577836800L) {
        Serial.println("[F1Cal] NTP not synced – skipping calendar scan");
        return false;
    }

    int idx = -1;

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
        idx = scanEntries(names, rDates, rTimes, fDates, fTimes, s_dynCount, (time_t)now);
        if (idx >= 0) {
            s_customLoaded = true;
            buildFallbackNextRace(names[idx], rDates[idx], rTimes[idx],
                                  fDates[idx], fTimes[idx]);
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
    idx = scanEntries(names, rDates, rTimes, fDates, fTimes, n, (time_t)now);
    if (idx >= 0) {
        buildFallbackNextRace(names[idx], rDates[idx], rTimes[idx],
                              fDates[idx], fTimes[idx]);
        return true;
    }

    Serial.println("[F1Cal] No upcoming race in any calendar");
    return false;
}

float f1cal_idleFactor() {
    if (!s_hasData) return 0.0f;
    long now     = (long)f1_clock();
    long target  = (s_firstSessEpoch > 0)
                   ? (long)s_firstSessEpoch : (long)s_raceEpoch;
    return f1_idleBrightnessFactor((time_t)now, (time_t)target);
}

bool f1cal_hasData() { return s_hasData; }

bool f1cal_weekendActive() {
    if (!s_hasData) return false;
    long now = (long)f1_clock();
    return f1_weekendWindowActive((time_t)now, s_firstSessEpoch, s_raceEpoch);
}

uint32_t f1cal_sleepSeconds() {
    if (!s_hasData) return 3600;
    long now = (long)f1_clock();
    long secsToWake = (s_firstSessEpoch > 0 ? (long)s_firstSessEpoch : (long)s_raceEpoch)
                      - 1800L - now;
    if (secsToWake < 7200L)     return   300;   /* < 2 h   → wake every 5 min   */
    if (secsToWake < 86400L)    return   900;   /* < 24 h  → every 15 min       */
    if (secsToWake < 604800L)   return  1800;   /* < 7 d   → every 30 min       */
    if (secsToWake < 1209600L)  return  3600;   /* < 14 d  → every 1 h          */
    return 10800;                               /* > 14 d  → every 3 h          */
}

const char* f1cal_nextRaceDate() { return s_raceDate; }

int f1cal_daysUntilRace() {
    if (!s_hasData) return INT16_MIN;
    long now = (long)f1_clock();
    long diffSec = (long)s_raceEpoch - now;
    return (int)(diffSec / 86400L);
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

/* Format a race name into a display label, appending " Grand Prix" only
   when the source name does not already carry it (Jolpica names e.g.
   "Australian Grand Prix" must not become "... Grand Prix Grand Prix"). */
static void raceNameFor(const char* base, char* out, size_t n) {
    strlcpy(out, base, n);
    size_t len = strlen(out);
    if (len >= 5 && strcasecmp(out + len - 5, "prix") == 0) return;
    if (len >= 2 && strcasecmp(out + len - 2, "gp") == 0) return;
    if (len + 12 < n) strlcat(out, " Grand Prix", n);
}

/* Populate s_nextRace from the built-in / custom calendar so /api/nextrace
   always has something to show even when the online calendar fetch fails.
   Runs only until the API has succeeded (fromApi data wins afterwards). */
static void buildFallbackNextRace(const char* name,
                                  const char* raceDate, const char* raceTime,
                                  const char* fssDate,  const char* fssTime) {
    if (s_apiFetched) return;             /* online data already preferred */
    if (s_nextRaceFallbackBuilt) return;

    char dt[22];
    s_nextRace.sessionCount = 0;
    raceNameFor(name, s_nextRace.raceName, sizeof(s_nextRace.raceName));
    s_nextRace.round = 0;
    s_nextRace.fromApi = false;
    s_nextRace.circuitName[0] = '\0';
    s_nextRace.locality[0]    = '\0';
    s_nextRace.country[0]     = '\0';

    if (raceDate[0] && raceTime[0]) {
        snprintf(dt, sizeof(dt), "%sT%s", raceDate, raceTime);
        addSession("Race", dt);
    }
    /* Built-in entries always carry a first session (FP1 / SprintQ). */
    if (fssDate[0] && fssTime[0]) {
        snprintf(dt, sizeof(dt), "%sT%s", fssDate, fssTime);
        addSession("FP1", dt);
    }
    s_nextRaceFallbackBuilt = true;
    buildNextRaceJson();
    Serial.printf("[F1Cal] Fallback next race cached: %s (%d sessions)\n",
                  s_nextRace.raceName, s_nextRace.sessionCount);
}

bool f1cal_fetchApi() {
    s_apiFetchRequested = false;
    s_apiFetching = true;
    s_apiError = "";

    /* Determine current year from NTP (ensure synced).
       f1_clock() masks the 64-bit time_t upper word that the hybrid libc
       leaves full of garbage. */
    long now = (long)f1_clock();
    if (now < 1740000000L) { // 2025-02-19
        Serial.println("[F1Cal] NTP not synced, aborting fetch");
        s_apiFetching = false;
        return false;
    }
    int year;
    {
        unsigned mo, d, h, mi, s;
        f1_fieldsFromEpoch((time_t)now, year, mo, d, h, mi, s);
    }

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
    Serial.printf("[F1Cal] Body hex head: %02x %02x %02x %02x %02x\n",
        (unsigned char)body[0], (unsigned char)body[1], (unsigned char)body[2], (unsigned char)body[3], (unsigned char)body[4]);

    /* Sanitize: remove ALL control characters (0-31).  Raw CR/LF/TAB
       inside JSON string literals are invalid JSON and would make
       deserializeJson() fail (or leak into the session values we cache). */
    String sanitized;
    sanitized.reserve(body.length());
    for (int i = 0; i < (int)body.length(); i++) {
        unsigned char c = (unsigned char)body[i];
        if (c >= 32) sanitized.concat((char)c);
    }
    body = sanitized;

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

        /* Parse full ISO 8601.  f1calendar.com times carry local-time
           offsets (e.g. "2026-03-08T04:00:00+10:00"); f1_parseIsoUtc
           normalises to UTC so "finished > 6h ago" compares correctly. */
        long gpEpoch = (long)f1_parseIsoUtc(gpTime);
        if (gpEpoch == 0) continue;
        if (gpEpoch + 21600L < now) continue; /* finished > 6h ago */

        /* Found the next race! */
        found = true;
        const char* rName = race["name"] | "Unknown";
        const char* rLoc  = race["location"] | "";
        int rRound        = race["round"] | 0;

        raceNameFor(rName, s_nextRace.raceName, sizeof(s_nextRace.raceName));
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

        /* Sort sessions chronologically by their UTC epoch, not by string
           comparison (mixed "+HH:MM" local offsets sort wrongly). */
        for (int i = 0; i < s_nextRace.sessionCount - 1; i++) {
            for (int j = 0; j < s_nextRace.sessionCount - 1 - i; j++) {
                time_t a = f1_parseIsoUtc(s_nextRace.sessions[j].dateTime);
                time_t b = f1_parseIsoUtc(s_nextRace.sessions[j+1].dateTime);
                if (a != 0 && b != 0 && a > b) {
                    NextRaceSession tmp = s_nextRace.sessions[j];
                    s_nextRace.sessions[j] = s_nextRace.sessions[j+1];
                    s_nextRace.sessions[j+1] = tmp;
                }
            }
        }

        /* Update main calendar state for ramp/sleep logic */
        strlcpy(s_raceName, s_nextRace.raceName, sizeof(s_raceName));
        {   /* UTC date of race day + its midnight — pure arithmetic */
            int gy; unsigned gmo, gd, gh, gmi, gs;
            f1_fieldsFromEpoch((time_t)gpEpoch, gy, gmo, gd, gh, gmi, gs);
            char rDate[11];
            snprintf(rDate, sizeof(rDate), "%04d-%02u-%02u", gy, gmo, gd);
            strlcpy(s_raceDate, rDate, sizeof(s_raceDate));
            s_raceEpoch = f1_epochFromFields(gy, gmo, gd, 0, 0, 0);
        }

        /* firstSessEpoch from first (earliest) session */
        if (s_nextRace.sessionCount > 0) {
            s_firstSessEpoch =
                f1_parseIsoUtc(s_nextRace.sessions[0].dateTime);
        }
        s_hasData = true;

        Serial.printf("[F1Cal] Next race = %s (Rd %d), %d sessions, on %s\n",
                      s_nextRace.raceName, rRound,
                      s_nextRace.sessionCount, s_raceDate);
        break;
    }

    if (!found) {
        s_apiError = "No upcoming race found";
        s_apiFetching = false;
        Serial.printf("[F1Cal] No upcoming race found in %u races (now=%ld)\n",
                      (unsigned)races.size(), now);
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
