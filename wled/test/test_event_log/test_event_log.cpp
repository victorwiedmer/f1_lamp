/*
 * test_event_log.cpp – Unity tests for F1EventLog.h
 * Run via: pio test -e native_test
 */
#include <unity.h>
#include <cstdio>
#include "F1EventLog.h"

/* ── basic push / get ───────────────────────────────────────────────────── */

void test_push_single_entry(void)
{
    F1EventLog<8> log;
    log.push(1000, "RC", "Safety car deployed");
    TEST_ASSERT_EQUAL(1, log.size());

    F1EventLog<8>::Entry e;
    TEST_ASSERT_TRUE(log.get(0, e));
    TEST_ASSERT_EQUAL(1000u, e.epoch);
    TEST_ASSERT_EQUAL_STRING("RC", e.category);
    TEST_ASSERT_EQUAL_STRING("Safety car deployed", e.message);
}

void test_multiple_entries_fifo_order(void)
{
    F1EventLog<8> log;
    log.push(1, "A", "first");
    log.push(2, "B", "second");
    log.push(3, "C", "third");
    TEST_ASSERT_EQUAL(3, log.size());

    F1EventLog<8>::Entry e;
    log.get(0, e); TEST_ASSERT_EQUAL_STRING("first",  e.message);
    log.get(1, e); TEST_ASSERT_EQUAL_STRING("second", e.message);
    log.get(2, e); TEST_ASSERT_EQUAL_STRING("third",  e.message);
}

void test_out_of_range_get_returns_false(void)
{
    F1EventLog<4> log;
    log.push(1, "X", "msg");
    F1EventLog<4>::Entry e;
    TEST_ASSERT_FALSE(log.get(1, e));
    TEST_ASSERT_FALSE(log.get(-1, e));
}

/* ── ring-buffer wrap ───────────────────────────────────────────────────── */

void test_ring_buffer_wraps_oldest_first(void)
{
    /* Capacity 4; push 5 entries → oldest (entry 0) is evicted */
    F1EventLog<4> log;
    for (int i = 0; i < 5; ++i) {
        char msg[16];
        snprintf(msg, sizeof(msg), "msg%d", i);
        log.push(i, "T", msg);
    }
    TEST_ASSERT_EQUAL(4, log.size());

    /* Oldest surviving entry should be index 1 ("msg1") */
    F1EventLog<4>::Entry e;
    log.get(0, e);
    TEST_ASSERT_EQUAL_STRING("msg1", e.message);
    /* Newest should be "msg4" */
    log.get(3, e);
    TEST_ASSERT_EQUAL_STRING("msg4", e.message);
}

void test_capacity_reported_correctly(void)
{
    F1EventLog<16> log;
    TEST_ASSERT_EQUAL(16, log.capacity());
}

/* ── clear ──────────────────────────────────────────────────────────────── */

void test_clear_resets_size(void)
{
    F1EventLog<8> log;
    log.push(1, "A", "x");
    log.push(2, "B", "y");
    log.clear();
    TEST_ASSERT_EQUAL(0, log.size());
}

void test_clear_then_push(void)
{
    F1EventLog<4> log;
    log.push(10, "Z", "old");
    log.clear();
    log.push(99, "N", "new");
    F1EventLog<4>::Entry e;
    TEST_ASSERT_TRUE(log.get(0, e));
    TEST_ASSERT_EQUAL(99u, e.epoch);
    TEST_ASSERT_EQUAL_STRING("new", e.message);
}

/* ── truncation ─────────────────────────────────────────────────────────── */

void test_long_message_truncated(void)
{
    F1EventLog<2> log;
    char long_msg[512];
    memset(long_msg, 'X', sizeof(long_msg) - 1);
    long_msg[sizeof(long_msg) - 1] = '\0';

    log.push(1, "T", long_msg);
    F1EventLog<2>::Entry e;
    log.get(0, e);
    /* message field is F1_EVENT_MSG_LEN bytes; last byte must be NUL */
    TEST_ASSERT_EQUAL('\0', e.message[F1_EVENT_MSG_LEN - 1]);
}

/* ── main ───────────────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_push_single_entry);
    RUN_TEST(test_multiple_entries_fifo_order);
    RUN_TEST(test_out_of_range_get_returns_false);
    RUN_TEST(test_ring_buffer_wraps_oldest_first);
    RUN_TEST(test_capacity_reported_correctly);
    RUN_TEST(test_clear_resets_size);
    RUN_TEST(test_clear_then_push);
    RUN_TEST(test_long_message_truncated);

    return UNITY_END();
}
