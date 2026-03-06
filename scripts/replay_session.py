#!/usr/bin/env python3
"""
replay_session.py – Replay a historical F1 session against a live F1 Lamp device.

Fetches .jsonStream files from livetiming.formula1.com/static/... and replays
state changes + race-control events to the lamp's REST API at real-time speed
(or at a chosen multiplier).

Usage:
    python replay_session.py [--lamp IP] [--speed N] [--list-sessions]

Examples:
    # List available sessions in 2026
    python replay_session.py --list-sessions

    # Replay Australian GP FP1 at 10× speed on default lamp IP
    python replay_session.py --session 2026/2026-03-08_Australian_Grand_Prix/2026-03-06_Practice_1/ --speed 10

    # Replay at real-time speed on a different IP
    python replay_session.py --session 2026/2026-03-08_Australian_Grand_Prix/2026-03-08_Race/ --lamp 192.168.1.50 --speed 1
"""

import argparse
import json
import time
import urllib.request
import urllib.error


# ──────────────────────────────────────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────────────────────────────────────
STATIC_BASE = "https://livetiming.formula1.com/static/"
DEFAULT_LAMP = "192.168.100.175"
DEFAULT_SPEED = 5   # 5× real time


# ──────────────────────────────────────────────────────────────────────────────
# TrackStatus code → lamp state number (mirrors F1NetWork.cpp trackCodeToState)
# ──────────────────────────────────────────────────────────────────────────────
TRACK_CODE_TO_STATE = {
    "1": (2, "Green flag"),
    "2": (1, "Yellow flag"),
    "4": (6, "Safety Car"),
    "5": (7, "Red Flag"),
    "6": (8, "Virtual SC"),
    "7": (9, "VSC Ending"),
}

# SessionStatus → lamp state number (mirrors processMessage SessionStatus branch)
SESSION_STATUS_TO_STATE = {
    "Started":  (3, "Session Start"),
    "Finished": (5, "Chequered Flag"),
    "Ends":     (5, "Chequered Flag"),
    "Inactive": (0, "Idle"),
}

# Test event indices (mirrors /api/test_event)
EVT_WINNER      = 0
EVT_FASTEST_LAP = 1
EVT_DRS         = 2
EVT_START_LIGHTS = 3


# ──────────────────────────────────────────────────────────────────────────────
# HTTP helpers
# ──────────────────────────────────────────────────────────────────────────────
def fetch(url: str, timeout: int = 10) -> str:
    """Fetch a URL and return the decoded body."""
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return r.read().decode("utf-8-sig")


def lamp_post(lamp_ip: str, path: str, body: dict | None = None, params: dict | None = None) -> bool:
    """POST to the lamp. body is JSON-encoded; params are appended as query string."""
    if params:
        qs = "&".join(f"{k}={v}" for k, v in params.items())
        url = f"http://{lamp_ip}{path}?{qs}"
    else:
        url = f"http://{lamp_ip}{path}"

    data = json.dumps(body).encode() if body else b""
    req = urllib.request.Request(
        url,
        data=data or None,
        headers={"Content-Type": "application/json"} if body else {},
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=5) as r:
            return r.status < 300
    except Exception as e:
        print(f"  [lamp] POST {path} failed: {e}")
        return False


def lamp_set_state(lamp_ip: str, state: int, label: str):
    ok = lamp_post(lamp_ip, "/api/state", params={"state": state})
    status = "✓" if ok else "✗"
    print(f"  {status} State → {state} ({label})")


def lamp_test_event(lamp_ip: str, ev: int, label: str):
    ok = lamp_post(lamp_ip, "/api/test_event", params={"ev": ev})
    status = "✓" if ok else "✗"
    print(f"  {status} Event → {label}")


# ──────────────────────────────────────────────────────────────────────────────
# Session index helpers
# ──────────────────────────────────────────────────────────────────────────────
def list_sessions(year: int = 2026):
    """Print all available sessions for a year."""
    idx = json.loads(fetch(f"{STATIC_BASE}{year}/Index.json"))
    for meeting in idx.get("Meetings", []):
        print(f"\n📍 {meeting.get('Name')} — {meeting.get('Location')}")
        for s in meeting.get("Sessions", []):
            path = s.get("Path", "")
            key  = s.get("Key", "?")
            name = s.get("Name", s.get("Type", "?"))
            date = s.get("StartDate", "?")[:10]
            print(f"   [{key}] {date}  {name:20s}  {path}")


# ──────────────────────────────────────────────────────────────────────────────
# Stream parsing
# ──────────────────────────────────────────────────────────────────────────────
def parse_timestamp(ts: str) -> float:
    """Parse 'HH:MM:SS.mmm' → seconds."""
    parts = ts.split(":")
    if len(parts) == 3:
        h, m, rest = parts
        s, *ms = rest.split(".")
        ms_val = int(ms[0]) if ms else 0
        return int(h) * 3600 + int(m) * 60 + int(s) + ms_val / 1000
    return 0.0


def load_stream(session_path: str, topic: str) -> list[tuple[float, dict]]:
    """Fetch and parse a .jsonStream file into (seconds_offset, data) pairs."""
    url = f"{STATIC_BASE}{session_path}{topic}.jsonStream"
    try:
        raw = fetch(url)
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return []
        raise
    events = []
    for line in raw.splitlines():
        line = line.strip()
        if not line:
            continue
        # Format: HH:MM:SS.mmm{json...}
        sep = line.index("{")
        ts  = line[:sep]
        payload = line[sep:]
        try:
            data = json.loads(payload)
        except json.JSONDecodeError:
            continue
        events.append((parse_timestamp(ts), data))
    return events


