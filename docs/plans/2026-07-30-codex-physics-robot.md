# Codex Physics Robot Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a red, independently controlled, physics-first robot beside the unchanged grey figure, with shared-world collisions, deformable damage, genuine falling, and physical get-up attempts.

**Architecture:** Keep every existing `w_fig_*` entry point and control path intact. Add a new `wasm/codex_figure.inc` translation-unit include with `w_codex_*` exports so it can share the existing Box3D world and voxel primitives without copying or routing through the grey controller. Generalize only neutral page mesh plumbing and contact ownership, then drive both robots from the existing fixed 72 Hz loop.

**Tech Stack:** C11 compiled to WASM with Zig, pinned Box3D, WebXR/WebGL2, plain JavaScript, Node 22 test harness, Playwright web-game validation.

---

### Task 1: Establish the red-figure contract without changing grey behaviour

**Files:**
- Create: `progress.md`
- Create: `wasm/codex_figure.inc`
- Modify: `wasm/bridge.c:451-474`
- Modify: `wasm/bridge.c:3620-end`
- Modify: `wasm/test.js:10-30`
- Modify: `wasm/test.js:after the existing grey-figure helpers`

**Step 1: Record the prompt and invariants**

Create `progress.md` beginning with the original user request and these fixed invariants: grey `w_fig_*` behaviour is preserved; Codex is red; both share one world and collide; Codex uses joint actuation only; an alive Codex robot keeps attempting recovery.

**Step 2: Write the failing export and coexistence tests**

Add a `CBONE` list with the existing 17 names plus `L_TOE` and `R_TOE`, then add checks equivalent to:

```js
check("the Codex controller has its own API",
  ["w_codex_create", "w_codex_update", "w_codex_post", "w_codex_state",
   "w_codex_pose", "w_codex_bones", "w_codex_bone_count", "w_codex_bone_grid"]
    .every((name) => typeof E[name] === "function"));

E.w_reset(0, 0);
E.w_fig_create(-0.45, 0, 1.75, 2);
E.w_codex_create(0.45, 0, 1.75, 2);
check("grey and red coexist", E.w_fig_state() && E.w_codex_state());
check("Codex has articulated toes", E.w_codex_bone_count() === 19);
```

Run `node wasm/test.js`. Expected: FAIL because the `w_codex_*` exports do not exist.

**Step 3: Add capacity and the include seam**

Raise `VOX_GRIDS` from 20 to 48 and update its comment. Add the following after the existing grey exports, without editing their bodies:

```c
#include "codex_figure.inc"
```

Start `codex_figure.inc` with `CODEX_CATEGORY 0x8ULL`, a 19-entry Codex bone enum/definition table, independent body/joint/grid arrays, reset/destroy/create functions, and read-only state/pose/grid exports. The two toe definitions are children of their respective feet and use bounded revolute joints.

**Step 4: Rebuild and verify**

Run the Zig command from `wasm/build.md`, copy `docs/box3d.wasm` to `wasm/box3d.wasm`, then run:

```powershell
node wasm/test.js
node wasm/page-test.js arena.html
```

Expected: all pre-existing 62 engine checks and 15 page checks pass; the new API/coexistence/toe checks pass.

**Step 5: Commit**

```powershell
git add progress.md wasm/bridge.c wasm/codex_figure.inc wasm/test.js docs/box3d.wasm
git commit -m "feat: add independent Codex figure skeleton"
```

### Task 2: Prove the Codex robot has only internal actuation

**Files:**
- Modify: `wasm/codex_figure.inc`
- Modify: `wasm/test.js`

**Step 1: Write failing physics-integrity tests**

Add exports `w_codex_actuation` and `w_codex_test_drop`. Test these invariants:

```js
check("Codex weight comes through its soles",
  Math.abs(codexGround().nTotal - codexRig().mass * 9.81) < codexRig().mass * 1.0);
check("Codex has no root actuator", codexActuation().rootForce === 0 && codexActuation().rootTorque === 0);
check("airborne Codex is ballistic", Math.abs(measuredAy + 9.81) < 0.35);
check("airborne Codex does not right itself", Math.abs(afterAngularMomentum - beforeAngularMomentum) < tolerance);
```

Run `node wasm/test.js`. Expected: FAIL because sensing/actuation diagnostics and standing control do not exist.

**Step 2: Implement bounded muscle helpers**

