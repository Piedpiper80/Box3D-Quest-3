# Codex Stable Punch and Physical Recovery Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Prevent the red robot from voluntarily punching from an unstable base and replace its endless floor-thrashing loop with a contact-driven recovery that returns an intact robot to a stable fighting stance.

**Architecture:** Keep the grey `w_fig_*` controller untouched. Extend only the independent `w_codex_*` controller with strike-entry guards and an orientation-aware roll/plant/kneel/crouch/stand recovery whose transitions require real contacts and measured pose progress. Preserve zero root force/torque and let damage or obstruction cause honest failed attempts.

**Tech Stack:** C11 compiled to WASM with Zig, pinned Box3D, Node 22 simulation tests, WebXR/WebGL2 page, Playwright flat-mode validation.

---

### Task 1: Replace endpoint checks with failing full-sequence regressions

**Files:**
- Modify: `wasm/test.js:842-910`

**Step 1: Write the unstable-punch regression**

Record every combat transition for at least 20 simulated seconds. Assert that entry into `CODEX_STRIKE` occurs only while the robot is upright, both feet have meaningful load, and the capture-point error is within the accepted support margin. Add a deterministic lateral chest disturbance during windup and assert that the strike is cancelled when the stance becomes unrecoverable.

**Step 2: Write the completed-recovery regression**

Replace the 12 cm hip-bounce assertion with a complete outcome assertion. Knock an intact robot down from the front and side, then simulate for a bounded interval. Require it to reach ordinary state `0`, hold upright pelvis/head geometry with both feet loaded for at least 0.5 seconds, and retain all 19 bones.

**Step 3: Add the anti-worm watchdog**

During `CODEX_GETUP`, track phase, torso-up alignment, pelvis/head height, and support loads. Fail if one recovery phase persists beyond its timeout without a minimum improvement in orientation, height, or support transfer.

**Step 4: Run the tests and verify RED**

Run:

```powershell
node wasm/test.js
```

Expected: the recovery completion and stalled-phase checks fail on Build 7; the failure reports the robot settled horizontal in phase 2. The existing 84 checks remain otherwise unchanged.

**Step 5: Commit the red tests**

```powershell
git add wasm/test.js progress.md
git commit -m "test: expose stalled Codex recovery"
```

### Task 2: Gate punches on a recoverable stance

**Files:**
- Modify: `wasm/codex_figure.inc:307-364`
- Test: `wasm/test.js:842-870`

**Step 1: Add a support predicate**

Create one internal predicate that requires `codexIsUpright()`, minimum load on each foot, and bounded capture-point error from `s_codexAct[11]`. It reads existing sensors only and applies no actuation.

**Step 2: Gate windup and cancel before strike**

Allow idle combat to enter windup only when the support predicate is true. If it becomes false during windup, return to guard without entering strike. Do not cancel a strike already physically in flight.

**Step 3: Add a modest counter-pose**

During windup and strike, aim the non-punching arm and abdomen/chest in the opposite rotational direction so joint reactions travel through the braced body. Keep existing hand momentum measurement and all root diagnostics unchanged.

**Step 4: Rebuild and verify GREEN for punch tests**

From the repository root, run the Zig build described in `wasm/build.md`, producing `wasm/box3d.wasm`, then:

```powershell
node wasm/test.js
```

Expected: unstable punch is withheld/cancelled, repeated supported punches still reach the player, and root force/torque remain zero.

**Step 5: Commit**

```powershell
git add wasm/codex_figure.inc wasm/test.js wasm/box3d.wasm progress.md
git commit -m "fix: brace Codex punches through stable footing"
```

### Task 3: Implement orientation-aware contact recovery

**Files:**
- Modify: `wasm/codex_figure.inc:22-47`
- Modify: `wasm/codex_figure.inc:366-558`
- Test: `wasm/test.js:870-930`

**Step 1: Add torso orientation sensing**

Read chest world axes from its quaternion and classify prone, supine, left-side, right-side, kneeling, crouched, or upright. Store per-attempt progress baselines and a chosen roll direction; expose only diagnostics already useful to the page/test state.

