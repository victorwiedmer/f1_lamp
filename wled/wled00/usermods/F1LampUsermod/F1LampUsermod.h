#pragma once

#include "wled.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

/*
 * F1LampUsermod  –  SignalR edition
 * ===================================
 * Connects directly to the F1 official live timing service
 * (livetiming.formula1.com/signalr) using the same ASP.NET SignalR 1.5
 * protocol as the f1_sensor Home Assistant integration.
 *
 * Data is PUSHED in real time — no polling interval.
 *
 * Protocol flow (mirrors signalr.py from Nicxe/f1_sensor):
 *   1. HTTP GET /signalr/negotiate?clientProtocol=1.5&connectionData=...
 *      → Response: JSON with "ConnectionToken"
 *   2. WebSocket connect wss://livetiming.formula1.com/signalr/connect
 *      with token + hub params in the query string
 *   3. Send Subscribe JSON → streams: TrackStatus, SessionStatus, Heartbeat
 *   4. Receive push events:
 *        {"R": {...}}        – initial state snapshot on connect
 *        {"M": [{...}]}      – live feed updates
 *   5. Re-subscribe every 5 min (keep Azure SignalR group alive)
 *   6. Auto-reconnect with exponential back-off on disconnect
 *
 * TrackStatus codes (F1 live timing spec):
 *   "1" = AllClear   → green flag
 *   "2" = Yellow     → yellow flag
 *   "3" = Flag       → yellow variant
 *   "4" = SafetyCar
 *   "5" = RedFlag
 *   "6" = VirtualSafetyCar
 *   "7" = VSCEnding  → treated as SafetyCar (SC coming in)
 *
 * SessionStatus: "Started" = live session; anything else = finished/idle.
 *
 * Requirements:
 *   - ESP32 with WiFi (tested on ESP32-C3)
 *   - WLED 0.14+ (ArduinoJson v6 + WebSocketsClient, both already in WLED)
 */

class F1LampUsermod : public Usermod {
public:
  static const char _name[];
  static const char _enabled[];

private:
  // ─────────────────────────────────────────────
  // Static instance pointer for the WS event callback
  // (WebSocketsClient requires a plain C-style function pointer)
  // ─────────────────────────────────────────────
  static F1LampUsermod* _instance;

  // ─────────────────────────────────────────────
  // F1 State machine
  // ─────────────────────────────────────────────
  enum class F1State : uint8_t {
    IDLE,             // No active F1 session          → Playlist - Idle (12)
    SESSION_START,    // Session just became active    → Playlist - Formation Lap (20)
    GREEN,            // AllClear / Green flag         → Playlist - Green Flag (15)
    YELLOW,           // Yellow flag (full or sector)  → Playlist - Yellow Flag (16)
    SAFETY_CAR,       // Safety car deployed           → Playlist - Safety Car (13)
    VIRTUAL_SC,       // Virtual safety car            → Playlist - VSC (22)
    RED_FLAG,         // Red flag                      → Playlist - Red Flag (14)
    CHEQUERED,        // Session ended / chequered     → Playlist - Chequered 30s (23)
    UNKNOWN
  };

  // ─────────────────────────────────────────────
  // Configurable settings (persisted in WLED flash)
  // ─────────────────────────────────────────────
  bool     enabled              = true;
  uint16_t reconnectIntervalSec = 30; // seconds between reconnect attempts

  // ── Test / override ──────────────────────────────────────────────────
  // Set forceState to a value 1‑8 to lock the lamp in that F1 state
  // regardless of live data. Useful for testing presets without a race.
  //   0 = disabled (normal live SignalR mode)
  //   1 = IDLE          2 = SESSION_START  3 = GREEN
  //   4 = YELLOW        5 = SAFETY_CAR     6 = VIRTUAL_SC
  //   7 = RED_FLAG      8 = CHEQUERED
  // Changeable at runtime via HTTP without reflashing:
  //   POST http://<wled-ip>/json/um  body: {"F1Lamp":{"forceState":5}}
  uint8_t forceState = 0;

