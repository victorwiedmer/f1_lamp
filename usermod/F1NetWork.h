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
    F1ST_IDLE          = 0,   /* No active session  */
    F1ST_SESSION_START = 1,   /* Formation lap / session about to start */
    F1ST_GREEN         = 2,   /* Green flag / all-clear  */
    F1ST_YELLOW        = 3,   /* Yellow flag (sectors 2 or 3)  */
    F1ST_SAFETY_CAR    = 4,   /* Safety car deployed  */
    F1ST_VIRTUAL_SC    = 5,   /* Virtual Safety Car   */
    F1ST_RED_FLAG      = 6,   /* Red flag – session suspended */
    F1ST_CHEQUERED     = 7,   /* Chequered flag shown */
    F1ST_UNKNOWN       = 8
} F1NetState;

/*
 * Callback invoked from the ESP-IDF WebSocket FreeRTOS task whenever
 * the F1 state changes.  Implementation should be short and thread-safe.
 */
typedef void (*F1NetStateCB)(F1NetState newState);

/* Register state-change callback (call before f1net_setup) */
void f1net_setCallback(F1NetStateCB cb);

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
