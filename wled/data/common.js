/* common.js – shared helpers for F1 Lamp pages */
const SN=['Idle','Session Start','Green Flag','Yellow Flag','Safety Car','Virtual SC','Red Flag','Chequered','VSC Ending','SC Ending'];
const SB=['#2a2a2a','#00c800','#00c800','#ffc800','#ffc800','#ff6400','#e10600','#f0f0f0','#ffc800','#ffc800'];
const SF=['#aaa','#000','#000','#000','#000','#000','#fff','#000','#000','#000'];
const EF=['Solid','Pulse','Spinner','Strobe','Alt Letters'];

function rgb2hex(r,g,b){return '#'+[r,g,b].map(x=>x.toString(16).padStart(2,'0')).join('');}
function hex2rgb(h){const n=parseInt(h.replace('#',''),16);return[(n>>16)&255,(n>>8)&255,n&255];}
function escHtml(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
function showOk(id){const e=document.getElementById(id);e.style.display='inline';setTimeout(()=>e.style.display='none',2500);}

async function fetchWithTimeout(url,ms){
  const c=new AbortController();
  const t=setTimeout(()=>c.abort(),ms||5000);
  try{return await fetch(url,{signal:c.signal});}finally{clearTimeout(t);}
}

function buildNav(active){
  const pages=[
    {id:'home',href:'/',label:'&#9873; Status'},
    {id:'effects',href:'/effects.html',label:'&#127912; Effects'},
    {id:'features',href:'/features.html',label:'&#9889; Features'},
    {id:'sessions',href:'/sessions.html',label:'&#127760; Sessions'},
    {id:'settings',href:'/settings.html',label:'&#9881; Settings'},
  ];
  const nav=document.createElement('nav');
  pages.forEach(p=>{
    const a=document.createElement('a');
    a.href=p.href;
    a.innerHTML=p.label;
    if(p.id===active) a.className='active';
    nav.appendChild(a);
  });
  document.getElementById('navBar').appendChild(nav);
}

/* Header bar with power button + badge – used by all pages */
function buildHeader(){
  const hdr=document.getElementById('header');
  if(!hdr) return;
  hdr.innerHTML=`
    <div style="display:flex;align-items:center;justify-content:space-between;padding:6px 0 4px">
      <h1>&#9873; F1 Lamp</h1>
      <div style="display:flex;gap:8px;align-items:center">
        <span class="badge" id="netBadge" style="background:#333;color:#888">&mdash;</span>
        <button id="pwrBtn" class="btn pwr-btn" onclick="togglePower()">ON</button>
      </div>
    </div>`;
}

async function togglePower(){
  const pb=document.getElementById('pwrBtn');
  const isOn=pb.textContent==='ON';
  await fetch('/api/power',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:!isOn})});
  pb.textContent=isOn?'OFF':'ON';
  pb.className='btn pwr-btn'+(isOn?' off':'');
}

/* Lightweight status poller – updates badge + power btn */
async function pollStatus(fullCb){
  try{
    const d=await(await fetchWithTimeout('/api/status',4000)).json();
    const nb=document.getElementById('netBadge');
    if(nb){
      nb.textContent=d.ap?'AP Mode':('✓ '+d.ssid);
      nb.style.background=d.ap?'#7a4000':'#003a35';
      nb.style.color=d.ap?'#ffb84d':'#00d2be';
    }
    const pb=document.getElementById('pwrBtn');
    if(pb){
      pb.textContent=d.power?'ON':'OFF';
      pb.className='btn pwr-btn'+(d.power?'':' off');
    }
    if(fullCb) fullCb(d);
  }catch(e){}
}