# ──────────────────────────────────────────────────────────────────────────────
# Build a merged, sorted timeline
# ──────────────────────────────────────────────────────────────────────────────
def build_timeline(session_path: str) -> list[tuple[float, str, dict]]:
    """Return [(t_seconds, topic, data), ...] sorted by time."""
    timeline = []
    for topic in ["TrackStatus", "SessionStatus", "RaceControlMessages"]:
        events = load_stream(session_path, topic)
        print(f"  {topic}: {len(events)} events")
        for t, data in events:
            timeline.append((t, topic, data))
    timeline.sort(key=lambda x: x[0])
    return timeline


# ──────────────────────────────────────────────────────────────────────────────
# Apply a single event to the lamp
# ──────────────────────────────────────────────────────────────────────────────
def apply_event(lamp_ip: str, topic: str, data: dict):
    if topic == "TrackStatus":
        code = str(data.get("Status", ""))
        if code in TRACK_CODE_TO_STATE:
            state_n, label = TRACK_CODE_TO_STATE[code]
            lamp_set_state(lamp_ip, state_n, label)
        else:
            print(f"  · TrackStatus {code} → unknown, skipped")

    elif topic == "SessionStatus":
        status = data.get("Status", "")
        for key, (state_n, label) in SESSION_STATUS_TO_STATE.items():
            if key in status:
                if key == "Started":
                    lamp_test_event(lamp_ip, EVT_START_LIGHTS, "Start Lights sequence")
                elif key in ("Finished", "Ends"):
                    lamp_test_event(lamp_ip, EVT_WINNER, "Winner rainbow spin")
                else:
                    lamp_set_state(lamp_ip, state_n, label)
                break

    elif topic == "RaceControlMessages":
        # Stream format: single {"Message": "...", ...}
        # Snapshot format: {"Messages": {"1": {...}, "2": {...}}} or {"Messages": [{...}]}
        messages_to_check = []
        raw_msgs = data.get("Messages")
        if raw_msgs is None:
            # Direct single message in stream
            messages_to_check = [data]
        elif isinstance(raw_msgs, dict):
            # Numbered dict (snapshot deltas)
            messages_to_check = list(raw_msgs.values())
        elif isinstance(raw_msgs, list):
            messages_to_check = raw_msgs

        for m in messages_to_check:
            msg = m.get("Message", "") if isinstance(m, dict) else str(m)
            mu = msg.upper()
            if "FASTEST LAP" in mu:
                lamp_test_event(lamp_ip, EVT_FASTEST_LAP, f"Fastest lap ({msg[:60]})")
            if "DRS" in mu and "ENABLED" in mu:
                lamp_test_event(lamp_ip, EVT_DRS, f"DRS enabled ({msg[:60]})")


# ──────────────────────────────────────────────────────────────────────────────
# Main replay loop
# ──────────────────────────────────────────────────────────────────────────────
def replay(session_path: str, lamp_ip: str, speed: float):
    if not session_path.endswith("/"):
        session_path += "/"

    print(f"\n{'='*60}")
    print(f"  Session : {session_path}")
    print(f"  Lamp    : http://{lamp_ip}")
    print(f"  Speed   : {speed}×")
    print(f"{'='*60}\n")

    print("Fetching streams...")
    timeline = build_timeline(session_path)
    if not timeline:
        print("No events found — check session path.")
        return

    total = timeline[-1][0]
    print(f"\nTotal session duration: {total/3600:.1f}h  ({len(timeline)} events)")
    print(f"Replay time at {speed}×: {total/speed/60:.1f} min\n")
    print("Starting replay — press Ctrl+C to stop.\n")

    real_start = time.time()
    event_idx  = 0

    while event_idx < len(timeline):
        t_event, topic, data = timeline[event_idx]
        t_now_real  = time.time() - real_start      # wall time elapsed
        t_now_sim   = t_now_real * speed             # simulated time elapsed

        if t_now_sim >= t_event:
            # Format display timestamp
            h  = int(t_event // 3600)
            m  = int((t_event % 3600) // 60)
            s  = int(t_event % 60)
            ts = f"{h:02d}:{m:02d}:{s:02d}"

            print(f"[{ts}] {topic}: {json.dumps(data)[:80]}")
            apply_event(lamp_ip, topic, data)
            event_idx += 1
        else:
            # Sleep at most 0.05 s so we don't drift
            sleep_sim  = t_event - t_now_sim
            sleep_real = min(sleep_sim / speed, 0.05)
            time.sleep(sleep_real)

    print("\n✅ Replay complete.")


# ──────────────────────────────────────────────────────────────────────────────
# Entry point
# ──────────────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(description="Replay a historical F1 session on the F1 Lamp.")
    parser.add_argument("--lamp",           default=DEFAULT_LAMP,
                        help=f"Lamp IP address (default: {DEFAULT_LAMP})")
    parser.add_argument("--speed", type=float, default=DEFAULT_SPEED,
                        help=f"Replay speed multiplier (default: {DEFAULT_SPEED})")
    parser.add_argument("--session",        default=None,
                        help="Session path (e.g. 2026/2026-03-08_Australian_Grand_Prix/2026-03-08_Race/)")
    parser.add_argument("--list-sessions",  action="store_true",
                        help="List available sessions and exit")
    parser.add_argument("--year",  type=int, default=2026,
                        help="Year index to list (default: 2026)")
    args = parser.parse_args()

    if args.list_sessions:
        list_sessions(args.year)
        return

    if not args.session:
        print("Error: --session PATH is required (use --list-sessions to browse)")
        parser.print_help()
        return

    replay(args.session, args.lamp, args.speed)


if __name__ == "__main__":
    main()
