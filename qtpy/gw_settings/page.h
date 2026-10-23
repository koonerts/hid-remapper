#pragma once
// The settings page, served at / (one file, no external resources). fw35: served gzipped from page_gz.h: run
// page_gz.ps1 after editing this file (the build fails until then).
static constexpr char PAGE_HTML[] = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><meta name="gw-rest" content="">
<title>Warg settings</title><link rel="icon" id="fav" href="data:,">
<style>
:root{--bg:#f3f4f6;--card:#fff;--fg:#17191c;--mut:#646b75;--line:#dcdfe4;--soft:#eef0f3;--acc:#1f6fe0;--on:#fff;--ok:#1a8a4a;--bad:#c23b2e;--warn:#9a5a00;--warnbg:#fff4de}
@media (prefers-color-scheme:dark){:root{--bg:#121416;--card:#1b1e22;--fg:#e8eaed;--mut:#9aa1ab;--line:#2e333a;--soft:#24282e;--acc:#5a9cf5;--on:#0c1420;--ok:#4cc27e;--bad:#f0796c;--warn:#f0b65a;--warnbg:#33290f}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.45 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:1080px;margin:0 auto;padding:4px 16px 72px}
/* QT Py fw37: title row, a status bar + tabs that stay at the top, cards in two columns where there is room */
.top{max-width:1080px;margin:0 auto;padding:14px 16px 6px}.top h1{margin:0}
.bar{position:sticky;top:0;z-index:50;background:var(--bg);border-bottom:1px solid var(--line)}
.barin{max-width:1080px;margin:0 auto;padding:8px 16px 0}
#state,#sbar{display:contents}
.tabs{display:flex;gap:2px;margin-top:6px;overflow-x:auto;scrollbar-width:none}
.tabs button{border:0;border-bottom:2px solid transparent;border-radius:0;background:none;padding:8px 14px;color:var(--mut);font-weight:600;white-space:nowrap}
.tabs button.on{color:var(--fg);border-bottom-color:var(--acc)}
.cols{display:grid;grid-template-columns:repeat(auto-fit,minmax(min(100%,430px),1fr));gap:0 16px;align-items:start}
/* QT Py fw39: saved calibration runs as one table */
.cbx{overflow-x:auto}.cbm{border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}
.cbm th,.cbm td{padding:4px 7px;text-align:right;white-space:nowrap;border-bottom:1px solid var(--line)}
.cbm th:first-child,.cbm td:first-child{text-align:left;position:sticky;left:0;background:var(--card)}
.cbm th{font-weight:600;color:var(--mut);vertical-align:bottom}.cbm th.rh{text-align:center}.cbm th.rh.on label{color:var(--fg)}
.cbm td.rc{text-align:center}.cbm .up{color:var(--acc)}.cbm .dn{color:var(--warn)}.cbm .wn{color:var(--warn)}.cbm .use{color:var(--ok);font-size:12px}
.cbm tr.ft td{border-bottom:0;padding-top:8px}.cbm tr.ft button{padding:3px 8px;font-size:12.5px}
.chipb{font-size:12px;padding:2px 10px;border-radius:99px;color:var(--mut)}.chipb.on{background:var(--acc);border-color:var(--acc);color:var(--on)}
.cols>.wide{grid-column:1/-1}
details.cap{border:1px solid var(--line);border-radius:10px;padding:8px 10px;margin:6px 0}details.cap>summary{font-size:13px}details.cap[open]>summary{margin-bottom:6px}
h1{font-size:21px;font-weight:650;margin:4px 0 8px;letter-spacing:-.01em}
h2{font-size:15px;font-weight:650;margin:0}
p{margin:8px 0 0}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:14px;margin:12px 0}
.hd{display:flex;align-items:center;justify-content:space-between;gap:10px;margin-bottom:12px}
.mut{color:var(--mut);font-size:13px}
.row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
.chips{display:flex;gap:6px;flex-wrap:wrap;align-items:center;min-height:26px}
.chip{font-size:12.5px;font-weight:600;padding:3px 9px;border-radius:99px;background:var(--soft);color:var(--mut)}
button.chip{border:0;cursor:pointer;color:var(--acc);font-size:12.5px}
[hidden]{display:none!important}  /* a .row's display:flex beat the hidden attribute (the swipe test's buttons, the cm/360 answers) */
.c3f label{display:flex;flex-direction:column;gap:3px;font-size:13px;color:var(--mut)}.c3f input,.c3f select{width:110px}
.c3t{width:100%;border-collapse:collapse;margin-top:8px;font-size:14px}.c3t td,.c3t th{padding:4px 6px;text-align:left;border-top:1px solid var(--line)}.c3t th{color:var(--mut);font-weight:600}.c3t button{padding:2px 8px}
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
/* QT Py fw25: which hosts switch to each stage (lit = this one) */
.hch{grid-column:3/-1;display:flex;gap:6px;flex-wrap:wrap;align-items:center}.hch:empty{display:none}
.hch button{font-size:12px;padding:2px 10px;border-radius:99px;color:var(--mut)}.hch button.on{background:var(--acc);border-color:var(--acc);color:var(--on);font-weight:600}
/* QT Py fw26: direction sensitivity */
.dgrow{display:flex;gap:14px;align-items:center;flex-wrap:wrap}
#dgsvg{width:230px;height:230px;flex:none;touch-action:none}
#dgsvg .ring{fill:none;stroke:var(--line)}#dgsvg .one{stroke:var(--mut);stroke-dasharray:3 3}#dgsvg .spoke{stroke:var(--line)}
#dgsvg .poly{fill:var(--acc);fill-opacity:.15;stroke:var(--acc);stroke-width:2}#dgsvg .saved{fill:none;stroke:var(--mut);stroke-dasharray:4 3}
#dgsvg .sug{fill:none;stroke:var(--ok);stroke-width:1.5;stroke-dasharray:6 3}
#dgsvg .hd{fill:var(--acc);cursor:grab}#dgsvg text{fill:var(--mut);font-size:13px;text-anchor:middle;dominant-baseline:middle}
.dgvals{display:grid;grid-template-columns:auto 70px auto 70px;gap:6px 8px;align-items:center}.dgvals input{width:70px}
#cbwrap{display:none;position:fixed;inset:0;z-index:1000;background:#000}#cbwrap.on{display:block}#cbc{display:block;width:100vw;height:100vh;cursor:crosshair}
#cbhint{position:absolute;top:14px;left:0;right:0;text-align:center;color:#bbb;font-size:14px;pointer-events:none}
.cbt{border-collapse:collapse;font-size:13px;margin-top:6px}.cbt td,.cbt th{padding:3px 8px;text-align:right}.cbt th{color:var(--mut);font-weight:600}.cbt td:first-child,.cbt th:first-child{text-align:left}
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
.ms.unk .sn,.ms.unk .arc,.ms.unk .mv{opacity:.3}
.ms .mv{fill:var(--acc);font-size:13px;font-weight:600;font-variant-numeric:tabular-nums;stroke:var(--soft);stroke-width:3px;paint-order:stroke}
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
#pskip.bad{color:var(--bad)}.tip{cursor:help}.pt tr.tip td:first-child{text-decoration:underline dotted var(--mut);text-underline-offset:3px}
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
</style></head><body><div class="top"><div class="row" style="justify-content:space-between"><h1>Warg settings</h1><div class="row"><button onclick="readMouse()">Read from mouse</button><button onclick="post('/api/flash?c=ffffff').then(r=>say(r,'Flash sent'))">Test flash</button></div></div></div>
<header class="bar"><div class="barin"><div class="chips"><span id="state" class="chips"><span class="chip">Connecting…</span></span><span id="sbar" class="chips"></span></div>
<nav class="tabs" role="tablist" aria-label="Sections"><button role="tab" id="tb-mouse" onclick="tabShow('mouse',1)">Mouse</button><button role="tab" id="tb-aim" onclick="tabShow('aim',1)">Aim</button><button role="tab" id="tb-setup" onclick="tabShow('setup',1)">Setup</button><button role="tab" id="tb-diag" onclick="tabShow('diag',1)">Diagnostics</button></nav></div></header>
<main>
<section class="tab" id="t-mouse" role="tabpanel"><div class="cols">
<div class="card"><div class="hd"><h2>DPI stages</h2><span class="cur" id="cur"></span></div>
<div id="rows"></div>
<div class="row" style="margin-top:10px;justify-content:space-between"><div class="row"><button class="p" id="stApply" onclick="applyStages()" disabled>Apply</button><button id="stRevert" onclick="revert()" disabled>Revert</button></div>
<label class="chk"><input type="checkbox" id="sepy" onchange="sepChange()"> Separate X / Y</label></div>
<p class="mut" id="stnote">Tap a number to switch stage right away. Colour and DPI edits are sent when you press Apply.</p><p class="mut" id="hstnote" hidden>Under a stage, a lit host switches to it whenever the ATEN connects the mouse to that host: tap a host to move it here, tap it again to stop switching. Names and details are under Hosts.</p></div>

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
<text class="mv" id="msat" text-anchor="middle"></text><text class="mv" id="mspt"></text>
</svg><div class="flds">
<div class="fld"><div class="lbl"><span>Angle</span><b id="anglev">–</b></div>
<div class="row"><button class="sq" onclick="step('angle',-1)" aria-label="Angle down">−</button><input type="range" id="angle" min="-30" max="30" step="1" autocomplete="off" aria-label="Sensor angle"><button class="sq" onclick="step('angle',1)" aria-label="Angle up">+</button><button class="sq" id="ahomeb" onclick="home('angle')" aria-label="Reset">↺</button></div></div>
<div class="fld"><div class="lbl"><span>Position</span><b id="posv">–</b></div>
<div class="row"><button class="sq" onclick="step('pos',-1)" aria-label="Position down">−</button><input type="range" id="pos" min="-100" max="101" step="1" autocomplete="off" aria-label="Sensor position"><button class="sq" onclick="step('pos',1)" aria-label="Position up">+</button><button class="sq" id="phomeb" onclick="home('pos')" aria-label="Reset">↺</button></div></div></div></div>
<p class="mut">Drag the sensor to move it, or the arrow's tip to turn it. Dashed: where the reset chords (and ↺) put it. The outline is a generic mouse, and position is in the Warg's own units, not mm.</p></div>

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
</div></section>
<section class="tab" id="t-aim" role="tabpanel" hidden><div class="cols">
<div class="card" id="dgc" hidden><div class="hd"><h2>Direction sensitivity</h2><span class="cur" id="dgst"></span></div>
<p class="mut" style="margin:0 0 8px">A sensitivity for each direction you move the mouse, like Corsair's PureVector: a direction you tend to fall short in gets a little more, one you overshoot a little less. Calibrate with flicks on this computer, or drag the points. 1.00 everywhere changes nothing.</p>
<div class="dgrow"><svg id="dgsvg" viewBox="-180 -180 360 360" aria-label="Sensitivity per direction: drag a point outwards for more"></svg><div class="dgvals" id="dgvals"></div></div>
<div class="row" style="margin-top:10px"><span class="mut">On for</span><div class="hch" id="dghosts"></div></div>
<div class="row" style="margin-top:10px"><button class="p" id="dgsave" disabled onclick="dgSave()">Save</button><button id="dgrev" disabled onclick="dgRevert()">Revert</button><button onclick="dgFlat()">All 1.00</button><button onclick="cbBegin()">Calibrate…</button></div>
<div class="row" style="margin-top:10px"><label class="mut">Flick lengths <input type="text" id="cblen" value="10" style="width:7em" aria-label="Flick lengths in cm"> cm</label><label class="mut">Rounds per length <input type="number" id="cbrpl" value="3" min="1" max="8" style="width:4.5em" aria-label="Rounds per length"></label></div>
<p class="note" id="cbmsg" hidden></p>
<p class="mut" id="dgnote" style="margin:8px 0 0">Calibrate: flicks at the lengths you use in game (e.g. "6, 12, 18" cm), each round the 8 directions. Click the middle target, flick to the one that appears and click it, flick back and click the middle again. For long flicks the test lowers the DPI and puts it back afterwards. Keep Windows' pointer speed at 6 of 11 (the default) with "Enhance pointer precision" off.</p>
<div id="cbres" hidden></div>
<div id="cbwrap"><canvas id="cbc" aria-label="Flick area"></canvas><div id="cbhint"></div></div>
</div>
<div class="card" id="lvl"><div class="hd"><h2>Level your swipes</h2></div>
<p class="mut" style="margin:0 0 8px">Suggests a sensor angle that makes left-right swipes come out level, starting from the angle the mouse has now. Use the mouse on this computer: click the box, swipe left and right a few times the way you do in game, then right-click or press Esc.</p>
<p class="note" id="lvsnap" hidden>Angle Snap is on: it straightens swipes, so it hides the tilt. Turn it off under More mouse settings to measure.</p>
<div id="lvwrap"><canvas id="lvc" aria-label="Swipe area"></canvas><div id="lvhint">Swipe left and right a few times · right-click or Esc to finish</div></div>
<p id="lvr" class="mut" style="margin:8px 0 0">Click the box to start.</p>
<div class="row" id="lvb" hidden style="margin-top:8px"><button class="p" id="lvset"></button><button id="lvagain">Measure again</button></div>
</div>
<div class="card" id="c3card"><div class="hd"><h2>cm/360 in game</h2></div>
<p class="mut" style="margin:0 0 10px">How far the mouse moves for one full turn in game, through the XIM. The Feather turns the view by itself at a steady speed, so the distance is exact; you only say where the crosshair stopped, and each answer halves the range. In BF6: stand still (firing range), put the crosshair on a sharp vertical edge, let go of the mouse (touching it stops the turn), then Turn.</p>
<p class="mut" id="c3off" hidden></p>
<div class="row c3f"><label>Guess, cm/360<input type="number" id="c3est" step="0.1" min="2" max="300"></label><label>Speed<select id="c3sp"><option value="5">5 cm/s</option><option value="10" selected>10 cm/s</option><option value="20">20 cm/s</option><option value="40">40 cm/s</option><option value="80">80 cm/s</option></select></label><label>Turns<select id="c3tn"><option>1</option><option>2</option><option>3</option></select></label></div>
<div class="row" style="margin-top:10px"><button class="p" id="c3go" onclick="c3Turn()">Turn</button><button id="c3stop" onclick="c3Stop()">Stop</button><button id="c3reset" onclick="c3Reset()">Start over</button></div>
<p id="c3now" style="margin:10px 0 6px"></p>
<div class="row" id="c3ans" hidden><span>Where did the crosshair stop?</span><button onclick="c3Ans(-1)">Short of the edge</button><button class="p" onclick="c3Ans(0)">On it</button><button onclick="c3Ans(1)">Past it</button></div>
<div id="c3cbox" hidden><h3 style="margin:16px 0 6px;font-size:15px">Or count your own turns</h3>
<p class="mut" style="margin:0 0 8px">Turn by hand in game at your own speed: put the crosshair on an edge, Start counting, turn whole turns back to that edge (lifting the mouse and small corrections are fine), Stop. The Feather counts what reaches the XIM, after your direction gains.</p>
<div class="row c3f"><label>Full turns<select id="c3ctn"><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select></label></div>
<div class="row" style="margin-top:10px"><button class="p" id="c3cgo" onclick="c3Count()">Start counting</button><button id="c3csave" onclick="c3CSave()">Save</button></div>
<p id="c3cnow" style="margin:10px 0 0"></p></div>
<table class="c3t" id="c3list"></table></div>
<details class="card wide" id="cbsaved" hidden><summary><div><h2>Calibration runs <span class="mut" id="cbsavedn"></span></h2><div class="mut" id="cbsrc"></div></div></summary><div id="cbsavedlist"></div></details>
</div></section>
<section class="tab" id="t-setup" role="tabpanel" hidden><div class="cols">
<div class="card" id="fchords"><div class="hd"><div><h2>Chords</h2><div class="mut">Hold one button, press another: what each pair does</div></div></div>
<div id="ched" hidden>
<p class="note" id="chnote" hidden></p>
<fieldset id="chfs" style="border:0;padding:0;margin:0;min-width:0"><div id="chlist"></div></fieldset>
<div class="row" style="margin-top:10px;justify-content:space-between"><div class="row"><button class="p" id="chSave" onclick="chSave()" disabled>Save chords</button><button id="chRevert" onclick="chRevert()" disabled>Revert</button></div>
<div class="row"><button id="chAdd" onclick="chAdd()">Add chord</button><button onclick="chDefaults()">Built-in chords</button></div></div>
<p class="mut">The second button never reaches the game while it's part of a chord; the first one always does. Pressing them the other way round isn't a chord. A tap acts when the second button comes up; with no hold action it acts as soon as it goes down.</p>
</div>
<p class="mut" id="chstate"></p>
<ul class="ch" id="chords"></ul>
<h3 class="sub">Tuning</h3>
<div class="tune" id="tune"></div>
<div class="row" style="margin-top:12px"><button class="p" onclick="saveTune()">Save tuning</button></div></div>

<div class="card"><div class="hd"><h2>Wi-Fi</h2><button onclick="post('/api/radio_off')">Turn off now</button></div>
<p class="mut" id="radio" style="margin:0 0 12px"></p>
<div class="opt"><span>Turns itself off</span><div class="seg" id="keep"><button data-m="0">after 10 min</button><button data-m="1">after 1 hour</button><button data-m="2">never</button></div></div>
<p class="mut" id="keepnote" hidden></p>
<details style="margin-top:12px" id="fwifi"><summary><span>Change Wi-Fi network</span></summary>
<p class="mut" style="margin:0 0 8px">The QT Py joins this network when its Wi-Fi comes on. 2.4 GHz only.</p>
<input type="text" id="ssid" placeholder="Network name" aria-label="Network name"><div style="height:6px"></div><input type="password" id="pass" placeholder="Password" aria-label="Password">
<div class="row" style="margin-top:8px"><button class="p" onclick="saveWifi()">Save and join</button></div></details></div>
</div></section>
<section class="tab" id="t-diag" role="tabpanel" hidden><div class="cols">
<div class="card" id="perfc"><div class="hd"><h2>Frame timing</h2><span class="mut">last second</span></div>
<div id="perf" hidden>
<div class="bud tip" role="img" id="bud" onclick="ptHelp({title:PTIP.bar})"><i id="budw"></i><i id="buda"></i></div>
<p class="mut" id="budt" style="margin:0"></p>
<table class="pt"><thead><tr><th></th><th>Average</th><th>Worst</th></tr></thead><tbody id="ptb"></tbody></table>
<p class="note" id="pthelp" hidden></p>
<p class="mut" id="pskip"></p></div>
<p class="mut" id="perfnone">Needs Feather firmware v19 or later.</p></div>

<details class="card" id="fedits" hidden><summary><div><h2>Recent page edits</h2><div class="mut" id="edlast"></div></div></summary>
<ol id="edlist"></ol><p class="mut">The last 20 edits made through this page, from any device. Kept on the QT Py, so they stay through refreshes, the ATEN switching ports and updates.</p></details>
</div>
<details class="card" id="fhosts" hidden><summary><div><h2>Hosts</h2><div class="mut" id="hauto"></div></div></summary>
<p class="mut">What the ATEN can connect the mouse to. The Feather recognises each kind of host by how it asks for its USB details, so both XIMs look the same and share one entry. Rename a host here; choose its stage under DPI stages.</p>
<div id="hprof"></div>
<h3 class="sub">USB hosts seen</h3><p class="mut" id="hlast"></p>
<p class="mut">What the host on the other side of the ATEN asked for while connecting, newest first; each power-on is one host. The QT Py keeps the last 8.</p>
<div id="hlist"></div></details>
</section>

<p class="mut" id="ver" style="text-align:center"></p>
</main><div class="toast" id="toast" role="status"></div>
<script>
let S=null,edit={},dCol=false,dDpi=false,dTune=false,hold=0,holdT=0,sepInit=false,deb={},tt;
const $=id=>document.getElementById(id);
const TUNE=[["angle_step","as","Angle step (°)",1,10],["pos_step","ps","Position step",1,25],["angle_home","ah","Angle reset target (°)",-30,30],["pos_home","ph","Position reset target",-100,101],["flash_ms","fl","Light flash length (ms)",100,5000],["pos_hold_ms","poh","Middle + Left hold for position reset (ms)",150,3000],["wheel_hold_ms","wh","Middle hold before the wheel steps angle (ms)",0,3000],["radio_hold_ms","rh","Middle + Right hold for Wi-Fi (ms)",200,3000]];
// QT Py fw22: every request carries this browser's clock, so the edits it saves get real times
function tq(u){return u+(u.includes('?')?'&':'?')+'t='+Math.floor(Date.now()/1000)}
// QT Py fw33: every request gives up after a few seconds (a request in flight when the ATEN cuts the power hung for minutes)
function fetchJ(u,o,ms){const c=new AbortController(),tm=setTimeout(()=>c.abort(),ms||4000);return fetch(u,Object.assign({signal:c.signal},o||{})).then(r=>r.json()).finally(()=>clearTimeout(tm))}
function post(u,b){return fetchJ(tq(u),{method:'POST',body:b}).catch(()=>({ok:false,err:'no answer'}))}
function toast(m,bad){const t=$('toast');t.textContent=m;t.className='toast show'+(bad?' bad':'');clearTimeout(tt);tt=setTimeout(()=>{t.className='toast'},2400)}
function say(r,m){toast(r.ok?m:'Failed: '+(r.err||'error'),!r.ok);return r}
function ink(c){const n=parseInt(c,16);return ((n>>16)*.299+(n>>8&255)*.587+(n&255)*.114)>140?'#111':'#fff'}
// tab icon: a mouse in the current DPI stage's colour
let favC='';
function fav(c){if(c===favC)return;favC=c;$('fav').href='data:image/svg+xml,'+encodeURIComponent("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'><path d='M16 1.5C23.5 1.5 26 7.5 26 14v8c0 6-4.5 8.5-10 8.5S6 28 6 22v-8C6 7.5 8.5 1.5 16 1.5Z' fill='#"+c+"' stroke='#222' stroke-width='1.8'/><path d='M16 2v10M6.6 12.6Q16 15.6 25.4 12.6' fill='none' stroke='#222' stroke-width='1.8'/></svg>")}
fav('1f6fe0');function readMouse(){post('/api/refresh').then(r=>say(r,'Reading from the mouse'))}
// QT Py fw22: while this page is in view and the Warg is connected but its angle or position is unknown (asleep, just
// plugged in), read from the mouse by itself every 5 s, so moving the mouse is enough
let autoRd=0;function autoRead(){if(!S||!S.link||!S.warg||S.refreshing||S.pending||document.visibilityState!=='visible')return;
if(S.angle!=null&&S.pos!=null)return;if(Date.now()-autoRd<5000)return;autoRd=Date.now();post('/api/refresh?auto=1')}

function rows(){return [...$('rows').children]}
function paintBtn(i,c){const b=rows()[i].querySelector('.st');b.style.background='#'+c;b.style.color=ink(c)}
function buildStages(){const el=$('rows');for(let i=0;i<5;i++){const n=i+1;el.insertAdjacentHTML('beforeend','<div class="srow"><button class="st" aria-label="Switch to stage '+n+'">'+n+'</button><input type="color" aria-label="Stage '+n+' colour"><input type="number" class="dx" min="50" max="50000" step="50" placeholder="DPI" aria-label="Stage '+n+' DPI"><input type="number" class="dy" min="50" max="50000" step="50" placeholder="Y" aria-label="Stage '+n+' Y DPI"><div class="hch"></div></div>')}
rows().forEach((r,i)=>{r.querySelector('.st').onclick=()=>{if(!S)return;S.stage=i;edit.stage=Date.now();paintStages();post('/api/set?stage='+i).then(x=>say(x,'Stage '+(i+1)))};
r.querySelector('input[type=color]').oninput=e=>{dCol=true;paintBtn(i,e.target.value.slice(1));dirty()};
r.querySelector('.dx').oninput=e=>{dDpi=true;if(!$('sepy').checked)r.querySelector('.dy').value=e.target.value;dirty()};
r.querySelector('.dy').oninput=()=>{dDpi=true;dirty()}})}
function paintStages(){if(!S)return;const keep=Date.now()<hold;rows().forEach((r,i)=>{r.classList.toggle('on',S.stage===i);
if(!dCol&&!keep&&S.palette){const c=S.palette[i];r.querySelector('input[type=color]').value='#'+c;paintBtn(i,c)}
if(!dDpi&&!keep&&S.dpi){r.querySelector('.dx').value=S.dpi[i][0]||'';r.querySelector('.dy').value=S.dpi[i][1]||''}});
if(!sepInit&&S.dpi_known){sepInit=true;if(S.dpi.some(d=>d[0]!==d[1])){$('sepy').checked=true;sepChange()}}
const d=S.dpi&&S.stage!=null&&S.dpi[S.stage]?S.dpi[S.stage]:null;
paintHostBtns();$('cur').textContent=S.stage==null?'':'Stage '+(S.stage+1)+(d&&d[0]?' · '+(d[0]===d[1]?d[0]:d[0]+' × '+d[1])+' DPI':'')}
function sepChange(){$('rows').classList.toggle('sep',$('sepy').checked)}
function dirty(){const d=dCol||dDpi;$('stApply').disabled=!d;$('stRevert').disabled=!d}
function revert(){dCol=dDpi=false;hold=0;dirty();paintStages()}
async function applyStages(){if(S&&S.tdpi&&(S.tdpi.on||S.tdpi.restoring)){toast('A calibration has the DPI lowered for its test: finish it first',1);return}const rs=rows();
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
// the values on the drawing too: the angle past the arrow's tip, the position beside the sensor
$('msat').setAttribute('x',(100+68*Math.sin(t)).toFixed(1));$('msat').setAttribute('y',(y-68*Math.cos(t)+4).toFixed(1));$('msat').textContent=a+'°';
$('mspt').setAttribute('x',126);$('mspt').setAttribute('y',(y+4).toFixed(1));$('mspt').textContent='pos '+p;
// ↺ carries no number (it read as the value): the reset value is in its tooltip, and it greys out when already there
for(const[id,v,h,u]of[['ahomeb',a,tu&&tu.angle_home,'°'],['phomeb',p,tu&&tu.pos_home,'']]){const b=$(id);b.disabled=h==null||v===h;b.title=h==null?'':(v===h?'Already at ':'Reset to ')+h+u+' (where the reset chord puts it)';b.setAttribute('aria-label',b.title||'Reset')}
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
// QT Py fw41: what each frame-timing row measures (hover on a computer, tap on a phone)
const PTIP={usb:'Every 1 ms a timer interrupt talks to the mouse over USB (start of frame, then its latest report). This is the time spent in that interrupt; the frame\'s tick is marked when it ends.',
remap:'After the tick, the main loop turns the mouse\'s latest input into the report for the PC or XIM: chords, direction gains (about 11 µs a frame at rest, 40 µs while moving), DPI and sensor logic, and every 20 ms the status for the QT Py.',
chord:'The chord-detection part of the remapping. The direction gains run after it and are not in this row.',
tick:'From the tick to the moment that frame\'s report is built and queued: the remapping plus the short wait before the loop picks the tick up. The PC or XIM then reads it on its own 1 ms schedule (0 to 1 ms later, not shown). Over 1000 µs the next tick would come first and a frame would be skipped.',
bar:'The share of each 1 ms frame the Feather is busy: USB interrupt plus frame finished. Dark: average over the last second; light: the worst frame.'};
function ptHelp(tr){const p=$('pthelp'),s=tr?tr.title:'';p.hidden=!s||p.textContent===s;p.textContent=s}
function drawPerf(pf){const el=$('perf');el.hidden=!pf;$('perfnone').hidden=!!pf;if(!pf)return;const v19=pf.missed_1s!==undefined,f=x=>us(x)+' µs';
const rows=[];if(v19)rows.push(['USB to the mouse','1 ms interrupt',f(pf.usb_avg_us),f(pf.usb_max_us),0,PTIP.usb]);
rows.push(['Remapping','',pf.avg_us.toFixed(1)+' µs',pf.max_us+' µs',0,PTIP.remap]);rows.push(['chord check','',pf.chords_avg_us.toFixed(1)+' µs',pf.chords_max_us+' µs',1,PTIP.chord]);
if(v19)rows.push(['Frame finished','after its tick',f(pf.tick_avg_us),f(pf.tick_max_us),0,PTIP.tick]);
$('ptb').innerHTML=rows.map(r=>'<tr class="tip" title="'+esc(r[5])+'" onclick="ptHelp(this)"><td'+(r[4]?' class="sub"':'')+'>'+(r[4]?'of which ':'')+r[0]+(r[1]?' <span class="mut">'+r[1]+'</span>':'')+'</td><td>'+r[2]+'</td><td>'+r[3]+'</td></tr>').join('');
const a=v19?pf.usb_avg_us+pf.tick_avg_us:pf.avg_us,w=v19?pf.usb_max_us+pf.tick_max_us:pf.max_us,pa=Math.min(100,a/10),pw=Math.min(100,w/10);
$('buda').style.width=pa+'%';$('budw').style.width=pw+'%';
const s='Each 1 ms frame: '+Math.round(pa)+'% used on average, '+Math.round(pw)+'% at worst'+(v19?'':' (remapping only; Feather v19 measures the whole frame)')+'.';$('budt').textContent=s;$('bud').setAttribute('aria-label',s);$('bud').title=PTIP.bar;
const ps=$('pskip');ps.hidden=!v19;if(v19){ps.textContent='Frames skipped: '+(pf.missed_1s>=255?'255+':pf.missed_1s)+' in the last second · '+pf.missed_total+' since the Feather started (plugging in or switching to a device pauses it about half a second).';ps.classList.toggle('bad',pf.missed_1s>0)}}
// recent actions (Feather v21): [age s, source, buttons, action, value], newest first
const EVSRC=['','tap','hold','double tap','wheel up','wheel down'],DEV=['device','Windows','Android','iPhone','Mac','Linux','script','automatic'];
function evText(e){const[age,src,b,a,v]=e;let s,ang,stg=false;
if(src>=1&&src<=5){s=BTN[b>>4]+' + '+(src>=4?'wheel '+(src===4?'up':'down'):BTN[b&15]+' '+EVSRC[src])+': '+ACT[a];ang=a>=8&&a<=10}
else if(src===6){s=b===1?'Page: DPI stage':b===2?'Page: angle':b===3?'Page: position':'Page: command '+b;ang=b===2;stg=b===1}
else{const n=b===1?'Angle':'Position';ang=b===1;s=src===7?n+' assumed (the mouse didn\'t answer)':src===8?n+': the mouse kept its own value (writes didn\'t take)':n+' sent again (the mouse woke)'}
if(v!==127)s+=' → '+(stg?'stage '+(v+1):ang?v+'°':v);return s}
// QT Py fw22: the page edits it keeps in flash (through refreshes, power cycles and updates), newest first:
// [Unix time or 0, age s this boot or -1, kind, device, ip, text]
const EDOPT={poll:['polling rate',' Hz'],lod:['lift-off',' mm'],snap:['Angle Snap'],sync:['Motion Sync'],spdt:['SPDT on']};
function edAt(x){return x[0]?x[0]*1000:x[1]>=0?Date.now()-x[1]*1000:0}
function edText(x){const[,,k,d0,ip,v]=x,d=d0&127,na=d0&128?' (not applied: the Feather never took it)':'';let s;
if(k===1)s='DPI stage → stage '+(Number(v)+1);else if(k===2)s='angle → '+v+'°';else if(k===3)s='position → '+v;
else if(k===4)s='DPI values';else if(k===5)s='stage colours';else if(k===6)s='chord settings (steps, reset values, hold times)';
else if(k===7){const[o,w]=v.split('='),n=EDOPT[o]||[o];s=n[0]+' → '+(o==='snap'||o==='sync'?(w==='1'?'on':'off'):o==='spdt'?['none','left button','right button','both buttons'][w]||w:w+(n[1]||''))}
else if(k===8)s='chords → '+v;else if(k===9)s='chords → defaults';
else if(k===10){const[n,w]=v.split('=');return 'Auto ('+(n||'host')+'): DPI stage → stage '+(Number(w)+1)+na}
else if(k===11){const[n,w]=v.split('=');s=(n||'a host')+' → '+(w==='0'?'leave the stage':'stage '+w)}
else if(k===12)s='direction sensitivity saved (on '+v+')';else if(k===13)s=v==='restored'?'DPI table back after the calibration':v==='dropped'?'calibration DPI: stopped putting the table back':/^unverified/.test(v)?'calibration DPI: gave up putting the table back (the mouse kept reporting '+v.slice(11)+')':'calibration DPI → '+v+' for the test';else if(k===14)s='refused '+v+' from an out-of-date page (reload that tab)';else s='edit '+k;
return (d===7?'QT Py: ':'Page ('+DEV[d]+' '+ip+'): ')+s+na}
// QT Py fw26 + Feather v24: sensitivity per direction (like Corsair's PureVector). Direction k = 0..7 every 45 degrees
// clockwise on screen from "right" (screen y grows downwards, as in the Feather's atan2).
const DGA=['→','↘','↓','↙','←','↖','↑','↗'],DGN=['right','down-right','down','down-left','left','up-left','up','up-right'];
let dgEdit=null,dgDrag=-1,dgKey='',dgBuilt=false,cbLast=null;
function dgR(g){return 30+(g-.5)/1.5*120}
function dgSaved(){return S&&S.dg?S.dg.g.map(v=>v/1000):[1,1,1,1,1,1,1,1]}
// QT Py fw39: where the gains being edited came from, sent with Save: pool:<run times>, run:<time>, page-run (this
// page's last run), flat, hand; ";hand" once edited by hand after loading a suggestion
let dgEditSrc='';
function dgSrcHand(){if(!/(^|;)hand$/.test(dgEditSrc))dgEditSrc=dgEditSrc?dgEditSrc+';hand':'hand'}
function dgCur(){return dgEdit||dgSaved()}
function dgClamp(g){return Math.round(Math.max(.5,Math.min(2,g))*100)/100}
function dgPts(gs){return gs.map((g,k)=>{const a=k*Math.PI/4,r=dgR(g);return (r*Math.cos(a)).toFixed(1)+','+(r*Math.sin(a)).toFixed(1)}).join(' ')}
function dgBuild(){dgBuilt=true;$('dgvals').innerHTML=DGA.map((a,k)=>'<span title="'+DGN[k]+'">'+a+' '+DGN[k]+'</span><input type="number" id="dgv'+k+'" min="0.5" max="2" step="0.01" aria-label="Sensitivity '+DGN[k]+'">').join('');
for(let k=0;k<8;k++)$('dgv'+k).onchange=e=>{const v=parseFloat(e.target.value);if(!(v>0))return;dgEdit=dgCur().slice();dgEdit[k]=dgClamp(v);dgSrcHand();dgKey='';drawDG()};
const sv=$('dgsvg');sv.addEventListener('pointerdown',e=>{const k=e.target.dataset&&e.target.dataset.k;if(k==null)return;dgDrag=+k;sv.setPointerCapture(e.pointerId);e.preventDefault()});
sv.addEventListener('pointermove',e=>{if(dgDrag<0)return;const q=new DOMPoint(e.clientX,e.clientY).matrixTransform(sv.getScreenCTM().inverse()),a=dgDrag*Math.PI/4,d=q.x*Math.cos(a)+q.y*Math.sin(a);
dgEdit=dgCur().slice();dgEdit[dgDrag]=dgClamp(.5+(d-30)/120*1.5);dgSrcHand();dgKey='';drawDG()});
['pointerup','pointercancel'].forEach(t=>sv.addEventListener(t,()=>{dgDrag=-1}))}
function drawDG(){const c=$('dgc');if(!S||!S.dg){c.hidden=true;return}c.hidden=false;if(!dgBuilt)dgBuild();const d=S.dg;
$('dgst').textContent=!d.sup?'needs Feather v24':d.temp?'BallSheet A/B test: temporary gains':d.on?'applying on this host':d.want?(d.g.every(v=>v===1000)?'on here, all 1.00':'sending…'):'off on this host';
$('dgsave').disabled=$('dgrev').disabled=!dgEdit;
const gs=dgCur(),key=JSON.stringify([gs,d.g,d.hosts,S.hosts,cbLast&&cbLast.id]);if(key===dgKey)return;dgKey=key;
let s='';[.5,1,1.5,2].forEach(g=>{s+='<circle class="ring'+(g===1?' one':'')+'" r="'+dgR(g)+'"/>'});
for(let k=0;k<8;k++){const a=k*Math.PI/4;s+='<line class="spoke" x1="0" y1="0" x2="'+(150*Math.cos(a)).toFixed(1)+'" y2="'+(150*Math.sin(a)).toFixed(1)+'"/><text x="'+(166*Math.cos(a)).toFixed(1)+'" y="'+(166*Math.sin(a)).toFixed(1)+'">'+DGA[k]+'</text>'}
if(dgEdit)s+='<polygon class="saved" points="'+dgPts(dgSaved())+'"/>';
if(cbLast&&cbLast.sug&&!dgEdit)s+='<polygon class="sug" points="'+dgPts(cbLast.sug)+'"/>';
s+='<polygon class="poly" points="'+dgPts(gs)+'"/>';
gs.forEach((g,k)=>{const a=k*Math.PI/4,r=dgR(g);s+='<circle class="hd" data-k="'+k+'" r="9" cx="'+(r*Math.cos(a)).toFixed(1)+'" cy="'+(r*Math.sin(a)).toFixed(1)+'"><title>'+DGN[k]+': '+g.toFixed(2)+'</title></circle>'});
$('dgsvg').innerHTML=s;
for(let k=0;k<8;k++){const i=$('dgv'+k);if(document.activeElement!==i)i.value=gs[k].toFixed(2)}
const hs=S.hosts||[];$('dghosts').innerHTML=hs.length?hs.map(x=>{const on=d.hosts.includes(x[0]);return '<button type="button" class="'+(on?'on':'')+'" data-sig="'+x[0]+'" aria-pressed="'+on+'">'+esc(hostName(x))+'</button>'}).join(''):'<span class="mut">no host recognised yet</span>';
$('dghosts').querySelectorAll('button').forEach(b=>b.onclick=()=>{const cur=S.dg.hosts,on=cur.includes(b.dataset.sig),h=on?cur.filter(x=>x!==b.dataset.sig):cur.concat([b.dataset.sig]);S.dg.hosts=h;dgKey='';drawDG();dgPost(dgSaved(),h)})}  // fw33: from the hosts at tap time (two quick taps lost the first)
function dgPost(gs,hosts,src){const g=gs.map(g=>Math.round(g*1000));return post('/api/dirgain?g='+g.join(',')+'&hosts='+hosts.join(',')+(src!=null?'&src='+encodeURIComponent(src):'')).then(r=>{say(r,'Saved');if(r.ok&&S&&S.dg){S.dg.g=g;S.dg.hosts=hosts.slice();if(src!=null){S.dg.src=src;S.dg.src_t=Math.floor(Date.now()/1000)}}dgKey='';if(r.ok&&src!=null)cbDrawRuns();return r})}  // fw35: what was saved is the state until the next poll (a quick second change used the old one)
function dgSave(){if(!dgEdit||!S||!S.dg)return;dgPost(dgEdit,S.dg.hosts,dgEditSrc||'hand').then(r=>{if(r.ok){dgEdit=null;dgEditSrc=''}})}
function dgRevert(){dgEdit=null;dgEditSrc='';dgKey='';drawDG()}
function dgFlat(){dgEdit=[1,1,1,1,1,1,1,1];dgEditSrc='flat';dgKey='';drawDG()}

// ---- flick calibration: full screen with the pointer locked. A home target in the middle and an outer one at a fixed
// distance in each of the 8 directions (shuffled, once per round): click home, flick to the outer target and click it,
// flick back and click home (that flick, the opposite direction, counts too), and so on.
// A flick's first ballistic movement ends where its speed along the target line first drops below 10% of its peak (or
// the samples pause, or it turns back); how far along the line that is, over the distance to the target, is its reach
// (1 = on target). Reaches per direction (median) give the suggestion: gain / reach, normalised so the geometric mean
// change is 1 (overall sensitivity stays), damped (^0.8) and capped at +-25% per run. Measured with whatever gains this
// host applies now, so a second run refines the first.
// ---- flick calibration. Full screen with Windows' own cursor (a hardware cursor, as smooth as on the desktop; a cursor
// drawn by the page lagged a frame or more), no pointer lock. With pointer speed 6/11 and no Enhance pointer precision
// the cursor moves 1:1 with the mouse's counts, so a flick's length on the pad is its length on screen over the DPI.
// fw28: flicks at the lengths given in cm (the size of flicks in game). So the longest one spans a third of the screen,
// the QT Py lowers the current stage's DPI for the test (only ever lowered, not below 50) and puts it back afterwards,
// also if this page closes. Rounds cycle through the lengths; each round is the 8 directions shuffled: click home, flick
// to the target and click it, flick back and click home (that flick, the opposite direction, counts too).
// A flick's first ballistic movement ends where its speed along the target line first drops below 10% of its peak (or
// the samples pause, or it turns back); how far along the line that is, over the distance to the target, is its reach
// (1 = on target). Reaches per direction (median, all lengths) give the suggestion: gain / reach, normalised so the
// geometric mean change is 1 (overall sensitivity stays), damped (^0.8) and capped at +-25% per run. Measured with the
// gains this host applies now, so a second run refines the first.
const CBHR=22,CBTR=24;let cb=null,cbRestoreP=Promise.resolve(),cbEatMenu=0;  // fw35: a stopped run's DPI restore goes out before the next run's lowering; the right-click that stops a run opens no menu
function cbMsg(m){const e=$('cbmsg');e.textContent=m||'';e.hidden=!m}
function cbLens(){const v=String($('cblen').value||'').split(/[\s,;]+/).map(Number).filter(x=>x>=1&&x<=40);if(!v.length)return[10];const u=[...new Set(v)].sort((a,b)=>a-b);return u.length>4?u.slice(0,3).concat(u.slice(-1)):u}  // fw33: the longest sets the test DPI: keep it
function cbRpl(){const v=parseInt($('cbrpl').value,10);return v>=1&&v<=8?v:3}
function cbPlan(lens){const short=Math.min(screen.width,screen.height),dpr=devicePixelRatio||1,cur=S.dpi[S.stage][0],Lmax=lens[lens.length-1];
let dpi=Math.round(short/3*dpr/(Lmax/2.54));if(dpi>cur)dpi=cur;if(dpi<50)dpi=50;return{dpi,cur,temp:dpi!==cur,short,Ds:lens.map(L=>L/2.54*dpi/dpr)}}
function cbBegin(){if(cb)return;cbMsg('');if(!S||!S.dg||!S.dg.sup){cbMsg('Needs Feather firmware v24.');return}
if(!S.dpi_known||S.stage==null||!S.dpi||!S.dpi[S.stage]||!S.dpi[S.stage][0]){cbMsg('The page doesn\'t know the mouse\'s DPI yet: move the mouse, then press Read from mouse.');return}
if(S.tdpi&&(S.tdpi.restoring||S.tdpi.on)){cbMsg('The last test\'s DPI is still being put back: try again in a few seconds.');return}
const lens=cbLens(),plan=cbPlan(lens);try{localStorage.setItem('cblen',$('cblen').value);localStorage.setItem('cbrpl',$('cbrpl').value)}catch(e){}
if(plan.Ds[plan.Ds.length-1]>.42*plan.short){cbMsg('A '+lens[lens.length-1]+' cm flick doesn\'t fit on this screen even at 50 DPI.');return}
if(plan.Ds[0]<80){cbMsg('A '+lens[0]+' cm flick would be only '+Math.round(plan.Ds[0])+' pixels next to '+lens[lens.length-1]+' cm ones on this screen, too short to measure: use lengths closer together.');return}  // fw33
const w=$('cbwrap');cb={lens,Ds:plan.Ds,dpi:plan.dpi,cur:plan.cur,temp:plan.temp,rounds:cbRpl()*lens.length,round:0,li:0,order:[],i:0,phase:'wait',p:{x:innerWidth/2,y:innerHeight/2},fl:null,data:[],skip:0,dts:[],dte:[],why:{},traces:[],
g:S.dg.on?S.dg.g.map(v=>v/1000):[1,1,1,1,1,1,1,1],gOff:!S.dg.on&&!S.dg.g.every(v=>v===1000),raw:false,geo:null,fs:false,keep:0,tp:null};const me=cb;cbLoadRuns();
w.classList.add('on');if(w.requestFullscreen)w.requestFullscreen().then(()=>{if(cb===me){cb.fs=true;cbGeoReset()}else if(document.fullscreenElement)document.exitFullscreen().catch(()=>{})}).catch(()=>{});cbDraw();  // fullscreen only inside the click
const start=()=>{if(cb!==me)return;cbRound();cb.phase='home';cbDraw()};if(!plan.temp){start();return}
me.tp=cbRestoreP.then(()=>post('/api/tempdpi?dpi='+plan.dpi)).then(r=>{if(cb!==me)return;if(!r.ok){cbStop(false);cbMsg('Couldn\'t lower the DPI for the test: '+(r.err||'no answer'));return}
me.keep=setInterval(()=>{if(cb!==me){clearInterval(me.keep);return}post('/api/tempdpi?keep=1').then(r=>{if(cb===me&&r&&!r.ok&&r.err!=='no answer'){cbStop(me.data.length>=8);cbMsg('The QT Py put the DPI back during the test, so the run stopped.')}})},3000);const t0=Date.now(),wait=()=>{if(cb!==me)return;
const set=S.tdpi&&S.tdpi.on&&S.tdpi.dpi||plan.dpi;if(S.dpi&&S.stage!=null&&S.dpi[S.stage]&&S.dpi[S.stage][0]===set){if(set!==me.dpi){me.Ds=me.Ds.map(D=>D*set/me.dpi);me.dpi=set;if(me.Ds[me.Ds.length-1]>.42*plan.short){cbStop(false);cbMsg('The QT Py set '+set+' DPI (it keeps Y at 50 or more), and the longest flick no longer fits: use shorter lengths.');return}}start();return}  // fw33
if(Date.now()-t0>6000){cbStop(false);cbMsg('The mouse didn\'t take the test DPI (asleep?): move it and try again.');return}setTimeout(wait,250)};wait()})}
function cbRound(){cb.round++;cb.li=(cb.round-1)%cb.lens.length;cb.i=0;const o=[0,1,2,3,4,5,6,7];for(let i=7;i>0;i--){const j=Math.floor(Math.random()*(i+1));[o[i],o[j]]=[o[j],o[i]]}cb.order=o}
function cbGeo(){if(cb&&cb.geo)return cb.geo;const W=$('cbc').clientWidth,H=$('cbc').clientHeight,g={W,H,cx:W/2,cy:H/2};if(cb)cb.geo=g;return g}
function cbGeoReset(){if(cb){cb.geo=null;cbDraw()}}
// the cursor's own positions with their times. fw30: Firefox stamps mouse events with Windows' message clock, in ~16 ms
// steps (measured 10-21: 57% of samples shared a stamp, every other gap was 16 ms or more), so each sample is timed by
// the page's own clock as it arrives instead; a browser that hands over its in-between samples (getCoalescedEvents,
// https or Chrome) has them stamped properly, and those keep their own times. Both clocks go into the diagnostics.
function cbMove(e){if(!cb)return;const now=performance.now(),evs=e.getCoalescedEvents?e.getCoalescedEvents():[],many=evs.length>1;for(const q of (many?evs:[e])){const tq=many?q.timeStamp:now;cb.p.x=q.clientX;cb.p.y=q.clientY;
if(cb.fl){const s=cb.fl.s;if(s.length){cb.dts.push(tq-s[s.length-1][0]);cb.dte.push(q.timeStamp-cb.fl.ev)}cb.fl.ev=q.timeStamp;s.push([tq,q.clientX,q.clientY])}}}
function cbFlick(kind,k,T,e){const now=performance.now();cb.fl={kind,k,L:cb.li,start:{x:cb.p.x,y:cb.p.y},T,t0:now,ev:e.timeStamp,s:[[now,cb.p.x,cb.p.y]]}}
function cbEnd(){if(!cb.fl)return;const r=cbAnalyze(cb.fl);if(r&&!r.why){r.L=cb.fl.L;cb.data.push(r)}else{cb.skip++;const w=r?r.why:'?';cb.why[w]=(cb.why[w]||0)+1;if(cb.traces.length<6)cb.traces.push(cbTrace(cb.fl,w))}cb.fl=null}
function cbDown(e){if(!cb)return;e.preventDefault();if(e.button===2){cbEatMenu=Date.now();cbStop(cb.data.length>=8);return}if(e.button!==0||cb.phase==='wait')return;cb.p.x=e.clientX;cb.p.y=e.clientY;const g=cbGeo();
if(cb.phase==='home'){if(Math.hypot(cb.p.x-g.cx,cb.p.y-g.cy)>CBHR)return;
if(S&&S.dpi&&S.stage!=null&&S.dpi[S.stage]&&S.dpi[S.stage][0]!==cb.dpi){const m=cb;cbStop(m.data.length>=8);cbMsg('The DPI changed during the test ('+(S.dpi[S.stage][0])+' instead of '+m.dpi+'), so the run stopped.');return}
cbEnd();
if(cb.i>=8){if(cb.round>=cb.rounds){cbStop(true);return}cbRound()}
const k=cb.order[cb.i++],a=k*Math.PI/4,D=cb.Ds[cb.li];cbFlick('out',k,{x:g.cx+D*Math.cos(a),y:g.cy+D*Math.sin(a)},e);cb.phase='out'}
else{const T=cb.fl&&cb.fl.T;if(!T||Math.hypot(cb.p.x-T.x,cb.p.y-T.y)>CBTR)return;const k=cb.fl.k;cbEnd();cbFlick('back',(k+4)%8,{x:g.cx,y:g.cy},e);cb.phase='home'}
cbDraw()}
function cbAnalyze(fl){const sx=fl.start.x,sy=fl.start.y,vx=fl.T.x-sx,vy=fl.T.y-sy,D=Math.hypot(vx,vy);if(D<60)return{why:'target too close'};const ux=vx/D,uy=vy/D;
// fw29: samples sharing a timestamp (a browser batching them) count once, at their last position
// fw33: and samples that arrive within 2 ms of a group's first (a burst after a page stall, stamped on arrival) join it
const s=[];for(const q of fl.s){if(s.length&&q[0]-s[s.length-1][0]<2)s[s.length-1]=[s[s.length-1][0],q[1],q[2]];else s.push(q)}
if(s.length<6)return{why:'too few movement samples'};
const a=s.map(q=>(q[1]-sx)*ux+(q[2]-sy)*uy),t=s.map(q=>q[0]),v=[];let j=0;
for(let i=0;i<s.length;i++){while(j<i-1&&t[i]-t[j+1]>=8)j++;v.push(i>j&&t[i]>t[j]?(a[i]-a[j])/(t[i]-t[j]):0)}
// a pause long enough to mean "stopped": 30 ms, or 3x this browser's usual spacing if it is sparser (fw29; was 12 ms)
const dts=[];for(let i=1;i<s.length;i++)dts.push(t[i]-t[i-1]);const gap=Math.max(30,3*cbMed(dts));
// the first movement only, anchored where it has covered 30% of the way (holding still or jitter before it can't start
// it): its start is the last point before that slower than 10% of the speed there; it ends at a pause, or where its
// speed falls under 10% of its running peak (a quicker correction afterwards can't pass for the flick's peak)
let i30=-1;for(let i=1;i<s.length;i++)if(a[i]>=.3*D){i30=i;break}if(i30<0)return{why:'never got 30% of the way'};if(v[i30]<.3)return{why:'too slow at 30% of the way'};
let on=i30;while(on>1&&v[on-1]>=.1*v[i30]&&t[on]-t[on-1]<=gap)on--;
let vmax=0,end=s.length-1;for(let i=on;i<s.length;i++){if(i>on&&t[i]-t[i-1]>gap){end=i-1;break}if(v[i]>vmax)vmax=v[i];if(i>i30&&v[i]<.1*vmax){end=i;break}}
const reach=a[end]/D,lat=((s[end][1]-sx)*-uy+(s[end][2]-sy)*ux)/D,rt=t[on]-fl.t0;
// only an outward flick can be too early (its direction isn't known until the target shows); home never moves
if(fl.kind==='out'&&rt<60)return{why:'moving before the target appeared'};
if(reach<.4)return{why:'stopped before 40% of the way'};if(reach>1.8)return{why:'went past 180%'};if(Math.abs(lat)>.5)return{why:'too far off the line'};
return{k:fl.k,kind:fl.kind,reach,lat,rt,vmax}}
// fw29: a skipped flick's along-line progress (% of the way) against ms since the click, 40 points at most, for diagnosis
function cbTrace(fl,why){const sx=fl.start.x,sy=fl.start.y,vx=fl.T.x-sx,vy=fl.T.y-sy,D=Math.hypot(vx,vy)||1,ux=vx/D,uy=vy/D,pts=fl.s,st=Math.max(1,Math.ceil(pts.length/40)),o=[];
for(let i=0;i<pts.length;i+=st){const q=pts[i];o.push([Math.round(q[0]-fl.t0),Math.round(((q[1]-sx)*ux+(q[2]-sy)*uy)/D*100)])}return{kind:fl.kind,k:fl.k,why,n:pts.length,D:Math.round(D),o}}
// drawn only when a target changes (the cursor is Windows' own, so nothing needs redrawing while you move)
function cbDraw(){if(!cb)return;const c=$('cbc'),x=c.getContext('2d'),dpr=devicePixelRatio||1,g=cbGeo();
if(c.width!==Math.round(g.W*dpr)||c.height!==Math.round(g.H*dpr)){c.width=Math.round(g.W*dpr);c.height=Math.round(g.H*dpr)}x.setTransform(dpr,0,0,dpr,0,0);x.clearRect(0,0,g.W,g.H);
cb.Ds.forEach((D,li)=>{x.strokeStyle=li===cb.li&&cb.phase!=='wait'?'#333':'#1a1a1a';x.beginPath();x.arc(g.cx,g.cy,D,0,7);x.stroke()});
const D=cb.Ds[cb.li];x.strokeStyle='#333';for(let k=0;k<8;k++){const a=k*Math.PI/4;x.beginPath();x.moveTo(g.cx+(D-8)*Math.cos(a),g.cy+(D-8)*Math.sin(a));x.lineTo(g.cx+(D+8)*Math.cos(a),g.cy+(D+8)*Math.sin(a));x.stroke()}
if(cb.phase!=='wait'){x.fillStyle=cb.phase==='home'?'#2f7de1':'#1a2a40';x.beginPath();x.arc(g.cx,g.cy,CBHR,0,7);x.fill()}
if(cb.phase==='out'&&cb.fl){x.fillStyle='#e8553e';x.beginPath();x.arc(cb.fl.T.x,cb.fl.T.y,CBTR,0,7);x.fill()}
const dp=cb.temp?' · DPI '+cb.dpi+' for the test (back to '+cb.cur+' after)':'';
$('cbhint').textContent=cb.phase==='wait'?'Setting the DPI to '+cb.dpi+' for the test…':'Round '+cb.round+' of '+cb.rounds+' · '+cb.lens[cb.li]+' cm flicks · target '+cb.i+' of 8 · '+cb.data.length+' measured'+dp+' · right-click or Esc to finish'}
function cbStop(done){if(!cb)return;const m=cb;cb=null;if(m.keep)clearInterval(m.keep);if(m.temp)cbRestoreP=(m.tp||Promise.resolve()).then(()=>post('/api/tempdpi?restore=1'));if(S)render();
if(document.fullscreenElement)document.exitFullscreen().catch(()=>{});$('cbwrap').classList.remove('on');if(done)cbFinish(m)}
function cbMed(a){const b=a.slice().sort((p,q)=>p-q),n=b.length;return n?(n%2?b[(n-1)/2]:(b[n/2-1]+b[n/2])/2):NaN}
function cbFinish(m){const per=[],perL=[];for(let k=0;k<8;k++){const r=m.data.filter(d=>d.k===k).map(d=>d.reach);per.push({n:r.length,med:cbMed(r)});
perL.push(m.lens.map((L,li)=>{const q=m.data.filter(d=>d.k===k&&d.L===li).map(d=>d.reach);return{n:q.length,med:cbMed(q)}}))}
const all=m.data.map(d=>d.reach),pf=all.filter(r=>Math.abs(r-1)<=.05).length,un=all.filter(r=>r<.95).length,ov=all.filter(r=>r>1.05).length;
// sample spacing: a browser that rounds its clock coarsely (resistFingerprinting) hides where a flick stops
const dts=m.dts.filter(x=>x>0).sort((p,q)=>p-q),dtm=dts.length?dts[Math.floor(dts.length/2)]:0,coarse=dtm>=8;
const ok=per.map(p=>p.n>=3&&p.med>0),c=per.map((p,k)=>ok[k]?1/p.med:1),okc=c.filter((v,k)=>ok[k]);
const gm=okc.length?Math.exp(okc.reduce((s,v)=>s+Math.log(v),0)/okc.length):1;
const sug=m.g.map((g,k)=>ok[k]?dgClamp(g*Math.max(.8,Math.min(1.25,Math.pow(c[k]/gm,.8)))):g);

// QT Py fw27: keep the run's summary there (the last 6), and compare with the run before it
if(all.length>=16){cbLast={id:Date.now(),per,perL,lens:m.lens,sug,g:m.g,n:all.length,skip:m.skip,coarse,raw:m.raw};const prev=cbRuns[0]||null;post('/api/calib?n='+per.map(p=>p.n).join(',')+'&med='+per.map(p=>p.n>=3?Math.round(p.med*1000):0).join(',')+'&g='+m.g.map(g=>Math.round(g*1000)).join(',')+'&f='+((m.raw?1:0)|(coarse?2:0))+'&s='+m.skip).then(()=>cbLoadRuns());cbLast.prev=prev}
const pct=v=>Math.round(v*100),rd=v=>pct(Math.abs(v-1))===0?'on target':v<1?pct(1-v)+'% short':pct(v-1)+'% over';let h='<h3 class="sub">Calibration result</h3>';
// fw29: why flicks were skipped and how this browser delivered the movement (also kept on the QT Py: GET /api/calibdiag)
const dz=m.dts.filter(x=>x<=0).length,dp=m.dts.filter(x=>x>0).sort((p,q)=>p-q),dmed=dp.length?dp[Math.floor(dp.length/2)]:0,dp95=dp.length?dp[Math.floor(dp.length*.95)]:0,d12=dp.filter(x=>x>12).length;
const sub=typeof PointerEvent!=='undefined'&&'getCoalescedEvents' in PointerEvent.prototype;const ez=m.dte.filter(x=>x<=0).length,ep=m.dte.filter(x=>x>0).sort((p,q)=>p-q),emed=ep.length?ep[Math.floor(ep.length/2)]:0;
const diag={t:Math.floor(Date.now()/1000),qt_fw:S&&S.qt_fw,dpi:m.dpi,lens:m.lens,measured:all.length,out:m.data.filter(d=>d.kind==='out').length,back:m.data.filter(d=>d.kind==='back').length,skipped:m.skip,why:m.why,
dt:{n:m.dts.length,median:+dmed.toFixed(2),p95:+dp95.toFixed(2),same_pct:m.dts.length?Math.round(dz/m.dts.length*100):0,over12_pct:dp.length?Math.round(d12/dp.length*100):0},ev_clock:{median:+emed.toFixed(2),same_pct:m.dte.length?Math.round(ez/m.dte.length*100):0},subframe:sub,secure:!!window.isSecureContext,ua:navigator.userAgent.slice(0,90),traces:m.traces};
let dj=JSON.stringify(diag);while(dj.length>4000&&diag.traces.length){diag.traces.pop();dj=JSON.stringify(diag)}if(all.length+m.skip>=16)post('/api/calibdiag',dj);  // fw33: fits the QT Py's 4000; a run cut short keeps the last diagnostics
const whyTxt=Object.entries(m.why).sort((p,q)=>q[1]-p[1]).map(([w,c])=>c+' '+w).join(' · ');
const diagH='<p class="mut">Skipped: '+(whyTxt||'none')+'.<br>Movement samples: every '+dmed.toFixed(1)+' ms usually ('+dp95.toFixed(1)+' ms at the 95th percentile), '+diag.dt.over12_pct+'% more than 12 ms apart, '+diag.dt.same_pct+'% sharing a timestamp (the browser\'s own event clock: '+emed.toFixed(1)+' ms, '+diag.ev_clock.same_pct+'% shared); '+(sub?'with':'without')+' the browser\'s in-between samples.</p>';
if(all.length<16){const note='Only '+all.length+' flicks could be measured ('+m.skip+' skipped). Do at least two rounds.';if(cbLast){cbMsg(note+' The last full result below is unchanged.');return}h+='<p class="note">'+note+'</p>'+diagH;$('cbres').innerHTML=h;$('cbres').hidden=false;dgKey='';drawDG();return}
h+='<p class="mut">'+all.length+' flicks of '+m.lens.join(', ')+' cm'+(m.temp?' (at '+m.dpi+' DPI for the test)':'')+', '+m.skip+' skipped (too early, off line or a misclick). On target (within 5%): '+pct(pf/all.length)+'% · short: '+pct(un/all.length)+'% · over: '+pct(ov/all.length)+'%.</p>';
if(m.lens.length>1)h+='<p class="mut">Overall by length: '+m.lens.map((L,li)=>{const q=m.data.filter(d=>d.L===li).map(d=>d.reach);return L+' cm '+(q.length?rd(cbMed(q)):'–')}).join(' · ')+'. Only how the directions compare goes into the suggestion.</p>';
h+=diagH;if(m.gOff)h+='<p class="note">The direction gains are off on this host, so this run measured the mouse without them and the suggestion starts from 1.00, not from your saved table. To refine the saved table, light this host under On for, then measure again.</p>';if(coarse)h+='<p class="note">Mouse movement arrived only every '+dtm.toFixed(0)+' ms, too coarse to find exactly where flicks stop. In Firefox, set dom.events.coalesce.mousemove to false in about:config (it hands over every movement instead of one per frame), then measure again. Treat the numbers below with care.</p>';
const pv=cbLast.prev,ml=m.lens.length>1;
h+='<table class="cbt"><tr><th>Direction</th><th>Flicks</th>'+(ml?m.lens.map(L=>'<th>'+L+' cm</th>').join(''):'')+'<th>'+(ml?'All':'Reach')+'</th>'+(pv?'<th>Last run</th>':'')+'<th>Now</th><th>Suggested</th></tr>'+per.map((p,k)=>'<tr><td>'+DGA[k]+' '+DGN[k]+'</td><td>'+p.n+'</td>'+(ml?perL[k].map(q=>'<td>'+(q.n>=2?rd(q.med):'–')+'</td>').join(''):'')+'<td>'+(ok[k]?rd(p.med):'–')+'</td>'+(pv?'<td>'+(pv.med[k]?rd(pv.med[k]/1000):'–')+'</td>':'')+'<td>'+m.g[k].toFixed(2)+'</td><td>'+sug[k].toFixed(2)+'</td></tr>').join('')+'</table>';
if(pv)h+='<p class="mut">Last run: '+(pv.t?whenText(pv.t*1000):'time unknown')+(pv.g.some((g,k)=>g!==Math.round(m.g[k]*1000))?', measured with different gains, so its reach isn\'t directly comparable':'')+'.</p>';
h+='<p class="mut">Reach is the median of where each flick\'s first movement stopped. The suggestion keeps your overall sensitivity: only how the directions compare changes. The green dashed shape above shows it.</p>';
h+='<div class="row"><button class="p" onclick="cbUse()">Use suggested</button><button onclick="cbBegin()">Measure again</button><button onclick="cbDiscard()">Discard</button></div>';
$('cbres').innerHTML=h;$('cbres').hidden=false;dgKey='';drawDG()}
try{const a=localStorage.getItem('cblen'),b=localStorage.getItem('cbrpl');if(a)$('cblen').value=a;if(b)$('cbrpl').value=b}catch(e){}
// QT Py fw31: the runs kept on the QT Py (this page's and BallSheet's: POST /api/calib, the last 6), each with its own
// "Use suggested": the suggestion is worked out again from what is kept (median reach and the gains in use per
// direction), the same way as for a fresh run
let cbRuns=[],cbRunsAt=0;
function cbSug(r){const ok=r.n.map((n,k)=>n>=3&&r.med[k]>0),c=r.med.map((m,k)=>ok[k]?1000/m:1),okc=c.filter((v,k)=>ok[k]);
const gm=okc.length?Math.exp(okc.reduce((s,v)=>s+Math.log(v),0)/okc.length):1;
return r.g.map((g,k)=>{const gg=g/1000;return ok[k]?dgClamp(gg*Math.max(.8,Math.min(1.25,Math.pow(c[k]/gm,.8)))):gg})}
// QT Py fw36: a suggestion pooled from several saved runs. Each run is corrected for the gains it was measured with (the
// hand's own reach = reach / gain), the runs are averaged in log space weighted by flicks, and the result is scaled so
// the overall sensitivity stays (geometric mean of the gains in use). Comparable by default: same tool (BallSheet or this
// page), not coarse, 16+ flicks, and for BallSheet a known speed within 5 points of the newest run's. Tick boxes override.
function cbComparable(r,ref){if(!r||!ref||(r.flags&2)||(r.flags&1)!==(ref.flags&1)||r.n.reduce((a,b)=>a+b,0)<16)return false;return !(r.flags&1)||(r.speed>0&&ref.speed>0&&Math.abs(r.speed-ref.speed)<=5)}
let cbPoolSel=null;  // run times ticked for pooling; null = default: the runs the gains in use were pooled from, else the comparable ones
// QT Py fw39: the runs the gains in use came from (dg.src "pool:t,t" / "run:t"), and a text saying so
function dgSrcRuns(){const s=(S&&S.dg&&S.dg.src||'').replace(/;hand$/,''),m=s.match(/^(pool|run):(.*)$/);return m?m[2].split(',').map(Number).filter(Boolean):[]}
function cbShort(t){const d=new Date(t*1000),tm=d.toLocaleTimeString([],{hour:'numeric',minute:'2-digit'});return d.toDateString()===new Date().toDateString()?tm:d.toLocaleDateString(undefined,{month:'short',day:'numeric'})+' '+tm}
function cbRunLabel(t){const r=cbRuns.find(r=>r.t===t);return cbShort(t)+(r?(r.speed?' '+r.speed+'%':''):' (deleted)')}
function dgSrcText(){const d=S&&S.dg;if(!d)return '';const src=d.src||'',hand=/;hand$/.test(src),s=src.replace(/;hand$/,''),m=s.match(/^(pool|run):(.*)$/);let x;
if(m&&m[1]==='pool'){const ts=m[2].split(',').map(Number).filter(Boolean);x='pooled from '+ts.length+' run'+(ts.length>1?'s':'')+' ('+ts.map(cbRunLabel).join(', ')+')'}
else if(m)x='from the run at '+cbRunLabel(+m[2]);else if(s==='page-run')x='from a run on this page';else if(s==='bs-run')x='from a BallSheet run that wasn\'t saved';
else if(s==='flat')x='all 1.00';else if(s==='hand')x='set by hand';else x='source not recorded (saved before QT Py firmware 39)';
if(hand&&s!=='hand')x+=', then edited by hand';return 'Gains in use: '+x+(d.src_t?' · saved '+whenText(d.src_t*1000):'')}
function cbPoolRuns(){if(!cbRuns.length)return[];if(cbPoolSel)return cbRuns.filter(r=>cbPoolSel.includes(r.t));const use=dgSrcRuns();
if(use.length&&/^pool:/.test((S&&S.dg&&S.dg.src)||'')){const u=cbRuns.filter(r=>use.includes(r.t));if(u.length)return u}return cbFitPool()}
// a run's pattern: per direction, the gain it needed (gain used / reach) relative to that run's average, in log space;
// runs measured with different gains or at different overall levels compare directly
function cbPat(r){const l=r.med.map((m,k)=>r.n[k]>=3&&m>0&&r.g[k]>0?Math.log(r.g[k]/m):null),v=l.filter(x=>x!==null),mu=v.reduce((a,b)=>a+b,0)/(v.length||1);return l.map(x=>x===null?null:x-mu)}
// how far a run's pattern is from the mean of other runs' (RMS over directions, %); null without others
function cbFit(r,others){const o=others.filter(x=>x!==r);if(!o.length)return null;const p=cbPat(r),ps=o.map(cbPat);let s=0,n=0;
for(let k=0;k<8;k++){const v=ps.map(q=>q[k]).filter(x=>x!==null);if(p[k]===null||!v.length)continue;const m=v.reduce((a,b)=>a+b,0)/v.length;s+=(p[k]-m)**2;n++}return n?(Math.exp(Math.sqrt(s/n))-1)*100:null}
// default pool: the BallSheet runs that agree with each other (drop the worst while it is over 3.5% from the rest)
const CB_FIT=3.5;
function cbFitPool(){let rs=cbRuns.filter(r=>(r.flags&1)&&!(r.flags&2)&&r.n.reduce((a,b)=>a+b,0)>=16);
while(rs.length>2){let w=null,wf=0;rs.forEach(r=>{const f=cbFit(r,rs);if(f!==null&&f>wf){wf=f;w=r}});if(wf<=CB_FIT)break;rs=rs.filter(r=>r!==w)}
return rs.length?rs:cbRuns.filter(r=>cbComparable(r,cbRuns[0]))}
let cbView='need';  // 'need': the needed change per direction (comparable across runs) | 'raw': reach as measured
function cbPooled(runs){const cur=dgSaved(),lh=[0,0,0,0,0,0,0,0],w=[0,0,0,0,0,0,0,0];
runs.forEach(r=>{for(let k=0;k<8;k++)if(r.n[k]>=3&&r.med[k]>0&&r.g[k]>0){lh[k]+=r.n[k]*Math.log(r.med[k]/r.g[k]);w[k]+=r.n[k]}});
const ok=w.map(x=>x>0),m=ok.filter(Boolean).length;if(!m)return cur.slice();
const ideal=lh.map((s,k)=>ok[k]?Math.exp(-s/w[k]):cur[k]),lg=a=>a.reduce((s,v,k)=>ok[k]?s+Math.log(v):s,0),sc=Math.exp((lg(cur)-lg(ideal))/m);
return ideal.map((v,k)=>ok[k]?dgClamp(Math.max(cur[k]*.8,Math.min(cur[k]*1.25,v*sc))):cur[k])}
function cbPoolTick(t,on){if(!cbPoolSel)cbPoolSel=cbPoolRuns().map(r=>r.t);cbPoolSel=on?cbPoolSel.concat([t]):cbPoolSel.filter(x=>x!==t);cbDrawRuns()}
function cbPoolSet(kind){const ref=cbRuns[0],use=dgSrcRuns(),fit=kind==='fit'?cbFitPool():[];cbPoolSel=cbRuns.filter(r=>kind==='speed'?cbComparable(r,ref):kind==='bs'?(r.flags&1)&&!(r.flags&2):kind==='use'?use.includes(r.t):kind==='fit'?fit.includes(r):false).map(r=>r.t);cbDrawRuns()}
function cbUsePooled(){const rs=cbPoolRuns();if(!rs.length)return;dgEdit=cbPooled(rs);dgEditSrc='pool:'+rs.map(r=>r.t).join(',');dgKey='';drawDG();toast('Pooled from '+rs.length+' run'+(rs.length>1?'s':'')+': press Save to keep it')}
// how much the ticked runs disagree per direction: each run's gain-corrected ideal, its overall level taken out, sample SD in %
function cbSpread(runs){if(runs.length<2)return null;const lr=runs.map(r=>{const l=r.med.map((m,k)=>r.n[k]>=3&&m>0&&r.g[k]>0?Math.log(r.g[k]/m):null),v=l.filter(x=>x!==null),mean=v.reduce((a,b)=>a+b,0)/(v.length||1);return l.map(x=>x===null?null:x-mean)});
return [0,1,2,3,4,5,6,7].map(k=>{const v=lr.map(l=>l[k]).filter(x=>x!==null);if(v.length<2)return null;const m=v.reduce((a,b)=>a+b,0)/v.length;return (Math.exp(Math.sqrt(v.reduce((a,b)=>a+(b-m)*(b-m),0)/(v.length-1)))-1)*100})}
let cbDelArm=0,cbDelAt=0;
function cbDel(t){if(cbDelArm!==t||Date.now()-cbDelAt>4000){cbDelArm=t;cbDelAt=Date.now();cbDrawRuns();setTimeout(()=>{if(cbDelArm===t){cbDelArm=0;cbDrawRuns()}},4100);return}
cbDelArm=0;post('/api/calibdel?run='+t).then(r=>{say(r,'Run deleted');if(cbPoolSel)cbPoolSel=cbPoolSel.filter(x=>x!==t);cbLoadRuns()})}
function cbLoadRuns(){cbRunsAt=Date.now();fetchJ(tq('/api/calib')).then(j=>{cbRuns=j.runs||[];if(!cb)cbDrawRuns()}).catch(()=>{})}
// QT Py fw39: the saved runs as ONE table: a row per direction; the gains in use, the pooled suggestion (and its change),
// how much the ticked runs disagree, then a column per run with where its flicks stopped (- short, + over)
function cbDrawRuns(){const box=$('cbsaved');box.hidden=!cbRuns.length;if(!cbRuns.length)return;$('cbsavedn').textContent='('+cbRuns.length+')';
const pr=cbPoolRuns(),ps=pr.length?cbPooled(pr):null,cur=dgSaved(),use=dgSrcRuns(),sp=cbSpread(pr),ref=cbRuns[0];
const pats=new Map(cbRuns.map(r=>[r,cbPat(r)]));
const cell=(v,r,k)=>{if(cbView==='need'){const x=pats.get(r)[k],p=(Math.exp(x)-1)*100,a=Math.min(1,Math.abs(p)/8),q=Math.round(p),txt=q===0?'0':(q>0?'+':'\u2212')+Math.abs(q);
return '<td class="rc" style="background:rgba('+(p>0?'240,182,90':'90,156,245')+','+(a*.38).toFixed(2)+')" title="'+(Math.abs(p)<.5?'about average':'needed '+Math.abs(p).toFixed(1)+'% '+(p>0?'more':'less')+' gain than this run\'s average')+' · measured: '+(Math.abs((v-1)*100)<.5?'on target':Math.abs((v-1)*100).toFixed(1)+'% '+(v<1?'short':'over'))+' with gain '+(r.g[k]/1000).toFixed(2)+' · '+r.n[k]+' flicks">'+txt+'</td>'}
const p=(v-1)*100,a=Math.min(1,Math.abs(p)/8),q=Math.round(p),txt=q===0?'0':(q>0?'+':'\u2212')+Math.abs(q);
return '<td class="rc" style="background:rgba('+(p<0?'240,182,90':'90,156,245')+','+(a*.38).toFixed(2)+')" title="'+(Math.abs(p)<.5?'on target':Math.abs(p).toFixed(1)+'% '+(p<0?'short':'over'))+' · '+r.n[k]+' flicks · gain then '+(r.g[k]/1000).toFixed(2)+'">'+txt+'</td>'};
$('cbsrc').textContent=dgSrcText();let h='';
h+='<div class="row" style="margin:0 0 8px"><span class="mut">Tick:</span><button class="chipb" onclick="cbPoolSet(\'speed\')">same speed as newest (±5)</button><button class="chipb" onclick="cbPoolSet(\'bs\')">all BallSheet</button>'+(use.length?'<button class="chipb" onclick="cbPoolSet(\'use\')">runs in use</button>':'')+'<button class="chipb" onclick="cbPoolSet(\'fit\')">runs that agree</button><button class="chipb" onclick="cbPoolSet(\'none\')">none</button><span class="mut" style="margin-left:8px">Show:</span><button class="chipb'+(cbView==='need'?' on':'')+'" onclick="cbView=\'need\';cbDrawRuns()">needed change</button><button class="chipb'+(cbView==='raw'?' on':'')+'" onclick="cbView=\'raw\';cbDrawRuns()">as measured</button></div>';
h+='<div class="cbx"><table class="cbm"><thead><tr><th></th><th>Now</th><th>Pooled</th><th title="How much the ticked runs disagree in this direction">±</th>';
cbRuns.forEach(r=>{const tot=r.n.reduce((a,b)=>a+b,0),on=pr.includes(r),fit=cbFit(r,pr),why=(fit===null?'no other ticked run to compare with':'pattern '+fit.toFixed(1)+'% (RMS) from the other ticked runs'+(fit>CB_FIT?': an outlier':''))+((r.flags&1)?'':' · browser run (coarser timing)');
h+='<th class="rh'+(on?' on':'')+'" title="'+esc(why)+'"><label><input type="checkbox"'+(on?' checked':'')+' onchange="cbPoolTick('+r.t+',this.checked)"> '+esc(cbShort(r.t))+'</label><div class="mut">'+((r.flags&1)?'BallSheet'+(r.speed?' '+r.speed+'%':''):'page')+' · '+tot+'</div>'+(fit!==null?'<div class="'+(fit>CB_FIT?'wn':'mut')+'">fit '+fit.toFixed(1)+'%</div>':'')+(use.includes(r.t)?'<div class="use">in use</div>':'')+'</th>'});
h+='</tr></thead><tbody>';
for(let k=0;k<8;k++){const d=ps?ps[k]-cur[k]:0;
h+='<tr><td>'+DGA[k]+' '+DGN[k]+'</td><td>'+cur[k].toFixed(2)+'</td><td>'+(ps?ps[k].toFixed(2)+(Math.abs(d)>=.005?' <span class="'+(d>0?'up':'dn')+'">'+(d>0?'+':'\u2212')+Math.abs(d).toFixed(2)+'</span>':''):'–')+'</td><td class="'+(sp&&sp[k]>3?'wn':'mut')+'">'+(sp&&sp[k]!=null?sp[k].toFixed(1)+'%':'–')+'</td>';
cbRuns.forEach(r=>{h+=r.n[k]>=3&&r.med[k]?cell(r.med[k]/1000,r,k):'<td class="mut">–</td>'});h+='</tr>'}
h+='<tr class="ft"><td colspan="4"></td>';
cbRuns.forEach((r,i)=>{h+='<td><button onclick="cbUseRun('+i+')" title="Load this run\'s own suggestion into the editor">Use</button> <button onclick="cbDel('+r.t+')" title="Delete this run">'+(cbDelArm===r.t?'Delete?':'\u00d7')+'</button></td>'});
h+='</tr></tbody></table></div>';
h+='<div class="row" style="margin-top:10px"><button class="p" onclick="cbUsePooled()"'+(ps?'':' disabled')+'>Use pooled suggestion'+(pr.length?' ('+pr.length+' run'+(pr.length>1?'s':'')+')':'')+'</button><span class="mut">then Save above</span></div>';
h+='<p class="mut">'+(cbView==='need'?'Each run column, in the needed-change view: how much more (+, it fell short) or less (\u2212, it overshot) gain each direction needed in that run, relative to the run\'s own average, so runs measured with different gains or speeds compare directly. Runs that belong together show the same pattern down their columns.':'Each run column, as measured: where its flicks stopped, as a % of the distance (\u2212 short, + over), with the gains that run used, so columns measured with different gains don\'t compare directly: use the needed-change view to compare.')+' Fit (under each run): how far its pattern is from the other ticked runs; over '+CB_FIT+'% (amber) is an outlier to untick. Ticked by default: the runs the gains in use came from, otherwise the BallSheet runs that agree with each other. \u00b1 over 3% (amber) means that direction needs more flicks.</p>';
$('cbsavedlist').innerHTML=h}
function cbUseRun(i){const r=cbRuns[i];if(!r)return;dgEdit=cbSug(r);dgEditSrc='run:'+r.t;dgKey='';drawDG();toast('Loaded into the editor: press Save to keep it')}setTimeout(cbLoadRuns,2000);setInterval(()=>{if(!cb&&document.visibilityState==='visible'&&Date.now()-cbRunsAt>15000)cbLoadRuns()},5000);
function cbUse(){if(!cbLast)return;dgEdit=cbLast.sug.slice();dgEditSrc='page-run';dgKey='';drawDG();toast('Press Save to keep it')}
function cbDiscard(){cbLast=null;$('cbres').hidden=true;dgKey='';drawDG()}
$('cbc').addEventListener('pointermove',cbMove);$('cbc').addEventListener('pointerdown',cbDown);$('cbc').addEventListener('contextmenu',e=>e.preventDefault());document.addEventListener('contextmenu',e=>{if(Date.now()-cbEatMenu<1000)e.preventDefault()});
document.addEventListener('fullscreenchange',()=>{if(!cb)return;if(document.fullscreenElement)cbGeoReset();else if(cb.fs)cbStop(cb.data.length>=8)});addEventListener('resize',cbGeoReset);
addEventListener('pagehide',()=>{if(cb&&cb.temp&&navigator.sendBeacon)navigator.sendBeacon('/api/tempdpi?restore=1')});  // the QT Py also restores after 12 s without a keep-alive
document.addEventListener('keydown',e=>{if(cb&&e.key==='Escape')cbStop(cb.data.length>=8)});
// QT Py fw23 + Feather v23: the USB host captures (/api/hosts), fetched while the fold-out is open and when they change
const HDESC={1:'device',2:'config',3:'string',6:'qualifier',7:'other-speed',15:'BOS',0x21:'HID',0x22:'report'};
function hReq(e){const[t,r,vl,vh,il,ih,ll,lh]=e,v=vl|vh<<8,i=il|ih<<8,l=ll|lh<<8,x=n=>n.toString(16).padStart(4,'0');
if(t===0xFF&&r===0xFF)return 'bus reset';
if(r===6&&(t&0x9E)===0x80){const n=HDESC[vh]||'descriptor '+vh;return 'get '+n+(vh===3?' '+vl+(i?' lang '+x(i):''):vh===2&&vl?' '+vl:'')+(t===0x81?' (interface '+i+')':'')+', '+l+' bytes'}
if(t===0&&r===5)return 'set address '+v;if(t===0&&r===9)return 'set configuration '+v;if(t===0x80&&r===8)return 'get configuration';
if(r===0&&(t&0x80))return 'get status ('+['device','interface','endpoint','other'][t&3]+' '+i+')';
if(r===1&&!(t&0x80))return 'clear feature '+v+' ('+['device','interface','endpoint '+x(i),'other'][t&3]+')';
if(r===3&&!(t&0x80))return 'set feature '+v+' ('+['device','interface','endpoint '+x(i),'other'][t&3]+')';
if(t===1&&r===11)return 'set interface '+i+' alt '+v;
if(t===0x21&&r===10)return 'set idle (interface '+i+', '+(vh*4)+' ms)';if(t===0x21&&r===11)return 'set protocol (interface '+i+', '+(v?'report':'boot')+')';
if(t===0x21&&r===9)return 'set report (interface '+i+', '+['','input','output','feature'][vh]+' id '+vl+', '+l+' bytes)';
if(t===0xA1&&r===1)return 'get report (interface '+i+', '+['','input','output','feature'][vh]+' id '+vl+', '+l+' bytes)';
if(t===0xA1&&r===2)return 'get idle (interface '+i+')';if(t===0xA1&&r===3)return 'get protocol (interface '+i+')';
return 'request '+e.map(b=>b.toString(16).padStart(2,'0')).join(' ')}
// QT Py fw24: a DPI stage per host. Hosts are told apart by a signature of their first 6 distinct requests (fw34; the QT Py works it
// out); the guess below only fills in a name: Windows asks for 64 bytes of the device descriptor first, the XIM for 8.
let hSeq=null,hBusy=false,hJ=null;
// QT Py fw25: the host buttons under each DPI stage (S.hosts: [sig, stage 0-5, name, guess when not named]). A host has
// one stage: lighting it under another stage moves it there; tapping a lit one stops switching for that host.
let hbKey='';
function hostName(x){return x[2]||x[3]||'Host'}
function paintHostBtns(){const hs=S.hosts||[];$('hstnote').hidden=!hs.length;const key=JSON.stringify(hs);if(key===hbKey)return;hbKey=key;
rows().forEach((r,i)=>{const box=r.querySelector('.hch');box.innerHTML=hs.map(x=>{const on=x[1]===i+1;return '<button type="button" class="'+(on?'on':'')+'" data-sig="'+x[0]+'" aria-pressed="'+on+'" title="'+(on?'Switches to stage '+(i+1)+' when the ATEN connects it: tap to stop':'Tap: switch to stage '+(i+1)+' when the ATEN connects it')+'">'+esc(hostName(x))+'</button>'}).join('');
box.querySelectorAll('button').forEach(b=>b.onclick=()=>hostToStage(b.dataset.sig,i))})}
function hostToStage(sig,i){const x=(S.hosts||[]).find(x=>x[0]===sig);if(!x)return;const st=x[1]===i+1?0:i+1,nm=x[2]||'';  // fw35: the guess isn't saved as a name, so untapping a never-named host removes its profile
const lb=hostName(x);x[1]=st;x[2]=nm;hbKey='';paintHostBtns();
post('/api/hostprof?sig='+sig+'&stage='+st+'&name='+encodeURIComponent(nm)).then(r=>{say(r,st?lb+' → stage '+st:lb+': stage no longer switched');hSeq=null})}
function hDecode(c){const reqs=[],raw=[];for(let k=0;k<c.req.length;k+=16){const e=[];for(let m=0;m<16;m+=2)e.push(parseInt(c.req.substr(k+m,2),16));raw.push(e);reqs.push(hReq(e))}return{reqs,raw}}
function hGuess(raw){const f=raw.find(e=>!(e[0]===0xFF&&e[1]===0xFF));return !f?'Host':f[6]===64?'PC':f[6]===8?'XIM':'Host'}
function hName(sig){const p=hJ&&hJ.profiles.find(p=>p.sig===sig);return p&&p.name?p.name:null}
function hAutoLine(){const a=S.auto;if(!a||a[0]==='00000000')return 'This power-on: the host isn\'t known yet.';
const n=hName(a[0])||'this host';return 'This power-on: '+n+' · '+(a[1]===4?'no stage chosen':a[1]===2?'stage '+a[2]+' set (the mouse confirmed it)':a[1]===3?'stage '+a[2]+' didn\'t take (the mouse kept another stage)':a[1]===5?'stage '+a[2]+' sent, checking with the mouse…':a[1]===6?'stage left as it was (only the QT Py restarted)':a[1]===7?'stage left as you set it (DPI chord)':a[1]===8?'reading the mouse\'s stage (move it if it sleeps)…':'setting stage '+a[2]+'…')}
function hForget(sig){post('/api/hostprof?sig='+sig+'&stage=0&name=').then(r=>{say(r,'Forgotten');hSeq=null;hbKey=''})}  // fw33: 8 hosts at most, so they can be removed
function hSave(sig){const p=hJ&&hJ.profiles.find(p=>p.sig===sig),st=p?p.stage:0,nm=$('hn_'+sig);const name=nm.value.trim();
post('/api/hostprof?sig='+sig+'&stage='+st+'&name='+encodeURIComponent(name)).then(r=>{say(r,'Saved');hSeq=null;hbKey=''})}
function drawHosts(){const box=$('fhosts');box.hidden=!S||S.hcap==null||!S.hcap_n;if(box.hidden)return;
$('hauto').textContent=hAutoLine();
if(!box.open){if(!$('hlast').textContent)$('hlast').textContent=S.hcap_n+' saved';return}
const hk=S.hcap+JSON.stringify(S.hosts||[]);if(hSeq===hk||hBusy)return;hBusy=true;fetchJ(tq('/api/hosts')).then(j=>{hSeq=hk;hJ=j;
const sigs=new Map();j.caps.forEach(c=>{if(c.sig==='00000000')return;const d=hDecode(c);const g=sigs.get(c.sig)||{n:0,last:0,guess:hGuess(d.raw)};g.n++;g.last=Math.max(g.last,c.t*1000);sigs.set(c.sig,g)});
j.profiles.forEach(p=>{if(!sigs.has(p.sig))sigs.set(p.sig,{n:0,last:0,guess:'Host'})});
const typing=$('hprof').contains(document.activeElement);if(typing)hSeq=null;if(!typing)$('hprof').innerHTML=[...sigs].map(([sig,g])=>{const p=j.profiles.find(p=>p.sig===sig)||{stage:0,name:''};
return '<div class="opt"><span><input type="text" id="hn_'+sig+'" maxlength="10" value="'+esc(p.name)+'" placeholder="'+g.guess+'" style="width:8em" onchange="hSave(\''+sig+'\')"> <span class="mut">'+(g.n?'seen '+g.n+'× · last '+(g.last?whenText(g.last):'time unknown'):'not in the saved list')+(sig===j.auto.sig?' · connected now':'')+'</span></span>'+
'<span><span class="chip'+(p.stage?' ok':'')+'">'+(p.stage?'stage '+p.stage:'stage not switched')+'</span>'+(p.stage||p.name?' <button type="button" class="x" title="Forget this host\'s stage and name" onclick="hForget(\''+sig+'\')">×</button>':'')+'</span></div>'}).join('')||'<p class="mut">No host recognised yet.</p>';
const h=j.caps.map(c=>{const d=hDecode(c);const when=c.t?whenText(c.t*1000):c.boot===j.boot?'this power-on':'power-on #'+c.boot+' (time unknown)';
const sum=(hName(c.sig)||hGuess(d.raw))+' · '+d.reqs.length+' kept of '+c.seen+' requests · '+c.resets+' bus reset'+(c.resets===1?'':'s')+' · '+(c.cfg_ms===65535?'not configured':'configured after '+c.cfg_ms+' ms');
return {when,sum,reqs:d.reqs}});
$('hlast').textContent=h.length?'Last: '+h[0].when+' · '+h[0].sum:'';
$('hlist').innerHTML=h.map(c=>'<details class="cap"><summary><span>'+esc(c.when)+' · '+esc(c.sum)+'</span></summary><ol class="hreq">'+c.reqs.map(s=>'<li>'+esc(s)+'</li>').join('')+'</ol></details>').join('')}).catch(()=>{}).finally(()=>{hBusy=false})}
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;')}
function whenText(ms){if(!ms)return 'earlier (time unknown)';const d=new Date(ms),t=d.toLocaleTimeString();
return d.toDateString()===new Date().toDateString()?t:d.toLocaleDateString(undefined,{month:'short',day:'numeric'})+' '+t}
function liHtml(a){return a.map(i=>'<li>'+(whenText(i.at)+'  '+i.s).replace(/&/g,'&amp;').replace(/</g,'&lt;')+'</li>').join('')}
// Sensor list: the Feather's events since it powered on (chords, wakes) plus the saved page edits of stage, angle and
// position. With saved edits the Feather's own "Page" events are left out: the saved ones are the same edits, with the sender.
function drawEvents(){const ed=S.edits,items=[];
(S.events||[]).forEach(e=>{if(!(ed&&e[1]===6))items.push({at:Date.now()-e[0]*1000,s:evText(e)})});
(ed||[]).forEach(x=>{if((x[2]>=1&&x[2]<=3)||x[2]===10)items.push({at:edAt(x),s:edText(x)})});
items.sort((p,q)=>q.at-p.at);const sh=items.slice(0,8),box=$('evbox');box.hidden=!sh.length;
if(sh.length){$('evlast').textContent='Last change: '+whenText(sh[0].at)+'  '+sh[0].s;$('evlist').innerHTML=liHtml(sh)}
const fe=$('fedits');fe.hidden=!ed||!ed.length;if(ed&&ed.length){const all=ed.map(x=>({at:edAt(x),s:edText(x)}));
$('edlast').textContent='Last: '+whenText(all[0].at)+'  '+all[0].s;$('edlist').innerHTML=liHtml(all)}}
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

// QT Py fw37: the tabs; the open one is kept on the QT Py like the fold-outs (a #hash in the address wins)
const TABS=['mouse','aim','setup','diag'];let tabSet=false;
function tabShow(n,save){if(!TABS.includes(n))n='mouse';TABS.forEach(t=>{$('t-'+t).hidden=t!==n;const b=$('tb-'+t);b.classList.toggle('on',t===n);b.setAttribute('aria-selected',t===n?'true':'false')});
if(save){tabSet=true;try{history.replaceState(null,'','#'+n)}catch(e){}post('/api/ui?tab='+TABS.indexOf(n))}}
function tabApply(){if(tabSet||!S)return;tabSet=true;if(TABS.includes(location.hash.slice(1)))return;if(S.ui_tab>=0&&S.ui_tab<TABS.length)tabShow(TABS[S.ui_tab])}
tabShow(location.hash.slice(1));
// status bar: the host the ATEN connects, its DPI stage, direction gains, battery
function drawBar(){const b=[],a=S.auto,hs=S.hosts||[],ch=(c,s,t)=>'<span class="chip'+(c?' '+c:'')+'"'+(t?' title="'+esc(t)+'"':'')+'>'+esc(s)+'</span>';
if(a&&a[0]!=='00000000'){const x=hs.find(x=>x[0]===a[0]);b.push(ch(a[1]===3?'bad':'',x?hostName(x):'Host',hAutoLine()))}
const d=S.stage!=null&&S.dpi&&S.dpi[S.stage];if(d)b.push(ch('','Stage '+(S.stage+1)+' · '+d[0]+(d[1]&&d[1]!==d[0]?'/'+d[1]:'')+' DPI'));
if(S.dg&&S.dg.sup)b.push(ch(S.dg.on?'ok':'','Gains '+(S.dg.on?'on':'off')));
const bt=S.battery;if(bt)b.push(ch(bt.pct<=15?'bad':'','Battery '+bt.pct+'%'));
$('sbar').innerHTML=b.join('')}
function render(){const st=$('state');
if(!S.link){st.innerHTML='<span class="chip bad">No answer from the Feather</span><span class="mut">Check the 4-pin cable.</span>'}
else{st.innerHTML='<span class="chip'+(S.warg||S.vuk?' ok':'')+'">'+(S.warg?'Warg connected':S.vuk?'VUK connected':'No G-Wolves mouse')+'</span>'+(S.warg&&S.paused?'<span class="chip bad">mouse didn\'t answer (asleep?)</span><span class="mut">Move it: this page reads it again by itself.</span>':S.warg&&S.reads===false?'<span class="chip">mouse hasn\'t answered a read yet</span>':'')+(S.refreshing?'<span class="chip">reading…</span>':'')+(S.pending?'<span class="chip">sending…</span>':'')}
if(pgNew)st.insertAdjacentHTML('beforeend','<button class="chip" onclick="pgGo()">New page version: reload</button>');
paintStages();if(S.palette&&S.stage!=null&&S.palette[S.stage])fav(S.palette[S.stage]);
['angle','pos'].forEach(k=>{if(!edit[k]||Date.now()-edit[k]>1500)setVal(k,S[k])});
drawChordEditor();drawTune();drawRadio();drawOpt();drawEvents();drawHosts();drawDG();drawLevel();c3Draw();foldsApply();
const bt=S.battery;$('batt').textContent=bt?bt.pct+'% · '+(bt.mv/1000).toFixed(2)+' V'+(bt.age_s<0?'':' · read '+(bt.age_s<60?'just now':Math.round(bt.age_s/60)+' min ago')):'not read yet: move the mouse, then Read from mouse';
$('ver').textContent=(S.fw?'Feather v'+S.fw+' · ':'')+'QT Py firmware '+S.qt_fw+(S.page&&S.page.fs?' · page uploaded '+whenText(S.page.t*1000):'');drawBar();tabApply()}
// Opened as gwolves.local: once, before the first render and before any input, check the plain
// address answers and move there, so reloads and button presses don't need another .local name
// lookup (those fail on some phones and PCs). If the address doesn't answer, stay on .local.
let ipTried=false,touched=false;['input','pointerdown','keydown'].forEach(e=>document.addEventListener(e,()=>{touched=true},true));
async function toIp(j){if(ipTried||touched)return false;ipTried=true;const ip=j&&j.radio&&j.radio.mode==='sta'?j.radio.ip:'';
if(!/\.local$/i.test(location.hostname)||!/^\d+\.\d+\.\d+\.\d+$/.test(ip)||ip==='0.0.0.0')return false;
const c=new AbortController(),tm=setTimeout(()=>c.abort(),2500);try{await fetch('http://'+ip+'/api/status',{mode:'no-cors',cache:'no-store',signal:c.signal})}catch(e){return false}finally{clearTimeout(tm)}
if(touched)return false;location.replace('http://'+ip+'/');return true}
// A new page on the QT Py (an upload, or new firmware): a style-only change is swapped in place; anything else
// reloads this page by itself once nothing is being edited (tab and fold-outs live on the QT Py, the scroll position
// comes back), otherwise a chip in the status bar offers it. page_gz.ps1 stamps <meta name="gw-rest"> with a hash of
// everything but the style, so equal stamps mean only the style changed.
let pgKey=null,pgNew=false,pgFetching=false;
function pgKeyOf(j){return j.qt_fw+':'+(j.page&&j.page.fs?j.page.t+':'+j.page.len:'built-in')}
function pgBusy(){const a=document.activeElement;return !!(cb||lvOn||c3.busy||c3.n||(S&&S.turn&&S.turn.state===6)||msDrag||dCol||dDpi||dTune||chDirty||dgEdit||dgDrag>=0||['angle','pos','stage'].some(k=>edit[k]&&Date.now()-edit[k]<3000)||
(a&&a!==document.body&&/^(INPUT|TEXTAREA|SELECT)$/.test(a.tagName)&&a.type!=='range'&&a.type!=='checkbox'&&a.type!=='color'))}
// back to where it was: the cards below fill in over a few seconds, so keep trying until the page is that tall (or you scroll)
function pgScroll(want){const t0=Date.now();let stop=false;['wheel','pointerdown','keydown','touchstart'].forEach(e=>addEventListener(e,()=>{stop=true},{once:true,passive:true}));
const go=()=>{if(stop)return;scrollTo(0,want);if(Math.abs(scrollY-want)>2&&Date.now()-t0<4000)setTimeout(go,100)};setTimeout(go,0)}  // a timer, not requestAnimationFrame: that never runs in a hidden tab
function pgGo(){try{sessionStorage.setItem('gwScroll',String(scrollY))}catch(e){}location.reload()}
async function pgCheck(j){const k=pgKeyOf(j);
if(pgKey===null){pgKey=k;try{const y=sessionStorage.getItem('gwScroll');if(y!==null){sessionStorage.removeItem('gwScroll');pgScroll(Number(y))}}catch(e){}return}
if(k===pgKey||pgFetching)return;if(pgNew){if(!pgBusy())pgGo();return}
pgFetching=true;try{const t=await(await fetch('/',{cache:'no-store'})).text(),m=t.match(/<meta name="gw-rest" content="([0-9a-f]+)">/),mine=document.querySelector('meta[name=gw-rest]'),s=t.match(/<style>([\s\S]*?)<\/style>/);
if(m&&s&&mine&&mine.content===m[1]){document.querySelector('style').textContent=s[1];pgKey=k;toast('Page style updated');return}
pgNew=true;if(!pgBusy())pgGo();else render()}catch(e){}finally{pgFetching=false}}
function poll(){fetchJ(tq('/api/status')).then(async j=>{if(!S&&await toIp(j))return;if(S&&edit.stage&&Date.now()-edit.stage<1500)j.stage=S.stage;S=j;optOverlay();if(!cb){render();autoRead()}pgCheck(j)}).catch(()=>{$('state').innerHTML='<span class="chip bad">Page lost the QT Py</span><span class="mut">Its Wi-Fi may be off, or its address changed: open gwolves.local again.</span>'}).finally(()=>setTimeout(poll,document.hidden?5000:1000))}  // fw33: no full render while a flick calibration runs (its stalls land in the timing); a background tab polls every 5 s
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
let lvRaf=0;
function lvStop(){if(!lvOn)return;lvOn=false;if(document.pointerLockElement===LV)document.exitPointerLock();if(document.fullscreenElement)document.exitFullscreen().catch(()=>{});$('lvwrap').classList.remove('full');lvDraw();lvResult();$('lvl').scrollIntoView({block:'nearest'})}
document.addEventListener('fullscreenchange',()=>{if(lvOn&&lvFs&&!document.fullscreenElement)lvStop()});
LV.addEventListener('mousedown',e=>{if(e.button===2){e.preventDefault();lvStop()}else if(e.button===0&&!lvOn)lvBegin()});
LV.addEventListener('contextmenu',e=>e.preventDefault());
document.addEventListener('mousemove',e=>{if(!lvOn)return;const dx=e.movementX||0,dy=e.movementY||0;if(!dx&&!dy)return;
lv.x+=dx;lv.y+=dy;lv.pts.push([lv.x,lv.y]);lv.sxx+=dx*dx;lv.syy+=dy*dy;lv.sxy+=dx*dy;lv.dist+=Math.hypot(dx,dy);if(!lvRaf)lvRaf=requestAnimationFrame(()=>{lvRaf=0;lvDraw()})});  // fw33: once a frame (every 3rd sample redrew everything: quadratic)
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
// fw43: cm/360 in game, through the XIM. The Feather turns the view by itself at a steady speed (v27 gw_turn), so the
// mouse travel is exact; you say whether the crosshair stopped short of the edge, on it or past it, and each answer
// halves the range. No small nudges: the XIM turns short bursts unreliably. Results are kept on the QT Py (c360).
let c3={n:0,lo:0,hi:0,sp:0,tn:1,busy:false,run:-1,at:0,dur:0,ask:false,msg:'',lk:'',csaved:-1};
// fw44 (Feather v28): count mode, you turn by hand and the Feather counts the X that reaches the XIM; live every 0.2 s
function c3Count(){const t=S.turn;if(!t)return;if(t.state===6){post('/api/turn?stop=1');return}post('/api/turn?count=1').then(r=>{if(!(r&&r.ok))toast('Not started: '+(r&&r.err||'no answer'),true)})}
function c3CSave(){const t=S.turn,cpc=c3Cpc(),tn=Number($('c3ctn').value);if(!t||t.state!==7||!cpc||!t.sent)return;const cm=Math.abs(t.sent)/cpc/tn;
post('/api/c360?cm='+Math.round(cm*100)+'&sp=0&dpi='+Math.round(cpc*2.54)).then(r=>{if(r&&r.ok){c3.csaved=t.run;toast('Saved: '+cm.toFixed(2)+' cm/360 (by hand)');c3Draw()}else toast('Not saved: '+(r&&r.err||'no answer'),true)})}
function c3Cpc(){const d=S&&S.stage!=null&&S.dpi&&S.dpi[S.stage];return d&&d[0]?d[0]/2.54:0}
function c3Reset(){c3.n=c3.lo=c3.hi=0;c3.ask=false;c3.msg='';c3Draw()}
async function c3Turn(){const cpc=c3Cpc(),e=Number($('c3est').value),sp=Number($('c3sp').value),tn=Number($('c3tn').value);
if(!S.turn||!S.turn.sup||!cpc||c3.busy)return;if(sp!==c3.sp||tn!==c3.tn){c3.lo=c3.hi=0;c3.sp=sp;c3.tn=tn}
if(!c3.n){if(!(e>=2&&e<=300)){toast('Type a guess first (cm/360)',true);return}c3.n=Math.round(e*cpc)}
const total=c3.n*tn,dur=total/(sp*cpc)*1000;if(dur>28000){toast('Too long for one turn (30 s at most): a faster speed or fewer turns',true);return}
c3.run=(S.turn.run+1)&255;c3.busy=true;c3.ask=false;c3.at=Date.now();c3.dur=dur;c3.msg='Turning'+(tn>1?' '+tn+' times':'')+'… hands off the mouse.';c3Draw();
const r=await post('/api/turn?n='+total+'&r='+Math.round(sp*cpc)+'&hand='+Math.round(cpc*0.2));if(!r||!r.ok){c3.busy=false;c3.msg='The QT Py refused: '+(r&&r.err||'no answer')+'.';c3Draw()}}
function c3Stop(){post('/api/turn?stop=1')}
function c3Ans(a){const cpc=c3Cpc();if(!c3.n||!cpc)return;
if(a===0){const cm=c3.n/cpc,sp=c3.sp;post('/api/c360?cm='+Math.round(cm*100)+'&sp='+Math.round(sp*10)+'&dpi='+Math.round(cpc*2.54)).then(r=>{if(r&&r.ok){toast('Saved: '+cm.toFixed(2)+' cm/360 at '+sp+' cm/s');$('c3est').value=cm.toFixed(2);c3Reset()}else toast('Not saved: '+(r&&r.err||'no answer'),true)});return}
if(a<0)c3.lo=c3.n;else c3.hi=c3.n;c3.n=Math.round(c3.lo&&c3.hi?(c3.lo+c3.hi)/2:c3.n*(a<0?1.06:0.94));c3.ask=false;c3.msg='Line the crosshair up on the edge again, let go, then Turn.';c3Draw()}
function c3Del(t){post('/api/c360?del='+t).then(r=>{if(!(r&&r.ok))toast('Not removed',true)})}
function c3Draw(){if(!S||!$('c3card'))return;const t=S.turn,cpc=c3Cpc(),ok=!!(t&&t.sup&&cpc);
if(c3.busy&&t&&t.run===c3.run&&t.state!==1){c3.busy=false;c3.ask=t.state===2;c3.msg=t.state===3?'Stopped: the mouse moved. Let go of it during a turn, line up again, then Turn.':t.state===4?'Stopped.':t.state===5?'Stopped after 30 s.':''}
if(c3.busy&&Date.now()-c3.at>c3.dur+8000){c3.busy=false;c3.msg='No word from the Feather about the turn: try again.'}
$('c3off').hidden=ok;$('c3off').textContent=!(t&&t.sup)?'Needs Feather firmware v27 or newer.':'The DPI isn\'t known yet: Read from mouse.';
$('c3go').disabled=!ok||c3.busy||t.state===6;$('c3stop').disabled=!c3.busy;$('c3reset').disabled=c3.busy||!c3.n;$('c3ans').hidden=!c3.ask||c3.busy;
const cm=c3.n&&cpc?c3.n/cpc:0,rg=c3.lo||c3.hi?' · between '+(c3.lo?(c3.lo/cpc).toFixed(2):'?')+' and '+(c3.hi?(c3.hi/cpc).toFixed(2):'?')+' cm':'';
$('c3now').textContent=(c3.msg?c3.msg+' ':'')+(cm?'This try: '+cm.toFixed(2)+' cm/360'+rg:'');$('c3go').textContent=cm?'Turn ('+cm.toFixed(2)+' cm)':'Turn';
const cnt=!!(t&&(t.state===6||t.state===7)),tn2=Number($('c3ctn').value),ccm=cnt&&cpc?Math.abs(t.sent)/cpc:0;$('c3cbox').hidden=!(t&&t.count);
$('c3cgo').textContent=t&&t.state===6?'Stop':'Start counting';$('c3cgo').disabled=!ok||c3.busy;$('c3csave').disabled=!(t&&t.state===7&&t.sent&&c3.csaved!==t.run);
$('c3cnow').textContent=cnt&&(t.state===6||t.sent)?(t.state===6?'Counting: ':'Counted: ')+ccm.toFixed(2)+' cm'+(tn2>1?' for '+tn2+' turns':'')+' = '+(ccm/tn2).toFixed(2)+' cm/360'+(t.state===6?' (updates as you turn)':''):'';
const L=S.c360||[],k=JSON.stringify(L);if(!$('c3est').value&&L.length&&document.activeElement!==$('c3est'))$('c3est').value=(L[0][1]/100).toFixed(2);
if(k!==c3.lk){c3.lk=k;$('c3list').innerHTML=L.length?'<tr><th>When</th><th>cm/360</th><th>Speed</th><th>DPI</th><th></th></tr>'+L.map(r=>'<tr><td>'+esc(whenText(r[0]*1000))+'</td><td><b>'+(r[1]/100).toFixed(2)+'</b></td><td>'+(r[2]?r[2]/10+' cm/s':'by hand')+'</td><td>'+r[3]+'</td><td><button onclick="c3Del('+r[0]+')" aria-label="Remove">✕</button></td></tr>').join(''):''}}
function drawLevel(){const sn=!!(S.opt&&(S.opt.snap===true||S.opt.snap===1));$('lvsnap').hidden=!sn}
new ResizeObserver(()=>lvDraw()).observe(LV);
// fold-outs open as they were left (kept on the QT Py: the page's address changes, and it's shared by your devices)
const FOLDS=['fchords','fwifi','evbox','fedits','fhosts'];let foldMask=null;
function foldNow(){let m=0;FOLDS.forEach((id,i)=>{if($(id).open)m|=1<<i});return m}
function foldsApply(){if(foldMask!==null||!S||S.ui==null)return;if(S.ui>=0)FOLDS.forEach((id,i)=>{$(id).open=!!(S.ui>>i&1)});foldMask=foldNow()}
FOLDS.forEach(id=>$(id).addEventListener('toggle',()=>{if(foldMask===null)return;const m=foldNow();if(m!==foldMask){foldMask=m;post('/api/ui?f='+m)}}));
poll();
</script></body></html>)HTML";
