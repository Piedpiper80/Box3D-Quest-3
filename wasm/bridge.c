// bridge.c — flat JS-facing API over the real Box3D engine (wasm build).
//
// Exports (called from the WebXR page):
//   w_init()                       create world, ground, cube tower + scatter
//   w_step(dt)                     advance the simulation (4 sub-steps)
//   w_spawn(px,py,pz,vx,vy,vz,h,c) throw a new cube; recycles oldest when full
//   w_count()                      number of dynamic cubes
//   w_reset(sleep)                 empty world + ground; sleep selects the regime
//   w_fill(n)                      drop n cubes into a pile (benchmark scenes)
//   w_state()                      pointer to packed floats, 9 per cube:
//                                  [x,y,z, qx,qy,qz,qw, halfExtent, colorIdx]
//
// Coordinates: right-handed, Y-up, meters — matches WebXR local-floor space.
#include <box3d/box3d.h>

#define WASM_EXPORT(name) __attribute__((export_name(name)))

#define MAX_CUBES 2048
#define TOWER_COUNT 12
#define SCATTER_COUNT 8

typedef struct
{
    b3BodyId id;
    float half;
    float color;
} Cube;

static b3WorldId s_world;
static int s_worldCreated = 0;
static Cube s_cubes[MAX_CUBES];
static int s_count = 0;
static int s_ambient = 0;      // initial cubes are never recycled
static int s_nextRecycle = 0;  // round-robin index among thrown cubes
static float s_state[MAX_CUBES * 9];

// tiny deterministic PRNG for spawn jitter (no libc rand needed)
static unsigned s_rng = 0x9e3779b9u;
static float frnd(float lo, float hi)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((float)(s_rng >> 8) / 16777216.0f);
}

static void addCube(float x, float y, float z, float half, float vx, float vy, float vz,
                    float color, float angVel)
{
    if (s_count >= MAX_CUBES)
    {
        return;
    }

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position.x = x;
    bd.position.y = y;
    bd.position.z = z;
    bd.linearVelocity.x = vx;
    bd.linearVelocity.y = vy;
    bd.linearVelocity.z = vz;
    bd.angularVelocity.x = angVel * frnd(-1.0f, 1.0f);
    bd.angularVelocity.y = angVel * frnd(-1.0f, 1.0f);
    bd.angularVelocity.z = angVel * frnd(-1.0f, 1.0f);

    b3BodyId body = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 1.0f;
    sd.baseMaterial.friction = 0.55f;
    sd.baseMaterial.restitution = 0.15f;

    b3BoxHull hull = b3MakeBoxHull(half, half, half);
    b3CreateHullShape(body, &sd, &hull.base);

    s_cubes[s_count].id = body;
    s_cubes[s_count].half = half;
    s_cubes[s_count].color = color;
    s_count++;
}

WASM_EXPORT("w_init")
void w_init(void)
{
    // Reachable a second time when the benchmark hands the page back to the
    // normal scene, so the previous world has to go or it leaks.
    if (s_worldCreated)
    {
        b3DestroyWorld(s_world);
    }
    s_worldCreated = 1;
    s_count = 0;
    s_rng = 0x9e3779b9u;

    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity.x = 0.0f;
    wd.gravity.y = -9.81f;
    wd.gravity.z = 0.0f;
    s_world = b3CreateWorld(&wd);

    // Static ground slab, top surface exactly at y = 0 (the player's floor).
    {
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_staticBody;
        bd.position.x = 0.0f;
        bd.position.y = -0.1f;
        bd.position.z = 0.0f;
        b3BodyId ground = b3CreateBody(s_world, &bd);

        b3ShapeDef sd = b3DefaultShapeDef();
        sd.baseMaterial.friction = 0.7f;
        b3BoxHull hull = b3MakeBoxHull(8.0f, 0.1f, 8.0f);
        b3CreateHullShape(ground, &sd, &hull.base);
    }

    // A tower ~1.2 m in front of the player that topples as it settles.
    const float half = 0.10f;
    for (int i = 0; i < TOWER_COUNT; i++)
    {
        float lean = 0.013f * (float)i;
        addCube(lean, half + (2.0f * half + 0.005f) * (float)i, -1.2f,
                half, 0.0f, 0.0f, 0.0f, (float)(i % 6), 0.0f);
    }

    // A loose scatter of smaller cubes dropping around the room.
    for (int i = 0; i < SCATTER_COUNT; i++)
    {
        addCube(frnd(-1.4f, 1.4f), frnd(0.8f, 2.6f), frnd(-1.4f, 1.4f),
                frnd(0.06f, 0.13f), frnd(-0.5f, 0.5f), 0.0f, frnd(-0.5f, 0.5f),
                (float)(i % 6), 2.0f);
    }

    s_ambient = s_count;
    s_nextRecycle = s_ambient;
}

WASM_EXPORT("w_step")
void w_step(float dt)
{
    b3World_Step(s_world, dt, 4);
}

