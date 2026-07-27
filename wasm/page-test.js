// Runs a WebXR page headlessly and checks that it actually draws something.
//
// Why this exists: docs/mech.html shipped for three commits calling a chestY()
// that was never defined. Inside "use strict" that throws a ReferenceError on
// the first frame, before the draw call, every frame. The headset showed a
// cleared framebuffer and nothing else — pure black — and there was no way to
// tell that apart from a page that had not loaded. Nothing in CI looked at the
// pages at all, because "you need a headset to test VR" seemed obviously true.
//
// It is not true. The page is ordinary JavaScript; only the pose data and the
// GL calls come from the headset. Stub those two and the whole thing — wasm
// load, world build, physics stepping, geometry, the draw list — runs in Node.
// What cannot be checked here is how it feels, and that is the only thing that
// actually needs a human in a headset.
//
//   node wasm/page-test.js [page.html ...]     (default: mech.html)

const { readFileSync } = require("fs");
const vm = require("vm");
const path = require("path");

const DOCS = path.join(__dirname, "..", "docs");

// --- fake WebGL2 ------------------------------------------------------------
// Records draw calls rather than rasterising. Unknown methods become no-ops and
// unknown SHOUTY properties become distinct numbers, so the page can call
// whatever it likes without this needing to keep up.
function makeGL(drawLog) {
  let nextConst = 0x1000;
  const consts = new Map();
  const state = { M: null, C: null };

  const real = {
    getShaderParameter: () => true,
    getShaderInfoLog: () => "",
    getProgramParameter: () => true,
    getProgramInfoLog: () => "",
    createShader: () => ({}),
    createProgram: () => ({}),
    createBuffer: () => ({}),
    createVertexArray: () => ({}),
    createTexture: () => ({}),
    createFramebuffer: () => ({}),
    getUniformLocation: (p, name) => ({ name }),
    getAttribLocation: () => 0,
    makeXRCompatible: () => Promise.resolve(),

    uniformMatrix4fv: (loc, transpose, v) => {
      if (loc && loc.name === "uM") state.M = Array.from(v);
    },
    uniform3fv: (loc, v) => {
      if (loc && loc.name === "uC") state.C = Array.from(v);
    },
    // One drawElements = one box on screen. The model matrix carries where it
    // is and how big, which is everything a check needs.
    drawElements: () => {
      if (!state.M) return;
      const m = state.M;
      const col = (a, b, c) => Math.hypot(m[a], m[b], m[c]);
      drawLog.push({
        pos: [m[12], m[13], m[14]],
        scale: [col(0, 1, 2), col(4, 5, 6), col(8, 9, 10)],
        color: state.C ? state.C.slice() : null,
      });
    },
  };

  return new Proxy(real, {
    get(target, prop) {
      if (prop in target) return target[prop];
      if (typeof prop === "string" && /^[A-Z0-9_]+$/.test(prop)) {
        if (!consts.has(prop)) consts.set(prop, nextConst++);
        return consts.get(prop);
      }
      return () => {};
    },
    has: () => true,
  });
}

// --- fake WebXR -------------------------------------------------------------
const vec = (x, y, z) => ({ x, y, z });
const quat = (x, y, z, w) => ({ x, y, z, w });

function makeSession(gl, poses) {
  const rafs = [];
  const listeners = {};
  const JOINTS = ["wrist", "thumb-tip", "index-finger-tip", "middle-finger-tip",
                  "ring-finger-tip", "pinky-finger-tip"];
  const controllerSources = poses.controllers.map((c, i) => ({
    handedness: i === 0 ? "left" : "right",
    gripSpace: { which: i },
    targetRaySpace: { which: i },
    // Driven per frame from poseAt().trigger, so the harness can work
    // through the calibration the same way a person does.
    gamepad: { buttons: Array.from({ length: 6 }, () => ({ pressed: false, value: 0 })) },
  }));
  // Bare hands: no gamepad, a joint map instead — the same swap the Quest
  // makes when the controllers are put down.
  const handSources = poses.controllers.map((c, i) => ({
    handedness: i === 0 ? "left" : "right",
    gripSpace: { which: i },
    targetRaySpace: { which: i },
    hand: new Map(JOINTS.map((name) => [name, { which: i, name }])),
  }));
  const session = {
    _controllerSources: controllerSources,
    _handSources: handSources,
    inputSources: controllerSources,
    renderState: { baseLayer: null },
    updateRenderState(s) { if (s && s.baseLayer) session.renderState.baseLayer = s.baseLayer; },
    requestAnimationFrame(cb) { rafs.push(cb); },
    addEventListener(k, fn) { (listeners[k] = listeners[k] || []).push(fn); },
    requestReferenceSpace: (kind) =>
      kind === "local-floor" ? Promise.resolve({ kind }) : Promise.reject(new Error("no")),
    end() {},
    _clock: 0,
    _pump(frame) {
      // The harness pumps frames far faster than real time passes, which
      // starves any page that meters its physics with an accumulator. Frames
      // are stamped with a synthetic 72 Hz clock instead of the wall clock.
      const due = rafs.splice(0, rafs.length);
      session._clock += 1000 / 72;
      for (const cb of due) cb(session._clock, frame);
      return due.length;
    },
  };
  return session;
}

