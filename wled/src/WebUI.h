#pragma once
/*
 * WebUI.h  –  ESPAsyncWebServer routes + embedded HTML UI
 *
 * Call webui_init() once from setup() after config is loaded and the
 * LED effect engine is running.  Registers all /api/* routes and the
 * catch-all that serves the single-page app.
 *
 * Callbacks:
 *   onForcedState  – called when the UI asks to force an F1 state
 *                    (state == 0xFF → clear forced state)
 *   onReboot       – called when the UI requests a reboot
 *   onTestEvent    – inject a live event for testing without a real session:
 *                    0=winner  1=fastest_lap  2=drs  3=start_lights
 */

#include <stdint.h>
#include <functional>
#include "F1NetWork.h"   /* F1NetState */

/* Returns true while the device is in AP mode (no STA connection). */
extern bool g_apMode;

void webui_init(
    std::function<void(uint8_t state)>   onForcedState,
    std::function<void()>                onReboot,
    std::function<void(uint8_t evType)>  onTestEvent
);
