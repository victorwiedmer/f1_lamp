/*
 * WebUI.cpp  –  ESPAsyncWebServer + single-page HTML UI
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

bool g_apMode = false;

/* ── global server instance (port 80) ───────────────────────────────────── */
static AsyncWebServer s_server(80);

/* ── forced state (0-7 = F1NetState, 0xFF = no forced state) ────────────── */
static uint8_t g_forcedState = 0xFF;

/* ── embedded single-page app ────────────────────────────────────────────── */
static const char UI_HTML[] PROGMEM = R"html(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>F1 Lamp</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#111;color:#eee;font-family:system-ui,Arial,sans-serif;max-width:520px;margin:0 auto;padding:12px 14px 30px}
h1{color:#e10600;font-size:1.5rem;display:inline}
.badge{display:inline-block;padding:3px 10px;border-radius:20px;font-size:.75rem;font-weight:600;vertical-align:middle;margin-left:10px}
.card{background:#1a1a1a;border-radius:10px;padding:14px;margin:10px 0}
h2{font-size:.7rem;text-transform:uppercase;letter-spacing:1.5px;color:#666;margin-bottom:10px}
.state-box{font-size:1.6rem;font-weight:800;text-align:center;padding:12px;border-radius:8px;letter-spacing:1px;transition:background .3s,color .3s}
.info-row{display:flex;justify-content:space-between;font-size:.8rem;color:#888;margin-top:7px}
.info-row span:last-child{color:#ccc}
.btn{border:none;border-radius:5px;padding:8px 14px;cursor:pointer;font-size:.82rem;font-weight:600}
.btn-sm{padding:5px 10px;font-size:.75rem}
.btn-save{background:#00d2be;color:#111}
.btn-danger{background:#e10600;color:#fff;margin-left:8px}
.btn-outline{background:transparent;border:1px solid #444;color:#aaa;font-size:.75rem}
.btn-scan{background:#2a2a2a;border:1px solid #444;color:#aaa;font-size:.75rem;padding:6px 12px;border-radius:5px;cursor:pointer;margin-left:6px}
.btn-scan:disabled{opacity:.45;cursor:default}
.btn-grid{display:flex;flex-wrap:wrap;gap:5px;margin-bottom:8px}
label{display:block;font-size:.78rem;color:#777;margin:8px 0 3px}
input[type=text],input[type=password]{width:100%;background:#252525;border:1px solid #333;color:#eee;padding:7px 10px;border-radius:5px;font-size:.9rem}
input[type=range]{width:100%;accent-color:#e10600;margin-top:2px;cursor:pointer}
input[type=color]{width:36px;height:28px;border:none;padding:0;cursor:pointer;border-radius:3px}
select{background:#252525;border:1px solid #333;color:#eee;padding:5px 6px;border-radius:4px;font-size:.8rem}
.row{display:flex;align-items:center;gap:7px;flex-wrap:wrap;margin:4px 0}
.eff-row{display:grid;grid-template-columns:110px 82px 36px 1fr;align-items:center;gap:6px;margin:5px 0}
.eff-label{font-size:.72rem;font-weight:700;padding:3px 6px;border-radius:4px;text-align:center}
.spd-wrap{display:flex;align-items:center;gap:4px;font-size:.75rem;color:#666}
.spd-wrap input{flex:1}
.ok{color:#00d2be;font-size:.78rem;margin-left:8px;display:none}
.feat-row{display:flex;justify-content:space-between;align-items:flex-start;padding:8px 0;border-bottom:1px solid #1e1e1e}
.feat-row:last-child{border-bottom:none}
.feat-title{font-size:.8rem;color:#ccc;font-weight:600;margin-bottom:2px}
.feat-desc{font-size:.72rem;color:#555;line-height:1.4}
.feat-toggle{flex-shrink:0;margin-left:12px;width:18px;height:18px;accent-color:#00d2be;cursor:pointer;margin-top:2px}
.pwr-btn{background:#1a3a1a;color:#00d2be;border:1px solid #00d2be;padding:5px 14px;font-size:.8rem;border-radius:5px;cursor:pointer;font-weight:700;transition:all .2s}
.pwr-btn.off{background:#3a1a1a;color:#e10600;border-color:#e10600}
.net-list{display:flex;flex-wrap:wrap;gap:5px;margin-top:8px;min-height:0}
.net-item{background:#252525;border:1px solid #333;border-radius:4px;padding:4px 9px;font-size:.78rem;cursor:pointer;color:#ccc;display:flex;align-items:center;gap:5px}
.net-item:hover{border-color:#00d2be;color:#fff}
.rssi-dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
small{display:block;color:#555;font-size:.72rem;margin-top:3px}
hr{border:none;border-top:1px solid #2a2a2a;margin:8px 0}
</style></head>
<body>
<div style="display:flex;align-items:center;justify-content:space-between;padding:6px 0 10px">
  <h1>&#9873; F1 Lamp</h1>
  <div style="display:flex;gap:8px;align-items:center">
    <span class="badge" id="netBadge" style="background:#333;color:#888">—</span>
    <button id="pwrBtn" class="btn pwr-btn" onclick="togglePower()">ON</button>
  </div>
</div>

<!-- status -->
<div class="card">
  <h2>Track Status</h2>
  <div class="state-box" id="stateBox">Loading…</div>
  <div class="info-row"><span>Network</span><span id="wifiInfo">Loading…</span></div>
  <div class="info-row"><span>IP</span><span id="ipInfo" style="font-family:monospace;font-size:.8rem">—</span></div>
  <div class="info-row"><span>Signal</span><span id="rssiInfo">—</span></div>
  <div class="info-row"><span>F1 LiveTiming</span><span id="f1Ok">—</span></div>
  <div class="info-row" id="raceRow" style="display:none"><span>Race Week</span><span id="raceLabel" style="color:#ffc800">—</span></div>
  <div class="info-row" id="rampRow" style="display:none"><span>Idle ramp</span><span id="rampPct">—</span></div>
  <div class="info-row"><span>Free heap</span><span id="heapInfo" style="font-size:.8rem">—</span></div>
  <div id="effDetail" style="margin-top:10px;padding:10px;background:#222;border-radius:8px;display:none">
    <div style="font-size:.65rem;text-transform:uppercase;letter-spacing:1px;color:#555;margin-bottom:6px">Current Effect</div>
    <div style="display:flex;align-items:center;gap:10px;flex-wrap:wrap">
      <span id="effName" style="font-size:.85rem;font-weight:700;color:#ccc"></span>
      <div style="display:flex;gap:3px;align-items:center">
        <div id="effSwatch" style="width:22px;height:22px;border-radius:4px;border:1px solid #444"></div>
        <div id="effSwatch2" style="width:22px;height:22px;border-radius:4px;border:1px solid #444;display:none"></div>
      </div>
      <span id="effSpd" style="font-size:.72rem;color:#666"></span>
      <span id="effTag" style="font-size:.65rem;padding:2px 6px;border-radius:10px;display:none"></span>
    </div>
    <div id="effHex" style="font-size:.68rem;color:#555;margin-top:5px;font-family:monospace"></div>
  </div>
</div>

<!-- force state -->
<div class="card">
  <h2>Force State (testing)</h2>
  <div class="btn-grid" id="forceGrid"></div>
  <button class="btn btn-outline btn-sm" onclick="forceState(255)">Clear Force</button>
</div>

<!-- brightness -->
<div class="card">
  <h2>Brightness</h2>
  <div class="row">
    <input type="range" min="1" max="255" id="briSlider" oninput="onBri(this.value)" style="flex:1">
    <span id="briVal" style="font-size:.85rem;color:#888;width:28px;text-align:right">128</span>
  </div>
</div>

<!-- effects per state -->
<div class="card">
  <h2>Effects by State</h2>
  <div style="font-size:.68rem;color:#555;margin-bottom:6px">State › Effect › Color › Speed</div>
  <div id="effectsGrid"></div>
</div>

<!-- wifi -->
<div class="card">
  <h2>WiFi</h2>
  <div class="row" style="margin-bottom:2px">
    <label style="margin:0;flex:1">SSID</label>
    <button class="btn-scan" id="scanBtn" onclick="scanNetworks()">Scan</button>
  </div>
  <input type="text" id="wSsid" autocomplete="off" placeholder="Network name">
  <div class="net-list" id="netList"></div>
  <label>Password</label>
  <input type="password" id="wPass" autocomplete="new-password">
  <small>Leave password blank to keep existing.</small>
  <div style="margin-top:10px">
    <button class="btn btn-save" onclick="saveWifi()">Save &amp; Reconnect</button>
    <span class="ok" id="wOk">Saved — rebooting…</span>
  </div>
</div>

<!-- features -->
<div class="card">
  <h2>Live Event Features</h2>
  <small style="display:block;color:#555;margin-bottom:10px">Automatic LED effects from live F1 telemetry. Requires a live timing connection.</small>
  <div class="feat-row">
    <div>
      <div class="feat-title">&#127942; Winner Celebration</div>
      <div class="feat-desc">Rainbow spinner on chequered flag. Lasts until the next state change.</div>
    </div>
    <input type="checkbox" class="feat-toggle" id="feat_winner" onchange="saveFeature('winner',this.checked)">
  </div>
  <div class="feat-row">
    <div>
      <div class="feat-title">&#128156; Fastest Lap Flash</div>
      <div class="feat-desc">3&thinsp;s purple strobe when a driver sets the overall fastest lap (from Race Control messages).</div>
    </div>
    <input type="checkbox" class="feat-toggle" id="feat_fastest_lap" onchange="saveFeature('fastest_lap',this.checked)">
  </div>
  <div class="feat-row">
    <div>
      <div class="feat-title">&#9989; DRS Open Flash</div>
      <div class="feat-desc">Brief white pulse when DRS detection zones open. Disabled by default &mdash; fires frequently in practice/quali.</div>
    </div>
    <input type="checkbox" class="feat-toggle" id="feat_drs" onchange="saveFeature('drs',this.checked)">
  </div>
  <div class="feat-row">
    <div>
      <div class="feat-title">&#128308; Start Lights</div>
      <div class="feat-desc">5 red LED segments light up one-by-one then go dark on session start, inspired by the F1 race start procedure.</div>
    </div>
    <input type="checkbox" class="feat-toggle" id="feat_start_lights" onchange="saveFeature('start_lights',this.checked)">
  </div>
  <div class="feat-row">
    <div>
      <div class="feat-title">&#128564; Deep Sleep</div>
      <div class="feat-desc">Outside race weekends the lamp sleeps (5&thinsp;&micro;A) and wakes every 5&ndash;60&thinsp;min depending on schedule. Requires battery use; disabled by default.</div>
    </div>
    <input type="checkbox" class="feat-toggle" id="feat_deep_sleep" onchange="saveFeature('deep_sleep',this.checked)">
  </div>
  <hr>
  <div style="font-size:.7rem;color:#555;margin-bottom:6px">Test without a live session:</div>
  <div style="display:flex;flex-wrap:wrap;gap:5px">
    <button class="btn btn-sm" style="background:#2a1a3a;color:#c080ff;border:1px solid #7040a0" onclick="testEvent(0)">&#127942; Winner</button>
    <button class="btn btn-sm" style="background:#2a1a3a;color:#c080ff;border:1px solid #7040a0" onclick="testEvent(1)">&#128156; Fastest Lap</button>
    <button class="btn btn-sm" style="background:#2a2a2a;color:#ccc;border:1px solid #555" onclick="testEvent(2)">&#9989; DRS Open</button>
    <button class="btn btn-sm" style="background:#3a0a0a;color:#ff8080;border:1px solid #e10600" onclick="testEvent(3)">&#128308; Start Lights</button>
  </div>
</div>

<!-- past sessions -->
<div class="card">
  <div style="display:flex;justify-content:space-between;align-items:center;margin-bottom:10px">
    <div style="font-weight:700;font-size:.85rem">&#127760; Past Sessions</div>
    <button class="btn btn-sm btn-outline" id="sessBtn" onclick="loadSessions()">Load</button>
  </div>
  <div id="sessList" style="font-size:.78rem;color:#888;max-height:260px;overflow-y:auto">Press Load to fetch from F1 live-timing.</div>
</div>

<!-- session replay -->
<div class="card">
  <div style="font-weight:700;font-size:.85rem;margin-bottom:10px">&#127909; Session Replay</div>
  <div class="row" style="gap:6px;margin-bottom:8px">
    <label style="font-size:.75rem;color:#888">Speed:</label>
    <select id="rpSpeed" style="background:#222;color:#ccc;border:1px solid #444;border-radius:6px;padding:3px 8px">
      <option value="1">1&times;</option>
      <option value="5" selected>5&times;</option>
      <option value="10">10&times;</option>
      <option value="30">30&times;</option>
      <option value="60">60&times;</option>
    </select>
    <button class="btn btn-sm" style="background:#1a3a1a;color:#6f6;border:1px solid #375" onclick="startReplay()">&#9654; Start</button>
    <button class="btn btn-sm" style="background:#3a1a1a;color:#f66;border:1px solid #733" onclick="stopReplay()">&#9632; Stop</button>
  </div>
  <div id="rpStatus" style="font-size:.72rem;color:#888;min-height:1.2em">Select speed, then press Start (built-in demo) or &#9654; on a session above.</div>
  <!-- replay live detail -->
  <div id="rpDetail" style="display:none;margin-top:8px">
    <div style="background:#191919;border-radius:6px;padding:8px;margin-bottom:6px">
      <div style="display:flex;align-items:center;gap:8px;margin-bottom:6px">
        <div id="rpStateBadge" style="font-size:.8rem;font-weight:700;padding:4px 10px;border-radius:5px;background:#333;color:#aaa">Idle</div>
        <span id="rpTime" style="font-size:.72rem;color:#555;font-family:monospace">00:00 / 00:00</span>
        <span id="rpPct" style="font-size:.68rem;color:#555;margin-left:auto">0%</span>
      </div>
      <div style="background:#2a2a2a;border-radius:3px;height:4px;overflow:hidden">
        <div id="rpBar" style="height:100%;background:#e10600;width:0%;transition:width .8s"></div>
      </div>
    </div>
    <div style="font-size:.6rem;text-transform:uppercase;letter-spacing:1px;color:#444;margin-bottom:3px">Event Log</div>
    <div id="rpLog" style="font-size:.7rem;color:#777;max-height:150px;overflow-y:auto;font-family:monospace;background:#151515;border-radius:5px;padding:5px 7px"></div>
  </div>
</div>

<!-- save / reboot -->
<div class="card">
  <div class="row">
    <button class="btn btn-save" onclick="saveAll()">Save Config</button>
    <span class="ok" id="saveOk">Saved!</span>
    <button class="btn btn-danger" onclick="doReboot()" style="margin-left:auto">Reboot</button>
  </div>
</div>

<script>
const SN=['Idle','Session Start','Green Flag','Yellow Flag','Safety Car','Virtual SC','Red Flag','Chequered','VSC Ending','SC Ending'];
const SB=['#2a2a2a','#00c800','#00c800','#ffc800','#ffc800','#ff6400','#e10600','#f0f0f0','#ffc800','#ffc800'];
const SF=['#aaa','#000','#000','#000','#000','#000','#fff','#000','#000','#000'];
const EF=['Solid','Pulse','Spinner','Strobe','Alt Letters'];

let cfg=null, briTimer=null, sessionPaths=[];

async function fetchWithTimeout(url,ms){
  const c=new AbortController();
  const t=setTimeout(()=>c.abort(),ms||5000);
  try{return await fetch(url,{signal:c.signal});}finally{clearTimeout(t);}
}
async function fetchStatus(){
  try{
    const d=await(await fetchWithTimeout('/api/status',4000)).json();
    const idx=Math.min(d.state,SN.length-1);
    const b=SB[idx]||'#333', f=SF[idx]||'#eee';
    const sb=document.getElementById('stateBox');
    sb.textContent=SN[idx]||'Unknown';
    sb.style.background=b; sb.style.color=f;
    document.getElementById('wifiInfo').textContent=d.ssid||(d.ap?'AP: F1-Lamp':'—');
    document.getElementById('ipInfo').textContent=d.ip||'—';
    const rssiEl=document.getElementById('rssiInfo');
    if(d.rssi){const rs=d.rssi;const col=rs>-60?'#00d2be':rs>-75?'#ffc800':'#e10600';rssiEl.textContent=rs+' dBm';rssiEl.style.color=col;}else{rssiEl.textContent=d.ap?'AP mode':'—';}
    document.getElementById('f1Ok').textContent=d.f1ok?'✓ Connected':'⏳ Waiting for session';
    document.getElementById('f1Ok').style.color=d.f1ok?'#00d2be':'#aaa';
    const nb=document.getElementById('netBadge');
    nb.textContent=d.ap?'AP Mode':('✓ '+d.ssid);
    nb.style.background=d.ap?'#7a4000':'#003a35';
    nb.style.color=d.ap?'#ffb84d':'#00d2be';
    if(!cfg){
      document.getElementById('briSlider').value=d.bri;
      document.getElementById('briVal').textContent=d.bri;
    }
    /* power button */
    const pb=document.getElementById('pwrBtn');
    pb.textContent=d.power?'ON':'OFF';
    pb.className='btn pwr-btn'+(d.power?'':' off');
    /* race week */
    const hasRace=d.nextRace&&d.nextRace!=='—';
    document.getElementById('raceRow').style.display=hasRace?'flex':'none';
    document.getElementById('raceLabel').textContent=d.nextRace||'';
    const hasRamp=d.rampPct>0&&d.rampPct<100;
    document.getElementById('rampRow').style.display=hasRamp?'flex':'none';
    document.getElementById('rampPct').textContent=hasRamp?(d.rampPct+'%'):'';
    document.getElementById('heapInfo').textContent=(d.heap/1024).toFixed(1)+' KB';
    /* current effect detail */
    const ep=document.getElementById('effDetail');
    if(d.eff){
      ep.style.display='block';
      const EN=['Solid','Pulse','Spinner','Strobe','Alt Letters','Rainbow Spin'];
      const eName=EN[d.eff.e]||('Effect '+d.eff.e);
      document.getElementById('effName').textContent=eName;
      const c1=rgb2hex(d.eff.r,d.eff.g,d.eff.b);
      document.getElementById('effSwatch').style.background=c1;
      const s2=document.getElementById('effSwatch2');
      if(d.eff.e===4){s2.style.display='inline-block';s2.style.background=rgb2hex(d.eff.r2,d.eff.g2,d.eff.b2);}else{s2.style.display='none';}
      document.getElementById('effSpd').textContent='Speed: '+d.eff.spd;
      const tag=document.getElementById('effTag');
      if(d.eff.fl){tag.style.display='inline';tag.textContent='\u26a1 Flash';tag.style.background='#3a2a00';tag.style.color='#ffc800';}
      else if(d.eff.rp){tag.style.display='inline';tag.textContent='\u25b6 Replay';tag.style.background='#1a3a1a';tag.style.color='#6f6';}
      else{tag.style.display='none';}
      const hexA=c1.toUpperCase();
      let hexTxt='Color: '+hexA+' ('+d.eff.r+','+d.eff.g+','+d.eff.b+')';
      if(d.eff.e===4){const c2=rgb2hex(d.eff.r2,d.eff.g2,d.eff.b2).toUpperCase();hexTxt+='  /  '+c2+' ('+d.eff.r2+','+d.eff.g2+','+d.eff.b2+')';}
      document.getElementById('effHex').textContent=hexTxt;
    }else{ep.style.display='none';}
  }catch(e){
    document.getElementById('stateBox').textContent='⚠ Offline';
    document.getElementById('stateBox').style.background='#333';
    document.getElementById('stateBox').style.color='#e10600';
  }
}

async function loadConfig(){
  try{
    cfg=await(await fetch('/api/config')).json();
    document.getElementById('wSsid').value=cfg.ssid||'';
    document.getElementById('briSlider').value=cfg.brightness||128;
    document.getElementById('briVal').textContent=cfg.brightness||128;
    buildEffects();
  }catch(e){console.error(e);}
}

function buildEffects(){
  if(!cfg)return;
  const g=document.getElementById('effectsGrid');
  g.innerHTML='';
  cfg.states.forEach((s,i)=>{
    if(i>=SN.length)return;
    const isAlt=s.effect===4;
    const row=document.createElement('div');
    row.className='eff-row';
    row.innerHTML=
      `<div class="eff-label" style="background:${SB[i]};color:${SF[i]}">${SN[i]}</div>`+
      `<select id="ee${i}" onchange="onEffChange(${i})">${EF.map((n,j)=>`<option value="${j}"${j==s.effect?' selected':''}>${n}</option>`).join('')}</select>`+
      `<div style="display:flex;gap:3px;align-items:center">`+
        `<input type="color" id="ec${i}" value="${rgb2hex(s.r,s.g,s.b)}" title="Color A">`+
        `<input type="color" id="ec2_${i}" value="${rgb2hex(s.r2,s.g2,s.b2)}" title="Color B (Alt Letters)" style="display:${isAlt?'inline-block':'none'}">`+
      `</div>`+
      `<div class="spd-wrap"><input type="range" min="1" max="255" id="es${i}" value="${s.speed}" oninput="document.getElementById('ev${i}').textContent=this.value"><span id="ev${i}">${s.speed}</span></div>`;
    g.appendChild(row);
  });
}

function onEffChange(i){
  const isAlt=+document.getElementById('ee'+i).value===4;
  document.getElementById('ec2_'+i).style.display=isAlt?'inline-block':'none';
}

function collectCfg(){
  const n=cfg?cfg.states.length:SN.length;
  const states=Array.from({length:n},(_,i)=>{
    const [r,gb,b]=hex2rgb(document.getElementById('ec'+i)?.value||'#000000');
    const [r2,g2,b2]=hex2rgb(document.getElementById('ec2_'+i)?.value||'#000000');
    return{
      effect:+document.getElementById('ee'+i).value,
      r, g:gb, b,
      speed:+document.getElementById('es'+i).value,
      r2, g2, b2,
      revert_s: cfg?cfg.states[i].revert_s:0
    };
  });
  return{
    brightness:+document.getElementById('briSlider').value||128,
    states
  };
}

async function saveAll(){
  const r=await fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(collectCfg())});
  if(r.ok) showOk('saveOk');
}

async function saveWifi(){
  const body={ssid:document.getElementById('wSsid').value,pass:document.getElementById('wPass').value};
  const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
  if(r.ok) showOk('wOk');
}

async function forceState(s){await fetch('/api/force?s='+s,{method:'POST'});}

function onBri(v){
  document.getElementById('briVal').textContent=v;
  clearTimeout(briTimer);
  briTimer=setTimeout(()=>fetch('/api/bri?v='+v,{method:'POST'}),200);
}

async function doReboot(){
  if(!confirm('Reboot the F1 Lamp?'))return;
  await fetch('/api/reboot',{method:'POST'});
}

async function togglePower(){
  const pb=document.getElementById('pwrBtn');
  const isOn=pb.textContent==='ON';
  await fetch('/api/power',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:!isOn})});
  /* optimistically flip before next status poll */
  pb.textContent=isOn?'OFF':'ON';
  pb.className='btn pwr-btn'+(isOn?' off':'');
}

async function loadFeatures(){
  try{
    const d=await(await fetch('/api/features')).json();
    document.getElementById('feat_winner').checked       = d.winner        ||false;
    document.getElementById('feat_fastest_lap').checked  = d.fastest_lap   ||false;
    document.getElementById('feat_drs').checked          = d.drs           ||false;
    document.getElementById('feat_start_lights').checked = d.start_lights  ||false;
    document.getElementById('feat_deep_sleep').checked    = d.deep_sleep    ||false;
  }catch(e){}
}
async function saveFeature(name,val){
  await fetch('/api/features',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({[name]:val})});
}
async function testEvent(ev){
  await fetch('/api/test_event?ev='+ev,{method:'POST'});
}

/* ── Session Replay ── */
let _rpPoll=null;
const TOPICS=['TrackStatus','SessionStatus','RaceControl'];
const TRACK_CODES={'1':'Green','2':'Yellow','4':'Safety Car','5':'Red Flag','6':'VSC','7':'VSC Ending'};
const SESS_MAP={'Started':'Session Start','Finished':'Chequered','Ends':'Chequered','Inactive':'Idle'};
let _rpLogLines=[];
function msToTime(ms){const m=Math.floor(ms/60000),s=Math.floor((ms%60000)/1000);return String(m).padStart(2,'0')+':'+String(s).padStart(2,'0');}
function evToLabel(topic,data){
  if(topic===0){return TRACK_CODES[data]||('Track:'+data);}
  if(topic===1){for(const[k,v]of Object.entries(SESS_MAP)){if(data.includes(k))return v;}return 'Session:'+data;}
  if(data==='FL')return 'Fastest Lap';
  if(data==='DRS')return 'DRS Enabled';
  return data;
}
function evToStateIdx(topic,data){
  if(topic===0){const m={'1':2,'2':3,'4':4,'5':6,'6':5,'7':8};return m[data]??-1;}
  if(topic===1){if(data.includes('Started'))return 1;if(data.includes('Finished')||data.includes('Ends'))return 7;if(data.includes('Inactive'))return 0;}
  return -1;
}
function pollReplay(){
  if(_rpPoll)clearInterval(_rpPoll);
  _rpLogLines=[];
  document.getElementById('rpLog').innerHTML='';
  document.getElementById('rpDetail').style.display='block';
  _rpPoll=setInterval(async()=>{
    try{
      const d=await(await fetch('/api/replay/status')).json();
      if(d.loading){
        document.getElementById('rpStatus').textContent='Fetching session data\u2026';
        return;
      }
      if(d.active){
        const pct=d.total>0?Math.round(d.idx*100/d.total):0;
        document.getElementById('rpStatus').textContent=`Playing \u25b6 ${d.idx}/${d.total} events  (${d.speed}\u00d7)`;
        document.getElementById('rpBar').style.width=pct+'%';
        document.getElementById('rpPct').textContent=pct+'%';
        if(d.ev){
          const simT=msToTime(d.ev.simMs);
          const totT=msToTime(d.ev.totalMs);
          document.getElementById('rpTime').textContent=simT+' / '+totT;
          const label=evToLabel(d.ev.topic,d.ev.data);
          const si=evToStateIdx(d.ev.topic,d.ev.data);
          const badge=document.getElementById('rpStateBadge');
          if(si>=0){badge.textContent=SN[si];badge.style.background=SB[si];badge.style.color=SF[si];}
          else{badge.textContent=label;badge.style.background='#2a1a3a';badge.style.color='#c080ff';}
          /* add to log if new */
          const line=msToTime(d.ev.ts)+' '+TOPICS[d.ev.topic]+': '+label;
          if(_rpLogLines.length===0||_rpLogLines[_rpLogLines.length-1]!==line){
            _rpLogLines.push(line);
            const log=document.getElementById('rpLog');
            const col=si>=0?SB[si]:(d.ev.topic===2?'#c080ff':'#888');
            log.innerHTML+=`<div style="color:${col}">${escHtml(line)}</div>`;
            log.scrollTop=log.scrollHeight;
          }
        }
      }else{
        document.getElementById('rpStatus').textContent='\u2705 Replay complete.';
        document.getElementById('rpBar').style.width='100%';
        document.getElementById('rpPct').textContent='100%';
        clearInterval(_rpPoll);_rpPoll=null;
      }
    }catch(e){}
  },800);
}
async function startReplay(){
  const speed=parseFloat(document.getElementById('rpSpeed').value);
  document.getElementById('rpStatus').textContent='Starting replay\u2026';
  try{
    await fetch('/api/replay/start',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({speed})});
  }catch(e){document.getElementById('rpStatus').textContent='Error: '+e.message;return;}
  pollReplay();
}
async function startSessionReplay(idx){
  const path=sessionPaths[idx];if(!path)return;
  const speed=parseFloat(document.getElementById('rpSpeed').value);
  document.getElementById('rpStatus').textContent='Fetching session streams\u2026';
  try{
    await fetch('/api/replay/session',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({path,speed})});
  }catch(e){document.getElementById('rpStatus').textContent='Error: '+e.message;return;}
  pollReplay();
}
async function stopReplay(){
  await fetch('/api/replay/stop',{method:'POST'});
  if(_rpPoll){clearInterval(_rpPoll);_rpPoll=null;}
  document.getElementById('rpStatus').textContent='Stopped.';
  document.getElementById('rpDetail').style.display='none';
}

async function scanNetworks(){
  const btn=document.getElementById('scanBtn');
  const list=document.getElementById('netList');
  btn.disabled=true; btn.textContent='Scanning…';
  list.innerHTML='<span style="font-size:.75rem;color:#555">Scanning…</span>';
  try{
    for(let i=0;i<30;i++){
      const d=await(await fetch('/api/scan')).json();
      if(d.scanning){await new Promise(r=>setTimeout(r,500));continue;}
      list.innerHTML='';
      if(!d.length){list.innerHTML='<span style="font-size:.75rem;color:#555">No networks found</span>';return;}
      d.forEach(n=>{
        const rssiColor=n.rssi>-60?'#00d2be':n.rssi>-75?'#ffc800':'#e10600';
        const el=document.createElement('div');
        el.className='net-item';
        el.innerHTML=`<span class="rssi-dot" style="background:${rssiColor}"></span>${escHtml(n.ssid)}<span style="font-size:.7rem;color:#555">${n.rssi}dBm</span>`;
        el.onclick=()=>{document.getElementById('wSsid').value=n.ssid;document.getElementById('wPass').focus();};
        list.appendChild(el);
      });
      return;
    }
    list.innerHTML='<span style="font-size:.75rem;color:#e10600">Scan timed out</span>';
  }catch(e){list.innerHTML='<span style="font-size:.75rem;color:#e10600">Scan failed</span>';}
  finally{btn.disabled=false;btn.textContent='Scan';}
}

function rgb2hex(r,g,b){return '#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('');}
function hex2rgb(h){const n=parseInt(h.replace('#',''),16);return[(n>>16)&255,(n>>8)&255,n&255];}
function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function showOk(id){const e=document.getElementById(id);e.style.display='inline';setTimeout(()=>e.style.display='none',2500);}

/* ── Past Sessions ── */
async function loadSessions(){
  const btn=document.getElementById('sessBtn');
  const list=document.getElementById('sessList');
  btn.disabled=true;btn.textContent='Loading\u2026';
  sessionPaths=[];
  list.innerHTML='<span style="color:#555">Fetching from F1 server\u2026</span>';
  try{
    /* Poll until the ESP finishes its background TLS fetch */
    let d;
    for(let i=0;i<30;i++){
      d=await(await fetch('/api/sessions')).json();
      if(!d.loading) break;
      await new Promise(r=>setTimeout(r,1000));
      list.innerHTML='<span style="color:#555">Fetching from F1 server\u2026 ('+Math.round(i+1)+'s)</span>';
    }
    if(d.loading){list.innerHTML='<span style="color:#e10600">Timed out waiting for data.</span>';return;}
    if(d.error){list.innerHTML='<span style="color:#e10600">Error: '+escHtml(d.error)+'</span><br><button class="btn btn-sm btn-outline" onclick="fetch(&apos;/api/sessions_debug?reset=1&apos;).then(()=>loadSessions())">Retry</button>';return;}
    if(!d.meetings||!d.meetings.length){list.innerHTML='No sessions found.';return;}
    let html='';
    d.meetings.forEach(m=>{
      html+=`<div style="margin-top:8px"><span style="color:#e10600;font-weight:700">${escHtml(m.name)}</span>`;
      html+=` <span style="color:#555">\u2014 ${escHtml(m.location||'')}</span></div>`;
      (m.sessions||[]).forEach(s=>{
        const hasPath=!!s.path;
        const icon=s.type==='Race'?'\ud83c\udfc1':s.type==='Qualifying'?'\u23f1':'\ud83d\udee0';
        const col=hasPath?'#ccc':'#555';
        html+=`<div style="margin-left:12px;padding:2px 0;color:${col}">`;
        html+=`${icon} <b>${escHtml(s.name)}</b> <span style="color:#555;font-size:.7rem">${escHtml(s.date||'')}</span>`;
        if(hasPath){const pi=sessionPaths.length;sessionPaths.push(s.path);html+=` <button class="btn btn-sm" style="font-size:.6rem;padding:1px 6px;background:#003a35;color:#00d2be;border:1px solid #005a4e;cursor:pointer" onclick="startSessionReplay(${pi})">&#9654; Replay</button>`;}
        html+=`</div>`;
      });
    });
    list.innerHTML=html;
  }catch(e){list.innerHTML='<span style="color:#e10600">Error: '+escHtml(e.message)+'</span>';}
  finally{btn.disabled=false;btn.textContent='Load';}
}

// ── init ─────────────────────────────────────────────────────────────────────
(function(){
  const g=document.getElementById('forceGrid');
  SN.forEach((n,i)=>{
    const b=document.createElement('button');
    b.className='btn btn-sm';b.textContent=n;
    b.style.background=SB[i];b.style.color=SF[i];
    b.onclick=()=>forceState(i);
    g.appendChild(b);
  });
  /* Stagger API calls – the 22 KB page takes seconds to transfer;
     fire status first with a small delay, then config/features.  */
  setTimeout(fetchStatus,300);
  setTimeout(loadConfig,800);
  setTimeout(loadFeatures,1200);
  setInterval(fetchStatus,2000);
})();
</script>
</body></html>
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

/* ═══════════════════════════════════════════════════════════════════════════
   Public init
   ═══════════════════════════════════════════════════════════════════════════ */

/* ── Lightweight WiFi-config page (~2 KB) for AP-mode use ────────────── */
static const char WIFI_HTML[] PROGMEM = R"html(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>F1 Lamp – WiFi Setup</title>
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
    /* Async scan: poll until results are ready (up to 15s) */
    for(let i=0;i<30;i++){
      const r=await fetch('/api/scan');const d=await r.json();
      if(d.scanning){await new Promise(r=>setTimeout(r,500));continue;}
      /* d is an array of networks */
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
    /* Try JSON POST first */
    const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid,pass})});
    if(r.ok){document.getElementById('ok').style.display='inline';return;}
  }catch(e){}
  /* Fallback: GET with query params (works even when JSON handler fails) */
  try{
    const u='/api/wifi/set?ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);
    const r2=await fetch(u);
    if(r2.ok){document.getElementById('ok').style.display='inline';return;}
  }catch(e){}
  alert('Save failed \u2013 check connection');
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

void webui_init(
    std::function<void(uint8_t state)>   onForcedState,
    std::function<void()>                onReboot,
    std::function<void(uint8_t evType)>  onTestEvent
) {
    /* ── GET /  →  full UI (STA) or redirect to /wifi (AP) ─────────────── */
    s_server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (g_apMode) {
            req->redirect("/wifi");
            return;
        }
        AsyncWebServerResponse* resp = req->beginResponse(
            200, "text/html", (const uint8_t*)UI_HTML, strlen_P(UI_HTML));
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
    });

    /* ── GET /wifi  →  lightweight WiFi-only setup page ────────────────── */
    s_server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest* req) {
        AsyncWebServerResponse* resp = req->beginResponse(
            200, "text/html", (const uint8_t*)WIFI_HTML, strlen_P(WIFI_HTML));
        resp->addHeader("Cache-Control", "no-cache");
        req->send(resp);
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
            /* reboot so WiFi.begin() picks up new credentials */
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
            if (obj["deep_sleep"].is<bool>())     g_cfg.deep_sleep        = obj["deep_sleep"];
            cfg_save();
            sendOk(req);
        });
    s_server.addHandler(featHandler);
    /* ── GET /api/scan  →  JSON array of nearby WiFi networks ──────────── */
    /* Uses async scan to avoid blocking the lwIP task (which kills the
       async web server connection).  First GET starts the scan and returns
       {"scanning":true}.  Client polls; once WiFi.scanComplete() >= 0
       we return the result array.                                          */
    s_server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest* req) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            /* No scan running / previous scan consumed → kick off new one */
            WiFi.scanNetworks(/*async=*/true, /*show_hidden=*/false);
            sendJson(req, "{\"scanning\":true}");
            return;
        }
        if (n == WIFI_SCAN_RUNNING) {
            sendJson(req, "{\"scanning\":true}");
            return;
        }
        /* n >= 0  → results ready */
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

    /* ── POST /api/replay/start  body: {"speed":10}  ─ load & play ── */
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
        /* last event details */
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

    /* ── GET /api/sessions  →  cached F1 session index ──────────────────── */
    s_server.on("/api/sessions", HTTP_GET, [](AsyncWebServerRequest* req) {
        if (f1sessions_isFetching()) {
            sendJson(req, "{\"loading\":true}");
            return;
        }
        if (!f1sessions_hasData()) {
            /* Check if we exhausted retries */
            const String& err = f1sessions_lastError();
            if (err.length() > 0) {
                /* Return the error so the UI can show it */
                String j = "{\"error\":\"" + err + "\"}";
                sendJson(req, j);
                return;
            }
            /* Kick off a background fetch (picked up by the f1net task
               so we don't run concurrent TLS). */
            f1sessions_requestFetch();
            sendJson(req, "{\"loading\":true}");
            return;
        }
        sendJson(req, f1sessions_json());
    });

    /* ── GET /api/sessions_debug  →  diagnostic info ────────────────────── */
    s_server.on("/api/sessions_debug", HTTP_GET, [](AsyncWebServerRequest* req) {
        /* If ?reset is present, clear the retry counter */
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

    /* ── POST /api/test_event?ev=N  (0=winner 1=fastest_lap 2=drs 3=start_lights) ─── */
    s_server.on("/api/test_event", HTTP_POST, [onTestEvent](AsyncWebServerRequest* req) {
        if (req->hasParam("ev")) {
            uint8_t ev = (uint8_t)req->getParam("ev")->value().toInt();
            if (onTestEvent) onTestEvent(ev);
        }
        sendOk(req);
    });

    /* ── 404 catch-all: redirect everything else to / ───────────────────── */
    s_server.onNotFound([](AsyncWebServerRequest* req) {
        req->redirect("/");
    });

    s_server.begin();
    Serial.println("[WebUI] Server started on port 80");
}
