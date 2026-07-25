# Phase 0 — Foundations

**Goal:** raise the engine ceiling far enough that the rest of the roadmap is
possible, and produce the one number every later phase depends on.

**Exit criterion:** the maximum dynamic body count that holds a stable 72 Hz
in-headset, measured on-device with the perf HUD. Write the number into
[`ROADMAP.md`](ROADMAP.md) when you have it. Stretch target: ≥1000 dynamic
boxes, which is roughly what one destructible mech needs.

> **On 72 Hz.** Quest 3 also offers 80, 90 and 120. For a physics-heavy game a
> rock-solid 72 beats a dropping 90 every time — inconsistent frame timing is
> what makes people sick, not the absolute number. 72 is the working default;
> revisit once this phase's number is known and the frame budget is understood.

Nothing here is a game feature. The scene at the end of Phase 0 looks almost
identical to the scene at the start — it just runs with an order of magnitude
more in it.

---

## Where things stand

| Constraint | Current | Location |
|---|---|---|
| Box3D revision | `c52908c9` — **16 commits ahead of v0.1.0** | `app/src/main/cpp/CMakeLists.txt` |
| SIMD | disabled | `CMakeLists.txt` — `BOX3D_DISABLE_SIMD ON` |
| Threading | none — `workerCount` left at default | `physics.cpp:114-118` |
| Draw calls | **one per body per eye** | `main.cpp:628-633` |
| Body cap | 400 | `physics.cpp:35` |
| Render item cap | 512 | `main.cpp:54` |
| Physics timestep | variable, tied to frame rate | `physics.cpp:164` |
| Backface culling | disabled | `main.cpp:621` |
| Web build cap | 96 cubes, scalar, single-threaded | `wasm/bridge.c` |

Every one of these is a hard ceiling on the roadmap. They come down in the order
below — each step is independently committable and testable, so a step that goes
wrong can be reverted without losing the others.

---

## Step 1 — Verify the pin (do not bump it)

**This step was originally written as "bump to v0.1.0". That was wrong**, and the
correction matters enough to record rather than quietly delete.

The pinned revision `c52908c9` is **16 commits *ahead* of the v0.1.0 release**
(`8441b4a`, 30 June), dated 2–22 July, and it includes fixes the release does
not — among them a hull-builder face and edge leak. Bumping "to v0.1.0" would
have moved the project *backwards* and reintroduced fixed bugs.

CI already confirms the app compiles and links against this revision, so there
is no drift to repair and nothing to do here but record what the revision
provides. Verified directly from the headers at `c52908c9`:

**Joints** — `b3CreateRevoluteJoint`, `b3CreatePrismaticJoint`,
`b3CreateSphericalJoint`, `b3CreateWeldJoint`, `b3CreateWheelJoint`,
`b3CreateDistanceJoint`, `b3CreateMotorJoint`, `b3CreateParallelJoint`,
`b3CreateFilterJoint`.

> **Spherical joints exist.** Box3D's public README lists only revolute,
> prismatic, distance, motor, weld and wheel, so the working assumption had been
> that shoulders and hips would have to be hinges and every mech would move
> stiffly. The headers say otherwise: `b3CreateSphericalJoint` is there, with
> `b3SphericalJoint_EnableMotor`, `b3SphericalJoint_SetMotorVelocity` and
> `b3SphericalJoint_SetMaxMotorTorque`. Shoulders and hips can be proper
> ball-and-socket with driven motors, which is a materially better mech.

**Joint strength and breaking are buildable as designed** — motors carry
explicit limits (`b3RevoluteJoint_SetMaxMotorTorque`,
`b3PrismaticJoint_SetMaxMotorForce`, `b3SphericalJoint_SetMaxMotorTorque`) and
the load is readable back (`b3RevoluteJoint_GetMotorTorque`,
`b3PrismaticJoint_GetMotorForce`, `b3DistanceJoint_GetMotorForce`). That is
exactly the mechanism Phase 3 and the piloting design assume.

**Shapes** — `b3CreateSphereShape`, `b3CreateCapsuleShape`, `b3CreateHullShape`,
`b3CreateTransformedHullShape`, `b3CreateMeshShape`,
`b3CreateHeightFieldShape`, `b3CreateBakedCompoundShape`.

> `b3CreateTransformedHullShape` is the multi-shape-per-body primitive a compound
> voxel assembly needs — a hull at an offset from the body origin. And
> `b3CreateBakedCompoundShape` looks like a direct fit for Phase 2's merged
> static chunks. Worth investigating properly when Phase 2 starts; it may do the
> expensive part of that phase for us.

