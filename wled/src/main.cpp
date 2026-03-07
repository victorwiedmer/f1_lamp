/*
 * main.cpp  –  F1 Lamp standalone firmware
 *
 * Hardware : ESP32-C3, WS2812B LED strip (data on GPIO LED_PIN, default 8)
 * Features : FastLED effects reacting to live F1 track status
 *            Web UI (ESPAsyncWebServer) for WiFi, effects, LED config
 *
 * WiFi strategy:
 *   - AP ("F1-Lamp" / "f1lamp123" / 192.168.4.1) is ALWAYS on
 *   - Also tries to connect to saved home WiFi (STA)
 *   - If STA connects: accessible on home network + via AP
 *   - F1 live-timing only works when STA is connected
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <time.h>
#include <esp_sleep.h>
#include "Config.h"
#include "LedFx.h"
#include "WebUI.h"
#include "F1NetWork.h"
#include "F1Calendar.h"
#include "Replay.h"

/* ── AP mode credentials ────────────────────────────────────────────────── */
static constexpr const char* AP_SSID = "F1-Lamp";
static constexpr const char* AP_PASS = "f1lamp123";

/* ── WiFi timing ─────────────────────────────────────────────────────────── */
static constexpr uint32_t WIFI_TIMEOUT_MS = 20000;

/* ── forced state (0xFF = no override) ──────────────────────────────────── */
static uint8_t g_forcedSt = 0xFF;

/* ── F1 state change callback (called from F1NetWork task) ──────────────── */
static volatile F1NetState g_pendingState = F1ST_IDLE;
static volatile bool       g_pendingValid = false;

static void applyState(F1NetState newState) {
    g_pendingState = newState;
    g_pendingValid = true;
}

static void onF1StateChange(F1NetState newState) {
    if (replay_isActive() || replay_isLoading()) return;  /* replay has the floor */
    applyState(newState);
}

/* ── Pending flash event (written by f1net task, consumed in loop()) ─────── */
static volatile bool     g_pendingFlash   = false;
static volatile uint8_t  g_pendingFlashR  = 0;
static volatile uint8_t  g_pendingFlashG  = 0;
static volatile uint8_t  g_pendingFlashB  = 0;
static volatile uint16_t g_pendingFlashMs = 0;

/* ── Start-lights state machine (loop-driven, non-blocking) ─────────────── */
static uint8_t  g_slPhase  = 0;   /* 0=idle  1-5=count-up  6=lights-out hold */
static uint32_t g_slNextMs = 0;
static void startLightsBegin() {
    g_slPhase  = 1;
    g_slNextMs = millis() + 500;
    ledfx_setStartLightsPhase(1);
}

/* ── F1 live-event callback (called from f1net FreeRTOS task) ────────────── */
static void onF1Event(F1Event ev) {
    switch (ev) {
        case F1EVT_FASTEST_LAP:
            if (g_cfg.feat_fastest_lap) {
                g_pendingFlashR  = 130; g_pendingFlashG = 0; g_pendingFlashB = 255;
                g_pendingFlashMs = 3000; g_pendingFlash = true;
            }
            break;
        case F1EVT_DRS_ENABLED:
            if (g_cfg.feat_drs) {
                g_pendingFlashR  = 255; g_pendingFlashG = 255; g_pendingFlashB = 255;
                g_pendingFlashMs = 800;  g_pendingFlash = true;
            }
            break;
    }
}

