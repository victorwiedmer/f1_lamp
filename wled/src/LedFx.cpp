/*
 * LedFx.cpp  –  FastLED effect engine
 */

#include "LedFx.h"
#include "Config.h"
#include <FastLED.h>
#include <Arduino.h>

#ifndef LED_PIN
  #define LED_PIN 8
#endif
#ifndef MAX_LEDS
  #define MAX_LEDS 300
#endif

/* ── strip state ─────────────────────────────────────────────────────────── */
static CRGB     s_leds[MAX_LEDS];
static uint16_t s_count   = 22;
static uint16_t s_f_count = 17;   /* LEDs 0..(f_count-1) = "F" segment       */

/* ── active effect params ────────────────────────────────────────────────── */
static uint8_t s_effect  = 0;
static CRGB    s_color   = CRGB::Black;   /* primary / segment-A color       */
static CRGB    s_color2  = CRGB::Black;   /* secondary / segment-B color     */
static uint8_t s_speed   = 50;

/* ── auto-revert timer ────────────────────────────────────────────────────── */
static uint32_t   s_revertAt    = 0;            /* millis() target, 0=none  */
static F1NetState s_revertState = F1ST_IDLE;

/* ── animation state ─────────────────────────────────────────────────────── */
static uint32_t s_lastMs  = 0;
static uint8_t  s_phase   = 0;    /* 0-255 phase for pulse / strobe / alt    */
static uint16_t s_spinPos = 0;    /* leading edge of spinner (pixel index)   */

/* ── flash override (temporary effect, auto-reverts) ───────────────────── */
static uint32_t s_flashEndMs     = 0;         /* 0 = no flash active            */
static uint8_t  s_preFlashEff    = 0;
static CRGB     s_preFlashColorA = CRGB::Black;
static CRGB     s_preFlashColorB = CRGB::Black;
static uint8_t  s_preFlashSpeed  = 50;
static uint8_t  s_preFlashPhase  = 0;

/* ── helpers ──────────────────────────────────────────────────────────────── */
static inline void clearExtra() {
    if (s_count < MAX_LEDS)
        fill_solid(s_leds + s_count, MAX_LEDS - s_count, CRGB::Black);
}

/* ── public API ───────────────────────────────────────────────────────────── */

void ledfx_init(uint16_t count, uint16_t f_count, uint8_t brightness) {
    s_count   = (count   > MAX_LEDS) ? MAX_LEDS : count;
    s_f_count = (f_count > s_count)  ? s_count  : f_count;
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(s_leds, MAX_LEDS)
           .setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(brightness);
    fill_solid(s_leds, MAX_LEDS, CRGB::Black);
    FastLED.show();
    Serial.printf("[LedFx] init  pin=%d count=%d f_count=%d bri=%d\n",
                  LED_PIN, s_count, s_f_count, brightness);
}

void ledfx_applyState(F1NetState state) {
    int idx = (int)state;
    if (idx < 0 || idx >= CFG_NUM_STATES) idx = 0;
    const StateEffect& se = g_cfg.states[idx];

    s_effect  = se.effect;
    s_color   = CRGB(se.r,  se.g,  se.b);
    s_color2  = CRGB(se.r2, se.g2, se.b2);
    s_speed   = (se.speed < 1) ? 1 : se.speed;
    s_phase   = 0;
    s_spinPos = 0;
    s_lastMs  = 0;

    if (se.revert_s > 0) {
        s_revertAt    = millis() + (uint32_t)se.revert_s * 1000UL;
        s_revertState = F1ST_IDLE;
    } else {
        s_revertAt = 0;
    }
}

void ledfx_setEffect(uint8_t effect, uint8_t r, uint8_t g, uint8_t b,
                     uint8_t speed) {
    s_effect  = effect;
    s_color   = CRGB(r, g, b);
    s_color2  = CRGB::Black;
    s_speed   = (speed < 1) ? 1 : speed;
    s_phase   = 0;
    s_spinPos = 0;
    s_lastMs  = 0;
    s_revertAt = 0;   /* manual overrides cancel any pending revert */
}

void ledfx_setBrightness(uint8_t bri) {
    FastLED.setBrightness(bri);
}

void ledfx_setCount(uint16_t count) {
    fill_solid(s_leds, MAX_LEDS, CRGB::Black);
    s_count   = (count > MAX_LEDS) ? MAX_LEDS : count;
    if (s_f_count > s_count) s_f_count = s_count;
    FastLED.show();
}

void ledfx_setFCount(uint16_t f_count) {
    s_f_count = (f_count > s_count) ? s_count : f_count;
}

void ledfx_allOff() {
    fill_solid(s_leds, MAX_LEDS, CRGB::Black);
    FastLED.show();
}

void ledfx_flashEffect(uint8_t r, uint8_t g, uint8_t b, uint16_t durationMs) {
    /* Save current effect the first time (don't overwrite if already flashing) */
    if (s_flashEndMs == 0) {
        s_preFlashEff    = s_effect;
        s_preFlashColorA = s_color;
        s_preFlashColorB = s_color2;
        s_preFlashSpeed  = s_speed;
        s_preFlashPhase  = s_phase;
    }
    /* Apply strobe in the requested colour */
    s_effect = 3;  /* strobe */
    s_color  = CRGB(r, g, b);
    s_speed  = 220;  /* fast flash */
    s_phase  = 0;
    s_flashEndMs = millis() + durationMs;
}

