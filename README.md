# F1 Lamp

A standalone ESP32-C3 firmware that turns an LED strip into a real-time Formula 1 session indicator.  
The device connects to the official F1 live-timing feed and changes the LEDs based on what is happening on track — no cloud service, no Home Assistant, no extra software required.

---

## Features

### Session States
The lamp reflects the current F1 session state with a distinct colour and effect:

| State | Colour | Effect |
|---|---|---|
| Idle / Off-season | White | Slow pulse |
| Race Weekend Approaching | White → increases over 7 days | Slow pulse |
| Practice | Blue | Spinner |
| Qualifying | Teal | Pulse |
| Sprint Qualifying | Cyan | Pulse |
| Sprint Race | Orange | Spinner |
| Race | Red | Spinner |
| Safety Car | Yellow | Strobe |
| Red Flag | Red | Strobe |
| Session Start | Red | Solid (triggers start-lights sequence) |
| Chequered Flag | Green | Pulse (triggers winner rainbow) |

Each colour, brightness, and effect is individually configurable from the web UI.

### Live Event Features
Triggered in real-time from official Race Control messages:

| Feature | Trigger | Visual | Default |
|---|---|---|---|
| **Winner Celebration** | Chequered flag | Rainbow spin for full duration | On |
| **Fastest Lap** | "FASTEST LAP" Race Control message | 3 s purple strobe | On |
| **DRS Open** | "DRS ENABLED" Race Control message | 0.8 s white flash | Off |
| **Start Lights** | Session start | 5 red segments add one-by-one, then lights-out | On |

Each feature can be individually enabled or disabled from the web UI, and all four can be tested with a single button click — no live session needed.

### Race Week Idle Ramp
During the 7 days before a race, the idle-state brightness automatically ramps from 5 % up to 100 %, peaking on race day.  The 2026 calendar is built-in (no network fetch required).

### Power Toggle
The LEDs can be turned completely off from the web UI.  The setting is persisted to flash and survives reboots.

---

## Hardware

| Part | Notes |
|---|---|
| **ESP32-C3 SuperMini** | Any ESP32-C3 board works; adjust `upload_port` in `platformio.ini` |
| **WS2812B (or compatible) LED strip** | Data line → GPIO 2 |
| **5 V power supply** | Size for your strip — WS2812B draws up to 60 mA per LED at full white |

No level shifter is required for short strips (< 1 m).  For longer strips add a 300–500 Ω series resistor on the data line and consider a logic-level shifter.

---

## Web Interface

Navigate to `http://f1lamp.local` (or the device IP) after first boot.

The single-page UI has four cards:

- **Control** — power on/off, manual brightness, force any F1 state for testing, reboot
- **States** — set colour (RGB) + effect (Solid / Pulse / Spinner / Strobe / Alt Letters) for each of the 10 states, plus per-state brightness
- **Live Event Features** — toggle winner celebration, fastest lap flash, DRS flash, start lights; test each feature instantly with the buttons at the bottom
- **Race Calendar** — shows the current round, next race name, date, and how many days away it is

---

## Quick Start

### 1. Build and flash