/* ── connect to STA WiFi; returns true if connected ─────────────────────── */
static bool connectSTA() {
    if (g_cfg.ssid[0] == '\0') return false;

    /* Scan and log all visible networks before connecting */
    Serial.println("[WiFi] Scanning for networks...");
    int n = WiFi.scanNetworks();
    if (n <= 0) {
        Serial.printf("[WiFi] Scan found %d networks\n", n);
    } else {
        Serial.printf("[WiFi] Scan found %d network(s):\n", n);
        for (int i = 0; i < n; i++) {
            Serial.printf("  [%d] SSID: %-32s  RSSI: %3d dBm  Ch: %2d  Auth: %d\n",
                i + 1,
                WiFi.SSID(i).c_str(),
                WiFi.RSSI(i),
                WiFi.channel(i),
                (int)WiFi.encryptionType(i));
        }
    }
    WiFi.scanDelete();

    /* Try up to 4 times with a pause between attempts, to handle slow APs
       (iPhone hotspot can take several seconds to become visible). */
    for (int attempt = 1; attempt <= 4; attempt++) {
        Serial.printf("[WiFi] Connecting to \"%s\" (attempt %d/4)...\n", g_cfg.ssid, attempt);
        WiFi.disconnect(false);
        delay(500);
        WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK); /* force WPA2 – avoids WPA3 4-way handshake timeout */
        WiFi.begin(g_cfg.ssid, g_cfg.pass);
        WiFi.setTxPower(WIFI_POWER_8_5dBm); /* C3 SuperMini broken antenna */

        uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - t0 > 8000) break;   /* 8 s per attempt */
            ledfx_tick();
            delay(20);
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.printf("[WiFi] STA connected! IP: %s\n", WiFi.localIP().toString().c_str());
            return true;
        }
        Serial.printf("[WiFi] Attempt %d failed (status=%d), retrying...\n", attempt, WiFi.status());
        WiFi.disconnect(false);
        delay(1000);
    }
    Serial.println("[WiFi] All attempts failed – AP-only mode");
    return false;
}

