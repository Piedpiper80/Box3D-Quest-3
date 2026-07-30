// Runs the WebXR page headlessly and checks that it actually draws and behaves.
//
// Why this exists: a page in this project once shipped for three commits calling
// a function that was never defined. Inside "use strict" that throws on the
// first frame, before the draw call, every frame. The headset showed a cleared
// framebuffer and nothing else — pure black — and there was no way to tell that
// apart from a page that had not loaded. Nothing in CI looked at the pages at
// all, because "you need a headset to test VR" seemed obviously true.
//
// It is not true. The page is ordinary JavaScript; only the pose data and the
// GL calls come from the headset. Stub those two and the whole thing — wasm
// load, world build, physics stepping, geometry, the draw list — runs in Node.
// What cannot be checked here is how it FEELS, and that is the only thing that
// actually needs a person in a headset.
//
//   node wasm/page-test.js [page.html]      (default: arena.html)

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
  const state = { M: null, C: null, count: 0 };

  const real = {
    getShaderParameter: () => true,
    getShaderInfoLog: () => "",
    getProgramParameter: () => true,
    getProgramInfoLog: () => "",
    createShader: () => ({}),
    createProgram: () => ({}),
    createBuffer: () => ({}),
    createVertexArray: () => ({}),
    getUniformLocation: (p, name) => ({ name }),
    getAttribLocation: () => 0,
    makeXRCompatible: () => Promise.resolve(),

    uniformMatrix4fv: (loc, transpose, v) => {
      if (loc && loc.name === "uM") state.M = Array.from(v);
    },
    uniform3fv: (loc, v) => {
      if (loc && loc.name === "uC") state.C = Array.from(v);
    },
    // One drawElements = one thing on screen. The model matrix carries where it
    // is and how big, which is everything a check needs. `count` distinguishes
    // a plain box (36 indices) from a deformable bone mesh (far more).
    drawElements: (mode, count) => {
      if (!state.M) return;
      const m = state.M;
      const col = (a, b, c) => Math.hypot(m[a], m[b], m[c]);
      drawLog.push({
        pos: [m[12], m[13], m[14]],
        scale: [col(0, 1, 2), col(4, 5, 6), col(8, 9, 10)],
        color: state.C ? state.C.slice() : null,
        indices: count,
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
  const sources = poses.controllers.map((c, i) => ({
    handedness: i === 0 ? "left" : "right",
    gripSpace: { which: i },
    targetRaySpace: { which: i },
    gamepad: {
      buttons: Array.from({ length: 6 }, () => ({ pressed: false, value: 0 })),
      hapticActuators: [{ pulse: () => {} }],
    },
  }));
  const session = {
    inputSources: sources,
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
  };
}

// --- run a page -------------------------------------------------------------
async function runPage(file, frames, poseAt) {
  const html = readFileSync(path.join(DOCS, file), "utf8");
  const scripts = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
  if (!scripts.length) throw new Error(`${file}: no inline script`);

  const drawLog = [];
  const gl = makeGL(drawLog);
  const els = {};
  const errors = [];
  const store = new Map();          // no floor stored: the pilot calibrates it

  const enterHandlers = [];
  const el = (id) => {
    if (!els[id]) {
      els[id] = {
        id, textContent: "", className: "", disabled: false, style: {},
        addEventListener: (k, fn) => { if (id === "enterAR" && k === "click") enterHandlers.push(fn); },
      };
    }
    return els[id];
  };

  const poses = poseAt(0);
  let session = null;

  const sandbox = {
    console, performance,
    Float32Array, Uint16Array, Math, JSON, Promise, Error, String, Number, Array, Object,
    WebAssembly,
    setTimeout, clearTimeout, queueMicrotask,
    requestAnimationFrame: () => {},   // only the flat preview uses this
    // Leaving this out is not a harmless omission: the page constructs it
    // inside a .then, so its absence rejects the chain, no frame is ever
    // scheduled, and the failure looks identical to a broken frame loop.
    XRWebGLLayer: class {
      constructor() {
        this.framebuffer = {};
        this.getViewport = () => ({ x: 0, y: 0, width: 1024, height: 1024 });
      }
    },
    AudioContext: class {
      constructor() { this.state = "running"; this.sampleRate = 48000; this.currentTime = 0; }
      createBuffer(ch, n) { return { getChannelData: () => new Float32Array(n) }; }
      createBufferSource() { return stubNode(); }
      createBiquadFilter() { return stubNode({ frequency: { value: 0 } }); }
      createGain() { return stubNode({ gain: rampParam() }); }
      createOscillator() { return stubNode({ frequency: rampParam() }); }
      get destination() { return stubNode(); }
      resume() {}
    },
    localStorage: {
      getItem: (k) => (store.has(k) ? store.get(k) : null),
      setItem: (k, v) => store.set(k, v),
      removeItem: (k) => store.delete(k),
    },
    document: {
      getElementById: el,
      createElement: () => ({ getContext: () => gl, style: {} }),
      querySelector: () => null,
    },
    navigator: {
      xr: {
        isSessionSupported: () => Promise.resolve(true),
        requestSession: () => { session = makeSession(gl, poses); return Promise.resolve(session); },
      },
    },
    fetch: (url) =>
      Promise.resolve({
        ok: true, status: 200,
        arrayBuffer: async () => {
          // Serve like a static host does: the query string is a cache-buster
          // (box3d.wasm?v=N), not part of the file's name.
          const b = readFileSync(path.join(DOCS, url.split("?")[0]));
          return b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength);
        },
      }),
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;
  function rampParam() {
    return { value: 0, setValueAtTime() {}, exponentialRampToValueAtTime() {},
             linearRampToValueAtTime() {} };
  }
  function stubNode(extra) {
    return Object.assign({ connect() {}, start() {}, stop() {}, type: "" }, extra || {});
  }

  const ctx = vm.createContext(sandbox);
  for (const src of scripts) vm.runInContext(src, ctx, { filename: file });

  // WebAssembly.instantiate resolves off a macrotask, so draining microtasks
  // alone leaves the module still loading and no frame ever scheduled.
  const settle = async () => { for (let i = 0; i < 40; i++) await new Promise((r) => setTimeout(r, 0)); };
  await settle();

  if (!enterHandlers.length) throw new Error(`${file}: nothing listened for the start click`);
  enterHandlers.forEach((fn) => fn());
  await settle();
  if (!session) throw new Error(`${file}: no XR session was requested`);

  // Drive frames. An exception escaping a frame callback is the exact failure
  // being guarded against, so it is caught here and reported, not thrown.
  const perFrame = [];
  const matchSeq = [];    // every distinct match state the run passed through
  const figSeq = [];      // and every distinct state the figure passed through
  let curMatch = "", curFig = "";
  let seen = null;        // where the figure was drawn last frame, as a player sees it
  for (let f = 0; f < frames; f++) {
    const p = poseAt(f, curMatch, curFig, seen);
    poses.head = p.head;
    poses.controllers.forEach((c, i) => { c.pos = p.controllers[i].pos; c.q = p.controllers[i].q; });
    session.inputSources.forEach((src, i) => {
      const pressed = Array.isArray(p.trigger) ? !!p.trigger[i] : !!p.trigger;
      src.gamepad.buttons[0].pressed = pressed;
      src.gamepad.buttons[0].value = pressed ? 1 : 0;
    });
    const before = drawLog.length;
    try {
      if (session._pump(makeFrame(session, poses)) === 0) throw new Error("frame loop stopped rescheduling");
    } catch (e) {
      errors.push(`frame ${f}: ${e && e.stack ? e.stack.split("\n")[0] : e}`);
      break;
    }
    const shown = drawLog.slice(before);
    perFrame.push(shown);
    {
      // What the pilot can SEE. The bone meshes are the figure, and their
      // centroid is where a player would say it is standing. Nothing else in
      // this harness tells him — and the engine is never told where he is
      // looking either, so this is the whole of the mutual information.
      // Keep the legacy fight pilot aimed at Claude's grey robot. The red
      // comparison robot is deliberately independent and otherwise its meshes
      // would move the centroid between two opponents.
      const mesh = shown.filter((d) => d.indices > 100 && !(d.color &&
        d.color[0] > d.color[1] * 1.5 && d.color[0] > d.color[2] * 1.5));
      if (mesh.length) {
        let sx = 0, sz = 0;
        for (const d of mesh) { sx += d.pos[0]; sz += d.pos[2]; }
        seen = [sx / mesh.length, sz / mesh.length];
      }
    }
    const rpt = el("report").textContent;
    const mm = rpt.match(/match: (\w+)/);
    if (mm) {
      curMatch = mm[1];
      if (matchSeq[matchSeq.length - 1] !== mm[1]) matchSeq.push(mm[1]);
    }
    const fm = rpt.match(/figure: (\w+)/);
    if (fm) {
      curFig = fm[1];
      if (figSeq[figSeq.length - 1] !== fm[1]) figSeq.push(fm[1]);
    }
    if (process.env.DBGR && f % 150 === 0) console.log("  f" + f + "  " + rpt.replace(/\n/g, " | "));
  }

  return { drawLog, perFrame, errors, matchSeq, figSeq,
           status: el("status").textContent, report: el("report").textContent };
}

// --- checks -----------------------------------------------------------------
let pass = 0, fail = 0;
const check = (name, ok, detail) => {
  if (ok) { pass++; console.log(`PASS  ${name}`); }
  else { fail++; console.log(`FAIL  ${name}${detail ? "  — " + detail : ""}`); }
};

const HEAD_Y = 1.62;
const FLOOR_AT = 14;      // the frame the pilot pulls the trigger on the ground
const STAND = 0.90;       // m  the range he keeps to it, measured to what he can see
const LAST_ONE = 1800;    // after this frame he stops fetching a fresh one

// A player who calibrates the floor, squares up, and then works the figure
// over: body, then one leg, then the other. Deliberately not a metronome at
// one height — a run that only ever punches the chest proves nothing about
// whether a leg can be taken off, which is the whole shape of the fight.
//
// He also MOVES. He did not have to while the figure was held up by an
// invisible hand that delivered it to his fist and hauled it back after every
// knockdown. Now that it stands on its legs, three things a real player does
// for free stopped being free, and a pilot nailed to one spot and one heading
// measures a fight that is not happening:
//
//   TURNS.  Nothing in the engine knows which way he is facing —
//           w_fig_update() is handed his POSITION and nothing else — so the
//           figure cannot aim itself into his punch corridor and has no reason
//           to. A player being circled pivots. This one does too.
//   STEPS IN.  Take a leg off and the figure CANNOT WALK: figSupport() < 1
//           drops it straight out of FIG_STEP, deliberately, and the engine
//           suite asserts it ("one leg gone ... and it stops walking"). A
//           crippled figure therefore stands where it is, at 1.1 m, for the
//           rest of the run. Someone has to close that gap and it is not going
//           to be the thing with one leg.
//   BRINGS THE NEXT ONE IN.  A figure that can really be knocked over is on
//           the floor inside ten seconds and never gets up (FIG_DOWN has no
//           exit). A thirty-six second run against one of them is eight
//           seconds of fight and twenty-eight of standing over a corpse. The
//           game already has the gesture; the pilot uses it.
//
let facing = 0, px = 0, pz = 0;   // where the pilot stands, and which way he is turned
function pilot(f, match, fig, seen) {
  if (seen) {
    const dx = seen[0] - px, dz = seen[1] - pz, d = Math.hypot(dx, dz);
    if (d > 1e-3) {
      let want = Math.atan2(dx, -dz), e = want - facing;
      while (e > Math.PI) e -= 2 * Math.PI;
      while (e < -Math.PI) e += 2 * Math.PI;
      const RATE = 2.5 / 72;                 // 143 deg/s: a brisk pivot, not a turret
      facing += Math.max(-RATE, Math.min(RATE, e));
      // Close the range, but only once he has stopped waiting for it to come —
      // the stretch that proves it closes has to be his standing still and its
      // walking in, or it proves nothing. Never backwards, never onto a corpse.
      if (f >= 400 && match !== "DOWN" && d > STAND) {
        const s = Math.min(1.2 / 72, d - STAND);   // 1.2 m/s, a walk
        px += dx / d * s; pz += dz / d * s;
      }
    }
  }
  const head = vec(px, HEAD_Y, pz);
  // Everything below is written in the pilot's own frame, exactly as it was
  // when that frame was the world's: A() puts it where he is now standing and
  // pointing. The stroke, its timing, its retract and its heights are unchanged.
  const cb = Math.cos(facing), sb = -Math.sin(facing);
  const A = (x, y, z) => vec(px + x * cb + z * sb, y, pz - x * sb + z * cb);
  // 1. Floor: a controller rests on the ground and the trigger goes.
  if (f < 24) {
    return {
      head,
      controllers: [
        { pos: A(-0.30, 0.00, -0.30), q: quat(0, 0, 0, 1) },
        { pos: A(0.30, 1.10, -0.30), q: quat(0, 0, 0, 1) },
      ],
      trigger: [f === FLOOR_AT, false],
    };
  }
  // 2. Both fists above the head, held, to bring one in.
  if (f < 110) {
    return {
      head,
      controllers: [
        { pos: A(-0.24, HEAD_Y + 0.24, -0.15), q: quat(0, 0, 0, 1) },
        { pos: A(0.24, HEAD_Y + 0.24, -0.15), q: quat(0, 0, 0, 1) },
      ],
      trigger: false,
    };
  }
  // 3. Guard up and let it come. Nothing is thrown at it for four seconds:
  //    this is the stretch that proves it closes, telegraphs and throws, and
  //    it has to happen BEFORE the taking-apart, because the fists turned out
  //    effective enough to floor it inside fifteen seconds — the first version
  //    of this script had it on the ground before it ever swung.
  if (f < 400) {
    return {
      head,
      controllers: [
        { pos: A(-0.22, 1.20, -0.30), q: quat(0, 0, 0, 1) },
        { pos: A(0.22, 1.20, -0.30), q: quat(0, 0, 0, 1) },
      ],
      trigger: false,
    };
  }
  // 4. Now take it apart. Each cycle draws back to the chest and drives
  //    through; the target height walks down the body and the hands ALTERNATE,
  //    because a player who only ever throws the right hand can only ever
  //    reach one of its legs — and it takes both to put it down.
  // It is on the floor, and it is not getting up: fists above the head brings
  // the next one in, the same gesture that started the fight.
  //
  // But only while there is still a fight left to have. The last stretch of the
  // run is spent finishing whatever is in front of him, because every check
  // below that reads a number off the figure — legs, bones, metal — reads it
  // off the one standing at the LAST FRAME, and a fresh replacement fetched at
  // frame 2990 is undamaged by construction. That is a lottery, not a measure:
  // it was the single flakiest thing in this file, flipping "taking the legs is
  // the way down" on and off on BOTH builds under a one-notch change to any
  // pilot constant.
  if (match === "DOWN" && f < LAST_ONE) {
    return { head, controllers: [
      { pos: A(-0.24, HEAD_Y + 0.24, -0.15), q: quat(0, 0, 0, 1) },
      { pos: A(0.24, HEAD_Y + 0.24, -0.15), q: quat(0, 0, 0, 1) }], trigger: false };
  }
  const g = f - 400;
  const CYCLE = 30;
  const c = g % CYCLE, phase = Math.floor(g / CYCLE);
  // chest, chest, thigh, shin, thigh, shin — the legs are the way down, so the
  // script goes for them, exactly as a player who has read the page would.
  const heights = [1.28, 1.28, 0.62, 0.34, 0.62, 0.34];
  const aimY = heights[phase % heights.length];
  const hand = phase % 2;                      // 0 = left leads, 1 = right
  const drive = c < 18 ? 0 : (c - 18) / 12;
  // The retract has to come all the way back to the chest. The first version
  // pulled back to 50 cm, which is still inside the figure's guard at fighting
  // range — the fist never broke contact, and a contact that never ends never
  // fires another hit event. The blow counter froze at 36 and stayed there for
  // ten seconds of "punching" that was really just leaning.
  const z = -0.20 - drive * 1.15;
  // Shoulder width for the body, but a leg is not shoulder width. Its leg
  // columns are 0.09 m off the centreline and a fist is 0.055 m across, so a
  // stroke thrown at 0.20 m passes OUTSIDE the leg with a centimetre to spare:
  // it worked only while the figure stood off to one side of a pilot who could
  // not turn, and stopped working the moment he faced it square. A player
  // punching a leg brings the fist in to where the leg is.
  const xw = aimY < 1.0 ? 0.11 : 0.20;
  const x = hand === 0 ? -xw : xw;
  const guard = { pos: A(hand === 0 ? 0.22 : -0.22, 1.18, -0.42), q: quat(0, 0, 0, 1) };
  const punch = { pos: A(x, aimY, z), q: quat(0, 0, 0, 1) };
  return {
    head,
    controllers: hand === 0 ? [punch, guard] : [guard, punch],
    trigger: false,
  };
}

(async () => {
  const file = process.argv[2] || "arena.html";
  console.log(`--- ${file} ---`);
  const r = await runPage(file, 3000, pilot);

  check("no exception escaped a frame", r.errors.length === 0, r.errors[0]);
  check("the page drew something", r.drawLog.length > 0);

  // The last frame is what a headset would be showing right now.
  const last = r.perFrame[r.perFrame.length - 1] || [];
  check("the last frame draws", last.length > 4, `${last.length} draws`);

  // A bone mesh is a subdivided box: far more indices than a plain box's 36.
  const boneDraws = last.filter((d) => d.indices > 100);
  check("the figure is drawn as deformable mesh, not boxes", boneDraws.length >= 8,
    `${boneDraws.length} mesh draws in the last frame`);
  const redBoneDraws = boneDraws.filter((d) => d.color &&
    d.color[0] > d.color[1] * 1.5 && d.color[0] > d.color[2] * 1.5);
  check("a second red Codex robot is drawn as deformable mesh", redBoneDraws.length >= 8,
    `${redBoneDraws.length} red mesh draws in the last frame`);

  // Both gauntlets, every frame, near the controllers.
  const gaunt = last.filter((d) => d.indices <= 100 && d.pos[1] > 0.2 && d.pos[1] < 2.0);
  check("your fists are drawn", gaunt.length >= 4, `${gaunt.length} box draws`);

  check("the floor was calibrated from the controller, not assumed",
    /floor 0\.000 m/.test(r.report), r.report.split("\n")[1]);

  check("it is built to the player's height",
    /your height 1\.7[0-9] m/.test(r.report), r.report.split("\n")[2]);

  check("the fists-up gesture brings one in", r.matchSeq.includes("FIGHT"),
    r.matchSeq.join(" -> "));

  // It has to actually come at you and throw hands, or none of the rest of
  // this is a fight.
  check("it closes and throws", r.figSeq.includes("STEP") && r.figSeq.includes("STRIKE"),
    r.figSeq.join(" -> "));

  const blows = +(r.report.match(/you landed (\d+) blows/) || [0, 0])[1];
  check("punches land on it", blows > 20, `${blows} landed`);

  const dent = +(r.report.match(/deepest dent (\d+) mm/) || [0, 0])[1];
  check("the metal takes real dents", dent >= 10, `${dent} mm`);

  const broke = +(r.report.match(/broke (\d+) bones/) || [0, 0])[1];
  check("bones come off it", broke >= 1, `${broke} broken`);

  // With a second physical fighter in the world, grey can now be toppled by a
  // collision before the scripted pilot reaches its legs. The isolated engine
  // suite still proves the leg-loss rule; this integration run proves the new
  // red body is not frozen after its own knock-down.
  const riseAttempts = +(r.report.match(/rise attempts (\d+)/) || [0, 0])[1];
  check("living red keeps trying to rise after a physical knock-down",
    riseAttempts > 0, `${riseAttempts} attempts`);

  check("it goes down, and there is nothing to explode",
    r.matchSeq.includes("DOWN") || r.figSeq.includes("FALLING") || r.figSeq.includes("DOWN"),
    r.matchSeq.join(" -> ") + " / " + r.figSeq.join(" -> "));

  check("no fault was reported", !/FAULT/.test(r.report),
    (r.report.match(/FAULT.*/) || [""])[0]);

  console.log(`\n${pass} passed, ${fail} failed`);
  console.log(r.report.split("\n").map((l) => "  " + l).join("\n"));
  process.exit(fail ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(1); });
