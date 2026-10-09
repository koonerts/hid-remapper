#pragma once
// The settings page, served at / (one file, no external resources).
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Warg settings</title>
<style>
:root{--bg:#f3f4f6;--card:#fff;--fg:#17191c;--mut:#646b75;--line:#dcdfe4;--soft:#eef0f3;--acc:#1f6fe0;--on:#fff;--ok:#1a8a4a;--bad:#c23b2e;--warn:#9a5a00;--warnbg:#fff4de}
@media (prefers-color-scheme:dark){:root{--bg:#121416;--card:#1b1e22;--fg:#e8eaed;--mut:#9aa1ab;--line:#2e333a;--soft:#24282e;--acc:#5a9cf5;--on:#0c1420;--ok:#4cc27e;--bad:#f0796c;--warn:#f0b65a;--warnbg:#33290f}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:600px;margin:0 auto;padding:14px 16px 72px}
h1{font-size:21px;font-weight:650;margin:4px 0 8px;letter-spacing:-.01em}
h2{font-size:15px;font-weight:650;margin:0}
p{margin:8px 0 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin:12px 0}
.hd{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}
.mut{color:var(--mut);font-size:13px}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.chips{display:flex;gap:6px;flex-wrap:wrap;align-items:center;min-height:26px}
.chip{font-size:12.5px;font-weight:600;padding:3px 9px;border-radius:99px;background:var(--soft);color:var(--mut)}
.chip.ok{color:var(--ok)}.chip.bad{color:var(--bad)}
button{font:inherit;padding:7px 12px;border-radius:9px;border:1px solid var(--line);background:var(--card);color:var(--fg);cursor:pointer}
button:disabled{opacity:.45;cursor:default}
button.p{background:var(--acc);border-color:var(--acc);color:var(--on);font-weight:600}
button.sq{width:38px;padding:7px 0;font-weight:700}
button:focus-visible,input:focus-visible,summary:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
input[type=number],input[type=text],input[type=password]{font:inherit;padding:7px 8px;border:1px solid var(--line);border-radius:9px;background:var(--bg);color:var(--fg);width:100%;min-width:0;font-variant-numeric:tabular-nums}
input[type=color]{width:42px;height:38px;border:1px solid var(--line);border-radius:9px;background:none;padding:2px;flex:none}
input[type=range]{flex:1;min-width:120px;accent-color:var(--acc)}
/* DPI stages: number (tap to select), colour, DPI */
.srow{display:grid;grid-template-columns:46px 42px 1fr;gap:8px;align-items:center;padding:5px;border-radius:12px;border:2px solid transparent}
.sep .srow{grid-template-columns:46px 42px 1fr 1fr}
.srow .dy{display:none}.sep .srow .dy{display:block}
.srow.on{border-color:var(--acc);background:var(--soft)}
.st{height:38px;border-radius:9px;border:1px solid var(--line);font-weight:700;padding:0}
.cur{font-size:12.5px;font-weight:600;color:var(--acc)}
.chk{display:flex;align-items:center;gap:6px;font-size:13px;color:var(--mut)}
/* sensor */
.fld+.fld{margin-top:16px;padding-top:14px;border-top:1px solid var(--line)}
.lbl{display:flex;justify-content:space-between;align-items:baseline;margin-bottom:6px}
.lbl b{font-size:17px;font-variant-numeric:tabular-nums}
/* segmented choice */
.seg{display:inline-flex;flex-wrap:wrap;border:1px solid var(--line);border-radius:10px;overflow:hidden}
.seg button{border:0;border-radius:0;border-right:1px solid var(--line);padding:6px 11px;background:var(--card)}
.seg button:last-child{border-right:0}
.seg button.on{background:var(--acc);color:var(--on);font-weight:600}
.opt{display:grid;grid-template-columns:1fr auto;gap:8px 12px;align-items:center}
.opt>span{font-size:14px}.opt>.seg,.opt>.mut{justify-self:end}
@media (max-width:540px){.opt{grid-template-columns:minmax(0,1fr);gap:4px}.opt>span:not(.mut):not(:first-child){margin-top:10px}.opt>.seg{display:flex;justify-self:stretch}.opt>.mut{justify-self:start}.opt>.seg button{flex:1;padding:7px 2px}}
.tag{font-size:11.5px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;padding:3px 8px;border-radius:6px;background:var(--warnbg);color:var(--warn)}
.note{background:var(--warnbg);color:var(--warn);border-radius:9px;padding:8px 10px;font-size:13px;margin:0 0 12px}
details>summary{cursor:pointer;list-style:none;display:flex;align-items:center;justify-content:space-between}
details>summary::-webkit-details-marker{display:none}
details>summary::after{content:"";flex:none;width:9px;height:9px;margin:0 5px 5px 12px;border-right:2px solid var(--mut);border-bottom:2px solid var(--mut);transform:rotate(45deg);transition:transform .15s}
details[open]>summary::after{transform:rotate(-135deg);margin-bottom:-4px}
details[open]>summary{margin-bottom:12px}
ul.ch{margin:0 0 14px;padding-left:18px;font-size:14px}ul.ch li{margin:4px 0}
.tune{display:grid;grid-template-columns:1fr 92px;gap:8px 10px;align-items:center;font-size:14px}
.toast{position:fixed;left:50%;bottom:calc(18px + env(safe-area-inset-bottom,0px));transform:translate(-50%,20px);background:var(--fg);color:var(--bg);padding:9px 14px;border-radius:10px;font-size:14px;opacity:0;pointer-events:none;transition:opacity .15s,transform .15s;max-width:90vw}
.toast.show{opacity:1;transform:translate(-50%,0)}
.toast.bad{background:var(--bad);color:#fff}
@media (prefers-reduced-motion:reduce){.toast,details>summary::after{transition:none}}
</style></head><body><main>
<h1>Warg settings</h1>
<div id="state" class="chips"><span class="chip">Connecting…</span></div>
<div class="row" style="margin-top:10px"><button onclick="readMouse()">Read from mouse</button><button onclick="post('/api/flash?c=ffffff').then(r=>say(r,'Flash sent'))">Test flash</button></div>

<div class="card"><div class="hd"><h2>DPI stages</h2><span class="cur" id="cur"></span></div>
<div id="rows"></div>
<div class="row" style="margin-top:10px;justify-content:space-between"><div class="row"><button class="p" id="stApply" onclick="applyStages()" disabled>Apply</button><button id="stRevert" onclick="revert()" disabled>Revert</button></div>
<label class="chk"><input type="checkbox" id="sepy" onchange="sepChange()"> Separate X / Y</label></div>
<p class="mut" id="stnote">Tap a number to switch stage right away. Colour and DPI edits are sent when you press Apply.</p></div>

<div class="card"><div class="hd"><h2>Sensor</h2></div>
<div class="fld"><div class="lbl"><span>Angle</span><b id="anglev">–</b></div>
<div class="row"><button class="sq" onclick="step('angle',-1)" aria-label="Angle down">−</button><input type="range" id="angle" min="-30" max="30" step="1" aria-label="Sensor angle"><button class="sq" onclick="step('angle',1)" aria-label="Angle up">+</button><button onclick="home('angle')">Reset to <span id="ahome">–</span>°</button></div></div>
<div class="fld"><div class="lbl"><span>Position</span><b id="posv">–</b></div>
<div class="row"><button class="sq" onclick="step('pos',-1)" aria-label="Position down">−</button><input type="range" id="pos" min="-100" max="101" step="1" aria-label="Sensor position"><button class="sq" onclick="step('pos',1)" aria-label="Position up">+</button><button onclick="home('pos')">Reset to <span id="phome">–</span></button></div></div></div>

<div class="card"><div class="hd"><h2>More mouse settings</h2></div>
<p class="note" id="optnote" hidden></p>
<div class="opt" id="pv">
<span>Polling rate (Hz)</span><div class="seg" data-k="poll"><button data-v="125">125</button><button data-v="250">250</button><button data-v="500">500</button><button data-v="1000">1000</button><button data-v="2000">2000</button><button data-v="4000">4000</button><button data-v="8000">8000</button></div>
<span>Lift-off distance (mm)</span><div class="seg" data-k="lod"><button data-v="0.7">0.7</button><button data-v="0.9">0.9</button><button data-v="1.2">1.2</button><button data-v="1.4">1.4</button><button data-v="1.6">1.6</button></div>
<span>Motion Sync</span><div class="seg" data-k="sync"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Angle Snap</span><div class="seg" data-k="snap"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Left button SPDT</span><div class="seg" data-k="spdt_l"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Right button SPDT</span><div class="seg" data-k="spdt_r"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Battery</span><span class="mut">not available yet</span>
</div>
<p class="mut">Through the Feather the mouse reports at up to 1000 Hz whatever is set here; higher rates apply when it is plugged straight into a PC.</p></div>

<details class="card"><summary><div><h2>Chords</h2><div class="mut">What each chord does, and its tuning</div></div></summary>
<ul class="ch" id="chords"></ul>
<div class="tune" id="tune"></div>
<div class="row" style="margin-top:12px"><button class="p" onclick="saveTune()">Save tuning</button></div></details>

<div class="card"><div class="hd"><h2>Wi-Fi</h2><button onclick="post('/api/radio_off')">Turn off now</button></div>
<p class="mut" id="radio" style="margin:0 0 12px"></p>
<div class="opt"><span>Turns itself off</span><div class="seg" id="keep"><button data-m="0">after 10 min</button><button data-m="1">after 1 hour</button><button data-m="2">never</button></div></div>
<p class="mut" id="keepnote" hidden>Wi-Fi stays on and comes on by itself at power-up. Hold Middle + Right to switch it off for a session.</p>
<details style="margin-top:12px"><summary><span>Change Wi-Fi network</span></summary>
<p class="mut" style="margin:0 0 8px">The QT Py joins this network when its Wi-Fi comes on. 2.4 GHz only.</p>
<input type="text" id="ssid" placeholder="Network name" aria-label="Network name"><div style="height:6px"></div><input type="password" id="pass" placeholder="Password" aria-label="Password">
<div class="row" style="margin-top:8px"><button class="p" onclick="saveWifi()">Save and join</button></div></details></div>

<p class="mut" id="ver" style="text-align:center"></p>
</main><div class="toast" id="toast" role="status"></div>
<script>
let S=null,edit={},dCol=false,dDpi=false,dTune=false,hold=0,holdT=0,sepInit=false,deb={},tt;
const $=id=>document.getElementById(id);
const TUNE=[["angle_step","as","Angle step (°)",1,10],["pos_step","ps","Position step",1,25],["angle_home","ah","Angle reset target (°)",-30,30],["pos_home","ph","Position reset target",-100,101],["flash_ms","fl","Light flash length (ms)",100,5000],["pos_hold_ms","poh","Middle + Left hold for position reset (ms)",150,3000],["wheel_hold_ms","wh","Middle hold before the wheel steps angle (ms)",0,3000],["radio_hold_ms","rh","Middle + Right hold for Wi-Fi (ms)",200,3000]];
function post(u,b){return fetch(u,{method:'POST',body:b}).then(r=>r.json()).catch(()=>({ok:false,err:'no answer'}))}
function toast(m,bad){const t=$('toast');t.textContent=m;t.className='toast show'+(bad?' bad':'');clearTimeout(tt);tt=setTimeout(()=>{t.className='toast'},2400)}
function say(r,m){toast(r.ok?m:'Failed: '+(r.err||'error'),!r.ok);return r}
function ink(c){const n=parseInt(c,16);return ((n>>16)*.299+(n>>8&255)*.587+(n&255)*.114)>140?'#111':'#fff'}
function readMouse(){post('/api/refresh').then(r=>say(r,'Reading from the mouse'))}

function rows(){return [...$('rows').children]}
function paintBtn(i,c){const b=rows()[i].querySelector('.st');b.style.background='#'+c;b.style.color=ink(c)}
function buildStages(){const el=$('rows');for(let i=0;i<5;i++){const n=i+1;el.insertAdjacentHTML('beforeend','<div class="srow"><button class="st" aria-label="Switch to stage '+n+'">'+n+'</button><input type="color" aria-label="Stage '+n+' colour"><input type="number" class="dx" min="50" max="50000" step="50" placeholder="DPI" aria-label="Stage '+n+' DPI"><input type="number" class="dy" min="50" max="50000" step="50" placeholder="Y" aria-label="Stage '+n+' Y DPI"></div>')}
rows().forEach((r,i)=>{r.querySelector('.st').onclick=()=>{if(!S)return;S.stage=i;edit.stage=Date.now();paintStages();post('/api/set?stage='+i).then(x=>say(x,'Stage '+(i+1)))};
r.querySelector('input[type=color]').oninput=e=>{dCol=true;paintBtn(i,e.target.value.slice(1));dirty()};
r.querySelector('.dx').oninput=e=>{dDpi=true;if(!$('sepy').checked)r.querySelector('.dy').value=e.target.value;dirty()};
r.querySelector('.dy').oninput=()=>{dDpi=true;dirty()}})}
function paintStages(){if(!S)return;const keep=Date.now()<hold;rows().forEach((r,i)=>{r.classList.toggle('on',S.stage===i);
if(!dCol&&!keep&&S.palette){const c=S.palette[i];r.querySelector('input[type=color]').value='#'+c;paintBtn(i,c)}
if(!dDpi&&!keep&&S.dpi){r.querySelector('.dx').value=S.dpi[i][0]||'';r.querySelector('.dy').value=S.dpi[i][1]||''}});
if(!sepInit&&S.dpi_known){sepInit=true;if(S.dpi.some(d=>d[0]!==d[1])){$('sepy').checked=true;sepChange()}}
const d=S.dpi&&S.stage!=null&&S.dpi[S.stage]?S.dpi[S.stage]:null;
$('cur').textContent=S.stage==null?'':'Stage '+(S.stage+1)+(d&&d[0]?' · '+(d[0]===d[1]?d[0]:d[0]+' × '+d[1])+' DPI':'')}
function sepChange(){$('rows').classList.toggle('sep',$('sepy').checked)}
function dirty(){const d=dCol||dDpi;$('stApply').disabled=!d;$('stRevert').disabled=!d}
function revert(){dCol=dDpi=false;hold=0;dirty();paintStages()}
async function applyStages(){const rs=rows();
if(dDpi){const sep=$('sepy').checked,v=[];for(const r of rs){const x=Number(r.querySelector('.dx').value),y=sep?Number(r.querySelector('.dy').value):x;v.push(x,y)}
if(v.some(n=>!(n>=50&&n<=50000))){toast('Each DPI must be 50 to 50000. Use Read from mouse if the fields are empty.',1);return}
const r=await post('/api/dpi?v='+v.join(','));if(!r.ok){say(r);return}}
if(dCol){const r=await post('/api/palette?c='+rs.map(r=>r.querySelector('input[type=color]').value.slice(1)).join(','));if(!r.ok){say(r);return}}
dCol=dDpi=false;hold=Date.now()+1800;dirty();toast('Sent to the mouse')}

function setVal(k,v){if(v!=null)$(k).value=v;$(k+'v').textContent=v==null?'–':(k==='angle'?v+'°':v)}
function send(k,v){post('/api/set?'+k+'='+v).then(r=>{if(!r.ok)say(r)})}
function bindSlider(k){$(k).addEventListener('input',e=>{edit[k]=Date.now();$(k+'v').textContent=e.target.value+(k==='angle'?'°':'');clearTimeout(deb[k]);deb[k]=setTimeout(()=>send(k,e.target.value),150)})}
function step(k,d){if(!S)return;const t=S.tune||{};let v=Number($(k).value);if(k==='angle')v+=d*(t.angle_step||1);else{const st=t.pos_step||5;v=d>0?Math.floor(v/st)*st+st:Math.ceil(v/st)*st-st}v=Math.max(Number($(k).min),Math.min(Number($(k).max),v));edit[k]=Date.now();setVal(k,v);send(k,v)}
function home(k){if(!S||!S.tune)return;const v=k==='angle'?S.tune.angle_home:S.tune.pos_home;edit[k]=Date.now();setVal(k,v);send(k,v)}

function drawChords(){const t=S.tune;if(!t)return;$('chords').innerHTML='<li><b>Middle + Right</b>, tap: next DPI stage. Hold '+t.radio_hold_ms+' ms: Wi-Fi on or off.</li><li><b>Middle + Left</b>, tap: angle back to '+t.angle_home+'°. Hold '+t.pos_hold_ms+' ms: position back to '+t.pos_home+'.</li><li><b>Middle + Back</b> / <b>Forward</b>: position up / down by '+t.pos_step+'.</li><li><b>Middle held '+t.wheel_hold_ms+' ms, then wheel</b>: angle up / down by '+t.angle_step+'°.</li>'}
function drawTune(){const el=$('tune');if(!S.tune)return;if(!el.children.length){TUNE.forEach(([k,a,l,lo,hi])=>el.insertAdjacentHTML('beforeend','<label for="t_'+a+'">'+l+'</label><input type="number" id="t_'+a+'" data-k="'+k+'" data-a="'+a+'" min="'+lo+'" max="'+hi+'">'));el.querySelectorAll('input').forEach(i=>{i.oninput=()=>{dTune=true}})}
if(dTune||Date.now()<holdT)return;el.querySelectorAll('input').forEach(i=>{if(document.activeElement!==i)i.value=S.tune[i.dataset.k]})}
function saveTune(){const q=[...$('tune').querySelectorAll('input')].map(i=>i.dataset.a+'='+i.value).join('&');post('/api/tune?'+q).then(r=>{say(r,'Tuning saved');if(r.ok){dTune=false;holdT=Date.now()+1800}})}

// values just set here, laid over the status for a while: a status read before the Feather had the
// change would otherwise undo it on screen, and SPDT's other side is combined from these
let optSet={};
function optOverlay(){if(!S||!S.opt)return;for(const k in optSet){if(Date.now()-optSet[k].t<2500)S.opt[k]=optSet[k].v;else delete optSet[k]}}
// the mouse's other settings; S.opt null = no status from the Feather yet, or its firmware is too old;
// a null value = not read yet
function drawOpt(){const o=S.opt,note=$('optnote'),btns=document.querySelectorAll('#pv .seg[data-k] button');
if(!o){note.hidden=false;note.textContent=S.link?'These need Feather firmware v14 or later.':'No answer from the Feather.';btns.forEach(b=>b.disabled=true);return}
btns.forEach(b=>b.disabled=false);
document.querySelectorAll('#pv .seg[data-k]').forEach(seg=>{const k=seg.dataset.k;const v=o[k];
seg.querySelectorAll('button').forEach(b=>b.classList.toggle('on',v!==null&&v!==undefined&&(typeof v==='boolean'?(b.dataset.v==='1')===v:Number(b.dataset.v)===v)))});
const missing=Object.values(o).some(v=>v===null);note.hidden=!missing;if(missing)note.textContent='Some of these haven\'t been read from the mouse yet: press Read from mouse.'}
function drawRadio(){const r=S.radio;let s=r.mode==='sta'?'On '+r.ssid+' at '+r.ip:r.mode==='ap'?'Setup network GW-Settings at '+r.ip:'Joining…';
s+=r.off_in_s<0?' · stays on':' · turns off in '+Math.ceil(r.off_in_s/60)+' min without use';
const w=S.wifi;if(w&&r.mode==='sta'&&w.rssi)s+=' · signal '+w.rssi+' dBm'+(w.drops?' · '+w.drops+' dropout'+(w.drops>1?'s':'')+' since it started':'');$('radio').textContent=s;
document.querySelectorAll('#keep button').forEach(b=>b.classList.toggle('on',Number(b.dataset.m)===r.keep));$('keepnote').hidden=r.keep!==2}
function saveWifi(){const b=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value});fetch('/api/wifi',{method:'POST',body:b}).then(()=>{$('radio').textContent='Joining '+$('ssid').value+'… reconnect to that network and open gwolves.local'})}

function render(){const st=$('state');
if(!S.link){st.innerHTML='<span class="chip bad">No answer from the Feather</span><span class="mut">Check the 4-pin cable.</span>'}
else{st.innerHTML='<span class="chip'+(S.warg||S.vuk?' ok':'')+'">'+(S.warg?'Warg connected':S.vuk?'VUK connected':'No G-Wolves mouse')+'</span>'+(S.warg&&S.paused?'<span class="chip bad">mouse didn\'t answer (asleep?)</span><span class="mut">Move it, then press Read from mouse.</span>':S.warg&&S.reads===false?'<span class="chip">mouse hasn\'t answered a read yet</span>':'')+(S.refreshing?'<span class="chip">reading…</span>':'')+(S.pending?'<span class="chip">sending…</span>':'')}
paintStages();
['angle','pos'].forEach(k=>{if(!edit[k]||Date.now()-edit[k]>1500)setVal(k,S[k])});
if(S.tune){$('ahome').textContent=S.tune.angle_home;$('phome').textContent=S.tune.pos_home}
drawChords();drawTune();drawRadio();drawOpt();
$('ver').textContent=(S.fw?'Feather v'+S.fw+' · ':'')+'QT Py firmware '+S.qt_fw}
// Opened as gwolves.local: once, before the first render and before any input, check the plain
// address answers and move there, so reloads and button presses don't need another .local name
// lookup (those fail on some phones and PCs). If the address doesn't answer, stay on .local.
let ipTried=false,touched=false;['input','pointerdown','keydown'].forEach(e=>document.addEventListener(e,()=>{touched=true},true));
async function toIp(j){if(ipTried||touched)return false;ipTried=true;const ip=j&&j.radio&&j.radio.mode==='sta'?j.radio.ip:'';
if(!/\.local$/i.test(location.hostname)||!/^\d+\.\d+\.\d+\.\d+$/.test(ip)||ip==='0.0.0.0')return false;
const c=new AbortController(),tm=setTimeout(()=>c.abort(),2500);try{await fetch('http://'+ip+'/api/status',{mode:'no-cors',cache:'no-store',signal:c.signal})}catch(e){return false}finally{clearTimeout(tm)}
if(touched)return false;location.replace('http://'+ip+'/');return true}
function poll(){fetch('/api/status').then(r=>r.json()).then(async j=>{if(!S&&await toIp(j))return;if(S&&edit.stage&&Date.now()-edit.stage<1500)j.stage=S.stage;S=j;optOverlay();render()}).catch(()=>{$('state').innerHTML='<span class="chip bad">Page lost the QT Py</span><span class="mut">Its Wi-Fi may be off, or its address changed: open gwolves.local again.</span>'}).finally(()=>setTimeout(poll,1000))}
buildStages();bindSlider('angle');bindSlider('pos');
document.querySelectorAll('#pv .seg[data-k]').forEach(seg=>seg.querySelectorAll('button').forEach(b=>{b.onclick=()=>{
if(!S||!S.opt)return;const k=seg.dataset.k;let key=k,val=b.dataset.v;const now=Date.now(),set={};
if(k==='spdt_l'||k==='spdt_r'){if(S.opt.spdt_l===null||S.opt.spdt_r===null){toast('Press Read from mouse first',1);return}
const l=k==='spdt_l'?val==='1':S.opt.spdt_l,r=k==='spdt_r'?val==='1':S.opt.spdt_r;
if(!l&&!r){toast('Both SPDT off isn\'t supported yet (not in the captures)',1);return}
set.spdt_l=l;set.spdt_r=r;key='spdt';val=(l?1:0)|(r?2:0)}
else set[k]=(k==='snap'||k==='sync')?val==='1':Number(val);
const prev={};for(const x in set){prev[x]=S.opt[x];optSet[x]={v:set[x],t:now};S.opt[x]=set[x]}drawOpt();
post('/api/opt?k='+key+'&v='+val).then(r=>{say(r,'Sent to the mouse');if(!r.ok){for(const x in set){delete optSet[x];if(S.opt)S.opt[x]=prev[x]}drawOpt()}})}}));
document.querySelectorAll('#keep button').forEach(b=>{b.onclick=()=>post('/api/wifimode?m='+b.dataset.m).then(r=>say(r,'Saved'))});
poll();
</script></body></html>)HTML";
