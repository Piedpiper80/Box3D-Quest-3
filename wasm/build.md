# Building `box3d.wasm`

The browser build compiles the real [Box3D](https://github.com/erincatto/box3d)
engine (pinned to commit `c52908c9a907714e4d3a8a30be5272a1761158e1`, the same
one the native app uses) plus `bridge.c` to a single WebAssembly module.

## Toolchain

Any machine with Python can build it — the Zig toolchain ships as a pip wheel
and bundles a complete C compiler + wasm libc:

```bash
pip install ziglang
```

## Build

From a directory containing the Box3D source tree (`box3d-src/`) and this
folder's `bridge.c`:

```bash
python3 -m ziglang cc -target wasm32-wasi -Oz -mexec-model=reactor \
  -DBOX3D_DISABLE_SIMD -DNDEBUG \
  -Ibox3d-src/include -Ibox3d-src/src \
  box3d-src/src/*.c bridge.c \
  -Wl,--strip-all -o box3d.wasm
```

Notes:
- `BOX3D_DISABLE_SIMD` selects Box3D's scalar path (`B3_SIMD_NONE`) — wasm has
  neither SSE2 nor NEON.
- `-mexec-model=reactor` builds a library-style module: the host calls
  `_initialize()` once, then the exported API.
- Single-threaded: the world is created with the default (null) task callbacks,
  so Box3D runs its solver inline.

## Exports

Grouped by what they are for. The page uses all of them; `wasm/test.js`
exercises them directly.

| Group | Exports |
|---|---|
| World | `w_init`, `w_step(dt)`, `w_reset(sleep, groundY)`, `w_fill(n)`, `w_spawn(...)`, `w_count`, `w_capacity`, `w_state` |
| Your fists | `w_hand_create(i,x,y,z,half,density,hertz,zeta,maxForce)`, `w_hand_target(i,x,y,z,qx,qy,qz,qw)`, `w_hand_limits`, `w_hand_reach_mass(i,kg)`, `w_hand_apply`, `w_hand_state`, `w_hand_mass` |
| Destructible matter | `w_vox_create`, `w_vox_blast`, `w_vox_post`, `w_vox_run_count`, `w_vox_runs`, `w_vox_hit_count`, `w_vox_hit_events`, `w_vox_stats`, `w_vox_grid_stats`, `w_vox_grid_pose`, `w_vox_chunk_box_count`, `w_vox_chunk_boxes`, `w_vox_hide`, `w_vox_heal`, `w_vox_scale_hp` |
| The figure | `w_fig_create(x,z,stature,material)`, `w_fig_destroy`, `w_fig_update(px,py,pz,dt)`, `w_fig_apply`, `w_fig_post`, `w_fig_state`, `w_fig_bones`, `w_fig_pose`, `w_fig_joints`, `w_fig_rig`, `w_fig_bone_count`, `w_fig_bone_grid`, `w_fig_hold`, `w_fig_tempo` |

Call order every frame is load-bearing:

```
w_fig_update(player, dt)   its decisions
w_hand_apply()             your fists' joint targets
w_fig_apply()              standing, balance, heading
w_step(dt)                 the solver
w_vox_post()               route impacts into cells, shed debris
w_fig_post()               sever anything beaten past its break point
```

The only WASI imports are `fd_write`, `fd_close`, `fd_fdstat_get`, `fd_seek`
(libc plumbing for error printing); the short shim in `docs/arena.html` covers
them.

## Test

```bash
cp ../docs/box3d.wasm box3d.wasm    # test.js loads it from its own directory
node test.js                        # 46 engine checks
node page-test.js arena.html        # 15 page checks
```

Both run in CI on every push. See `docs/design/ROADMAP.md` for what they cover
and what they deliberately do not.
