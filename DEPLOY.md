# F1 Lamp — Deploy & Operations

Standalone ESP32-C3 firmware (PlatformIO) that drives a WS2812B LED strip
(data on `LED_PIN`, default GPIO 2) to visualise live Formula 1 session
state. It subscribes to the F1 live-timing feed
(`livetiming.formula1.com`, SignalR Core over WSS), turns the LEDs into the
current flag/session state, and ships a small web UI + REST API served from
LittleFS.

> The `wled/` folder name is a historical artefact — this is **not** WLED.
> See `README.md` for the user-facing feature set; `CLAUDE.md` for
> architecture notes that must stay true.

---

## Repository layout

| Path | What it is |
|---|---|
| `wled/` | PlatformIO project (firmware `src/`, web UI `data/`) |
| `mbedtls/` | **git submodule** → upstream mbedTLS pinned at `v2.28.8` (the SDK ships TLS compiled out; a few `mbedtls/library/*.c` are recompiled into the firmware — see `wled/src/mbedtls_*.c` wrappers and `mbedtls_ssl_enable.h`) |
| `.gitmodules` | submodule registration |

### First clone

```bash
git clone <repo-url>
git submodule update --init   # populates mbedtls/ (needed to build!)
```

---

## Prerequisites

- PlatformIO Core (`pio`) — the project uses PlatformIO (espressif32 platform).
- An ESP32-C3 board with native USB (CDC) — appears as `/dev/cu.usbmodem*`.
- First build downloads the toolchain (~500 MB) and takes 5–10 min;
  later builds ~30–60 s.

### Toolchain note (Apple Silicon)

The firmware links precompiled ESP-IDF SDK libs (built with GCC 8.4 + newlib)
together with the Espressif GCC 14.x (picolibc) toolchain. That hybrid has
real quirks:

- `wled/src/toolchain_compat.c/.cpp` provide link/runtime shims
  (`_Unwind_SetEnableExceptionFdeSorting`, `_cleanup_r`, `__atomic_add`,
  `__exchange_and_add`) — **do not delete them; the build fails without them**.
- The C library clock functions are unreliable across the hybrid (64-bit
  `time_t` headers vs 32-bit newlib returns corrupt comparisons). All
  calendar math lives in `F1TimeUtils.h` as pure 32-bit arithmetic;
  `f1_clock()` masks the libc clock. Do **not** “simplify” device code back
  to `mktime`/`gmtime_r`.

---

## Build, flash, monitor

All commands run from `wled/`:

```bash
# Build firmware
pio run -e esp32c3_f1lamp

# Build + flash (auto-detect port, or pass --upload-port /dev/cu.usbmodemXXXX)
pio run -e esp32c3_f1lamp --target upload

# Serial monitor (115200 baud)
pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200

# Host-side unit tests (currently zero tests — known gap)
pio test -e native_test
```

If upload fails with “Failed to connect”: hold **BOOT**, tap **RESET**,
release **BOOT**, retry.

### Web UI filesystem (LittleFS)

The web UI lives in `wled/data/*` and is baked into the LittleFS partition
with:

```bash
pio run -e esp32c3_f1lamp -t uploadfs
```

> ⚠️ `uploadfs` **wipes the whole LittleFS partition**, including the saved
> `/config.json` (WiFi credentials, LED config, delay, feature flags). Prefer
> pushing single files with the OTA file endpoint (below), which does not
> touch other files.

### OTA / file updates over the network

Firmware OTA: web UI **Settings → Firmware Update**, or

```bash
curl -F "firmware=@.pio/build/esp32c3_f1lamp/firmware.bin" http://<ip>/api/update
```

Web assets (html/css/js/json) can be replaced one at a time without wiping the
filesystem:

```bash
curl -F "file=@index.html" "http://<ip>/api/file?name=/index.html"
```

(Allows only `.html/.css/.js/.json`, rejects `..`.)

---

## Network model

- The **AP `F1-Lamp` (password `f1lamp123`) is always on** → the device is
  always reachable at `http://192.168.4.1`, even without home WiFi.
- When saved STA credentials exist it also joins that network; mDNS
  `f1lamp.local` resolves only in STA mode.
- Live timing requires STA.

## Configuration

Saved on LittleFS as `/config.json` (see `wled/src/Config.*`):

- WiFi (`ssid`, `pass`)
- LED hardware: `led_count`, `f_count` (LEDs forming the “F”), `brightness`
- `power`, `delay_s` (event→LED delay, default 40 s)
- Feature flags: fastest-lap flash, DRS flash, start lights, deep sleep
- **LED reaction switches** (`react_track`, `react_global`, `react_sector`):
  - `react_track`  — react to TrackStatus codes (ON)
  - `react_global` — react to global race-control messages: RED FLAG, SAFETY
    CAR, VSC, CHEQUERED (ON) — these are what the TV mirrors (TrackStatus in
    this feed can stay “2/Yellow” through SC/red periods)
  - `react_sector` — treat “YELLOW IN TRACK SECTOR x” notes as yellow (OFF)
- Per-state effects (`states[]`, index = `F1NetState`)

Toggles live on the **Features** page; the delay slider is on **Settings**.

## Live-timing behaviour

- Connects to `livetiming.formula1.com/signalrcore` (SignalR Core JSON,
  record separator `0x1E`), subscribes to `TrackStatus`, `SessionStatus`,
  `Heartbeat`, `RaceControlMessages`.
- The SignalR handshake and every protocol message must be sent as **masked
  WebSocket text frames** — sending raw bytes makes the server close with
  code 1002.
- On (re)connect the snapshot sets the current LED state; stale queued events
  are dropped so only the *latest* state applies. The most recent
  race-control messages from the snapshot backlog restore the correct flag if
  the device boots mid-incident. Snapshot values are **not** written to the
  event log (they would look like events that happened at boot time).
- Events are logged (up to 40) and shown in the UI; logging is independent of
  the reaction switches.

## Calendar / sessions

- **Next-race card**: fetched from the f1calendar CDN feed
  (`cdn.jsdelivr.net/gh/sportstimes/f1@main/_db/f1/<year>.json`, fallback to
  `raw.githubusercontent.com`), with a built-in 2026 table as last resort.
- **Sessions page**: also uses the CDN calendar feed (all meetings, all
  sessions: FP1/FP2/FP3, Sprint, Qualifying, Race), prefetched at boot before
  the live WebSocket allocates its TLS buffers.
  - Known limitation: replay of a specific historical session needs the
    livetiming `Index.json` stream paths; that endpoint intermittently
    truncates large responses on this device, so per-session replay buttons
    are currently disabled. The built-in demo replay still works.
- Deep sleep: when enabled, the device wakes periodically, checks whether the
  next race weekend is near, and sleeps otherwise.

## Troubleshooting

- Serial markers: `[F1Net]` connection phases (`wifi`, `connecting`, `live`,
  …); `[F1Cal]`, `[F1Sess]`, `[F1Lamp]` for their subsystems.
- `GET /api/status` → `f1phase`, `f1err`, `state`, `connected`.
- `GET /api/sessions_debug?reset=1` resets a failed session fetch.
- Do not include `ESPAsyncWebServer.h` in `F1NetWork.cpp`/`F1Sessions.cpp`
  (enum collisions with the IDF HTTP parser). Raw mbedTLS + lwIP sockets only.
- The firmware intentionally does not verify the server certificate
  (`MBEDTLS_SSL_VERIFY_NONE`).
