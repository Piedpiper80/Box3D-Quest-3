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

## TODO

- Implement articulated attacks, falls, and recovery.
- Integrate red deformable rendering and deterministic page hooks.