  // Preset IDs — defaulted to your WLED export playlist IDs.
  // Override in WLED → Settings → Usermods → F1Lamp. Use 0 to skip.
  //
  //   12 = Playlist - Idle           (→ preset  1 dim red)
  //   13 = Playlist - Safety Car     (→ preset  9 amber chase)
  //   14 = Playlist - Red Flag       (→ preset  4 red blink)
  //   15 = Playlist - Green Flag     (→ preset  5 solid green)
  //   16 = Playlist - Yellow Flag    (→ preset  6 yellow blink)
  //   20 = Playlist - Formation Lap  (alternates presets 2 ↔ 3)
  //   22 = Playlist - VSC            (alternates presets 7 ↔ 8)
  //   23 = Playlist - Chequered 30s  (alternates presets 10 ↔ 11 × 15, ends on 1)
  uint8_t presetIdle         = 12;
  uint8_t presetSessionStart = 20;
  uint8_t presetGreen        = 15;
  uint8_t presetYellow       = 16;
  uint8_t presetSafetyCar    = 13;
  uint8_t presetVirtualSC    = 22;
  uint8_t presetRedFlag      = 14;
  uint8_t presetChequered    = 23;

  // ─────────────────────────────────────────────
  // Runtime state (not persisted)
  // ─────────────────────────────────────────────
  WebSocketsClient  _ws;
  bool              _wsConnected       = false;
  bool              _subscribed        = false;
  bool              _initDone          = false;
  unsigned long     _lastConnectMs     = 0;  // last connect attempt timestamp
  unsigned long     _lastKeepAliveMs   = 0;  // last subscribe (keepalive) sent
  uint32_t          _reconnectDelaySec = 5;  // current back-off delay

  F1State           _currentState  = F1State::IDLE;
  F1State           _lastApplied   = F1State::UNKNOWN;
  bool              _sessionActive = false;
  uint8_t           _lastForce     = 0; // tracks previous forceState to detect changes

  // ─────────────────────────────────────────────
  // F1 SignalR endpoints
  // Matches NEGOTIATE_URL / CONNECT_URL in signalr.py
  // ─────────────────────────────────────────────
  static constexpr const char* SIGNALR_HOST  = "livetiming.formula1.com";
  static constexpr int         SIGNALR_PORT  = 443;
  static constexpr const char* NEGOTIATE_PATH = "/signalr/negotiate";
  static constexpr const char* CONNECT_PREFIX = "/signalr/connect";
  // URL-encoded: [{"name":"Streaming"}]
  static constexpr const char* HUB_DATA_ENC =
    "%5B%7B%22name%22%3A%22Streaming%22%7D%5D";

  // Minimal subscribe — only streams the lamp needs.
  // The Python integration subscribes to many more, but keeping this
  // small reduces JSON parse overhead on the ESP32.
  static constexpr const char* SUBSCRIBE_MSG =
    "{\"H\":\"Streaming\",\"M\":\"Subscribe\","
    "\"A\":[[\"TrackStatus\",\"SessionStatus\",\"Heartbeat\"]],"
    "\"I\":1}";

  // ─────────────────────────────────────────────
  // Helpers
  // ─────────────────────────────────────────────

  bool isWifiReady() { return WiFi.status() == WL_CONNECTED; }

