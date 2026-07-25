# Phase 0 — Foundations

**Goal:** raise the engine ceiling far enough that the rest of the roadmap is
possible, and produce the one number every later phase depends on.

**Exit criterion:** the maximum dynamic body count that holds a stable 72 Hz
in-headset, measured on-device with the perf HUD. Write the number into
[`ROADMAP.md`](ROADMAP.md) when you have it. Stretch target: ≥1000 dynamic
boxes, which is roughly what one destructible mech needs.

Nothing here is a game feature. The scene at the end of Phase 0 looks almost
identical to the scene at the start — it just runs with an order of magnitude
more in it.

---

## Where things stand

| Constraint | Current | Location |
|---|---|---|
| Box3D revision | pinned to pre-release `c52908c9` | `app/src/main/cpp/CMakeLists.txt` |
| SIMD | disabled | `CMakeLists.txt` — `BOX3D_DISABLE_SIMD ON` |
| Threading | none — single-threaded by design | `main.cpp` header comment |
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

## Step 1 — Bump Box3D to v0.1.0

Box3D reached its official v0.1.0 release after the currently pinned commit was
taken. The release is required, not optional: **joints** and **multiple shapes
per body** are what Phases 3 and 4 are built from, and neither is usable at the
current pin.

- Update `GIT_TAG` in `app/src/main/cpp/CMakeLists.txt` to the v0.1.0 release tag.
- Expect API drift. `physics.cpp` currently calls `b3DefaultWorldDef`,
  `b3CreateBody`, `b3MakeBoxHull`, `b3CreateHullShape`, `b3Body_GetPosition`,
  `b3Body_GetRotation`, `b3MakeMatrixFromQuat`. Some of these may have been
  renamed or restructured between a pre-release commit and the release.
- **`wasm/bridge.c` uses the same API and must be bumped in lockstep**, or the
  browser build silently diverges from the native one. Re-run the harness in
  `wasm/test.js` afterwards.
- Keep `BOX3D_DISABLE_SIMD ON` for this step. One variable at a time.

**Done when:** the existing cube tower topples exactly as before, on both the
headset and the web build.

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

Box3D ships a task-callback interface in its world definition (worker count plus
enqueue/finish callbacks), mirroring Box2D v3's design — **verify the exact names
against the v0.1.0 headers** rather than assuming them.

- Wire a small fixed-size thread pool to those callbacks.
- Quest 3's XR2 Gen 2 has 8 cores but the app doesn't get all of them. Start at
  3–4 workers, leaving headroom for the render thread and the OpenXR runtime,
  then tune against the HUD. More workers is not automatically faster.
- Watch for the wrong result: if step time doesn't improve, the scene may be too
  small for threading to pay for its synchronisation. Re-test at high body counts
  before concluding anything.

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

Spawn dynamic boxes until 72 Hz breaks. Record:

- max dynamic bodies at stable 72 Hz, settled
- max dynamic bodies at stable 72 Hz, **in motion and colliding** — this is the
  number that actually matters, and it will be much lower
- physics step time and render time at that point, so you know which one you hit

Put those numbers in `ROADMAP.md`. Phase 0.5 immediately stress-tests them with
a realistic destruction load.

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
