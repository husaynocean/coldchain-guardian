// The 3D scene: a glass vaccine refrigerator, its cold air, and two orbits
// that make the real-time scheduling visible.
//   inner cyan orbit  — the guardian (prio 60): advances by its real tick count
//   outer grey orbit  — the naive monitor (prio 10): stalls when starved
import * as THREE from 'three';
import { OrbitControls } from 'three/addons/controls/OrbitControls.js';
import { EffectComposer } from 'three/addons/postprocessing/EffectComposer.js';
import { RenderPass } from 'three/addons/postprocessing/RenderPass.js';
import { UnrealBloomPass } from 'three/addons/postprocessing/UnrealBloomPass.js';
import { OutputPass } from 'three/addons/postprocessing/OutputPass.js';

const C = {
  safe: new THREE.Color('#59e3ff'), watch: new THREE.Color('#ffb347'), alarm: new THREE.Color('#ff4d5e'),
  frozen: new THREE.Color('#8b7bff'), naive: new THREE.Color('#9aa3b2'), off: new THREE.Color('#1c2430'),
};
const FW = 1.3, FH = 2.1, FD = 0.9, BASE = 0.14;   // fridge size, plinth height
const CY = BASE + FH / 2;

export function tempColor(t, out = new THREE.Color()) {
  if (t < 2) return out.copy(C.frozen).lerp(C.safe, Math.max(0, (t + 2) / 4));
  if (t < 5) return out.copy(C.safe);
  if (t < 8) return out.copy(C.safe).lerp(C.watch, (t - 5) / 3);
  return out.copy(C.watch).lerp(C.alarm, Math.min(1, (t - 8) / 2));
}

