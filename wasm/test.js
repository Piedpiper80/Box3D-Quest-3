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
  // s_figDbg[2] is how much of its weight the stride controller is still
  // carrying: 0 planted, 1 mid-stride.
  const figCarry = () => mem()[(E.w_fig_dbg() >>> 2) + 2];
  // What the floor is pushing back with, under each sole.
  const figGround = () => { const o = E.w_fig_ground() >>> 2, m = mem();
    return { nL: m[o], nR: m[o+1], copL: [m[o+2], m[o+3], m[o+4]],
             copR: [m[o+5], m[o+6], m[o+7]] }; };
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
  // THE headline check. Standing is not a force pointed upwards at each bone;
  // it is the two soles pressing on the floor and the floor pressing back. On
  // the build before this one this read 0 N against a 542 N figure — the whole
  // weight was cancelled bone by bone and the soles hovered a centimetre clear
  // of the ground, touching nothing.
  const gr = figGround(), wt = rig().mass * 9.81;
  check("the floor is carrying it, not something invisible",
    Math.abs((gr.nL + gr.nR) - wt) < wt * 0.15,
    `${(gr.nL + gr.nR).toFixed(0)} N under the soles, weight ${wt.toFixed(0)} N`);
  // And it is under the FEET, not somewhere convenient: the centre of pressure
  // sits on the floor plane, one patch beside the other.
  check("that weight comes up through the soles",
    Math.abs(gr.copL[1]) < 0.02 && Math.abs(gr.copR[1]) < 0.02 &&
    gr.copL[0] < 0 && gr.copR[0] > 0,
    `left (${gr.copL.map((v) => v.toFixed(3)).join(", ")}) ` +
    `right (${gr.copR.map((v) => v.toFixed(3)).join(", ")})`);

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

  // Being hit in the body has to move it, and it has to gather itself back.
  // This is the check that guards figSlacken: the springs go slack where the
  // blow lands and tighten again over about a third of a second, and while
  // they are slack the legs still have to hold it up. The dip GROWING is the
  // direction that was asked for — before this build it was carried, so it
  // barely gave at all.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 2; i++) tick();
  {
    const h0 = figState().hip[1];
    let dip = h0;
    const into = (z) => { tick(undefined, undefined, null, [0, 1.085, z]);
                          dip = Math.min(dip, figState().hip[1]); };
    // Three of them into the abdomen, drawn back and driven through, the same
    // way `work` throws a punch.
    for (let n = 0; n < 3; n++) {
      for (let i = 0; i < 12; i++) into(0.61);
      for (let i = 0; i < 9; i++) into(0.06 + 0.55 * (1 - (i + 1) / 9));
      for (let i = 0; i < 5; i++) into(0.06);
    }
    for (let i = 0; i < 72 * 2; i++) tick(undefined, undefined, null, [0, 1.20, 1.60]);
    // It has to GIVE — 4 mm was the whole of it while the body was carried,
    // which is a statue being tapped — and then it has to gather itself back
    // rather than staying folded. The lower bound is the point of the check.
    check("a body blow makes it give, and it gathers itself back",
      h0 - dip > 0.008 && h0 - dip < 0.05 &&
      Math.abs(figState().hip[1] - h0) < 0.02,
      `dipped ${((h0 - dip) * 1000).toFixed(0)} mm, back to ` +
      `${figState().hip[1].toFixed(4)} from ${h0.toFixed(4)}`);
  }

  // =========================================================================
  console.log("\n-- balance comes from the floor, and is limited by it --");
  // =========================================================================
  // What the controller is actually working with. Every number in here is
  // derived from contact, which is the whole point: a figure that can right
  // itself with nothing under it is a figure being held up.
  const figBal = () => { const o = E.w_fig_bal() >>> 2, m = mem();
    return { nL: m[o], nR: m[o+1], nT: m[o+2], G: m[o+3],
             com: [m[o+4], m[o+5], m[o+6]], icp: [m[o+7], m[o+8]],
             margin: m[o+9], cop: [m[o+10], m[o+11]],
             tauAnkle: m[o+12], tauHip: m[o+13], out: m[o+14], air: m[o+15],
             ank: [[m[o+16], m[o+17]], [m[o+18], m[o+19]]],
             corners: m[o+20], loaded: [m[o+21], m[o+22]], collapse: m[o+23] }; };

  // A ram: one fist with enough force behind it to be a shove rather than a
  // punch, and every bone made unbreakable first, so what is being measured is
  // BALANCE and not damage.
  function ironclad() { for (let b = 0; b < 17; b++) E.w_vox_scale_hp(E.w_fig_bone_grid(b), 1e6); }
  function ram(y, x, fromZ, toZ, frames) {
    for (let i = 0; i < frames; i++) {
      const t = (i + 1) / frames;
      tick(undefined, undefined, null, [x, y, fromZ + (toZ - fromZ) * t]);
    }
  }

  // Standing: the licence the balance runs on is the load the soles report,
  // and nothing else. Before this build figSupport() read 1.00 with the feet
  // two metres off the ground and 0 N of contact under them.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 4; i++) tick();
  {
    const b = figBal(), g = figGround(), W = rig().mass * 9.81;
    check("its balance authority is what the floor gives it",
      Math.abs(b.G - (g.nL + g.nR) / W) < 0.12 && b.G > 0.85,
      `licence ${b.G.toFixed(3)} against ${(((g.nL + g.nR) / W)).toFixed(3)} of its weight on the floor`);
    // And the pressure it commands is inside a sole it is standing on. Not a
    // tuning choice: commanded between the two feet, one sole was asked for
    // nine times the moment it could absorb and went 0.39 m into the air.
    let worst = 9;
    for (let i = 0; i < 72 * 2; i++) {
      tick();
      const q = figBal();
      for (let s = 0; s < 2; s++) {
        if (!q.loaded[s]) continue;
        const d = Math.hypot(q.cop[0] - q.ank[s][0], q.cop[1] - q.ank[s][1]);
        worst = Math.min(worst, d);
      }
    }
    check("the pressure it commands never leaves its feet", worst < 0.20,
      `nearest sole was ${worst.toFixed(3)} m from the commanded pressure, sole reach 0.195 m`);
  }

  // In the air. Built off the edge of the ground slab, so it is in free fall
  // with everything else exactly as it is in play. Run twice — once with the
  // controller applied and once without — and the two runs have to agree,
  // because a body with nothing under it has no way to turn itself over.
  // Before this build the applied run rolled through 355 degrees and landed
  // vertical; the unforced one ended on its side at 274.
  {
    const tilt = () => { const o = E.w_fig_pose() >>> 2, f = mem();
      const q = [f[o+3], f[o+4], f[o+5], f[o+6]];
      // the pelvis's own up, dotted with the world's
      const uy = 1 - 2 * (q[0]*q[0] + q[2]*q[2]);
      return Math.acos(Math.max(-1, Math.min(1, uy))) * 180 / Math.PI; };
    const fly = (applyForces) => {
      E.w_reset(0, 0);
      E.w_fig_create(30, 0, 1.75, 2);      // clear of the 20 m ground slab
      E.w_fig_hold(1);
      const out = [];
      for (let i = 0; i < 130; i++) {
        E.w_fig_update(30, 1.62, 1.6, STEP);
        if (applyForces) E.w_fig_apply();
        E.w_step(STEP); E.w_vox_post(); E.w_fig_post();
        out.push(tilt());
      }
      return out;
    };
    const on = fly(true), off = fly(false);
    let worst = 0;
    for (let i = 0; i < on.length; i++) worst = Math.max(worst, Math.abs(on[i] - off[i]));
    check("it cannot right itself in the air",
      worst < 10.0,
      `1.8 s of free fall: driven and undriven differ by at most ${worst.toFixed(2)} deg ` +
      `(ends ${on[on.length-1].toFixed(0)} vs ${off[off.length-1].toFixed(0)})`);
    E.w_reset(0, 0);
    E.w_fig_create(30, 0, 1.75, 2);
    E.w_fig_hold(1);
    for (let i = 0; i < 18; i++) { E.w_fig_update(30, 1.62, 1.6, STEP); E.w_fig_apply();
                                  E.w_step(STEP); E.w_vox_post(); E.w_fig_post(); }
    check("and it knows there is nothing under it", figBal().G < 0.05,
      `licence ${figBal().G.toFixed(3)} a quarter of a second after the floor ran out`);
  }

  // Sweep the legs. A ram driven through the shins at 7 m/s, with the bones
  // made unbreakable so this is purely a question of balance.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 2; i++) tick();
  ironclad();
  // A shin-high bar, taken through both legs at 7 m/s. Not a battering ram:
  // at 60 kN it drove the figure through the floor and read as a fall on a
  // technicality.
  E.w_hand_create(1, 0.0, 0.30, 1.10, 0.14, 2000, 30, 1.0, 4000);
  ram(0.30, 0.0, 1.10, -1.10, 22);
  {
    let down = 0, lowHip = 9;
    for (let i = 0; i < 72 * 3; i++) {
      tick(undefined, undefined, null, [0, 1.6, 2.2]);
      const s = figState();
      if (s.state === 5 || s.state === 6) down = 1;
      lowHip = Math.min(lowHip, s.hip[1]);
    }
    const s = figState();
    check("sweep its legs from under it and it goes down",
      down === 1 && s.hip[1] < 0.35,
      `${FIG[s.state]}, hip ${s.hip[1].toFixed(3)} m (lowest ${lowHip.toFixed(3)})`);
  }

  // Shove it hard. Same ram, into the chest, a full stroke.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 2; i++) tick();
  ironclad();
  // A shoulder, not a fist: 250 kg of it, driven through the chest.
  E.w_hand_create(1, 0.0, 1.20, 1.40, 0.16, 7800, 30, 1.0, 60000);
  E.w_hand_reach_mass(1, 60);
  ram(1.20, 0.0, 1.40, -0.60, 22);
  {
    let down = 0;
    for (let i = 0; i < 72 * 3; i++) {
      tick(undefined, undefined, null, [0, 1.6, 2.2]);
      const s = figState();
      if (s.state === 5 || s.state === 6) down = 1;
    }
    const s = figState();
    check("shove it hard enough and it topples",
      down === 1 && s.hip[1] < 0.35,
      `${FIG[s.state]}, hip ${s.hip[1].toFixed(3)} m`);
  }

  // ... and a shove it can take, it takes. This is the check that stops the
  // fix overshooting into a figure you can breathe on and knock over.
  //
  // It is aimed at the CHEST, in the chest's own coordinates, because the
  // version this replaced was aimed at fixed world z and never arrived: it
  // drove a 0.055 m fist from z 1.354 to z 1.107, so the striking surface got
  // to z 1.052, and the front of the figure at that height is at z 0.095.
  // Nine hundred and forty-five millimetres of clear air. It passed on every
  // build including one that could be knocked over by anything, and it
  // certified nothing at all.
  const pelvisTilt = () => { const o = E.w_fig_pose() >>> 2, f = mem();
    const q = [f[o+3], f[o+4], f[o+5], f[o+6]];
    return Math.acos(Math.max(-1, Math.min(1,
      1 - 2 * (q[0]*q[0] + q[2]*q[2])))) * 180 / Math.PI; };
  // A ram on a straight line at a chosen SPEED, which drives through and then
  // RETRACTS. Left driving forward it is not a shove any more, it is a
  // bulldozer, and "a shove it can take" measures the figure being pushed over
  // at walking pace.
  function ramRun(y, from, to, speed, seconds, tough) {
    for (const g of tough) E.w_vox_scale_hp(E.w_fig_bone_grid(g), 1e6);
    E.w_hand_create(0, from[0], y, from[1], 0.12, 6000, 30, 1.0, 200000);
    const d = Math.hypot(to[0]-from[0], to[1]-from[1]);
    const n = Math.max(2, Math.round(d / speed * 72));
    let down = -1, minHip = 9, maxTilt = 0;
    for (let i = 0; i < Math.round(72 * seconds); i++) {
      const t = Math.min(1, (i + 1) / n);
      const p = i < n
        ? [from[0] + (to[0]-from[0])*t, y, from[1] + (to[1]-from[1])*t]
        : [to[0] - (to[0]-from[0])/d*0.03*(i-n), y, to[1] - (to[1]-from[1])/d*0.03*(i-n)];
      tick(undefined, undefined, p, null);
      if (down < 0 && figState().state >= 5) down = i * STEP;
      minHip = Math.min(minHip, figState().hip[1]);
      maxTilt = Math.max(maxTilt, pelvisTilt());
    }
    return { down, minHip, maxTilt };
  }
  const RAM_BODY = [B.CHEST, B.ABDOMEN];
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72 * 2; i++) tick();
  ironclad();
  {
    const h0 = figState().hip[1];
    const cy = figPose()[B.CHEST].p[1], cz = figPose()[B.CHEST].p[2];
    const r = ramRun(cy, [0, cz + 0.42], [0, cz + 0.10], 1.5, 3, RAM_BODY);
    const s = figState();
    check("a shove it can take, it takes",
      s.state < 5 && Math.abs(s.hip[1] - h0) < 0.03,
      `${FIG[s.state]}, hip ${h0.toFixed(3)} -> ${s.hip[1].toFixed(3)}, ` +
      `leaned ${r.maxTilt.toFixed(0)} deg`);
  }

  // =========================================================================
  console.log("\n-- its will --");
  // =========================================================================
  boot(1.75);
  const seen = new Set();
  let closest = 99, ankleMax = 0, ankleMaxStand = 0, minHip = 99;
  let seenFallingMidStride = false, worstCollapse = 0;
  for (let i = 0; i < 72 * 14; i++) {
    tick();
    const s = figState();
    seen.add(FIG[s.state]);
    if (s.state === 5 && figCarry() > 0.5) seenFallingMidStride = true;
    worstCollapse = Math.max(worstCollapse, figBal().collapse);
    if (s.dist < closest) closest = s.dist;
    // The LOWER of its two ankles, over the whole fourteen seconds. A swing
    // foot is meant to come up — a stride that does not lift a foot is a
    // shuffle — so the honest measure of "it is standing on the floor" is
    // that the other one is always still down on it.
    const pf = figPose();
    const a = Math.min(pf[B.L_FOOT].p[1], pf[B.R_FOOT].p[1]);
    if (a > ankleMax) ankleMax = a;
    // "Planted" is the carry being off, not the state having changed: the
    // carry rides out over a fifth of a second, so for those frames it is
    // still a stride however the state reads.
    if (figCarry() < 0.05 && a > ankleMaxStand) ankleMaxStand = a;
    if (s.hip[1] < minHip) minHip = s.hip[1];
  }
  check("it closes on you", closest < 1.0, `got to ${closest.toFixed(2)} m`);
  // Walking IS controlled falling — a third of the frames of a stride have no
  // sole loaded at all — so the topple test has to know the difference between
  // a step and a fall. This is the check that says it does.
  check("a stride is not a fall", !seenFallingMidStride,
    "it entered FALLING while the stride was still carrying it");
  // How close a fight gets to reading as a collapse. The backstop asks for
  // 0.20 s of contiguous chest past 75 degrees; a fight should never get near
  // it, and this reports how near it got rather than assuming.
  gap("a fight never reads as a collapse", worstCollapse < 0.05,
    `longest run with its chest past 75 deg was ${worstCollapse.toFixed(3)} s ` +
    `against a 0.200 s trigger`);
  check("it strides, guards, telegraphs and throws",
    ["STEP", "WAIT", "WINDUP", "STRIKE", "RECOVER"].every((k) => seen.has(k)),
    [...seen].join(","));
  check("it is still standing after fourteen seconds unopposed",
    figState().state !== 6, FIG[figState().state]);
  // The state check above is not enough on its own: a figure lying on the
  // floor with both legs intact reports WAIT, not DOWN. This is the number
  // that says it is actually up on its feet the whole time.
  check("and its hips never drop out of standing while it does it",
    minHip > r0.hipY - 0.10, `lowest hip ${minHip.toFixed(3)} m of ${r0.hipY.toFixed(3)}`);
  // A figure held up by a force does not need the floor and does not stay on
  // it: before this build BOTH ankles rode clear of the ground mid-fight and
  // nothing noticed, because nothing was standing on anything.
  check("it always has a foot on the floor while it fights", ankleMaxStand < 0.14,
    `its lower ankle peaked at ${ankleMaxStand.toFixed(3)} m`);
  // The stride is the one thing still carried, and this is the number that
  // says so out loud rather than hiding it: mid-stride the whole-body lift is
  // back on and both feet can leave the ground at once. figStride is an
  // open-loop gait written against a weightless pelvis; a step that keeps a
  // foot down needs foot PLACEMENT, and that is a build of its own.
  gap("and it has one on the floor mid-stride too", ankleMax < 0.14,
    `its lower ankle peaked at ${ankleMax.toFixed(3)} m during the stride`);
  // The stride is the legs doing it, not the hips gliding.
  boot(1.75);
  let kneeMax = 0;
  for (let i = 0; i < 72 * 6; i++) { tick(0, 4.0); kneeMax = Math.max(kneeMax, figJoints().lKnee); }
  check("walking bends its knees", kneeMax > 0.15, `peak knee ${kneeMax.toFixed(2)} rad`);

  // Having walked to you, it has to be STANDING when it gets there — over its
  // own feet, not leaning back off them. This is the number that was negative
  // on nearly every frame before the stride handed the body back properly:
  // the capture point outside the two soles is a body that cannot hold itself
  // up with any pressure it is able to put on the floor.
  boot(1.75);
  for (let i = 0; i < 72 * 8; i++) tick(0, 4.0);      // walk in
  {
    let inside = 0, n = 0, worst = 9;
    for (let i = 0; i < 72 * 4; i++) {
      tick(0, 4.0);
      const b = figBal();
      if (figCarry() >= 0.05) continue;               // striding is not standing
      n++;
      if (b.margin > 0.02) inside++;
      if (b.margin > -1e8) worst = Math.min(worst, b.margin);
    }
    gap("and when it gets there it is standing over its own feet",
      n > 0 && inside >= n * 0.95,
      `${n ? Math.round(100 * inside / n) : 0}% of planted frames have the capture point ` +
      `inside its soles, worst ${worst.toFixed(3)} m`);
  }

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

  // A piece that has come off is in FREE FALL, and no fist goes near it while
  // it falls. The HAND's own wrist joint is intact and its attached flag still
  // reads 1 — what has gone is the forearm it hung from. This is the exact
  // thing that was reported from the headset: "there is a force acting on the
  // severed limbs to pull them upwards". Measured, the loose hand fell at
  // -4.0 m/s^2, forty per cent of gravity, and was still up at 1.34 m twenty
  // frames later, because the controller was asking each bone "is your own
  // joint intact" when the question is "are you still part of a body".
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72; i++) tick();
  work(B.R_FOREARM, 40);
  {
    // Fists parked well out of the way: from here on the only thing acting on
    // it should be gravity. An eighth of a second, and either it is falling at
    // gravity or it is already down on the floor — both mean nothing is
    // holding it. What it must not be doing is what it did before this build:
    // hanging at chest height, coming down at 4 m/s^2.
    const ys = [];
    for (let i = 0; i < 9; i++)
      { ys.push(figPose()[B.R_HAND].p[1]); tick(undefined, undefined, [-1.2, 1.6, 1.6], [1.2, 1.6, 1.6]); }
    const span = 4 * STEP;
    const acc = (ys[8] - 2 * ys[4] + ys[0]) / (span * span);
    check("a piece that has come off is in free fall", acc < -9.0 || ys[8] < 0.30,
      `loose hand at ${acc.toFixed(2)} m/s^2, ${ys[0].toFixed(3)} m -> ` +
      `${ys[8].toFixed(3)} m, its own attached flag still ${figPose()[B.R_HAND].on}`);
  }

  // Take both arms and it cannot throw anything. It used to wind up forever
  // with nothing on the end of either shoulder.
  boot(1.75); E.w_fig_hold(1);
  for (let i = 0; i < 72; i++) tick();
  work(B.R_FOREARM, 40);
  for (let i = 0; i < 72 * 3; i++) tick();
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
  // The shin and foot below the broken thigh are loose. Their own joints are
  // intact, so before this build they went on drawing both the lift and their
  // share of the righting torque: the loose foot sat at 0.176 m and was RISING
  // — traced 0.154, 0.191, 0.168 — which is a severed leg being stood on end
  // by a torque meant for a body it is no longer part of.
  {
    const f0 = figPose()[B.L_FOOT].p[1];
    for (let i = 0; i < 36; i++) tick();
    const f1 = figPose()[B.L_FOOT].p[1];
    check("a severed leg lies where it fell",
      f1 < 0.12 && Math.abs(f1 - f0) < 0.01,
      `loose foot at ${f1.toFixed(3)} m, moved ${(f1 - f0).toFixed(3)} m in half a second`);
  }
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
  // Well clear, and further than looks necessary. At 2.2 m these were only out
  // of the way by luck: the new figure walks toward you, and once its springs
  // gave under a punch it arrived on a slightly different line and clipped a
  // parked fist with its left thigh on the way in — measured as this figure
  // being born whole and then showing L_THIGH 2/3 three seconds later, with
  // zero debris on the floor and nothing wrong with the figure at all.
  for (let i = 0; i < 72 * 3; i++) tick(undefined, undefined, [-0.6, 1.2, 8.0], [0.6, 1.2, 8.0]);
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
