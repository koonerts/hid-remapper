#pragma once
// The settings page, served at / (one file, no external resources).
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Warg settings</title><link rel="icon" id="fav" href="data:,">
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
.sens{display:flex;gap:18px;align-items:center}
.sens>svg{flex:none;width:170px;height:auto;touch-action:none;user-select:none;-webkit-user-select:none}
.flds{flex:1;min-width:0}
@media (max-width:540px){.sens{flex-direction:column;align-items:stretch}.sens>svg{align-self:center;width:200px}}
.ms .bd{fill:var(--soft);stroke:var(--mut);stroke-opacity:.5;stroke-width:2;cursor:ns-resize}
.ms .sm{fill:none;stroke:var(--mut);stroke-opacity:.35;stroke-width:1.5;pointer-events:none}
.ms .ax{stroke:var(--mut);stroke-opacity:.5;stroke-width:1;stroke-dasharray:3 3}
.ms .gh{fill:none;stroke:var(--mut);stroke-width:1.5;stroke-dasharray:4 3}
.ms .ax,.ms .gh,.ms .arc,.ms text{pointer-events:none}
.ms .sn{cursor:ns-resize}
.ms .sn rect{fill:var(--acc);fill-opacity:.22;stroke:var(--acc);stroke-width:2}
.ms .sn line,.ms .sn path{stroke:var(--acc);stroke-width:2.5;fill:var(--acc)}
.ms .dot{fill:var(--acc)}
.ms .arc{fill:none;stroke:var(--acc);stroke-width:1.5;stroke-opacity:.7}
.ms .hit{fill:transparent;cursor:grab}
.ms text{fill:var(--mut);font-size:11px}
.ms.unk .sn,.ms.unk .arc{opacity:.3}
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
/* chord editor */
select{font:inherit;padding:6px 8px;border:1px solid var(--line);border-radius:9px;background:var(--bg);color:var(--fg);min-width:0;max-width:100%}
select:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
.crow{border:1px solid var(--line);border-radius:12px;padding:10px;margin:0 0 8px}
.cwhen{display:flex;align-items:center;gap:6px;flex-wrap:wrap;font-size:14px}
.cwhen select{font-weight:600}
.cwhen .x{margin-left:auto}
.cdo{display:grid;grid-template-columns:1fr 1fr 120px;gap:8px;margin-top:8px}
.cdo label{display:flex;flex-direction:column;gap:3px;font-size:12.5px;color:var(--mut);min-width:0}
.cdo select{width:100%}
.cdo input:disabled{opacity:.45}
.cdo .dbl{display:flex;flex-direction:row;align-items:center;gap:6px;font-size:12.5px;margin-top:4px}
#evbox{margin:0 0 10px;font-size:13px}
/* frame timing (Chords card) */
.bud{position:relative;height:12px;border-radius:6px;background:var(--soft);overflow:hidden;margin:8px 0 4px}
.bud i{position:absolute;left:0;top:0;bottom:0;border-radius:6px}
#budw{background:var(--acc);opacity:.3}#buda{background:var(--acc)}
.pt{width:100%;border-collapse:collapse;font-size:13px;margin:6px 0}
.pt th,.pt td{padding:4px 6px;text-align:right;border-bottom:1px solid var(--line);font-variant-numeric:tabular-nums}
.pt th{font-weight:600;color:var(--mut)}
.pt th:first-child,.pt td:first-child{text-align:left}
.pt td.sub{padding-left:20px;color:var(--mut)}
.pt .mut{font-size:12px}
#pskip.bad{color:var(--bad)}
#lvc{display:block;width:100%;height:220px;background:#000;border:1px solid var(--line);border-radius:10px;cursor:crosshair;touch-action:none}
#lvwrap{position:relative}#lvhint{display:none;position:absolute;top:14px;left:0;right:0;text-align:center;color:#bbb;font-size:14px;pointer-events:none}
#lvwrap.full{position:fixed;inset:0;z-index:1000;background:#000}#lvwrap.full #lvc{height:100vh;border:0;border-radius:0;cursor:none}#lvwrap.full #lvhint{display:block}
#lvr b{color:var(--fg)}#evbox summary{cursor:pointer;color:var(--mut)}#evlist{margin:6px 0 0;padding-left:18px;color:var(--mut)}#evlist li{margin:2px 0}
@media (max-width:540px){.cdo{grid-template-columns:1fr}}
.cerr{color:var(--bad);font-size:12.5px;margin:6px 0 0}
.cerr:empty{display:none}
h3.sub{font-size:14px;font-weight:650;margin:18px 0 8px}
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
<p class="mut" id="sensok" hidden style="margin:0 0 10px"></p>
<details id="evbox" hidden><summary id="evlast"></summary><ol id="evlist"></ol></details>
<div class="sens"><svg class="ms unk" id="ms" viewBox="0 0 200 300" role="img" aria-label="Sensor position and angle">
<text x="100" y="13" text-anchor="middle">front</text>
<path class="bd" d="M100,22 C146,22 160,62 160,112 C160,172 166,232 150,264 C138,284 62,284 50,264 C34,232 40,172 40,112 C40,62 54,22 100,22 Z"/>
<path class="sm" d="M100,22 V118 M41,114 Q100,128 159,114"/><rect class="sm" x="94" y="44" width="12" height="30" rx="6"/>
<rect class="sm" x="37" y="130" width="7" height="22" rx="3"/><rect class="sm" x="37" y="156" width="7" height="22" rx="3"/>
<g id="msg"><rect class="gh" x="-13" y="-16" width="26" height="32" rx="5"/><line class="gh" x1="0" y1="-16" x2="0" y2="-42"/></g>
<g id="msa"><line class="ax" x1="0" y1="-60" x2="0" y2="60"/><path class="arc" id="msarc"/></g>
<g class="sn" id="mss"><rect x="-13" y="-16" width="26" height="32" rx="5"/><line x1="0" y1="-16" x2="0" y2="-42"/><path d="M0,-54 L-6.5,-41 L6.5,-41 Z"/><circle class="dot" r="3"/><circle class="hit" id="msh" cx="0" cy="-48" r="14"/></g>
</svg><div class="flds">
<div class="fld"><div class="lbl"><span>Angle</span><b id="anglev">–</b></div>
<div class="row"><button class="sq" onclick="step('angle',-1)" aria-label="Angle down">−</button><input type="range" id="angle" min="-30" max="30" step="1" autocomplete="off" aria-label="Sensor angle"><button class="sq" onclick="step('angle',1)" aria-label="Angle up">+</button><button onclick="home('angle')">Reset to <span id="ahome">–</span>°</button></div></div>
<div class="fld"><div class="lbl"><span>Position</span><b id="posv">–</b></div>
<div class="row"><button class="sq" onclick="step('pos',-1)" aria-label="Position down">−</button><input type="range" id="pos" min="-100" max="101" step="1" autocomplete="off" aria-label="Sensor position"><button class="sq" onclick="step('pos',1)" aria-label="Position up">+</button><button onclick="home('pos')">Reset to <span id="phome">–</span></button></div></div></div></div>
<p class="mut">Drag the sensor to move it, or the arrow's tip to turn it. Dashed: where the reset chords put it. The outline is a generic mouse, and position is in the Warg's own units, not mm.</p></div>

