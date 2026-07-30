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

## TODO

- Implement joint-only stance and contact sensing.
- Implement contact-driven stepping, articulated attacks, falls, and recovery.
- Integrate red deformable rendering and deterministic page hooks.
