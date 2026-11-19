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
input,select{width:100%;padding:8px;margin-top:3px;font:inherit;color:var(--fg);background:var(--bg);border:1px solid var(--line);border-radius:6px}
input[type=checkbox]{width:auto;margin:0 6px 0 0}input[type=file]{padding:6px}.chk{align-self:end;padding-bottom:9px}
.btns{grid-column:1/-1;display:flex;gap:8px;flex-wrap:wrap}.full{grid-column:1/-1}
button{padding:9px 16px;font:inherit;font-weight:600;border-radius:6px;border:1px solid var(--line);background:var(--bg);color:var(--fg)}
button.go{background:var(--run);border-color:var(--run);color:#fff}button:disabled{opacity:.5}
pre{margin:0;max-height:260px;overflow:auto;font:12px/1.35 ui-monospace,Menlo,monospace;white-space:pre-wrap;word-break:break-word}
ol{margin:0 0 10px;padding-left:20px}ol li{margin-bottom:10px}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin-top:4px}
.note{font-size:13px;color:var(--dim);margin:8px 0 0}.msg{color:var(--ok);font-weight:600}.err{color:var(--bad)}
.more-btn{display:flex;align-items:center;gap:12px;width:100%;height:44px;margin:0 0 12px;padding:0;background:none;border:0;border-radius:8px;color:var(--dim);font:inherit;font-size:14px;font-weight:400;cursor:pointer}
.more-btn::before,.more-btn::after{content:'';flex:1;height:1px;background:var(--line)}
.more-btn i{display:block;width:7px;height:7px;margin:-3px 0 0 -4px;border-right:2px solid currentColor;border-bottom:2px solid currentColor;transform:rotate(45deg);transition:transform .15s}
.more-btn.open i{transform:rotate(-135deg);margin-top:3px}
.more-btn:hover,.more-btn:focus-visible{color:var(--fg)}.more-btn:focus-visible{outline:2px solid var(--run);outline-offset:-2px}
</style></head><body><main>
<div class="top"><div><h1>Sump Master 2000</h1><div class="sub" id="sub">connecting&hellip;</div></div><div><span class="badge" id="alarm" hidden>HIGH WATER</span> <span class="badge" id="state">&hellip;</span></div></div>
<div class="tiles">
<div class="card"><h2>Vibration</h2><div class="big" id="rms">&ndash;</div><div class="sub">m/s&sup2; RMS, loudest in the last second</div></div>
<div class="card"><h2>Water depth</h2><div class="big" id="level">&ndash;</div><div class="sub" id="dist">&nbsp;</div></div>
<div class="card"><h2>Air</h2><div class="big" id="air">&ndash;</div><div class="sub" id="wifi">&nbsp;</div></div>
</div>
<div class="card"><h2>Vibration, last few minutes</h2><canvas id="chart"></canvas>
<p class="note">Solid line: start threshold. Dashed line: stop threshold. The chart starts when this page is opened.</p></div>
<button type="button" class="more-btn" id="more" aria-expanded="false" aria-controls="more-body"><span>Show more settings</span><i></i></button>
<div id="more-body" hidden>
<div class="card"><h2>Calibrate</h2><ol>
<li>With the pump <b>off</b> (and the house as busy as it normally gets), measure the idle vibration.
<div class="row"><button type="button" class="cal" data-p="idle">Measure idle (30 s)</button><span class="sub" id="calidle">not measured</span></div></li>
<li>With the pump <b>running</b> (fill the pit with a bucket of water if it needs a push), measure again.
<div class="row"><button type="button" class="cal" data-p="run">Measure running (30 s)</button><span class="sub" id="calrun">not measured</span></div></li>
<li>Use the suggestion, then <b>Save</b> under Settings.
<div class="row"><button type="button" id="use" disabled>Use suggested thresholds</button><span class="sub" id="sugg">needs both measurements</span></div></li>
</ol>
<table>
<tr><td>Passive: idle since restart</td><td id="idle">&ndash;</td></tr>
<tr><td>Passive: last pump run</td><td id="lastrun">none since restart</td></tr>
</table>
<p class="note">The suggestion puts both thresholds between the loudest idle moment and the quietest running moment. The passive lines keep the idle peak and the last run's lowest value since the last restart, so you can check the thresholds after a few real runs.</p></div>
<div class="card"><h2>Settings</h2><div id="saved"></div>
<form method="post" action="/settings" id="fs">
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
</form><p class="note">Saving asks for a user name and password: <b>admin</b> and the admin password (the OTA password, unless you have changed it below). Settings are kept on the board and survive restarts and updates.</p></div>
<div class="card"><h2>Device</h2><div id="dsaved"></div>
<form method="post" action="/device" id="fd">
<label>Device name<input name="name" required pattern="[a-z0-9]([a-z0-9\-]{0,22}[a-z0-9])?" autocapitalize="off" spellcheck="false"></label>
<label>Time zone (POSIX TZ)<input name="tz" list="tzs" required autocapitalize="off" spellcheck="false"></label>
<datalist id="tzs">
<option value="EST5EDT,M3.2.0,M11.1.0/2">US Eastern</option>
<option value="CST6CDT,M3.2.0,M11.1.0/2">US Central</option>
<option value="MST7MDT,M3.2.0,M11.1.0/2">US Mountain</option>
<option value="MST7">US Arizona</option>
<option value="PST8PDT,M3.2.0,M11.1.0/2">US Pacific</option>
<option value="GMT0BST,M3.5.0/1,M10.5.0">UK</option>
<option value="CET-1CEST,M3.5.0,M10.5.0/3">Central Europe</option>
<option value="AEST-10AEDT,M10.1.0,M4.1.0/3">Australia East</option>
<option value="UTC0">UTC</option>
</datalist>
<div class="btns"><button class="go" type="submit">Save device</button></div>
</form><p class="note">The name is used in notifications, as the InfluxDB <code>device</code> tag and as the Home Assistant device; lowercase letters, digits and -. The board stays at <b>sm2k.local</b>. The time zone sets log times, the daily report and the 3 AM restart. Both take effect at once.</p></div>
<div class="card"><h2>Connections</h2><div id="csaved"></div><p class="sub" id="cstat">&nbsp;</p>
<form method="post" action="/connections" id="fc">
<label>Telegraf address (empty turns it off)<input name="tg_url" placeholder="http://server:8186/sump" autocapitalize="off"></label>
<label>ntfy topic<input name="ntfy" autocomplete="off" autocapitalize="off"></label>
<label class="chk"><input type="checkbox" name="ntfy_off" value="1">Turn ntfy pushes off</label>
<label>MQTT broker for Home Assistant (empty turns it off)<input name="mqtt_host" placeholder="192.168.1.10" autocapitalize="off"></label>
<label>MQTT port<input name="mqtt_port" inputmode="numeric"></label>
<label>MQTT user name (empty if the broker needs none)<input name="mqtt_user" autocomplete="off" autocapitalize="off"></label>
<label>MQTT password<input name="mqtt_pass" type="password" autocomplete="new-password"></label>
<label>New admin password<input name="admin" type="password" autocomplete="new-password" placeholder="leave empty to keep the current one"></label>
<div class="btns"><button class="go" type="submit">Save connections</button></div>
</form><p class="note">The ntfy topic and the passwords are never shown here; leave those boxes empty to keep what is saved. Changes take effect at once, with no restart.</p></div>
<div class="card"><h2>Maintenance</h2>
<form method="post" action="/update" enctype="multipart/form-data" id="fu">
<label class="full">Firmware update (<code>.pio/build/sump/firmware.bin</code>)<input type="file" name="fw" accept=".bin" required></label>
<div class="btns"><button class="go" type="submit">Upload and install</button></div>
</form><form method="post" action="/restart" style="margin-top:10px"><div class="btns">
<button type="submit" onclick="return confirm('Restart the board now?')">Restart</button></div></form>
<p class="note">Both ask for <b>admin</b> and the admin password. A new firmware runs as a trial and rolls back by itself if it isn't back up with Wi-Fi and every sensor that worked before within 10 minutes. Uploads are refused while the pump is running.</p></div>
<div class="card"><h2>Log</h2><pre id="log"></pre></div>
</div>
</main><script>
const $=id=>document.getElementById(id),f=(v,d)=>v==null?'–':v.toFixed(d);
let hist=[],cfg=null,seq=0,filled=false,sug=null;
const q=new URLSearchParams(location.search);
if(q.has('saved'))$('saved').innerHTML='<p class="msg">Settings saved.</p>';
if(q.has('conn'))$('csaved').innerHTML='<p class="msg">Connections saved.</p>';
if(q.has('reset'))$('saved').innerHTML='<p class="msg">Settings reset to the firmware defaults.</p>';
if(q.has('dev'))$('dsaved').innerHTML='<p class="msg">Device saved.</p>';
// Calibration, settings, device, connections, maintenance and the log sit under one divider. It
// starts closed, except straight after a save so the confirmation shows.
function more(o){const b=$('more'),m=$('more-body');m.hidden=!o;b.setAttribute('aria-expanded',o);b.classList.toggle('open',o);
b.firstChild.textContent=(o?'Hide':'Show')+' more settings';if(o){const p=$('log');p.scrollTop=p.scrollHeight;}}
$('more').onclick=()=>more($('more-body').hidden);
more(q.has('saved')||q.has('conn')||q.has('reset')||q.has('dev'));
function draw(){const c=$('chart'),r=devicePixelRatio||1,w=c.clientWidth,h=c.clientHeight;c.width=w*r;c.height=h*r;
const g=c.getContext('2d');g.scale(r,r);const cs=getComputedStyle(document.documentElement),col=n=>cs.getPropertyValue(n);
let top=Math.max(cfg?cfg.on*1.6:1,...hist)*1.05;const y=v=>h-4-(v/top)*(h-8);
g.strokeStyle=col('--line');g.lineWidth=1;g.strokeRect(.5,.5,w-1,h-1);
if(cfg){g.strokeStyle=col('--warn');g.beginPath();g.moveTo(0,y(cfg.on));g.lineTo(w,y(cfg.on));g.stroke();
g.setLineDash([5,4]);g.beginPath();g.moveTo(0,y(cfg.off));g.lineTo(w,y(cfg.off));g.stroke();g.setLineDash([]);}
g.strokeStyle=col('--run');g.lineWidth=1.5;g.beginPath();const n=150;
hist.forEach((v,i)=>{const x=(i+n-hist.length)/(n-1)*w;i?g.lineTo(x,y(v)):g.moveTo(x,y(v));});g.stroke();
g.fillStyle=col('--dim');g.font='11px system-ui';g.fillText(top.toFixed(2),4,13);}
// Thresholds from the two 30 s measurements. Best case: every running reading is
// above every idle one (lowest running vs loudest idle). If running dips close to
// idle it falls back to the running average, and if idle bumps reach the running
// level to the idle average; the start and stop delays ride out short overlaps.
function suggest(ci,cr){if(!ci.n||!cr.n)return[null,'needs both measurements'];
let lo=ci.max,hi=cr.min,f=[.6,.3],note='';
if(hi<=lo*1.25){hi=cr.avg;f=[.5,.25];note=' (running dips close to idle; the stop delay rides them out)';
if(hi<=lo*1.5){lo=ci.avg;note=' (idle bumps reach the running level; the start and stop delays filter them)';}}
if(hi<=lo*1.5||hi-lo<0.03)return[null,'running is too close to idle: mount the sensor closer to the pump, or firmer'];
const on=lo+f[0]*(hi-lo),off=lo+f[1]*(hi-lo);return[{on:+on.toFixed(3),off:+off.toFixed(3)},'start '+on.toFixed(3)+' · stop '+off.toFixed(3)+note];}
async function status(){try{const s=await(await fetch('/api/status')).json();cfg=s.cfg;
document.title=s.device+' · Sump Master 2000';
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
const c=s.cal,busy=c.phase>0;
document.querySelectorAll('.cal').forEach(e=>{const mine=(e.dataset.p=='idle'?1:2)==c.phase;e.textContent=mine?'Stop':(e.dataset.p=='idle'?'Measure idle (30 s)':'Measure running (30 s)');e.disabled=busy&&!mine;e.dataset.stop=mine?1:'';});
$('calidle').textContent=c.phase==1?'measuring… '+c.left_s+' s left':c.idle.n?'average '+f(c.idle.avg,3)+' · peak '+f(c.idle.max,3):'not measured';
$('calrun').textContent=c.phase==2?'measuring… '+c.left_s+' s left':c.run.n?'average '+f(c.run.avg,3)+' · lowest '+f(c.run.min,3):'not measured';
const r=suggest(c.idle,c.run);sug=r[0];$('sugg').textContent=r[1];$('use').disabled=!sug||busy;
hist.push(s.rms_max);if(hist.length>150)hist.shift();draw();
if(!filled){filled=true;const F=$('fs'),k=s.cfg;F.on.value=k.on;F.off.value=k.off;F.run_s.value=k.run_ms/1000;F.stop_s.value=k.stop_ms/1000;
F.floor.value=k.floor_cm;F.high.value=k.high_cm;F.hyst.value=k.hyst_cm;F.drop.value=k.drop_cm;
const D=$('fd');D.name.value=s.device;D.tz.value=s.conn.tz;
const G=$('fc'),n=s.conn;G.tg_url.value=n.tg_url;G.mqtt_host.value=n.mqtt_host;G.mqtt_port.value=n.mqtt_port;G.mqtt_user.value=n.mqtt_user;
G.ntfy.placeholder=n.ntfy?'set; leave empty to keep it':'not set';G.mqtt_pass.placeholder=n.mqtt_pass?'set; leave empty to keep it':'not set';}
$('cstat').textContent='Wi-Fi "'+s.conn.wifi+'" · Telegraf '+(s.conn.tg_url?'on':'off')+' · ntfy '+(s.conn.ntfy?'on':'off')+' · Home Assistant (MQTT) '+s.conn.mqtt;
}catch(e){$('sub').textContent='not responding…';}}
document.querySelectorAll('.cal').forEach(e=>e.onclick=async()=>{await fetch('/calibrate',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'phase='+(e.dataset.stop?'stop':e.dataset.p)});status();});
$('use').onclick=()=>{if(!sug)return;const F=$('fs');F.on.value=sug.on;F.off.value=sug.off;F.scrollIntoView({behavior:'smooth'});F.on.focus();};
async function log(){try{const t=await(await fetch('/api/log?since='+seq)).text(),i=t.indexOf('\n');seq=+t.slice(0,i);
const body=t.slice(i+1);if(body){const p=$('log'),end=p.scrollTop+p.clientHeight>=p.scrollHeight-8;p.textContent+=body;if(end)p.scrollTop=p.scrollHeight;}}catch(e){}}
status();log();setInterval(status,2000);setInterval(log,5000);
</script></body></html>
)HTML";
