#pragma once
/*
 * Replay.h  –  Static session replay from LittleFS
 *
 * A pre-built replay.json in LittleFS contains events extracted from
 * a real (or fake) F1 session.  The replay engine loads them, sorts
 * by timestamp, and dispatches through the same callback system as
 * live F1 timing.
 *
 * Usage:
 *   1. Call replay_setCallbacks() once from setup().
 *   2. replay_start(speed) → loads /replay.json and begins playback.
 *   3. Call replay_tick() from loop() every iteration.
 *   4. Call replay_stop() to cancel early.
 */

#include <stdint.h>
#include "F1NetWork.h"   /* F1NetStateCB, F1EventCB, F1NetState, F1Event */

/* Max events kept in memory. */
#ifndef REPLAY_MAX_EVENTS
#define REPLAY_MAX_EVENTS 400
#endif

struct ReplayEvent {
    uint32_t ts_ms;     /* milliseconds offset from session start          */
    uint8_t  topic;     /* 0=TrackStatus  1=SessionStatus  2=RaceControl   */
    char     data[20];  /* extracted value (status code / keyword)         */
};

/* Register the callbacks that get fired as events are replayed. */
void replay_setCallbacks(F1NetStateCB stateCB, F1EventCB eventCB);

/* Load /replay.json from LittleFS and begin playback at given speed. */
void replay_start(float speed);

/* Stop playback and free event memory. */
void replay_stop();

bool  replay_isLoading();
bool  replay_isActive();
int   replay_eventCount();
int   replay_currentIdx();
float replay_speed();

/* Start replay from a pre-built heap-allocated event array.
   Takes ownership of the array (freed on stop).  Events are sorted
   internally; caller does NOT need to pre-sort. */
void replay_startFromEvents(ReplayEvent* events, int count, float speed);

/* Information about the most recently dispatched replay event. */
struct ReplayLastEvent {
    bool     valid;       /* false if no event dispatched yet       */
    uint32_t ts_ms;       /* event timestamp (ms from session start)*/
    uint8_t  topic;       /* 0=Track 1=Session 2=RaceControl        */
    char     data[20];    /* raw data string                        */
    uint32_t simMs;       /* current simulated time (ms)            */
    uint32_t totalMs;     /* total session duration (ms)            */
};

/* Snapshot of the last dispatched event (safe to call from any context). */
ReplayLastEvent replay_lastEvent();

/* Call this from loop() every iteration. Non-blocking. */
void replay_tick();
