#pragma once
/*
 * F1NetWork.h  –  Pure C interface to the F1 SignalR network layer.
 *
 * Must NOT include any ESP-IDF HTTP/WS headers here (they conflict with
 * ESPAsyncWebServer's WebRequestMethod enum in WLED).  All IDF code lives
 * in F1NetWork.cpp, which never includes wled.h.
 */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* F1 session / track states (mirrors the F1 live-timing TrackStatus codes) */
typedef enum {
    F1ST_IDLE          = 0,   /* No active session / race running normally   */
    F1ST_SESSION_START = 1,   /* Formation lap / session about to start      */
    F1ST_GREEN         = 2,   /* Green flag / all-clear  (temporary, reverts) */
    F1ST_YELLOW        = 3,   /* Yellow flag (FCY)                           */
    F1ST_SAFETY_CAR    = 4,   /* Safety car deployed                         */
    F1ST_VIRTUAL_SC    = 5,   /* Virtual Safety Car deployed                 */
    F1ST_RED_FLAG      = 6,   /* Red flag – session suspended                */
    F1ST_CHEQUERED     = 7,   /* Chequered flag shown                        */
    F1ST_VSC_ENDING    = 8,   /* VSC ending – prepare to race                */
    F1ST_SC_ENDING     = 9,   /* SC ending  – no live code, force/test only  */
    F1ST_UNKNOWN       = 10
} F1NetState;

/*
 * Callback invoked from the ESP-IDF WebSocket FreeRTOS task whenever
 * the F1 state changes.  Implementation should be short and thread-safe.
 */
typedef void (*F1NetStateCB)(F1NetState newState);

/* Register state-change callback (call before f1net_setup) */
void f1net_setCallback(F1NetStateCB cb);

/*
 * Live-timing event notifications (fastest lap, DRS open, etc.).
 * These fire independently of the state machine callbacks above.
 */
typedef enum {
    F1EVT_FASTEST_LAP = 0,   /* Overall fastest lap set by a driver     */
    F1EVT_DRS_ENABLED = 1,   /* DRS detection zones opened              */
} F1Event;

typedef void (*F1EventCB)(F1Event ev);
void f1net_setEventCallback(F1EventCB cb);

/* Initialise – call once from WLED setup()  */
void f1net_setup(void);

/* Drive reconnect / keepalive – call every loop() tick */
void f1net_loop(void);

/* Query connection state */
bool       f1net_isConnected(void);
F1NetState f1net_getState(void);

/* Disconnect and reset (called when usermod is disabled) */
void f1net_disconnect(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
