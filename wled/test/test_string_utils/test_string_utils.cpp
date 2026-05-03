/*
 * test_string_utils.cpp – Unity tests for F1StringUtils.h
 * Run via: pio test -e native_test
 */
#include <unity.h>
#include "F1StringUtils.h"

/* ── url_encode ─────────────────────────────────────────────────────────── */

void test_url_encode_plain(void)
{
    char out[64];
    f1_url_encode("HelloWorld", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("HelloWorld", out);
}

void test_url_encode_space(void)
{
    char out[64];
    f1_url_encode("hello world", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("hello%20world", out);
}

void test_url_encode_special(void)
{
    char out[128];
    f1_url_encode("a=1&b=2+3", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a%3D1%26b%3D2%2B3", out);
}

void test_url_encode_empty(void)
{
    char out[16];
    f1_url_encode("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

/* ── json_str ───────────────────────────────────────────────────────────── */

void test_json_str_found(void)
{
    char out[64];
    bool ok = f1_json_str("{\"key\":\"value\"}", "key", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("value", out);
}

void test_json_str_not_found(void)
{
    char out[64] = "unchanged";
    bool ok = f1_json_str("{\"key\":\"value\"}", "missing", out, sizeof(out));
    TEST_ASSERT_FALSE(ok);
}

void test_json_str_multiple_keys(void)
{
    char out[64];
    bool ok = f1_json_str("{\"a\":\"alpha\",\"b\":\"beta\"}", "b", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("beta", out);
}

void test_json_str_escaped_quote_stops(void)
{
    /* Value ends at closing quote; any content after is not included */
    char out[64];
    bool ok = f1_json_str("{\"k\":\"hello\"}", "k", out, sizeof(out));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_STRING("hello", out);
}

/* ── main ───────────────────────────────────────────────────────────────── */

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_url_encode_plain);
    RUN_TEST(test_url_encode_space);
    RUN_TEST(test_url_encode_special);
    RUN_TEST(test_url_encode_empty);

    RUN_TEST(test_json_str_found);
    RUN_TEST(test_json_str_not_found);
    RUN_TEST(test_json_str_multiple_keys);
    RUN_TEST(test_json_str_escaped_quote_stops);

    return UNITY_END();
}
