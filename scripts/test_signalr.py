#!/usr/bin/env python3
"""Test F1 SignalR live stream using requests + websocket-client."""
import requests, websocket, json, time, urllib.parse

HOST = "https://livetiming.formula1.com"
session = requests.Session()
session.headers["User-Agent"] = "BestHTTP"

# 1. Negotiate
print("Negotiating...")
r = session.get(
    f"{HOST}/signalr/negotiate",
    params={
        "connectionData": '[{"name":"streaming"}]',
        "clientProtocol": "1.5",
    },
    timeout=10,
)
r.raise_for_status()
neg = r.json()
token = neg["ConnectionToken"]
print(f"Token: {token[:50]}...")
print(f"Cookies: {dict(session.cookies)}")

# 2. Build WebSocket URL
token_enc = urllib.parse.quote(token, safe="")
conn_data = urllib.parse.quote('[{"name":"streaming"}]', safe="")
ws_url = (
    f"wss://livetiming.formula1.com/signalr/connect"
    f"?transport=webSockets&clientProtocol=1.5"
    f"&connectionToken={token_enc}"
    f"&connectionData={conn_data}"
)
print(f"WS URL length: {len(ws_url)}")

# 3. Connect WebSocket with cookies from negotiate
cookie_str = "; ".join(f"{k}={v}" for k, v in session.cookies.items())
print("Connecting WebSocket...")
ws = websocket.create_connection(
    ws_url,
    header=[
        "User-Agent: BestHTTP",
        f"Cookie: {cookie_str}",
    ],
    timeout=10,
)
print("WebSocket connected!")

# 4. Subscribe
subscribe = json.dumps({
    "H": "streaming",
    "M": "Subscribe",
    "A": [["Heartbeat", "SessionStatus", "TrackStatus",
           "RaceControlMessages", "SessionInfo"]],
    "I": 1,
})
ws.send(subscribe)
print("Subscribed. Reading frames for 20 seconds...\n")

# 5. Read frames
t0 = time.time()
frames = 0
ws.settimeout(3.0)

while time.time() - t0 < 20:
    try:
        msg = ws.recv()
        if not msg:
            continue
        text = msg if isinstance(msg, str) else msg.decode("utf-8", "replace")
        if text.strip() and text.strip() != "{}":
            frames += 1
            print(f"[Frame {frames}] ({len(text)}b)")
            try:
                j = json.loads(text)
                print(json.dumps(j, indent=2)[:800])
            except Exception:
                print(text[:800])
            print()
            if frames >= 10:
                break
    except websocket.WebSocketTimeoutException:
        continue
    except Exception as e:
        print(f"Error: {e}")
        break

print(f"Done. {frames} data frames in {time.time()-t0:.1f}s")
ws.close()
