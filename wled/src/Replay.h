#pragma once
/*
 * Replay.h  –  Offline session replay from livetiming.formula1.com/static/
 *
 * Usage:
 *   1. Call replay_setCallbacks() once from setup() with the same
 *      state/event callbacks used by the live F1 network task.
 *   2. Call replay_start(path, speed) from a UI button.
 *      This is non-blocking: a FreeRTOS task loads the 3 .jsonStream files
 *      in the background; poll replay_isLoading() / replay_isActive().
 *   3. Call replay_tick() from loop() every iteration.
 *   4. Call replay_stop() to cancel early.
 *   5. To populate the session selector, call replay_fetchSessionsJson(year)
 *      (synchronous, ~1-2 s).  Returns a cached JSON array string.
 */

#include <stdint.h>
#include "F1NetWork.h"   /* F1NetStateCB, F1EventCB, F1NetState, F1Event */

/* Max events kept in memory.  Typical race day: ~250 actionable events. */
#ifndef REPLAY_MAX_EVENTS
#define REPLAY_MAX_EVENTS 400
#endif

struct ReplayEvent {
    uint32_t ts_ms;     /* milliseconds offset from session start          */
    uint8_t  topic;     /* 0=TrackStatus  1=SessionStatus  2=RaceControl   */
    char     data[20];  /* extracted value (status code / keyword)         */
};

/* Register the callbacks that get fired as events are replayed.
   Use the same functions passed to f1net_setCallback() —
   events flow through the pending system so features still trigger. */
void replay_setCallbacks(F1NetStateCB stateCB, F1EventCB eventCB);

/* Start a replay.  Launches a background FreeRTOS task that fetches
   and parses the session streams; s_active becomes true when ready. */
void replay_start(const char* sessionPath, float speed);

/* Stop playback and free event memory. */
void replay_stop();

bool  replay_isLoading();    /* background fetch is in progress            */
bool  replay_isActive();     /* events are being dispatched                */
int   replay_eventCount();   /* total actionable events loaded             */
int   replay_currentIdx();   /* index of next event to fire                */
float replay_speed();

/* Call this from loop() every iteration. Non-blocking. */
void replay_tick();

/* Fetch + parse the session index for a year; returns a compact JSON
   array cached in heap:  [{"l":"Australian GP - Practice 1","p":"..."}...]
   Returns a static error string on failure.  ~1-2 s first call. */
const char* replay_fetchSessionsJson(int year);