<div class="card" id="lvl"><div class="hd"><h2>Level your swipes</h2></div>
<p class="mut" style="margin:0 0 8px">Suggests a sensor angle that makes left-right swipes come out level, starting from the angle the mouse has now. Use the mouse on this computer: click the box, swipe left and right a few times the way you do in game, then right-click or press Esc.</p>
<p class="note" id="lvsnap" hidden>Angle Snap is on: it straightens swipes, so it hides the tilt. Turn it off under More mouse settings to measure.</p>
<div id="lvwrap"><canvas id="lvc" aria-label="Swipe area"></canvas><div id="lvhint">Swipe left and right a few times · right-click or Esc to finish</div></div>
<p id="lvr" class="mut" style="margin:8px 0 0">Click the box to start.</p>
<div class="row" id="lvb" hidden style="margin-top:8px"><button class="p" id="lvset"></button><button id="lvagain">Measure again</button></div>
</div>
<div class="card"><div class="hd"><h2>More mouse settings</h2></div>
<p class="note" id="optnote" hidden></p>
<div class="opt" id="pv">
<span>Polling rate (Hz)</span><div class="seg" data-k="poll"><button data-v="125">125</button><button data-v="250">250</button><button data-v="500">500</button><button data-v="1000">1000</button><button data-v="2000">2000</button><button data-v="4000">4000</button><button data-v="8000">8000</button></div>
<span>Lift-off distance (mm)</span><div class="seg" data-k="lod"><button data-v="0.7">0.7</button><button data-v="0.9">0.9</button><button data-v="1.2">1.2</button><button data-v="1.4">1.4</button><button data-v="1.6">1.6</button></div>
<span>Motion Sync</span><div class="seg" data-k="sync"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Angle Snap</span><div class="seg" data-k="snap"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Left button SPDT</span><div class="seg" data-k="spdt_l"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Right button SPDT</span><div class="seg" data-k="spdt_r"><button data-v="0">Off</button><button data-v="1">On</button></div>
<span>Battery</span><span class="mut" id="batt">not read yet</span>
</div>
<p class="mut">Through the Feather the mouse reports at up to 1000 Hz whatever is set here; higher rates apply when it is plugged straight into a PC.</p></div>

