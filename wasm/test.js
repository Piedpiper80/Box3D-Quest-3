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
  for (let i = 0; i < 120; i++) E.w_spawn(0, 1, 0, 0, 0, -2, 0.06, 2);
  check("cube cap respected", E.w_count() <= 96, `got ${E.w_count()}`);
  for (let i = 0; i < 72; i++) E.w_step(1 / 72);
  const after = read();
  const nan2 = after.filter((c) => c.p.some((v) => !Number.isFinite(v))).length;
  check("stable after recycling storm", nan2 === 0, `${nan2} NaN`);

  // --- perf: how expensive is a step at full load? ---
  const t0 = process.hrtime.bigint();
  for (let i = 0; i < 200; i++) E.w_step(1 / 72);
  const usPerStep = Number(process.hrtime.bigint() - t0) / 200 / 1000;
  console.log(`perf: ${usPerStep.toFixed(0)} us/step with ${E.w_count()} cubes (budget: 13888 us/frame @72Hz)`);
  check("step fits a 72 Hz frame budget", usPerStep < 6000, `${usPerStep.toFixed(0)} us`);

  console.log(`\n${pass} passed, ${fail} failed`);
  process.exit(fail === 0 ? 0 : 1);
})().catch((e) => { console.error("HARNESS ERROR:", e); process.exit(2); });
