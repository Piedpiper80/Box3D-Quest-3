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

| Export | Purpose |
|---|---|
| `w_init()` | Create world, static ground (top at y=0), cube tower + scatter |
| `w_step(dt)` | Advance the simulation (4 solver sub-steps) |
| `w_spawn(px,py,pz,vx,vy,vz,half,colorIdx)` | Throw a cube; recycles the oldest thrown cube at the 96-body cap |
| `w_count()` | Number of dynamic cubes |
| `w_state()` | Pointer into wasm memory: 9 floats per cube — `x,y,z, qx,qy,qz,qw, halfExtent, colorIdx` |

The only WASI imports are `fd_write`, `fd_close`, `fd_fdstat_get`, `fd_seek`
(libc plumbing for error printing) — the ~15-line shim in `docs/index.html`
covers them.

## Test

```bash
node test.js   # 13 checks: falling, settling, no floor penetration, spawn,
               # flight, landing, recycling stability, unit quaternions,
               # NaN sweep, and a 72 Hz frame-budget perf gate
```
