"""
capture_serial.py  –  Robust USB-CDC serial capture for ESP32-C3 SuperMini
Usage:
    python capture_serial.py

1. Run this script
2. Press RST on the board
3. Script waits for COM5 to re-enumerate, then prints all output

The ClearCommError PermissionError happens because pyserial tries to use a
stale HANDLE after USB-CDC disconnects on reset.  We fix it by:
  - waiting for the port to disappear (disconnect event)
  - waiting for it to come back (re-enumeration, ~2-4 s)
  - opening a fresh handle only after the port is stable
"""

import sys
import time
import serial
import serial.tools.list_ports

PORT    = "COM5"
BAUD    = 115200
TIMEOUT = 30      # seconds to wait after reset for the port to come back

def port_exists(name: str) -> bool:
    return any(p.device == name for p in serial.tools.list_ports.comports())

def wait_for_port(name: str, appear: bool, deadline: float) -> bool:
    """Wait until a port appears (appear=True) or disappears (appear=False)."""
    while time.time() < deadline:
        if port_exists(name) == appear:
            return True
        time.sleep(0.2)
    return False

def read_loop(port: str, baud: int):
    """Open the port and read until KeyboardInterrupt or 60 s of silence."""
    try:
        s = serial.Serial(port, baud, timeout=0.5)
    except serial.SerialException as e:
        print(f"[capture] Could not open {port}: {e}")
        return

    print(f"[capture] Opened {port} @ {baud} baud — reading…\n{'─'*60}")
    silence = 0
    try:
        while silence < 60:
            try:
                chunk = s.read(512)
            except (serial.SerialException, PermissionError, OSError) as e:
                print(f"\n[capture] Port dropped ({e}) – device reset?")
                break
            if chunk:
                silence = 0
                sys.stdout.write(chunk.decode("utf-8", errors="replace"))
                sys.stdout.flush()
            else:
                silence += 0.5
    except KeyboardInterrupt:
        print("\n[capture] Stopped by user.")
    finally:
        try:
            s.close()
        except Exception:
            pass

def main():
    print(f"[capture] Waiting for {PORT}…  (press RST on the board now)")

    deadline = time.time() + TIMEOUT

    # If port is currently open/present, wait for it to disappear first (reset event)
    if port_exists(PORT):
        print(f"[capture] {PORT} is visible – watching for disconnect (RST)…")
        if wait_for_port(PORT, appear=False, deadline=deadline):
            print(f"[capture] {PORT} disconnected (RST detected)")
        else:
            print(f"[capture] Timeout waiting for disconnect – trying anyway")

    # Now wait for it to re-appear
    print(f"[capture] Waiting for {PORT} to re-enumerate…")
    deadline2 = time.time() + TIMEOUT
    if wait_for_port(PORT, appear=True, deadline=deadline2):
        # Give the OS a moment to finish enumeration before opening
        time.sleep(0.8)
        print(f"[capture] {PORT} is back!")
        read_loop(PORT, BAUD)
    else:
        print(f"[capture] {PORT} never came back within {TIMEOUT} s")
        print("  • Check USB cable / device manager")
        print("  • The board may have crashed before USB re-enumerated")

if __name__ == "__main__":
    main()