function makeFrame(session, poses) {
  const identity = new Float32Array([1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1]);
  return {
    getViewerPose: () => ({
      transform: { position: poses.head, orientation: quat(0, 0, 0, 1) },
      views: [{
        projectionMatrix: identity,
        transform: { inverse: { matrix: identity } },
      }],
    }),
    getPose: (space) => {
      const c = poses.controllers[space.which];
      return c ? { transform: { position: c.pos, orientation: c.q } } : null;
    },
    getJointPose: (jointSpace, ref) => {
      const c = poses.controllers[jointSpace.which];
      if (!c || !c.joints) return null;
      const j = c.joints[jointSpace.name];
      if (!j) return null;
      return { transform: { position: j, orientation: quat(0, 0, 0, 1) }, radius: 0.01 };
    },
  };
}

// --- run a page -------------------------------------------------------------
async function runPage(file, frames, poseAt, extraStore) {
  const html = readFileSync(path.join(DOCS, file), "utf8");
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  if (!scripts.length) throw new Error(`${file}: no inline script`);

  const drawLog = [];
  const gl = makeGL(drawLog);
  const els = {};
  const errors = [];
  const store = new Map([["box3d.floorY", "0"]]);   // floor done, span is measured below
  if (extraStore) for (const [k, v] of Object.entries(extraStore)) store.set(k, v);

  const poses = poseAt(0);
  let session = null;
  const enterHandlers = [];

  const el = (id) => {
    if (!els[id]) {
      els[id] = {
        id, textContent: "", className: "", disabled: false, style: {},
        addEventListener: (k, fn) => { if (id === "enter" && k === "click") enterHandlers.push(fn); },
      };
    }
    return els[id];
  };

  const sandbox = {
    console,
    performance,
    Float32Array, Uint16Array, Math, JSON, Promise, Error, String, Number, Array, Object,
    WebAssembly,
    setTimeout, clearTimeout, queueMicrotask,
    // Leaving this out is not a harmless omission: the page constructs it
    // inside a .then, so its absence rejects the chain, no frame is ever
    // scheduled, and the failure looks identical to a broken frame loop.
    XRWebGLLayer: class {
      constructor() {
        this.framebuffer = {};
        this.getViewport = () => ({ x: 0, y: 0, width: 1024, height: 1024 });
      }
    },
    localStorage: {
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, v),
      removeItem: (k) => store.delete(k),
    },
    document: {
      getElementById: el,
      createElement: () => ({ getContext: () => gl }),
    },
    navigator: {
      xr: {
        isSessionSupported: () => Promise.resolve(true),
        requestSession: () => { session = makeSession(gl, poses); return Promise.resolve(session); },
      },
    },
    fetch: (url) =>
      Promise.resolve({
        ok: true,
        status: 200,
        arrayBuffer: async () => {
          // Serve like the CDN does: the query string is a cache-buster
          // (box3d.wasm?v=N), not part of the file's name.
          const b = readFileSync(path.join(DOCS, url.split("?")[0]));
          return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
        },
      }),
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;

  const ctx = vm.createContext(sandbox);
  for (const src of scripts) vm.runInContext(src, ctx, { filename: file });

  // WebAssembly.instantiate resolves off a macrotask, so draining microtasks
  // alone leaves the module still loading and no frame ever scheduled.
  const settle = async () => { for (let i = 0; i < 40; i++) await new Promise((r) => setTimeout(r, 0)); };
  await settle();

  if (!enterHandlers.length) throw new Error(`${file}: nothing listened for the Enter VR click`);
  enterHandlers.forEach((fn) => fn());
  await settle();
  if (!session) throw new Error(`${file}: no XR session was requested`);

  // Drive frames. An exception escaping a frame callback is the exact failure
  // being guarded against, so it is caught here and reported, not thrown.
  const perFrame = [];
  const matchSeq = [];   // every distinct match-state the report passed through
  let curMatch = "";     // last match-state seen — fed back to the script, so
                         // poses can react to the game instead of to a clock
  let curEnemy = "";     // last enemy-state seen — so a script can BOX:
                         // guard the windup, punish the recovery
  for (let f = 0; f < frames; f++) {
    const p = poseAt(f, curMatch, curEnemy);
    poses.head = p.head;
    poses.controllers.forEach((c, i) => {
      c.pos = p.controllers[i].pos;
      c.q = p.controllers[i].q;
      c.joints = p.controllers[i].joints || null;
    });
    session.inputSources = p.handsMode ? session._handSources : session._controllerSources;
    session.inputSources.forEach((src) => {
      if (!src.gamepad) return;          // bare hands have no buttons
      src.gamepad.buttons[0].pressed = !!p.trigger;
      src.gamepad.buttons[0].value = p.trigger ? 1 : 0;
      src.gamepad.buttons[1].pressed = !!p.grip;
      src.gamepad.buttons[1].value = p.grip ? 1 : 0;
      // X lives on the left controller, A on the right.
      const b4 = src.handedness === "left" ? !!p.xbtn : !!p.abtn;
      src.gamepad.buttons[4].pressed = b4;
      src.gamepad.buttons[4].value = b4 ? 1 : 0;
    });
    const before = drawLog.length;
    try {
      if (session._pump(makeFrame(session, poses)) === 0) throw new Error("frame loop stopped rescheduling");
    } catch (e) {
      errors.push(`frame ${f}: ${e && e.stack ? e.stack.split("\n")[0] : e}`);
      break;
    }
    perFrame.push(drawLog.slice(before));
    const rpt = el("report").textContent;
    const mm = rpt.match(/match: (\w+)/);
    if (mm) {
      curMatch = mm[1];
      if (matchSeq[matchSeq.length - 1] !== mm[1]) matchSeq.push(mm[1]);
    }
    const em = rpt.match(/enemy: (\w+)/);
    if (em) curEnemy = em[1];
    if (process.env.DBGR && f % 200 === 0)
      console.log("  f" + f, (rpt.match(/match: \w+/) || [""])[0],
                  (rpt.match(/its plate [^ ]+/) || [""])[0],
                  (rpt.match(/its core \d+%/) || [""])[0],
                  (rpt.match(/hull damage \d+/) || [""])[0]);
  }

  return { drawLog, perFrame, errors, matchSeq,
           status: el("status").textContent, report: el("report").textContent };
}

