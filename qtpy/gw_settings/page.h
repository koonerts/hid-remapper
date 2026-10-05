#pragma once
// The settings page, served at / (one file, no external resources).
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Warg settings</title>
<style>
:root{--bg:#f6f6f4;--card:#fff;--fg:#1f1f1d;--mut:#6b6b66;--line:#ddd;--acc:#2b6fd6;--bad:#c0392b}
@media (prefers-color-scheme:dark){:root{--bg:#161615;--card:#1f1f1e;--fg:#ececea;--mut:#9a9a94;--line:#333;--acc:#5b93ea;--bad:#e57366}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,sans-serif}
main{max-width:560px;margin:0 auto;padding:12px 16px 40px}
h1{font-size:19px;font-weight:600;margin:6px 0 2px}h2{font-size:15px;font-weight:600;margin:0 0 10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px;margin:12px 0}
.mut{color:var(--mut);font-size:13px}.bad{color:var(--bad)}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
button{font:inherit;padding:7px 12px;border-radius:8px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
button.p{background:var(--acc);border-color:var(--acc);color:#fff}
input[type=range]{flex:1;min-width:140px}input[type=number],input[type=text],input[type=password]{font:inherit;width:90px;padding:6px;border:1px solid var(--line);border-radius:8px;background:var(--bg);color:var(--fg)}
input[type=text],input[type=password]{width:100%}
.val{min-width:44px;text-align:right;font-weight:600}
.stages{display:grid;grid-template-columns:repeat(5,1fr);gap:8px}
.stage{height:46px;border-radius:10px;border:2px solid var(--line);font-weight:600;color:#111}
.stage.on{outline:3px solid var(--acc);outline-offset:2px}
.grid{display:grid;grid-template-columns:auto 1fr 1fr;gap:6px 10px;align-items:center}
input[type=color]{width:44px;height:34px;border:1px solid var(--line);border-radius:8px;background:none;padding:2px}
</style></head><body><main>
<h1>Warg settings</h1>
<div id="state" class="mut">Connecting…</div>

<div class="card"><h2>DPI stage</h2><div class="stages" id="stages"></div>
<p class="mut" id="dpinote"></p></div>

<div class="card"><h2>Sensor angle</h2>
<div class="row"><button onclick="step('angle',-1)">−</button><input type="range" id="angle" min="-30" max="30" step="1"><button onclick="step('angle',1)">+</button><span class="val" id="anglev">–</span></div>
<div class="row" style="margin-top:8px"><button onclick="home('angle')">Reset to <span id="ahome"></span>°</button></div></div>

<div class="card"><h2>Sensor position</h2>
<div class="row"><button onclick="step('pos',-1)">−</button><input type="range" id="pos" min="-100" max="101" step="1"><button onclick="step('pos',1)">+</button><span class="val" id="posv">–</span></div>
<div class="row" style="margin-top:8px"><button onclick="home('pos')">Reset to <span id="phome"></span></button></div></div>

<div class="card"><h2>Stage colours</h2><div class="row" id="cols"></div>
<div class="row" style="margin-top:10px"><button class="p" onclick="saveCols()">Apply colours</button><span class="mut" id="colmsg"></span></div></div>

<div class="card"><h2>DPI per stage</h2>
<label class="row mut"><input type="checkbox" id="sepy" onchange="drawDpi()"> Separate Y</label>
<div class="grid" id="dpigrid" style="margin-top:8px"></div>
<div class="row" style="margin-top:10px"><button class="p" onclick="saveDpi()">Apply DPI</button><span class="mut" id="dpimsg"></span></div></div>

<div class="card"><h2>Chord tuning</h2><div class="grid" id="tune"></div>
<div class="row" style="margin-top:10px"><button class="p" onclick="saveTune()">Save tuning</button><span class="mut" id="tunemsg"></span></div></div>

<div class="card"><h2>Mouse</h2>
<div class="row"><button onclick="post('/api/refresh')">Read settings from the mouse</button><button onclick="post('/api/flash?c=ffffff')">Test flash</button></div>
<p class="mut" id="mousenote"></p></div>

<div class="card"><h2>Radio</h2><p class="mut" id="radio"></p>
<div class="row"><button onclick="post('/api/radio_off')">Turn radio off now</button></div>
<details style="margin-top:10px"><summary>Wi-Fi network</summary>
<p class="mut">The QT Py joins this network when its radio comes on; without one it opens “GW-Settings”.</p>
<input type="text" id="ssid" placeholder="Network name"><div style="height:6px"></div><input type="password" id="pass" placeholder="Password">
<div class="row" style="margin-top:8px"><button class="p" onclick="saveWifi()">Save and join</button></div></details></div>
</main>
<script>
let S=null,edit={},sentAt={};
const $=id=>document.getElementById(id);
const TUNE=[["angle_step","as","Angle step (°)",1,10],["pos_step","ps","Position step",1,25],["angle_home","ah","Angle reset target (°)",-30,30],["pos_home","ph","Position reset target",-100,101],["flash_ms","fl","LED flash (ms)",100,5000],["pos_hold_ms","poh","Mid+Left hold for position reset (ms)",150,3000],["wheel_hold_ms","wh","Middle hold before wheel steps angle (ms)",0,3000],["radio_hold_ms","rh","Mid+Right hold for radio (ms)",200,3000]];
function post(u){return fetch(u,{method:'POST'}).then(r=>r.json()).catch(()=>({ok:false,err:'no answer'}))}
function drawStages(){const el=$('stages');if(!S||!S.palette)return;el.innerHTML='';S.palette.forEach((c,i)=>{const b=document.createElement('button');b.className='stage'+(S.stage===i?' on':'');b.style.background='#'+c;const d=S.dpi&&S.dpi[i][0]?S.dpi[i][0]:'';b.textContent=(i+1);b.title=d?d+' DPI':'';b.onclick=()=>{post('/api/set?stage='+i);S.stage=i;drawStages()};el.appendChild(b)})}
function setVal(k,v){$(k).value=v;$(k+'v').textContent=v==null?'–':(k==='angle'?v+'°':v)}
function send(k,v){sentAt[k]=Date.now();post('/api/set?'+k+'='+v)}
let deb={};
function bindSlider(k){$(k).addEventListener('input',e=>{edit[k]=Date.now();$(k+'v').textContent=e.target.value+(k==='angle'?'°':'');clearTimeout(deb[k]);deb[k]=setTimeout(()=>send(k,e.target.value),150)})}
function step(k,d){if(!S)return;const t=S.tune||{};let v=Number($(k).value);if(k==='angle')v+=d*(t.angle_step||1);else{const st=t.pos_step||5;v=d>0?Math.floor(v/st)*st+st:Math.ceil(v/st)*st-st}v=Math.max(Number($(k).min),Math.min(Number($(k).max),v));edit[k]=Date.now();setVal(k,v);send(k,v)}
function home(k){if(!S||!S.tune)return;const v=k==='angle'?S.tune.angle_home:S.tune.pos_home;edit[k]=Date.now();setVal(k,v);send(k,v)}
function drawCols(force){const el=$('cols');if(!S||!S.palette)return;if(el.dataset.dirty&&!force)return;el.innerHTML='';S.palette.forEach((c,i)=>{const w=document.createElement('label');w.className='row mut';w.innerHTML=(i+1)+' <input type="color" value="#'+c+'">';w.querySelector('input').oninput=()=>{el.dataset.dirty=1};el.appendChild(w)})}
function saveCols(){const v=[...$('cols').querySelectorAll('input')].map(i=>i.value.slice(1)).join(',');post('/api/palette?c='+v).then(r=>{$('colmsg').textContent=r.ok?'Sent':'Failed: '+r.err;delete $('cols').dataset.dirty})}
function drawDpi(force){const el=$('dpigrid');if(!S||!S.dpi)return;if(el.dataset.dirty&&!force)return;const sep=$('sepy').checked;el.innerHTML='<span class="mut">Stage</span><span class="mut">'+(sep?'X':'DPI')+'</span><span class="mut">'+(sep?'Y':'')+'</span>';S.dpi.forEach((xy,i)=>{el.insertAdjacentHTML('beforeend','<span>'+(i+1)+'</span><input type="number" min="50" max="50000" step="50" value="'+(xy[0]||'')+'"><span>'+(sep?'<input type="number" min="50" max="50000" step="50" value="'+(xy[1]||'')+'">':'')+'</span>')});el.querySelectorAll('input').forEach(i=>i.oninput=()=>{el.dataset.dirty=1})}
function saveDpi(){const ins=[...$('dpigrid').querySelectorAll('input')].map(i=>Number(i.value));const sep=$('sepy').checked;let v=[];for(let i=0;i<5;i++){const x=sep?ins[2*i]:ins[i],y=sep?ins[2*i+1]:ins[i];v.push(x,y)}if(v.some(n=>!(n>=50&&n<=50000))){$('dpimsg').textContent='Each value 50–50000';return}post('/api/dpi?v='+v.join(',')).then(r=>{$('dpimsg').textContent=r.ok?'Sent':'Failed: '+r.err;delete $('dpigrid').dataset.dirty})}
function drawTune(force){const el=$('tune');if(!S||!S.tune)return;if(el.dataset.dirty&&!force)return;el.innerHTML='';TUNE.forEach(([k,a,l,lo,hi])=>{el.insertAdjacentHTML('beforeend','<span class="mut" style="grid-column:1/3">'+l+'</span><input type="number" data-a="'+a+'" min="'+lo+'" max="'+hi+'" value="'+S.tune[k]+'">')});el.querySelectorAll('input').forEach(i=>i.oninput=()=>{el.dataset.dirty=1})}
function saveTune(){const q=[...$('tune').querySelectorAll('input')].map(i=>i.dataset.a+'='+i.value).join('&');post('/api/tune?'+q).then(r=>{$('tunemsg').textContent=r.ok?'Saved':'Failed: '+r.err;delete $('tune').dataset.dirty})}
function saveWifi(){const b=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value});fetch('/api/wifi',{method:'POST',body:b}).then(()=>{$('radio').textContent='Joining '+$('ssid').value+'… reconnect to that network and open gwolves.local'})}
function render(){const st=$('state');if(!S.link){st.innerHTML='<span class="bad">No answer from the Feather.</span> Check the STEMMA QT cable.';return}
const mouse=S.warg?'Warg connected':(S.vuk?'VUK connected':'No G-Wolves mouse');st.textContent=mouse+(S.reads===false&&S.warg?' · mouse doesn\'t answer reads yet, values are what the Feather last set':'')+(S.pending?' · sending…':'');
drawStages();$('dpinote').textContent=S.stage==null?'Current stage unknown until a DPI step or a read.':'';
['angle','pos'].forEach(k=>{if(!edit[k]||Date.now()-edit[k]>1500)setVal(k,S[k])});
if(S.tune){$('ahome').textContent=S.tune.angle_home;$('phome').textContent=S.tune.pos_home}
drawCols();drawDpi();drawTune();
$('mousenote').textContent=(S.colours_read?'Colours read from the mouse. ':'Colours: the Feather\'s built-in copy. ')+(S.dpi_known?'DPI table known.':'DPI table not read yet: use Read settings.')+(S.refreshing?' Reading…':'');
const r=S.radio;$('radio').textContent=(r.mode==='sta'?'On '+r.ssid+' at '+r.ip:r.mode==='ap'?'Setup network GW-Settings at '+r.ip:'Joining…')+' · turns off in '+Math.ceil(r.off_in_s/60)+' min without use (or hold Mid+Right).'}
function poll(){fetch('/api/status').then(r=>r.json()).then(j=>{S=j;render()}).catch(()=>{$('state').innerHTML='<span class="bad">Page lost the QT Py</span> (radio off?)'}).finally(()=>setTimeout(poll,1000))}
bindSlider('angle');bindSlider('pos');poll();
</script></body></html>)HTML";