/* ── start AP (always on) ───────────────────────────────────────────────── */
static bool startAP() {
    bool ok = WiFi.softAP(AP_SSID, AP_PASS);
    delay(1000);
    const char* status = ok ? "UP" : "FAILED";
    Serial.printf("[WiFi] AP %s – SSID: %s  IP: %s\n", status, AP_SSID, WiFi.softAPIP().toString().c_str());
    return ok;
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  setup                                                                     */
/* ══════════════════════════════════════════════════════════════════════════ */
/* Helper: log to UART0 (Serial) */
static void LOG(const char* msg) {
    Serial.print(msg);
}
template<typename... Args>
static void LOGF(const char* fmt, Args... args) {
    Serial.printf(fmt, args...);
}

void setup() {
    Serial.begin(115200);
    /* 3-second delay so USB Serial/JTAG has time to re-enumerate after reset */
    delay(3000);
    LOG("\n[F1Lamp] Booting...\n");

    /* 0. Stop firmware from writing WiFi credentials to NVS (we use LittleFS) */
    WiFi.persistent(false);
    WiFi.mode(WIFI_OFF);
    delay(200);

    /* 1. Load config from LittleFS */
    cfg_init();

    /* AP+STA mode – connect STA first so AP inherits the same channel */
    WiFi.mode(WIFI_AP_STA);
    delay(100);
    WiFi.setTxPower(WIFI_POWER_8_5dBm ); /* C3 SuperMini broken antenna – set early so AP beacon is also low-power */

    /* 2. Initialise LED strip with saved settings */
    ledfx_init(g_cfg.led_count, g_cfg.f_count, g_cfg.brightness);

    /* Immediately show idle (dim red) as first visual feedback */
    ledfx_applyState(F1ST_IDLE);
    ledfx_tick();

    /* Slow dim blue during WiFi connect phase */
    ledfx_setEffect(1 /* pulse */, 0, 0, 55, 18);
    ledfx_tick();

    /* 4. Connect STA first (channel is determined by the router) */
    bool sta = connectSTA();

    /* Now start AP – it will auto-use the same channel as STA */
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(500);

    bool apOk = (WiFi.softAPIP()[0] != 0);
    g_apMode = !sta;
    Serial.printf("[Setup] sta=%d apOk=%d\n", sta, apOk);

    /* 5. Start mDNS – only useful when STA is connected */
    if (sta) {
        if (MDNS.begin("f1lamp")) {
            MDNS.addService("http", "tcp", 80);
            LOG("[mDNS] Responder started – http://f1lamp.local\n");
        } else {
            LOG("[mDNS] Failed to start\n");
        }
    }

    /* 6. Start web server */
    Serial.println("[Setup] Starting WebUI...");
    webui_init(
        /* onForcedState */
        [](uint8_t s) {
            g_forcedSt = s;
            if (s != 0xFF) {
                ledfx_applyState((F1NetState)s);
            }
        },
        /* onReboot */ []() { ESP.restart(); },
        /* onTestEvent  0=winner  1=fastest_lap  2=drs  3=start_lights */
        [](uint8_t ev) {
            g_forcedSt = 0xFF;   // make sure pending system is active
            switch (ev) {
                case 0:  // winner rainbow spin
                    g_pendingState = F1ST_CHEQUERED;
                    g_pendingValid = true;
                    break;
                case 1:  // fastest lap – purple flash
                    g_pendingFlashR  = 130;
                    g_pendingFlashG  = 0;
                    g_pendingFlashB  = 255;
                    g_pendingFlashMs = 3000;
                    g_pendingFlash   = true;
                    break;
                case 2:  // DRS open – white flash
                    g_pendingFlashR  = 255;
                    g_pendingFlashG  = 255;
                    g_pendingFlashB  = 255;
                    g_pendingFlashMs = 800;
                    g_pendingFlash   = true;
                    break;
                case 3:  // start lights sequence
                    g_pendingState = F1ST_SESSION_START;
                    g_pendingValid = true;
                    break;
            }
        }
    );
    Serial.println("[Setup] WebUI started");

    /* 7. Start F1 live-timing in a dedicated FreeRTOS task so blocking
          DNS / TCP calls in negotiate() never stall the lwIP thread or
          prevent the web server from accepting browser connections.         */
    if (sta) {
        f1net_setCallback(onF1StateChange);
        f1net_setEventCallback(onF1Event);
        replay_setCallbacks(applyState, onF1Event);
        f1net_setup();
        static TaskHandle_t s_f1netTask = nullptr;
        xTaskCreate(
            [](void*) {
                uint32_t loopCnt = 0;
                for (;;) {
                    f1net_loop();
                    vTaskDelay(pdMS_TO_TICKS(10));
                    /* Log stack high-water mark every ~60s */
                    if (++loopCnt % 6000 == 0) {
                        UBaseType_t hwm = uxTaskGetStackHighWaterMark(nullptr);
                        Serial.printf("[f1net] stack HWM=%u words (%u bytes free)\n",
                                      hwm, hwm * sizeof(StackType_t));
                    }
                }
            },
            "f1net",   /* task name  */
            20480,     /* stack (bytes) – TlsConn is heap-allocated; 20 KB enough */
            nullptr,
            1,         /* priority 1 (idle+1) – below loop() at priority 1? */
            &s_f1netTask
        );
        Serial.println("[F1Lamp] F1 network task started");
        LOG("[F1Lamp] F1 network task started\n");
    }

    /* 8. Set final effect */
    if (sta) {
        /* 8a. NTP time sync */
        configTime(0, 0, "pool.ntp.org", "time.nist.gov");
        uint32_t ntpT0 = millis();
        while (time(nullptr) < 1577836800UL && millis() - ntpT0 < 10000) {
            ledfx_tick(); delay(100);
        }
        if (time(nullptr) > 1577836800UL) {
            Serial.printf("[NTP] Synced: %lu\n", (unsigned long)time(nullptr));
            f1cal_update();
        } else {
            Serial.println("[NTP] Sync timed out");
        }

        /* 8b. Solid green for 1 s: connected! */
        ledfx_setEffect(0, 0, 180, 0, 50);
        { uint32_t t = millis() + 1000; while (millis() < t) { ledfx_tick(); delay(20); } }
        ledfx_applyState(F1ST_IDLE);
    } else {
        ledfx_setEffect(1 /* pulse */, 0, 0, 220, 160); /* fast breath blue – AP only */
    }

    LOG("[F1Lamp] Ready!\n");
    LOGF("  AP always available: http://192.168.4.1  (SSID: %s)\n", AP_SSID);
    if (sta) {
        LOGF("  Home network: http://%s\n", WiFi.localIP().toString().c_str());
        LOG("  mDNS: http://f1lamp.local\n");
    }

    /* ── Deep sleep: if enabled and outside a race weekend, hibernate ───── */
    if (g_cfg.deep_sleep) {
        bool awake = f1cal_weekendActive();
        if (!awake) {
            uint32_t secs = f1cal_sleepSeconds();
            Serial.printf("[Sleep] Deep sleep for %us (%.1f min)\n",
                          secs, secs / 60.0f);
            /* 1-second white pulse so you can see it's alive before sleeping */
            ledfx_setEffect(0, 12, 12, 12, g_cfg.brightness);
            ledfx_tick();
            delay(1000);
            ledfx_allOff();
            delay(50);
            esp_sleep_enable_timer_wakeup((uint64_t)secs * 1000000ULL);
            esp_deep_sleep_start();   /* never returns */
        }
        Serial.println("[Sleep] Race weekend active – staying awake");
    }
}

/* ══════════════════════════════════════════════════════════════════════════ */
/*  loop                                                                      */
/* ══════════════════════════════════════════════════════════════════════════ */
void loop() {
    /* ── WiFi watchdog: reconnect every 30 s if STA drops ───────────── */
    static uint32_t wifiRetryMs = 0;
    if (!g_apMode && WiFi.status() != WL_CONNECTED) {
        if (millis() - wifiRetryMs > 30000) {
            wifiRetryMs = millis();
            Serial.println("[WiFi] Reconnecting…");
            WiFi.disconnect(false);
            WiFi.setMinSecurity(WIFI_AUTH_WPA2_PSK);
            WiFi.begin(g_cfg.ssid, g_cfg.pass);
            WiFi.setTxPower(WIFI_POWER_8_5dBm);
        }
    }

    /* ── Power state: if off, keep LEDs dark and return early ───────── */
    if (!g_cfg.power) {
        ledfx_allOff();
        delay(50);
        return;
    }

    /* ── Apply any pending F1 state change (feature-aware dispatch) ───── */
    if (g_pendingValid && g_forcedSt == 0xFF) {
        g_pendingValid = false;
        F1NetState st = (F1NetState)g_pendingState;
        if (st == F1ST_SESSION_START && g_cfg.feat_start_lights && g_slPhase == 0) {
            startLightsBegin();             /* start lights countdown        */
        } else if (st == F1ST_CHEQUERED && g_cfg.feat_winner) {
            ledfx_setEffect(5, 0, 0, 0, 200); /* effect 5 = rainbow_spin    */
        } else {
            ledfx_applyState(st);
        }
    } else {
        g_pendingValid = false;
    }

    /* ── Consume pending flash events (flash effect, then auto-restore) ── */
    if (g_pendingFlash) {
        g_pendingFlash = false;
        ledfx_flashEffect(g_pendingFlashR, g_pendingFlashG, g_pendingFlashB,
                          g_pendingFlashMs);
    }

    /* ── Start-lights state machine ─────────────────────────────────────── */
    if (g_slPhase > 0 && millis() >= g_slNextMs) {
        if (g_slPhase < 5) {
            /* Advance to next light */
            g_slPhase++;
            ledfx_setStartLightsPhase(g_slPhase);
            g_slNextMs = millis() + 700;
        } else if (g_slPhase == 5) {
            /* All 5 lit – hold 1.5 s then lights out */
            g_slPhase  = 6;
            g_slNextMs = millis() + 1500;
        } else {
            /* Lights out! */
            g_slPhase = 0;
            ledfx_setStartLightsPhase(0);     /* all off              */
            ledfx_applyState(F1ST_SESSION_START); /* session start anim */
        }
    }

    /* ── Race-week idle brightness ramp ──────────────────────────────── */
    {
        static F1NetState s_lastSt = F1ST_UNKNOWN;
        F1NetState curSt = (g_forcedSt != 0xFF)
            ? (F1NetState)g_forcedSt
            : f1net_getState();
        if (curSt == F1ST_IDLE) {
            float fac = f1cal_idleFactor();
            if (fac > 0.0f && fac < 1.0f) {
                ledfx_setBrightness(
                    max((uint8_t)3, (uint8_t)((float)g_cfg.brightness * fac)));
            } else {
                ledfx_setBrightness(g_cfg.brightness);
            }
        } else if (s_lastSt == F1ST_IDLE) {
            /* Leaving idle – restore full configured brightness */
            ledfx_setBrightness(g_cfg.brightness);
        }
        s_lastSt = curSt;
    }
    /* ── Replay tick ────────────────────────────────────────────────── */
    replay_tick();
    /* ── LED animation tick (skip during start-lights) ─────────────────── */
    if (g_slPhase == 0) ledfx_tick();

    /* ── Periodic calendar refresh (once per day) ────────────────────── */
    {
        static uint32_t s_calMs = 0;
        if (!g_apMode && WiFi.status() == WL_CONNECTED
                && millis() - s_calMs > 86400000UL) {
            s_calMs = millis();
            f1cal_update();
        }
    }
}