**Threading** — `b3WorldDef` carries `uint32_t workerCount` (clamped to
`[1, B3_MAX_WORKERS]`), `b3EnqueueTaskCallback* enqueueTask`,
`b3FinishTaskCallback* finishTask` and `void* userTaskContext`. Critically: **if
the task callbacks are left null and `workerCount > 1`, the engine creates its
own internal threads.** See Step 6 — this may be a one-line change.

**Sleep** — `bool enableSleep`, which is what makes settled voxel debris cheap.

**Done when:** nothing changes. The pin stays where it is. Revisit only when
there's a specific reason, and re-test deliberately when you do.

## Step 2 — Perf HUD

Deliberately *before* the optimisations, so that every later step can be
measured rather than assumed. This is the most valuable single artifact in
Phase 0 and it stays useful for the entire life of the project.

Display, view-locked in a corner:

- frame time (ms) and whether the last frame was missed
- physics step time (ms), separated from render time
- body count, split dynamic / static
- draw calls and instance count
- worker thread count (once Step 6 lands)

Simplest implementation that isn't throwaway: a small bitmap font atlas drawn as
textured quads on a panel locked to the view. If you do Step 4 first, each glyph
is just another instance and the HUD costs one draw call — but resist the
reordering temptation; a crude `logcat` readout in Step 2 that gets upgraded in
Step 4 is better than optimising blind.

**Done when:** you can read live frame and step times inside the headset while
throwing cubes.

## Step 2b — Automated benchmark mode

The stress test must not be a human throwing cubes until it feels bad. That
isn't reproducible, it depends on how fast someone can pull a trigger, and it
produces one fuzzy data point instead of a curve.

Build it into the app as a mode that runs unattended:

- **Ramp** the body count — 50, 100, 200, 400, 800, … — spawning automatically.
- **Hold each level** long enough to measure properly (a few seconds), in two
  regimes, because they cost wildly different amounts:
  - **settled** — bodies at rest and asleep. Nearly free, and the easy number
    that flatters the engine.
  - **agitated** — bodies in motion and colliding, re-thrown on a timer so
    nothing gets to sleep. This is the fight, and it's the number that matters.
- **Record** at each level: mean and 99th-percentile frame time, physics step
  time, render time, body count, draw calls.
- **Stop** when 99th-percentile frame time misses the budget consistently.
- **Emit** results to `logcat` in a parseable form, so a run is captured with
  `adb logcat` and handed straight back for analysis.

Launch it, put the headset down, come back in two minutes. The output is a
curve — where the knee is, and whether physics or rendering hit the wall first —
rather than a single number of unclear provenance.

## Step 2c — Headless physics benchmark

Half of this question doesn't need a headset at all. Box3D is a physics-only
library: it can be stepped with no VR, no rendering and no Android, in a plain
desktop binary. The repo already has the precedent — `wasm/test.js` is a
simulation harness of exactly this kind.

Build a small native benchmark target that steps the same scenes and reports
step time against body count, so that the following can be answered **without
the headset in the loop at all**:

- Does step time grow linearly with body count, or worse?
- Does SIMD (Step 5) actually help, and by how much?
- Does threading (Step 6) actually scale, and at what worker count does it stop
  paying?
- Did a change to the physics code make things slower?

> **What it cannot tell you:** absolute Quest numbers. A desktop or CI CPU is far
> faster than the XR2 Gen 2, and this benchmark says nothing about GPU or render
> cost. It gives *relative* scaling and regression detection — enough to tune
> and verify the physics work before it ever reaches hardware. The absolute
> ceiling still comes from Step 2b on the device.

## Step 3 — Fixed timestep

`physics.cpp:164` currently steps Box3D with whatever `dt` the frame loop hands
it. Variable timesteps make a solver behave differently under load, which is
exactly the condition you're about to start measuring — and it makes every
number from the HUD noisy and non-reproducible.

- Fixed `dt` of 1/72 s with an accumulator.
- Clamp the maximum steps per frame (2 or 3) so a hitch can't spiral into a
  death loop where physics falls further behind every frame.
- Optionally interpolate render transforms between steps; only worth it if
  motion looks stuttery afterwards.

**Done when:** step time is stable frame to frame and the tower settles
identically across runs.

## Step 4 — Instanced rendering

The single biggest ceiling. `main.cpp:628-633` sets two uniforms and issues a
draw call **per body, per eye** — so the current scene costs hundreds of draw
calls per frame and a destructible mech would cost thousands. Everything is the
same cube mesh, so this is textbook instancing.

- Add a per-instance vertex buffer: model matrix (16 floats) plus colour, padded
  to `vec4` for alignment — 20 floats / 80 bytes per instance. At 2000 instances
  that's 160 KB uploaded per frame, which is comfortable.
- A `mat4` instance attribute consumes **four** consecutive attribute locations;
  start instance attributes after whatever the cube mesh already occupies.