void ledfx_setStartLightsPhase(uint8_t phase) {
    /* phase 0 = all off; 1-5 = progressively more LEDs lit red */
    fill_solid(s_leds, MAX_LEDS, CRGB::Black);
    if (phase >= 1 && phase <= 5 && s_count > 0) {
        /* Divide strip into 5 equal segments, light `phase` of them */
        uint16_t litCount = (uint16_t)(((uint32_t)s_count * phase + 4) / 5);
        if (litCount > s_count) litCount = s_count;
        fill_solid(s_leds, litCount, CRGB(220, 0, 0));
    }
    FastLED.show();
}

/* ── tick – called every loop() ──────────────────────────────────────────── */
void ledfx_tick() {
    uint32_t now = millis();
    if (now - s_lastMs < 20) return;   /* cap at ~50 fps */
    s_lastMs = now;

    /* ── Flash restore: revert to saved effect when flash timer expires ── */
    if (s_flashEndMs > 0 && now >= s_flashEndMs) {
        s_flashEndMs = 0;
        s_effect  = s_preFlashEff;
        s_color   = s_preFlashColorA;
        s_color2  = s_preFlashColorB;
        s_speed   = s_preFlashSpeed;
        s_phase   = s_preFlashPhase;
    }

    /* segment boundaries */
    uint16_t f_end = s_f_count;                         /* exclusive end of F */
    uint16_t o_end = s_count;                           /* exclusive end of 1 */

    switch (s_effect) {

        /* ── 0: solid ────────────────────────────────────────────────────── */
        case 0:
            fill_solid(s_leds,       f_end,          s_color);
            fill_solid(s_leds + f_end, o_end - f_end, s_color);
            clearExtra();
            break;

        /* ── 1: pulse (breathing) ────────────────────────────────────────── */
        case 1: {
            uint8_t inc = (uint8_t)((s_speed * 25UL) / 255 + 1);
            s_phase += inc;
            uint8_t bright = scale8(sin8(s_phase), 220) + 35;
            CRGB c = s_color;
            c.nscale8_video(bright);
            fill_solid(s_leds, s_count, c);
            clearExtra();
            break;
        }

        /* ── 2: spinner (rotating half-strip) ────────────────────────────── */
        case 2: {
            if (s_count == 0) break;
            uint8_t inc = (uint8_t)((s_speed * 3UL) / 255 + 1);
            s_spinPos = (s_spinPos + inc) % s_count;
            fill_solid(s_leds, s_count, CRGB::Black);
            uint16_t segLen = s_count / 2 ? s_count / 2 : 1;
            for (uint16_t i = 0; i < segLen; i++)
                s_leds[(s_spinPos + i) % s_count] = s_color;
            clearExtra();
            break;
        }

        /* ── 3: strobe ───────────────────────────────────────────────────── */
        case 3: {
            uint8_t inc = (uint8_t)((s_speed * 20UL) / 255 + 1);
            s_phase += inc;
            CRGB c = (s_phase < 128) ? s_color : CRGB::Black;
            fill_solid(s_leds, s_count, c);
            clearExtra();
            break;
        }

        /* ── 4: alt_letters ──────────────────────────────────────────────── */
        /*  Phase 0-127 → F=colorA, 1=colorB
         *  Phase 128-255 → F=colorB, 1=colorA  (swapped)
         *  speed 1-255 maps to alternation rate; lower speed = slower swap   */
        case 4: {
            if (o_end <= f_end) {
                /* degenerate: no split point, fall back to solid */
                fill_solid(s_leds, s_count, s_color);
                clearExtra();
                break;
            }
            /* phase increment: speed=70 → ~2/frame → ~64 frames/half = 1.3 s */
            uint8_t inc = (uint8_t)((s_speed * 8UL) / 255 + 1);
            s_phase += inc;
            bool swap = (s_phase >= 128);
            CRGB colorF = swap ? s_color2 : s_color;
            CRGB color1 = swap ? s_color  : s_color2;
            fill_solid(s_leds,         f_end,          colorF);
            fill_solid(s_leds + f_end, o_end - f_end,  color1);
            clearExtra();
            break;
        }

        /* ── 5: rainbow_spin (winner celebration) ──────────────────────── */
        /* Hue cycles across the whole strip and advances over time. */
        case 5: {
            uint8_t inc = (uint8_t)((s_speed * 3UL) / 255 + 1);
            s_phase += inc;
            uint8_t delta = s_count ? (256 / s_count) : 8;
            fill_rainbow(s_leds, s_count, s_phase, delta);
            clearExtra();
            break;
        }

        default:
            fill_solid(s_leds, MAX_LEDS, CRGB::Black);
            break;
    }

    FastLED.show();

    /* ── auto-revert ─────────────────────────────────────────────────────── */
    if (s_revertAt && millis() >= s_revertAt) {
        s_revertAt = 0;
        ledfx_applyState(s_revertState);   /* applies IDLE (or chosen target) */
    }
}