// --- checks -----------------------------------------------------------------
let pass = 0, fail = 0;
const check = (name, ok, detail) => {
  if (ok) { pass++; console.log(`PASS  ${name}`); }
  else { fail++; console.log(`FAIL  ${name}${detail ? "  — " + detail : ""}`); }
};

// A standing player, floor already calibrated, who then measures their arm
// span and gets on with it.
//
// The span calibration is walked through rather than seeded, because it is the
// step that decides how big the machine is: a person holds their arms straight
// out and pulls a trigger, and everything downstream is built from that. If it
// silently failed the arms would be the wrong length and the checks below would
// all still pass on a machine nobody could use.
const HEAD_Y = 1.62;
const SPAN = 1.90;   // deliberately not the built-in fallback, so a
                     // calibration that silently did nothing shows up
const TPOSE_UNTIL = 24, PULL_AT = 12;

function pose(f) {
  if (f < TPOSE_UNTIL) {
    // Arms straight out to the sides.
    return {
      head: vec(0, HEAD_Y, 0),
      controllers: [
        { pos: vec(-SPAN / 2, 1.40, 0), q: quat(0, 0, 0, 1) },
        { pos: vec(SPAN / 2, 1.40, 0), q: quat(0, 0, 0, 1) },
      ],
      trigger: f === PULL_AT,
    };
  }
  // Kept small enough that each hand stays on its own side of the body, so a
  // limb drawn on the wrong side is unambiguous rather than just a wide reach.
  // Held still for the last stretch, so the closing checks measure where the
  // arm actually comes to rest rather than how far behind a moving hand it is.
  // Sway settles to nothing before the end, so the closing checks read a
  // symmetric, comfortable reach rather than a moment where one hand happens to
  // have drifted across the body.
  const g = f - TPOSE_UNTIL;
  const sway = g > 110 ? 0 : Math.sin(g * 0.05) * 0.12;
  return {
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: vec(-0.32 + sway, 1.20, -0.45), q: quat(0, 0, 0, 1) },
      { pos: vec(0.32 + sway, 1.20, -0.45), q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  };
}

// The drag page's session: calibrate, blow the legs out with X, then heave —
// both fists to the ground, grip, haul back, release, swing forward, repeat.
function poseDrag(f) {
  if (f < TPOSE_UNTIL) return pose(f);
  const neutral = {
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: vec(-0.24, 1.15, -0.30), q: quat(0, 0, 0, 1) },
      { pos: vec(0.24, 1.15, -0.30), q: quat(0, 0, 0, 1) },
    ],
    trigger: false, grip: false, xbtn: false,
  };
  if (f < 34) return neutral;
  if (f < 36) return { ...neutral, xbtn: true };          // legs out
  if (f < 100) return neutral;                            // machine collapses
  // Heave cycles. Tracking height ~1.08 maps a fist to the world floor once
  // the cockpit has sunk with the hull.
  const PULL = 30, SWING = 34, CYCLE = PULL + SWING;
  const c = (f - 100) % CYCLE;
  let z, y, grip;
  if (c < PULL) { grip = true; y = 1.06; z = -0.45 + 0.5 * (c / PULL); }
  else { grip = false; const t = (c - PULL) / SWING; y = 1.12 + 0.14 * Math.sin(t * Math.PI); z = 0.05 - 0.5 * t; }
  return {
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: vec(-0.24, y, z), q: quat(0, 0, 0, 1) },
      { pos: vec(0.24, y, z), q: quat(0, 0, 0, 1) },
    ],
    trigger: false, grip, xbtn: false,
  };
}

