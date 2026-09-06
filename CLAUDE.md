# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this project is

F1 Lamp is a standalone ESP32-C3 firmware that drives a WS2812B LED strip (data on `LED_PIN`, default GPIO 2) to visualise live Formula 1 session state. The device connects to `livetiming.formula1.com` over a SignalR-Core WebSocket (WSS) and changes the LEDs to match what is happening on track — safety car, red flag, chequered flag, fastest lap, DRS, etc. It is **not** built on top of WLED; the `wled/` folder name is a historical artefact.

The full user-facing feature set (states, effects, REST API, calendar) is documented in `README.md`. Anything contradicting `README.md` is wrong — `DEPLOY.md` describes the old WLED-usermod workflow and is stale.

## Build, flash, and test commands

All work happens inside `wled/`. The PlatformIO environments are:

| Env | Purpose | Command |
|---|---|---|
| `esp32c3_f1lamp` | The firmware itself (default) | `pio run -e esp32c3_f1lamp` |
| `native_test` | Host-side unit tests (Unity, C++17) | `pio test -e native_test` |

Common invocations from `wled/`:

```bash
# Build firmware only
pio run -e esp32c3_f1lamp

# Build and flash (replace COMx with your port; auto-detect works on most systems)
pio run -e esp32c3_f1lamp --target upload --upload-port COMx

# Serial monitor (115200 baud — see config in platformio.ini)
pio device monitor --port COMx --baud 115200

# Run host-side unit tests
pio test -e native_test
```

VS Code: the PlatformIO extension picks up `wled/` automatically. The build task in `wled/.vscode/tasks.json` is named "Build: binary only".

First build downloads ~500 MB of toolchain and takes 5–10 min. Subsequent builds are ~30–60 s.

If the upload fails with "Failed to connect", enter bootloader mode: hold **BOOT**, press **RESET**, release **BOOT**, then retry.

## High-level architecture

Three concurrent execution contexts share the device, and they must never block each other:

1. **`loop()` in `main.cpp`** — runs on the Arduino main task. Owns the F1-state ring buffer, the start-lights state machine, the race-week brightness ramp, the LED tick (`ledfx_tick()`), and the WiFi watchdog.
2. **`f1net` FreeRTOS task** — created in `setup()` with a 20 KB stack. Runs `f1net_loop()` every 10 ms. Owns the persistent mbedTLS+WebSocket connection to the F1 timing server, parses SignalR frames, and pushes state changes / event notifications back to `loop()` via `volatile` flags and the `g_stateQueue[]` ring buffer (capacity 8). No FreeRTOS primitives (queues/mutexes) are used between the two tasks — uint8_t counters are naturally atomic on the RISC-V core.
3. **AsyncWebServer** — runs on the lwIP task. The web server, REST API, and LittleFS HTML UI live here. UI callbacks into `loop()` are also done via `volatile` flags (`g_pendingFlash*`, `g_forcedSt`).

### File layout (`wled/src/`)

| File | Role |
|---|---|
| `main.cpp` | `setup()` / `loop()`, state queue, start-lights SM, WiFi/AP management, deep-sleep, `f1net` task creation |
| `Config.{h,cpp}` | `AppConfig` + `StateEffect[10]` persisted to LittleFS `/config.json`; `g_cfg` is the global instance |
| `F1NetWork.{h,cpp}` | SignalR-Core WebSocket client over mbedTLS, event ring buffer, state-change callbacks |
| `F1Sessions.{h,cpp}` | HTTPS fetch of the F1 session index from `livetiming.formula1.com/static/{year}/Index.json`; also handles `.jsonStream` download for replay |
| `F1Calendar.{h,cpp}` | Jolpica/Ergast-compatible calendar fetch + race-week idle-brightness ramp + deep-sleep duration helpers |
| `LedFx.{h,cpp}` | FastLED engine: solid, pulse, spinner, strobe, alt_letters, checkered sweep, start-lights phases, flash override |
| `Replay.{h,cpp}` | Replay engine that consumes a `replay.json` and replays it through the same `F1NetStateCB` / `F1EventCB` callbacks as live data |
| `WebUI.{h,cpp}` | `ESPAsyncWebServer` routes (REST API) + serves `wled/data/*.html` from LittleFS |
| `F1StringUtils.h` | Header-only: `f1_url_encode`, `f1_json_str` (no Arduino deps → host-testable) |
| `F1TimeUtils.h` | Header-only: portable `f1_timegm`, `f1_parseUtc`, `f1_weekendWindowActive`, `f1_idleBrightnessFactor` |
| `F1EventLog.h` | Header-only templated ring buffer (compile-time capacity) |
| `mbedtls_*.c` | A handful of mbedTLS source files compiled into the firmware |
| `mbedtls_ssl_enable.h` | Re-enables `MBEDTLS_SSL_TLS_C` / `MBEDTLS_SSL_CLI_C` and TLS 1.2 / ECDHE cipher suites that the SDK ships with `CONFIG_MBEDTLS_TLS_DISABLED=1` |

### Data flow

```
F1 SignalR server ──TLS──▶ f1net task ──volatile flags/queue──▶ loop()
                                                                   │
                                                  ledfx_applyState()
                                                                   ▼
                                                       FastLED (GPIO 2)

WebUI (AsyncWebServer) ──REST callbacks──▶ loop()  (force state, test events, config save)
```

The `F1NetState` enum is the lingua franca: producer (f1net) and consumer (loop / ledfx) both speak it. F1 states 0–9 are persisted as `StateEffect`s in `g_cfg.states[]`, index-aligned with the enum (0=IDLE, 1=SESSION_START, 2=GREEN, 3=YELLOW, 4=SAFETY_CAR, 5=VIRTUAL_SC, 6=RED_FLAG, 7=CHEQUERED, 8=VSC_ENDING, 9=SC_ENDING).

