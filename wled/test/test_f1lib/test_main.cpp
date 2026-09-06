/*
 * test_f1lib.cpp – host unit tests (Unity) for the Arduino-free logic layers.
 *
 * Coverage:
 *   F1StringUtils.h  – URL encoding + JSON string-value extraction
 *   F1TimeUtils.h    – pure 32-bit UTC calendar math (the device uses this
 *                      instead of the hybrid-libc mktime/gmtime)
 *   F1Reaction.h     – flag-reaction rules (TrackStatus codes + RC messages)
 *   F1EventLog.h     – ring buffer semantics
 *
 * Firmware-level concerns (network stack, FastLED, Web UI) are exercised on
 * the device; see DEPLOY.md/README.
 */
#include <unity.h>
#include <cstring>
#include <string>

#include "F1StringUtils.h"
#include "F1TimeUtils.h"
#include "F1Reaction.h"
#include "F1EventLog.h"

/* ═══════════════ F1StringUtils ═══════════════ */

void test_url_encode(void)
{
    char out[128];
    f1_url_encode("Abz_-.~", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("Abz_-.~", out);

    f1_url_encode("a b/c?d=1&e=2", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%3Fd%3D1%26e%3D2", out);

    f1_url_encode("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

void test_json_str(void)
{
    char out[64];
    /* simple */
    TEST_ASSERT_TRUE(f1_json_str("{\"Status\":\"1\"}", "Status", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("1", out);
    /* whitespace around colon */
    TEST_ASSERT_TRUE(f1_json_str("{\"Status\" :  \"AllClear\"}", "Status", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("AllClear", out);
    /* first match wins when keys repeat deeper in the document */
    const char* doc =
        "{\"result\":{\"TrackStatus\":{\"Status\":\"2\",\"Message\":\"Yellow\"},"
        "\"SessionStatus\":{\"Status\":\"Inactive\"}}}";
    TEST_ASSERT_TRUE(f1_json_str(doc, "Status", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("2", out);
    TEST_ASSERT_TRUE(f1_json_str(doc, "Message", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Yellow", out);
    /* missing key */
    TEST_ASSERT_FALSE(f1_json_str("{\"a\":\"1\"}", "b", out, sizeof(out)));
    /* non-string value -> false */
    TEST_ASSERT_FALSE(f1_json_str("{\"n\":123}", "n", out, sizeof(out)));
}

/* ═══════════════ F1TimeUtils ═══════════════ */

void test_parse_utc_known(void)
{
    TEST_ASSERT_EQUAL_INT64(1788699600LL, (long long)f1_parseUtc("2026-09-06", "13:00:00"));
    TEST_ASSERT_EQUAL_INT64(1767225600LL, (long long)f1_parseUtc("2026-01-01", "00:00:00"));
    TEST_ASSERT_EQUAL_INT64(0LL, (long long)f1_parseUtc("2026-02-30", "00:00:00"));  /* invalid day -> 0? */
    TEST_ASSERT_EQUAL_INT64(0LL, (long long)f1_parseUtc("2026-13-01", "00:00:00"));
    TEST_ASSERT_EQUAL_INT64(0LL, (long long)f1_parseUtc("not-a-date", "00:00:00"));
}

void test_parse_iso_utc(void)
{
    /* plain UTC */
    TEST_ASSERT_EQUAL_INT64(1772942400LL, (long long)f1_parseIsoUtc("2026-03-08T04:00:00Z"));
    /* with positive offset (+10:00) */
    TEST_ASSERT_EQUAL_INT64(1772906400LL, (long long)f1_parseIsoUtc("2026-03-08T04:00:00+10:00"));
    /* with negative offset: 04:00 at -05:00 == 09:00Z */
    TEST_ASSERT_EQUAL_INT64(1772960400LL, (long long)f1_parseIsoUtc("2026-03-08T04:00:00-05:00"));
    /* malformed */
    TEST_ASSERT_EQUAL_INT64(0LL, (long long)f1_parseIsoUtc("2026-03-08T04:00Z"));    /* too short */
    TEST_ASSERT_EQUAL_INT64(0LL, (long long)f1_parseIsoUtc("2026-03-08 04:00:00Z")); /* no 'T' */
}

void test_fields_roundtrip(void)
{
    const char* cases[] = {
        "2026-09-06 13:00:00", "2026-03-08 04:00:00", "2026-11-22 04:00:00",
        "2024-02-29 12:00:00",  /* leap day */
        "2020-01-01 00:00:00",  "2036-12-31 23:59:59", "1970-01-01 00:00:00",
        "2027-01-01 00:00:00"
    };
    for (auto& c : cases) {
        char d[16] = {}, t[16] = {};
        sscanf(c, "%15s %15s", d, t);
        time_t e = f1_parseUtc(d, t);
        int y; unsigned mo, dd, h, mi, s;
        f1_fieldsFromEpoch(e, y, mo, dd, h, mi, s);
        char back[32];
        snprintf(back, sizeof back, "%04d-%02u-%02u %02u:%02u:%02u", y, mo, dd, h, mi, s);
        TEST_ASSERT_EQUAL_STRING(c, back);
    }
}

void test_utc_midnight(void)
{
    time_t mid = f1_utcMidnight(f1_parseUtc("2026-09-06", "13:00:00"));
    TEST_ASSERT_EQUAL_INT64(1788652800LL, (long long)mid);
    TEST_ASSERT_EQUAL_INT64(1788652800LL,
        (long long)f1_utcMidnight(f1_parseUtc("2026-09-06", "00:00:00")));
}

void test_weekend_window(void)
{
    time_t fp1  = f1_parseUtc("2026-09-04", "10:30:00");  /* first session */
    time_t race = f1_parseUtc("2026-09-06", "00:00:00");  /* race-day midnight */
    /* 1 h before FP1 -> not yet active */
    TEST_ASSERT_FALSE(f1_weekendWindowActive(fp1 - 3600, fp1, race));
    /* 10 min before FP1 -> active */
    TEST_ASSERT_TRUE(f1_weekendWindowActive(fp1 - 600, fp1, race));
    /* 26 h after race-day midnight -> active (window lasts midnight + 27 h) */
    TEST_ASSERT_TRUE(f1_weekendWindowActive(f1_parseUtc("2026-09-07", "02:00:00"), fp1, race));
    /* beyond race-midnight + 27 h (03:00 on Sep 7) -> not active */
    TEST_ASSERT_FALSE(f1_weekendWindowActive(f1_parseUtc("2026-09-07", "03:00:00"), fp1, race));
}

void test_idle_factor(void)
{
    time_t target = f1_parseUtc("2026-09-06", "10:30:00");
    /* far out (>7 d) -> 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f1_idleBrightnessFactor(target - 8 * 86400, target));
    /* exactly 7 d out -> ~0.05 */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.05f, f1_idleBrightnessFactor(target - 7 * 86400, target));
    /* race day -> 1.0 */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, f1_idleBrightnessFactor(target, target));
    /* past -> 0 */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f1_idleBrightnessFactor(target + 60, target));
}

void test_track_index_map(void)
{
    TEST_ASSERT_EQUAL_INT(2, f1_trackCodeToIndex("1"));
    TEST_ASSERT_EQUAL_INT(3, f1_trackCodeToIndex("2"));
    TEST_ASSERT_EQUAL_INT(4, f1_trackCodeToIndex("4"));
    TEST_ASSERT_EQUAL_INT(5, f1_trackCodeToIndex("5"));
    TEST_ASSERT_EQUAL_INT(6, f1_trackCodeToIndex("6"));
    TEST_ASSERT_EQUAL_INT(7, f1_trackCodeToIndex("7"));
    TEST_ASSERT_EQUAL_INT(-1, f1_trackCodeToIndex("9"));
    TEST_ASSERT_EQUAL_INT(-1, f1_trackCodeToIndex(""));
}

/* ═══════════════ F1Reaction ═══════════════ */

void test_track_codes(void)
{
    TEST_ASSERT_EQUAL_INT(F1ST_GREEN,        f1_trackCodeToState("1"));
    TEST_ASSERT_EQUAL_INT(F1ST_YELLOW,       f1_trackCodeToState("2"));
    TEST_ASSERT_EQUAL_INT(F1ST_YELLOW,       f1_trackCodeToState("3"));
    TEST_ASSERT_EQUAL_INT(F1ST_SAFETY_CAR,   f1_trackCodeToState("4"));
    TEST_ASSERT_EQUAL_INT(F1ST_RED_FLAG,     f1_trackCodeToState("5"));
    TEST_ASSERT_EQUAL_INT(F1ST_VIRTUAL_SC,   f1_trackCodeToState("6"));
    TEST_ASSERT_EQUAL_INT(F1ST_VSC_ENDING,   f1_trackCodeToState("7"));
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,      f1_trackCodeToState("8"));
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,      f1_trackCodeToState(""));
}

void test_rc_global_announcements(void)
{
    const bool G = true, S = false;
    TEST_ASSERT_EQUAL_INT(F1ST_RED_FLAG,
        f1_rcEventToState(G, S, "Other", "", "", "RED FLAG - RACE SUSPENDED"));
    TEST_ASSERT_EQUAL_INT(F1ST_SAFETY_CAR,
        f1_rcEventToState(G, S, "SafetyCar", "", "", "SAFETY CAR DEPLOYED"));
    TEST_ASSERT_EQUAL_INT(F1ST_VIRTUAL_SC,
        f1_rcEventToState(G, S, "Other", "", "", "VIRTUAL SAFETY CAR DEPLOYED"));
    TEST_ASSERT_EQUAL_INT(F1ST_VIRTUAL_SC,
        f1_rcEventToState(G, S, "Other", "", "", "VSC DEPLOYED"));
    TEST_ASSERT_EQUAL_INT(F1ST_CHEQUERED,
        f1_rcEventToState(G, S, "Other", "", "", "CHEQUERED FLAG"));
    /* case-insensitive */
    TEST_ASSERT_EQUAL_INT(F1ST_RED_FLAG,
        f1_rcEventToState(G, S, "Other", "", "", "red flag shown"));
}

void test_rc_track_scope_flags(void)
{
    const bool G = true, S = false;
    /* structured track-scope yellow / red */
    TEST_ASSERT_EQUAL_INT(F1ST_YELLOW,
        f1_rcEventToState(G, S, "Flag", "YELLOW", "Track", "YELLOW FLAG"));
    TEST_ASSERT_EQUAL_INT(F1ST_RED_FLAG,
        f1_rcEventToState(G, S, "Flag", "RED", "Track", "RED FLAG SHOWN"));
    /* green/clear deliberately ignored (left to TrackStatus code 1) */
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,
        f1_rcEventToState(G, S, "Flag", "GREEN", "Track", "TRACK CLEAR"));
}

void test_rc_sector_notes(void)
{
    const bool G = true;
    const char* sectorYellow = "YELLOW IN TRACK SECTOR 13";
    const char* sectorClear  = "CLEAR IN TRACK SECTOR 13";
    /* sector handling off (default): ignored for LED */
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,
        f1_rcEventToState(G, false, "Flag", "YELLOW", "Sector", sectorYellow));
    /* sector handling on: yellow note -> yellow */
    TEST_ASSERT_EQUAL_INT(F1ST_YELLOW,
        f1_rcEventToState(G, true, "Flag", "YELLOW", "Sector", sectorYellow));
    /* sector clear never clears a state */
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,
        f1_rcEventToState(G, true, "Flag", "CLEAR", "Sector", sectorClear));
    /* all reactions disabled */
    TEST_ASSERT_EQUAL_INT(F1ST_UNKNOWN,
        f1_rcEventToState(false, false, "Other", "", "", "RED FLAG - RACE SUSPENDED"));
}

/* ═══════════════ F1EventLog ═══════════════ */

void test_event_log_ring(void)
{
    F1EventLog<4> log;
    TEST_ASSERT_EQUAL_INT(0, log.size());
    TEST_ASSERT_EQUAL_INT(4, log.capacity());
    log.push(1, "Track", "A");
    log.push(2, "Session", "B");
    F1EventLog<4>::Entry e;
    TEST_ASSERT_TRUE(log.get(0, e));
    TEST_ASSERT_EQUAL_UINT32(1, e.epoch);
    TEST_ASSERT_EQUAL_STRING("Track", e.category);
    TEST_ASSERT_TRUE(log.get(1, e));
    TEST_ASSERT_EQUAL_STRING("B", e.message);
    TEST_ASSERT_FALSE(log.get(2, e));

    /* wrap + overwrite oldest */
    log.push(3, "RaceCtrl", "C");
    log.push(4, "Track", "D");
    log.push(5, "Session", "E");   /* ring full (4) -> A overwritten */
    TEST_ASSERT_EQUAL_INT(4, log.size());
    TEST_ASSERT_TRUE(log.get(0, e));
    TEST_ASSERT_EQUAL_STRING("Session", e.category);  /* oldest surviving (entry B) */
    TEST_ASSERT_EQUAL_STRING("B", e.message);
    TEST_ASSERT_TRUE(log.get(3, e));
    TEST_ASSERT_EQUAL_STRING("E", e.message);

    log.clear();
    TEST_ASSERT_EQUAL_INT(0, log.size());
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_url_encode);
    RUN_TEST(test_json_str);

    RUN_TEST(test_parse_utc_known);
    RUN_TEST(test_parse_iso_utc);
    RUN_TEST(test_fields_roundtrip);
    RUN_TEST(test_utc_midnight);
    RUN_TEST(test_weekend_window);
    RUN_TEST(test_idle_factor);
    RUN_TEST(test_track_index_map);

    RUN_TEST(test_track_codes);
    RUN_TEST(test_rc_global_announcements);
    RUN_TEST(test_rc_track_scope_flags);
    RUN_TEST(test_rc_sector_notes);

    RUN_TEST(test_event_log_ring);

    return UNITY_END();
}