Implement helpers that set spherical target rotations or revolute target angles and scale spring hertz/maximum motor torque from segment inertia, leverage, and surviving grid material. All actuation must call Box3D joint APIs; do not call `b3Body_ApplyForce*`, `b3Body_ApplyTorque`, or `b3Body_SetTransform` from the Codex controller.

Expose diagnostics in a fixed float layout:

```c
// [rootForce, rootTorque, motorWork, nLeft, nRight, comX, comY, comZ,
//  velX, velY, velZ, supportMargin, airborne]
WASM_EXPORT("w_codex_actuation") float* w_codex_actuation(void);
```

**Step 3: Implement contact sensing and quiet stance**

Measure sole/toe contact impulses from Box3D contact data, calculate carried centre of mass, velocity, and the support hull, and drive only ankle/toe/hip/spine joint targets to keep the capture point recoverable. Torque-limit every joint and remove the command immediately when the relevant chain is detached.

**Step 4: Rebuild and run focused and regression tests**

Expected: the red robot stands for ten simulated seconds, its weight is floor-supported, the airborne tests pass, and all grey checks remain numerically within their previous tolerances.

**Step 5: Commit**

```powershell
git add wasm/codex_figure.inc wasm/test.js docs/box3d.wasm progress.md
git commit -m "feat: balance Codex robot through joint actuation"
```

### Task 3: Add contact-driven stepping

**Files:**
- Modify: `wasm/codex_figure.inc`
- Modify: `wasm/test.js`

**Step 1: Write failing gait tests**

Test that the controller closes toward the player on a normal floor, alternates loaded feet, bends knees and toes, never commands a swing before unloading that foot, and cannot translate under zero friction. Add a test-only floor-friction export instead of weakening production constants:

```js
check("Codex closes on the player", endDistance < startDistance - 0.35);
check("Codex gait transfers support", sawLeftStance && sawRightStance);
check("Codex cannot walk without floor friction", zeroMuTravel < 0.04);
check("a Codex stride is not counted as a fall", !sawFallingDuringHealthyStep);
```

Run the suite. Expected: FAIL because Codex can only stand.

**Step 2: Implement the capture-step state machine**

Add `CODEX_STAND`, `CODEX_UNLOAD`, `CODEX_SWING`, `CODEX_TOUCHDOWN`, and `CODEX_TRANSFER`. Choose a reachable landing point from capture point and desired velocity toward the player. Advance phases from measured load, joint configuration, and contact—not elapsed animation time alone. Timeouts may abort a failed step but may not fabricate contact.

**Step 3: Implement swing and stance muscle targets**

Use hip/knee/ankle/toe targets to unload and place the foot. Clamp reach to leg geometry. The stance chain and floor friction must supply the reaction that moves the body; do not add a pelvis translation controller.

**Step 4: Rebuild and run all tests**

Expected: gait tests pass, ballistic/internal-actuation tests still pass, grey tests stay green, and frame time remains under the existing budget with a second figure.

**Step 5: Commit**

```powershell
git add wasm/codex_figure.inc wasm/test.js docs/box3d.wasm progress.md
git commit -m "feat: walk Codex robot through foot placement"
```

### Task 4: Add physically generated punches

**Files:**
- Modify: `wasm/codex_figure.inc`
- Modify: `wasm/test.js`

**Step 1: Write failing attack tests**

Measure the hand trajectory and joint work during an attack. Assert that a punch includes stance load shift, trunk rotation, shoulder drive, and elbow extension; reaches the player only when geometry permits; produces no hand-to-world motor; and recoils through the articulated body.

```js
check("a Codex punch is joint generated", codexActuation().rootForce === 0);
check("the punch shifts floor load", peakLoadDifference > 0.12 * weight);
check("the striking hand gains real momentum", peakHandMomentum > 2.0);
check("Codex keeps targeting the player", codexState().target === CODEX_TARGET_PLAYER);
```

**Step 2: Implement attack phases**

Add guard, wind-up, plant, strike, and recover targets. Transitions require support and achieved joint pose. Strike power comes from torque caps and available stance, not a direct hand target or injected velocity. Abort to brace or fall when support is lost.

**Step 3: Rebuild and run tests**

Expected: the hand gains momentum through the chain, the root-integrity checks remain green, and the robot remains fallible under obstruction.

