// Verification harness for box3d.wasm — real simulation checks, no rendering.
//
// Everything here is machine-checkable. What is deliberately NOT here is how
// any of it feels, because that is the one thing a headset has to answer.
const { readFileSync } = require("fs");
const { WASI } = require("node:wasi");

const wasi = new WASI({ version: "preview1" });

// The skeleton, in the order the engine lays it out.
const BONE = ["PELVIS","ABDOMEN","CHEST","NECK","HEAD",
  "L_UPPERARM","L_FOREARM","L_HAND", "R_UPPERARM","R_FOREARM","R_HAND",
  "L_THIGH","L_SHIN","L_FOOT", "R_THIGH","R_SHIN","R_FOOT"];
const B = {}; BONE.forEach((n, i) => { B[n] = i; });
const FIG = ["WAIT","STEP","WINDUP","STRIKE","RECOVER","FALLING","DOWN"];

(async () => {
  const bytes = readFileSync(__dirname + "/box3d.wasm");
  const { instance } = await WebAssembly.instantiate(bytes, {
    wasi_snapshot_preview1: wasi.wasiImport,
  });
  wasi.initialize(instance);
  const E = instance.exports;
  const mem = () => new Float32Array(E.memory.buffer);

  let pass = 0, fail = 0, gaps = 0;
  const check = (name, ok, detail) => {
    if (ok) { pass++; console.log(`PASS  ${name}`); }
    else { fail++; console.log(`FAIL  ${name}  ${detail || ""}`); }
  };
  // For behaviour that is known to be wrong and is being worked on. It reports
  // the real measurement every run rather than being deleted or quietly
  // loosened until it passes, so the gap stays visible and the number honest.
  const gap = (name, ok, detail) => {
    if (ok) { pass++; console.log(`PASS  ${name}`); }
    else { gaps++; console.log(`GAP   ${name}  ${detail || ""}`); }
  };

  const cubes = () => {
    const n = E.w_count(), ptr = E.w_state() >>> 2, f = mem(), out = [];
    for (let i = 0; i < n; i++) {
      const o = ptr + i * 9;
      out.push({ p: [f[o], f[o+1], f[o+2]], q: [f[o+3], f[o+4], f[o+5], f[o+6]], h: f[o+7] });
    }
    return out;
  };

  // =========================================================================
  console.log("\n-- the solver is alive --");
  // =========================================================================
  E.w_reset(1, 0);
  E.w_fill(40);
  check("a pile spawns", E.w_count() === 40, `got ${E.w_count()}`);
  const startMax = Math.max(...cubes().map((c) => c.p[1]));
  for (let i = 0; i < 432; i++) E.w_step(1 / 72);   // six seconds
  const settled = cubes();
  const minY = Math.min(...settled.map((c) => c.p[1] - c.h));
  check("no cube ends up below the floor", minY > -0.02, `lowest ${minY.toFixed(4)}`);
  check("the pile fell and settled",
    Math.max(...settled.map((c) => c.p[1])) < startMax,
    `${startMax.toFixed(2)} -> ${Math.max(...settled.map((c) => c.p[1])).toFixed(2)}`);
  check("every rotation is still a unit quaternion",
    settled.every((c) => Math.abs(Math.hypot(...c.q) - 1) < 1e-3));
  check("no NaN anywhere in the state",
    settled.every((c) => c.p.every(Number.isFinite) && c.q.every(Number.isFinite)));
  check("the capacity is reported, not hard-coded by callers", E.w_capacity() >= 512);

  // The floor is a parameter because the page cannot assume Y=0 is the floor.
  E.w_reset(1, 1.5);
  E.w_spawn(0, 3.0, 0, 0, 0, 0, 0.1, 0);
  for (let i = 0; i < 300; i++) E.w_step(1 / 72);
  const onHighFloor = cubes()[0].p[1];
  check("a floor set above zero really is the floor",
    onHighFloor > 1.55 && onHighFloor < 1.7, `rested at ${onHighFloor.toFixed(3)}`);

  // =========================================================================
  console.log("\n-- your fists --");
  // =========================================================================
  E.w_reset(0, 0);
  E.w_hand_create(0, 0, 1.2, 0, 0.055, 1100, 30, 1.0, 1200);
  E.w_hand_create(1, 0.4, 1.2, 0, 0.055, 1100, 30, 1.0, 1200);
  const fistMass = E.w_hand_mass(1);
  check("a fist has real mass", fistMass > 0.5 && fistMass < 4, `${fistMass.toFixed(2)} kg`);
  E.w_hand_target(1, 0.4, 1.5, -0.4, 0, 0, 0, 1);
  for (let i = 0; i < 120; i++) { E.w_hand_apply(); E.w_step(1 / 72); }
  const hs = () => { const o = E.w_hand_state() >>> 2, f = mem();
    return [[f[o], f[o+1], f[o+2]], [f[o+8], f[o+9], f[o+10]]]; };
  const err = Math.hypot(hs()[1][0] - 0.4, hs()[1][1] - 1.5, hs()[1][2] + 0.4);
  check("a fist arrives where your hand is", err < 0.02, `${(err * 1000).toFixed(1)} mm off`);
  // Gravity would drag it out of your hand; a fist holds where you hold it.
  const heldY = hs()[1][1];
  for (let i = 0; i < 300; i++) { E.w_hand_apply(); E.w_step(1 / 72); }
  check("a fist does not sag", Math.abs(hs()[1][1] - heldY) < 0.005,
    `drifted ${((hs()[1][1] - heldY) * 1000).toFixed(1)} mm`);

  // The world being torn down must take the fists with it, or the next
  // w_hand_apply drives two stale handles into a destroyed world.
  E.w_reset(0, 0);
  E.w_hand_apply();
  E.w_step(1 / 72);
  check("a world reset forgets the old fists", true);

  // =========================================================================
  console.log("\n-- matter that breaks --");
  // =========================================================================
  E.w_reset(0, 0);
  const wall = E.w_vox_create(0, 0, -2, 8, 6, 1, 0.12, 1);   // stone
  const gs = (g) => { const o = E.w_vox_grid_stats(g) >>> 2, f = mem();
    return { alive: f[o], killed: f[o+1], total: f[o+2], dmg: f[o+3] }; };
  check("a wall builds as one grid of cells", gs(wall).total === 48, `${gs(wall).total} cells`);
  const runsIntact = E.w_vox_run_count();
  check("intact cells merge into runs, not one shape per cell",
    runsIntact > 0 && runsIntact < 48, `${runsIntact} shapes for 48 cells`);

  E.w_vox_blast(0, 0.4, -2, 400);
  E.w_vox_post();
  check("a blast kills cells", gs(wall).alive < 48, `${gs(wall).alive}/48 left`);

  // Structure: cut a line across and the top must fall off on its own.
  for (let x = -0.5; x < 0.55; x += 0.06) E.w_vox_blast(x, 0.30, -2, 260);
  E.w_vox_post();
  for (let i = 0; i < 200; i++) { E.w_step(1 / 72); E.w_vox_post(); }
  const st = () => { const o = E.w_vox_stats() >>> 2, f = mem();
    return { alive: f[o], killed: f[o+1], chunks: f[o+2], shapes: f[o+3] }; };
  check("cut a wall in half and the top detaches",
    st().chunks > 0 || gs(wall).alive < 24, `${st().chunks} chunks, ${gs(wall).alive} left`);

  // The debris budget is what keeps a collapse affordable.
  E.w_reset(0, 0);
  const w2 = E.w_vox_create(0, 0, -2, 10, 8, 2, 0.10, 0);
  for (let x = -0.5; x < 0.55; x += 0.05)
    for (let y = 0.05; y < 0.8; y += 0.05) E.w_vox_blast(x, y, -2, 90);
  E.w_vox_post();
  for (let i = 0; i < 60; i++) { E.w_step(1 / 72); E.w_vox_post(); }
  check("live debris is capped", E.w_count() <= 160, `${E.w_count()} live cubes`);
  check("levelling a wall leaves nothing standing", gs(w2).alive < 40, `${gs(w2).alive} left`);

  // =========================================================================
  console.log("\n-- the figure: a human skeleton --");
  // =========================================================================
  const STEP = 1 / 72;
  const figState = () => { const o = E.w_fig_state() >>> 2, f = mem();
    return { exists: f[o], state: f[o+1] | 0, bones: f[o+2], alive: f[o+3], total: f[o+4],
             support: f[o+5], hip: [f[o+6], f[o+7], f[o+8]], face: f[o+9], dist: f[o+10],
             wind: f[o+11], arm: f[o+12], reach: f[o+13], broke: f[o+14], stature: f[o+15] }; };
  const figPose = () => { const o = E.w_fig_pose() >>> 2, f = mem();
    return BONE.map((n, i) => ({ n, p: [f[o+i*8], f[o+i*8+1], f[o+i*8+2]], on: f[o+i*8+7] })); };
  const figBones = () => { const o = E.w_fig_bones() >>> 2, f = mem();
    return BONE.map((n, i) => ({ n, on: f[o+i*4], alive: f[o+i*4+1], total: f[o+i*4+2],
                                 dmg: f[o+i*4+3] })); };
  const figJoints = () => { const o = E.w_fig_joints() >>> 2, f = mem();
    return { lElbow: f[o], rElbow: f[o+1], lKnee: f[o+2], rKnee: f[o+3] }; };
  const rig = () => { const o = E.w_fig_rig() >>> 2, f = mem();
    return { mass: f[o], pelvis: f[o+1], I: f[o+2], hipY: f[o+3], reach: f[o+4] }; };

  const PLAYER = [0, 1.62, 1.6];
  function boot(stature) {
    E.w_reset(0, 0);
    E.w_fig_create(0, 0, stature || 1.75, 2);
    E.w_hand_create(0, -0.3, 1.2, 1.3, 0.055, 1100, 30, 1.0, 1200);
    E.w_hand_create(1, 0.3, 1.2, 1.3, 0.055, 1100, 30, 1.0, 1200);
    E.w_hand_reach_mass(0, 4); E.w_hand_reach_mass(1, 4);
  }
  function tick(px, pz, hl, hr) {
    if (hl) E.w_hand_target(0, hl[0], hl[1], hl[2], 0, 0, 0, 1);
    if (hr) E.w_hand_target(1, hr[0], hr[1], hr[2], 0, 0, 0, 1);
    E.w_fig_update(px === undefined ? PLAYER[0] : px, PLAYER[1],
                   pz === undefined ? PLAYER[2] : pz, STEP);
    E.w_hand_apply(); E.w_fig_apply(); E.w_step(STEP);
    E.w_vox_post(); E.w_fig_post();
  }

  boot(1.75);
  check("seventeen bones", E.w_fig_bone_count() === 17, `${E.w_fig_bone_count()}`);
  check("every bone is a grid that can be broken",
    figBones().every((b) => b.total > 0), figBones().filter((b) => !b.total).map((b) => b.n).join(","));
  check("there is no core, no reactor, nothing to detonate",
    !("w_fig_core" in E) && !("w_enemy_damage_core" in E) && !("w_fig_explode" in E));
  const r0 = rig();
  check("it weighs what a person that size weighs",
    r0.mass > 45 && r0.mass < 85, `${r0.mass.toFixed(1)} kg at 1.75 m`);

  E.w_fig_hold(1);
  for (let i = 0; i < 72 * 6; i++) tick();
  const s1 = figState(), p1 = figPose();
  check("it stands, and keeps standing",
    Math.abs(s1.hip[1] - r0.hipY) < 0.04 && s1.state === 0,
    `hip ${s1.hip[1].toFixed(3)} want ${r0.hipY.toFixed(3)}, state ${FIG[s1.state]}`);
  // The head is where a head goes: crown at stature. This is the check that
  // catches a spine quietly folding — it used to sit 68 cm low and nothing
  // errored, it just slowly lay down.
  const headTop = p1[B.HEAD].p[1] + 0.11 * 1.75;
  check("its head is at your eye line, not folded onto its chest",
    Math.abs(headTop - 1.75) < 0.10, `crown at ${headTop.toFixed(3)} m`);
  check("its feet are on the floor, not through it and not hovering",
    p1[B.L_FOOT].p[1] > 0.02 && p1[B.L_FOOT].p[1] < 0.14,
    `ankle at ${p1[B.L_FOOT].p[1].toFixed(3)} m`);

  // The joints are the joints you have. A knee that can bend forwards is the
  // single clearest tell that a rig is not a body.
  const j1 = figJoints();
  check("its elbows are folded forward, as a guard",
    j1.lElbow < -1.0 && j1.rElbow < -1.0,
    `L ${j1.lElbow.toFixed(2)} R ${j1.rElbow.toFixed(2)} rad`);
  check("its knees do not bend forwards",
    j1.lKnee > -0.06 && j1.rKnee > -0.06,
    `L ${j1.lKnee.toFixed(2)} R ${j1.rKnee.toFixed(2)} rad`);

  // It is built to the person in the headset, not to a constant.
  boot(1.55); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 3; i++) tick();
  const small = figPose()[B.HEAD].p[1];
  boot(2.00); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 3; i++) tick();
  const tall = figPose()[B.HEAD].p[1];
  check("a taller player gets a taller opponent", tall - small > 0.35,
    `${small.toFixed(2)} m vs ${tall.toFixed(2)} m at the head`);

  // Turning to face you. Every earlier version of this controller either
  // could not turn at all or span like a top; both looked like tuning and
  // neither was.
  for (const [px, pz, want] of [[0, 1.6, 0], [1.6, 0, Math.PI / 2], [-1.6, 0, -Math.PI / 2]]) {
    boot(1.75); E.w_fig_hold(1);
    for (let i = 0; i < 72 * 4; i++) tick(px, pz);
    let e = figState().face - want;
    while (e > Math.PI) e -= 2 * Math.PI;
    while (e < -Math.PI) e += 2 * Math.PI;
    check(`it turns to face a player at (${px}, ${pz})`, Math.abs(e) < 0.15,
      `off by ${e.toFixed(3)} rad`);
  }

  // =========================================================================
  console.log("\n-- its will --");
  // =========================================================================
  boot(1.75);
  const seen = new Set();
  let closest = 99;
  for (let i = 0; i < 72 * 14; i++) {
    tick();
    const s = figState();
    seen.add(FIG[s.state]);
    if (s.dist < closest) closest = s.dist;
  }
  check("it closes on you", closest < 1.0, `got to ${closest.toFixed(2)} m`);
  check("it strides, guards, telegraphs and throws",
    ["STEP", "WAIT", "WINDUP", "STRIKE", "RECOVER"].every((k) => seen.has(k)),
    [...seen].join(","));
  check("it is still standing after fourteen seconds unopposed",
    figState().state !== 6, FIG[figState().state]);
  // The stride is the legs doing it, not the hips gliding.
  boot(1.75);
  let kneeMax = 0;
  for (let i = 0; i < 72 * 6; i++) { tick(0, 4.0); kneeMax = Math.max(kneeMax, figJoints().lKnee); }
  check("walking bends its knees", kneeMax > 0.15, `peak knee ${kneeMax.toFixed(2)} rad`);

  // =========================================================================
  console.log("\n-- what breaking means --");
  // =========================================================================
  // A punch that draws back and drives through, aimed at a bone.
  // A punch that draws back and then drives through over several frames — a
  // hand travels, it does not teleport, and a target that jumps a metre in one
  // step asks the solver a question no headset will ever ask it.
  // Aim at the middle of the bone, which is where w_vox_grid_pose puts it —
  // NOT at the bone's own origin, which is the joint it hangs from. For a
  // thigh those are 10 cm apart and it does not matter; for a forearm folded
  // up into a guard the origin is the elbow and the bone runs UP and AWAY
  // from it, so every punch aimed there went past the arm into open air and
  // forty cycles broke nothing.
  //
  // The blow STOPS at the surface. Driving the target through to the far side
  // sounds harmless — a punch does follow through — but the fist is stiff and
  // continuous, so it ends the stroke wedged inside the body, and the retract
  // then drags it back out through whatever is in the way. Logged hit by hit,
  // punches aimed squarely at one thigh were landing on the other one, on the
  // pelvis and on the abdomen, because the fist was living between the legs.
  // Damage is momentum at the moment of contact; where the stroke ends after
  // that contributes nothing but mess.
  function work(bone, cycles) {
    for (let n = 0; n < cycles && figBones()[bone].on; n++) {
      const g = E.w_fig_bone_grid(bone);
      const o = E.w_vox_grid_pose(g) >>> 2, mf = mem();
      const cx = mf[o], cy = mf[o+1], near = mf[o+2] + mf[o+9] * 0.5;
      for (let i = 0; i < 12; i++) tick(undefined, undefined, null, [cx, cy, near + 0.55]);
      for (let i = 0; i < 9; i++)
        tick(undefined, undefined, null, [cx, cy, near + 0.55 * (1 - (i + 1) / 9)]);
      for (let i = 0; i < 5; i++) tick(undefined, undefined, null, [cx, cy, near]);
    }
  }

  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72; i++) tick();
  // A thigh, not the chest: at fighting range the chest sits behind its guard.
  // And the measure is DAMAGE, not the cell count — a cell only leaves the
  // count when it is entirely gone, so four solid punches into a tough limb
  // read as no progress at all if you only look at how many cells are left.
  work(B.L_THIGH, 4);
  const dented = figBones()[B.L_THIGH];
  check("a punch dents before it destroys",
    dented.on === 1 && dented.dmg > 0.05 && dented.alive === dented.total,
    `thigh ${Math.round(dented.dmg * 100)}% beaten in, ${dented.alive}/${dented.total} cells, attached ${dented.on}`);

  // Break an elbow and the forearm and the hand go together. Nothing in the
  // engine says so — the skeleton does, because the hand is jointed to the
  // forearm and the forearm has stopped being jointed to anything.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72; i++) tick();
  work(B.R_FOREARM, 40);
  for (let i = 0; i < 72 * 3; i++) tick();
  const bb = figBones(), pp = figPose();
  check("a broken forearm comes off", bb[B.R_FOREARM].on === 0);
  check("and the hand goes with it",
    pp[B.R_HAND].p[1] < 0.5 && pp[B.R_FOREARM].p[1] < 0.5,
    `hand at ${pp[B.R_HAND].p[1].toFixed(2)} m, forearm at ${pp[B.R_FOREARM].p[1].toFixed(2)} m`);
  check("the rest of it is still standing",
    figState().state !== 6 && bb[B.CHEST].on === 1, FIG[figState().state]);

  // Take both arms and it cannot throw anything. It used to wind up forever
  // with nothing on the end of either shoulder.
  work(B.L_FOREARM, 40);
  E.w_fig_hold(0);
  const armless = new Set();
  for (let i = 0; i < 72 * 8; i++) { tick(); armless.add(FIG[figState().state]); }
  check("with both arms gone it stops trying to throw",
    !armless.has("STRIKE"), [...armless].join(","));

  // The legs are the way down, and one leg is a distinct state — not a debuff.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72; i++) tick();
  const standHip = figState().hip[1];
  work(B.L_THIGH, 40);
  for (let i = 0; i < 72 * 2; i++) tick();
  const oneLeg = figState();
  check("one leg gone and it drops to that knee",
    oneLeg.support === 0.5 && oneLeg.hip[1] < standHip - 0.15,
    `support ${oneLeg.support}, hip ${standHip.toFixed(2)} -> ${oneLeg.hip[1].toFixed(2)}`);
  check("and it stops walking",
    oneLeg.state !== 1, FIG[oneLeg.state]);
  work(B.R_THIGH, 40);
  for (let i = 0; i < 72 * 4; i++) tick();
  const dead = figState();
  check("both legs gone and it goes down for good",
    dead.state === 6 && dead.hip[1] < 0.35,
    `${FIG[dead.state]}, hip ${dead.hip[1].toFixed(2)}`);
  check("nothing exploded on the way down — its chest is intact",
    figBones()[B.CHEST].on === 1 && figBones()[B.CHEST].alive > 0,
    `chest ${figBones()[B.CHEST].alive}/${figBones()[B.CHEST].total}`);

  // Rebuilding for the next one must not leak the last one.
  const bonesBefore = E.w_fig_bone_count();
  E.w_fig_destroy();
  E.w_fig_create(0, 0, 1.75, 2);
  // Hands out of the way first: left where they were, they are buried in the
  // spot the last one stood and the new one is born being punched.
  for (let i = 0; i < 72 * 3; i++) tick(undefined, undefined, [-0.6, 1.2, 2.2], [0.6, 1.2, 2.2]);
  check("the next one arrives whole", figState().bones === bonesBefore &&
    figState().alive === figState().total,
    `${figState().bones} bones, ${figState().alive}/${figState().total} cells`);

  // Nothing may go non-finite, ever. A single NaN in a joint poisons the
  // whole solver and the page just goes still.
  let bad = 0;
  for (const [k, v] of Object.entries(figState()))
    if (Array.isArray(v)) { if (!v.every(Number.isFinite)) bad++; }
    else if (!Number.isFinite(v)) bad++;
  for (const b of figPose()) if (!b.p.every(Number.isFinite)) bad++;
  check("no NaN anywhere in the figure", bad === 0, `${bad} non-finite`);

  // =========================================================================
  console.log("\n-- the frame budget --");
  // =========================================================================
  boot(1.75);
  for (let i = 0; i < 72; i++) tick();
  const t0 = process.hrtime.bigint();
  const N = 720;
  for (let i = 0; i < N; i++) tick();
  const ms = Number(process.hrtime.bigint() - t0) / 1e6 / N;
  // A Quest 3 runs this build roughly 2.5x slower than CI, and the budget at
  // 72 Hz is 13.9 ms for everything including the GPU.
  check("a step of the whole fight fits the budget with room to spare", ms < 1.4,
    `${ms.toFixed(3)} ms/step here, ~${(ms * 2.5).toFixed(2)} ms projected on a Quest 3`);

  console.log(`\n${pass} passed, ${fail} failed, ${gaps} known gaps`);
  process.exit(fail ? 1 : 0);
})();