export class Scene {
  constructor(canvas, { onDoorClick } = {}) {
    this.canvas = canvas;
    this.onDoorClick = onDoorClick;
    this.state = { temp: 4.6, door: 0, power: 1, compressor: 0, backup: 0, battery: 100, level: 'SAFE', stress: 0 };
    this.ticks = { g: 0, n: 0, gShown: 0, nShown: 0, gBase: null, nBase: null };
    this.doorAngle = 0;
    this.flicker = 0;
    this.t0 = performance.now();

    const r = (this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true, powerPreference: 'high-performance' }));
    r.setPixelRatio(Math.min(devicePixelRatio, 2));
    r.toneMapping = THREE.ACESFilmicToneMapping;
    r.toneMappingExposure = 1.0;

    const s = (this.scene = new THREE.Scene());
    s.background = new THREE.Color('#04060a');
    s.fog = new THREE.FogExp2('#04060a', 0.085);

    this.camera = new THREE.PerspectiveCamera(32, 1, 0.1, 100);
    this.camera.position.set(6.2, 3.6, 8.4);          // intro starts far; eases in
    this.camHome = new THREE.Vector3(3.5, 2.05, 4.7);

    const ctl = (this.controls = new OrbitControls(this.camera, canvas));
    ctl.target.set(0, CY - 0.1, 0);
    ctl.enableDamping = true; ctl.dampingFactor = 0.06; ctl.enablePan = false;
    ctl.minDistance = 3.6; ctl.maxDistance = 10; ctl.maxPolarAngle = 1.5; ctl.minPolarAngle = 0.35;
    ctl.autoRotate = true; ctl.autoRotateSpeed = 0.35; ctl.enabled = false;
    let idle;
    ctl.addEventListener('start', () => { ctl.autoRotate = false; clearTimeout(idle); });
    ctl.addEventListener('end', () => { idle = setTimeout(() => (ctl.autoRotate = true), 9000); });

    s.add(new THREE.AmbientLight('#8fa6c8', 0.35));
    const key = new THREE.DirectionalLight('#cfe6ff', 0.9); key.position.set(3, 6, 4); s.add(key);
    this.inner = new THREE.PointLight('#bfefff', 2.2, 3.2, 1.6); this.inner.position.set(0, BASE + FH - 0.25, 0.1); s.add(this.inner);

    this.buildFloor();
    this.buildFridge();
    this.buildMist();
    this.buildOrbits();
    this.buildFlood();

    const comp = (this.composer = new EffectComposer(r));
    comp.addPass(new RenderPass(s, this.camera));
    this.bloom = new UnrealBloomPass(new THREE.Vector2(1, 1), 0.85, 0.5, 0.12);
    comp.addPass(this.bloom);
    comp.addPass(new OutputPass());

    this.ray = new THREE.Raycaster();
    this.ptr = new THREE.Vector2();
    let down = null;
    canvas.addEventListener('pointerdown', (e) => (down = [e.clientX, e.clientY]));
    canvas.addEventListener('pointerup', (e) => {
      if (down && Math.hypot(e.clientX - down[0], e.clientY - down[1]) < 5 && this.hitDoor(e)) this.onDoorClick?.();
      down = null;
    });
    canvas.addEventListener('pointermove', (e) => { canvas.style.cursor = this.hitDoor(e) ? 'pointer' : ''; });

    addEventListener('resize', () => this.resize());
    this.resize();
    this.clock = new THREE.Clock();
    r.setAnimationLoop(() => this.frame());
  }

  hitDoor(e) {
    const b = this.canvas.getBoundingClientRect();
    this.ptr.set(((e.clientX - b.left) / b.width) * 2 - 1, -((e.clientY - b.top) / b.height) * 2 + 1);
    this.ray.setFromCamera(this.ptr, this.camera);
    return this.ray.intersectObject(this.doorPane, false).length > 0;
  }

  resize() {
    const w = innerWidth, h = innerHeight;
    this.renderer.setSize(w, h, false);
    this.composer.setSize(w, h);
    this.bloom.setSize(w, h);
    this.camera.aspect = w / h;
    // keep the fridge framed on narrow screens
    this.camera.fov = w < 700 ? 50 : 32;
    this.camera.updateProjectionMatrix();
  }

  // ------------------------------------------------------------------ build
  buildFloor() {
    const cv = document.createElement('canvas'); cv.width = cv.height = 512;
    const g = cv.getContext('2d');
    const grd = g.createRadialGradient(256, 256, 0, 256, 256, 256);
    grd.addColorStop(0, 'rgba(89,227,255,0.10)'); grd.addColorStop(0.35, 'rgba(89,227,255,0.03)'); grd.addColorStop(1, 'rgba(0,0,0,0)');
    g.fillStyle = grd; g.fillRect(0, 0, 512, 512);
    g.strokeStyle = 'rgba(160,200,255,0.06)'; g.lineWidth = 1;
    for (let i = 0; i <= 512; i += 32) { g.beginPath(); g.moveTo(i, 0); g.lineTo(i, 512); g.stroke(); g.beginPath(); g.moveTo(0, i); g.lineTo(512, i); g.stroke(); }
    const tex = new THREE.CanvasTexture(cv);
    this.floorMat = new THREE.MeshBasicMaterial({ map: tex, transparent: true, depthWrite: false });
    const floor = new THREE.Mesh(new THREE.PlaneGeometry(12, 12), this.floorMat);
    floor.rotation.x = -Math.PI / 2;
    this.scene.add(floor);
  }

  buildFridge() {
    const grp = (this.fridge = new THREE.Group());
    this.scene.add(grp);

    // plinth
    const plinth = new THREE.Mesh(new THREE.BoxGeometry(FW + 0.08, BASE, FD + 0.08),
      new THREE.MeshStandardMaterial({ color: '#0b1018', roughness: 0.6, metalness: 0.4 }));
    plinth.position.y = BASE / 2; grp.add(plinth);

    // glowing status strip on the plinth front (battery gauge when on backup)
    this.strip = new THREE.Mesh(new THREE.BoxGeometry(FW - 0.2, 0.012, 0.01), new THREE.MeshBasicMaterial({ color: C.safe, toneMapped: false }));
    this.strip.position.set(0, BASE * 0.55, FD / 2 + 0.045); grp.add(this.strip);

    // glass shell (no front face — that's the door)
    this.glassMat = new THREE.MeshBasicMaterial({ color: '#a8dcff', transparent: true, opacity: 0.045,
      side: THREE.DoubleSide, depthWrite: false });
    const face = (w, h, x, y, z, ry = 0, rx = 0) => {
      const m = new THREE.Mesh(new THREE.PlaneGeometry(w, h), this.glassMat);
      m.position.set(x, y, z); m.rotation.set(rx, ry, 0); grp.add(m);
    };
    face(FW, FH, 0, CY, -FD / 2);
    face(FD, FH, -FW / 2, CY, 0, Math.PI / 2);
    face(FD, FH, FW / 2, CY, 0, Math.PI / 2);
    face(FW, FD, 0, BASE + FH, 0, 0, Math.PI / 2);

    this.edgeMat = new THREE.LineBasicMaterial({ color: C.safe, transparent: true, opacity: 0, toneMapped: false });
    const edges = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(FW, FH, FD)), this.edgeMat);
    edges.position.y = CY; grp.add(edges);

    // shelves
    const shelfMat = new THREE.MeshBasicMaterial({ color: '#7fd8ff', transparent: true, opacity: 0.08, side: THREE.DoubleSide, depthWrite: false });
    this.shelfLineMat = new THREE.LineBasicMaterial({ color: C.safe, transparent: true, opacity: 0.25, toneMapped: false });
    this.shelfY = [0.42, 1.0, 1.58].map((f) => BASE + f);
    for (const y of this.shelfY) {
      const p = new THREE.Mesh(new THREE.PlaneGeometry(FW - 0.06, FD - 0.06), shelfMat);
      p.rotation.x = -Math.PI / 2; p.position.y = y; grp.add(p);
      const l = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.PlaneGeometry(FW - 0.06, FD - 0.06)), this.shelfLineMat);
      l.rotation.x = -Math.PI / 2; l.position.y = y; grp.add(l);
    }

    // vials: 3 shelves x 7 columns x 3 rows
    const cols = 7, rows = 3, n = this.shelfY.length * cols * rows;
    const vialGeo = new THREE.CylinderGeometry(0.036, 0.036, 0.17, 14);
    vialGeo.translate(0, 0.085, 0);
    this.vialMat = new THREE.MeshBasicMaterial({ color: '#ffffff', transparent: true, opacity: 0.9 });
    this.vials = new THREE.InstancedMesh(vialGeo, this.vialMat, n);
    const capGeo = new THREE.CylinderGeometry(0.039, 0.039, 0.035, 14); capGeo.translate(0, 0.19, 0);
    const caps = new THREE.InstancedMesh(capGeo, new THREE.MeshStandardMaterial({ color: '#c9d3e0', roughness: 0.4, metalness: 0.6 }), n);
    this.vialFront = new Float32Array(n);
    this.vialJitter = new Float32Array(n);
    const m = new THREE.Matrix4();
    let i = 0;
    for (const y of this.shelfY) for (let r = 0; r < rows; r++) for (let c = 0; c < cols; c++) {
      const x = -FW / 2 + 0.16 + c * ((FW - 0.32) / (cols - 1));
      const z = -FD / 2 + 0.2 + r * ((FD - 0.4) / (rows - 1));
      m.makeTranslation(x, y + 0.003, z);
      this.vials.setMatrixAt(i, m); caps.setMatrixAt(i, m);
      this.vialFront[i] = r / (rows - 1);                  // 1 = nearest the door
      this.vialJitter[i] = (Math.random() - 0.5) * 0.35;
      this.vials.setColorAt(i, C.safe);
      i++;
    }
    grp.add(this.vials, caps);

    // compressor glow at the back
    this.compMat = new THREE.MeshBasicMaterial({ color: C.safe, transparent: true, opacity: 0.0, toneMapped: false });
    const comp = new THREE.Mesh(new THREE.BoxGeometry(FW - 0.3, 0.05, 0.02), this.compMat);
    comp.position.set(0, BASE + 0.12, -FD / 2 + 0.03); grp.add(comp);

    // door: hinged at the left front edge
    this.doorPivot = new THREE.Group();
    this.doorPivot.position.set(-FW / 2, CY, FD / 2);
    grp.add(this.doorPivot);
    this.doorMat = new THREE.MeshBasicMaterial({ color: '#bfe8ff', transparent: true, opacity: 0.07,
      side: THREE.DoubleSide, depthWrite: false });
    this.doorPane = new THREE.Mesh(new THREE.PlaneGeometry(FW, FH), this.doorMat);
    this.doorPane.position.x = FW / 2;
    this.doorEdge = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.PlaneGeometry(FW, FH)), this.edgeMat);
    this.doorEdge.position.x = FW / 2;
    this.handleMat = new THREE.MeshBasicMaterial({ color: C.safe, toneMapped: false });
    const handle = new THREE.Mesh(new THREE.BoxGeometry(0.018, 0.55, 0.03), this.handleMat);
    handle.position.set(FW - 0.08, 0.15, 0.03);
    this.doorPivot.add(this.doorPane, this.doorEdge, handle);
  }

  buildMist() {
    const N = 520;
    const pos = new Float32Array(N * 3);
    this.mistVel = new Float32Array(N * 3);
    this.mistN = N;
    for (let i = 0; i < N; i++) this.spawnMist(pos, i, true);
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    const dot = document.createElement('canvas'); dot.width = dot.height = 64;
    const g = dot.getContext('2d');
    const grd = g.createRadialGradient(32, 32, 0, 32, 32, 32);
    grd.addColorStop(0, 'rgba(255,255,255,1)'); grd.addColorStop(0.4, 'rgba(255,255,255,.35)'); grd.addColorStop(1, 'rgba(255,255,255,0)');
    g.fillStyle = grd; g.fillRect(0, 0, 64, 64);
    this.mistMat = new THREE.PointsMaterial({ size: 0.075, map: new THREE.CanvasTexture(dot), color: C.safe, transparent: true,
      opacity: 0.55, depthWrite: false, blending: THREE.AdditiveBlending, toneMapped: false });
    this.mist = new THREE.Points(geo, this.mistMat);
    this.fridge.add(this.mist);
  }

  spawnMist(pos, i, anywhere) {
    const j = i * 3;
    pos[j] = (Math.random() - 0.5) * (FW - 0.12);
    pos[j + 1] = anywhere ? BASE + 0.05 + Math.random() * (FH - 0.1) : BASE + FH - 0.06;
    pos[j + 2] = (Math.random() - 0.5) * (FD - 0.12);
    this.mistVel[j] = (Math.random() - 0.5) * 0.02;
    this.mistVel[j + 1] = -0.05 - Math.random() * 0.06;
    this.mistVel[j + 2] = (Math.random() - 0.5) * 0.02;
  }

  buildOrbits() {
    const ring = (r, color, opacity, dashed) => {
      const pts = [];
      for (let i = 0; i <= 256; i++) { const a = (i / 256) * Math.PI * 2; pts.push(new THREE.Vector3(Math.cos(a) * r, 0.005, Math.sin(a) * r)); }
      const geo = new THREE.BufferGeometry().setFromPoints(pts);
      const mat = dashed
        ? new THREE.LineDashedMaterial({ color, transparent: true, opacity, dashSize: 0.05, gapSize: 0.06, toneMapped: false })
        : new THREE.LineBasicMaterial({ color, transparent: true, opacity, toneMapped: false });
      const l = new THREE.Line(geo, mat); if (dashed) l.computeLineDistances();
      this.scene.add(l); return l;
    };
    this.gRing = ring(1.25, C.safe, 0.22, false);
    this.nRing = ring(1.55, C.naive, 0.18, true);
    const orb = (color, size) => {
      const m = new THREE.Mesh(new THREE.SphereGeometry(size, 16, 12), new THREE.MeshBasicMaterial({ color, toneMapped: false }));
      this.scene.add(m); return m;
    };
    this.gOrb = orb(C.safe, 0.028);
    this.nOrb = orb(C.naive, 0.024);
    // short comet tails
    const tail = (color) => {
      const N = 24, geo = new THREE.BufferGeometry();
      geo.setAttribute('position', new THREE.BufferAttribute(new Float32Array(N * 3), 3));
      const l = new THREE.Line(geo, new THREE.LineBasicMaterial({ color, transparent: true, opacity: 0.5, toneMapped: false }));
      this.scene.add(l); return l;
    };
    this.gTail = tail(C.safe); this.nTail = tail(C.naive);
  }

  buildFlood() {
    // the "CPU flood": a restless swarm in the outer field
    const N = 360, pos = new Float32Array(N * 3);
    this.flood = { N, seed: Float32Array.from({ length: N * 3 }, () => Math.random()) };
    const geo = new THREE.BufferGeometry();
    geo.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    this.floodMat = new THREE.PointsMaterial({ size: 0.022, color: '#ff6b7a', transparent: true, opacity: 0, depthWrite: false,
      blending: THREE.AdditiveBlending, toneMapped: false });
    this.floodPts = new THREE.Points(geo, this.floodMat);
    this.scene.add(this.floodPts);
  }

  // ------------------------------------------------------------------ data
  update(d) {
    const prevPower = this.state.power;
    this.state = d;
    if (prevPower && !d.power) this.flicker = 1;
    const T = this.ticks;
    if (T.gBase === null || d.guardian.ticks < T.g) { T.gBase = d.guardian.ticks; T.nBase = d.naive.ticks; T.gShown = 0; T.nShown = 0; }
    T.g = d.guardian.ticks; T.n = d.naive.ticks;
  }

  // ------------------------------------------------------------------ frame
  frame() {
    const dt = Math.min(this.clock.getDelta(), 0.05);
    const t = (performance.now() - this.t0) / 1000;
    const s = this.state;
    const lerp = (a, b, k) => a + (b - a) * Math.min(1, k * dt);

    // intro: ease the camera in, then hand over to the user
    const intro = Math.min(1, t / 3.2);
    if (intro < 1) {
      const e = 1 - Math.pow(1 - intro, 3);
      this.camera.position.lerpVectors(new THREE.Vector3(6.2, 3.6, 8.4), this.camHome, e);
    } else if (!this.controls.enabled) this.controls.enabled = true;
    this.edgeMat.opacity = Math.min(1, t / 1.6) * (s.power || s.backup ? 0.95 : 0.45);

    // state colour
    const target = s.level === 'ALARM' ? C.alarm : s.level === 'WATCH' ? C.watch : C.safe;
    const pulse = s.level === 'ALARM' ? 0.65 + 0.35 * Math.sin(t * 9) : 1;
    let ec = this.edgeMat.color.lerp(target, Math.min(1, 3 * dt));
    if (this.flicker > 0) {               // grid loss: a short electrical stutter
      this.flicker = Math.max(0, this.flicker - dt * 1.4);
      this.edgeMat.opacity *= Math.random() < 0.5 ? 0.15 : 1;
    }
    this.edgeMat.opacity *= pulse;
    this.handleMat.color.copy(ec);
    this.shelfLineMat.color.copy(ec);
    this.floorMat.color.copy(new THREE.Color(1, 1, 1).lerp(ec, 0.6));

    // interior light follows power
    const powered = s.power || (s.backup && s.compressor);
    this.inner.intensity = lerp(this.inner.intensity, s.power ? 2.2 : s.backup ? 0.9 : 0.15, 4);
    this.inner.color.copy(tempColor(s.temp, new THREE.Color())).lerp(new THREE.Color('#ffffff'), 0.6);

    // door swing (ease, slight overshoot feel)
    const want = s.door ? 1.3 : 0;
    this.doorAngle = lerp(this.doorAngle, want, 5);
    this.doorPivot.rotation.y = -this.doorAngle;

    // vials: colour by temperature, front rows warm first when the door is open
    const tc = new THREE.Color();
    const open = this.doorAngle / 1.3;
    for (let i = 0; i < this.vials.count; i++) {
      const local = s.temp + this.vialJitter[i] * 0.4 + open * this.vialFront[i] * 1.6;
      tempColor(local, tc);
      tc.multiplyScalar(powered ? 0.62 : 0.4);
      this.vials.setColorAt(i, tc);
    }
    this.vials.instanceColor.needsUpdate = true;

    // compressor + status strip
    this.compMat.opacity = lerp(this.compMat.opacity, s.compressor ? 0.55 + 0.25 * Math.sin(t * 6) : 0.0, 6);
    this.compMat.color.copy(s.backup ? C.watch : C.safe);
    const onBackup = !s.power;
    this.strip.material.color.copy(onBackup ? C.watch : ec);
    this.strip.scale.x = lerp(this.strip.scale.x, onBackup ? Math.max(0.02, s.battery / 100) : 1, 4);
    this.strip.position.x = -(FW - 0.2) * (1 - this.strip.scale.x) / 2;

    this.animMist(dt, s, open);
    this.animOrbits(dt, t, s);
    this.animFlood(t, s);

    this.controls.update();
    this.composer.render();
  }

  animMist(dt, s, open) {
    const pos = this.mist.geometry.attributes.position.array, v = this.mistVel;
    const cold = Math.max(0, Math.min(1, (9 - s.temp) / 6));
    this.mistMat.opacity = 0.12 + 0.5 * cold;
    tempColor(s.temp, this.mistMat.color);
    for (let i = 0; i < this.mistN; i++) {
      const j = i * 3;
      // cold air pours out of an open door and falls
      if (open > 0.3 && pos[j + 2] > FD / 2 - 0.35 && pos[j + 1] < BASE + FH * 0.75) {
        v[j + 2] += 0.35 * open * dt; v[j + 1] -= 0.25 * dt;
      }
      v[j] += (Math.random() - 0.5) * 0.03 * dt;
      pos[j] += v[j] * dt; pos[j + 1] += v[j + 1] * dt; pos[j + 2] += v[j + 2] * dt;
      const inside = Math.abs(pos[j]) < FW / 2 - 0.04 && Math.abs(pos[j + 2]) < FD / 2 - 0.04;
      if (pos[j + 1] < 0.02 || pos[j + 2] > FD / 2 + 1.4 || (!inside && pos[j + 2] < FD / 2) || (inside && pos[j + 1] < BASE + 0.03 && Math.random() < 0.08))
        this.spawnMist(pos, i, false);
      if (inside && pos[j + 2] < FD / 2 && Math.abs(pos[j]) > FW / 2 - 0.06) v[j] *= -1;
    }
    this.mist.geometry.attributes.position.needsUpdate = true;
  }

  animOrbits(dt, t, s) {
    const T = this.ticks;
    // move each orb toward its *measured* tick count (200 ticks = one revolution).
    // The guardian keeps pace; the naive monitor visibly stalls when starved.
    const step = (shown, real) => {
      const gap = real - shown;
      if (gap <= 0) return shown;
      return shown + Math.min(gap, Math.max(gap * 6 * dt, 200 * dt));
    };
    T.gShown = step(T.gShown, T.g - (T.gBase ?? T.g));
    T.nShown = step(T.nShown, T.n - (T.nBase ?? T.n));
    const place = (orb, tail, ticks, r) => {
      const a = (ticks / 200) * Math.PI * 2;
      orb.position.set(Math.cos(a) * r, 0.02, Math.sin(a) * r);
      // tail = fixed arc behind the orb, independent of frame rate
      const arr = tail.geometry.attributes.position.array;
      for (let i = 0; i < 24; i++) {
        const b = a - (i / 23) * 0.9;
        arr[i * 3] = Math.cos(b) * r; arr[i * 3 + 1] = 0.02; arr[i * 3 + 2] = Math.sin(b) * r;
      }
      tail.geometry.attributes.position.needsUpdate = true;
    };
    place(this.gOrb, this.gTail, T.gShown, 1.25);
    place(this.nOrb, this.nTail, T.nShown, 1.55);
    const starving = s.naive && s.naive.win_max_us > 1000;
    this.nOrb.material.color.lerp(starving ? C.alarm : C.naive, Math.min(1, 4 * dt));
    this.nTail.material.color.copy(this.nOrb.material.color);
    this.nRing.material.color.copy(this.nOrb.material.color);
  }

  animFlood(t, s) {
    this.floodMat.opacity += ((s.stress ? 0.85 : 0) - this.floodMat.opacity) * 0.05;
    if (this.floodMat.opacity < 0.01) { this.floodPts.visible = false; return; }
    this.floodPts.visible = true;
    const pos = this.floodPts.geometry.attributes.position.array, sd = this.flood.seed;
    for (let i = 0; i < this.flood.N; i++) {
      const a = sd[i * 3] * 6.283 + t * (0.6 + sd[i * 3 + 1] * 1.8) * (i % 2 ? 1 : -1);
      const r = 1.9 + sd[i * 3 + 2] * 1.6 + Math.sin(t * 3 + i) * 0.08;
      pos[i * 3] = Math.cos(a) * r;
      pos[i * 3 + 1] = 0.05 + ((sd[i * 3 + 1] * 2.6 + t * 0.15 * (1 + sd[i * 3])) % 2.6);
      pos[i * 3 + 2] = Math.sin(a) * r;
    }
    this.floodPts.geometry.attributes.position.needsUpdate = true;
  }
}