**Step 4: Commit**

```powershell
git add wasm/codex_figure.inc wasm/test.js docs/box3d.wasm progress.md
git commit -m "feat: drive Codex punches through the body"
```

### Task 5: Route shared-world collision and independent damage

**Files:**
- Modify: `wasm/bridge.c:476-500`
- Modify: `wasm/bridge.c:1010-1100`
- Modify: `wasm/codex_figure.inc`
- Modify: `wasm/test.js`

**Step 1: Write failing collision/ownership tests**

Spawn the robots close enough for a controlled red shoulder impact into grey. Verify separation impulse, no target change, no self-collision, and independent grid damage. Then intercept a Codex punch with a grey limb and verify that a high-energy contact can dent grey while slow resting contact cannot.

**Step 2: Add neutral grid ownership and strike qualification**

Extend `VoxGrid` with owner/team metadata and replace the `armOnly` boolean with an explicit damage-source policy. Preserve the old grey rule for player fists exactly. For Codex grids, accept player fists and sufficiently concentrated robot-body impacts using solver approach speed and effective mass. Route slackening to `figSlacken` or `codexSlacken` by owner.

**Step 3: Recompute damaged-part authority**

After voxel remeshing, read remaining/total material for Codex parts and scale their body mass/inertia and downstream motor torque within safe lower bounds. A detached chain receives zero commands. Do not change the corresponding grey calculation.

**Step 4: Rebuild and run all tests**

Expected: grey and red collide, both still target the player, damage remains attributed to the contacted grid, slow contact is harmless, and all old fist-damage checks pass.

**Step 5: Commit**

```powershell
git add wasm/bridge.c wasm/codex_figure.inc wasm/test.js docs/box3d.wasm progress.md
git commit -m "feat: make robot collisions physical and damage-aware"
```

### Task 6: Add falling, bracing, and repeated physical get-up attempts

**Files:**
- Modify: `wasm/codex_figure.inc`
- Modify: `wasm/test.js`

**Step 1: Write failing recovery tests**

Create deterministic supine, prone, and side drops. Assert that no body transform is assigned after creation, contacts precede phase changes, the pelvis height rises through joint work, and standing is regained on an unobstructed floor. Add damaged cases proving that missing arms/legs remove strategies and a severed central spine disables control.

```js
check("supine Codex attempts a physical get-up", sawBrace && sawKneel && stood);
check("prone Codex attempts a physical get-up", sawBrace && sawFootPlant && stood);
check("side Codex rolls into a viable recovery", sawRoll && stood);
check("a blocked recovery retries without snapping", retries > 0 && maxFrameJump < 0.03);
check("a broken central spine is a true ragdoll", !codexState().alive && motorWork === 0);
```

**Step 2: Implement fall classification and bracing**

Classify upright, prone, supine, left side, and right side from chest orientation and contact geometry. When momentum is unrecoverable, switch from gait/attack to bounded bracing targets; never apply airborne righting.

**Step 3: Implement contact-gated recoveries**

Implement roll, forearm/hand brace, knee draw, kneel, foot plant, and rise phases. Enter a phase only when its required limb chain exists. Advance only when the expected contact and pose are measured. On timeout, choose another valid route or retry from classification.

**Step 4: Implement structural life rules**

Keep attempting while the pelvis-spine-chest control chain is connected. Limb loss removes related actions. A broken central chain sets `alive=0`, clears all motor authority, and leaves Box3D alone to simulate the ragdoll.

**Step 5: Rebuild, run, and commit**

Expected: all orientation and damage cases pass without root actuation or transform discontinuity.

```powershell
git add wasm/codex_figure.inc wasm/test.js docs/box3d.wasm progress.md
git commit -m "feat: make Codex robot fall and physically recover"
```

### Task 7: Render and drive both robots in the WebXR page

**Files:**
- Modify: `docs/arena.html:35-75`
- Modify: `docs/arena.html:289-390`
- Modify: `docs/arena.html:560-820`
- Modify: `wasm/page-test.js`

**Step 1: Write failing page tests**

Extend the draw-log checks to require grey deformable draws near `[0.56, 0.60, 0.67]` and red deformable draws near `[0.82, 0.12, 0.10]`. Require both figures after the gesture, independent dent counts, a shared-world collision response, no target change, and no reported fault.

**Step 2: Generalize deformable mesh ownership**

