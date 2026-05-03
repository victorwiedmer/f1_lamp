/*
 * test_time_utils.cpp – Unity tests for F1TimeUtils.h
 * Run via: pio test -e native_test
 */
#include <unity.h>
#include "F1TimeUtils.h"

/* ── f1_parseUtc ────────────────────────────────────────────────────────── */

void test_parseUtc_date_only(void)
{
    /* 2026-01-01 00:00:00 UTC */
    time_t t = f1_parseUtc("2026-01-01", nullptr);
    TEST_ASSERT_EQUAL(1767225600, (long)t);   /* verified with: date -u -d "2026-01-01" +%s */
}

void test_parseUtc_date_and_time(void)
{
    /* 2026-05-03 14:30:00 UTC */
    time_t t = f1_parseUtc("2026-05-03", "14:30:00Z");
    TEST_ASSERT_EQUAL(1777818600, (long)t);  /* date -u -d "2026-05-03 14:30:00" +%s */
}

void test_parseUtc_bad_date_returns_zero(void)
{
    time_t t = f1_parseUtc("not-a-date", nullptr);
    TEST_ASSERT_EQUAL(0, (long)t);
}

void test_parseUtc_null_date_returns_zero(void)
{
    time_t t = f1_parseUtc(nullptr, nullptr);
    TEST_ASSERT_EQUAL(0, (long)t);
}

/* ── f1_weekendWindowActive ─────────────────────────────────────────────── */

void test_weekend_active_inside_window(void)
{
    time_t fp1   = 1746100800;   /* arbitrary anchor */
    time_t race  = fp1 + 2 * 86400;
    time_t now   = fp1 + 3600;  /* 1 h after FP1 */
    TEST_ASSERT_TRUE(f1_weekendWindowActive(now, fp1, race));
}

void test_weekend_active_before_window(void)
{
    time_t fp1   = 1746100800;
    time_t race  = fp1 + 2 * 86400;
    time_t now   = fp1 - 3600;  /* 1 h BEFORE 30-min grace → still before */
    TEST_ASSERT_FALSE(f1_weekendWindowActive(now, fp1, race));
}

void test_weekend_active_in_grace_period(void)
{
    time_t fp1   = 1746100800;
    time_t race  = fp1 + 2 * 86400;
    time_t now   = fp1 - 1000;  /* 1000 s before FP1 – inside 30-min grace */
    TEST_ASSERT_TRUE(f1_weekendWindowActive(now, fp1, race));
}

void test_weekend_active_after_window(void)
{
    time_t fp1   = 1746100800;
    time_t race  = fp1 + 2 * 86400;
    time_t now   = race + 97201; /* 1 s after 27-h close */
    TEST_ASSERT_FALSE(f1_weekendWindowActive(now, fp1, race));
}

void test_weekend_active_at_window_end(void)
{
    time_t fp1   = 1746100800;
    time_t race  = fp1 + 2 * 86400;
    time_t now   = race + 97200; /* exactly at the boundary → closed */
    TEST_ASSERT_FALSE(f1_weekendWindowActive(now, fp1, race));
}

/* ── f1_trackCodeToIndex ────────────────────────────────────────────────── */

void test_track_code_green(void)  { TEST_ASSERT_EQUAL(2, f1_trackCodeToIndex("1")); }
void test_track_code_yellow(void) { TEST_ASSERT_EQUAL(3, f1_trackCodeToIndex("2")); }
void test_track_code_sc(void)     { TEST_ASSERT_EQUAL(4, f1_trackCodeToIndex("4")); }
void test_track_code_red(void)    { TEST_ASSERT_EQUAL(5, f1_trackCodeToIndex("5")); }
void test_track_code_vsc(void)    { TEST_ASSERT_EQUAL(6, f1_trackCodeToIndex("6")); }
void test_track_code_vsc_end(void){ TEST_ASSERT_EQUAL(7, f1_trackCodeToIndex("7")); }
void test_track_code_unknown(void){ TEST_ASSERT_EQUAL(-1, f1_trackCodeToIndex("9")); }
void test_track_code_null(void)   { TEST_ASSERT_EQUAL(-1, f1_trackCodeToIndex(nullptr)); }
void test_track_code_empty(void)  { TEST_ASSERT_EQUAL(-1, f1_trackCodeToIndex("")); }

/* ── f1_idleBrightnessFactor ────────────────────────────────────────────── */

void test_idle_factor_over_7_days(void)
{
    time_t now    = 1000000;
    time_t target = now + 8 * 86400; /* 8 days away */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f1_idleBrightnessFactor(now, target));
}

void test_idle_factor_past_target(void)
{
    time_t now    = 1000000;
    time_t target = now - 3600; /* already in the past */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f1_idleBrightnessFactor(now, target));
}

void test_idle_factor_exactly_7_days(void)
{
    time_t now    = 1000000;
    time_t target = now + 7 * 86400;
    /* diffDays == 7.0 → factor == 0.05 (minimum) */
    float f = f1_idleBrightnessFactor(now, target);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.05f, f);
}

void test_idle_factor_3_5_days(void)
{
    time_t now    = 1000000;
    time_t target = now + (long)(3.5 * 86400);
    float expected = 1.0f - (3.5f / 7.0f) * 0.95f;  /* ≈ 0.525 */
    float f = f1_idleBrightnessFactor(now, target);
    TEST_ASSERT_FLOAT_WITHIN(0.02f, expected, f);
}

void test_idle_factor_imminent(void)
{
    time_t now    = 1000000;
    time_t target = now + 60; /* 60 s away → ~1.0 */
    float f = f1_idleBrightnessFactor(now, target);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1.0f, f);
}

/* ── main ───────────────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_parseUtc_date_only);
    RUN_TEST(test_parseUtc_date_and_time);
    RUN_TEST(test_parseUtc_bad_date_returns_zero);
    RUN_TEST(test_parseUtc_null_date_returns_zero);

    RUN_TEST(test_weekend_active_inside_window);
    RUN_TEST(test_weekend_active_before_window);
    RUN_TEST(test_weekend_active_in_grace_period);
    RUN_TEST(test_weekend_active_after_window);
    RUN_TEST(test_weekend_active_at_window_end);

    RUN_TEST(test_track_code_green);
    RUN_TEST(test_track_code_yellow);
    RUN_TEST(test_track_code_sc);
    RUN_TEST(test_track_code_red);
    RUN_TEST(test_track_code_vsc);
    RUN_TEST(test_track_code_vsc_end);
    RUN_TEST(test_track_code_unknown);
    RUN_TEST(test_track_code_null);
    RUN_TEST(test_track_code_empty);

    RUN_TEST(test_idle_factor_over_7_days);
    RUN_TEST(test_idle_factor_past_target);
    RUN_TEST(test_idle_factor_exactly_7_days);
    RUN_TEST(test_idle_factor_3_5_days);
    RUN_TEST(test_idle_factor_imminent);

    return UNITY_END();
}
