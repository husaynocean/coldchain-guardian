// Browser-side simulation used when no QNX target is connected.
// It mirrors src/plant.c and src/guardian.c so the scenario behaves the
// same; the timing numbers here are SIMULATED and are labelled as such.

const K_LEAK = 0.0016, K_DOOR = 0.012, COOL_MAINS = 0.09, COOL_BACKUP = 0.06;
const TSTAT_ON = 5.0, TSTAT_OFF = 3.5, BATT_DRAIN = 0.45, BATT_CHARGE = 0.3;
const HIST_N = 40, BACKUP_LEAD_S = 60, DOOR_BUZZ_S = 15;

const timing = () => ({ ticks: 0, period_us: 5000, jitter_us: 0, max_jitter_us: 0, win_max_us: 0,
  exec_us: 0, exec_max_us: 0, misses: 0, hist: [0, 0, 0, 0, 0, 0, 0, 0], _cur: 0, _prev: 0 });

export class Sim {
  constructor() { this.t0 = performance.now(); this.reset(true); }

  reset(first = false) {
    this.T = 4.6; this.amb = 32; this.door = 0; this.door_s = 0; this.power = 1; this.comp = 0;
    this.backup = 0; this.fault = 0; this.batt = 100; this.stress = 0;
    this.level = 'SAFE'; this.slope = 0; this.ttb = -1; this.buzzer = 0;
    this.hist = []; this.g = timing(); this.n = timing();
    if (first) { this.events = []; this.evId = 0; this.ev('g', 'guardian online: prio 60, 5 ms period (simulated)'); }
  }

  now() { return (performance.now() - this.t0) / 1000; }
  ev(l, m) { this.events.push({ id: this.evId++, t: this.now(), l, m }); if (this.events.length > 10) this.events.shift(); }

  cmd(c) {
    const map = {
      door_open: () => (this.door = 1), door_close: () => (this.door = 0), door_toggle: () => (this.door ^= 1),
      power_off: () => (this.power = 0), power_on: () => (this.power = 1), power_toggle: () => (this.power ^= 1),
      fault_on: () => (this.fault = 1), fault_off: () => (this.fault = 0),
      stress_on: () => this.setStress(1), stress_off: () => this.setStress(0), stress_toggle: () => this.setStress(this.stress ^ 1),
      reset: () => this.reset(), reset_stats: () => { this.g = timing(); this.n = timing(); },
    };
    if (!map[c]) return false;
    map[c]();
    if (c !== 'reset_stats') this.ev('i', `command: ${c}`);
    return true;
  }

  setStress(on) {
    if (on === this.stress) return;
    this.stress = on;
    this.stallAt = on ? this.now() : null;
    if (!on) { this.n._cur = 0; this.n._prev = 0; this.n.win_max_us = 0; }
    this.ev(on ? 'w' : 'i', on ? 'CPU flood: 3 busy threads at prio 11' : 'CPU flood stopped');
  }

  // advance the plant by dt seconds in 10 ms steps, the guardian every 100 ms
  step(dt) {
    let left = Math.min(dt, 0.5);
    while (left > 0) {
      const h = Math.min(0.01, left); left -= h;
      const canMains = this.power && !this.fault, canBackup = !this.power && this.backup && this.batt > 0;
      if (canMains || canBackup) { if (this.T > TSTAT_ON) this.comp = 1; if (this.T < TSTAT_OFF) this.comp = 0; } else this.comp = 0;
      let dT = K_LEAK * (this.amb - this.T);
      if (this.door) dT += K_DOOR * (this.amb - this.T);
      if (this.comp) dT -= canMains ? COOL_MAINS : COOL_BACKUP;
      this.T = Math.min(this.amb, Math.max(-4, this.T + dT * h));
      this.door_s = this.door ? this.door_s + h : 0;
      if (canBackup && this.comp) { this.batt -= BATT_DRAIN * h; if (this.batt <= 0) { this.batt = 0; this.backup = 0; } }
      if (this.power && this.batt < 100) this.batt = Math.min(100, this.batt + BATT_CHARGE * h);
      this._acc = (this._acc || 0) + h;
      if (this._acc >= 0.1) { this._acc -= 0.1; this.guard(); }
      this.tickTiming(h);
    }
  }