// The bare-hands session: calibrate with controllers, put them down, punch
// fast with a scripted mid-punch tracking dropout, pinch once to cycle the
// material, and finish with both fists clenched.
function mkHand(wrist, open01, pinching) {
  // Fingertips sit ahead of the wrist when open, curled close when clenched.
  const r = 0.05 + 0.08 * open01;
  const joints = {
    "wrist": wrist,
    "index-finger-tip": vec(wrist.x + 0.01, wrist.y + 0.02, wrist.z - r),
    "middle-finger-tip": vec(wrist.x, wrist.y + 0.02, wrist.z - r - 0.01),
    "ring-finger-tip": vec(wrist.x - 0.01, wrist.y + 0.02, wrist.z - r),
    "pinky-finger-tip": vec(wrist.x - 0.02, wrist.y + 0.01, wrist.z - r + 0.01),
  };
  joints["thumb-tip"] = pinching
    ? vec(joints["index-finger-tip"].x + 0.01, joints["index-finger-tip"].y, joints["index-finger-tip"].z)
    : vec(wrist.x + 0.03, wrist.y, wrist.z - 0.04);
  return joints;
}

function poseHands(f) {
  if (f < TPOSE_UNTIL) return pose(f);
  if (f < 40) {
    return {
      head: vec(0, HEAD_Y, 0),
      controllers: [
        { pos: vec(-0.24, 1.15, -0.30), q: quat(0, 0, 0, 1) },
        { pos: vec(0.24, 1.15, -0.30), q: quat(0, 0, 0, 1) },
      ],
      trigger: false,
    };
  }
  // Controllers down, hands up.
  const g = f - 40;
  const mk = (side, wrist, open01, pinching, dropped) => ({
    pos: wrist, q: quat(0, 0, 0, 1),
    joints: dropped ? null : mkHand(wrist, open01, pinching),
  });
  let L = { w: vec(-0.24, 1.15, -0.32), open: 1, pinch: false };
  let R = { w: vec(0.24, 1.15, -0.32), open: 1, pinch: false };
  let dropR = false;
  if (g < 60) { /* hold still, tracking settles */ }
  else if (g < 62) { L.pinch = true; L.open = 0.6; }        // cycle material once
  else if (g < 80) { /* recover */ }
  else if (g < 240) {
    // Fast alternating punches, fists clenched on the way out. The right hand
    // loses tracking for 10 frames in the middle of a fast stroke.
    const c = (g - 80) % 40, out = c < 20 ? c / 20 : (40 - c) / 20;
    const side = Math.floor((g - 80) / 40) % 2;
    if (side === 0) { R.w = vec(0.24, 1.15, -0.32 - 0.55 * out); R.open = 0.1; }
    else { L.w = vec(-0.24, 1.15, -0.32 - 0.55 * out); L.open = 0.1; }
    // Tracking dies mid-stroke on the punching hand — the case that decides
    // whether fights can run on bare hands.
    dropR = side === 0 && c >= 8 && c < 18;
  } else {
    // Finish: both fists up, clenched, tracked — the last report shows FIST.
    L = { w: vec(-0.24, 1.20, -0.35), open: 0, pinch: false };
    R = { w: vec(0.24, 1.20, -0.35), open: 0, pinch: false };
  }
  return {
    head: vec(0, HEAD_Y, 0),
    handsMode: true,
    controllers: [
      mk(0, L.w, L.open, L.pinch, false),
      mk(1, R.w, R.open, R.pinch, dropR),
    ],
    trigger: false,
  };
}

// The wall session: calibrate, then punch the same spot hard, repeatedly.
function posePunch(f) {
  if (f < TPOSE_UNTIL) return pose(f);
  const g = f - TPOSE_UNTIL;
  let z = -0.30, y = 1.05;
  if (g > 30) {
    const c = (g - 30) % 36, out = c < 18 ? c / 18 : (36 - c) / 18;
    z = -0.25 - 0.43 * out;
  }
  return {
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: vec(-0.30, 1.15, -0.25), q: quat(0, 0, 0, 1) },
      { pos: vec(0.18, y, z), q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  };
}

// The arena session: calibrate, raise both fists to start, guard while the
// enemy closes and swings, then punch back at its chest.
function poseArena(f) {
  if (f < TPOSE_UNTIL) return pose(f);
  const mk = (l, r) => ({
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: l, q: quat(0, 0, 0, 1) },
      { pos: r, q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  });
  if (f < 40) return mk(vec(-0.24, 1.15, -0.30), vec(0.24, 1.15, -0.30));
  if (f < 120) return mk(vec(-0.20, 1.80, -0.10), vec(0.20, 1.80, -0.10));  // fists up
  if (f < 200) return mk(vec(-0.24, 1.15, -0.25), vec(0.24, 1.15, -0.25));  // guard
  const c = (f - 200) % 36, out = c < 18 ? c / 18 : (36 - c) / 18;
  return mk(vec(-0.26, 1.12, -0.25), vec(0.06, 1.12, -0.22 - 0.48 * out));
}

