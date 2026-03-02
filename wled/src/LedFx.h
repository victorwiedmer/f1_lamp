#pragma once
/*
 * LedFx.h  –  FastLED effect engine for F1 Lamp
 *
 * The LED strip is split into two named segments:
 *   Segment "F" : pixels [0 .. f_count-1]          (default 17 LEDs)
 *   Segment "1" : pixels [f_count .. led_count-1]  (default  5 LEDs)
 *
 * Effects (effect value):
 *   0 = solid        – fill both segments with primary color
 *   1 = pulse        – breathing (sine-wave brightness)
 *   2 = spinner      – rotating half-strip colored segment
 *   3 = strobe       – rapid on/off flash
 *   4 = alt_letters  – alternate F-segment and 1-segment between two colors;
 *                      phase A: F=colorA / 1=colorB, phase B: F=colorB / 1=colorA
 *
 * LED_PIN and MAX_LEDS come from platformio.ini build_flags.
 * Actual LED count is set at runtime via ledfx_init() / ledfx_setCount().
 * The F-segment size is set via ledfx_setFCount().
 */

#include <stdint.h>
#include "F1NetWork.h"   /* F1NetState enum */

/* Initialise FastLED.  Must be called once in setup() after config is loaded. */
void ledfx_init(uint16_t count, uint16_t f_count, uint8_t brightness);

/* Apply the effect defined in g_cfg.states[state] (including auto-revert). */
void ledfx_applyState(F1NetState state);

/* Override current effect (used by web UI force-state and direct edits).
   Resets auto-revert and secondary color. */
void ledfx_setEffect(uint8_t effect, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t speed);

/* Update global brightness (1–255). */
void ledfx_setBrightness(uint8_t bri);

/* Update active LED count (≤ MAX_LEDS).  Clears any extra pixels to black. */
void ledfx_setCount(uint16_t count);

/* Update the number of LEDs that form the "F" letter segment. */
void ledfx_setFCount(uint16_t f_count);

/* Advance animation by one frame.  Call every loop() iteration. */
void ledfx_tick();

/* Immediately clear all LEDs to black and show (power-off state). */
void ledfx_allOff();

/* Temporarily override the current effect with a strobe in the given colour
   for `durationMs` milliseconds, then automatically restore.
   Safe to call from loop() context.  Resets any active flash timer if called again. */
void ledfx_flashEffect(uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs);

/* Directly set the strip to show N of 5 "start lights" (red).
   phase 0 = all off, phase 1–5 = progressively more LEDs lit.
   Calls FastLED.show() immediately (bypasses tick). */
void ledfx_setStartLightsPhase(uint8_t phase);
