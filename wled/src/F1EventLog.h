/*
 * F1EventLog.h  –  Compile-time-configurable ring buffer for F1 live events.
 *
 * Header-only so it can be included in both the ESP32 firmware and in
 * native host unit tests without any Arduino / ESP-IDF dependencies.
 *
 * Usage (firmware):
 *   #define F1_EVLOG_MAX  32
 *   #include "F1EventLog.h"
 *   static F1EventLog<F1_EVLOG_MAX> s_evLog;
 */
#pragma once
#include <cstring>
#include <cstdint>

#ifndef F1_EVENT_MSG_LEN
#define F1_EVENT_MSG_LEN 80
#endif

template<int N>
struct F1EventLog {
    struct Entry {
        uint32_t epoch;
        char category[20];
        char message[F1_EVENT_MSG_LEN];
    };

    Entry  buf[N];
    int    head  = 0;   /* next write position */
    int    count = 0;   /* number of valid entries */

    void push(uint32_t epoch, const char* cat, const char* msg)
    {
        Entry& e = buf[head];
        e.epoch = epoch;
        strncpy(e.category, cat, sizeof(e.category) - 1);
        e.category[sizeof(e.category) - 1] = '\0';
        strncpy(e.message, msg, sizeof(e.message) - 1);
        e.message[sizeof(e.message) - 1] = '\0';
        head = (head + 1) % N;
        if (count < N) count++;
    }

    /* Get entry at logical index (0 = oldest). Returns false if out of range. */
    bool get(int idx, Entry& out) const
    {
        if (idx < 0 || idx >= count) return false;
        int start = (count < N) ? 0 : head;
        int pos = (start + idx) % N;
        out = buf[pos];
        return true;
    }

    void clear()
    {
        head  = 0;
        count = 0;
    }

    int size() const { return count; }
    int capacity() const { return N; }
};