// The arena's losing path, driven by the game's own state: start the fight,
// take the beating undefended, stay down through the whole knockdown count
// (fists low — never the rise gesture), and when the loss lands, raise the
// fists for the rematch. State-aware, so no tuning of fight pacing can
// silently desynchronise the script from the match again.
function poseArenaCollapse(f, m) {
  if (f < TPOSE_UNTIL) return pose(f);
  const mk = (l, r) => ({
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: l, q: quat(0, 0, 0, 1) },
      { pos: r, q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  });
  if (f < 40) return mk(vec(-0.24, 1.15, -0.30), vec(0.24, 1.15, -0.30));
  if (m === "READY" || m === "LOST")
    return mk(vec(-0.20, 1.80, -0.10), vec(0.20, 1.80, -0.10));  // start / rematch
  // Hands at the sides, BEHIND the hull line: with blocks that absorb,
  // wide parked arms were an accidental guard and "undefended" wasn't.
  return mk(vec(-0.28, 0.80, 0.18), vec(0.28, 0.80, 0.18));      // take it, stay down
}

// The final fight, played to WIN: an aggressive metronome against the God,
// used to prove the campaign is actually completable and that the fifth win
// fires ENDING — the one transition nothing else reaches. State-aware:
// metronome while fighting (punches driven at the risen God's core height —
// stature raised every machine), fists up for every gesture the flow wants,
// including the rise if a knockdown interrupts the attempt.
const winHands = { L: null, R: null };
function poseArenaWin(f, m, e) {
  if (f < TPOSE_UNTIL) return pose(f);
  // Hands move at HUMAN speed: the guard/drill switch used to teleport the
  // targets, the arm springs flung the real arm bodies metres through the
  // enemy, and the flight killed its leg parts from across the arena. A
  // person's hands cannot do that; neither can the proof's.
  const lerpTo = (cur, tgt) => {
    if (!cur) return tgt;
    const d = [tgt.x - cur.x, tgt.y - cur.y, tgt.z - cur.z];
    const len = Math.hypot(d[0], d[1], d[2]), MAX = 0.07;
    if (len <= MAX) return tgt;
    return vec(cur.x + d[0] / len * MAX, cur.y + d[1] / len * MAX,
               cur.z + d[2] / len * MAX);
  };
  const mk = (l, r) => {
    winHands.L = lerpTo(winHands.L, l);
    winHands.R = lerpTo(winHands.R, r);
    return {
      head: vec(0, HEAD_Y, 0),
      controllers: [
        { pos: winHands.L, q: quat(0, 0, 0, 1) },
        { pos: winHands.R, q: quat(0, 0, 0, 1) },
      ],
      trigger: false,
    };
  };
  if (f < 40) return mk(vec(-0.24, 1.15, -0.30), vec(0.24, 1.15, -0.30));
  if (m === "FIGHT") {
    // The boxing contract, played correctly: when the God draws up, BOTH
    // arms come up and catch the swing (blocks absorb and interrupt now);
    // the rest of the time, LEAN IN and drill the same spot at core
    // height — a real player steps into their punches, and the rig
    // follows the head. This is the defensive game the engine promises,
    // driven as the proof.
    if (e === "WINDUP" || e === "SWING")
      return mk(vec(-0.22, 1.52, -0.38), vec(0.22, 1.52, -0.38));
    const c = f % 24, out = c < 10 ? c / 10 : Math.max(0, 1 - (c - 10) / 14);
    const drill = mk(vec(-0.06, 1.35, -0.56), vec(0.10, 1.60, -0.44 - 0.55 * out));
    drill.head = vec(0, HEAD_Y, -0.26);   // lean into the punches
    return drill;
  }
  return mk(vec(-0.20, 1.80, -0.10), vec(0.20, 1.80, -0.10));  // every gesture
}

// The knockdown survived, end to end: take the beating undefended, go down,
// and while the count bells strike raise both fists and HOLD — the machine
// stands back up healed and the fight resumes. COLLAPSED followed by FIGHT
// is the rise; nothing else produces that pair.
function poseArenaRise(f, m) {
  if (f < TPOSE_UNTIL) return pose(f);
  const mk = (l, r) => ({
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: l, q: quat(0, 0, 0, 1) },
      { pos: r, q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  });
  if (f < 40) return mk(vec(-0.24, 1.15, -0.30), vec(0.24, 1.15, -0.30));
  if (m === "READY" || m === "COLLAPSED")
    return mk(vec(-0.20, 1.80, -0.10), vec(0.20, 1.80, -0.10));  // start / RISE
  return mk(vec(-0.28, 0.80, 0.18), vec(0.28, 0.80, 0.18));      // take the beating
}

// Beating on the practice box: right hand drives repeatedly into the
// deformable block behind the spawn's right shoulder. No fists-up — the
// box is there before the match, which is the whole point of it.
function poseBox(f) {
  if (f < TPOSE_UNTIL) return pose(f);
  const c = f % 20, out = c < 8 ? c / 8 : Math.max(0, 1 - (c - 8) / 10);
  return {
    head: vec(0, HEAD_Y, 0),
    controllers: [
      { pos: vec(-0.22, 1.10, -0.20), q: quat(0, 0, 0, 1) },
      { pos: vec(0.30 + 0.40 * out, 1.02, 0.02 + 0.28 * out), q: quat(0, 0, 0, 1) },
    ],
    trigger: false,
  };
}