  guard() {
    const T = this.T + (Math.random() - Math.random()) * 0.012;
    this.hist.push(T); if (this.hist.length > HIST_N) this.hist.shift();
    const n = this.hist.length;
    if (n >= 10) {
      let sx = 0, sy = 0, sxx = 0, sxy = 0;
      this.hist.forEach((y, i) => { const x = i * 0.1; sx += x; sy += y; sxx += x * x; sxy += x * y; });
      const d = n * sxx - sx * sx; this.slope = d ? (n * sxy - sx * sy) / d : 0;
    }
    const coolingOk = (this.power && !this.fault) || this.backup;
    const runaway = !coolingOk || this.door || T > 5.4;
    this.ttb = -1;
    if (T <= 8 && this.slope > 0.002 && runaway) this.ttb = (8 - T) / this.slope;
    else if (T >= 2 && T < 2.8 && this.slope < -0.002) this.ttb = (T - 2) / -this.slope;

    let nl = 'SAFE';
    if (T > 8 || T < 2) nl = 'ALARM';
    else if ((this.ttb >= 0 && this.ttb < 120) || this.door_s > DOOR_BUZZ_S || !this.power) nl = 'WATCH';
    if (nl !== this.level) { this.ev(nl === 'ALARM' ? 'a' : nl === 'WATCH' ? 'w' : 'i', `${this.level} → ${nl} at ${T.toFixed(2)} °C`); this.level = nl; }

    if (!this.power && !this.backup && this.batt > 1 && ((this.ttb >= 0 && this.ttb < BACKUP_LEAD_S) || T > 6.5)) {
      this.backup = 1; this.ev('g', `grid lost — battery cooling engaged ${this.ttb.toFixed(0)} s before breach`);
    }
    if (this.power && this.backup) { this.backup = 0; this.ev('g', 'grid restored — battery cooling released'); }
    const buzz = this.door_s > DOOR_BUZZ_S ? 1 : 0;
    if (buzz !== this.buzzer) { this.buzzer = buzz; if (buzz) this.ev('g', 'door open > 15 s — buzzer on, pharmacist alerted'); }
    this.temp = T;
  }

  // synthetic timing so the visuals behave like the real thing (clearly labelled "simulated")
  tickTiming(h) {
    const ticks = Math.round(h / 0.005);
    const upd = (t, j) => {
      const edges = [10, 25, 50, 100, 250, 1000, 5000]; let b = 0; while (b < 7 && j >= edges[b]) b++;
      t.hist[b]++; t.ticks++; t.jitter_us = j; t.max_jitter_us = Math.max(t.max_jitter_us, j);
      if (j > 1000) t.misses++;
      t._cur = Math.max(t._cur, j); if (t.ticks % 200 === 0) { t._prev = t._cur; t._cur = 0; }
      t.win_max_us = Math.max(t._cur, t._prev);
    };
    for (let i = 0; i < ticks; i++) {
      upd(this.g, 1.5 + Math.random() * (this.stress ? 9 : 6));
      // under the flood the naive loop is outranked and never runs
      if (!this.stress) upd(this.n, 3 + Math.random() * 30 + (Math.random() < 0.01 ? 200 : 0));
    }
  }

  snapshot() {
    const strip = ({ _cur, _prev, ...t }) => t;
    const naive = strip(this.n);
    if (this.stress && this.stallAt != null) {
      const us = (this.now() - this.stallAt) * 1e6;
      naive.stalled_us = us; naive.win_max_us = Math.max(naive.win_max_us, us); naive.misses += Math.floor(us / 5000);
    }
    return {
      v: 'sim', platform: 'browser simulation', qnx: false, simulated: true, t: this.now(),
      temp: this.temp ?? this.T, ambient: this.amb, door: this.door, door_s: this.door_s, power: this.power,
      compressor: this.comp, backup: this.backup, fault: this.fault, battery: this.batt,
      level: this.level, slope: this.slope * 60, ttb: this.ttb, buzzer: this.buzzer,
      stress: this.stress, stress_threads: this.stress ? 3 : 0, cpus: 2,
      guardian: strip(this.g), naive, events: this.events.slice(),
    };
  }
}