Replace the singleton `boneMeshes` plumbing with two descriptors:

```js
var figures = {
  grey: { meshes: [], color: [0.56, 0.60, 0.67], api: "w_fig" },
  codex: { meshes: [], color: [0.82, 0.12, 0.10], api: "w_codex" }
};
```

Make build, update, release, dent routing, and draw helpers accept a descriptor. Preserve the existing deformation algorithm unchanged.

**Step 3: Spawn and simulate both**

Spawn grey and red at symmetric offsets with enough initial clearance. Keep the grey update/apply/post call sequence unchanged. Call Codex update/apply/post independently in the same fixed step and perform only one shared `w_step`/`w_vox_post`.

**Step 4: Update copy and report**

Describe the two simultaneous opponents, identify the red one as Codex, and report each robot's physical state separately. A Codex fault should name the red path and leave the grey report usable.

**Step 5: Run page and engine regressions**

```powershell
node wasm/test.js
node wasm/page-test.js arena.html
```

Expected: all checks pass and both mesh colours are present in the draw log.

**Step 6: Commit**

```powershell
git add docs/arena.html wasm/page-test.js progress.md
git commit -m "feat: add red Codex robot to the first fight"
```

### Task 8: Add deterministic browser state and exercise the full fight

**Files:**
- Modify: `docs/arena.html`
- Modify: `wasm/page-test.js`
- Modify: `docs/design/ROADMAP.md`
- Modify: `README.md`
- Modify: `progress.md`

**Step 1: Write failing deterministic-hook tests**

Assert that flat mode exposes `window.render_game_to_text` and `window.advanceTime`, that advancing 1000 ms produces exactly 72 fixed updates, and that parsed state contains the coordinate-system note, player fists, grey state, Codex position/velocity/contact/action/recovery/damage, and active debris.

**Step 2: Implement the hooks**

Return concise current state only. Make `advanceTime(ms)` run fixed steps through the same simulation function used by XR without scheduling duplicate animation frames.

**Step 3: Extend the flat pilot into observable windows**

Stage brief periods for: both standing; player movement causing the figures to obstruct one another; a controlled red fall; a red get-up attempt; attacks and independent dents. Do not teleport either robot during the scenario.

**Step 4: Run the mandated web-game loop**

Use `@develop-web-game`. Run the bundled Playwright client against a local static server with short action bursts and pauses. Capture state and screenshots for standing, collision, falling, and recovery. Open every relevant screenshot and inspect robot colour, floor contact, body pose, mesh deformation, and visibility. Fix the first console error before continuing.

**Step 5: Run complete verification**

Run:

```powershell
node wasm/test.js
node wasm/page-test.js arena.html
git diff --check
git status --short
```

Expected: zero failed checks, only deliberately recorded `GAP` measurements if any, no new console errors, and no unexpected files.

**Step 6: Update the record**

Document what is mechanically proved, measured performance, known gaps, and the one remaining headset question. Do not claim the physical feel is verified before a Quest session.

**Step 7: Commit**

```powershell
git add docs/arena.html wasm/page-test.js docs/design/ROADMAP.md README.md progress.md
git commit -m "test: verify the two-robot physics fight"
```

### Task 9: Review, integrate, deploy, and verify the live build

**Files:**
- Modify only if review finds a concrete defect.

**Step 1: Use `@superpowers:requesting-code-review`**

Review the implementation against the approved design, with special attention to forbidden root forces, grey-controller regressions, collision ownership, damaged-limb authority, and deterministic get-up evidence.

**Step 2: Fix review findings test-first**

For every accepted finding, add or tighten the failing test before changing implementation. Rebuild WASM and rerun both suites after each fix.

**Step 3: Use `@superpowers:verification-before-completion`**

Re-run the complete engine/page/browser verification from a clean working tree and record exact pass/fail/gap counts in `progress.md`.

**Step 4: Use `@superpowers:finishing-a-development-branch`**

Merge the feature branch into `main` with merge history preserved, wait for CI to pass, deploy the matching `docs` files and rebuilt WASM to `gh-pages`, and verify the live page reports the new build and both robots. Follow `CLAUDE.md`: do not strand green work on an unmerged branch.

**Step 5: Headset handoff**

Provide the live URL and ask only: “Does the red robot feel like its weight is passing through its feet, or does anything still feel as though it is being held up?”
