// Coldchain Guardian dashboard: wires a data source (live QNX target or the
// in-browser simulation) to the 3D scene, the readout and the proof chart.
import { Scene } from './scene.js';
import { Sim } from './sim.js';

const $ = (id) => document.getElementById(id);
const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { v == null ? localStorage.removeItem(k) : localStorage.setItem(k, v); } catch { /* private mode */ } },
};

// ---------------------------------------------------------------- sources
class LiveSource {
  constructor(base) { this.base = base.replace(/\/$/, ''); this.kind = 'live'; }
  start(onData, onState) {
    this.es = new EventSource(this.base + '/api/stream');
    this.es.onmessage = (e) => { try { onData(JSON.parse(e.data)); onState('live'); } catch { /* partial */ } };
    this.es.onerror = () => onState('lost');
  }
  stop() { this.es?.close(); }
  cmd(c) { return fetch(`${this.base}/api/cmd?do=${encodeURIComponent(c)}`, { method: 'POST' }).catch(() => {}); }
}

class SimSource {
  constructor() { this.kind = 'sim'; this.sim = new Sim(); }
  start(onData, onState) {
    let last = performance.now();
    onState('sim');
    this.timer = setInterval(() => {
      const now = performance.now();
      this.sim.step((now - last) / 1000); last = now;
      onData(this.sim.snapshot());
    }, 100);
  }
  stop() { clearInterval(this.timer); }
  cmd(c) { this.sim.cmd(c); return Promise.resolve(); }
}

async function probe(base, ms = 3000) {
  const ctl = new AbortController();
  const to = setTimeout(() => ctl.abort(), ms);
  try {
    const r = await fetch(base.replace(/\/$/, '') + '/api/state', { signal: ctl.signal });
    if (!r.ok) return false;
    const j = await r.json();
    return typeof j.temp === 'number';
  } catch { return false; } finally { clearTimeout(to); }
}

// ---------------------------------------------------------------- app
const scene = new Scene($('scene'), { onDoorClick: () => send('door_toggle') });
let src = null, last = null, seenEvents = -1;

function setLink(state) {
  const pill = $('linkPill');
  pill.classList.toggle('live', state === 'live');
  pill.classList.toggle('sim', state === 'sim');
  if (state === 'live') $('linkText').textContent = `LIVE · ${last?.platform ?? 'QNX target'}`;
  else if (state === 'sim') $('linkText').textContent = 'SIMULATION · connect a QNX target';
  else if (state === 'lost') $('linkText').textContent = 'reconnecting…';
}

function use(source) {
  src?.stop();
  src = source;
  seenEvents = -1;
  $('feed').innerHTML = '';
  chart.clear();
  src.start(onData, setLink);
}

async function boot() {
  const qp = new URLSearchParams(location.search).get('target');
  const saved = store.get('cc.target');
  const candidates = [];
  if (qp) candidates.push(qp);
  if (location.protocol.startsWith('http') && !/vercel\.app|github\.io|claude/.test(location.host)) candidates.push(location.origin);
  if (saved) candidates.push(saved);
  for (const c of candidates) {
    if (await probe(c)) { if (c !== location.origin) store.set('cc.target', c); return use(new LiveSource(c)); }
  }
  use(new SimSource());
}

function send(c) {
  if (!src) return;
  src.cmd(c);
}

// ---------------------------------------------------------------- readout
const fmtUs = (us) => (us >= 1000 ? `${(us / 1000).toFixed(us >= 10000 ? 0 : 1)} ms` : `${us.toFixed(us < 10 ? 1 : 0)} µs`);