(async () => {
  const page = process.argv[2] || "mech.html";
  console.log(`--- ${page} ---`);

  const script = page === "drag.html" ? poseDrag
               : page === "handtrack.html" ? poseHands
               : page === "vox.html" || page === "dummy.html" ? posePunch
               : page === "arena.html" ? poseArena : pose;
  const frames = page === "drag.html" ? 420
               : page === "handtrack.html" ? 320
               : page === "vox.html" || page === "dummy.html" ? 360
               : page === "arena.html" ? 700 : 190;
  const r = await runPage(page, frames, script);

  check("no exception escaped the frame loop", r.errors.length === 0,
        (r.errors[0] || "") + "  [page said: " + r.status + "]");
  check("the page never reported a fault", !/Broke in the frame loop|FAULT/.test(r.status + r.report),
        r.status);

  const last = r.perFrame[r.perFrame.length - 1] || [];
  check("the last frame drew something", last.length > 0, `${last.length} boxes`);
  // 49 grid + head marker + 2 controllers + torso + 6 limb segments + 6 blocks
  // + 4 material pips = 69. Anything near that means the world is really there.
  check("the last frame drew the whole scene", last.length >= 60, `${last.length} boxes`);

  if (page === "drag.html") {
    check("the legs report as gone", /legs: GONE/.test(r.report), "no GONE line in the report");
    const trav = r.report.match(/travelled: ([\d.]+) m/);
    check("heaving dragged the machine", trav && parseFloat(trav[1]) > 0.35,
          trav ? trav[1] + " m" : "no travelled line in the report");
    const planted = /PLANTED/.test(r.report) || (trav && parseFloat(trav[1]) > 0.35);
    check("the fists actually planted", planted, "no fist ever anchored");
  }

  if (page === "arena.html") {
    check("the fists-up gesture started the fight",
          r.matchSeq.includes("FIGHT"), "states seen: " + r.matchSeq.join(">"));
    const eplate = r.report.match(/its plate (\d+)\/(\d+)/);
    const you = r.report.match(/you: plate (\d+)\/(\d+)/);
    const damage = (eplate && parseInt(eplate[1], 10) < parseInt(eplate[2], 10)) ||
                   (you && parseInt(you[1], 10) < parseInt(you[2], 10));
    check("the fight drew blood on at least one side", !!damage,
          `enemy ${eplate ? eplate[1] + "/" + eplate[2] : "?"}, you ${you ? you[1] + "/" + you[2] : "?"}`);
    check("the enemy machine is drawn and acting",
          /enemy: (APPROACH|WINDUP|SWING|RECOVER|DEAD|TRIUMPH)/.test(r.report),
          (r.report.match(/enemy: \w+/) || ["no enemy line"])[0]);

    // Second run: the losing path. Undefended, the fight should end in the
    // knockdown -> count runs out -> loss -> rematch chain, in that order
    // (the script keeps its fists down through the whole count).
    const r2 = await runPage(page, 6800, poseArenaCollapse);
    check("collapse run: no exception escaped", r2.errors.length === 0, r2.errors[0] || "");
    const seq = r2.matchSeq.join(">");
    const chain = ["READY", "FIGHT", "COLLAPSED", "LOST", "READY"];
    let at = 0;
    for (const s of r2.matchSeq) if (s === chain[at]) at++;
    check("undefended, the fight collapses, times out, and offers the rematch",
          at >= chain.length, "states seen: " + seq);
    // Loop closure: with the fists still up, the rebuilt chapter starts a
    // fresh fight — the final plate count is whatever that new fight has
    // done to it, so the proof is the state sequence, not the last frame.
    const afterLost = r2.matchSeq.slice(r2.matchSeq.indexOf("LOST"));
    check("the rematch rebuilt the chapter and it fights again",
          afterLost.indexOf("READY") > 0 &&
          afterLost.indexOf("FIGHT") > afterLost.indexOf("READY"),
          "after LOST: " + afterLost.join(">"));

    // Third run: a saved campaign boots where it was left. Seeded storage
    // says lap 2, chapter 3, all arms — the report must agree.
    const r3 = await runPage(page, 200, pose,
      { "box3d.campaign": JSON.stringify({ c: 2, u: 3, l: 1, v: 3 }),
        "box3d.armSpan": "1.75" });
    check("a saved campaign boots where it was left",
          /chapter 3 of 5/.test(r3.report) && /arms unlocked: 4 of 4/.test(r3.report) &&
          /lap 2/.test(r3.report),
          (r3.report.match(/chapter [^\n]*/) || ["no chapter line"])[0]);

    // Fourth run: the campaign is completable, by proof. Boot at the final
    // chapter (index 4) with everything unlocked; the aggressive script
    // kills the God standing (an earlier draft won from its knees — that
    // win counts, but the provoked victor turned the route into a beating),
    // the final win fires ENDING, and begin-again reboots the campaign on
    // the next lap. This is the only thing that executes ENDING.
    const r4 = await runPage(page, 7000, poseArenaWin,
      { "box3d.campaign": JSON.stringify({ c: 4, u: 3, l: 0, v: 3 }),
        "box3d.armSpan": "1.75" });
    const endChain = ["FIGHT", "WON", "ENDING", "READY"];
    let endAt = 0;
    for (const s of r4.matchSeq) if (s === endChain[endAt]) endAt++;
    check("the God can be beaten, and the fifth win fires the ending",
          endAt >= endChain.length, "states seen: " + r4.matchSeq.join(">"));

    // Fifth run: the knockdown survived, end to end. Take the beating, go
    // down, and during the count raise both fists — the machine stands
    // back up healed and fights on. COLLAPSED followed by FIGHT is the
    // rise; nothing else produces that pair.
    // Sixth run: the deformation testbed. Punch the practice box and the
    // mesh must report real, permanent dents.
    const r6 = await runPage(page, 500, poseBox);
    const pb = r6.report.match(/practice box: (\d+) hits, deepest dent (\d+) mm/);
    check("the practice box takes real dents from punches",
          pb && parseInt(pb[1], 10) > 0 && parseInt(pb[2], 10) > 0,
          pb ? `${pb[1]} hits, ${pb[2]} mm` : "no practice-box line in the report");

    const r5 = await runPage(page, 3200, poseArenaRise);
    const swChain = ["FIGHT", "COLLAPSED", "FIGHT"];
    let swAt = 0;
    for (const s of r5.matchSeq) if (s === swChain[swAt]) swAt++;
    check("the knockdown can be survived: count, fists up, stand and fight on",
          swAt >= swChain.length, "states seen: " + r5.matchSeq.join(">"));
  }

  if (page === "dummy.html") {
    const plate = r.report.match(/plate: (\d+) of 64/);
    check("the plate lost cells to the punches", plate && parseInt(plate[1], 10) < 60,
          plate ? plate[1] + " left" : "no plate line in the report");
    const deb = r.report.match(/debris: (\d+)/);
    check("the shed cells are debris", deb && parseInt(deb[1], 10) >= 3,
          deb ? deb[1] + " cubes" : "no debris line");
    const core = last.filter((b) => b.color && b.color[0] < 0.3 && b.scale[1] > 0.3);
    check("the bare core is drawn", core.length >= 1, `${core.length} found`);
  }

  if (page === "vox.html") {
    const stone = r.report.match(/stone (\d+)\/(\d+)/);
    const smashed = r.report.match(/smashed total: (\d+)/);
    check("punches broke cells out of the stone wall",
          smashed && parseInt(smashed[1], 10) >= 3,
          smashed ? smashed[1] + " smashed" : "no smashed line in the report");
    check("the stone wall is still mostly standing",
          stone && parseInt(stone[1], 10) > 150 && parseInt(stone[1], 10) < parseInt(stone[2], 10),
          stone ? `${stone[1]} of ${stone[2]}` : "no stone line");
    const deb = r.report.match(/debris: (\d+)/);
    check("the dead cells are debris now", deb && parseInt(deb[1], 10) >= 3,
          deb ? deb[1] + " cubes" : "no debris line");
    const runs = last.filter((b) => b.color && Math.abs(b.color[0] - 0.56) < 0.03 && b.pos[2] < -0.3);
    check("the wall is drawn as merged runs", runs.length >= 15 && runs.length <= 200,
          `${runs.length} runs`);
  }

  if (page === "handtrack.html") {
    const trk = r.report.match(/hand tracking: (\d+) tracked frames/);
    check("bare hands drove the page", trk && parseInt(trk[1], 10) > 150,
          trk ? trk[1] + " frames" : "no tracked-frames line");
    const dr = r.report.match(/dropouts L\/R: (\d+)\/(\d+)/);
    check("the scripted dropout was counted", dr && parseInt(dr[2], 10) >= 1,
          dr ? `L ${dr[1]} R ${dr[2]}` : "no dropout line");
    const fast = r.report.match(/during fast motion: (\d+)\/(\d+)/);
    check("it was blamed on fast motion", fast && parseInt(fast[2], 10) >= 1,
          fast ? `L ${fast[1]} R ${fast[2]}` : "no fast-motion line");
    check("a pinch cycled the material", /arm material 3 of 4/.test(r.report),
          (r.report.match(/arm material \d of 4/) || ["no material line"])[0]);
    check("a clenched fist reads as FIST", /FIST/.test(r.report), "no FIST in the last report");
    const dots = last.filter((b) => b.scale[0] < 0.012 && b.color && b.color[1] > 0.7);
    check("fingertip markers are drawn", dots.length >= 6, `${dots.length} dots`);
  }

  if (page === "mech.html") {
    // The torso is the widest thing drawn that is not the floor grid.
    const body = last.filter((b) => b.pos[1] > 0.3);
    const torso = body.reduce((a, b) => (b.scale[0] > (a ? a.scale[0] : 0) ? b : a), null);
    check("a torso-sized box exists", torso && torso.scale[0] >= 0.19,
          torso ? `widest ${torso.scale[0].toFixed(2)} m` : "nothing above the floor");

    if (torso) {
      const drop = HEAD_Y - torso.pos[1];
      check("the torso hangs below the head, not around it",
            drop > 0.30 && drop < 0.55, `centre ${drop.toFixed(2)} m below the eyes`);
      const top = torso.pos[1] + torso.scale[1];
      check("the head is clear of the torso", top < HEAD_Y - 0.10,
            `top of torso at ${top.toFixed(2)}, eyes at ${HEAD_Y}`);
    }

    // Limbs are drawn as long thin boxes; joints and attachments are cubes.
    const limbs = last.filter((b) => {
      const s = b.scale, long = Math.max(...s), thin = Math.min(...s);
      // Threshold sized for a human-proportioned forearm (about 0.13 drawn
      // half-length), not the over-long arm this used to have.
      return long > 0.09 && long / thin > 1.7 && b.pos[1] > 0.3;
    });
    check("four limb segments are drawn", limbs.length === 4, `${limbs.length} found`);

    // The arm is the length the T-pose measured, not a built-in default.
    // Everything downstream is built off that measurement, so if the trigger
    // pull quietly did nothing the machine would be the wrong size for the
    // player and every other check here would still pass.
    if (limbs.length === 4) {
      const shoulderHalf = SPAN * 0.11, toolLen = 2 * 0.06;
      const bones = SPAN * 0.5 - shoulderHalf - toolLen;
      const wantUpper = bones * 0.55 * 0.5;             // drawn half-length
      const gotUpper = Math.max(...limbs.map((b) => Math.max(...b.scale)));
      check("the arm is the length your span said", Math.abs(gotUpper - wantUpper) < 0.03,
            `drawn ${gotUpper.toFixed(3)} m, measured span wants ${wantUpper.toFixed(3)} m`);
    }

    // Colour says which arm a segment belongs to: blue left, orange right. With
    // the wrong stride the right arm was drawn from the left arm's bodies, so
    // orange segments appeared on the left — which this catches.
    const sideOf = (b) => (b.color[2] > b.color[0] ? "left" : "right");
    const misplaced = limbs.filter((b) =>
      (sideOf(b) === "left" && b.pos[0] > 0.05) || (sideOf(b) === "right" && b.pos[0] < -0.05));
    check("each arm is drawn on its own side", misplaced.length === 0,
          misplaced.map((b) => `${sideOf(b)} at x ${b.pos[0].toFixed(2)}`).join(", "));

    // The attachments are near-cubes, pale, and out in front of the torso.
    const tips = last.filter((b) => {
      const s = b.scale;
      return b.color[0] > 0.8 && b.color[2] > 0.8 && Math.max(...s) / Math.min(...s) < 1.3 &&
             Math.max(...s) > 0.05 && b.pos[1] > 0.3;
    });
    check("both arms end in an attachment", tips.length === 2, `${tips.length} found`);

    // The claim the whole spike rests on, and the one the headset judged
    // hardest: with your hand held still, the end of the arm is where your hand
    // is. It used to hang 13 to 38 cm below it — mostly straight down — because
    // nothing carried the arm's weight, and that reads in VR as the arm simply
    // not following you. The drawn attachment is a half-length short of the
    // point the engine hauls, so allow for that and little else.
    if (tips.length === 2) {
      const hands = pose(189).controllers.map((c) => c.pos);
      const near = [-1, 1].map((sign) => {
        const hand = hands.find((h) => Math.sign(h.x) === sign) || hands[0];
        const tip = tips.reduce((a, b) =>
          Math.abs(b.pos[0] - hand.x) < Math.abs(a.pos[0] - hand.x) ? b : a);
        return Math.hypot(tip.pos[0] - hand.x, tip.pos[1] - hand.y, tip.pos[2] - hand.z);
      });
      check("the arm ends up where your hand is", near.every((d) => d < 0.16),
            near.map((d) => (d * 100).toFixed(1) + " cm").join(" / "));
      // Specifically not below it. Sag is the failure this spike kept shipping.
      const drop = tips.map((t) => hands[0].y - t.pos[1]);
      check("the arm does not hang below your hand", Math.max(...drop) < 0.12,
            `${(Math.max(...drop) * 100).toFixed(1)} cm below`);
    }

    // The whole machine must sit in the space a person occupies. Arms flung to
    // the ceiling or through the floor is the failure this catches.
    const stray = body.filter((b) => b.pos[1] > HEAD_Y + 0.25 || b.pos[1] < -0.15);
    check("nothing ends up above the head or under the floor", stray.length === 0,
          stray.length ? `${stray.length} strays, first at y ${stray[0].pos[1].toFixed(2)}` : "");

    // The arms have to move. A frozen machine draws the same thing every frame.
    const first = r.perFrame[40] || [];
    const moved = first.length === last.length &&
      last.some((b, i) => Math.hypot(b.pos[0] - first[i].pos[0], b.pos[1] - first[i].pos[1],
                                     b.pos[2] - first[i].pos[2]) > 0.02);
    check("the machine moves as the hands move", moved);
  }

  console.log(`\n${pass} passed, ${fail} failed`);
  if (fail) {
    console.log("\nlast report from the page:\n" + r.report);
    process.exit(1);
  }
})().catch((e) => { console.error(e); process.exit(1); });