<details class="card" id="fchords"><summary><div><h2>Chords</h2><div class="mut">Hold one button, press another: what each pair does</div></div></summary>
<div id="ched" hidden>
<p class="note" id="chnote" hidden></p>
<fieldset id="chfs" style="border:0;padding:0;margin:0;min-width:0"><div id="chlist"></div></fieldset>
<div class="row" style="margin-top:10px;justify-content:space-between"><div class="row"><button class="p" id="chSave" onclick="chSave()" disabled>Save chords</button><button id="chRevert" onclick="chRevert()" disabled>Revert</button></div>
<div class="row"><button id="chAdd" onclick="chAdd()">Add chord</button><button onclick="chDefaults()">Built-in chords</button></div></div>
<p class="mut">The second button never reaches the game while it's part of a chord; the first one always does. Pressing them the other way round isn't a chord. A tap acts when the second button comes up; with no hold action it acts as soon as it goes down.</p>
</div>
<p class="mut" id="chstate"></p>
<div id="perf" hidden><h3 class="sub">Frame timing <span class="mut">(last second)</span></h3>
<div class="bud" role="img" id="bud"><i id="budw"></i><i id="buda"></i></div>
<p class="mut" id="budt" style="margin:0"></p>
<table class="pt"><thead><tr><th></th><th>Average</th><th>Worst</th></tr></thead><tbody id="ptb"></tbody></table>
<p class="mut" id="pskip"></p></div>
<ul class="ch" id="chords"></ul>
<h3 class="sub">Tuning</h3>
<div class="tune" id="tune"></div>
<div class="row" style="margin-top:12px"><button class="p" onclick="saveTune()">Save tuning</button></div></details>

<div class="card"><div class="hd"><h2>Wi-Fi</h2><button onclick="post('/api/radio_off')">Turn off now</button></div>
<p class="mut" id="radio" style="margin:0 0 12px"></p>
<div class="opt"><span>Turns itself off</span><div class="seg" id="keep"><button data-m="0">after 10 min</button><button data-m="1">after 1 hour</button><button data-m="2">never</button></div></div>
<p class="mut" id="keepnote" hidden></p>
<details style="margin-top:12px" id="fwifi"><summary><span>Change Wi-Fi network</span></summary>
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
// tab icon: a mouse in the current DPI stage's colour
let favC='';
function fav(c){if(c===favC)return;favC=c;$('fav').href='data:image/svg+xml,'+encodeURIComponent("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><path d='M16 1.5C23.5 1.5 26 7.5 26 14v8c0 6-4.5 8.5-10 8.5S6 28 6 22v-8C6 7.5 8.5 1.5 16 1.5Z' fill='#"+c+"' stroke='#222' stroke-width='1.8'/><path d='M16 2v10M6.6 12.6Q16 15.6 25.4 12.6' fill='none' stroke='#222' stroke-width='1.8'/></svg>")}
fav('1f6fe0');function readMouse(){post('/api/refresh').then(r=>say(r,'Reading from the mouse'))}

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

function setVal(k,v){if(v!=null)$(k).value=v;$(k+'v').textContent=v==null?'–':(k==='angle'?v+'°':v);drawSensor()}
// fw21: a value goes to the mouse only once the page holds the mouse's real one, and a slider sends only right after a
// person's touch, click or key on it. Before that the sliders sit at their empty defaults (angle 0, position 1), and a
// browser bringing the page back (tab restore, a phone waking a background tab) fires input events that once sent those.
let gest={},downK=null;
function known(k){return !!S&&S[k]!=null}
function mine(k,e){return e.isTrusted&&known(k)&&(downK===k||Date.now()-(gest[k]||0)<3000)}
document.addEventListener('pointerdown',e=>{const k=e.target&&e.target.id;downK=k==='angle'||k==='pos'?k:null;if(downK)gest[downK]=Date.now()},true);
document.addEventListener('pointermove',e=>{if(downK&&e.buttons)gest[downK]=Date.now()},true);
['pointerup','pointercancel'].forEach(t=>document.addEventListener(t,()=>{if(downK)gest[downK]=Date.now();downK=null},true));
document.addEventListener('keydown',e=>{const k=e.target&&e.target.id;if(k==='angle'||k==='pos')gest[k]=Date.now()},true);
function send(k,v){if(!known(k))return;post('/api/set?'+k+'='+v).then(r=>{if(!r.ok)say(r)})}
function bindSlider(k){$(k).addEventListener('input',e=>{if(!mine(k,e)){if(known(k))setVal(k,S[k]);return}edit[k]=Date.now();$(k+'v').textContent=e.target.value+(k==='angle'?'°':'');drawSensor();clearTimeout(deb[k]);deb[k]=setTimeout(()=>send(k,e.target.value),150)})}
function live(k,v){if(!known(k))return;edit[k]=Date.now();setVal(k,v);clearTimeout(deb[k]);deb[k]=setTimeout(()=>send(k,v),150)}
// sensor drawing: y0 = the sensor's spot at position 0, k = drawing units per position step (lower
// positions are toward the front), dir = 1 draws a positive angle turned clockwise
const MS={y0:162,k:.5,dir:1};
function msY(p){return MS.y0+p*MS.k}
function drawSensor(){const a=Number($('angle').value),p=Number($('pos').value),y=msY(p),t=a*MS.dir*Math.PI/180,R=34;
$('mss').setAttribute('transform','translate(100 '+y+') rotate('+a*MS.dir+')');$('msa').setAttribute('transform','translate(100 '+y+')');
$('msarc').setAttribute('d',a?'M0,'+(-R)+' A'+R+' '+R+' 0 0 '+(t>0?1:0)+' '+(R*Math.sin(t)).toFixed(1)+','+(-R*Math.cos(t)).toFixed(1):'');
const tu=S&&S.tune;$('msg').style.display=tu?'':'none';if(tu)$('msg').setAttribute('transform','translate(100 '+msY(tu.pos_home)+') rotate('+tu.angle_home*MS.dir+')');
$('ms').classList.toggle('unk',!S||(S.angle==null&&!edit.angle)||(S.pos==null&&!edit.pos));$('ms').setAttribute('aria-label','Sensor at position '+p+', angle '+a+'°');sensOk()}
function sensOk(){const o=S&&S.sens,w=[];if(o)for(const[k,n]of[['angle','Angle'],['pos','Position']]){if(S[k]==null)continue;if(o[k+'_wait'])w.push(n+' was sent while the mouse was asleep; it goes out again when the mouse wakes.');else if(!o[k+'_ok'])w.push(n+' not confirmed by the mouse yet; it may differ.')}$('sensok').hidden=!w.length;$('sensok').textContent=w.join(' ')}
let msDrag=null,msOff=0;
function msMove(e){if(!msDrag)return;const q=new DOMPoint(e.clientX,e.clientY).matrixTransform($('ms').getScreenCTM().inverse());
if(msDrag==='pos'){const p=Math.max(-100,Math.min(101,Math.round((q.y-msOff-MS.y0)/MS.k)));if(p!==Number($('pos').value))live('pos',p)}
else{const y=msY(Number($('pos').value)),a=Math.max(-30,Math.min(30,Math.round(Math.atan2(q.x-100,y-q.y)*180/Math.PI*MS.dir)));if(a!==Number($('angle').value))live('angle',a)}}
function step(k,d){if(!known(k))return;const t=S.tune||{};let v=Number($(k).value);if(k==='angle')v+=d*(t.angle_step||1);else{const st=t.pos_step||5;v=d>0?Math.floor(v/st)*st+st:Math.ceil(v/st)*st-st}v=Math.max(Number($(k).min),Math.min(Number($(k).max),v));edit[k]=Date.now();setVal(k,v);send(k,v)}
function home(k){if(!S||!S.tune)return;const v=k==='angle'?S.tune.angle_home:S.tune.pos_home;edit[k]=Date.now();setVal(k,v);send(k,v)}

