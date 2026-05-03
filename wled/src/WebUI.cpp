/*
 * WebUI.cpp  –  ESPAsyncWebServer routes for F1 Lamp
 *
 * HTML/CSS/JS files are served from LittleFS (/data directory).
 * Only the minimal AP-mode WiFi-setup page is kept inline as a
 * fallback in case LittleFS is empty or corrupted.
 *
 * All API routes are registered here.
 */

#include "WebUI.h"
#include "Config.h"
#include "LedFx.h"
#include "F1NetWork.h"
#include "F1Calendar.h"
#include "Replay.h"
#include "F1Sessions.h"
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <Update.h>

bool g_apMode = false;

#define FW_VERSION "1.0.0-2026.05.02"

/* ── global server instance (port 80) ───────────────────────────────────── */
static AsyncWebServer s_server(80);

/* ── Minimal inline AP-mode WiFi setup page (fallback) ──────────────────── */
static const char AP_WIFI_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>F1 Lamp – WiFi</title>
<style>
body{background:#111;color:#eee;font-family:sans-serif;margin:0;padding:20px;max-width:400px;margin:auto}
h2{color:#e10600;margin-top:0}
input[type=text],input[type=password]{width:100%;padding:10px;margin:6px 0 12px;background:#222;color:#eee;border:1px solid #444;border-radius:6px;box-sizing:border-box;font-size:1rem}
button{padding:10px 20px;border:none;border-radius:6px;font-size:1rem;cursor:pointer;margin:4px 2px}
.btn-scan{background:#333;color:#aaa;font-size:.85rem;padding:8px 14px}
.btn-save{background:#00d2be;color:#111;font-weight:bold;width:100%}
.net{padding:8px;border-bottom:1px solid #333;cursor:pointer}
.net:hover{background:#222}
.ok{color:#0c8;display:none}
.info{color:#666;font-size:.8rem;margin-top:20px}
</style></head><body>
<h2>F1 Lamp WiFi</h2>
<div style="display:flex;align-items:center;gap:6px;margin-bottom:4px">
  <label>SSID</label>
  <button class="btn-scan" id="sb" onclick="doScan()">Scan</button>
</div>
<div id="nl"></div>
<input type="text" id="ss" placeholder="Network name">
<label>Password</label>
<input type="password" id="pw" placeholder="WiFi password">
<button class="btn-save" onclick="doSave()">Save &amp; Reconnect</button>
<span class="ok" id="ok">Saved – rebooting…</span>
<div class="info" id="di">Loading…</div>
<script>
async function doScan(){
  const b=document.getElementById('sb'),l=document.getElementById('nl');
  b.disabled=true;b.textContent='Scanning…';
  l.innerHTML='<span style="color:#555">Scanning…</span>';
  try{
    for(let i=0;i<30;i++){
      const r=await fetch('/api/scan');const d=await r.json();
      if(d.scanning){await new Promise(r=>setTimeout(r,500));continue;}
      l.innerHTML='';
      if(!d.length){l.innerHTML='<span style="color:#555">No networks found</span>';return;}
      d.forEach(n=>{
        const el=document.createElement('div');el.className='net';
        el.textContent=n.ssid+' ('+n.rssi+'dBm)';
        el.onclick=()=>{document.getElementById('ss').value=n.ssid;document.getElementById('pw').focus();};
        l.appendChild(el);
      });
      return;
    }
    l.innerHTML='<span style="color:#e10600">Scan timed out</span>';
  }catch(e){l.innerHTML='<span style="color:#e10600">Scan failed: '+e.message+'</span>';}
  finally{b.disabled=false;b.textContent='Scan';}
}
async function doSave(){
  const ssid=document.getElementById('ss').value;
  const pass=document.getElementById('pw').value;
  if(!ssid){alert('Enter an SSID');return;}
  try{
    const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
    if(r.ok){document.getElementById('ok').style.display='inline';return;}
  }catch(e){}
  try{
    const u='/api/wifi/set?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
    const r2=await fetch(u);
    if(r2.ok){document.getElementById('ok').style.display='inline';return;}
  }catch(e){}
  alert('Save failed');
}
try{
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('di').textContent=
      'Heap: '+(d.heap||'?')+' B  |  SSID: '+(d.ssid||'(none)')+'  |  AP: '+(d.ap?'Yes':'No');
    if(d.ssid)document.getElementById('ss').value=d.ssid;
  }).catch(()=>{});
}catch(e){}
</script></body></html>
)html";

/* ═══════════════════════════════════════════════════════════════════════════
   Route helpers
   ═══════════════════════════════════════════════════════════════════════════ */

static void sendJson(AsyncWebServerRequest* req, const String& json) {
    req->send(200, "application/json", json);
}

static void sendOk(AsyncWebServerRequest* req) {
    sendJson(req, "{\"ok\":true}");
}

/* Serve a LittleFS file, with Cache-Control for static assets */
static void serveFile(AsyncWebServerRequest* req, const char* path,
                      const char* contentType, bool cache = false) {
    if (LittleFS.exists(path)) {
        AsyncWebServerResponse* resp = req->beginResponse(LittleFS, path, contentType);
        if (cache) {
            resp->addHeader("Cache-Control", "public, max-age=86400");
        } else {
            resp->addHeader("Cache-Control", "no-cache");
        }
        req->send(resp);
    } else {
        req->send(404, "text/plain", "File not found");
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
   Public init
   ═══════════════════════════════════════════════════════════════════════════ */

void webui_init(
    std::function<void(uint8_t state)>   onForcedState,
    std::function<void()>                onReboot,
    std::function<void(uint8_t evType)>  onTestEvent
) {
    /* ── GET /  →  index.html from LittleFS (STA) or AP WiFi setup ─────── */
    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (g_apMode) {
            /* In AP mode, serve the inline WiFi page as fallback */
            if (LittleFS.exists("/wifi.html")) {
                serveFile(req, "/wifi.html", "text/html");
            } else {
                AsyncWebServerResponse* resp = req->beginResponse(
                    200, "text/html", (const uint8_t*)AP_WIFI_HTML, strlen_P(AP_WIFI_HTML));
                resp->addHeader("Cache-Control", "no-cache");
                req->send(resp);
            }
            return;
        }
        serveFile(req, "/index.html", "text/html");
    });

    /* ── Static assets from LittleFS ───────────────────────────────────── */
    s_server.on("/common.css", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/common.css", "text/css", true);
    });
    s_server.on("/common.js", HTTP_GET, [](AsyncWebServerRequest* req) {
        /* no-cache: JS changes with every firmware build */
        serveFile(req, "/common.js", "application/javascript", false);
    });
    s_server.on("/effects.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/effects.html", "text/html");
    });
    s_server.on("/features.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/features.html", "text/html");
    });
    s_server.on("/wifi.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/wifi.html", "text/html");
    });
    s_server.on("/sessions.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/sessions.html", "text/html");
    });
    s_server.on("/settings.html", HTTP_GET, [](AsyncWebServerRequest* req) {
        serveFile(req, "/settings.html", "text/html");
    });

    /* ── GET /api/status ────────────────────────────────────────────────── */
    s_server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["state"]     = (int)f1net_getState();
        doc["connected"] = f1net_isConnected();
        doc["ssid"]      = g_cfg.ssid;
        doc["bri"]       = (int)g_cfg.brightness;
        doc["f1ok"]      = f1net_isConnected();
        doc["ap"]        = g_apMode;
        doc["power"]     = g_cfg.power;
        doc["nextRace"]  = f1cal_hasData() ? f1cal_nextRaceLabel() : "—";
        doc["rampPct"]   = (int)(f1cal_idleFactor() * 100.0f);
        doc["heap"]      = (int)ESP.getFreeHeap();
        doc["version"]   = FW_VERSION;
        doc["ip"]        = WiFi.localIP().toString();
        doc["rssi"]      = (int)WiFi.RSSI();
        /* active effect details */
        LedFxInfo fx = ledfx_getActiveEffect();
        JsonObject eff = doc["eff"].to<JsonObject>();
        eff["e"]  = fx.effect;
        eff["r"]  = fx.r;  eff["g"]  = fx.g;  eff["b"]  = fx.b;
        eff["r2"] = fx.r2; eff["g2"] = fx.g2; eff["b2"] = fx.b2;
        eff["spd"] = fx.speed;
        eff["fl"]  = fx.flashing;
        eff["rp"]  = replay_isActive();
        String out; serializeJson(doc, out);
        sendJson(req, out);
    });

    /* ── GET /api/config ────────────────────────────────────────────────── */
    s_server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["ssid"]       = g_cfg.ssid;
        doc["pass"]       = "";   /* never send password back */
        doc["led_count"]  = g_cfg.led_count;
        doc["f_count"]    = g_cfg.f_count;
        doc["led_pin"]    = LED_PIN;
        doc["brightness"] = g_cfg.brightness;
        doc["delay_s"]    = g_cfg.delay_s;
        JsonArray arr = doc["states"].to<JsonArray>();
        for (int i = 0; i < CFG_NUM_STATES; i++) {
          JsonObject s = arr.add<JsonObject>();
          s["effect"]   = g_cfg.states[i].effect;
          s["r"]        = g_cfg.states[i].r;
          s["g"]        = g_cfg.states[i].g;
          s["b"]        = g_cfg.states[i].b;
          s["speed"]    = g_cfg.states[i].speed;
          s["r2"]       = g_cfg.states[i].r2;
          s["g2"]       = g_cfg.states[i].g2;
          s["b2"]       = g_cfg.states[i].b2;
          s["revert_s"] = g_cfg.states[i].revert_s;
        }
        String out;
        serializeJson(doc, out);
        sendJson(req, out);
    });

    /* ── POST /api/config  (JSON body) ──────────────────────────────────── */
    auto cfgHandler = new AsyncCallbackJsonWebHandler("/api/config",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
          JsonObject obj = json.as<JsonObject>();
          if (obj["ssid"].is<const char*>())
            strlcpy(g_cfg.ssid, obj["ssid"] | "", sizeof(g_cfg.ssid));
          const char* p = obj["pass"] | "";
          if (p && p[0] != '\0')
            strlcpy(g_cfg.pass, p, sizeof(g_cfg.pass));
          if (obj["led_count"].is<int>()) {
            g_cfg.led_count = obj["led_count"] | 22;
            ledfx_setCount(g_cfg.led_count);
          }
          if (obj["f_count"].is<int>()) {
            g_cfg.f_count = obj["f_count"] | 17;
            ledfx_setFCount(g_cfg.f_count);
          }
          if (obj["brightness"].is<int>()) {
            g_cfg.brightness = obj["brightness"] | 128;
            ledfx_setBrightness(g_cfg.brightness);
          }
          if (obj["delay_s"].is<int>()) {
            g_cfg.delay_s = obj["delay_s"] | 40;
          }
          JsonArray arr = obj["states"].as<JsonArray>();
          for (int i = 0; i < CFG_NUM_STATES && i < (int)arr.size(); i++) {
            JsonObject s = arr[i];
            g_cfg.states[i].effect   = s["effect"]   | (uint8_t)0;
            g_cfg.states[i].r        = s["r"]        | (uint8_t)0;
            g_cfg.states[i].g        = s["g"]        | (uint8_t)0;
            g_cfg.states[i].b        = s["b"]        | (uint8_t)0;
            g_cfg.states[i].speed    = s["speed"]    | (uint8_t)50;
            g_cfg.states[i].r2       = s["r2"]       | (uint8_t)0;
            g_cfg.states[i].g2       = s["g2"]       | (uint8_t)0;
            g_cfg.states[i].b2       = s["b2"]       | (uint8_t)0;
            g_cfg.states[i].revert_s = s["revert_s"] | (uint8_t)0;
          }
          cfg_save();
          sendOk(req);
        });
    s_server.addHandler(cfgHandler);

    /* ── POST /api/wifi  (JSON body) ─────────────────────────────────────── */
    auto wifiHandler = new AsyncCallbackJsonWebHandler("/api/wifi",
        [onReboot](AsyncWebServerRequest* req, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            strlcpy(g_cfg.ssid, obj["ssid"] | "", sizeof(g_cfg.ssid));
            const char* p = obj["pass"] | "";
            if (p && p[0] != '\0')
                strlcpy(g_cfg.pass, p, sizeof(g_cfg.pass));
            cfg_save();
            sendOk(req);
            delay(200);
            ESP.restart();
        });
    wifiHandler->setMethod(HTTP_POST);
    s_server.addHandler(wifiHandler);

    /* ── GET /api/wifi/set?ssid=X&pass=Y  (fallback for AP mode) ─────── */
    s_server.on("/api/wifi/set", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (!req->hasParam("ssid")) { req->send(400, "text/plain", "missing ssid"); return; }
        strlcpy(g_cfg.ssid, req->getParam("ssid")->value().c_str(), sizeof(g_cfg.ssid));
        if (req->hasParam("pass")) {
            const String& pv = req->getParam("pass")->value();
            if (pv.length() > 0)
                strlcpy(g_cfg.pass, pv.c_str(), sizeof(g_cfg.pass));
        }
        cfg_save();
        req->send(200, "text/html", "<html><body style='background:#111;color:#eee;font-family:sans-serif;padding:40px;text-align:center'>"
                   "<h2 style='color:#00d2be'>WiFi Saved!</h2><p>SSID: " + String(g_cfg.ssid) + "</p>"
                   "<p>Rebooting in 2 seconds...</p></body></html>");
        delay(200);
        ESP.restart();
    });

    /* ── POST /api/force?s=N ─────────────────────────────────────────────── */
    s_server.on("/api/force", HTTP_POST, [onForcedState](AsyncWebServerRequest* req) {
        if (req->hasParam("s")) {
            uint8_t s = (uint8_t)req->getParam("s")->value().toInt();
            onForcedState(s);
        }
        sendOk(req);
    });

    /* ── POST /api/bri?v=N ──────────────────────────────────────────────── */
    s_server.on("/api/bri", HTTP_POST, [](AsyncWebServerRequest* req) {
        if (req->hasParam("v")) {
            g_cfg.brightness = (uint8_t)constrain(
                req->getParam("v")->value().toInt(), 1, 255);
            ledfx_setBrightness(g_cfg.brightness);
        }
        sendOk(req);
    });

    /* ── POST /api/reboot ────────────────────────────────────────────────── */
    s_server.on("/api/reboot", HTTP_POST, [onReboot](AsyncWebServerRequest* req) {
        sendOk(req);
        delay(200);
        ESP.restart();
    });

    /* ── POST /api/power  {"on": true|false} ──────────────────────────── */
    auto pwrHandler = new AsyncCallbackJsonWebHandler("/api/power",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            g_cfg.power = json["on"] | true;
            if (!g_cfg.power) ledfx_allOff();
            cfg_save();
            sendOk(req);
        });
    s_server.addHandler(pwrHandler);

    /* ── GET /api/features  →  feature-flag JSON ────────────────────────── */
    s_server.on("/api/features", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["winner"]       = g_cfg.feat_winner;
        doc["fastest_lap"]  = g_cfg.feat_fastest_lap;
        doc["drs"]          = g_cfg.feat_drs;
        doc["start_lights"] = g_cfg.feat_start_lights;
        doc["deep_sleep"]   = g_cfg.deep_sleep;
        String out; serializeJson(doc, out);
        sendJson(req, out);
    });

    /* ── POST /api/features  {"winner":true, ...}  ───────────────────────── */
    auto featHandler = new AsyncCallbackJsonWebHandler("/api/features",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            if (obj["winner"].is<bool>())       g_cfg.feat_winner       = obj["winner"];
            if (obj["fastest_lap"].is<bool>())  g_cfg.feat_fastest_lap  = obj["fastest_lap"];
            if (obj["drs"].is<bool>())          g_cfg.feat_drs          = obj["drs"];
            if (obj["start_lights"].is<bool>()) g_cfg.feat_start_lights = obj["start_lights"];
            if (obj["deep_sleep"].is<bool>())   g_cfg.deep_sleep        = obj["deep_sleep"];
            cfg_save();
            sendOk(req);
        });
    s_server.addHandler(featHandler);

    /* ── GET /api/scan  →  WiFi scan ───────────────────────────────────── */
    s_server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
            sendJson(req, "{\"scanning\":true}");
            return;
        }
        if (n == WIFI_SCAN_RUNNING) {
            sendJson(req, "{\"scanning\":true}");
            return;
        }
        String out = "[";
        for (int i = 0; i < n; i++) {
            if (i) out += ",";
            String ssid = WiFi.SSID(i);
            ssid.replace("\\", "\\\\");
            ssid.replace("\"", "\\\"");
            out += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + String(WiFi.RSSI(i)) + "}";
        }
        out += "]";
        WiFi.scanDelete();
        sendJson(req, out);
    });

    /* ── POST /api/replay/start  body: {"speed":10} ───────────────────── */
    auto rpStartHandler = new AsyncCallbackJsonWebHandler("/api/replay/start",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            JsonObject obj = json.as<JsonObject>();
            float speed = obj["speed"] | 5.0f;
            replay_start(speed);
            sendOk(req);
        });
    s_server.addHandler(rpStartHandler);

    /* ── POST /api/replay/stop ──────────────────────────────────────── */
    s_server.on("/api/replay/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
        replay_stop();
        sendOk(req);
    });

    /* ── GET /api/replay/status ─────────────────────────────────────── */
    s_server.on("/api/replay/status", HTTP_GET, [](AsyncWebServerRequest* req) {
        JsonDocument doc;
        doc["loading"] = replay_isLoading() || f1sessions_isReplayFetching();
        doc["active"]  = replay_isActive();
        doc["idx"]     = replay_currentIdx();
        doc["total"]   = replay_eventCount();
        doc["speed"]   = (int)replay_speed();
        ReplayLastEvent le = replay_lastEvent();
        if (le.valid) {
            JsonObject ev   = doc["ev"].to<JsonObject>();
            ev["ts"]        = le.ts_ms;
            ev["topic"]     = le.topic;
            ev["data"]      = le.data;
            ev["simMs"]     = le.simMs;
            ev["totalMs"]   = le.totalMs;
        }
        String out; serializeJson(doc, out);
        sendJson(req, out);
    });

    /* ── POST /api/replay/session  body: {"path":"...","speed":5} ──── */
    auto rpSessHandler = new AsyncCallbackJsonWebHandler("/api/replay/session",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            const char* path = json["path"] | "";
            float speed = json["speed"] | 5.0f;
            if (!path || !path[0]) {
                req->send(400, "application/json", "{\"error\":\"missing path\"}");
                return;
            }
            f1sessions_requestReplay(path, speed);
            sendOk(req);
        });
    s_server.addHandler(rpSessHandler);

    /* ── GET /api/sessions ──────────────────────────────────────────── */
    s_server.on("/api/sessions", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (f1sessions_isFetching()) {
            sendJson(req, "{\"loading\":true}");
            return;
        }
        if (!f1sessions_hasData()) {
            const String& err = f1sessions_lastError();
            if (err.length() > 0) {
                String j = "{\"error\":\"" + err + "\"}";
                sendJson(req, j);
                return;
            }
            f1sessions_requestFetch();
            sendJson(req, "{\"loading\":true}");
            return;
        }
        sendJson(req, f1sessions_json());
    });

    /* ── GET /api/sessions_debug ────────────────────────────────────── */
    s_server.on("/api/sessions_debug", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (req->hasParam("reset")) {
            f1sessions_resetRetries();
            f1sessions_clear();
        }
        String j = "{\"fetching\":" + String(f1sessions_isFetching() ? "true" : "false")
                 + ",\"hasData\":" + String(f1sessions_hasData() ? "true" : "false")
                 + ",\"requested\":" + String(f1sessions_fetchRequested() ? "true" : "false")
                 + ",\"error\":\"" + f1sessions_lastError() + "\""
                 + ",\"heap\":" + String(ESP.getFreeHeap())
                 + "}";
        sendJson(req, j);
    });

    /* ── POST /api/test_event?ev=N ──────────────────────────────────── */
    s_server.on("/api/test_event", HTTP_POST, [onTestEvent](AsyncWebServerRequest* req) {
        if (req->hasParam("ev")) {
            uint8_t ev = (uint8_t)req->getParam("ev")->value().toInt();
            if (onTestEvent) onTestEvent(ev);
        }
        sendOk(req);
    });

    /* ══════════════════════════════════════════════════════════════════════
       Live Session Events – ring buffer of TrackStatus / SessionStatus /
       RaceControlMessages received via SignalR
       ══════════════════════════════════════════════════════════════════════ */

    /* ── GET /api/events → event log JSON array ──────────────────────── */
    s_server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = f1net_eventCount();
        JsonDocument doc;
        doc["active"]      = f1net_sessionActive();
        doc["connected"]   = f1net_isConnected();
        doc["sessionEnd"]  = f1net_sessionEndEpoch();
        JsonArray arr = doc["events"].to<JsonArray>();
        F1LiveEvent ev;
        for (int i = 0; i < n; i++) {
            if (!f1net_getEvent(i, &ev)) break;
            JsonObject o = arr.add<JsonObject>();
            o["t"]   = ev.epoch;
            o["cat"] = ev.category;
            o["msg"] = ev.message;
        }
        String out; serializeJson(doc, out);
        sendJson(req, out);
    });

    /* ── POST /api/events/clear → manually clear event log ───────────── */
    s_server.on("/api/events/clear", HTTP_POST, [](AsyncWebServerRequest* req) {
        f1net_clearEvents();
        sendOk(req);
    });

    /* ══════════════════════════════════════════════════════════════════════
       Next Race API – full session schedule from Jolpica
       ══════════════════════════════════════════════════════════════════════ */

    /* ── GET /api/nextrace → next race with all sessions (UTC times) ──── */
    s_server.on("/api/nextrace", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (f1cal_isApiFetching()) {
            sendJson(req, "{\"loading\":true}");
            return;
        }
        if (!f1cal_apiFetched()) {
            const String& err = f1cal_apiError();
            if (err.length() > 0) {
                String j = "{\"error\":\"" + err + "\"}";
                sendJson(req, j);
                return;
            }
            /* Not fetched yet and no error – still pending */
            sendJson(req, "{\"loading\":true}");
            return;
        }
        sendJson(req, f1cal_nextRaceJson());
    });

    /* ══════════════════════════════════════════════════════════════════════
       Calendar API (Phase 4 – dynamic calendar from LittleFS)
       ══════════════════════════════════════════════════════════════════════ */

    /* ── GET /api/calendar → {custom: bool} ────────────────────────────── */
    s_server.on("/api/calendar", HTTP_GET, [](AsyncWebServerRequest* req) {
        bool custom = LittleFS.exists("/calendar_custom.json");
        String j = "{\"custom\":" + String(custom ? "true" : "false") + "}";
        sendJson(req, j);
    });

    /* ── POST /api/calendar  (JSON body = calendar file) ───────────────── */
    auto calHandler = new AsyncCallbackJsonWebHandler("/api/calendar",
        [](AsyncWebServerRequest* req, JsonVariant& json) {
            /* Validate: must have "races" array */
            JsonArray races = json["races"];
            if (races.isNull() || races.size() == 0) {
                req->send(400, "application/json",
                          "{\"error\":\"JSON must have a 'races' array\"}");
                return;
            }
            /* Write to /calendar_custom.json */
            File f = LittleFS.open("/calendar_custom.json", "w");
            if (!f) {
                req->send(500, "application/json",
                          "{\"error\":\"Cannot write to LittleFS\"}");
                return;
            }
            serializeJson(json, f);
            f.close();
            /* Reload the calendar */
            f1cal_update();
            sendOk(req);
        });
    s_server.addHandler(calHandler);

    /* ── DELETE /api/calendar  (remove custom, revert to built-in) ──────── */
    s_server.on("/api/calendar", HTTP_DELETE, [](AsyncWebServerRequest* req) {
        if (LittleFS.exists("/calendar_custom.json")) {
            LittleFS.remove("/calendar_custom.json");
        }
        f1cal_update();
        sendOk(req);
    });

    /* ══════════════════════════════════════════════════════════════════════
       Firmware OTA update endpoint
       ══════════════════════════════════════════════════════════════════════ */

    s_server.on("/api/update", HTTP_POST,
        /* onRequest (called after all data received) */
        [](AsyncWebServerRequest* req) {
            bool ok = !Update.hasError();
            AsyncWebServerResponse* resp = req->beginResponse(
                ok ? 200 : 500, "application/json",
                ok ? "{\"ok\":true}" : "{\"error\":\"Update failed\"}");
            resp->addHeader("Connection", "close");
            req->send(resp);
            if (ok) {
                delay(500);
                ESP.restart();
            }
        },
        /* onUpload (called for each chunk) */
        [](AsyncWebServerRequest* req, String filename, size_t index,
           uint8_t* data, size_t len, bool final) {
            if (index == 0) {
                Serial.printf("[OTA] Begin: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            }
            if (len > 0) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Success: %u bytes\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );

    /* ── 404 catch-all ──────────────────────────────────────────────────── */
    s_server.onNotFound([](AsyncWebServerRequest* req) {
        /* Try to serve from LittleFS as a generic file */
        String path = req->url();
        if (LittleFS.exists(path)) {
            String ct = "application/octet-stream";
            if (path.endsWith(".html"))      ct = "text/html";
            else if (path.endsWith(".css"))  ct = "text/css";
            else if (path.endsWith(".js"))   ct = "application/javascript";
            else if (path.endsWith(".json")) ct = "application/json";
            else if (path.endsWith(".gz"))   ct = "application/gzip";
            req->send(LittleFS, path, ct);
            return;
        }
        req->redirect("/");
    });

    s_server.begin();
    Serial.println("[WebUI] Server started on port 80");
}