function onData(d) {
  last = d;
  document.body.dataset.level = d.level;
  scene.update(d);

  $('temp').textContent = d.temp.toFixed(1);
  const pct = Math.max(0, Math.min(100, (d.temp / 12) * 100));
  $('bandMark').style.left = pct + '%';
  $('level').textContent = d.level;

  let ttb;
  if (d.level === 'ALARM') ttb = d.temp > 8 ? 'vials above 8 °C' : 'vials below 2 °C';
  else if (d.ttb >= 0) ttb = `breach in <b>${Math.round(d.ttb)} s</b>`;
  else if (d.backup) ttb = 'held by battery cooling';
  else ttb = 'holding steady';
  $('ttb').innerHTML = ttb;

  const sl = d.slope;
  $('slope').textContent = `${sl >= 0 ? '+' : ''}${sl.toFixed(2)} °C/min`;
  $('power').textContent = d.power ? 'grid' : d.backup ? 'battery' : 'none';
  $('comp').textContent = d.fault ? 'fault' : d.compressor ? (d.power ? 'cooling' : 'cooling · DC') : 'idle';
  $('battRow').classList.toggle('on', !d.power || d.battery < 99.5);
  $('battFill').style.width = d.battery.toFixed(0) + '%';
  $('batt').textContent = d.battery.toFixed(0) + '%';

  $('bDoor').querySelector('span').textContent = d.door ? 'Close door' : 'Open door';
  $('bDoor').classList.toggle('on', !!d.door);
  $('bPower').querySelector('span').textContent = d.power ? 'Cut grid power' : 'Restore power';
  $('bPower').classList.toggle('on', !d.power);
  $('bStress').querySelector('span').textContent = d.stress ? 'Stop CPU flood' : 'Flood CPU';
  $('bStress').classList.toggle('on', !!d.stress);

  const g = d.guardian, n = d.naive;
  $('gStat').textContent = `${fmtUs(g.win_max_us)} · ${g.misses} missed`;
  $('nStat').textContent = `${fmtUs(n.win_max_us)} · ${n.misses} missed`;
  $('nStat').classList.toggle('bad', n.win_max_us > 1000);
  $('proofNote').textContent = d.simulated
    ? 'SIMULATED timing — connect a QNX target for measured values'
    : `measured on ${d.platform} · worst wake-up jitter per second · log scale`;
  chart.push(g.win_max_us, n.win_max_us);

  // events
  const feed = $('feed');
  for (const e of d.events) {
    if (e.id <= seenEvents) continue;
    seenEvents = e.id;
    const li = document.createElement('li');
    li.className = e.l;
    li.innerHTML = `<time>${e.t.toFixed(1)}s</time><span></span>`;
    li.lastChild.textContent = e.m;
    feed.appendChild(li);
    while (feed.children.length > 6) feed.firstChild.remove();
  }
}

// ---------------------------------------------------------------- chart
const chart = (() => {
  const cv = $('chart'), ctx = cv.getContext('2d');
  const N = 300; let g = [], n = [];
  const css = (v) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();
  const y = (us, h) => {
    const lo = Math.log10(1), hi = Math.log10(200000);
    const v = Math.log10(Math.max(1, us));
    return h - 4 - ((v - lo) / (hi - lo)) * (h - 10);
  };
  function draw() {
    const dpr = Math.min(devicePixelRatio, 2), w = cv.clientWidth, h = cv.clientHeight;
    if (cv.width !== w * dpr) { cv.width = w * dpr; cv.height = h * dpr; }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.font = '10px "JetBrains Mono", monospace';
    // grid: 10 µs, 100 µs, 1 ms (deadline), 10 ms, 100 ms
    for (const [us, label] of [[10, '10µs'], [100, '100µs'], [1000, '1ms deadline'], [10000, '10ms'], [100000, '100ms']]) {
      const yy = y(us, h);
      ctx.strokeStyle = us === 1000 ? 'rgba(255,77,94,.45)' : 'rgba(255,255,255,.06)';
      ctx.setLineDash(us === 1000 ? [3, 4] : []);
      ctx.beginPath(); ctx.moveTo(0, yy); ctx.lineTo(w, yy); ctx.stroke();
      ctx.fillStyle = us === 1000 ? 'rgba(255,77,94,.7)' : 'rgba(255,255,255,.25)';
      ctx.fillText(label, w - ctx.measureText(label).width - 2, yy - 3);
    }
    ctx.setLineDash([]);
    const line = (arr, color, glow) => {
      if (arr.length < 2) return;
      ctx.beginPath();
      arr.forEach((v, i) => { const xx = (i / (N - 1)) * w; const yy = y(v, h); i ? ctx.lineTo(xx, yy) : ctx.moveTo(xx, yy); });
      ctx.strokeStyle = color; ctx.lineWidth = 1.5; ctx.shadowColor = color; ctx.shadowBlur = glow; ctx.stroke(); ctx.shadowBlur = 0;
    };
    line(n, css('--naive'), 0);
    line(g, css('--safe'), 8);
  }
  return {
    push(a, b) { g.push(a); n.push(b); if (g.length > N) { g.shift(); n.shift(); } draw(); },
    clear() { g = []; n = []; draw(); },
    draw,
  };
})();
addEventListener('resize', () => chart.draw());

