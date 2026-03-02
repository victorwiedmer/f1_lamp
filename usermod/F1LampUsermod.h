#pragma once
/*
 * F1LampUsermod.h  -  WLED usermod for F1 live-timing reaction lamp.
 *
 * Custom web UI at  http://<device>/f1
 *   - WiFi setup (SSID + password)
 *   - Preset ID configuration
 *   - Live F1 status with auto-refresh
 *   - Force-state test buttons
 *   - Brightness slider
 *   - Reboot button
 *
 * Includes ONLY wled.h + F1NetWork.h (pure-C interface).
 * All IDF HTTP/WS code lives in F1NetWork.cpp (never includes wled.h).
 */

#include "wled.h"
#include "F1NetWork.h"

/* ----------------------------------------------------------------
   Embedded HTML page (raw string literal – quotes/backslashes safe)
   ---------------------------------------------------------------- */
static const char F1_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>F1 Lamp</title>
<style>
*{box-sizing:border-box}
body{background:#111;color:#eee;font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px}
h1{color:#e10600;margin:0 0 4px;font-size:1.6rem}
.sub{color:#555;font-size:.8rem;margin-bottom:16px}
.card{background:#1e1e1e;border-radius:8px;padding:16px;margin-bottom:14px}
h2{margin:0 0 12px;font-size:.75rem;color:#777;text-transform:uppercase;letter-spacing:1px}
.status{font-size:1.8rem;font-weight:bold;text-align:center;padding:10px 8px;border-radius:6px;margin-bottom:6px;transition:background .4s}
.conn{text-align:center;font-size:.82rem;margin-top:2px}
label{display:block;font-size:.82rem;color:#888;margin:10px 0 3px}
input[type=text],input[type=password],input[type=number]{width:100%;background:#2a2a2a;border:1px solid #333;color:#eee;padding:8px 10px;border-radius:4px;font-size:.95rem}
input[type=number]{width:100%}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:8px}
.btn{display:inline-block;padding:9px 18px;border-radius:4px;border:none;cursor:pointer;font-size:.88rem;font-weight:600;margin:3px 2px}
.btn-save{background:#00d2be;color:#111}
.btn-reboot{background:#e10600;color:#fff}
.ok{color:#00d2be;font-size:.8rem;margin-left:8px;display:none}
.btn-row{display:flex;flex-wrap:wrap;gap:5px;margin-top:4px}
.sb{flex:1;min-width:72px;padding:9px 4px;text-align:center;border-radius:4px;border:none;cursor:pointer;font-size:.78rem;font-weight:700}
input[type=range]{width:100%;accent-color:#e10600;margin-top:4px}
.bv{text-align:center;font-size:.85rem;color:#888;margin-top:2px}
</style></head>
<body>
<h1>&#127937; F1 Lamp</h1>
<div class="sub" id="ipLine">loading...</div>

<div class="card">
  <h2>Track Status</h2>
  <div class="status" id="stDiv" style="background:#333">&#8212;</div>
  <div class="conn" id="cnDiv">&#8212;</div>
</div>

<div class="card">
  <h2>Test / Force State</h2>
  <div class="btn-row">
    <button class="sb" style="background:#444;color:#eee" onclick="force(0)">&#127358; Live</button>
    <button class="sb" style="background:#555;color:#eee" onclick="force(1)">Idle</button>
    <button class="sb" style="background:#00b33a;color:#fff" onclick="force(3)">Green&#9872;</button>
    <button class="sb" style="background:#e6b800;color:#111" onclick="force(4)">Yellow&#9940;</button>
    <button class="sb" style="background:#ff6600;color:#fff" onclick="force(5)">SC&#128993;</button>
    <button class="sb" style="background:#ff9900;color:#111" onclick="force(6)">VSC</button>
    <button class="sb" style="background:#e10600;color:#fff" onclick="force(7)">Red&#128681;</button>
    <button class="sb" style="background:#eee;color:#111" onclick="force(8)">Chq&#127937;</button>
    <button class="sb" style="background:#2244bb;color:#fff" onclick="force(2)">Start&#9193;</button>
  </div>
</div>

<div class="card">
  <h2>Brightness</h2>
  <input type="range" id="briR" min="1" max="255" value="128" oninput="setBri(this.value)">
  <div class="bv" id="briV">128</div>
</div>

<div class="card">
  <h2>WiFi Setup</h2>
  <label>Network (SSID)</label>
  <input type="text" id="ssid" autocomplete="off" spellcheck="false">
  <label>Password</label>
  <input type="password" id="pass" autocomplete="new-password">
  <div style="margin-top:12px">
    <button class="btn btn-save" onclick="saveWifi()">Save &amp; Reconnect</button>
    <span class="ok" id="wOk">&#10003; Saved &mdash; reconnecting&hellip;</span>
  </div>
</div>

<div class="card">
  <h2>Preset / Playlist IDs &nbsp;<span style="color:#555;font-weight:400;font-size:.75rem">(0 = skip)</span></h2>
  <div class="grid2">
    <div><label>Idle</label><input type="number" id="p0" min="0" max="250"></div>
    <div><label>Session Start</label><input type="number" id="p1" min="0" max="250"></div>
    <div><label>Green Flag</label><input type="number" id="p2" min="0" max="250"></div>
    <div><label>Yellow Flag</label><input type="number" id="p3" min="0" max="250"></div>
    <div><label>Safety Car</label><input type="number" id="p4" min="0" max="250"></div>
    <div><label>Virtual SC</label><input type="number" id="p5" min="0" max="250"></div>
    <div><label>Red Flag</label><input type="number" id="p6" min="0" max="250"></div>
    <div><label>Chequered</label><input type="number" id="p7" min="0" max="250"></div>
  </div>
  <div style="margin-top:12px">
    <button class="btn btn-save" onclick="savePresets()">Save Presets</button>
    <span class="ok" id="pOk">&#10003; Saved</span>
  </div>
</div>

<div class="card" style="text-align:center">
  <button class="btn btn-reboot" onclick="if(confirm('Reboot device?')){fetch('/f1/reboot',{method:'POST'});document.body.style.opacity='.4'}">Reboot</button>
</div>

<script>
const SN=['&#8212;','Idle','Starting','Green Flag','Yellow Flag','Safety Car','Virtual SC','Red Flag','Chequered'];
const SC=['#333','#555','#1a3a8a','#00b33a','#e6b800','#ff6600','#ff9900','#e10600','#f0f0f0'];
const ST=['#eee','#eee','#fff','#fff','#111','#fff','#111','#fff','#111'];
let briT,lastBri=-1;

async function refresh(){
  try{
    const d=await(await fetch('/f1/status')).json();
    const s=d.state??0;
    document.getElementById('stDiv').innerHTML=SN[s]??'?';
    document.getElementById('stDiv').style.background=SC[s];
    document.getElementById('stDiv').style.color=ST[s];
    document.getElementById('cnDiv').textContent=d.connected?'\u25cf Connected to livetiming.formula1.com':'\u25cb Offline / No session';
    document.getElementById('cnDiv').style.color=d.connected?'#00d2be':'#666';
    document.getElementById('ipLine').textContent='http://'+location.host+'/f1   \u2022  WiFi: '+(d.ssid||'(AP mode)');
    if(d.bri!=null&&d.bri!==lastBri){lastBri=d.bri;document.getElementById('briR').value=d.bri;document.getElementById('briV').textContent=d.bri;}
    if(d.presets&&!document.activeElement.id.startsWith('p')){
      for(let i=0;i<8;i++)document.getElementById('p'+i).value=d.presets[i]||0;
    }
    if(d.ssid&&document.getElementById('ssid').value==='')
      document.getElementById('ssid').placeholder=d.ssid;
  }catch(e){}
}

function force(n){fetch('/f1/force?s='+n,{method:'POST'});}

function setBri(v){
  document.getElementById('briV').textContent=v;
  clearTimeout(briT);
  briT=setTimeout(()=>{lastBri=+v;fetch('/f1/bri?v='+v,{method:'POST'});},250);
}

async function saveWifi(){
  const s=document.getElementById('ssid').value.trim();
  const p=document.getElementById('pass').value;
  if(!s){document.getElementById('ssid').focus();return;}
  await fetch('/f1/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p)});
  const el=document.getElementById('wOk');el.style.display='inline';
  setTimeout(()=>el.style.display='none',5000);
}

async function savePresets(){
  let b='';for(let i=0;i<8;i++)b+=(b?'&':'')+'p'+i+'='+document.getElementById('p'+i).value;
  await fetch('/f1/presets',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b});
  const el=document.getElementById('pOk');el.style.display='inline';
  setTimeout(()=>el.style.display='none',3000);
}

refresh();setInterval(refresh,3000);
</script></body></html>
)html";

/* ================================================================
   Usermod class
   ================================================================ */
class F1LampUsermod : public Usermod {
public:
  static const char _name[];
  static const char _enabled[];

private:
  /* ---- Configurable ---- */
  bool    enabled            = true;
  uint8_t forceState         = 0;
  uint8_t presetIdle         = 12;
  uint8_t presetSessionStart = 20;
  uint8_t presetGreen        = 15;
  uint8_t presetYellow       = 16;
  uint8_t presetSafetyCar    = 13;
  uint8_t presetVirtualSC    = 22;
  uint8_t presetRedFlag      = 14;
  uint8_t presetChequered    = 23;

  /* ---- Runtime ---- */
  bool       _initDone         = false;
  bool       _routesRegistered = false;
  F1NetState _lastApplied      = F1ST_UNKNOWN;
  uint8_t    _lastForce        = 0;

  static volatile F1NetState s_pending;
  static volatile bool       s_pendingValid;

  /* ---- Helpers ---- */
  uint8_t presetForState(F1NetState s) const {
    switch (s) {
      case F1ST_IDLE:          return presetIdle;
      case F1ST_SESSION_START: return presetSessionStart;
      case F1ST_GREEN:         return presetGreen;
      case F1ST_YELLOW:        return presetYellow;
      case F1ST_SAFETY_CAR:    return presetSafetyCar;
      case F1ST_VIRTUAL_SC:    return presetVirtualSC;
      case F1ST_RED_FLAG:      return presetRedFlag;
      case F1ST_CHEQUERED:     return presetChequered;
      default: return 0;
    }
  }

  static const char* stateLabel(F1NetState s) {
    switch (s) {
      case F1ST_IDLE:          return "No session";
      case F1ST_SESSION_START: return "Session starting";
      case F1ST_GREEN:         return "Green flag";
      case F1ST_YELLOW:        return "Yellow flag";
      case F1ST_SAFETY_CAR:    return "Safety Car";
      case F1ST_VIRTUAL_SC:    return "Virtual SC";
      case F1ST_RED_FLAG:      return "Red flag";
      case F1ST_CHEQUERED:     return "Chequered flag";
      default:                 return "---";
    }
  }

  void applyStatePreset(F1NetState state) {
    if (state == _lastApplied) return;
    _lastApplied = state;
    uint8_t pid = presetForState(state);
    DEBUG_PRINTF("[F1Lamp] -> %s  preset=%d\n", stateLabel(state), pid);
    if (pid > 0) applyPreset(pid);
  }

  static void onStateChange(F1NetState state) {
    s_pending      = state;
    s_pendingValid = true;
  }

  /* ---- Route registration ---- */
  void registerRoutes() {
    // GET /f1  ->  custom HTML page
    server.on("/f1", HTTP_GET, [](AsyncWebServerRequest* req) {
      AsyncWebServerResponse* resp = req->beginResponse_P(
          200, "text/html", (const uint8_t*)F1_HTML, strlen_P(F1_HTML));
      resp->addHeader("Cache-Control", "no-cache");
      req->send(resp);
    });

    // GET /f1/status  ->  JSON
    server.on("/f1/status", HTTP_GET, [this](AsyncWebServerRequest* req) {
      char buf[320];
      snprintf(buf, sizeof(buf),
        "{\"state\":%d,\"connected\":%s,\"ssid\":\"%s\","
        "\"bri\":%d,"
        "\"presets\":[%d,%d,%d,%d,%d,%d,%d,%d]}",
        (int)_lastApplied,
        f1net_isConnected() ? "true" : "false",
        multiWiFi[0].clientSSID,
        (int)bri,
        presetIdle, presetSessionStart, presetGreen, presetYellow,
        presetSafetyCar, presetVirtualSC, presetRedFlag, presetChequered
      );
      req->send(200, "application/json", buf);
    });

    // POST /f1/force?s=N
    server.on("/f1/force", HTTP_POST, [this](AsyncWebServerRequest* req) {
      if (req->hasParam("s", true)) {
        forceState = (uint8_t)req->getParam("s", true)->value().toInt();
      } else if (req->hasParam("s")) {
        forceState = (uint8_t)req->getParam("s")->value().toInt();
      }
      req->send(200, "text/plain", "ok");
    });

    // POST /f1/bri?v=N
    server.on("/f1/bri", HTTP_POST, [](AsyncWebServerRequest* req) {
      String v = req->hasParam("v", true) ? req->getParam("v", true)->value()
               : req->hasParam("v")       ? req->getParam("v")->value()
               : String("128");
      bri = (uint8_t)constrain(v.toInt(), 1, 255);
      stateUpdated(CALL_MODE_DIRECT_CHANGE);
      req->send(200, "text/plain", "ok");
    });

    // POST /f1/wifi  (form body: ssid=...&pass=...)
    server.on("/f1/wifi", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (req->hasParam("ssid", true)) {
        String s = req->getParam("ssid", true)->value();
        String p = req->hasParam("pass", true)
                   ? req->getParam("pass", true)->value() : "";
        strlcpy(multiWiFi[0].clientSSID, s.c_str(), 33);
        strlcpy(multiWiFi[0].clientPass, p.c_str(), 65);
        serializeConfigToFS();   // persist to flash
        forceReconnect = true;   // reconnect with new credentials
      }
      req->send(200, "text/plain", "ok");
    });

    // POST /f1/presets  (form body: p0=..&p1=..&...&p7=..)
    server.on("/f1/presets", HTTP_POST, [this](AsyncWebServerRequest* req) {
      auto get = [&](const char* key) -> uint8_t {
        return req->hasParam(key, true)
               ? (uint8_t)req->getParam(key, true)->value().toInt() : 0;
      };
      presetIdle         = get("p0");
      presetSessionStart = get("p1");
      presetGreen        = get("p2");
      presetYellow       = get("p3");
      presetSafetyCar    = get("p4");
      presetVirtualSC    = get("p5");
      presetRedFlag      = get("p6");
      presetChequered    = get("p7");
      serializeConfigToFS();
      req->send(200, "text/plain", "ok");
    });

    // POST /f1/reboot
    server.on("/f1/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
      req->send(200, "text/plain", "rebooting");
      doReboot = true;
    });
  }

/* ================================================
   Usermod interface
   ================================================ */
public:
  void setup() override {
    f1net_setCallback(F1LampUsermod::onStateChange);
    f1net_setup();
    _initDone = true;
    DEBUG_PRINTLN("[F1Lamp] setup done");
  }

  void connected() override {
    if (!_routesRegistered) {
      registerRoutes();
      _routesRegistered = true;
      DEBUG_PRINTLN("[F1Lamp] Routes registered – UI at http://<IP>/f1");
    }
  }

  void loop() override {
    if (!enabled || !_initDone) return;

    if (forceState > 0) {
      F1NetState forced = (F1NetState)(forceState - 1);
      if (forced != _lastApplied || forceState != _lastForce) {
        _lastForce = forceState;
        applyStatePreset(forced);
      }
      return;
    }
    if (_lastForce != 0) {
      _lastForce   = 0;
      _lastApplied = F1ST_UNKNOWN;
    }

    if (s_pendingValid) {
      s_pendingValid = false;
      applyStatePreset((F1NetState)s_pending);
    }

    f1net_loop();
  }

  /* Minimal WLED info panel entry (just a link) */
  void addToJsonInfo(JsonObject& root) override {
    JsonObject user = root["u"];
    if (user.isNull()) user = root.createNestedObject("u");
    JsonArray r1 = user.createNestedArray(F("F1 Status"));
    r1.add(stateLabel(_lastApplied));
    JsonArray r2 = user.createNestedArray(F("F1 UI"));
    r2.add(F("/f1"));
  }

  void addToConfig(JsonObject& root) override {
    JsonObject top = root.createNestedObject(FPSTR(_name));
    top[FPSTR(_enabled)]      = enabled;
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

  bool readFromConfig(JsonObject& root) override {
    JsonObject top = root[FPSTR(_name)];
    if (top.isNull()) return false;
    bool prev = enabled;
    getJsonValue(top[FPSTR(_enabled)],      enabled);
    getJsonValue(top["forceState"],          forceState);
    getJsonValue(top["presetIdle"],          presetIdle);
    getJsonValue(top["presetSessionStart"],  presetSessionStart);
    getJsonValue(top["presetGreen"],         presetGreen);
    getJsonValue(top["presetYellow"],        presetYellow);
    getJsonValue(top["presetSafetyCar"],     presetSafetyCar);
    getJsonValue(top["presetVirtualSC"],     presetVirtualSC);
    getJsonValue(top["presetRedFlag"],       presetRedFlag);
    getJsonValue(top["presetChequered"],     presetChequered);
    if (prev && !enabled) f1net_disconnect();
    return true;
  }

  uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
};

const char F1LampUsermod::_name[]    PROGMEM = "F1Lamp";
const char F1LampUsermod::_enabled[] PROGMEM = "enabled";

volatile F1NetState F1LampUsermod::s_pending      = F1ST_IDLE;
volatile bool       F1LampUsermod::s_pendingValid  = false;