  // Percent-encode a string (RFC 3986).
  // Required for the ConnectionToken which contains +, / and = characters.
  static String urlEncode(const String& s) {
    String out;
    out.reserve(s.length() * 3);
    for (unsigned i = 0; i < s.length(); i++) {
      char c = s[i];
      if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
        out += c;
      } else {
        char buf[4];
        snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
        out += buf;
      }
    }
    return out;
  }

  // ─────────────────────────────────────────────
  // Step 1: HTTP GET /signalr/negotiate → ConnectionToken
  // Mirrors the negotiate() / connect() flow in signalr.py
  // ─────────────────────────────────────────────
  bool negotiate(String& outToken, String& outCookie) {
    String url = String("https://") + SIGNALR_HOST + NEGOTIATE_PATH
               + "?clientProtocol=1.5"
               + "&connectionData=" + HUB_DATA_ENC;

    WiFiClientSecure client;
    client.setInsecure(); // F1 uses Let's Encrypt; no root CA on device

    HTTPClient http;
    http.begin(client, url);
    http.addHeader("User-Agent", "BestHTTP");         // same UA as Python client
    http.addHeader("Accept-Encoding", "gzip,identity");
    http.setTimeout(10000);

    int code = http.GET();
    if (code != HTTP_CODE_OK) {
      DEBUG_PRINTF("[F1Lamp] Negotiate HTTP %d\n", code);
      http.end();
      return false;
    }

    // Capture Set-Cookie header BEFORE consuming the body stream
    outCookie = http.header("Set-Cookie");

    DynamicJsonDocument doc(1024);
    DeserializationError err = deserializeJson(doc, http.getStream());
    http.end();

    if (err) {
      DEBUG_PRINTF("[F1Lamp] Negotiate JSON error: %s\n", err.c_str());
      return false;
    }

    outToken = doc["ConnectionToken"] | "";
    if (outToken.isEmpty()) {
      DEBUG_PRINTLN("[F1Lamp] Empty ConnectionToken");
      return false;
    }

    DEBUG_PRINTF("[F1Lamp] Token obtained (len=%d)\n", outToken.length());
    return true;
  }

  // ─────────────────────────────────────────────
  // Step 2: Start WebSocket connection
  // ─────────────────────────────────────────────
  void connectSignalR() {
    if (!isWifiReady()) return;

    DEBUG_PRINTLN("[F1Lamp] Negotiating with F1 timing service...");
    String token, cookie;
    if (!negotiate(token, cookie)) {
      scheduleReconnect();
      return;
    }

    // Build WS path — same structure as CONNECT_URL in signalr.py
    String path = String(CONNECT_PREFIX)
                + "?transport=webSockets"
                + "&clientProtocol=1.5"
                + "&connectionToken=" + urlEncode(token)
                + "&connectionData=" + HUB_DATA_ENC;

    if (cookie.length() > 0)
      _ws.setExtraHeaders(("Cookie: " + cookie).c_str());

    _ws.beginSSL(SIGNALR_HOST, SIGNALR_PORT, path.c_str());
    _ws.onEvent(F1LampUsermod::_wsEventCB);
    _ws.setUserAgent("BestHTTP");
    _ws.setReconnectInterval(0);  // we manage reconnect
    _ws.enableHeartbeat(0, 0, 0); // disable lib heartbeat; we send SignalR keepalive

    DEBUG_PRINTLN("[F1Lamp] WS connecting...");
  }

  // ─────────────────────────────────────────────
  // Step 3: Send subscribe message
  // Called on WStype_CONNECTED and every 5 min (keepalive)
  // Mirrors _heartbeat() in signalr.py
  // ─────────────────────────────────────────────
  void sendSubscribe() {
    if (!_wsConnected) return;
    _ws.sendTXT(SUBSCRIBE_MSG);
    _lastKeepAliveMs = millis();
    _subscribed      = true;
    DEBUG_PRINTLN("[F1Lamp] Subscribe sent (TrackStatus, SessionStatus, Heartbeat)");
  }

  // ─────────────────────────────────────────────
  // Static WS event callback → dispatches to instance method
  // ─────────────────────────────────────────────
  static void _wsEventCB(WStype_t type, uint8_t* payload, size_t length) {
    if (_instance) _instance->handleWsEvent(type, payload, length);
  }

  void handleWsEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
      case WStype_CONNECTED:
        DEBUG_PRINTF("[F1Lamp] WS connected → %s\n", (char*)payload);
        _wsConnected       = true;
        _subscribed        = false;
        _reconnectDelaySec = 10; // reset back-off on success
        sendSubscribe();
        break;

      case WStype_DISCONNECTED:
        DEBUG_PRINTLN("[F1Lamp] WS disconnected");
        _wsConnected = false;
        _subscribed  = false;
        scheduleReconnect();
        break;

      case WStype_TEXT:
        handleMessage(String((char*)payload, length));
        break;

      case WStype_ERROR:
        DEBUG_PRINTLN("[F1Lamp] WS error");
        _wsConnected = false;
        scheduleReconnect();
        break;

      default:
        break;
    }
  }

  // ─────────────────────────────────────────────
  // Parse an incoming SignalR JSON frame.
  //
  // Frame types (from signalr.py / LiveBus._run):
  //   {"R": {"TrackStatus":{...},"SessionStatus":{...}}}   ← initial snapshot
  //   {"M": [{"H":"Streaming","M":"feed","A":["StreamName",{data}]}]}  ← live feed
  //   "{}" or {"C":"..."}                                   ← ping / ack (ignored)
  // ─────────────────────────────────────────────
  void handleMessage(const String& raw) {
    if (raw.length() < 5 || raw == "{}") return;

    DynamicJsonDocument doc(2048);
    if (deserializeJson(doc, raw) != DeserializationError::Ok) return;

    // ── Initial snapshot ("R" key) ─────────────────────────────────────
    if (doc.containsKey("R")) {
      JsonVariant r = doc["R"];
      if (r.is<JsonObject>()) {
        JsonObject snap = r.as<JsonObject>();
        if (snap.containsKey("TrackStatus"))
          dispatchStream("TrackStatus",   snap["TrackStatus"].as<JsonObject>());
        if (snap.containsKey("SessionStatus"))
          dispatchStream("SessionStatus", snap["SessionStatus"].as<JsonObject>());
      }
    }

    // ── Live feed messages ("M" key) ──────────────────────────────────
    if (doc.containsKey("M")) {
      JsonVariant mVar = doc["M"];
      if (!mVar.is<JsonArray>()) return;
      for (JsonVariant entry : mVar.as<JsonArray>()) {
        if (!entry.is<JsonObject>()) continue;
        JsonObject msg = entry.as<JsonObject>();
        if (strcmp(msg["M"] | "", "feed") != 0) continue;
        JsonVariant argsVar = msg["A"];
        if (!argsVar.is<JsonArray>()) continue;
        JsonArray args = argsVar.as<JsonArray>();
        if (args.size() < 2 || !args[1].is<JsonObject>()) continue;
        dispatchStream(args[0] | "", args[1].as<JsonObject>());
      }
    }
  }

  // ─────────────────────────────────────────────
  // React to a named stream update
  // ─────────────────────────────────────────────
  void dispatchStream(const char* stream, JsonObject data) {

    // ── SessionStatus ────────────────────────────────────────────────
    if (strcmp(stream, "SessionStatus") == 0) {
      const char* status = data["Status"] | "";
      bool nowActive = (strcmp(status, "Started") == 0);
      DEBUG_PRINTF("[F1Lamp] SessionStatus: %s\n", status);

      if (nowActive && !_sessionActive) {
        _sessionActive = true;
        _currentState  = F1State::SESSION_START;
        applyStatePreset(_currentState);
      } else if (!nowActive && _sessionActive) {
        _sessionActive = false;
        _currentState  = F1State::CHEQUERED;
        applyStatePreset(_currentState);
      } else if (!nowActive) {
        _currentState = F1State::IDLE;
        applyStatePreset(_currentState);
      }
    }

    // ── TrackStatus ──────────────────────────────────────────────────
    else if (strcmp(stream, "TrackStatus") == 0) {
      const char* code = data["Status"]  | "1";
      const char* msg  = data["Message"] | "";
      DEBUG_PRINTF("[F1Lamp] TrackStatus: %s (%s)\n", code, msg);

      // If we receive a track status before a SessionStatus update,
      // assume the session is active (can happen on first snapshot).
      _sessionActive = true;
      F1State newState = trackCodeToState(code);
      if (newState != _currentState) {
        _currentState = newState;
        applyStatePreset(_currentState);
      }
    }

    // ── Heartbeat ────────────────────────────────────────────────────
    else if (strcmp(stream, "Heartbeat") == 0) {
      DEBUG_PRINTLN("[F1Lamp] Heartbeat ♥");
    }
  }

  // ─────────────────────────────────────────────
  // Map F1 track status code → F1State
  // ─────────────────────────────────────────────
  static F1State trackCodeToState(const char* code) {
    if (strcmp(code, "1") == 0)                                   return F1State::GREEN;
    if (strcmp(code, "2") == 0 || strcmp(code, "3") == 0)         return F1State::YELLOW;
    if (strcmp(code, "4") == 0 || strcmp(code, "7") == 0)         return F1State::SAFETY_CAR;
    if (strcmp(code, "5") == 0)                                   return F1State::RED_FLAG;
    if (strcmp(code, "6") == 0)                                   return F1State::VIRTUAL_SC;
    return F1State::GREEN;
  }

  // ─────────────────────────────────────────────
  // Apply the WLED preset for a given state.
  // Only acts when the state has actually changed.
  // ─────────────────────────────────────────────
  void applyStatePreset(F1State state) {
    if (state == _lastApplied) return;
    _lastApplied = state;

    uint8_t     pid   = 0;
    const char* label = "?";
    switch (state) {
      case F1State::IDLE:          pid = presetIdle;         label = "IDLE (pl.12)";          break;
      case F1State::SESSION_START: pid = presetSessionStart; label = "SESSION_START (pl.20)"; break;
      case F1State::GREEN:         pid = presetGreen;        label = "GREEN (pl.15)";         break;
      case F1State::YELLOW:        pid = presetYellow;       label = "YELLOW (pl.16)";        break;
      case F1State::SAFETY_CAR:    pid = presetSafetyCar;    label = "SAFETY_CAR (pl.13)";   break;
      case F1State::VIRTUAL_SC:    pid = presetVirtualSC;    label = "VIRTUAL_SC (pl.22)";   break;
      case F1State::RED_FLAG:      pid = presetRedFlag;      label = "RED_FLAG (pl.14)";      break;
      case F1State::CHEQUERED:     pid = presetChequered;    label = "CHEQUERED (pl.23)";     break;
      default: break;
    }

    DEBUG_PRINTF("[F1Lamp] → %s  preset=%d\n", label, pid);
    if (pid > 0) applyPreset(pid); // WLED built-in
  }

  // ─────────────────────────────────────────────
  // Exponential back-off: 5 → 10 → 20 → 40 → 80 → 120 s (capped)
  // ─────────────────────────────────────────────
  void scheduleReconnect() {
    _lastConnectMs = millis();
    DEBUG_PRINTF("[F1Lamp] Will reconnect in %us\n", _reconnectDelaySec);
    _reconnectDelaySec = min((uint32_t)120, _reconnectDelaySec * 2);
  }

  const char* stateLabel(F1State s) {
    switch (s) {
      case F1State::IDLE:          return "No session (idle)";
      case F1State::SESSION_START: return "Session starting";
      case F1State::GREEN:         return "Green flag";
      case F1State::YELLOW:        return "Yellow flag";
      case F1State::SAFETY_CAR:    return "Safety Car";
      case F1State::VIRTUAL_SC:    return "Virtual SC";
      case F1State::RED_FLAG:      return "Red flag";
      case F1State::CHEQUERED:     return "Chequered flag";
      default:                     return "---";
    }
  }

