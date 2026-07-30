Original prompt: Add a second, visibly red Codex robot to the first VR fight. Reuse Claude's physical model, deformable meshes, and damage system, but keep Claude's controller unchanged. The Codex robot must move, hit, fall, and keep trying to get up through genuine physics; both robots target the player and physically collide with each other.

## Invariants

- Existing `w_fig_*` behaviour stays intact.
- Grey and red share one Box3D world and can collide, but neither targets the other.
- Codex actuation is internal, torque-limited joint work; no root lift, root drive, gravity cancellation, teleporting, or airborne righting.
- Codex is allowed to stumble, miss, fail a recovery, or be physically unable to rise.
- An alive Codex robot keeps attempting a physically valid recovery.

## Progress

- 2026-07-30: Approved design and implementation plan committed.
- 2026-07-30: Clean baseline: 62 engine checks passed, 2 known gaps; 15 page checks passed.
- 2026-07-30: Local pinned Box3D source and Zig toolchain prepared for WASM rebuilds.
- 2026-07-30: Added the independent 19-body Codex skeleton/API in the shared world. Engine: 65 passed, 0 failed, 2 known gaps. Page: 15 passed, 0 failed.
- 2026-07-30: Codex now stands for eight measured seconds using only contact-gated, equal-and-opposite ankle torque pairs. The toe sole geometry was corrected to the floor; before that, its initial penetration supplied a persistent overturning impulse. Engine: 68 passed, 0 failed, 2 known gaps. Page remains 15/15.
- 2026-07-30: Added contact-driven cautious stepping. Support points are latched, the hip shifts weight with an equal-and-opposite pelvis/thigh torque pair, swing and touchdown require measured foot loading, and an unsafe attempted step returns to double support instead of being forced through. Zero-friction testing confirms there is no hidden forward drive. Engine: 74 passed, 0 failed, 2 pre-existing grey-robot gaps. Page: 15 passed, 0 failed.
- 2026-07-30: Added joint-driven windup/strike/recovery attacks, measured fist momentum, deformable damage and joint breakage, physical fall detection, and repeated floor-contact get-up attempts while the torso remains alive. Recovery now gates its push phases on measured arm and foot reactions, and an intact body measurably lifts its torso on a clear floor without root actuation; failed rises retry instead of being forced upright. Both fighters must be beaten before the round ends. Engine: 84 passed, 0 failed, 2 pre-existing grey-robot gaps. Page: 16 passed, 0 failed.

## TODO

- 2026-07-30 user playtest regression: red can topple during a punch and then repeat a floor-thrashing recovery indefinitely. The existing tests missed this: the attack check samples only the five-second endpoint, while the recovery check accepts a 0.12 m hip bounce without requiring a return to an upright fighting state. A traced shove recovery reached phase 2 with both feet loaded but settled with hip/head at roughly 0.09/0.10 m, then repeated the same lateral hop. Root cause: the recovery has no body-orientation-dependent roll/kneel stage and phase 2 has no stalled-contact exit; it extends the legs while the torso remains horizontal.
- Replace the weak checks with full-sequence regressions, then implement and deploy the approved correction.
- 2026-07-30 red tests: the disturbed-windup scenario proves Build 7 still enters STRIKE from an unrecoverable base; the completed-recovery scenario runs 20 seconds and ends in GETTING UP phase 2 after 11 retries with 0/36 stable standing frames. Suite baseline is now 84 passed, 2 failed, 2 pre-existing grey gaps.
- 2026-07-30 punch correction: windup now reads current COM velocity, two-foot load, upright geometry, and capture-point error before committing. The disturbed-windup regression passes; the controller cancels to guard and may punch later after it truly settles. Suite is 85 passed, 1 failed (completed recovery), 2 pre-existing grey gaps.
- 2026-07-30 recovery correction: Codex now rolls toward prone through a loaded shoulder, plants bent arms, completes a measured elbow-extension push, brings both soles down, then uses equal-and-opposite leg/sole force and pelvis/sole torque pairs to rise and settle. The leg actuators have zero net force and torque over the robot and cannot create airborne root authority. Recovery holds a quiet, two-foot fighting stance for half a second before handing control back, and the same physical postural muscles remain available after that handoff instead of disappearing in one frame. Engine: 86 passed, 0 failed, 2 pre-existing grey gaps.
