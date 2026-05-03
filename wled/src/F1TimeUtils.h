/*
 * F1TimeUtils.h  –  Pure-C++, Arduino-free date/time utilities.
 *
 * Extracted here so they can be unit-tested on any host (no ESP32 SDK
 * required).  Included by F1Calendar.cpp and by native test builds.
 */
#pragma once
#include <ctime>
#include <cstring>
#include <cstdio>

/* ── Portable UTC mktime ──────────────────────────────────────────────────
 * mktime() is TZ-dependent on POSIX.  Provide a UTC-safe wrapper:
 *   - Device (Arduino/ESP-IDF): configTime(0,0) sets TZ=UTC → mktime is fine.
 *   - Linux/macOS: timegm() is available.
 *   - Windows (MSVC / MinGW): _mkgmtime() is available.
 */
inline time_t f1_timegm(struct tm* t)
{
#if defined(ARDUINO) || defined(IDF_VER)
    return mktime(t);      /* device: TZ already UTC from configTime(0,0,...) */
#elif defined(_WIN32)
    return _mkgmtime(t);   /* Windows MSVC/MinGW */
#else
    return timegm(t);      /* Linux / macOS */
#endif
}

/* ── parseUtc ─────────────────────────────────────────────────────────────
 * Convert "YYYY-MM-DD" (dateStr) + "HH:MM:SSZ" (timeStr, may be nullptr)
 * into a UTC epoch.
 *
 * Returns 0 on parse failure.
 */
inline time_t f1_parseUtc(const char* dateStr, const char* timeStr)
{
    if (!dateStr || !dateStr[0]) return 0;
    struct tm t = {};
    int year, mon, day;
    if (sscanf(dateStr, "%d-%d-%d", &year, &mon, &day) != 3) return 0;
    t.tm_year = year - 1900;
    t.tm_mon  = mon  - 1;
    t.tm_mday = day;
    if (timeStr && timeStr[0]) {
        int h = 0, m = 0, s = 0;
        sscanf(timeStr, "%d:%d:%d", &h, &m, &s);
        t.tm_hour = h; t.tm_min = m; t.tm_sec = s;
    }
    t.tm_isdst = 0;
    return f1_timegm(&t);
}

/* ── weekendWindowActive ──────────────────────────────────────────────────
 * Returns true if 'now' falls within the race-weekend window:
 *   [firstSessEpoch - 30 min , raceEpoch + 27 h)
 */
inline bool f1_weekendWindowActive(time_t now,
                                    time_t firstSessEpoch,
                                    time_t raceEpoch)
{
    time_t winStart = firstSessEpoch - 1800;
    time_t winEnd   = raceEpoch + 97200;   /* 27 h */
    return (now >= winStart && now < winEnd);
}

/* ── trackCodeToStateIndex ────────────────────────────────────────────────
 * Maps SignalR TrackStatus code string to an integer index:
 *   0=Idle  1=?  2=Green  3=Yellow  4=SafetyCar  5=RedFlag  6=VSC  7=VSCEnding
 * Returns -1 for unknown.
 */
inline int f1_trackCodeToIndex(const char* code)
{
    if (!code || !code[0]) return -1;
    switch (code[0]) {
        case '1': return 2;   /* Green      */
        case '2': return 3;   /* Yellow     */
        case '4': return 4;   /* Safety Car */
        case '5': return 5;   /* Red Flag   */
        case '6': return 6;   /* VSC        */
        case '7': return 7;   /* VSC Ending */
        default:  return -1;
    }
}

/* ── idleBrightnessFactor ─────────────────────────────────────────────────
 * Linear brightness ramp over 7 days before 'target' (first session).
 *   Returns 0.0f if > 7 days away or already past.
 *   Returns values [0.05, 1.0] as target approaches.
 */
inline float f1_idleBrightnessFactor(time_t now, time_t target)
{
    double diffSec  = (double)(target - now);
    double diffDays = diffSec / 86400.0;
    if (diffDays < 0.0) return 0.0f;
    if (diffDays > 7.0) return 0.0f;
    float factor = 1.0f - (float)(diffDays / 7.0) * 0.95f;
    if (factor < 0.05f) factor = 0.05f;
    if (factor > 1.0f)  factor = 1.0f;
    return factor;
}