function drawChords(){const t=S.tune;if(!t)return;$('chords').innerHTML='<li><b>Middle + Right</b>, tap: next DPI stage. Hold '+t.radio_hold_ms+' ms: Wi-Fi on or off.</li><li><b>Middle + Left</b>, tap: angle back to '+t.angle_home+'°. Hold '+t.pos_hold_ms+' ms: position back to '+t.pos_home+'.</li><li><b>Middle + Back</b> / <b>Forward</b>: position up / down by '+t.pos_step+'.</li><li><b>Middle held '+t.wheel_hold_ms+' ms, then wheel</b>: angle up / down by '+t.angle_step+'°.</li>'}
function drawTune(){const el=$('tune');if(!S.tune)return;if(!el.children.length){TUNE.forEach(([k,a,l,lo,hi])=>{const h=/_hold_ms$/.test(k)?' class="th"':'';el.insertAdjacentHTML('beforeend','<label for="t_'+a+'"'+h+'>'+l+'</label><input type="number" id="t_'+a+'"'+h+' data-k="'+k+'" data-a="'+a+'" min="'+lo+'" max="'+hi+'">')});el.querySelectorAll('input').forEach(i=>{i.oninput=()=>{dTune=true}})}
// since Feather v15 each chord has its own hold time; these three only matter to older firmware
el.querySelectorAll('.th').forEach(x=>{x.hidden=!!S.chords});
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
// ---- chord editor (Feather v15+): rows of [first button, second button, tap, hold, ms]
const BTN=['','Left','Right','Middle','Back','Forward','Wheel'];
const ACT=['Nothing','Next DPI stage','Previous DPI stage','DPI stage 1','DPI stage 2','DPI stage 3','DPI stage 4','DPI stage 5','Angle up','Angle down','Angle reset','Position up (toward the back)','Position down (toward the front)','Position reset','Wi-Fi on/off','Motion Sync on/off','Angle Snap on/off','Lift-off distance: next'];
let chEdit=[],chDirty=false,chSeq=-1,chWait=-1,chWaitAt=0,chLost=false;
function opts(list,from,sel){let h='';for(let i=from;i<list.length;i++)h+='<option value="'+i+'"'+(i===sel?' selected':'')+'>'+list[i]+'</option>';return h}
function chProblems(){const seen={};return chEdit.map((r,i)=>{const [m,t,a0,h,ms]=r,a=a0&127,k=m+'.'+t;let e='';
if(m===t)e='Pick two different buttons.';else if(!a&&!h)e='Pick at least one action.';else if((a0&128)&&!a)e='A double tap needs a tap action.';else if(t!==6&&h&&ms<100)e='A hold needs at least 100 ms.';else if(seen[k]!==undefined)e='Same buttons as chord '+(seen[k]+1)+'.';
if(seen[k]===undefined)seen[k]=i;return e})}
function chCheck(){const p=chProblems();document.querySelectorAll('#chlist .crow').forEach((row,i)=>{row.querySelector('.cerr').textContent=p[i]||''});
const ok=!p.some(Boolean)&&chEdit.length<=12,busy=chWait>=0;$('chSave').disabled=!chDirty||!ok||busy;$('chRevert').disabled=!chDirty;$('chAdd').disabled=chEdit.length>=12||busy;$('chfs').disabled=busy;
const wifi=chEdit.some(r=>(r[2]&127)===14||r[3]===14);$('chnote').hidden=wifi;$('chnote').textContent='No chord switches Wi-Fi. Unless Wi-Fi is set to stay on, the QT Py\'s own button would then be the only way to turn it on.'}
function chBuild(){const el=$('chlist');el.innerHTML='';chEdit.forEach((r,i)=>{const w=r[1]===6,n=i+1;
el.insertAdjacentHTML('beforeend','<div class="crow" data-i="'+i+'"><div class="cwhen"><span>Hold</span><select class="cm" aria-label="Chord '+n+': button held first">'+opts(BTN.slice(0,6),1,r[0])+'</select><span>then</span><select class="ct" aria-label="Chord '+n+': second button">'+opts(BTN,1,r[1])+'</select><button class="sq x" aria-label="Remove chord '+n+'">×</button></div>'+
'<div class="cdo"><label><span>'+(w?'Wheel up':'Tap')+'</span><select class="ca">'+opts(ACT,0,r[2]&127)+'</select>'+(w?'':'<span class="dbl"><input type="checkbox" class="c2" aria-label="Chord '+n+': double tap only"'+((r[2]&128)?' checked':'')+'> Double tap only</span>')+'</label><label><span>'+(w?'Wheel down':'Hold')+'</span><select class="ch">'+opts(ACT,0,r[3])+'</select></label>'+
'<label><span>'+(w?'Held first (ms)':'Hold time (ms)')+'</span><input type="number" class="cms" min="0" max="2550" step="10" value="'+r[4]+'"'+((!w&&!r[3])?' disabled':'')+'></label></div><p class="cerr"></p></div>')});
el.querySelectorAll('.crow').forEach(row=>{const i=+row.dataset.i;const set=(k,v,rebuild)=>{chEdit[i][k]=v;chDirty=true;rebuild?chBuild():chCheck()};
row.querySelector('.cm').onchange=e=>set(0,+e.target.value);
row.querySelector('.ct').onchange=e=>{const v=+e.target.value;if(v===6)chEdit[i][2]&=127;set(1,v,true)};
row.querySelector('.ca').onchange=e=>set(2,(+e.target.value)|(chEdit[i][2]&128));
const c2=row.querySelector('.c2');if(c2)c2.onchange=e=>set(2,(chEdit[i][2]&127)|(e.target.checked?128:0));
row.querySelector('.ch').onchange=e=>{const v=+e.target.value;if(v&&!chEdit[i][3]&&chEdit[i][1]!==6&&chEdit[i][4]<100)chEdit[i][4]=500;set(3,v,true)};
row.querySelector('.cms').oninput=e=>set(4,Math.max(0,Math.min(2550,Math.round(+e.target.value||0))));
row.querySelector('.x').onclick=()=>{chEdit.splice(i,1);chDirty=true;chBuild()}});chCheck()}
function chAdd(){const used=new Set(chEdit.map(r=>r[0]+'.'+r[1]));let m=3,t=1;for(const c of [[3,1],[3,2],[3,4],[3,5],[3,6],[4,1],[4,2],[5,1],[5,2],[4,6],[5,6],[1,6]])if(!used.has(c[0]+'.'+c[1])){[m,t]=c;break}
chEdit.push([m,t,0,0,0]);chDirty=true;chBuild()}
function chRevert(){chDirty=false;chSeq=-1;chWait=-1;chLost=false;drawChordEditor()}
function chSave(){const r=chEdit.map(x=>x.join('.')).join(',');chWait=S&&S.chords?S.chords.seq:-1;chWaitAt=Date.now();chLost=false;chCheck();$('chstate').textContent='Saving to the Feather…';
post('/api/chords?r='+encodeURIComponent(r)).then(x=>{say(x,'Sending the chords to the Feather');if(!x.ok){chWait=-1;chCheck()}})}
function chDefaults(){if(!S||!S.chords||!confirm('Replace your chords with the built-in ones (as they came)?'))return;chWait=S.chords.seq;chWaitAt=Date.now();chLost=false;chCheck();$('chstate').textContent='Saving to the Feather…';
post('/api/chords?defaults=1').then(x=>{say(x,'Back to the built-in chords');if(x.ok)chDirty=false;else chWait=-1;chCheck()})}
function us(v){return v>=1020?'1020+':String(v)}
// frame timing (Feather v15+; the whole frame from v19): a bar of each 1 ms frame's use, then the parts
function drawPerf(pf){const el=$('perf');el.hidden=!pf;if(!pf)return;const v19=pf.missed_1s!==undefined,f=x=>us(x)+' µs';
const rows=[];if(v19)rows.push(['USB to the mouse','1 ms interrupt',f(pf.usb_avg_us),f(pf.usb_max_us)]);
rows.push(['Remapping','',pf.avg_us.toFixed(1)+' µs',pf.max_us+' µs']);rows.push(['chord check','',pf.chords_avg_us.toFixed(1)+' µs',pf.chords_max_us+' µs',1]);
if(v19)rows.push(['Frame finished','after its tick',f(pf.tick_avg_us),f(pf.tick_max_us)]);
$('ptb').innerHTML=rows.map(r=>'<tr><td'+(r[4]?' class="sub"':'')+'>'+(r[4]?'of which ':'')+r[0]+(r[1]?' <span class="mut">'+r[1]+'</span>':'')+'</td><td>'+r[2]+'</td><td>'+r[3]+'</td></tr>').join('');
const a=v19?pf.usb_avg_us+pf.tick_avg_us:pf.avg_us,w=v19?pf.usb_max_us+pf.tick_max_us:pf.max_us,pa=Math.min(100,a/10),pw=Math.min(100,w/10);
$('buda').style.width=pa+'%';$('budw').style.width=pw+'%';
const s='Each 1 ms frame: '+Math.round(pa)+'% used on average, '+Math.round(pw)+'% at worst'+(v19?'':' (remapping only; Feather v19 measures the whole frame)')+'.';$('budt').textContent=s;$('bud').setAttribute('aria-label',s);
const ps=$('pskip');ps.hidden=!v19;if(v19){ps.textContent='Frames skipped: '+pf.missed_1s+' in the last second · '+pf.missed_total+' since the Feather started (plugging in or switching to a device pauses it about half a second).';ps.classList.toggle('bad',pf.missed_1s>0)}}
// recent actions (Feather v21): [age s, source, buttons, action, value], newest first
const EVSRC=['','tap','hold','double tap','wheel up','wheel down'],DEV=['device','Windows','Android','iPhone','Mac','Linux','script'];
function evText(e){const[age,src,b,a,v]=e,t=new Date(Date.now()-age*1000).toLocaleTimeString();let s,ang,stg=false;
if(src>=1&&src<=5){s=BTN[b>>4]+' + '+(src>=4?'wheel '+(src===4?'up':'down'):BTN[b&15]+' '+EVSRC[src])+': '+ACT[a];ang=a>=8&&a<=10}
else if(src===6){s=b===1?'Page: DPI stage':b===2?'Page: angle':b===3?'Page: position':'Page: command '+b;ang=b===2;stg=b===1;
// QT Py fw21: the request that matches it (same command and value, nearest in time) names the sender
let m=null,dm=11;(S.sets||[]).forEach(x=>{const d=Math.abs(x[0]-age);if(x[1]===b&&x[2]===v&&d<dm){m=x;dm=d}});
if(m)s=s.replace('Page:','Page ('+DEV[m[3]]+' '+m[4]+'):')}
else{const n=b===1?'Angle':'Position';ang=b===1;s=src===7?n+' assumed (the mouse didn\'t answer)':src===8?n+': the mouse kept its own value (writes didn\'t take)':n+' sent again (the mouse woke)'}
if(v!==127)s+=' → '+(stg?'stage '+(v+1):ang?v+'°':v);return t+'  '+s}
function drawEvents(){const ev=S.events,box=$('evbox');box.hidden=!ev||!ev.length;if(!ev||!ev.length)return;$('evlast').textContent='Last change: '+evText(ev[0]);$('evlist').innerHTML=ev.map(e=>'<li>'+evText(e).replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</li>').join('')}
function drawChordEditor(){const c=S.chords,ed=$('ched'),leg=$('chords'),pf=S.perf;
drawPerf(pf);
if(!c){ed.hidden=true;leg.hidden=false;drawChords();$('chstate').textContent=S.link&&S.fw?'Editing chords needs Feather firmware v15.':'';return}
ed.hidden=false;leg.hidden=true;
if(chWait>=0&&c.fresh&&c.seq!==chWait){chWait=-1;chDirty=false}
// the Feather never showed a new table (a frame lost or refused): stop waiting, keep the edits
if(chWait>=0&&Date.now()-chWaitAt>8000){chWait=-1;chLost=true}
if(!chDirty&&c.fresh&&c.rules&&c.seq!==chSeq){chSeq=c.seq;chEdit=c.rules.map(r=>r.slice());chBuild()}else chCheck();
let s=(c.flags&1)?'Saved on the Feather':'The Feather\'s built-in chords';if(c.saved_here)s+=', with a copy on the QT Py';s+='.';
if((c.flags&2)||chWait>=0)s='Saving to the Feather…';if(c.flags&4)s+=' The last table sent was refused.';if(c.flags&8)s+=' The Feather couldn\'t write its flash.';if(chLost&&chWait<0)s+=' The Feather didn\'t take the last table: press Save again.';
$('chstate').textContent=s}
// how Wi-Fi gets switched, for the Wi-Fi card
function wifiChord(){const c=S.chords&&S.chords.rules;if(!S.chords)return 'Hold Middle + Right';if(!c)return '';const r=c.find(x=>(x[2]&127)===14||x[3]===14);return r?(r[3]===14?'Hold ':'Press ')+BTN[r[0]]+' + '+BTN[r[1]]:''}
function drawRadio(){const r=S.radio;let s=r.mode==='sta'?'On '+r.ssid+' at '+r.ip:r.mode==='ap'?'Setup network GW-Settings at '+r.ip:'Joining…';
s+=r.off_in_s<0?' · stays on':' · turns off in '+Math.ceil(r.off_in_s/60)+' min without use';
const w=S.wifi;if(w&&r.mode==='sta'&&w.rssi)s+=' · signal '+w.rssi+' dBm'+(w.drops?' · '+w.drops+' dropout'+(w.drops>1?'s':'')+' since it started':'');$('radio').textContent=s;
document.querySelectorAll('#keep button').forEach(b=>b.classList.toggle('on',Number(b.dataset.m)===r.keep));$('keepnote').hidden=r.keep!==2;
const wc=wifiChord();$('keepnote').textContent='Wi-Fi stays on and comes on by itself at power-up. '+(wc?wc+' to switch it off for a session.':'The QT Py\'s own button switches it off for a session.')}
function saveWifi(){const b=new URLSearchParams({ssid:$('ssid').value,pass:$('pass').value});fetch('/api/wifi',{method:'POST',body:b}).then(()=>{$('radio').textContent='Joining '+$('ssid').value+'… reconnect to that network and open gwolves.local'})}

function render(){const st=$('state');
if(!S.link){st.innerHTML='<span class="chip bad">No answer from the Feather</span><span class="mut">Check the 4-pin cable.</span>'}
else{st.innerHTML='<span class="chip'+(S.warg||S.vuk?' ok':'')+'">'+(S.warg?'Warg connected':S.vuk?'VUK connected':'No G-Wolves mouse')+'</span>'+(S.warg&&S.paused?'<span class="chip bad">mouse didn\'t answer (asleep?)</span><span class="mut">Move it, then press Read from mouse.</span>':S.warg&&S.reads===false?'<span class="chip">mouse hasn\'t answered a read yet</span>':'')+(S.refreshing?'<span class="chip">reading…</span>':'')+(S.pending?'<span class="chip">sending…</span>':'')}
paintStages();if(S.palette&&S.stage!=null&&S.palette[S.stage])fav(S.palette[S.stage]);
['angle','pos'].forEach(k=>{if(!edit[k]||Date.now()-edit[k]>1500)setVal(k,S[k])});
if(S.tune){$('ahome').textContent=S.tune.angle_home;$('phome').textContent=S.tune.pos_home}
drawChordEditor();drawTune();drawRadio();drawOpt();drawEvents();drawLevel();foldsApply();
const bt=S.battery;$('batt').textContent=bt?bt.pct+'% · '+(bt.mv/1000).toFixed(2)+' V'+(bt.age_s<0?'':' · read '+(bt.age_s<60?'just now':Math.round(bt.age_s/60)+' min ago')):'not read yet: move the mouse, then Read from mouse';
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
$('ms').addEventListener('pointerdown',e=>{if(!S||e.target===$('ms'))return;msDrag=e.target.id==='msh'?'angle':'pos';msOff=0;if(msDrag==='pos'&&$('mss').contains(e.target))msOff=new DOMPoint(e.clientX,e.clientY).matrixTransform($('ms').getScreenCTM().inverse()).y-msY(Number($('pos').value));$('ms').setPointerCapture(e.pointerId);e.preventDefault();msMove(e)});
$('ms').addEventListener('pointermove',msMove);['pointerup','pointercancel'].forEach(t=>$('ms').addEventListener(t,()=>{msDrag=null}));
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
// Level your swipes, like Razer's mouse rotation tool: the main direction of the raw movement (pointer lock) while
// swiping left and right. The Warg turns its output by its angle setting, so what's measured is the tilt left at
// the current angle; the suggestion corrects that: on the Warg a higher angle tilts swipes down to the right, so a
// rise to the right needs a higher angle and a fall a lower one (the user's rule, 2026-10-18).
const LV=$('lvc');let lv=null,lvOn=false,lvFs=false;
function lvBegin(){if(!S||S.angle==null){$('lvr').textContent='The page doesn\'t know the mouse\'s angle yet: press Read from mouse at the top, then try again.';return}
lv={x:0,y:0,pts:[[0,0]],sxx:0,syy:0,sxy:0,dist:0,a0:S.angle};lvOn=true;lvFs=false;$('lvb').hidden=true;
$('lvr').textContent='Measuring at '+S.angle+'°: swipe left and right a few times, then right-click or press Esc.';
// the whole screen to swipe in (the window if full screen is refused), the pointer captured so it never hits an edge
const w=$('lvwrap'),lock=()=>{if(lvOn&&LV.requestPointerLock)LV.requestPointerLock()};w.classList.add('full');
if(w.requestFullscreen)w.requestFullscreen().then(()=>{lvFs=true;lock()}).catch(lock);else lock();lvDraw()}
function lvStop(){if(!lvOn)return;lvOn=false;if(document.pointerLockElement===LV)document.exitPointerLock();if(document.fullscreenElement)document.exitFullscreen().catch(()=>{});$('lvwrap').classList.remove('full');lvDraw();lvResult();$('lvl').scrollIntoView({block:'nearest'})}
document.addEventListener('fullscreenchange',()=>{if(lvOn&&lvFs&&!document.fullscreenElement)lvStop()});
LV.addEventListener('mousedown',e=>{if(e.button===2){e.preventDefault();lvStop()}else if(e.button===0&&!lvOn)lvBegin()});
LV.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('mousemove',e=>{if(!lvOn)return;const dx=e.movementX||0,dy=e.movementY||0;if(!dx&&!dy)return;
lv.x+=dx;lv.y+=dy;lv.pts.push([lv.x,lv.y]);lv.sxx+=dx*dx;lv.syy+=dy*dy;lv.sxy+=dx*dy;lv.dist+=Math.hypot(dx,dy);if(lv.pts.length%3===0)lvDraw()});
document.addEventListener('pointerlockchange',()=>{if(lvOn&&document.pointerLockElement!==LV)lvStop()});
document.addEventListener('keydown',e=>{if(lvOn&&e.key==='Escape')lvStop()});
// screen angle of the swipes (y down, so negative = rising to the right)
function lvTheta(m){return .5*Math.atan2(2*m.sxy,m.sxx-m.syy)}
function lvDraw(){const W=LV.clientWidth,H=LV.clientHeight,d=devicePixelRatio||1;if(LV.width!==Math.round(W*d)){LV.width=Math.round(W*d);LV.height=Math.round(H*d)}
const c=LV.getContext('2d');c.setTransform(d,0,0,d,0,0);c.clearRect(0,0,W,H);if(!lv||lv.pts.length<2)return;
let x0=1/0,x1=-1/0,y0=1/0,y1=-1/0,cx=0,cy=0;for(const[x,y]of lv.pts){x0=Math.min(x0,x);x1=Math.max(x1,x);y0=Math.min(y0,y);y1=Math.max(y1,y);cx+=x;cy+=y}cx/=lv.pts.length;cy/=lv.pts.length;
// the same scale on both axes, so a tilt looks as big as it is
const k=Math.min((W-30)/Math.max(1,x1-x0),(H-30)/Math.max(1,y1-y0),1),px=x=>W/2+(x-(x0+x1)/2)*k,py=y=>H/2+(y-(y0+y1)/2)*k;
c.strokeStyle='rgba(255,255,255,.12)';c.beginPath();c.moveTo(0,py(cy));c.lineTo(W,py(cy));c.stroke();
c.strokeStyle='#9a9a9a';c.lineWidth=1;c.beginPath();lv.pts.forEach(([x,y],i)=>i?c.lineTo(px(x),py(y)):c.moveTo(px(x),py(y)));c.stroke();
if(!lvOn&&lv.sxx+lv.syy>0){const t=lvTheta(lv),L=W*2;c.strokeStyle='#3ddc4a';c.lineWidth=2;c.beginPath();c.moveTo(px(cx)-Math.cos(t)*L,py(cy)-Math.sin(t)*L);c.lineTo(px(cx)+Math.cos(t)*L,py(cy)+Math.sin(t)*L);c.stroke()}}
function lvResult(){const m=lv;if(!m||m.dist<800||m.sxx<4*m.syy){$('lvr').textContent=!m||m.dist<800?'Too little movement: swipe further, a few times each way.':'Those swipes were not mostly sideways: swipe left and right.';return}
const tilt=-lvTheta(m)*180/Math.PI,a0=m.a0;
const want=Math.max(-30,Math.min(30,Math.round(a0+tilt)));
const dir=Math.abs(tilt)<.05?'level':(tilt>0?'rise to the right':'fall to the right');
let h='Your swipes '+(Math.abs(tilt)<.05?'are level':dir+' by <b>'+Math.abs(tilt).toFixed(1)+'°</b>')+' at the current angle of '+a0+'°.';
if(want===a0){h+=' That rounds to no change: keep '+a0+'°.';$('lvb').hidden=false;$('lvset').hidden=true}
else{h+=' Suggested angle: <b>'+want+'°</b>.';
$('lvset').hidden=false;$('lvset').textContent='Set '+want+'°';$('lvset').onclick=()=>post('/api/set?angle='+want).then(r=>say(r,'Angle set to '+want+'°'));$('lvb').hidden=false}
$('lvr').innerHTML=h}
$('lvagain').onclick=()=>lvBegin();
function drawLevel(){const sn=!!(S.opt&&(S.opt.snap===true||S.opt.snap===1));$('lvsnap').hidden=!sn}
new ResizeObserver(()=>lvDraw()).observe(LV);
// fold-outs open as they were left (kept on the QT Py: the page's address changes, and it's shared by your devices)
const FOLDS=['fchords','fwifi','evbox'];let foldMask=null;
function foldNow(){let m=0;FOLDS.forEach((id,i)=>{if($(id).open)m|=1<<i});return m}
function foldsApply(){if(foldMask!==null||!S||S.ui==null)return;if(S.ui>=0)FOLDS.forEach((id,i)=>{$(id).open=!!(S.ui>>i&1)});foldMask=foldNow()}
FOLDS.forEach(id=>$(id).addEventListener('toggle',()=>{if(foldMask===null)return;const m=foldNow();if(m!==foldMask){foldMask=m;post('/api/ui?f='+m)}}));
poll();
</script></body></html>)HTML";
