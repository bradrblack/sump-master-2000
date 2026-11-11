// The status and calibration page served at http://sm2k.local/ (see the "Web
// page" section of sm2k.ino). One self-contained file: no external scripts,
// fonts or images, so it works with no internet connection.
#pragma once
#include <Arduino.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Sump Master 2000</title>
<style>
:root{--bg:#f4f5f7;--card:#fff;--fg:#1c2330;--dim:#667085;--line:#d8dce3;--ok:#1a8f4c;--run:#1f6feb;--bad:#d1242f;--warn:#b7791f}
@media(prefers-color-scheme:dark){:root{--bg:#12151b;--card:#1c212b;--fg:#e6e9ef;--dim:#98a2b3;--line:#333b49;--ok:#4cc38a;--run:#6ea8fe;--bad:#ff7b72;--warn:#e3b341}}
*{box-sizing:border-box}body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:15px/1.4 system-ui,sans-serif}
main{max-width:760px;margin:0 auto}h1{font-size:19px;margin:0}h2{font-size:14px;margin:0 0 8px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
.top{display:flex;justify-content:space-between;align-items:center;gap:8px;flex-wrap:wrap;margin-bottom:12px}
.sub{color:var(--dim);font-size:13px}
.badge{display:inline-block;padding:4px 12px;border-radius:999px;color:#fff;font-weight:600;background:var(--dim)}#alarm{background:var(--bad)}[hidden]{display:none!important}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin-bottom:12px}
.tiles{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:12px;margin-bottom:12px}
.tiles .card{margin:0}.big{font-size:24px;font-weight:650;font-variant-numeric:tabular-nums}
canvas{width:100%;height:150px;display:block}
table{border-collapse:collapse;width:100%}td{padding:3px 0;font-variant-numeric:tabular-nums}td:last-child{text-align:right}
form{display:grid;grid-template-columns:repeat(auto-fit,minmax(200px,1fr));gap:10px}
label{display:block;font-size:13px;color:var(--dim)}
input{width:100%;padding:8px;margin-top:3px;font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:6px}
.btns{grid-column:1/-1;display:flex;gap:8px;flex-wrap:wrap}
button{padding:9px 16px;font:inherit;font-weight:600;border-radius:6px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
button.go{background:var(--run);border-color:var(--run);color:#fff}
pre{margin:0;max-height:260px;overflow:auto;font:12px/1.35 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-word}
.note{font-size:13px;color:var(--dim);margin:8px 0 0}.msg{color:var(--ok);font-weight:600}.err{color:var(--bad)}
</style></head><body><main>
<div class="top"><div><h1>Sump Master 2000</h1><div class="sub" id="sub">connecting&hellip;</div></div><div><span class="badge" id="alarm" hidden>HIGH WATER</span> <span class="badge" id="state">&hellip;</span></div></div>
<div class="tiles">
<div class="card"><h2>Vibration</h2><div class="big" id="rms">&ndash;</div><div class="sub">m/s&sup2; RMS, loudest in the last second</div></div>
<div class="card"><h2>Water depth</h2><div class="big" id="level">&ndash;</div><div class="sub" id="dist">&nbsp;</div></div>
<div class="card"><h2>Air</h2><div class="big" id="air">&ndash;</div><div class="sub" id="wifi">&nbsp;</div></div>
</div>
<div class="card"><h2>Vibration, last few minutes</h2><canvas id="chart"></canvas>
<p class="note">Solid line: start threshold. Dashed line: stop threshold. The chart starts when this page is opened.</p></div>
<div class="card"><h2>Calibration readings</h2><table>
<tr><td>Idle, since restart</td><td id="idle">&ndash;</td></tr>
<tr><td>Last pump run</td><td id="lastrun">none since restart</td></tr>
<tr><td>Suggested thresholds</td><td id="sugg">&ndash;</td></tr>
</table><p class="note">The suggestion is a starting point: it puts both thresholds between the idle average and the quietest moment of the last run.</p></div>
<div class="card"><h2>Settings</h2><div id="saved"></div>
<form method="post" action="/settings">
<label>Pump start threshold (m/s&sup2;)<input name="on" inputmode="decimal" required></label>
<label>Pump stop threshold (m/s&sup2;)<input name="off" inputmode="decimal" required></label>
<label>Seconds of vibration to count as started<input name="run_s" inputmode="decimal" required></label>
<label>Seconds of quiet to count as stopped<input name="stop_s" inputmode="decimal" required></label>
<label>Sensor to pit floor (cm)<input name="floor" inputmode="decimal" required></label>
<label>High-water alarm depth (cm)<input name="high" inputmode="decimal" required></label>
<label>Alarm clears this far below (cm)<input name="hyst" inputmode="decimal" required></label>
<label>A run should lower the water by (cm)<input name="drop" inputmode="decimal" required></label>
<div class="btns"><button class="go" type="submit">Save</button>
<button type="submit" name="reset" value="1" formnovalidate onclick="return confirm('Put every setting back to the firmware default?')">Reset to defaults</button></div>
</form><p class="note">Saving asks for a user name and password: <b>admin</b> and the OTA password. Settings are kept on the board and survive restarts and updates.</p></div>
<div class="card"><h2>Log</h2><pre id="log"></pre></div>
</main><script>
const $=id=>document.getElementById(id),f=(v,d)=>v==null?'–':v.toFixed(d);
let hist=[],cfg=null,seq=0,filled=false;
const q=new URLSearchParams(location.search);
if(q.has('saved'))$('saved').innerHTML='<p class="msg">Settings saved.</p>';
if(q.has('reset'))$('saved').innerHTML='<p class="msg">Settings reset to the firmware defaults.</p>';
function draw(){const c=$('chart'),r=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*r;c.height=h*r;
const g=c.getContext('2d');g.scale(r,r);const cs=getComputedStyle(document.documentElement),col=n=>cs.getPropertyValue(n);
let top=Math.max(cfg?cfg.on*1.6:1,...hist)*1.05;const y=v=>h-4-(v/top)*(h-8);
g.strokeStyle=col('--line');g.lineWidth=1;g.strokeRect(.5,.5,w-1,h-1);
if(cfg){g.strokeStyle=col('--warn');g.beginPath();g.moveTo(0,y(cfg.on));g.lineTo(w,y(cfg.on));g.stroke();
g.setLineDash([5,4]);g.beginPath();g.moveTo(0,y(cfg.off));g.lineTo(w,y(cfg.off));g.stroke();g.setLineDash([]);}
g.strokeStyle=col('--run');g.lineWidth=1.5;g.beginPath();const n=150;
hist.forEach((v,i)=>{const x=(i+n-hist.length)/(n-1)*w;i?g.lineTo(x,y(v)):g.moveTo(x,y(v));});g.stroke();
g.fillStyle=col('--dim');g.font='11px system-ui';g.fillText(top.toFixed(2),4,13);}
async function status(){try{const s=await(await fetch('/api/status')).json();cfg=s.cfg;
$('sub').textContent=s.device+' · firmware '+s.version+' · '+s.time+' · up '+(s.uptime_s/3600).toFixed(1)+' h';
const b=$('state'),m={running:['Pump running','--run'],idle:['Idle','--ok'],'no-accel':['No accelerometer','--bad']}[s.state];
b.textContent=m[0];b.style.background='var('+m[1]+')';$('alarm').hidden=!s.high_water;
$('rms').textContent=f(s.rms_max,3);
$('level').textContent=s.level_cm==null?'no reading':(s.level_near?'≥ ':'')+f(s.level_cm,1)+' cm';
$('dist').textContent=s.dist_cm==null?'':'sensor to water '+f(s.dist_cm,1)+' cm';
$('air').textContent=s.temp_c==null?'no reading':f(s.temp_c,1)+' °C · '+f(s.humidity,0)+' %';
$('wifi').textContent='Wi-Fi '+s.rssi+' dBm · '+s.tgq+' point(s) queued';
$('idle').textContent=s.idle.n?'average '+f(s.idle.avg,3)+' · peak '+f(s.idle.max,3):'no readings yet';
if(s.last_run.n)$('lastrun').textContent='average '+f(s.last_run.avg,3)+' · lowest '+f(s.last_run.min,3)+' · '+f(s.last_run.seconds,1)+' s, '+Math.round(s.last_run.ago_s/60)+' min ago';
if(s.idle.n&&s.last_run.n){const a=s.idle.avg,d=s.last_run.min-a;
$('sugg').textContent=s.last_run.min>2*a?'start '+(a+.6*d).toFixed(2)+' · stop '+(a+.35*d).toFixed(2):'pump vibration is too close to the idle noise';}
else $('sugg').textContent='needs one pump run';
hist.push(s.rms_max);if(hist.length>150)hist.shift();draw();
if(!filled){filled=true;const F=document.forms[0],c=s.cfg;F.on.value=c.on;F.off.value=c.off;F.run_s.value=c.run_ms/1000;F.stop_s.value=c.stop_ms/1000;
F.floor.value=c.floor_cm;F.high.value=c.high_cm;F.hyst.value=c.hyst_cm;F.drop.value=c.drop_cm;}
}catch(e){$('sub').textContent='not responding…';}}
async function log(){try{const t=await(await fetch('/api/log?since='+seq)).text(),i=t.indexOf('\n');seq=+t.slice(0,i);
const body=t.slice(i+1);if(body){const p=$('log'),end=p.scrollTop+p.clientHeight>=p.scrollHeight-8;p.textContent+=body;if(end)p.scrollTop=p.scrollHeight;}}catch(e){}}
status();log();setInterval(status,2000);setInterval(log,5000);
</script></body></html>
)HTML";
