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

  let pass = 0, fail = 0;
  const check = (name, ok, detail) => {
    if (ok) { pass++; console.log(`PASS  ${name}`); }
    else { fail++; console.log(`FAIL  ${name}  ${detail || ""}`); }
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
  const mech = () => { const p = E.w_mech_state() >>> 2; return mem().slice(p, p + 49); };
  const joints = () => { const p = E.w_mech_joints() >>> 2; return mem().slice(p, p + 6); };
  // The end of the left arm — the far face of the attachment, which is the
  // point the engine hauls toward your hand. Body 3's origin is its wrist end,
  // one attachment-length short of that, so comparing that against the hand
  // would report a permanent 18 cm error that is not real.
  const tipL = (f) => {
    const o = 21, x = f[o+3], y = f[o+4], z = f[o+5], w = f[o+6];
    const d = -2 * 0.091;                       // thickness 0.07 * 1.3, doubled
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
    E.w_mech_create(0, CHEST, 0, 0.40, 0.46, 0.07, density, 25, 16, cone);
    E.w_mech_tune(2000, 1.0, 3000, 900);
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
  for (const d of [350, 800, 1600, 2800]) {
    const e = restErrAt(d);
    check(`held still, the arm reaches the hand (density ${d})`, e < 0.08,
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

  // Weight shows up as momentum: a heavier arm takes longer to get going and
  // longer to stop, so it falls further behind a *moving* hand. Measured while
  // moving, not while still — standing still it should track regardless.
  const movingLagAt = (density) => {
    build(density);
    let lag = [];
    for (let s = 0; s <= 500; s++) {
      const t = s / 72;
      const h = [-0.30 + 0.18*Math.sin(t*1.6), 1.15 + 0.22*Math.sin(t*1.1), -0.35 - 0.22*Math.sin(t*2.1)];
      E.w_mech_hand(0, ...h, 1, 0, 0, 0, 1);
      E.w_mech_hand(1, -h[0], h[1], h[2], 1, 0, 0, 0, 1);
      E.w_mech_stand(0, CHEST, 0); E.w_mech_apply(); E.w_step(1 / 72);
      if (s > 150) { const tp = tipL(mech()); lag.push(Math.hypot(tp[0]-h[0], tp[1]-h[1], tp[2]-h[2])); }
    }
    return lag.reduce((a, b) => a + b, 0) / lag.length;
  };
  const lightLag = movingLagAt(350), heavyLag = movingLagAt(2800);
  check("heavier arms fall further behind a moving hand", heavyLag > lightLag * 1.4,
        `light ${(lightLag*100).toFixed(1)} cm, heavy ${(heavyLag*100).toFixed(1)} cm`);

  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error("HARNESS ERROR:", e); process.exit(2); });