Prerequisites: [PlatformIO](https://platformio.org) with the VS Code extension, or the PlatformIO CLI.

```bash
cd wled
pio run -e esp32c3_f1lamp --target upload --upload-port <COM_PORT>
```

On Windows, replace `<COM_PORT>` with the port assigned to the ESP32-C3 (e.g. `COM5`).

### 2. Connect to WiFi

On first boot (or when no saved network is found) the device starts a WiFi access point named **F1-Lamp-Setup**.  
Connect to it, open `http://192.168.4.1`, enter your SSID and password, and save.  
The device restarts and joins your network.

### 3. Open the UI

```
http://f1lamp.local        # mDNS — works on most systems without knowing the IP
http://<device-ip>         # fallback if mDNS is not available
```

---

## Configuration

All settings are written to `/config.json` on the device's SPIFFS partition.  
Changes made in the UI take effect immediately and are persisted across reboots.

| Setting | Default |
|---|---|
| LED count | 22 |
| F1 LED count (for state display) | 17 |
| Brightness | 128 |
| Power | On |
| feat_winner | On |
| feat_fastest_lap | On |
| feat_drs | Off |
| feat_start_lights | On |

---

## REST API

| Method | Path | Body / Params | Description |
|---|---|---|---|
| `GET` | `/api/state` | — | Current state, brightness, power, next race |
| `POST` | `/api/state` | `?state=N` | Force F1 state 0-9 (`255` = clear) |
| `GET` | `/api/power` | — | `{"power": true\|false}` |
| `POST` | `/api/power` | `{"power": true\|false}` | Toggle power |
| `GET` | `/api/brightness` | — | `{"brightness": 0-255}` |
| `POST` | `/api/brightness` | `{"brightness": N}` | Set brightness |
| `GET` | `/api/effects` | — | Full effects/states config JSON |
| `POST` | `/api/effects` | JSON body | Save effects/states config |
| `GET` | `/api/features` | — | `{winner, fastest_lap, drs, start_lights}` |
| `POST` | `/api/features` | `{"winner": true}` | Toggle a single feature |
| `POST` | `/api/test_event` | `?ev=N` | Inject test event: `0`=winner `1`=fastest lap `2`=DRS `3`=start lights |
| `POST` | `/api/reboot` | — | Restart the device |

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│  ESP32-C3                                                   │
│                                                             │
│  ┌──────────────┐  FreeRTOS task  ┌─────────────────────┐  │
│  │  F1NetWork   │ ─────────────▶  │  main loop()        │  │
│  │  (SignalR    │  state change   │  • pending state    │  │
│  │  WebSocket)  │  event flash    │  • flash events     │  │
│  └──────────────┘                 │  • start-lights SM  │  │
│                                   │  • brightness ramp  │  │
│  ┌──────────────┐                 └──────────┬──────────┘  │
│  │  WebUI       │  REST callbacks             │             │
│  │  (AsyncWeb   │ ─────────────────────────▶  │             │
│  │   Server)    │                             ▼             │
│  └──────────────┘                 ┌─────────────────────┐  │
│                                   │  LedFx              │  │
│  ┌──────────────┐                 │  (FastLED, GPIO 2)  │  │
│  │  F1Calendar  │ ─── ramp ──▶    └─────────────────────┘  │
│  │  (static 26) │                                          │
│  └──────────────┘                                          │
└─────────────────────────────────────────────────────────────┘
```

**F1NetWork** runs in its own FreeRTOS task and connects to `livetiming.formula1.com` on port 80 via a SignalR WebSocket.  It subscribes to `TrackStatus`, `SessionStatus`, `Heartbeat`, and `RaceControlMessages`, then fires callbacks into the main loop via `volatile` flags — no RTOS primitives needed.

**LedFx** owns the FastLED state.  Flash overrides (fastest lap, DRS) save the current effect, apply a colour strobe, then auto-restore when the flash duration expires — all inside `ledfx_tick()`.

HTTPS is intentionally not used: the ESP32-C3's TLS stack is large and the F1 live-timing endpoint is already plain HTTP.

---

## Project Structure

```
wled/
├── platformio.ini
└── src/
    ├── main.cpp          # Setup + loop, state machine, feature dispatch
    ├── Config.h/cpp      # Persistent config (SPIFFS /config.json)
    ├── F1NetWork.h/cpp   # SignalR WebSocket client, state + event callbacks
    ├── F1Calendar.h/cpp  # Static 2026 race calendar, race-week ramp logic
    ├── LedFx.h/cpp       # FastLED effects, flash override, start-lights phases
    └── WebUI.h/cpp       # ESPAsyncWebServer, REST API, self-contained HTML/CSS/JS UI
```

---

## License

MIT