// ---------------------------------------------------------------- drill
const DRILL = [
  [0, 'reset', 'Kano, 2:14 pm. 32 °C outside.', 'A clinic fridge holds 63 vials at 2–8 °C.'],
  [5, 'power_off', 'The grid drops.', 'The guardian projects when the vials will breach 8 °C.'],
  [24, null, 'It acts before the breach.', 'Battery cooling engages with ~60 s to spare.'],
  [34, 'stress_on', 'Now flood the CPU.', 'Busy threads at priority 10. Watch the outer orbit stall.'],
  [46, 'door_open', 'A nurse leaves the door open.', 'Cold air spills. The guardian is still on time.'],
  [60, 'door_close', 'Door closed.', 'Guardian: 0 deadlines missed. Naive monitor: starved.'],
  [70, 'stress_off', 'Load gone.', ''],
  [75, 'power_on', 'Grid restored.', 'Battery released, compressor back on mains.'],
  [84, null, '', ''],
];
let drillTimers = [];
function caption(title, sub) {
  const el = $('caption');
  if (!title) { el.classList.remove('show'); return; }
  el.innerHTML = `<small>BLACKOUT DRILL</small><span></span>`;
  el.lastChild.textContent = title + (sub ? ' ' + sub : '');
  el.classList.add('show');
}
function runDrill() {
  drillTimers.forEach(clearTimeout);
  drillTimers = DRILL.map(([at, cmd, title, sub]) => setTimeout(() => { if (cmd) send(cmd); caption(title, sub); }, at * 1000));
}

// ---------------------------------------------------------------- input
document.querySelectorAll('.controls button').forEach((b) =>
  b.addEventListener('click', () => (b.dataset.cmd === 'drill' ? runDrill() : (b.dataset.cmd === 'reset' && (drillTimers.forEach(clearTimeout), caption()), send(b.dataset.cmd)))));

addEventListener('keydown', (e) => {
  if (e.target.closest('input, dialog') || e.metaKey || e.ctrlKey) return;
  const k = e.key.toLowerCase();
  if (k === 'd') send('door_toggle');
  else if (k === 'p') send('power_toggle');
  else if (k === 'f') send('stress_toggle');
  else if (k === 'r') { drillTimers.forEach(clearTimeout); caption(); send('reset'); }
  else if (k === ' ') { e.preventDefault(); runDrill(); }
});

$('linkPill').addEventListener('click', () => { $('linkUrl').value = store.get('cc.target') || ''; $('linkDlg').showModal(); });
$('linkDlg').addEventListener('close', async () => {
  const v = $('linkDlg').returnValue;
  if (v === 'sim') { store.set('cc.target', null); use(new SimSource()); return; }
  if (v === 'live') {
    let url = $('linkUrl').value.trim();
    if (!url) return;
    if (!/^https?:\/\//.test(url)) url = 'http://' + url;
    $('linkText').textContent = 'connecting…';
    if (await probe(url, 2500)) { store.set('cc.target', url); use(new LiveSource(url)); }
    else { $('linkText').textContent = 'target unreachable — still simulating'; }
  }
});

setTimeout(() => ($('hint').style.opacity = 0), 9000);
boot();