## Critical constraints (do not break)

### `F1NetWork.cpp` must not include `ESPAsyncWebServer.h`

`F1NetWork.cpp` uses raw mbedTLS + lwIP sockets, NOT `esp_tls`, because the SDK has TLS compiled out (`CONFIG_MBEDTLS_TLS_DISABLED=1`). The same mbedTLS sources in `mbedtls/library/` are compiled into the firmware and the corresponding macros are re-enabled via `mbedtls_ssl_enable.h` (included via `MBEDTLS_USER_CONFIG_FILE`).

`F1NetWork.h` deliberately exposes a **pure C interface** (typedefs, function pointers) so that headers from esp-idf's `http_parser` cannot leak in. If you need to add HTTP/WebSocket behaviour, do it via lwIP `send`/`recv` on the existing `TlsConn::sock` (or open a new `lwip_socket`) — **never** `#include <ESPAsyncWebServer.h>` here, and **never** `#include <WebServer.h>`. The reason: `ESPAsyncWebServer` defines `HTTP_GET`, `HTTP_POST`, etc. as bare C enum values that collide with the same identifiers in esp-idf's HTTP parser, producing confusing compile errors.

`F1Sessions.cpp` follows the same rule (raw mbedTLS, no ESPAsyncWebServer).

### TLS configuration is a moving target

The F1 live-timing endpoint moved from classic SignalR (`/signalr/`) to SignalR Core (`/signalrcore/`) in 2026. The protocol constants in `F1NetWork.cpp` (`SR_NEGOTIATE`, `SR_CONNECT`, frame types, `0x1E` record separator) are based on the current SignalR Core spec. If they break again, this is the file to update.

`F1NetWork.cpp` also explicitly does **not** verify the server certificate (`mbedtls_ssl_conf_authmode(... MBEDTLS_SSL_VERIFY_NONE)`). Don't "fix" this without first checking that adding CA bundle storage fits the flash budget.

### Deep sleep has two paths

When `g_cfg.deep_sleep` is on, `setup()` runs a *minimal* early boot: STA-only WiFi (no AP, no LED init, no web server), NTP sync, then either go straight back to `esp_deep_sleep_start()` or fall through to the full boot path. After race weekend ends, `loop()` re-checks every 60 s and sleeps again. If you modify `setup()` or `loop()`, preserve the order: `f1cal_update()` → `f1cal_weekendActive()` → `f1cal_sleepSeconds()` is the contract.

### State queue: drain one entry per loop tick, preserve order

The `g_stateQueue[]` ring buffer in `main.cpp` exists so a fast burst (Yellow → Green within one broadcast delay window) shows in the right order. Each entry has its own `tsMs`; the consumer waits for `millis() - tsMs >= g_cfg.delay_s * 1000` before applying. The default delay is 40 s. If `sqFull()`, drop the **oldest** entry, not the newest — the latest live state is what users actually want to see.

### Flash override is non-reentrant

`ledfx_flashEffect()` saves the active effect, paints a strobe, and auto-restores when its duration expires. Calling it again resets the timer and saves the *current* (possibly mid-flash) state, so chained flashes can lose the original effect — but the API was designed this way to keep it simple. Don't add reentrancy unless the use case demands it.

## Web UI

`wled/data/*.html` is the source of truth for the UI — it gets uploaded to the LittleFS partition at build time. The C++ side in `WebUI.cpp` only handles API routes (`/api/state`, `/api/power`, `/api/brightness`, `/api/effects`, `/api/features`, `/api/test_event`, `/api/reboot`, etc., per `README.md`) and an inline fallback WiFi-setup page used only when the LittleFS image is missing/corrupted.

`wled/data/replay.json` is the static replay file used by `Replay.cpp` for offline testing.

## Testing

The `native_test` PlatformIO environment compiles `wled/src/` with C++17 and links Unity. The header-only utilities (`F1StringUtils.h`, `F1TimeUtils.h`, `F1EventLog.h`) are designed to be testable on the host with no ESP32 SDK. If you add a new pure-C++ helper, put it in one of those headers (or add a similar one) and write a test under `wled/test/`.

To run: `cd wled && pio test -e native_test`. No test files currently exist in the repo, so this runs zero tests — that is a known gap, not a bug.

## Things that are *not* obvious from the code

- **Why the file is named `wled/`**: legacy from an earlier WLED-based usermod build. The folder now hosts a standalone PlatformIO project. The `.envrc` and `wled/.vscode/tasks.json` are leftovers from that era.
- **Why `mbedtls_*.c` files are committed**: the SDK ships `libesp-tls.a` as a precompiled archive with `CONFIG_MBEDTLS_TLS_DISABLED=1`, which `#undef`s the SSL macros. The precompiled archive still references the `mbedtls_ssl_*` functions, so we recompile just the bits we need from the upstream mbedTLS source. The `mbedtls_ssl_enable.h` user config re-enables the macros on the second pass through `esp_config.h`.
- **Why the AP is always on**: `F1-Lamp` (pass `f1lamp123`) is broadcast regardless of STA state, so a user can always reach the device at `http://192.168.4.1` even if their home WiFi is down. The mDNS name `f1lamp.local` only resolves when STA is connected.
- **Race-week idle ramp**: only applies when the device is in `F1ST_IDLE`. Fades from 5 % of configured brightness at 7 days out up to 100 % on race day. Driven by `f1cal_idleFactor()` and applied inside `loop()` to `ledfx_setBrightness()`.
- **`tmp_*.txt` / `tmp_*.html` files**: transient captures from manual SignalR session debugging (see `test_signalr.ps1` for the PowerShell that creates `tmp_f1page.html`). Safe to delete; not part of the build.