// ---------------------------------------------------------------------------
// Benchmark support
//
// The browser build is the only way to measure Box3D on real Quest hardware
// without a PC: no sideloading, no adb, no cable — just a URL in the headset
// browser. These two exports let the page build controlled scenes that mirror
// bench/bench.c, so its numbers describe the same thing.
//
// Caveat worth remembering when reading the results: this build is scalar and
// single-threaded (wasm has neither SSE2 nor Neon, and SharedArrayBuffer is
// unavailable on plain static hosting), so it measures the engine's *slowest*
// configuration. The native app runs SIMD with four workers, which CI measured
// at roughly 3.6x faster. Web numbers are therefore a floor, not a ceiling.
// ---------------------------------------------------------------------------

// Tear the world down and rebuild it with only the ground.
//
// `enableSleep` selects the regime: with sleep on, settled bodies leave the
// solver and cost almost nothing — the flattering number. With it off, every
// body is solved every step whether or not it has come to rest, which is what a
// fight looks like and the number that matters.
WASM_EXPORT("w_reset")
void w_reset(int enableSleep)
{
    // Tracked with an explicit flag rather than by inspecting the id: the id is
    // zero-initialised at load, and a zeroed id is not guaranteed to compare
    // equal to the library's null id.
    if (s_worldCreated)
    {
        b3DestroyWorld(s_world);
    }
    s_worldCreated = 1;
    s_count = 0;
    s_ambient = 0;
    s_nextRecycle = 0;
    s_rng = 0x9e3779b9u; // identical scene every run, so runs are comparable

    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity.x = 0.0f;
    wd.gravity.y = -9.81f;
    wd.gravity.z = 0.0f;
    wd.enableSleep = (enableSleep != 0);
    s_world = b3CreateWorld(&wd);

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_staticBody;
    bd.position.x = 0.0f;
    bd.position.y = -0.1f;
    bd.position.z = 0.0f;
    b3BodyId ground = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.baseMaterial.friction = 0.7f;
    // Wide enough that the largest pile cannot roll off the edge and fall
    // forever, which would quietly flatter the measurement.
    b3BoxHull hull = b3MakeBoxHull(20.0f, 0.1f, 20.0f);
    b3CreateHullShape(ground, &sd, &hull.base);
}

// Drop `n` cubes in a loose jittered cuboid, high enough that they fall,
// collide and pile up. Mirrors Physics_SpawnPile and bench.c's addCubes.
WASM_EXPORT("w_fill")
void w_fill(int n)
{
    const float half = 0.10f;
    const float spacing = half * 2.6f;
    const int perRow = 12;

    for (int i = 0; i < n && s_count < MAX_CUBES; i++)
    {
        const int cx = i % perRow;
        const int cz = (i / perRow) % perRow;
        const int cy = i / (perRow * perRow);

        addCube(((float)cx - perRow * 0.5f) * spacing + frnd(-0.01f, 0.01f),
                0.5f + (float)cy * spacing,
                ((float)cz - perRow * 0.5f) * spacing + frnd(-0.01f, 0.01f) - 2.0f,
                half, 0.0f, 0.0f, 0.0f, (float)(i % 6), 0.0f);
    }
    s_ambient = s_count;
    s_nextRecycle = s_ambient;
}

WASM_EXPORT("w_spawn")
void w_spawn(float px, float py, float pz, float vx, float vy, float vz,
             float half, float color)
{
    if (s_count >= MAX_CUBES)
    {
        // Recycle the oldest thrown cube (never the ambient scene).
        if (s_nextRecycle >= s_count)
        {
            s_nextRecycle = s_ambient;
        }
        b3DestroyBody(s_cubes[s_nextRecycle].id);
        for (int i = s_nextRecycle; i < s_count - 1; i++)
        {
            s_cubes[i] = s_cubes[i + 1];
        }
        s_count--;
    }
    addCube(px, py, pz, half, vx, vy, vz, color, 4.0f);
}

WASM_EXPORT("w_count")
int w_count(void)
{
    return s_count;
}

// Maximum bodies this build can hold. Exported so callers and tests can derive
// the limit rather than hard-coding it — the previous hard-coded 96 in test.js
// silently went stale the moment the cap was raised.
WASM_EXPORT("w_capacity")
int w_capacity(void)
{
    return MAX_CUBES;
}

WASM_EXPORT("w_state")
float* w_state(void)
{
    for (int i = 0; i < s_count; i++)
    {
        b3Pos p = b3Body_GetPosition(s_cubes[i].id);
        b3Quat q = b3Body_GetRotation(s_cubes[i].id);
        float* o = &s_state[i * 9];
        o[0] = (float)p.x;
        o[1] = (float)p.y;
        o[2] = (float)p.z;
        o[3] = q.v.x;
        o[4] = q.v.y;
        o[5] = q.v.z;
        o[6] = q.s;
        o[7] = s_cubes[i].half;
        o[8] = s_cubes[i].color;
    }
    return s_state;
}
