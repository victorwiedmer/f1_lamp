# F1 Lamp — Deployment Guide (Windows)

Everything you need to go from zero to a flashed ESP32-C3, step by step.

---

## Table of contents

1. [Prerequisites](#1-prerequisites)
2. [One-time setup](#2-one-time-setup)
3. [Clone & prepare WLED](#3-clone--prepare-wled)
4. [Copy the F1Lamp usermod](#4-copy-the-f1lamp-usermod)
5. [Patch WLED's usermod registration file](#5-patch-wleds-usermod-registration-file)
6. [Build the firmware](#6-build-the-firmware)
7. [Flash to the ESP32-C3](#7-flash-to-the-esp32-c3)
8. [First boot & WiFi setup](#8-first-boot--wifi-setup)
9. [Testing without a live race](#9-testing-without-a-live-race)
10. [Serial monitor (debug logs)](#10-serial-monitor-debug-logs)
11. [Updating after code changes](#11-updating-after-code-changes)
12. [Troubleshooting](#12-troubleshooting)

---

## 1. Prerequisites

Install each tool below **in order**. All are free.

### 1.1 Git
Download and install from https://git-scm.com/download/win  
During install: accept all defaults (tick "Git Bash", leave PATH option as "Git from the command line and also from 3rd-party software").

Verify:
```powershell
git --version
# should print: git version 2.x.x
```

### 1.2 Python 3.x
Download from https://python.org/downloads  
**Critical:** on the first installer screen, tick **"Add Python to PATH"** before clicking Install.

Verify:
```powershell
python --version
# should print: Python 3.x.x
```

### 1.3 PlatformIO CLI
PlatformIO is the build system used to compile WLED. Install it using pip:

```powershell
pip install platformio
# Wait — this downloads ~50 MB of toolchains on first use
```

Verify:
```powershell
pio --version
# should print: PlatformIO Core, version 6.x.x
```

> **Alternative:** if you prefer a GUI, install [VS Code](https://code.visualstudio.com/) and the
> [PlatformIO IDE extension](https://marketplace.visualstudio.com/items?itemName=platformio.platformio-ide).
> The steps below show CLI commands, but every command has an equivalent button in the VS Code PlatformIO sidebar.

### 1.4 USB driver for your ESP32-C3 board
The driver depends on which USB-to-serial chip your specific board uses.
Check the underside of the board for the chip marking:

| Chip on board | Driver download |
|---|---|
| **CP2102** / CP2104 | https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers |
| **CH340** / CH341 | https://www.wch-ic.com/downloads/CH341SER_EXE.html |
| **No external chip** (USB direct, marked "USB" near the port) | Built-in on Windows 10/11 — no driver needed |

After installing, plug in the ESP32-C3. Open **Device Manager** → expand **Ports (COM & LPT)**. You should see a new entry like `Silicon Labs CP210x (COM3)`. Note your COM port number.

---

## 2. One-time setup

Open **PowerShell** (search "PowerShell" in the Start menu — no need to run as Admin).

```powershell
# Allow running local scripts (only needed once)
Set-ExecutionPolicy -ExecutionPolicy RemoteSigned -Scope CurrentUser
```

---

## 3. Clone & prepare WLED

```powershell
# Go to the f1-lamp repo root
cd C:\Users\Victor\source\repos\f1-lamp

# Clone WLED into a subfolder called "wled"
git clone https://github.com/Aircoookie/WLED.git wled

# Enter the WLED directory and check out the latest stable release
cd wled
git fetch --tags
$tag = git describe --tags (git rev-list --tags --max-count=1)
git checkout $tag
Write-Host "Checked out WLED $tag"
```

> If you want the very latest development code instead, skip the checkout step and stay on `main`.
> Stable releases are safer.

---

## 4. Copy the F1Lamp usermod

Run this from the `f1-lamp` repo root (the directory containing this file):

```powershell
cd C:\Users\Victor\source\repos\f1-lamp

# Create the usermod folder inside WLED
New-Item -ItemType Directory -Force "wled\wled00\usermods\F1LampUsermod"

# Copy the usermod header
Copy-Item -Force "usermod\F1LampUsermod.h" "wled\wled00\usermods\F1LampUsermod\"

# Copy the PlatformIO override (tells PlatformIO about the ESP32-C3 and the flag -D USERMOD_F1_LAMP)
Copy-Item -Force "platformio_override.ini" "wled\"

Write-Host "Files copied."
```

---

## 5. Patch WLED's usermod registration file

WLED needs to know our usermod exists. You add two small blocks to one file.

Open the file in Notepad:
```powershell
notepad wled\wled00\usermods_list.cpp
```

### Block A — add the include (near the top, after the other `#ifdef USERMOD_` includes)

Find a section that looks like this (there are many similar blocks):
```cpp
#ifdef USERMOD_EXAMPLE
  #include "usermods/example/example.h"
#endif
```

Add this block **immediately after** the last similar `#endif`:
```cpp
#ifdef USERMOD_F1_LAMP
  #include "../usermods/F1LampUsermod/F1LampUsermod.h"
#endif
```

### Block B — register the instance (inside `void registerUsermods()`)

Scroll down to the function that looks like:
```cpp
void registerUsermods()
{
  // ... other usermods.add() calls ...
}
```

Add this line **inside the function body**, alongside the other `usermods.add()` lines:
```cpp
#ifdef USERMOD_F1_LAMP
  usermods.add(new F1LampUsermod());
#endif
```

Save and close Notepad.

> Both blocks are also in `usermods_list_patch.cpp` in this repo for easy copy-paste reference.

### Automated version (optional)

The setup script does both edits automatically:
```powershell
cd C:\Users\Victor\source\repos\f1-lamp
.\scripts\setup.ps1
```

---

## 6. Build the firmware

```powershell
cd C:\Users\Victor\source\repos\f1-lamp\wled

# First build downloads the ESP32 toolchain (~500 MB) — takes 5-10 min once.
# Subsequent builds are fast (~30-60 s).
pio run -e esp32c3_f1lamp
```

If the build succeeds you will see output ending with:
```
RAM:   [=         ]  xx.x% (used xxxxx bytes from 327680 bytes)
Flash: [====      ]  xx.x% (used xxxxxxx bytes from 4194304 bytes)
========================= [SUCCESS] Took xx.xx seconds =========================
```

If you see errors, check the [Troubleshooting](#12-troubleshooting) section.

---

## 7. Flash to the ESP32-C3

### Normal upload

Connect your ESP32-C3 via USB, then:

```powershell
cd C:\Users\Victor\source\repos\f1-lamp\wled

# Replace COM3 with your actual port (from Device Manager)
pio run -e esp32c3_f1lamp --target upload --upload-port COM3
```

If you leave out `--upload-port`, PlatformIO will try to auto-detect. That usually works.

### Bootloader mode (if upload fails with "Failed to connect")

1. Hold the **BOOT** button on the board.
2. While holding BOOT, briefly press and release **RESET** (or unplug/replug the USB cable while holding BOOT).
3. Release BOOT — the board is now in download mode (you may see a different COM port appear).
4. Run the upload command above.

### VS Code alternative

1. Open the `wled/` folder in VS Code.
2. PlatformIO will detect the project automatically.
3. Select `esp32c3_f1lamp` in the environment picker (bottom status bar).
4. Click the **→ Upload** button (arrow icon in the bottom bar).

---

## 8. First boot & WiFi setup

After flashing:

1. The ESP32-C3 reboots and broadcasts a WiFi access point named **`WLED-AP`** (password: `wled1234`).
2. Connect your phone or laptop to `WLED-AP`.
3. A captive portal opens automatically (if not, navigate to `http://4.3.2.1`).
4. Go to **WiFi Settings** and enter your home WiFi credentials.
5. WLED reboots and connects to your home network.
6. Find the device IP in your router's admin panel (look for a device named `WLED`), or use the WLED Android/iOS app which auto-discovers it.
7. Open `http://<your-wled-ip>` in a browser.

> **Tip:** Set a static IP or DHCP reservation for the ESP32-C3 in your router. Makes life easier.

---

## 9. Testing without a live race

The usermod has a built-in **Force State** feature. When active, it bypasses the SignalR connection and locks the lamp in a specific F1 state. You can trigger it in two ways:

### Method A — WLED web UI (easiest)

1. Open `http://<wled-ip>` → **Settings** → **Usermods**.
2. Find the **F1Lamp** section.
3. Set **`forceState`** to a number:

| Value | State | What you'll see |
|---|---|---|
| `0` | Off (live mode) | Normal SignalR operation |
| `1` | IDLE | Playlist 12 — dim red (preset 1) |
| `2` | SESSION START | Playlist 20 — formation lap animation |
| `3` | GREEN FLAG | Playlist 15 — solid green |
| `4` | YELLOW FLAG | Playlist 16 — amber blink |
| `5` | SAFETY CAR | Playlist 13 — amber chase |
| `6` | VIRTUAL SC | Playlist 22 — VSC pulse |
| `7` | RED FLAG | Playlist 14 — red blink |
| `8` | CHEQUERED | Playlist 23 — white/black × 15 |

4. Click **Save**. The lamp changes instantly.
5. Set back to `0` to return to live mode.

### Method B — HTTP API (scriptable, no browser needed)

Use PowerShell to cycle through all states automatically:

```powershell
$ip = "192.168.1.XXX"   # ← replace with your WLED IP

$states = @{1="IDLE"; 2="SESSION START"; 3="GREEN"; 4="YELLOW"; 5="SC"; 6="VSC"; 7="RED FLAG"; 8="CHEQUERED"}

foreach ($num in 1..8) {
    Write-Host "Testing state $num : $($states[$num])"
    Invoke-RestMethod -Method Post -Uri "http://$ip/json/um" `
        -ContentType "application/json" `
        -Body "{`"F1Lamp`":{`"forceState`":$num}}"
    Start-Sleep -Seconds 4   # watch for 4 seconds
}

# Restore live mode
Invoke-RestMethod -Method Post -Uri "http://$ip/json/um" `
    -ContentType "application/json" `
    -Body '{"F1Lamp":{"forceState":0}}'
Write-Host "Done - back to live mode"
```

### Method C — test a single state directly

```powershell
$ip = "192.168.1.XXX"

# Lock on RED FLAG  
Invoke-RestMethod -Method Post -Uri "http://$ip/json/um" `
    -ContentType "application/json" `
    -Body '{"F1Lamp":{"forceState":7}}'

# Restore live mode
Invoke-RestMethod -Method Post -Uri "http://$ip/json/um" `
    -ContentType "application/json" `
    -Body '{"F1Lamp":{"forceState":0}}'
```

### Checking the current state

```powershell
# GET the current usermod state
Invoke-RestMethod -Uri "http://$ip/json/um" | ConvertTo-Json
```

Or in the WLED UI, tap the **ℹ Info** button — you'll see:
```
F1 Status  │  Red flag
F1 Stream  │  TEST MODE
```

---

## 10. Serial monitor (debug logs)

The firmware prints detailed logs to the serial port. View them with:

```powershell
cd C:\Users\Victor\source\repos\f1-lamp\wled
pio device monitor --port COM3 --baud 115200
```

Or in VS Code: click the **🔌 Serial Monitor** plug icon in the PlatformIO sidebar.

You'll see output like:
```
[F1Lamp] SignalR usermod ready
[F1Lamp] Negotiating with F1 timing service...
[F1Lamp] Token obtained (len=148)
[F1Lamp] WS connecting...
[F1Lamp] WS connected → /signalr/connect?...
[F1Lamp] Subscribe sent (TrackStatus, SessionStatus, Heartbeat)
[F1Lamp] SessionStatus: Inactive
[F1Lamp] → IDLE (pl.12)  preset=12
[F1Lamp] Heartbeat ♥
```

Press **Ctrl+C** to exit the monitor.

---

## 11. Updating after code changes

When you edit `F1LampUsermod.h` in this repo, re-deploy with:

```powershell
# Copy the updated file
Copy-Item -Force `
    "C:\Users\Victor\source\repos\f1-lamp\usermod\F1LampUsermod.h" `
    "C:\Users\Victor\source\repos\f1-lamp\wled\wled00\usermods\F1LampUsermod\"

# Rebuild and flash
cd C:\Users\Victor\source\repos\f1-lamp\wled
pio run -e esp32c3_f1lamp --target upload --upload-port COM3
```

WLED preserves its configuration (WiFi credentials, presets, usermod settings) across firmware updates because it stores them in a separate flash partition.

---

## 12. Troubleshooting

### Build errors

| Error | Fix |
|---|---|
| `'WebSocketsClient' was not declared` | Run `pio lib install` inside `wled/`, then rebuild |
| `'applyPreset' was not declared` | WLED version is too old; update to the latest release tag |
| `Multiple definition of F1LampUsermod::_instance` | You added the `#include` in `usermods_list.cpp` twice; remove the duplicate |
| Python / pip not found | Re-run the Python installer and tick "Add to PATH" |
| `pio: command not found` | `pip install platformio` or restart PowerShell after install |

### Flash / upload errors

| Error | Fix |
|---|---|
| `Failed to connect to ESP32: Timed out` | Enter bootloader mode: hold BOOT, press RESET, release BOOT, then retry |
| `No serial port found` | Check Device Manager; install the correct USB driver (see §1.4) |
| Upload completes but device doesn't appear | Try a different USB cable (some cheap cables are power-only, no data) |
| Wrong COM port | Check Device Manager → Ports and update `--upload-port COMx` |

### Runtime issues

| Symptom | Fix |
|---|---|
| `F1 Stream: offline` | WiFi not connected or F1 server unreachable; reconnect happens automatically |
| Lamp stuck on one state | Check if `forceState` is non-zero in Settings → Usermods; set to `0` |
| Preset not triggering | Verify the preset ID exists in Settings → Presets (hover to see the ID) |
| Preset resets to 0 after save | Use values 1–250; WLED does not have preset 0 |
| `[F1Lamp] Negotiate HTTP 403` | F1 server rate-limiting; wait 60 s, device will auto-retry |

### Finding your WLED IP

```powershell
# Scan your local network for the device (replace 192.168.1 with your subnet)
1..254 | ForEach-Object {
    $ip = "192.168.1.$_"
    if (Test-Connection -ComputerName $ip -Count 1 -Quiet) {
        $hostname = try { [System.Net.Dns]::GetHostEntry($ip).HostName } catch { "?" }
        Write-Host "$ip  $hostname"
    }
}
```

Or use the free tool [Angry IP Scanner](https://angryip.org/).

---

## Quick reference card

```
# One-time setup
pip install platformio
git clone https://github.com/Aircoookie/WLED.git wled

# Every time you edit the usermod
Copy-Item -Force "usermod\F1LampUsermod.h" "wled\wled00\usermods\F1LampUsermod\"
cd wled
pio run -e esp32c3_f1lamp --target upload --upload-port COM3

# Test a state (5 = Safety Car)
Invoke-RestMethod -Method Post -Uri "http://192.168.1.XXX/json/um" `
    -ContentType "application/json" -Body '{"F1Lamp":{"forceState":5}}'

# Watch serial logs
pio device monitor --port COM3 --baud 115200

# Back to live mode
Invoke-RestMethod -Method Post -Uri "http://192.168.1.XXX/json/um" `
    -ContentType "application/json" -Body '{"F1Lamp":{"forceState":0}}'
```