// ════════════════════════════════════════════════
// Usermod interface
// ════════════════════════════════════════════════
public:
  void setup() override {
    _instance = this;
    DEBUG_PRINTLN("[F1Lamp] SignalR usermod ready");
    // First connect attempt ~5 s after boot (give WiFi time)
    _lastConnectMs     = millis() - ((uint32_t)_reconnectDelaySec - 5) * 1000UL;
    _initDone          = true;
  }

  void loop() override {
    if (!enabled || !_initDone) return;
    // ── Test / force mode ────────────────────────────────────────────────────
    // When forceState > 0, bypass SignalR entirely and hold the lamp in the
    // requested state. Change via Settings → Usermods or the HTTP API.
    if (forceState > 0) {
      F1State forced = static_cast<F1State>(forceState - 1); // 1-indexed → 0-indexed enum
      if (forced != _lastApplied || forceState != _lastForce) {
        _lastForce    = forceState;
        _currentState = forced;
        applyStatePreset(forced);
      }
      return; // skip all SignalR logic while forced
    }
    if (_lastForce != 0) {
      // Force mode just turned off — reset so the next real event re-applies
      _lastForce   = 0;
      _lastApplied = F1State::UNKNOWN;
    }
    // Drive the WebSocket state machine — must be called every loop tick
    _ws.loop();

    unsigned long now = millis();

    // ── Connect / retry ──────────────────────────────────────────────
    if (!_wsConnected) {
      if (now - _lastConnectMs >= (uint32_t)_reconnectDelaySec * 1000UL) {
        _lastConnectMs = now;
        if (isWifiReady()) connectSignalR();
      }
      return;
    }

    // ── SignalR keepalive: re-subscribe every 5 min ──────────────────
    // Azure SignalR drops group subscriptions after ~20 min inactivity.
    // Re-sending Subscribe every 5 min prevents this — same as
    // _heartbeat() coroutine (asyncio.sleep 300) in signalr.py.
    if (now - _lastKeepAliveMs >= 5UL * 60UL * 1000UL) {
      sendSubscribe();
    }
  }

  // ─── WLED /info panel ────────────────────────────────────────────────────
  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");

    JsonArray row = user.createNestedArray(F("F1 Status"));
    row.add(stateLabel(_currentState));

    JsonArray row2 = user.createNestedArray(F("F1 Stream"));
    if (forceState > 0) {
      row2.add(F("TEST MODE"));
    } else {
      row2.add(_wsConnected ? (_subscribed ? F("live ✓") : F("connecting…")) : F("offline"));
    }
  }

  // ─── Hint text for WLED Usermods settings page ───────────────────────────
  void appendConfigData() override {
    oappend(SET_F(
      "addInfo('F1Lamp:reconnectInterval',1,'Seconds between reconnect attempts');"
      "addInfo('F1Lamp:forceState',1,'0=off 1=idle 2=start 3=green 4=yellow 5=SC 6=VSC 7=red 8=chequered');"
      "addInfo('F1Lamp:presetIdle',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetSessionStart',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetGreen',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetYellow',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetSafetyCar',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetVirtualSC',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetRedFlag',1,'Preset/playlist ID  |  0 = skip');"
      "addInfo('F1Lamp:presetChequered',1,'Preset/playlist ID  |  0 = skip');"
    ));
  }

  // ─── Config save ─────────────────────────────────────────────────────────
  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)]      = enabled;
    top["reconnectInterval"]  = reconnectIntervalSec;
    top["forceState"]         = forceState;
    top["presetIdle"]         = presetIdle;
    top["presetSessionStart"] = presetSessionStart;
    top["presetGreen"]        = presetGreen;
    top["presetYellow"]       = presetYellow;
    top["presetSafetyCar"]    = presetSafetyCar;
    top["presetVirtualSC"]    = presetVirtualSC;
    top["presetRedFlag"]      = presetRedFlag;
    top["presetChequered"]    = presetChequered;
  }

  // ─── Config load ─────────────────────────────────────────────────────────
  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;

    bool prevEnabled = enabled;
    getJsonValue(top[FPSTR(_enabled)],      enabled);
    getJsonValue(top["reconnectInterval"],   reconnectIntervalSec);
    getJsonValue(top["forceState"],          forceState);
    getJsonValue(top["presetIdle"],          presetIdle);
    getJsonValue(top["presetSessionStart"],  presetSessionStart);
    getJsonValue(top["presetGreen"],         presetGreen);
    getJsonValue(top["presetYellow"],        presetYellow);
    getJsonValue(top["presetSafetyCar"],     presetSafetyCar);
    getJsonValue(top["presetVirtualSC"],     presetVirtualSC);
    getJsonValue(top["presetRedFlag"],       presetRedFlag);
    getJsonValue(top["presetChequered"],     presetChequered);

    if (prevEnabled != enabled) {
      _lastApplied   = F1State::UNKNOWN;
      _sessionActive = false;
      if (!enabled && _wsConnected) {
        _ws.disconnect();
        _wsConnected = false;
      }
    }

    return true;
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

// Static member definitions
F1LampUsermod* F1LampUsermod::_instance = nullptr;

const char F1LampUsermod::_name[]    PROGMEM = "F1Lamp";
const char F1LampUsermod::_enabled[] PROGMEM = "enabled";
