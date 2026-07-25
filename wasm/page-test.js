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
  const session = {
    inputSources: poses.controllers.map((c, i) => ({
      handedness: i === 0 ? "left" : "right",
      gripSpace: { which: i },
      targetRaySpace: { which: i },
      // Driven per frame from poseAt().trigger, so the harness can work
      // through the calibration the same way a person does.
      gamepad: { buttons: Array.from({ length: 6 }, () => ({ pressed: false, value: 0 })) },
    })),
    renderState: { baseLayer: null },
    updateRenderState(s) { if (s && s.baseLayer) session.renderState.baseLayer = s.baseLayer; },
    requestAnimationFrame(cb) { rafs.push(cb); },
    addEventListener(k, fn) { (listeners[k] = listeners[k] || []).push(fn); },
    requestReferenceSpace: (kind) =>
      kind === "local-floor" ? Promise.resolve({ kind }) : Promise.reject(new Error("no")),
    end() {},
    _pump(frame) {
      const due = rafs.splice(0, rafs.length);
      for (const cb of due) cb(performance.now(), frame);
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
  const store = new Map([["box3d.floorY", "0"]]);   // floor done, span is measured below

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
          const b = readFileSync(path.join(DOCS, url));
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
  for (let f = 0; f < frames; f++) {
    const p = poseAt(f);
    poses.head = p.head;
    poses.controllers.forEach((c, i) => { c.pos = p.controllers[i].pos; c.q = p.controllers[i].q; });
    session.inputSources.forEach((src) => {
      src.gamepad.buttons[0].pressed = !!p.trigger;
      src.gamepad.buttons[0].value = p.trigger ? 1 : 0;
    });
    const before = drawLog.length;
    try {
      if (session._pump(makeFrame(session, poses)) === 0) throw new Error("frame loop stopped rescheduling");
    } catch (e) {
      errors.push(`frame ${f}: ${e && e.stack ? e.stack.split("\n")[0] : e}`);
      break;
    }
    perFrame.push(drawLog.slice(before));
  }

  return { drawLog, perFrame, errors, status: el("status").textContent, report: el("report").textContent };
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

(async () => {
  const page = process.argv[2] || "mech.html";
  console.log(`--- ${page} ---`);

  const r = await runPage(page, 190, pose);

  check("no exception escaped the frame loop", r.errors.length === 0,
        (r.errors[0] || "") + "  [page said: " + r.status + "]");
  check("the page never reported a fault", !/Broke in the frame loop|FAULT/.test(r.status + r.report),
        r.status);

  const last = r.perFrame[r.perFrame.length - 1] || [];
  check("the last frame drew something", last.length > 0, `${last.length} boxes`);
  // 49 grid + head marker + 2 controllers + torso + 6 limb segments + 6 blocks
  // + 4 material pips = 69. Anything near that means the world is really there.
  check("the last frame drew the whole scene", last.length >= 60, `${last.length} boxes`);

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
