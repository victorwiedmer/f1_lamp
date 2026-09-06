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
#include <cstdint>

/* ── Clean wall clock ──────────────────────────────────────────────────────
 * The ESP32-C3 toolchain hybrid here is nasty: picolibc headers declare a
 * 64-bit time_t, but the linked (newlib) time()/mktime() only write the
 * low 32 bits of the return register, leaving the upper word full of stale
 * garbage.  Comparisons/arithmetic on such a "dirty" time_t go haywire
 * (observed: every race in the calendar scan looked finished).  All epochs
 * we handle are < 2038, so truncating to the low 32 bits yields the true
 * UTC epoch.  Use f1_clock() instead of time(nullptr) and do the calendar
 * math in 32-bit longs.
 */
inline time_t f1_clock()
{
    return (time_t)(uint32_t)time(nullptr);
}

/* ══════════════════════════════════════════════════════════════════════════
 * Pure UTC calendar arithmetic — NO dependency on the C library clock
 * conversion functions (mktime / gmtime / timegm / sscanf / _mkgmtime).
 *
 * WHY: on the ESP32-C3 build this firmware links precompiled SDK libc
 * (GCC 8.4 + newlib) together with the newer Espressif toolchain
 * (GCC 14.x + picolibc).  The two disagree on struct tm layout and on
 * some runtime helpers, so gmtime_r()/sscanf() and 64-bit helper calls
 * across the boundary return garbage (observed on device: gmtime_r wrote
 * year 1 266 959; f1cal even produced year 101 600 335 through 64-bit
 * div helpers).  Everything below therefore uses only 32-bit integer
 * arithmetic (native RISC-V `div`) and hand-rolled digit parsing.
 * time(nullptr) is still used to obtain "now".
 * ══════════════════════════════════════════════════════════════════════════ */

/* Days since 1970-01-01 for a Gregorian date (proleptic). 32-bit safe for
 * any date in [1900, 2100] (range we ever care about). */
inline int32_t f1_daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= (int)(m <= 2);
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const int32_t yoe = y - era * 400;                    /* [0, 399]  */
    const int32_t mo  = (int32_t)(m + (m > 2 ? -3u : 9u));
    const int32_t doy = (153 * mo + 2) / 5 + (int32_t)d - 1; /* [0, 365] */
    const int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy; /* [0,146096] */
    return era * 146097 + doe - 719468;
}

/* Inverse: civil date from days-since-1970-01-01. */
inline void f1_civilFromDays(int32_t z, int& y, unsigned& m, unsigned& d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int32_t doe = z - era * 146097;                 /* [0, 146096] */
    const int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t yv  = yoe + era * 400;
    const int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100); /* [0, 365] */
    const int32_t mp  = (5 * doy + 2) / 153;              /* [0, 11] */
    d = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
    m = (mp < 10) ? (unsigned)(mp + 3) : (unsigned)(mp - 9); /* [1, 12] */
    y = (int)(yv + (int)(m <= 2));
}

/* UTC epoch seconds from broken-down UTC fields. */
inline time_t f1_epochFromFields(int y, unsigned mo, unsigned d,
                                 unsigned h, unsigned mi, unsigned s)
{
    return (time_t)((long)f1_daysFromCivil(y, (unsigned)mo, d) * 86400L
                    + (long)h * 3600L + (long)mi * 60L + (long)s);
}

/* Split a (non-negative) UTC epoch into UTC fields. */
inline void f1_fieldsFromEpoch(time_t t, int& y, unsigned& mo, unsigned& d,
                               unsigned& h, unsigned& mi, unsigned& s)
{
    long secs = (long)t;
    long days = secs / 86400L;
    long rem  = secs % 86400L;
    if (rem < 0) { rem += 86400L; --days; }   /* defensive; t is >= 0 here */
    f1_civilFromDays((int32_t)days, y, mo, d);
    h  = (unsigned)(rem / 3600L);
    mi = (unsigned)((rem % 3600L) / 60L);
    s  = (unsigned)(rem % 60L);
}

/* UTC midnight (00:00:00 UTC) of the UTC day containing epoch t. */
inline time_t f1_utcMidnight(time_t t)
{
    long days = (long)t / 86400L;
    if (t < 0 && (long)t % 86400L != 0) --days;
    return (time_t)(days * 86400L);
}

/* ── Manual digit parsing (no sscanf / libc) ─────────────────────────────── */

/* Parse up to 'maxd' leading digits into out. Returns chars consumed, or 0
 * if no digit present. */
inline int f1_parseDigits(const char* s, int maxd, long& out)
{
    long v = 0;
    int n = 0;
    while (n < maxd && s[n] >= '0' && s[n] <= '9') {
        v = v * 10 + (long)(s[n] - '0');
        ++n;
    }
    if (n == 0) return 0;
    out = v;
    return n;
}