**Step 2: Replace the old three-phase loop**

Implement these contact-driven phases:

- settle/classify;
- asymmetric roll toward prone;
- plant hands/forearms and tuck knees;
- raise chest and move pelvis into the hand/knee support polygon;
- transfer support to both feet in a crouch;
- extend the legs and stabilize with ordinary ankle balance.

Use existing `codexPoint`, `codexPointWorld`, `codexBend`, spring hertz controls, and equal-and-opposite ankle/hip balance torques. Do not add any root body force, root torque, velocity assignment, transform assignment, gravity cancellation, or hidden fixture.

**Step 3: Add progress-based escape paths**

For every phase, record the expected measurable progress. If contact exists but progress stalls beyond the phase timeout, return to classification and alternate the roll strategy. If a required limb chain is broken, skip to another physically possible support strategy or keep retrying without claiming success.

**Step 4: Require a real completed stand**

Return to combat only after pelvis and head meet upright geometry, both feet carry load, capture-point error is bounded, speed is quiet, and the condition remains true for 0.5 seconds. Reset collapse/air timers only then.

**Step 5: Rebuild and verify GREEN**

Run:

```powershell
node wasm/test.js
```

Expected: front and side knockdowns complete an intact recovery within the bound, no phase stalls, the punch checks pass, zero-friction travel remains bounded, and root force/torque are exactly zero.

**Step 6: Commit**

```powershell
git add wasm/codex_figure.inc wasm/test.js wasm/box3d.wasm progress.md
git commit -m "fix: make Codex recover through real support"
```

### Task 4: Integrate the rebuilt engine into the page and visually validate

**Files:**
- Modify: `docs/box3d.wasm`
- Modify: `docs/arena.html`
- Modify: `wasm/page-test.js` only if the state contract needs the new build number
- Modify: `progress.md`

**Step 1: Copy the verified WASM and increment the build**

Copy `wasm/box3d.wasm` to `docs/box3d.wasm` and increment the `BUILD` constant in `docs/arena.html` so Quest browsers cannot reuse Build 7.

**Step 2: Run both project suites**

```powershell
node wasm/test.js
node wasm/page-test.js arena.html
```

Expected: zero failures; only the two pre-existing grey gait gaps may remain.

**Step 3: Run the mandated browser loop**

Use `@develop-web-game` with the bundled Playwright client against `docs/arena.html?flat=1`. Capture gameplay after the fighters engage and after a controlled red fall/recovery scenario. Inspect screenshots, `render_game_to_text`, and console errors. Verify visually that red is not horizontal cycling its limbs and remains beside the fight rather than launching across the arena.

**Step 4: Record results**

Append exact engine/page counts and visual findings to `progress.md`. Keep the remaining headset feel question explicit.

**Step 5: Commit**

```powershell
git add docs/arena.html docs/box3d.wasm wasm/page-test.js progress.md
git commit -m "build: ship corrected Codex recovery"
```

### Task 5: Verify, integrate, deploy, and check the public page

**Files:**
- Modify only if verification reveals a concrete defect.

**Step 1: Run completion verification**

Use `@superpowers:verification-before-completion` and run fresh:

```powershell
node wasm/test.js
node wasm/page-test.js arena.html
git diff --check
git status --short
```

Do not claim success unless the full outputs show the expected counts and no unexpected changes.

**Step 2: Integrate without stranding the fix**

Merge the feature history into `main` without squashing, push `main`, and ensure CI is green. Follow `CLAUDE.md`; do not leave the correction only on the feature branch.

**Step 3: Deploy matching files to `gh-pages`**

Update the gh-pages worktree with the exact verified `docs/arena.html` and `docs/box3d.wasm`, commit, and push. Verify the public page reports the new build and that the live WASM hash matches the verified artifact.

**Step 4: Headset handoff**

Provide only the live URL and ask whether the red robot now braces its punches and moves through a believable roll/kneel/stand recovery instead of worming.
