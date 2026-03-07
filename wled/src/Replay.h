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

/* Call this from loop() every iteration. Non-blocking. */
void replay_tick();