- `glVertexAttribDivisor(loc, 1)` on each instance attribute, then a single
  `glDrawElementsInstanced`. GLES 3.0 supports both.
- Upload once per frame, not per eye — `Physics_BuildRenderItems` is already
  called once per frame (`main.cpp:692-693`), so keep the buffer and draw it
  twice.
- Have `Physics_BuildRenderItems` write straight into the mapped instance buffer
  to avoid the extra copy. `RenderItem` in `physics.h` becomes the instance
  layout; keep the padding explicit.
- Raise `kMaxBodies` (`physics.cpp:35`) and `kMaxRenderItems` (`main.cpp:54`)
  together — they should be one shared constant, not two that can drift.

**Done when:** the scene renders in a fixed small number of draw calls
regardless of body count, and the HUD confirms it.

## Step 5 — Enable SIMD

Flip `BOX3D_DISABLE_SIMD` to `OFF`. Box3D uses Neon on arm64, and the comment in
`CMakeLists.txt` already anticipates this flip once the app is known to run.

Keep it as its own commit — if Neon turns out to have build or precision trouble
on the NDK toolchain, this needs to be revertible without taking Steps 1–4 with it.

**Done when:** physics step time drops measurably on the HUD with identical
simulation behaviour.

## Step 6 — Multithreading

The API is confirmed (see Step 1): `b3WorldDef.workerCount`, plus optional
`enqueueTask` / `finishTask` / `userTaskContext` callbacks.

**Try the one-line version first.** With the callbacks left null and
`workerCount > 1`, Box3D spawns its own internal threads. That may be the whole
step. Only build a thread pool and wire the callbacks if you need to share
workers with other subsystems or control thread priority and affinity — which on
Quest is a real possibility eventually, but not a Phase 0 problem.

- Quest 3's XR2 Gen 2 has 8 cores but the app doesn't get all of them. Start at
  3–4 workers, leaving headroom for the render thread and the OpenXR runtime,
  then tune. More workers is not automatically faster.
- Watch for the wrong result: if step time doesn't improve, the scene may be too
  small for threading to pay for its synchronisation. Re-test at high body counts
  before concluding anything.
- The Step 2c benchmark sweeps worker counts directly, so the shape of this
  curve can be known before it ever reaches the device.

The web build stays single-threaded and scalar. That divergence is expected and
worth stating in `wasm/build.md` so the two builds' body budgets aren't confused
for each other later.

**Done when:** step time at high body counts improves and the HUD reports the
active worker count.

## Step 7 — Small wins

- Enable backface culling (`main.cpp:621` currently disables it, noting winding
  isn't relied upon). Verify the cube mesh winding first, then turn it on — it's
  free fill-rate.
- Confirm the render target resolution and MSAA settings against the frame
  budget now that there's a HUD to see the cost.

## Step 8 — Measure

Run the Step 2b benchmark on-device and capture the log. It produces:

- max dynamic bodies at a stable frame rate, settled
- max dynamic bodies at a stable frame rate, **in motion and colliding** — the
  number that actually matters, and much lower than the settled one
- physics step time and render time at the knee, so you know which one you hit
- the shape of the curve either side of it

Put those numbers in `ROADMAP.md`. Phase 0.5 immediately stress-tests them with
a realistic destruction load — a heavyweight, per the roadmap.

### Who does what

The split matters, because two parts of this cannot be done from a development
machine at all:

| Task | Who |
|---|---|
| All eight steps, plus both benchmark harnesses | Claude |
| Verifying it compiles | CI — `.github/workflows/build.yml` builds the APK on every push |
| Relative physics scaling, SIMD and threading gains, regressions | Claude, via the Step 2c headless benchmark |
| Installing the APK and launching the benchmark | You — `adb install` the CI artifact |
| Absolute frame rate and render cost on real hardware | Only the headset can answer this |
| Reading the result | `adb logcat` during the run, then hand the log back |

Device involvement is one install, one launch, and roughly two minutes of
waiting — not a person throwing cubes until it feels bad.

---

## Risks

**API drift at Step 1 is the main unknown.** A pre-release-to-release jump can
rename or restructure anything. If the drift turns out to be large, do Step 1
alone and get it green before touching anything else — and remember that
`wasm/bridge.c` needs the same treatment.

**Threading may not pay at small scale.** Step 6's benefit only shows up at body
counts that the scene may not reach until Phase 2. Don't tune it against the
12-cube tower and conclude it doesn't work.

**The exit number may be disappointing.** That is a *successful* outcome of
Phase 0, not a failure — it's much cheaper to learn it here than after building
Phases 2–4 on an assumption. If the number is low, the response is smaller
voxels counts and more aggressive chunk merging in Phase 2, decided with data.
