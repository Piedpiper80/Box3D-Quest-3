// Verification harness for box3d.wasm — real simulation checks, no rendering.
const { readFileSync } = require("fs");
const { WASI } = require("node:wasi");

const wasi = new WASI({ version: "preview1" });

(async () => {
  const bytes = readFileSync(__dirname + "/box3d.wasm");
  const { instance } = await WebAssembly.instantiate(bytes, {
    wasi_snapshot_preview1: wasi.wasiImport,
  });
  wasi.initialize(instance);
  const E = instance.exports;
  const mem = () => new Float32Array(E.memory.buffer);

  const read = () => {
    const n = E.w_count();
    const ptr = E.w_state() >>> 2;
    const f = mem();
    const out = [];
    for (let i = 0; i < n; i++) {
      const o = ptr + i * 9;
      out.push({
        p: [f[o], f[o + 1], f[o + 2]],
        q: [f[o + 3], f[o + 4], f[o + 5], f[o + 6]],
        h: f[o + 7],
        c: f[o + 8],
      });
    }
    return out;
  };

  let pass = 0, fail = 0, gaps = 0;
  const check = (name, ok, detail) => {
    if (ok) { pass++; console.log(`PASS  ${name}`); }
    else { fail++; console.log(`FAIL  ${name}  ${detail || ""}`); }
  };
  // For behaviour that is known to be wrong and is being worked on. It reports
  // the real measurement every run rather than being deleted or quietly
  // loosened until it passes, so the gap stays visible and the number stays
  // honest.
  const gap = (name, ok, detail) => {
    if (ok) { pass++; console.log(`PASS  ${name}`); }
    else { gaps++; console.log(`GAP   ${name}  ${detail || ""}`); }
  };

  // --- init ---
  E.w_init();
  const n0 = E.w_count();
  check("init creates 20 cubes", n0 === 20, `got ${n0}`);

  const before = read();
  const maxY0 = Math.max(...before.map((c) => c.p[1]));
  check("cubes start elevated", maxY0 > 1.5, `maxY=${maxY0.toFixed(3)}`);

  // --- settle: 6 simulated seconds at 72 Hz ---
  for (let i = 0; i < 432; i++) E.w_step(1 / 72);
  const settled = read();

  const minY = Math.min(...settled.map((c) => c.p[1] - c.h));
  check("no cube below the floor", minY > -0.02, `min bottom=${minY.toFixed(4)}`);

  const maxY = Math.max(...settled.map((c) => c.p[1]));
  check("cubes fell (tower height collapsed)", maxY < maxY0 - 0.5,
    `maxY ${maxY0.toFixed(2)} -> ${maxY.toFixed(2)}`);

  const grounded = settled.filter((c) => c.p[1] - c.h < 0.15).length;
  check("most cubes near the ground", grounded >= 14, `${grounded}/20 grounded`);

  const badQuat = settled.filter((c) => {
    const [x, y, z, w] = c.q;
    return Math.abs(Math.hypot(x, y, z, w) - 1) > 0.01;
  }).length;
  check("rotations are unit quaternions", badQuat === 0, `${badQuat} bad`);

  const nan = settled.filter((c) => c.p.some((v) => !Number.isFinite(v))).length;
  check("no NaN positions", nan === 0, `${nan} NaN`);

  // --- throw a cube forward (-z), like a trigger pull ---
  E.w_spawn(0, 1.3, 0, 0, 1.0, -4.5, 0.07, 1);
  check("spawn added a cube", E.w_count() === 21, `got ${E.w_count()}`);
  for (let i = 0; i < 36; i++) E.w_step(1 / 72); // 0.5 s of flight
  const thrown = read()[20];
  check("thrown cube traveled forward", thrown.p[2] < -1.0, `z=${thrown.p[2].toFixed(3)}`);
  for (let i = 0; i < 216; i++) E.w_step(1 / 72); // 3 s to land
  const landed = read()[20];
  check("thrown cube landed on floor", Math.abs(landed.p[1] - landed.h) < 0.12,
    `y=${landed.p[1].toFixed(3)} h=${landed.h.toFixed(3)}`);

  // --- recycling: overflow the cube budget ---
  // Derived from the module rather than hard-coded: this assertion previously
  // baked in 96 and quietly went stale when the cap was raised.
  const cap = E.w_capacity();
  for (let i = 0; i < cap + 50; i++) E.w_spawn(0, 1, 0, 0, 0, -2, 0.06, 2);
  check("cube cap respected", E.w_count() <= cap, `got ${E.w_count()}, cap ${cap}`);
  check("recycling engaged at the cap", E.w_count() === cap, `got ${E.w_count()}, cap ${cap}`);
  for (let i = 0; i < 72; i++) E.w_step(1 / 72);
  const after = read();
  const nan2 = after.filter((c) => c.p.some((v) => !Number.isFinite(v))).length;
  check("stable after recycling storm", nan2 === 0, `${nan2} NaN`);

  // --- benchmark scene construction ---
  E.w_reset(0);
  check("reset empties the world", E.w_count() === 0, `got ${E.w_count()}`);
  E.w_fill(200);
  check("fill builds the requested pile", E.w_count() === 200, `got ${E.w_count()}`);

  // --- perf: how expensive is a step at a defined load? ---
  // Measured on the 200-body scene just built, rather than on whatever bodies
  // happened to survive the recycling storm — a controlled load makes this
  // number reproducible and comparable between runs.
  for (let i = 0; i < 30; i++) E.w_step(1 / 72); // let the pile form
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < 200; i++) E.w_step(1 / 72);
  const usPerStep = Number(process.hrtime.bigint() - t0) / 200 / 1000;
  console.log(`perf: ${usPerStep.toFixed(0)} us/step with ${E.w_count()} cubes (budget: 13888 us/frame @72Hz)`);
  check("step fits a 72 Hz frame budget", usPerStep < 6000, `${usPerStep.toFixed(0)} us`);

  // --- the mech ---
  //
  // Four separate faults were live here at once, each hiding the others, and
  // none of them showed up as an error: a cone limit past the quarter turn
  // Box3D allows (the assert is compiled out of a release build), an elbow
  // hinged along the arm's own length so it could only twist, that elbow's fold
  // direction fenced off by its limits, and two controllers with gains scaled
  // by mass where the dynamics wanted rotational inertia. Every one of them is
  // silent — the machine just thrashed. These pin the behaviour instead.
  const HEAD = 1.62, CHEST = HEAD - 0.40;
  // What the page computes from a measured arm span, kept in step with it.
  const SPAN = 1.75, SHOULDER_HALF = SPAN * 0.11, TOOL_LEN = 2 * 0.06 * 1.0;
  const BONES = SPAN * 0.5 - SHOULDER_HALF - TOOL_LEN;
  const UPPER = BONES * 0.55, FORE = BONES * 0.45;
  const mech = () => { const p = E.w_mech_state() >>> 2; return mem().slice(p, p + 49); };
  const joints = () => { const p = E.w_mech_joints() >>> 2; return mem().slice(p, p + 6); };
  // The end of the left arm — the far face of the attachment, which is the
  // point the engine hauls toward your hand. Body 3's origin is its wrist end,
  // one attachment-length short of that, so comparing that against the hand
  // would report a permanent 18 cm error that is not real.
  const tipL = (f) => {
    const o = 21, x = f[o+3], y = f[o+4], z = f[o+5], w = f[o+6];
    const d = -2 * (0.06 * 1.0);               // thickness * mount multiplier
    // rotate (0, 0, d) by the attachment's quaternion
    const tx = 2 * (y * d), ty = 2 * (-x * d);
    return [f[o] + w*tx + y*0 - z*ty,
            f[o+1] + w*ty + z*tx - x*0,
            f[o+2] + d + (x*ty - y*tx)];
  };
  const tiltOf = (f) => Math.acos(Math.max(-1, Math.min(1, 1 - 2 * (f[3]*f[3] + f[5]*f[5])))) * 180 / Math.PI;

  const build = (density, cone = 1.5707) => {
    E.w_reset(1, 0);
    // Joint friction matched to docs/mech.html, or this measures a machine the
    // headset never runs.
    // Arm proportions from a measured 1.75 m span, as the page derives them.
    E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, density, 4, 3, cone, SHOULDER_HALF);
  };
  const drive = (steps, target) => {
    for (let s = 0; s < steps; s++) {
      if (target) { E.w_mech_hand(0, ...target, 1, 0, 0, 0, 1); E.w_mech_hand(1, -target[0], target[1], target[2], 1, 0, 0, 0, 1); }
      E.w_mech_stand(0, CHEST, 0);
      E.w_mech_apply();
      E.w_step(1 / 72);
    }
    return mech();
  };

  // Stands under the weight of its own arms. Before the uprighting torque was
  // derived from inertia this reached 54 degrees and kept going.
  build(800);
  let f = drive(400, null);
  check("the machine stands up on its own", tiltOf(f) < 12, `tilted ${tiltOf(f).toFixed(1)} deg`);
  check("it holds its height", Math.abs(f[1] - CHEST) < 0.06, `y ${f[1].toFixed(3)} vs ${CHEST}`);

  // Settles rather than drifting. A fixed target cannot cause motion, so any
  // steady drift is a controller adding energy — which is how the unstable aim
  // gains were found.
  build(800);
  drive(300, [-0.30, 1.15, -0.35]);
  const a = tipL(mech());
  drive(120, [-0.30, 1.15, -0.35]);
  const b = tipL(mech());
  const drift = Math.hypot(b[0]-a[0], b[1]-a[1], b[2]-a[2]);
  check("the arm settles instead of drifting", drift < 0.03, `moved ${drift.toFixed(3)} m while the hand was still`);

  // The elbow is a hinge, and it folds. It used to sit pinned at its limit in
  // every pose with the arm stretched out full length past a near target.
  build(800);
  E.w_mech_pin(1);
  drive(400, [-0.25, 1.20, -0.20]);              // a hand close to the chest
  const jf = joints(), elbow = jf[1] * 180 / Math.PI;
  check("the elbow folds when the hand is close", elbow < -40, `elbow at ${elbow.toFixed(0)} deg`);
  const sh = [-0.22, CHEST + 0.17, 0], t = tipL(mech());
  const extended = Math.hypot(t[0]-sh[0], t[1]-sh[1], t[2]-sh[2]);
  check("the arm shortens to reach a near target", extended < 0.6, `arm out to ${extended.toFixed(2)} m for a 0.28 m reach`);

  // A cone past a quarter turn is out of range for Box3D and must be clamped
  // rather than handed to the solver.
  build(800, 2.2);
  E.w_mech_pin(1);
  f = drive(400, [-0.30, 1.15, -0.35]);
  const finite = [...f].every((v) => Number.isFinite(v));
  check("an over-wide cone is clamped, not passed through", finite, "NaN in the mech state");

  // The arm has to sit where the hand is when the hand is still.
  //
  // This is the check that was missing, and it is the one that matters most.
  // The arm used to hang 13 cm below the hand at the lightest weight and 38 cm
  // at the heaviest, nearly all of it straight down, because nothing carried
  // the arm's own weight. That was defended as the weight signal for a whole
  // round of work. It is not one — it is the arm visibly failing to follow you,
  // which is the single thing this spike has to get right.
  const still = [-0.30, 1.15, -0.35];
  const restErrAt = (density) => {
    build(density);
    const s = drive(400, still), tp = tipL(s);
    return Math.hypot(tp[0]-still[0], tp[1]-still[1], tp[2]-still[2]);
  };
  //
  // This was 7-22 cm depending on the pose until the machine stopped colliding
  // with itself. The folded arm was pressing its own forearm and mount into the
  // torso, and a contact holds a spring off its target indefinitely.
  for (const d of [350, 800, 1600, 2800]) {
    const e = restErrAt(d);
    check(`held still, the arm reaches the hand (density ${d})`, e < 0.05,
          `${(e*100).toFixed(1)} cm short`);
  }

  // And it has to keep articulating at every weight. The heaviest arm used to
  // lock out dead straight — 1 to 6 degrees of elbow across a whole session —
  // because the pull had nothing spare to fold it with once it was also
  // holding the arm up.
  const elbowRangeAt = (density) => {
    build(density);
    let lo = 999, hi = -999;
    for (let s = 0; s <= 500; s++) {
      const t = s / 72;
      const h = [-0.30 + 0.18*Math.sin(t*1.6), 1.15 + 0.22*Math.sin(t*1.1), -0.35 - 0.22*Math.sin(t*2.1)];
      E.w_mech_hand(0, ...h, 1, 0, 0, 0, 1);
      E.w_mech_hand(1, -h[0], h[1], h[2], 1, 0, 0, 0, 1);
      E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
      if (s > 150) { const a = joints()[1] * 180 / Math.PI; lo = Math.min(lo, a); hi = Math.max(hi, a); }
    }
    return hi - lo;
  };
  for (const d of [350, 2800]) {
    const r = elbowRangeAt(d);
    check(`the elbow keeps working at density ${d}`, r > 15, `only ${r.toFixed(0)} deg of travel`);
  }

  // Weight shows up as momentum, so it has to be measured with a motion that
  // has some. A gentle wave reported only 1.3x between the lightest and
  // heaviest arm and read as "weight barely matters" — but nothing is being
  // accelerated hard enough there for mass to bite. A punch is the motion this
  // game is made of, and it separates them properly.
  const punchLagAt = (density) => {
    build(density);
    const rest = [-0.28, 1.20, -0.20], out = -0.62;
    let peak = 0;
    for (let s = 0; s <= 400; s++) {
      // 0.42 m forward in about 0.15 s, then stop dead.
      const t = s < 150 ? 0 : Math.min(1, (s - 150) / 11);
      const h = [rest[0], rest[1], rest[2] + (out - rest[2]) * t];
      E.w_mech_hand(0, ...h, 1, 0, 0, 0, 1);
      E.w_mech_hand(1, -h[0], h[1], h[2], 1, 0, 0, 0, 1);
      E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
      if (s >= 150) {
        const tp = tipL(mech());
        peak = Math.max(peak, Math.hypot(tp[0]-h[0], tp[1]-h[1], tp[2]-h[2]));
      }
    }
    return peak;
  };
  // Box3D's joint springs are mass-normalised, which on its own would make a
  // 3 kg and a 26 kg arm punch identically. Each joint's spring rate is derived
  // from its own inertia instead, so a fixed actuator stiffness gives a heavy
  // limb a lower natural frequency — which is what being heavy means.
  const lightLag = punchLagAt(350), heavyLag = punchLagAt(2800);
  check("a heavy arm is harder to throw a punch with", heavyLag > lightLag * 1.8,
        `light ${(lightLag*100).toFixed(1)} cm behind, heavy ${(heavyLag*100).toFixed(1)} cm`);

  // The elbow has to sit where an elbow sits.
  //
  // A ball shoulder plus a pull at the wrist leaves one degree of freedom that
  // nothing determines: the elbow can be anywhere on a circle about the
  // shoulder-to-wrist line. Left alone it settled at 167 degrees — pointing
  // very nearly straight up, a chicken wing — and wandered 21 degrees while the
  // hand moved. Zero here means hanging straight down, which is where yours is.
  const elbowSwivel = () => {
    const f = mech();
    const s = [f[7], f[8], f[9]], e = [f[14], f[15], f[16]], w = [f[21], f[22], f[23]];
    const ax = [w[0]-s[0], w[1]-s[1], w[2]-s[2]];
    const L = Math.hypot(...ax); if (L < 0.12) return null;
    for (let i = 0; i < 3; i++) ax[i] /= L;
    const off = [e[0]-s[0], e[1]-s[1], e[2]-s[2]];
    const oa = off[0]*ax[0] + off[1]*ax[1] + off[2]*ax[2];
    const u = [off[0]-ax[0]*oa, off[1]-ax[1]*oa, off[2]-ax[2]*oa];
    // Near a straight arm the elbow's position round the axis is ill-defined
    // and the measurement is noise, so it is not counted.
    const ul = Math.hypot(...u); if (ul < 0.05) return null;
    for (let i = 0; i < 3; i++) u[i] /= ul;
    const d = [ax[0]*ax[1], -1 + ax[1]*ax[1], ax[2]*ax[1]];
    const dl = Math.hypot(...d);
    for (let i = 0; i < 3; i++) d[i] /= dl;
    return Math.acos(Math.max(-1, Math.min(1, u[0]*d[0] + u[1]*d[1] + u[2]*d[2]))) * 180 / Math.PI;
  };
  build(800);
  let swiv = [];
  for (let s = 0; s <= 500; s++) {
    const t = s / 72;
    const h = [-0.28 + 0.15*Math.sin(t*1.6), 1.15 + 0.18*Math.sin(t*1.1), -0.30 - 0.16*Math.sin(t*2.1)];
    E.w_mech_hand(0, ...h, 1, 0, 0, 0, 1);
    E.w_mech_hand(1, -h[0], h[1], h[2], 1, 0, 0, 0, 1);
    E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
    if (s > 200) { const a = elbowSwivel(); if (a !== null) swiv.push(a); }
  }
  const swAvg = swiv.reduce((a, b) => a + b, 0) / swiv.length;
  check("the elbow stays low rather than sticking up", swAvg < 75,
        `sitting ${swAvg.toFixed(0)} deg off straight-down`);

  check("the elbow does not wander", Math.max(...swiv) - Math.min(...swiv) < 60,
        `${(Math.max(...swiv) - Math.min(...swiv)).toFixed(0)} deg of drift`);

  // --- knuckle-haul locomotion (legs gone) ---
  //
  // The fallback the locomotion ladder rests on: a machine that has lost its
  // legs lies on the ground and drags itself with both fists. Standing at full
  // height the fists cannot reach the floor at all — measured, the tip bottoms
  // out 19 cm up — which is the geometry saying this mechanic belongs to the
  // collapsed machine, not the standing one.
  const heave = (density, cycles) => {
    E.w_reset(1, 0);
    E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, density, 4, 3, 1.5707, SHOULDER_HALF);
    E.w_mech_legs(0);
    const HEAD_TRACK = HEAD, O = { x: 0, y: 0, z: 0 };
    const PULL = 30, SWING = 34, CYCLE = PULL + SWING;
    let peak = 0, f = null;
    for (let s = 0; s < 60 + cycles * CYCLE + 160; s++) {
      // The stroke's height is given in WORLD terms, because that is what a
      // player does: they watch the fist and put it on the ground, whatever
      // their own height. Scripting it in tracking space made the gait depend
      // on the head height — at 1.62 m the pull stroke mapped below the floor,
      // the machine did a push-up on its own fists and lurched backwards.
      let grips = [0, 0], hz = -0.25, hy = 0.60;
      if (s >= 60 && s < 60 + cycles * CYCLE) {
        const c = (s - 60) % CYCLE;
        if (c < PULL) { grips = [1, 1]; hz = -0.45 + 0.5 * (c / PULL); hy = 0.10; }
        else { const t = (c - PULL) / SWING; hz = 0.05 - 0.5 * t; hy = 0.14 + 0.14 * Math.sin(t * Math.PI); }
      }
      for (let i = 0; i < 2; i++) {
        E.w_mech_anchor(i, grips[i]);
        E.w_mech_hand(i, (i === 0 ? -0.24 : 0.24) + O.x, hy, hz + O.z, 1, 0, 0, 0, 1);
      }
      E.w_mech_stand(O.x, CHEST + O.y, O.z);
      E.w_mech_apply(); E.w_step(1 / 72);
      f = mech();
      const dp = E.w_mech_drag_state() >>> 2, dd = mem();
      O.y = (f[1] + 0.40) - HEAD_TRACK;
      if (dd[dp + 4] > 0.5) { O.x = f[0]; O.z = f[2]; }
      peak = Math.max(peak, dd[dp + 5]);
    }
    return { dist: -f[2], peak, y: f[1], tilt: tiltOf(f) };
  };

  const hMed = heave(800, 4);
  check("legs gone, the hull rests on the ground", hMed.y < 0.30, `y ${hMed.y.toFixed(2)}`);
  check("four heaves drag the machine over a metre", hMed.dist > 1.0, `${hMed.dist.toFixed(2)} m`);
  check("it stays flat while dragging", hMed.tilt < 15, `tilt ${hMed.tilt.toFixed(0)} deg`);
  const hLight = heave(350, 4), hHeavy = heave(2800, 4);
  check("a heavy machine is much harder to drag", hLight.dist > hHeavy.dist * 2,
        `light ${hLight.dist.toFixed(2)} m, heavy ${hHeavy.dist.toFixed(2)} m`);

  // Squeezing in mid-air grabs nothing.
  E.w_reset(1, 0);
  E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, 800, 4, 3, 1.5707, SHOULDER_HALF);
  for (let s = 0; s < 200; s++) {
    E.w_mech_anchor(0, 1);
    E.w_mech_hand(0, -0.24, 1.0, -0.35, 1, 0, 0, 0, 1);
    E.w_mech_hand(1, 0.24, 1.0, -0.35, 1, 0, 0, 0, 1);
    E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
  }
  let fAir = mech();
  const airDp = E.w_mech_drag_state() >>> 2;
  check("gripping mid-air anchors nothing and moves nothing",
        mem()[airDp] === 0 && Math.hypot(fAir[0], fAir[2]) < 0.05,
        `anchored ${mem()[airDp]}, moved ${Math.hypot(fAir[0], fAir[2]).toFixed(3)} m`);

  // And when the legs come back, the machine stands back up.
  E.w_reset(1, 0);
  E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, 800, 4, 3, 1.5707, SHOULDER_HALF);
  E.w_mech_legs(0);
  for (let s = 0; s < 200; s++) { E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72); }
  const collapsedY = mech()[1];
  E.w_mech_legs(1);
  for (let s = 0; s < 300; s++) { E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72); }
  const stoodY = mech()[1];
  check("legs restored, it stands back up", collapsedY < 0.30 && Math.abs(stoodY - CHEST) < 0.06,
        `collapsed ${collapsedY.toFixed(2)}, stood ${stoodY.toFixed(2)}, target ${CHEST.toFixed(2)}`);

  // The machine no longer collides with itself, which must not also mean it
  // stopped colliding with everything else.
  E.w_reset(1, 0);
  E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, 1600, 4, 3, 1.5707, SHOULDER_HALF);
  E.w_spawn(-0.28, 1.20, -0.62, 0, 0, 0, 0.10, 0);
  const blockAt = () => { const p = E.w_state() >>> 2, m = mem(); return [m[p], m[p+1], m[p+2]]; };
  const wasAt = blockAt();
  for (let s = 0; s <= 300; s++) {
    const t = s < 100 ? 0 : Math.min(1, (s - 100) / 11);
    const h = [-0.28, 1.20, -0.24 + (-0.66 + 0.24) * t];
    E.w_mech_hand(0, ...h, 1, 0, 0, 0, 1);
    E.w_mech_hand(1, -h[0], h[1], h[2], 1, 0, 0, 0, 1);
    E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
  }
  const nowAt = blockAt();
  const knocked = Math.hypot(nowAt[0]-wasAt[0], nowAt[1]-wasAt[1], nowAt[2]-wasAt[2]);
  check("a punch still knocks a block flying", knocked > 0.3, `moved ${(knocked*100).toFixed(0)} cm`);

  // --- voxel core (Phase 2) ---
  //
  // A voxel is a cell in a grid, not a body. The solver sees greedy-merged
  // static runs, a capped debris pool, and single chunk bodies for regions
  // that lose their connection to the ground. Damage is momentum through
  // contact hit events, so fists, thrown blocks and falling chunks all use
  // one mechanism.
  const vst = () => { const p = E.w_vox_stats() >>> 2; return mem().slice(p, p + 5); };
  const vstep = () => { E.w_mech_apply(); E.w_step(1 / 72); E.w_vox_post(); };
  const vpunch = (x, y) => {
    for (let s = 0; s < 50; s++) {
      const t = s / 50, out = t < 0.4 ? t / 0.4 : Math.max(0, 1 - (t - 0.4) / 0.5);
      E.w_mech_hand(1, x, y, -0.20 - 0.45 * out, 1, 0, 0, 0, 1);
      E.w_mech_hand(0, -0.30, 1.15, -0.25, 1, 0, 0, 0, 1);
      E.w_mech_stand(0, CHEST, 0);
      vstep();
    }
  };
  // Materials: 0 wood, 1 stone, 2 steel.
  const vwall = (density, mat) => {
    E.w_reset(1, 0);
    E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, density, 4, 3, 1.5707, SHOULDER_HALF);
    E.w_vox_create(0, 0, -0.62, 22, 12, 2, 0.10, mat);
  };

  vwall(1600, 1);
  let v = vst();
  check("a 528-cell wall is 24 merged shapes", v[0] === 528 && v[3] <= 26,
        `alive ${v[0]}, shapes ${v[3]}`);

  for (let p = 0; p < 4; p++) vpunch(0.18, 1.05);
  v = vst();
  check("punches knock cells out of the wall", v[1] >= 6, `${v[1]} killed`);
  check("dead cells became debris", E.w_count() >= 6, `${E.w_count()} cubes`);

  const killsAt = (density, mat) => {
    vwall(density, mat);
    for (let p = 0; p < 4; p++) vpunch(0.18, 1.05);
    return vst()[1];
  };
  const kLight = killsAt(350, 1), kHeavy = killsAt(2800, 1);
  check("a heavier arm smashes more wall", kHeavy > kLight * 1.5,
        `light ${kLight}, heavy ${kHeavy}`);

  // Materials: the same light arm guts wood and cannot mark steel.
  const kWood = killsAt(350, 0), kSteel = killsAt(350, 2);
  check("a light arm breaks wood", kWood >= 4, `${kWood} cells`);
  check("the same arm cannot dent steel", kSteel === 0, `${kSteel} cells`);
  const kSteelHeavy = killsAt(2800, 2);
  check("steel yields only to a heavy arm", kSteelHeavy > kSteel,
        `heavy ${kSteelHeavy} vs light ${kSteel}`);

  // Structure: cut a column's waist and the top must fall as one chunk.
  E.w_reset(1, 0);
  E.w_vox_create(0, 0, -0.62, 2, 12, 1, 0.10, 1);
  E.w_vox_blast(0, 0.55, -0.60, 400);
  v = vst();
  const chunkBoxes = E.w_vox_chunk_box_count();
  check("the orphaned top detaches as a chunk", v[2] >= 1 && chunkBoxes >= 1,
        `chunks ${v[2]}, boxes ${chunkBoxes}`);
  const chunkTop = () => {
    const n = E.w_vox_chunk_box_count(); if (!n) return null;
    const p = E.w_vox_chunk_boxes() >>> 2, f = mem();
    let hi = -1e9; for (let i = 0; i < n; i++) hi = Math.max(hi, f[p + i * 11 + 1]);
    return hi;
  };
  const cy1 = chunkTop();
  for (let s = 0; s < 90; s++) { E.w_step(1 / 72); E.w_vox_post(); }
  const cy2 = chunkTop();
  check("the chunk actually falls", cy1 !== null && cy2 !== null && cy1 - cy2 > 0.3,
        `${cy1 === null ? "none" : cy1.toFixed(2)} -> ${cy2 === null ? "none" : cy2.toFixed(2)}`);

  // Budget: level a whole wall at once. Live debris must stay capped and the
  // step must stay far inside the frame even in this slowest build.
  E.w_reset(1, 0);
  E.w_vox_create(0, 0, -0.9, 22, 12, 2, 0.10, 1);
  for (let x = -1.0; x <= 1.0; x += 0.2)
    for (let y = 0.1; y <= 1.1; y += 0.3)
      E.w_vox_blast(x, y, -0.9, 900);
  const tv0 = process.hrtime.bigint();
  for (let s = 0; s < 120; s++) { E.w_step(1 / 72); E.w_vox_post(); }
  const collapseUs = Number(process.hrtime.bigint() - tv0) / 120 / 1000;
  v = vst();
  check("levelling the wall leaves nothing standing", v[0] === 0, `${v[0]} alive`);
  check("live debris stays inside its budget", E.w_count() <= 160, `${E.w_count()} cubes`);
  console.log(`perf: ${collapseUs.toFixed(0)} us/step during total wall collapse`);
  check("a total collapse still fits the frame", collapseUs < 9000, `${collapseUs.toFixed(0)} us`);
  const vp = E.w_state() >>> 2, vf = mem();
  let vbad = 0;
  for (let i = 0; i < E.w_count(); i++) if (!Number.isFinite(vf[vp + i * 9])) vbad++;
  check("rubble has no NaNs", vbad === 0, `${vbad} bad`);

  // --- armour on a moving body (the dummy) ---
  //
  // A grid riding a dynamic body: impacts map through the body's transform,
  // debris and detached slabs leave with the body's pose and velocity. This
  // is the tech a damageable mech is made of.
  E.w_reset(1, 0);
  E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, 1600, 4, 3, 1.5707, SHOULDER_HALF);
  E.w_dummy_create(0, -0.55, 1);
  check("the dummy wears a 64-cell plate", vst()[0] === 64, `${vst()[0]} cells`);

  let maxSwing = 0;
  for (let p = 0; p < 4; p++) {
    for (let sst = 0; sst < 50; sst++) {
      const t = sst / 50, out = t < 0.4 ? t / 0.4 : Math.max(0, 1 - (t - 0.4) / 0.5);
      E.w_mech_hand(1, 0.05, 1.15, -0.18 - 0.42 * out, 1, 0, 0, 0, 1);
      E.w_mech_hand(0, -0.30, 1.15, -0.25, 1, 0, 0, 0, 1);
      E.w_mech_stand(0, CHEST, 0);
      E.w_mech_apply(); E.w_step(1 / 72); E.w_vox_post();
      const dp = E.w_dummy_state() >>> 2;
      maxSwing = Math.max(maxSwing, Math.abs(mem()[dp + 2] + 0.55));
    }
  }
  check("punches swing the whole dummy", maxSwing > 0.10, `${maxSwing.toFixed(2)} m`);
  check("the plate sheds cells while the body moves", vst()[0] < 60,
        `${vst()[0]} of 64 left`);
  check("shed cells fly off as debris", E.w_count() > 5, `${E.w_count()} cubes`);

  // Cut the plate across its waist at its CURRENT swung position: the top
  // half must tear off as a slab born at the body's pose.
  const dp2 = E.w_dummy_state() >>> 2;
  const dz = mem()[dp2 + 2], dy = mem()[dp2 + 1];
  for (let bx = -0.25; bx <= 0.25; bx += 0.07) E.w_vox_blast(bx, dy, dz + 0.16, 60);
  const chunksNow = vst()[2];
  check("a slab can tear off the moving body", chunksNow >= 1 || vst()[0] < 20,
        `chunks ${chunksNow}, cells left ${vst()[0]}`);

  // --- the first fight (arena) ---
  //
  // Enemy machine: same legs, same uprighting, same voxel armour, plus a
  // small will — approach, telegraph, swing. Its core dies to punches once
  // the plate is off; the player's hull is scored the same way.
  E.w_reset(1, 0);
  E.w_mech_create(0, CHEST, 0, UPPER, FORE, 0.06, 1600, 4, 3, 1.5707, SHOULDER_HALF);
  const pPlate = E.w_player_plate(2);
  const ePlate = E.w_enemy_create(0, -3.0, 1);
  const est = () => { const p = E.w_enemy_state() >>> 2; return mem().slice(p, p + 26); };
  const grid = (g) => { const p = E.w_vox_grid_stats(g) >>> 2; return mem().slice(p, p + 3); };
  const fstep = (hx, hz) => {
    E.w_mech_hand(0, -0.22, 1.15, -0.25, 1, 0, 0, 0, 1);
    E.w_mech_hand(1, hx, 1.15, hz, 1, 0, 0, 0, 1);
    E.w_mech_stand(0, CHEST, 0);
    E.w_enemy_update(0, CHEST, 0, 1 / 72);
    E.w_mech_apply(); E.w_step(1 / 72); E.w_vox_post(); E.w_enemy_post();
  };

  for (let s = 0; s < 900; s++) fstep(0.22, -0.25);
  let ev2 = est();
  check("the enemy closes to fighting range", Math.abs(ev2[2] + 0.72) < 0.25,
        `at z ${ev2[2].toFixed(2)}`);
  check("its swings strip the player's plate", grid(pPlate)[0] < 34,
        `${grid(pPlate)[0]} of 36 left`);

  let dead = false, punches = 0;
  for (let p = 0; p < 40 && !dead; p++) {
    for (let s = 0; s < 40; s++) {
      const t = s / 40, out = t < 0.4 ? t / 0.4 : Math.max(0, 1 - (t - 0.4) / 0.5);
      ev2 = est();
      fstep(ev2[0] * out * 0.9, -0.25 + (ev2[2] + 0.25) * out * 0.95);
    }
    punches++;
    dead = est()[7] === 5;
  }
  check("punching back kills it", dead, `still alive after ${punches} punches`);
  check("its death bursts the plate off", grid(ePlate)[0] === 0,
        `${grid(ePlate)[0]} cells still on`);
  for (let s = 0; s < 120; s++) fstep(0.22, -0.25);
  check("the dead machine ends up on the ground", est()[1] < 0.60,
        `resting at y ${est()[1].toFixed(2)} (stood at 1.15)`);

  console.log(`\n${pass} passed, ${fail} failed, ${gaps} known gaps`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error("HARNESS ERROR:", e); process.exit(2); });