/* ── parseUtc ─────────────────────────────────────────────────────────────
 * Convert "YYYY-MM-DD" (dateStr) + "HH:MM:SSZ" (timeStr, may be nullptr)
 * into a UTC epoch.  Returns 0 on parse failure.
 */
inline time_t f1_parseUtc(const char* dateStr, const char* timeStr)
{
    if (!dateStr || !dateStr[0]) return 0;
    const char* p = dateStr;
    long y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;

    int n = f1_parseDigits(p, 4, y);   if (n != 4 || p[n] != '-') return 0;
    p += n + 1;
    n = f1_parseDigits(p, 2, mo);      if (n != 2 || p[n] != '-') return 0;
    p += n + 1;
    n = f1_parseDigits(p, 2, d);       if (n != 2) return 0;
    if (y < 1970 || y > 2100 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;

    if (timeStr && timeStr[0]) {
        const char* t = timeStr;
        n = f1_parseDigits(t, 2, h);   if (n != 2 || t[n] != ':') return 0;
        t += n + 1;
        n = f1_parseDigits(t, 2, mi);  if (n != 2 || t[n] != ':') return 0;
        t += n + 1;
        n = f1_parseDigits(t, 2, s);   if (n != 2) return 0;
        if (h > 23 || mi > 59 || s > 59) return 0;
    }

    time_t e = f1_epochFromFields((int)y, (unsigned)mo, (unsigned)d,
                                  (unsigned)h, (unsigned)mi, (unsigned)s);

    /* Reject impossible calendar dates (e.g. 2026-02-30) by round-tripping
       through the civil arithmetic: a real date must map back to itself. */
    int   vy; unsigned vmo, vd, vh, vmi, vs;
    f1_fieldsFromEpoch(e, vy, vmo, vd, vh, vmi, vs);
    if (vy != (int)y || vmo != (unsigned)mo || vd != (unsigned)d) return 0;

    return e;
}

/* ── parseIsoUtc ──────────────────────────────────────────────────────────
 * Parse a full ISO 8601 timestamp into a UTC epoch:
 *   "YYYY-MM-DDTHH:MM:SSZ"          → treated as UTC
 *   "YYYY-MM-DDTHH:MM:SS+HH:MM"     → local clock with offset → converted
 *   "YYYY-MM-DDTHH:MM:SS-HH:MM"     → local clock with negative offset
 *
 * Handles calendar feeds whose session times carry local-time offsets
 * (e.g. "2026-03-08T04:00:00+10:00") that must be normalised to UTC
 * before the race-week ramp / session logic compares them.
 *
 * Returns 0 on parse failure.
 */
inline time_t f1_parseIsoUtc(const char* iso)
{
    if (!iso || !iso[0] || strlen(iso) < 19) return 0;
    char date[12] = {};
    char time[9]  = {};
    memcpy(date, iso, 10);
    memcpy(time, iso + 11, 8);   /* "HH:MM:SS" */
    if (iso[10] != 'T') return 0;

    time_t utc = f1_parseUtc(date, time);
    if (utc == 0) return 0;

    /* Timezone suffix starts at offset 19: 'Z' (UTC) or ±HH:MM */
    const char* tz = iso + 19;
    if (*tz == '\0' || *tz == 'Z') return utc;

    char sign = *tz;
    if (sign != '+' && sign != '-') return 0;
    long oh = 0, om = 0;
    int n = f1_parseDigits(tz + 1, 2, oh);
    if (n != 2 || tz[3] != ':') return 0;
    n = f1_parseDigits(tz + 4, 2, om);
    if (n != 2) return 0;

    long offSec = oh * 3600L + om * 60L;
    if (sign == '-') offSec = -offSec;
    return (time_t)((long)utc - offSec);   /* local clock + offset = UTC */
}

/* ── weekendWindowActive ──────────────────────────────────────────────────
 * Returns true if 'now' falls within the race-weekend window:
 *   [firstSessEpoch - 30 min , raceEpoch + 27 h)
 */
inline bool f1_weekendWindowActive(time_t now,
                                    time_t firstSessEpoch,
                                    time_t raceEpoch)
{
    long n  = (long)now;
    long a  = (long)firstSessEpoch - 1800L;   /* FP1/sprint-Q minus 30 min  */
    long b  = (long)raceEpoch   + 97200L;     /* race midnight + 27 h       */
    return (n >= a && n < b);
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
    double diffSec  = (double)((long)target - (long)now);
    double diffDays = diffSec / 86400.0;
    if (diffDays < 0.0) return 0.0f;
    if (diffDays > 7.0) return 0.0f;
    float factor = 1.0f - (float)(diffDays / 7.0) * 0.95f;
    if (factor < 0.05f) factor = 0.05f;
    if (factor > 1.0f)  factor = 1.0f;
    return factor;
}
