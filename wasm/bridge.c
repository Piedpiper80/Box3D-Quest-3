// bridge.c — flat JS-facing API over the real Box3D engine (wasm build).
//
// This is the whole engine behind one page: a room, your two fists, a
// destructible-matter system, and one opponent built on a human skeleton.
//
// Nothing here knows about chapters, campaigns, upgrades, weight classes or
// a core you break to win. There is no engine to explode. What there is:
// bone, and what happens to bone when you hit it.
//
//   world      w_init, w_step, w_reset, w_spawn, w_count, w_state
//   fists      w_hand_*      one body per controller, held at your grip
//   matter     w_vox_*       cells in a grid: dent, break, tear off, fall
//   opponent   w_fig_*       seventeen bones, sixteen joints, no core
//
// Coordinates: right-handed, Y-up, metres — matches WebXR local-floor space.
#include <box3d/box3d.h>

#define WASM_EXPORT(name) __attribute__((export_name(name)))

#define MAX_CUBES 2048

typedef struct
{
    b3BodyId id;
    float half;
    float color;
} Cube;

static b3WorldId s_world;
static int s_worldCreated = 0;
static b3BodyId s_groundBody;      // the fists' motor joints anchor to it
static float s_groundY = 0.0f;
static Cube s_cubes[MAX_CUBES];
static int s_count = 0;
static int s_ambient = 0;      // initial cubes are never recycled
static int s_nextRecycle = 0;  // round-robin index among thrown cubes
static float s_state[MAX_CUBES * 9];

// The world is being torn down; forget every voxel grid and bone in it.
static void vxWorldReset(void);
static void figWorldReset(void);
static void handWorldReset(void);

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
    if (s_count >= MAX_CUBES) return;

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

// ---------------------------------------------------------------------------
// The world
// ---------------------------------------------------------------------------

static float s_worldFloorY = 0.0f;   // where the ground plane actually is

// Tear the world down and rebuild it with only the ground.
//
// `groundY` is the top surface of the floor slab, and it is a parameter
// because the page cannot assume Y=0 is the floor. In passthrough the floor
// is wherever the player's real floor is, and they measure it themselves;
// a figure built against the wrong one stands buried to the shins.
WASM_EXPORT("w_reset")
void w_reset(int enableSleep, float groundY)
{
    s_worldFloorY = groundY;
    // Tracked with an explicit flag rather than by inspecting the id: the id is
    // zero-initialised at load, and a zeroed id is not guaranteed to compare
    // equal to the library's null id.
    if (s_worldCreated) b3DestroyWorld(s_world);
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
    bd.position.y = groundY - 0.1f;
    bd.position.z = 0.0f;
    b3BodyId ground = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.baseMaterial.friction = 0.7f;
    // Wide enough that a severed limb sliding away cannot reach the edge and
    // fall forever.
    b3BoxHull hull = b3MakeBoxHull(20.0f, 0.1f, 20.0f);
    b3CreateHullShape(ground, &sd, &hull.base);

    s_groundBody = ground;
    s_groundY = groundY;
    vxWorldReset();
    figWorldReset();
    // The fists live in the world too, and their body ids died with it. Left
    // set, the next w_hand_apply drives two handles into a destroyed world.
    handWorldReset();
}

WASM_EXPORT("w_init")
void w_init(void)
{
    w_reset(1, 0.0f);
}

WASM_EXPORT("w_step")
void w_step(float dt)
{
    if (!s_worldCreated) return;
    b3World_Step(s_world, dt, 4);
}

// Drop `n` cubes in a loose jittered cuboid. Kept because it is the cheapest
// possible proof that the solver in this build is alive and behaving.
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
                s_worldFloorY + 0.5f + (float)cy * spacing,
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
        if (s_nextRecycle >= s_count) s_nextRecycle = s_ambient;
        b3DestroyBody(s_cubes[s_nextRecycle].id);
        for (int i = s_nextRecycle; i < s_count - 1; i++) s_cubes[i] = s_cubes[i + 1];
        s_count--;
    }
    addCube(px, py, pz, half, vx, vy, vz, color, 4.0f);
}

WASM_EXPORT("w_count")
int w_count(void) { return s_count; }

// Maximum bodies this build can hold. Exported so callers and tests derive
// the limit rather than hard-coding it — a hard-coded copy in test.js went
// stale the moment the cap moved.
WASM_EXPORT("w_capacity")
int w_capacity(void) { return MAX_CUBES; }

// 9 floats per cube: [x,y,z, qx,qy,qz,qw, halfExtent, colorIdx]
WASM_EXPORT("w_state")
float* w_state(void)
{
    for (int i = 0; i < s_count; i++)
    {
        b3Pos p = b3Body_GetPosition(s_cubes[i].id);
        b3Quat q = b3Body_GetRotation(s_cubes[i].id);
        float* o = &s_state[i * 9];
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
        o[7] = s_cubes[i].half;
        o[8] = s_cubes[i].color;
    }
    return s_state;
}

// ---------------------------------------------------------------------------
// Your fists
//
// No mech, no jointed arm chain, no IK, no torque curve, no upgrade slider.
// Your hand is where the game says it is: one body per controller, held at
// your grip pose by a motor joint against the world.
//
// It is a JOINT and not a force, and that distinction is the whole section.
// The first cut applied an explicit spring — F = k*(target - p) - c*v — every
// step, which is how the rest of this project has always driven things. At
// 72 Hz that spring is only stable while c*dt/m stays under 2, and a fist
// crisp enough to feel like your own hand needs stiffness far past it: the
// numbers that felt right measured 2.5, and the fist crept 58 mm downward
// through a target it was supposed to be sitting on. Softening it until the
// arithmetic was safe capped the tracking at about 5 cm of lag per metre per
// second, which is a mushy fist, and mush is exactly the verdict this
// project already collected once.
//
// Box3D solves joint springs implicitly, so a motor joint has no such
// ceiling — it can be as stiff as you like and it cannot blow up. The force
// cap stays, and now it does something honest: press your fist into
// something immovable and it stops at the cap instead of driving through.
// ---------------------------------------------------------------------------

#define HAND_COUNT 2
static b3BodyId s_handBody[HAND_COUNT];
static b3JointId s_handJoint[HAND_COUNT];
static float s_handHalf[HAND_COUNT];
static int s_handExists[HAND_COUNT] = {0, 0};
static float s_handReachMass[HAND_COUNT] = {0.0f, 0.0f};
static b3Transform s_handTarget[HAND_COUNT];

static void handWorldReset(void)
{
    for (int i = 0; i < HAND_COUNT; i++) { s_handExists[i] = 0; s_handReachMass[i] = 0.0f; }
}

// `hertz` and `zeta` describe the spring the way Box3D wants it — a frequency
// and a damping ratio — rather than as newtons per metre picked by eye. A gain
// that looks reasonable and is fifty times too large behaves like a physics
// bug rather than a tuning problem, and naming it as a frequency makes that
// impossible to write by accident.
WASM_EXPORT("w_hand_create")
void w_hand_create(int i, float x, float y, float z, float half, float density,
                   float hertz, float zeta, float maxForce)
{
    if (i < 0 || i >= HAND_COUNT || !s_worldCreated) return;
    s_handHalf[i] = half;
    s_handTarget[i] = b3Transform_identity;
    s_handTarget[i].p.x = x; s_handTarget[i].p.y = y; s_handTarget[i].p.z = z;

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position.x = x; bd.position.y = y; bd.position.z = z;
    // Gravity would drag it out of your hand. The spring is meant to hold a
    // pose, not to fight weight.
    bd.gravityScale = 0.0f;
    bd.angularDamping = 4.0f;
    // A punch is FAST, and a fist that is only tested where it happens to land
    // at the end of a step is a fist that goes straight through a forearm. At
    // 20 m/s it covers 28 cm in one step and a limb is 14 cm thick; measured,
    // the whole damage suite went silent the moment the fists got stiff enough
    // to keep up with a hand. Continuous collision is exactly what this flag
    // is for.
    bd.isBullet = true;
    s_handBody[i] = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = density;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;
    sd.enableHitEvents = true;
    b3BoxHull hull = b3MakeBoxHull(half, half, half);
    b3CreateHullShape(s_handBody[i], &sd, &hull.base);

    b3MotorJointDef mj = b3DefaultMotorJointDef();
    mj.base.bodyIdA = s_groundBody;
    mj.base.bodyIdB = s_handBody[i];
    mj.base.localFrameA = b3Transform_identity;
    mj.base.localFrameB = b3Transform_identity;
    // The fist must still meet the floor and everything on it, so the two
    // bodies of this joint are NOT excluded from each other.
    mj.base.collideConnected = true;
    mj.linearHertz = hertz;
    mj.linearDampingRatio = zeta;
    mj.maxSpringForce = maxForce;
    mj.angularHertz = hertz * 0.5f;
    mj.angularDampingRatio = 1.0f;
    mj.maxSpringTorque = maxForce * 0.02f;
    mj.maxVelocityForce = 0.0f;
    mj.maxVelocityTorque = 0.0f;
    s_handJoint[i] = b3CreateMotorJoint(s_world, &mj);

    s_handExists[i] = 1;
    s_handReachMass[i] = 0.0f;
}

// The effective striking mass of the arm BEHIND the fist.
//
// A punch does not arrive as a loose fist: shoulder, arm and a braced body are
// driving it, and a bare fist mass craters one cell where a real blow should
// stave a panel in. This is that arm, as one honest number rather than a fudge
// factor buried in the damage maths. Zero means "just the fist".
WASM_EXPORT("w_hand_reach_mass")
void w_hand_reach_mass(int i, float kg)
{
    if (i < 0 || i >= HAND_COUNT) return;
    s_handReachMass[i] = kg > 0.0f ? kg : 0.0f;
}

// Where your grip is, and how it is turned — a gauntlet that never rolls with
// your wrist reads as a floating block rather than as your hand.
WASM_EXPORT("w_hand_target")
void w_hand_target(int i, float x, float y, float z,
                   float qx, float qy, float qz, float qw)
{
    if (i < 0 || i >= HAND_COUNT || !s_handExists[i]) return;
    s_handTarget[i].p.x = x; s_handTarget[i].p.y = y; s_handTarget[i].p.z = z;
    const float l2 = qx*qx + qy*qy + qz*qz + qw*qw;
    if (l2 > 1e-6f)
    {
        const float inv = 1.0f / __builtin_sqrtf(l2);
        s_handTarget[i].q.v.x = qx * inv; s_handTarget[i].q.v.y = qy * inv;
        s_handTarget[i].q.v.z = qz * inv; s_handTarget[i].q.s = qw * inv;
    }
}

WASM_EXPORT("w_hand_limits")
void w_hand_limits(int i, float hertz, float zeta, float maxForce)
{
    if (i < 0 || i >= HAND_COUNT || !s_handExists[i]) return;
    b3MotorJoint_SetLinearHertz(s_handJoint[i], hertz);
    b3MotorJoint_SetLinearDampingRatio(s_handJoint[i], zeta);
    b3MotorJoint_SetMaxSpringForce(s_handJoint[i], maxForce);
}

// Applied every step, before b3World_Step. The joint's frame on the world side
// IS the target, so driving the fists is moving that frame.
WASM_EXPORT("w_hand_apply")
void w_hand_apply(void)
{
    if (!s_worldCreated) return;
    for (int i = 0; i < HAND_COUNT; i++)
    {
        if (!s_handExists[i]) continue;
        // The frame is expressed in the ground body's local space, and the
        // ground slab does not sit at the origin — it sits half its thickness
        // below the floor the player measured. It never rotates, so the
        // conversion is a subtraction.
        b3Transform fa = b3Transform_identity;
        fa.p.x = s_handTarget[i].p.x;
        fa.p.y = s_handTarget[i].p.y - (s_groundY - 0.1f);
        fa.p.z = s_handTarget[i].p.z;
        fa.q = s_handTarget[i].q;
        b3Joint_SetLocalFrameA(s_handJoint[i], fa);
        b3Body_SetAwake(s_handBody[i], true);
    }
}

WASM_EXPORT("w_hand_mass")
float w_hand_mass(int i)
{
    if (i < 0 || i >= HAND_COUNT || !s_handExists[i]) return 0.0f;
    return b3Body_GetMass(s_handBody[i]);
}

// 8 floats per body: position, rotation, half extent.
WASM_EXPORT("w_hand_state")
float* w_hand_state(void)
{
    static float out[HAND_COUNT * 8];
    for (int i = 0; i < HAND_COUNT; i++)
    {
        float* o = &out[i * 8];
        if (!s_handExists[i]) continue;
        b3Pos p = b3Body_GetPosition(s_handBody[i]);
        b3Quat q = b3Body_GetRotation(s_handBody[i]);
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
        o[7] = s_handHalf[i];
    }
    return out;
}

// Is `b` one of the player's fists, and what is it carrying behind it?
// Returns 0 when it is not a fist at all.
static float handStrikeMass(b3BodyId b)
{
    for (int i = 0; i < HAND_COUNT; i++)
    {
        if (!s_handExists[i]) continue;
        if (!B3_ID_EQUALS(b, s_handBody[i])) continue;
        const float m = b3Body_GetMass(s_handBody[i]);
        return s_handReachMass[i] > m ? s_handReachMass[i] : m;
    }
    return 0.0f;
}

// ---------------------------------------------------------------------------
// Voxel core — Phase 2, with Phase 3's foundation: materials
//
// The design rule that makes destruction affordable on a Quest: a voxel is a
// CELL IN A GRID, not a physics body. The solver only ever sees:
//
//   - one static body per grid whose shapes are greedy-merged runs of intact
//     cells — an untouched wall costs the solver almost nothing;
//   - a debris pool with a hard cap on live cubes, shared by every grid;
//   - a bounded ring of chunk bodies for regions that lose their connection
//     to the ground and fall as one piece.
//
// Damage arrives through contact hit events, so a fist, a thrown block and a
// falling chunk all hurt a wall through one mechanism: momentum — impactor
// mass times approach speed, spent killing cells outward from the hit point.
// A fist counts the whole arm rigidly driving it.
//
// A grid does not have to stand on the ground. It can ride any body — cells
// live in the BODY'S local space and only touch world space at the borders:
// impacts come in through the body's transform, debris and detached chunks
// leave through it. A wall is just the degenerate case where the body is
// static and sits at the grid's corner. This is armour: the same cells,
// worn by something that moves.
//
// Materials are why this section exists twice over: what a thing is made of
// sets how dense its chunks are, how many hits a cell soaks, and what colour
// it breaks into. The same punch that guts a wooden fence bounces off steel
// plate — no per-object tuning, just the table.
// ---------------------------------------------------------------------------

// 20: the figure's SEVENTEEN bones, plus room for loose scenery. Per-bone
// damage IS the vox system — each bone is a small grid, so hits, hp, debris,
// the dent read and the renderer all come free, and a bone that runs out of
// cells is a bone that comes off.
#define VOX_GRIDS 20
#define VOX_MAX 4096
#define VOX_ROWS_MAX 512
#define VOX_RUNS_PER_ROW 16
#define VOX_CHUNK_MAX 10
#define VOX_CHUNK_RUNS 96

typedef struct
{
    float density;      // kg/m^3 — chunk mass comes from this
    float hp;           // damage points one cell soaks
    float colorIdx;     // render palette index for intact cells
    float debrisColor;  // palette index for loose debris cubes
} VoxMaterial;

// wood, stone, steel. Phase 3 grows this table; nothing else changes.
static const VoxMaterial VOX_MATS[] = {
    { 400.0f, 4.0f, 0.0f, 1.0f },
    { 1600.0f, 9.0f, 1.0f, 2.0f },
    { 7800.0f, 32.0f, 2.0f, 4.0f },
};
#define VOX_MAT_COUNT 3

typedef struct
{
    int used;
    int n[3];
    float size;
    int material;
    int anchorAxis;                // cells at coord 0 on this axis are the foundation
    unsigned long long category;   // team bits; 0 means plain scenery
    b3Vec3 local;                  // grid min corner, in the body's local space
    float hp[VOX_MAX];             // <= 0 means gone
    b3BodyId body;
    b3ShapeId rowShape[VOX_ROWS_MAX][VOX_RUNS_PER_ROW];
    int rowShapes[VOX_ROWS_MAX];
    unsigned char rowDirty[VOX_ROWS_MAX];
    int alive;
    int killed;
    int hidden;                    // simulated but not handed to the renderer
    int armOnly;                   // only player-arm impacts damage it (body
                                   // parts must not break on walls or floors)
    float partDens;                // >0 overrides material density: limb parts
                                   // must be liftable by their own joints
    float cellHpMax;               // true per-cell hp ceiling (parts scale it) —
                                   // the dent read is hp against THIS
} VoxGrid;
static VoxGrid s_vxG[VOX_GRIDS];
static int s_vxLastHits = 0;
// Ring of this step's impacts: where, how hard, which grid — the page
// reads it to drive real mesh deformation.
#define VOX_HITEV_MAX 16
static float s_vxHitEv[VOX_HITEV_MAX * 8];   // x,y,z, force, grid, striker vx,vy,vz
static int s_vxHitEvCount = 0;

typedef struct
{
    b3BodyId body;
    int used;
    int material;
    int runCount;
    float runs[VOX_CHUNK_RUNS][6];
} VoxChunk;
static VoxChunk s_vxChunks[VOX_CHUNK_MAX];
static int s_vxChunkNext = 0;

// The live-debris budget, shared across every grid. Cells burst into cubes
// from the shared pool, but no more than this many may exist at once: past
// the cap the oldest is teleported to the newest break. Measured before the
// cap existed, levelling one wall put 496 live cubes in the solver and a
// step cost 8.8 ms; capped, the same collapse costs 2.3 ms. The ring holds
// body ids, not indices, because the shared pool compacts under recycling.
#define VOX_DEBRIS_CAP 150
static b3BodyId s_vxDebrisRing[VOX_DEBRIS_CAP];
static int s_vxDebrisCount = 0;
static int s_vxDebrisNext = 0;

static void vxWorldReset(void)
{
    for (int i = 0; i < VOX_GRIDS; i++) s_vxG[i].used = 0;
    for (int i = 0; i < VOX_CHUNK_MAX; i++) s_vxChunks[i].used = 0;
    s_vxDebrisCount = 0;
    s_vxDebrisNext = 0;
}

static void vxDebris(const VoxGrid* g, float x, float y, float z,
                     float vx, float vy, float vz)
{
    // A hidden grid is simulated here but drawn by the page — the practice
    // piece IS its deformed mesh, and that mesh falls on its own when the
    // budget is spent. Bursting a debris cube as well drops a second,
    // undented box beside it, which is the one thing the piece is not.
    if (g->hidden) return;

    const float half = g->size * 0.42f;
    const float color = VOX_MATS[g->material].debrisColor;
    if (s_vxDebrisCount < VOX_DEBRIS_CAP && s_count < MAX_CUBES)
    {
        addCube(x, y, z, half, vx, vy, vz, color, 3.0f);
        s_vxDebrisRing[s_vxDebrisCount++] = s_cubes[s_count - 1].id;
        return;
    }
    if (s_vxDebrisCount == 0) return;

    b3BodyId id = s_vxDebrisRing[s_vxDebrisNext];
    const int slot = s_vxDebrisNext;
    s_vxDebrisNext = (s_vxDebrisNext + 1) % s_vxDebrisCount;
    if (!b3Body_IsValid(id))
    {
        if (s_count >= MAX_CUBES) return;
        addCube(x, y, z, half, vx, vy, vz, color, 3.0f);
        s_vxDebrisRing[slot] = s_cubes[s_count - 1].id;
        return;
    }
    b3Pos p; p.x = x; p.y = y; p.z = z;
    b3Body_SetTransform(id, p, b3Quat_identity);
    b3Vec3 v; v.x = vx; v.y = vy; v.z = vz;
    b3Body_SetLinearVelocity(id, v);
    v.x = frnd(-3.0f, 3.0f); v.y = frnd(-3.0f, 3.0f); v.z = frnd(-3.0f, 3.0f);
    b3Body_SetAngularVelocity(id, v);
}

// The grid's body pose, as a float transform.
static b3Transform vxPose(const VoxGrid* g)
{
    b3WorldTransform wt = b3Body_GetTransform(g->body);
    b3Transform t;
    t.p.x = (float)wt.p.x; t.p.y = (float)wt.p.y; t.p.z = (float)wt.p.z;
    t.q = wt.q;
    return t;
}

// Cell centre in body-local space.
static b3Vec3 vxCellLocal(const VoxGrid* g, int x, int y, int z)
{
    b3Vec3 v;
    v.x = g->local.x + (x + 0.5f) * g->size;
    v.y = g->local.y + (y + 0.5f) * g->size;
    v.z = g->local.z + (z + 0.5f) * g->size;
    return v;
}

// Squared distance from a world point to the grid's box, in body-local
// space — how the router decides WHICH grid on a body an impact belongs to.
static float vxLocalDist2(const VoxGrid* g, float wx, float wy, float wz)
{
    const b3Transform t = vxPose(g);
    b3Vec3 w = { wx - t.p.x, wy - t.p.y, wz - t.p.z };
    const b3Vec3 lp = b3InvRotateVector(t.q, w);
    float d2 = 0.0f;
    const float lo[3] = { g->local.x, g->local.y, g->local.z };
    const float p[3] = { lp.x, lp.y, lp.z };
    for (int a = 0; a < 3; a++)
    {
        const float hi = lo[a] + g->n[a] * g->size;
        float d = 0.0f;
        if (p[a] < lo[a]) d = lo[a] - p[a];
        else if (p[a] > hi) d = p[a] - hi;
        d2 += d * d;
    }
    return d2;
}

static int vxIdx(const VoxGrid* g, int x, int y, int z)
{
    return x + g->n[0] * (y + g->n[1] * z);
}
static int vxRow(const VoxGrid* g, int y, int z) { return y + g->n[1] * z; }
static int vxAliveAt(const VoxGrid* g, int x, int y, int z)
{
    if (x < 0 || y < 0 || z < 0 || x >= g->n[0] || y >= g->n[1] || z >= g->n[2]) return 0;
    return g->hp[vxIdx(g, x, y, z)] > 0.0f;
}

// Rebuild one row's collision shapes as greedy runs along x.
static void vxMeshRow(VoxGrid* g, int y, int z)
{
    const int row = vxRow(g, y, z);
    for (int i = 0; i < g->rowShapes[row]; i++)
    {
        b3DestroyShape(g->rowShape[row][i], false);
    }
    g->rowShapes[row] = 0;

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.baseMaterial.friction = 0.7f;
    sd.enableHitEvents = true;
    if (g->partDens > 0.0f) sd.density = g->partDens;
    if (g->category)
    {
        // A body belongs to a team: its own limbs pass through each other,
        // everyone else's connect. Without this the figure's own guard
        // ground against its own chest and dented it from the inside.
        sd.filter.categoryBits = g->category;
        sd.filter.maskBits = ~g->category;
    }

    int x = 0;
    while (x < g->n[0])
    {
        if (!vxAliveAt(g, x, y, z)) { x++; continue; }
        int x0 = x;
        while (x < g->n[0] && vxAliveAt(g, x, y, z)) x++;
        if (g->rowShapes[row] >= VOX_RUNS_PER_ROW)
        {
            // More fragments than slots: fold the remainder into the last run
            // rather than dropping collision silently.
            x = g->n[0];
        }
        const float half = 0.5f * g->size;
        const int len = x - x0;
        b3BoxHull hull = b3MakeBoxHull(len * half, half, half);
        b3Transform off = b3Transform_identity;
        off.p.x = g->local.x + (x0 + 0.5f * len) * g->size;
        off.p.y = g->local.y + (y + 0.5f) * g->size;
        off.p.z = g->local.z + (z + 0.5f) * g->size;
        b3Vec3 one = { 1.0f, 1.0f, 1.0f };
        g->rowShape[row][g->rowShapes[row]++] =
            b3CreateTransformedHullShape(g->body, &sd, &hull.base, off, one);
    }
    g->rowDirty[row] = 0;
}

// Shared construction: fill the cells and mesh them onto whatever body the
// grid rides. Returns the handle, or -1 if there is no room.
static int vxBuild(b3BodyId body, b3Vec3 local, int nx, int ny, int nz,
                   float size, int material, int anchorAxis,
                   unsigned long long category)
{
    if (nx * ny * nz > VOX_MAX) return -1;
    if (ny * nz > VOX_ROWS_MAX) return -1;
    if (material < 0 || material >= VOX_MAT_COUNT) material = 1;

    VoxGrid* g = 0;
    for (int i = 0; i < VOX_GRIDS; i++)
        if (!s_vxG[i].used) { g = &s_vxG[i]; break; }
    if (!g) return -1;

    g->used = 1;
    g->n[0] = nx; g->n[1] = ny; g->n[2] = nz;
    g->size = size;
    g->material = material;
    g->anchorAxis = anchorAxis;
    g->category = category;
    g->hidden = 0;
    g->armOnly = 0;
    g->partDens = 0.0f;
    g->local = local;
    g->body = body;
    g->alive = nx * ny * nz;
    g->killed = 0;

    const float hp = VOX_MATS[material].hp;
    g->cellHpMax = hp;
    for (int i = 0; i < nx * ny * nz; i++) g->hp[i] = hp;
    for (int r = 0; r < ny * nz; r++) { g->rowShapes[r] = 0; g->rowDirty[r] = 0; }

    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            vxMeshRow(g, y, z);

    // Anything slower than a shove should not chip a wall.
    b3World_SetHitEventThreshold(s_world, 1.0f);
    return (int)(g - s_vxG);
}

// A BONE is a grid with its own toughness and its own density: hp scales so
// a hand is not a wall; density drops to something a shoulder spring can
// actually lift (at steel density one hand cell weighed 83 kg and every arm
// hung dead); and only your fist can break it.
static void vxMakePart(int grid, float hpMul)
{
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used) return;
    VoxGrid* g = &s_vxG[grid];
    const int total = g->n[0] * g->n[1] * g->n[2];
    for (int i = 0; i < total; i++) g->hp[i] *= hpMul;
    g->cellHpMax *= hpMul;
    g->armOnly = 1;
    g->partDens = 220.0f;
    for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
            vxMeshRow(g, y, z);        // remesh so the new density takes
}

// Override a bone's density after the fact, so a trunk can be heavy while a
// hand stays light enough for its own shoulder to throw.
static void vxPartDensity(int grid, float dens)
{
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used || dens <= 0.0f) return;
    VoxGrid* g = &s_vxG[grid];
    g->partDens = dens;
    for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
            vxMeshRow(g, y, z);
}

// A wall: a grid standing on the ground on its own static body.
WASM_EXPORT("w_vox_create")
int w_vox_create(float cx, float baseY, float cz, int nx, int ny, int nz,
                 float size, int material)
{
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_staticBody;
    bd.position.x = cx - 0.5f * nx * size;
    bd.position.y = baseY;
    bd.position.z = cz - 0.5f * nz * size;
    b3BodyId body = b3CreateBody(s_world, &bd);
    b3Vec3 zero = { 0.0f, 0.0f, 0.0f };
    return vxBuild(body, zero, nx, ny, nz, size, material, 1, 0);
}

// Spend impact damage killing cells outward from the hit cell. The point
// arrives in world space and is pulled into the body's local frame, so a
// grid riding a swinging body still knows exactly which cell was struck.
// A blow makes the body GIVE where it landed. Defined with the skeleton below;
// the hit loop needs it up here. Every bone is held in its pose by a stiff
// spring standing in for muscle, and stiff everywhere means the skeleton
// answers a punch as one solid assembly swinging about a point — which is what
// a playtest called floating around a midpoint. This slackens the springs at
// the bone that was struck, less and less along the chain away from it, and
// they tighten back over about a third of a second: it gives, then gathers
// itself up. A boxer, not a rag doll.
static void figSlacken(int gi, float speed);

static void vxDamageAt(VoxGrid* g, float wx, float wy, float wz, float damage)
{
    const b3Transform t = vxPose(g);
    b3Vec3 w = { wx - t.p.x, wy - t.p.y, wz - t.p.z };
    const b3Vec3 lp = b3InvRotateVector(t.q, w);
    int cx = (int)((lp.x - g->local.x) / g->size);
    int cy = (int)((lp.y - g->local.y) / g->size);
    int cz = (int)((lp.z - g->local.z) / g->size);
    if (cx < 0) cx = 0; if (cx >= g->n[0]) cx = g->n[0] - 1;
    if (cy < 0) cy = 0; if (cy >= g->n[1]) cy = g->n[1] - 1;
    if (cz < 0) cz = 0; if (cz >= g->n[2]) cz = g->n[2] - 1;

    // Ring by ring outward, nearest cells soaking damage first. Radius 2 is
    // as wide as a single impact reaches; bigger holes come from more hits.
    for (int ring = 0; ring <= 2 && damage > 0.0f; ring++)
    {
        for (int dz = -ring; dz <= ring && damage > 0.0f; dz++)
        for (int dy = -ring; dy <= ring && damage > 0.0f; dy++)
        for (int dx = -ring; dx <= ring && damage > 0.0f; dx++)
        {
            int m = (dx < 0 ? -dx : dx);
            int a = (dy < 0 ? -dy : dy);
            if (a > m) m = a;
            a = (dz < 0 ? -dz : dz);
            if (a > m) m = a;
            if (m != ring) continue;              // surface of this ring only
            const int x = cx + dx, y = cy + dy, z = cz + dz;
            if (!vxAliveAt(g, x, y, z)) continue;
            const int idx = vxIdx(g, x, y, z);
            const float spent = g->hp[idx] < damage ? g->hp[idx] : damage;
            g->hp[idx] -= spent;
            damage -= spent;
            g->rowDirty[vxRow(g, y, z)] = 1;
            if (g->hp[idx] <= 0.0f)
            {
                g->alive--; g->killed++;
                const b3Vec3 wc = b3TransformPoint(t, vxCellLocal(g, x, y, z));
                vxDebris(g, wc.x, wc.y, wc.z,
                         frnd(-0.6f, 0.6f), frnd(0.5f, 1.6f), frnd(-0.6f, 0.6f));
            }
        }
    }
}

// Flood from the ground row; anything alive that cannot reach it detaches.
static void vxStructure(VoxGrid* g)
{
    static unsigned char mark[VOX_MAX];
    static unsigned char island[VOX_MAX];
    static unsigned short queue[VOX_MAX];
    const int nx = g->n[0], ny = g->n[1], nz = g->n[2];
    const int total = nx * ny * nz;
    for (int i = 0; i < total; i++) { mark[i] = 0; island[i] = 0; }

    // The foundation row: coordinate zero along the anchor axis. For a wall
    // that is the row on the ground; for a worn plate it is the layer bolted
    // to the body.
    int qn = 0;
    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            for (int x = 0; x < nx; x++)
            {
                const int c = g->anchorAxis == 0 ? x : (g->anchorAxis == 1 ? y : z);
                if (c != 0) continue;
                if (!vxAliveAt(g, x, y, z)) continue;
                const int i = vxIdx(g, x, y, z);
                mark[i] = 1;
                queue[qn++] = (unsigned short)i;
            }

    while (qn > 0)
    {
        const int i = queue[--qn];
        const int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
        const int nbs[6][3] = { {x-1,y,z}, {x+1,y,z}, {x,y-1,z}, {x,y+1,z}, {x,y,z-1}, {x,y,z+1} };
        for (int k = 0; k < 6; k++)
        {
            const int a = nbs[k][0], b = nbs[k][1], c = nbs[k][2];
            if (!vxAliveAt(g, a, b, c)) continue;
            const int j = vxIdx(g, a, b, c);
            if (mark[j]) continue;
            mark[j] = 1;
            queue[qn++] = (unsigned short)j;
        }
    }

    for (int start = 0; start < total; start++)
    {
        if (g->hp[start] <= 0.0f || mark[start] || island[start]) continue;

        int members[VOX_MAX / 8];
        int mn = 0, qn2 = 0;
        queue[qn2++] = (unsigned short)start;
        island[start] = 1;
        while (qn2 > 0 && mn < VOX_MAX / 8)
        {
            const int i = queue[--qn2];
            members[mn++] = i;
            const int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
            const int nbs[6][3] = { {x-1,y,z}, {x+1,y,z}, {x,y-1,z}, {x,y+1,z}, {x,y,z-1}, {x,y,z+1} };
            for (int k = 0; k < 6; k++)
            {
                const int a = nbs[k][0], b = nbs[k][1], c = nbs[k][2];
                if (!vxAliveAt(g, a, b, c)) continue;
                const int j = vxIdx(g, a, b, c);
                if (mark[j] || island[j]) continue;
                island[j] = 1;
                queue[qn2++] = (unsigned short)j;
            }
        }

        if (mn <= 3)
        {
            const b3Transform t = vxPose(g);
            for (int m = 0; m < mn; m++)
            {
                const int i = members[m];
                const int x = i % nx, y = (i / nx) % ny, z = i / (nx * ny);
                g->hp[i] = 0.0f;
                g->alive--; g->killed++;
                g->rowDirty[vxRow(g, y, z)] = 1;
                const b3Vec3 wc = b3TransformPoint(t, vxCellLocal(g, x, y, z));
                vxDebris(g, wc.x, wc.y, wc.z,
                         frnd(-0.4f, 0.4f), 0.0f, frnd(-0.4f, 0.4f));
            }
            continue;
        }

        // A real chunk: one dynamic body, its shapes the island's x-runs.
        // The ring is the demotion budget — admitting a new chunk may retire
        // the oldest one still lying about.
        VoxChunk* ch = &s_vxChunks[s_vxChunkNext];
        if (ch->used) b3DestroyBody(ch->body);
        s_vxChunkNext = (s_vxChunkNext + 1) % VOX_CHUNK_MAX;

        // The chunk is born at the body's current pose with its velocity, so
        // a plate knocked off a swinging target flies the way it should.
        const b3Transform gt = vxPose(g);
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position.x = gt.p.x;
        bd.position.y = gt.p.y;
        bd.position.z = gt.p.z;
        bd.rotation = gt.q;
        bd.linearVelocity = b3Body_GetLinearVelocity(g->body);
        ch->body = b3CreateBody(s_world, &bd);
        ch->used = 1;
        ch->material = g->material;
        ch->runCount = 0;

        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = VOX_MATS[g->material].density;
        sd.baseMaterial.friction = 0.7f;
        sd.enableHitEvents = true;

        for (int m = 0; m < mn; m++)
        {
            const int i = members[m];
            if (g->hp[i] <= 0.0f) continue;       // swallowed by an earlier run
            const int y = (i / nx) % ny, z = i / (nx * ny);
            int x0 = i % nx, x1 = x0;
            while (x1 + 1 < nx)
            {
                const int j = vxIdx(g, x1 + 1, y, z);
                if (g->hp[j] <= 0.0f || !island[j] || mark[j]) break;
                x1++;
            }
            const int len = x1 - x0 + 1;
            for (int x = x0; x <= x1; x++)
            {
                g->hp[vxIdx(g, x, y, z)] = 0.0f;
                g->alive--;
            }
            g->rowDirty[vxRow(g, y, z)] = 1;

            if (ch->runCount < VOX_CHUNK_RUNS)
            {
                const float half = 0.5f * g->size;
                b3BoxHull hull = b3MakeBoxHull(len * half, half, half);
                b3Transform off = b3Transform_identity;
                off.p.x = g->local.x + (x0 + 0.5f * len) * g->size;
                off.p.y = g->local.y + (y + 0.5f) * g->size;
                off.p.z = g->local.z + (z + 0.5f) * g->size;
                b3Vec3 one = { 1.0f, 1.0f, 1.0f };
                b3CreateTransformedHullShape(ch->body, &sd, &hull.base, off, one);
                ch->runs[ch->runCount][0] = off.p.x;
                ch->runs[ch->runCount][1] = off.p.y;
                ch->runs[ch->runCount][2] = off.p.z;
                ch->runs[ch->runCount][3] = len * half;
                ch->runs[ch->runCount][4] = half;
                ch->runs[ch->runCount][5] = half;
                ch->runCount++;
            }
        }
    }
}

static void vxRemesh(VoxGrid* g)
{
    for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
            if (g->rowDirty[vxRow(g, y, z)])
                vxMeshRow(g, y, z);
}

// Point damage from anything that is not a physical impact — explosions,
// weapon hits, tests. Hits whichever grid contains the point.
WASM_EXPORT("w_vox_blast")
void w_vox_blast(float x, float y, float z, float damage)
{
    for (int i = 0; i < VOX_GRIDS; i++)
    {
        VoxGrid* g = &s_vxG[i];
        if (!g->used) continue;
        const b3Transform t = vxPose(g);
        b3Vec3 w = { x - t.p.x, y - t.p.y, z - t.p.z };
        const b3Vec3 lp = b3InvRotateVector(t.q, w);
        const float m = g->size * 1.5f;
        if (lp.x < g->local.x - m || lp.x > g->local.x + g->n[0] * g->size + m) continue;
        if (lp.y < g->local.y - m || lp.y > g->local.y + g->n[1] * g->size + m) continue;
        if (lp.z < g->local.z - m || lp.z > g->local.z + g->n[2] * g->size + m) continue;
        vxDamageAt(g, x, y, z, damage);
        vxStructure(g);
        vxRemesh(g);
    }
}

// Call once per frame, after w_step: reads the step's impacts, damages cells,
// detaches whatever lost its footing, and remeshes only the rows that changed.
WASM_EXPORT("w_vox_post")
void w_vox_post(void)
{
    int any = 0;
    for (int i = 0; i < VOX_GRIDS; i++) any |= s_vxG[i].used;
    if (!any) return;

    b3ContactEvents ev = b3World_GetContactEvents(s_world);
    s_vxLastHits = 0;
    s_vxHitEvCount = 0;
    int killedBefore[VOX_GRIDS];
    for (int i = 0; i < VOX_GRIDS; i++) killedBefore[i] = s_vxG[i].killed;

    for (int i = 0; i < ev.hitCount; i++)
    {
        const b3ContactHitEvent* h = &ev.hitEvents[i];
        b3BodyId bodyA = b3Shape_GetBody(h->shapeIdA);
        b3BodyId bodyB = b3Shape_GetBody(h->shapeIdB);

        // One body carries SEVERAL grids (a bone wears its own, and severed
        // bones keep theirs). First-match routing sent a head hit into the
        // chest — and vxDamageAt clamps into bounds, so it even "landed".
        // Pick the grid whose box actually contains the impact, nearest
        // wins, and let nothing but a fist break a bone.
        VoxGrid* g = 0;
        b3BodyId other;
        float strikeMass = 0.0f;
        float bestD = 1e30f;
        for (int k = 0; k < VOX_GRIDS; k++)
        {
            if (!s_vxG[k].used) continue;
            b3BodyId oth;
            if (B3_ID_EQUALS(bodyA, s_vxG[k].body)) oth = bodyB;
            else if (B3_ID_EQUALS(bodyB, s_vxG[k].body)) oth = bodyA;
            else continue;
            float sm = 0.0f;
            if (s_vxG[k].armOnly)
            {
                // A fist, and a fist that is actually MOVING. Without both
                // gates the figure took itself apart: its own swings landing
                // on your resting guard broke its hands, and the spring that
                // yanks your fist back after a shove read as another punch.
                // Your deliberate blow breaks bone; everything else is two
                // bodies touching.
                sm = handStrikeMass(oth);
                if (sm <= 0.0f) continue;
                b3Vec3 vo = b3Body_GetLinearVelocity(oth);
                b3Vec3 vg = b3Body_GetLinearVelocity(s_vxG[k].body);
                const float so = vo.x*vo.x + vo.y*vo.y + vo.z*vo.z;
                const float sg = vg.x*vg.x + vg.y*vg.y + vg.z*vg.z;
                if (h->approachSpeed < 1.8f) continue;
                if (so < sg * 1.3f + 0.25f) continue;
            }
            const float d2 = vxLocalDist2(&s_vxG[k], (float)h->point.x,
                                          (float)h->point.y, (float)h->point.z);
            if (d2 < bestD) { bestD = d2; g = &s_vxG[k]; other = oth; strikeMass = sm; }
        }
        if (!g) continue;
        // Beyond a cell and a half from the chosen grid's box, the impact
        // belongs to a bare hull shape, not to any cell of this grid.
        if (bestD > (1.5f * g->size) * (1.5f * g->size)) continue;

        // A punch does not arrive as a loose fist — the arm behind it is
        // driving it, and that is what the impactor's effective mass is.
        // Without it a committed blow chipped one cell where it should
        // stave the panel in.
        float mass = strikeMass > 0.0f ? strikeMass : b3Body_GetMass(other);
        if (mass <= 0.0f) continue;

        s_vxLastHits++;
        if (s_vxHitEvCount < VOX_HITEV_MAX)
        {
            float* he = &s_vxHitEv[s_vxHitEvCount * 8];
            he[0] = (float)h->point.x; he[1] = (float)h->point.y;
            he[2] = (float)h->point.z;
            he[3] = h->approachSpeed * mass;
            he[4] = (float)(g - s_vxG);
            // The striker's velocity: deformation follows the BLOW, so a
            // hammer-fist on the top of a box craters straight down.
            b3Vec3 sv = b3Body_GetLinearVelocity(other);
            he[5] = sv.x; he[6] = sv.y; he[7] = sv.z;
            s_vxHitEvCount++;
        }
        vxDamageAt(g, (float)h->point.x, (float)h->point.y, (float)h->point.z,
                   h->approachSpeed * mass);

        // Only a real strike gives — strikeMass is already gated on a moving
        // fist, so the figure brushing your guard does not make it fold.
        if (strikeMass > 0.0f)
            figSlacken((int)(g - s_vxG), h->approachSpeed);
    }

    for (int i = 0; i < VOX_GRIDS; i++)
    {
        VoxGrid* g = &s_vxG[i];
        if (!g->used) continue;
        if (g->killed != killedBefore[i]) vxStructure(g);
        vxRemesh(g);
    }
}

// Render data: every grid's intact cells as merged runs, posed in world
// space. Twelve floats each: centre, quat, half extents, a hurt flag (cell
// under half health — a run breaks where the flag changes, so damage shows
// before cells die), material.
static float s_vxRunBuf[VOX_GRIDS * VOX_ROWS_MAX * VOX_RUNS_PER_ROW * 12 / 2];
static int s_vxRunCount = 0;

static void vxFillRuns(void)
{
    s_vxRunCount = 0;
    const int cap = (int)(sizeof(s_vxRunBuf) / sizeof(float) / 12);
    for (int gi = 0; gi < VOX_GRIDS; gi++)
    {
        const VoxGrid* g = &s_vxG[gi];
        if (!g->used || g->hidden) continue;
        const b3Transform t = vxPose(g);
        const float half = 0.5f * g->size;
        // Dent = damage, graded: hp measured against the grid's TRUE cell
        // ceiling (parts scale theirs — against the raw material number a
        // part could lose most of its metal and still read pristine).
        const float maxHp = g->cellHpMax > 0.0f ? g->cellHpMax : VOX_MATS[g->material].hp;
        b3Vec3 gc;
        gc.x = g->local.x + 0.5f * g->n[0] * g->size;
        gc.y = g->local.y + 0.5f * g->n[1] * g->size;
        gc.z = g->local.z + 0.5f * g->n[2] * g->size;
        for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
        {
            int x = 0;
            while (x < g->n[0])
            {
                if (!vxAliveAt(g, x, y, z)) { x++; continue; }
                float d0 = 1.0f - g->hp[vxIdx(g, x, y, z)] / maxHp;
                if (d0 < 0.0f) d0 = 0.0f; if (d0 > 0.96f) d0 = 0.96f;
                const int q0 = (int)(d0 * 4.0f);
                int x0 = x;
                while (x < g->n[0] && vxAliveAt(g, x, y, z))
                {
                    float dn = 1.0f - g->hp[vxIdx(g, x, y, z)] / maxHp;
                    if (dn < 0.0f) dn = 0.0f; if (dn > 0.96f) dn = 0.96f;
                    if ((int)(dn * 4.0f) != q0) break;
                    x++;
                }
                if (s_vxRunCount >= cap) return;
                float* o = &s_vxRunBuf[s_vxRunCount * 12];
                const int len = x - x0;
                const float d = q0 * 0.25f;
                b3Vec3 c;
                c.x = g->local.x + (x0 + 0.5f * len) * g->size;
                c.y = g->local.y + (y + 0.5f) * g->size;
                c.z = g->local.z + (z + 0.5f) * g->size;
                // The crumple: dented metal caves toward the body's centre,
                // deeper the more it has taken.
                if (d > 0.0f)
                {
                    b3Vec3 in = { gc.x - c.x, gc.y - c.y, gc.z - c.z };
                    const float il = __builtin_sqrtf(in.x*in.x + in.y*in.y + in.z*in.z);
                    if (il > 1e-4f)
                    {
                        const float push = d * g->size * 0.45f;
                        c.x += in.x / il * push;
                        c.y += in.y / il * push;
                        c.z += in.z / il * push;
                    }
                }
                const b3Vec3 wc = b3TransformPoint(t, c);
                o[0] = wc.x; o[1] = wc.y; o[2] = wc.z;
                o[3] = t.q.v.x; o[4] = t.q.v.y; o[5] = t.q.v.z; o[6] = t.q.s;
                o[7] = len * half; o[8] = half; o[9] = half;
                o[10] = d;
                o[11] = VOX_MATS[g->material].colorIdx;
                s_vxRunCount++;
            }
        }
    }
}

// Scale a grid's hp budget (and its dent ceiling with it) — the practice
// piece wants one LONG life, not sixty-four small ones.
WASM_EXPORT("w_vox_scale_hp")
void w_vox_scale_hp(int grid, float mul)
{
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used || mul <= 0.0f) return;
    VoxGrid* g = &s_vxG[grid];
    const int total = g->n[0] * g->n[1] * g->n[2];
    for (int i = 0; i < total; i++) g->hp[i] *= mul;
    g->cellHpMax *= mul;
}

// Where a grid is in the world and how big it is, so a page that draws a grid
// ITSELF — a deforming mesh in place of the engine's boxes — can follow it.
// This is what lets the machine's own body crumple the way the practice piece
// does, instead of shedding cells whole.
// 11 floats: centre x,y,z, rotation qx,qy,qz,qw, half extents hx,hy,hz, and the
// fraction of the grid still alive.
WASM_EXPORT("w_vox_grid_pose")
float* w_vox_grid_pose(int grid)
{
    static float out[11];
    for (int i = 0; i < 11; i++) out[i] = 0.0f;
    out[6] = 1.0f;
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used) return out;

    const VoxGrid* g = &s_vxG[grid];
    const b3Transform t = vxPose(g);
    b3Vec3 gc;
    gc.x = g->local.x + 0.5f * (float)g->n[0] * g->size;
    gc.y = g->local.y + 0.5f * (float)g->n[1] * g->size;
    gc.z = g->local.z + 0.5f * (float)g->n[2] * g->size;
    const b3Vec3 w = b3TransformPoint(t, gc);

    out[0] = w.x; out[1] = w.y; out[2] = w.z;
    out[3] = t.q.v.x; out[4] = t.q.v.y; out[5] = t.q.v.z; out[6] = t.q.s;
    out[7] = 0.5f * (float)g->n[0] * g->size;
    out[8] = 0.5f * (float)g->n[1] * g->size;
    out[9] = 0.5f * (float)g->n[2] * g->size;
    const int total = g->n[0] * g->n[1] * g->n[2];
    out[10] = total > 0 ? (float)g->alive / (float)total : 0.0f;
    return out;
}

WASM_EXPORT("w_vox_hit_count")
int w_vox_hit_count(void) { return s_vxHitEvCount; }

WASM_EXPORT("w_vox_hit_events")
float* w_vox_hit_events(void) { return s_vxHitEv; }

WASM_EXPORT("w_vox_run_count")
int w_vox_run_count(void) { vxFillRuns(); return s_vxRunCount; }

WASM_EXPORT("w_vox_runs")
float* w_vox_runs(void) { return s_vxRunBuf; }

// Fallen chunks, flattened to world-space boxes: pos, quat, half extents,
// material. Eleven floats per box.
static float s_vxChunkBuf[VOX_CHUNK_MAX * VOX_CHUNK_RUNS * 11];
static int s_vxChunkBoxCount = 0;

static void vxFillChunkBoxes(void)
{
    s_vxChunkBoxCount = 0;
    for (int c = 0; c < VOX_CHUNK_MAX; c++)
    {
        if (!s_vxChunks[c].used) continue;
        b3Pos p = b3Body_GetPosition(s_vxChunks[c].body);
        b3Quat q = b3Body_GetRotation(s_vxChunks[c].body);
        b3Matrix3 m = b3MakeMatrixFromQuat(q);
        for (int r = 0; r < s_vxChunks[c].runCount; r++)
        {
            const float* run = s_vxChunks[c].runs[r];
            float* o = &s_vxChunkBuf[s_vxChunkBoxCount * 11];
            o[0] = (float)p.x + m.cx.x * run[0] + m.cy.x * run[1] + m.cz.x * run[2];
            o[1] = (float)p.y + m.cx.y * run[0] + m.cy.y * run[1] + m.cz.y * run[2];
            o[2] = (float)p.z + m.cx.z * run[0] + m.cy.z * run[1] + m.cz.z * run[2];
            o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
            o[7] = run[3]; o[8] = run[4]; o[9] = run[5];
            o[10] = VOX_MATS[s_vxChunks[c].material].colorIdx;
            s_vxChunkBoxCount++;
        }
    }
}

WASM_EXPORT("w_vox_chunk_box_count")
int w_vox_chunk_box_count(void) { vxFillChunkBoxes(); return s_vxChunkBoxCount; }

WASM_EXPORT("w_vox_chunk_boxes")
float* w_vox_chunk_boxes(void) { return s_vxChunkBuf; }

// Aggregate: [alive, killed, chunks, staticShapes, hitsLastPost]
WASM_EXPORT("w_vox_stats")
float* w_vox_stats(void)
{
    static float out[5];
    int alive = 0, killed = 0, shapes = 0, chunks = 0;
    for (int i = 0; i < VOX_GRIDS; i++)
    {
        const VoxGrid* g = &s_vxG[i];
        if (!g->used) continue;
        alive += g->alive;
        killed += g->killed;
        for (int r = 0; r < g->n[1] * g->n[2]; r++) shapes += g->rowShapes[r];
    }
    for (int c = 0; c < VOX_CHUNK_MAX; c++) chunks += s_vxChunks[c].used ? 1 : 0;
    out[0] = (float)alive;
    out[1] = (float)killed;
    out[2] = (float)chunks;
    out[3] = (float)shapes;
    out[4] = (float)s_vxLastHits;
    return out;
}

// Restore every cell of a grid to full health and refill the holes.
WASM_EXPORT("w_vox_heal")
void w_vox_heal(int grid)
{
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used) return;
    VoxGrid* g = &s_vxG[grid];
    const float hp = VOX_MATS[g->material].hp;
    const int total = g->n[0] * g->n[1] * g->n[2];
    int revived = 0;
    for (int i = 0; i < total; i++)
    {
        if (g->hp[i] <= 0.0f) revived++;
        g->hp[i] = hp;
    }
    g->alive = total;
    for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
            vxMeshRow(g, y, z);
    (void)revived;
}

// A hidden grid keeps simulating and keeps taking hits; it just stays out of
// the engine's draw list. Every bone is hidden — the page draws each one as a
// deformable mesh instead, so a blow craters the surface rather than
// repainting a box.
// How far through this grid the damage has got, 0 to 1.
//
// The cell count alone cannot answer that: a cell only leaves the count when
// it is completely gone, so four solid punches into a tough limb read as no
// progress at all. Everything above a cell's death — how a dent deepens, how
// close a bone is to coming off — lives in the accumulated hp, and until this
// existed there was no way to see it from outside.
static float vxDamage(const VoxGrid* g)
{
    const int total = g->n[0] * g->n[1] * g->n[2];
    if (total <= 0 || g->cellHpMax <= 0.0f) return 0.0f;
    float left = 0.0f;
    for (int i = 0; i < total; i++) if (g->hp[i] > 0.0f) left += g->hp[i];
    const float full = (float)total * g->cellHpMax;
    const float d = 1.0f - left / full;
    return d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

WASM_EXPORT("w_vox_hide")
void w_vox_hide(int grid, int hide)
{
    if (grid < 0 || grid >= VOX_GRIDS) return;
    s_vxG[grid].hidden = hide ? 1 : 0;
}

WASM_EXPORT("w_vox_grid_stats")
float* w_vox_grid_stats(int grid)
{
    static float out[4];
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used) return out;
    const VoxGrid* g = &s_vxG[grid];
    out[0] = (float)g->alive;
    out[1] = (float)g->killed;
    out[2] = (float)(g->n[0] * g->n[1] * g->n[2]);
    out[3] = vxDamage(g);
    return out;
}


// ---------------------------------------------------------------------------
// The figure — one opponent, built on a human skeleton
//
// Seventeen bones and sixteen joints, laid out on real anthropometry: every
// length here is a fraction of standing height, and the height is the height
// of the person wearing the headset. Stand next to it and it is your size,
// because it was built from your measurement.
//
//   pelvis - abdomen - chest - neck - head          the spine, five bones
//   chest  - upper arm - forearm - hand             each arm, three
//   pelvis - thigh - shin - foot                    each leg, three
//
// The joints are the joints you have. Shoulders and hips are ball joints with
// a cone. Elbows and knees are HINGES, and they bend the way yours bend —
// the elbow forward, the knee back — which is most of why the thing reads as
// a body rather than a rig. Spine, neck, wrists and ankles are ball joints
// with tight cones.
//
// What there is no of: a core, a reactor, an engine, a chest plate, a weak
// point that ends the fight when you find it. Every bone is a voxel grid, so
// every bone dents under a fist, and a bone battered past a third of itself
// BREAKS — its joint is destroyed and it falls, taking everything below it
// with it. Break an elbow and the forearm and hand go together. That is not a
// special case in the code; it is what a skeleton is.
//
// It goes down when it cannot stand: both legs gone, or the spine broken. No
// explosion, nothing to detonate. You take the body apart.
//
// The whole build is in the REST POSE with every body at identity rotation.
// That single decision pays for itself all through this file: every joint's
// rest target is the identity quaternion, so a pose is a rotation away from
// standing rather than an absolute frame nobody can picture.
// ---------------------------------------------------------------------------

enum {
    BONE_PELVIS = 0, BONE_ABDOMEN, BONE_CHEST, BONE_NECK, BONE_HEAD,
    BONE_L_UPPERARM, BONE_L_FOREARM, BONE_L_HAND,
    BONE_R_UPPERARM, BONE_R_FOREARM, BONE_R_HAND,
    BONE_L_THIGH, BONE_L_SHIN, BONE_L_FOOT,
    BONE_R_THIGH, BONE_R_SHIN, BONE_R_FOOT,
    BONE_COUNT
};
// Arms and legs index as BONE_L_* + side * 3.
#define ARM(side) (BONE_L_UPPERARM + (side) * 3)
#define LEG(side) (BONE_L_THIGH + (side) * 3)

// How a bone's joint frame is oriented, which decides where its cone points
// and which way its hinge turns.
typedef enum {
    AX_UP = 0,     // the bone extends along its own +Y  (the spine)
    AX_DOWN,       // along its own -Y                   (arms, legs)
    AX_FWD,        // along its own +Z                   (feet)
    AX_HINGE       // a hinge turning about world X      (elbows, knees)
} BoneAxis;

typedef struct
{
    int parent;              // -1 for the root
    float ox, oy, oz;        // rest origin: the JOINT, in stature fractions
    float hx, hy, hz;        // half extents, in stature fractions
    int cells;               // cells along the longest axis, and the taper of a
                             // limb comes from here: a shin given the same
                             // count as its thigh gets the same cell size and
                             // therefore the same thickness, and the whole leg
                             // reads as one uninterrupted shaft with no knee
                             // in it. Looking at a render is the only thing
                             // that catches that; every measurement passed.
                             // It sets the CELL
                             // SIZE, so it is also what decides the other two
                             // dimensions — the foot's 2 made a cubic cell
                             // 13 cm on a side, which is a correct 27 cm long
                             // foot and a 13 cm THICK one. Six centimetres of
                             // extra ankle drove the soles through the floor,
                             // the leg chain jammed, and floor friction pinned
                             // the figure so hard it could no longer turn to
                             // face anybody: 10 N m of heading torque went in
                             // every step for three seconds and moved it 0.06
                             // radians. The leg chain and the stand height
                             // have to agree to the millimetre.
    BoneAxis axis;
    float cone;              // cone half-angle, or hinge lower limit
    float hi;                // hinge upper limit (hinges only)
    float hertz;             // joint spring stiffness
    float hpMul;             // how much punishment one cell of it soaks
    float dens;              // kg/m^3 — trunk heavy, limbs light, as a body is
} BoneDef;

// Anthropometry, as fractions of stature. Ankle .039, knee .285, hip .530,
// waist .620, shoulder .820, chin .845, crown 1.000 — the standard canon,
// which is why the silhouette comes out human without anybody art-directing
// it. The origin of every bone is the joint it hangs from.
//
// The spring rates are MUSCLE, and the first cut of them was not. At 5-9 Hz
// every joint sagged a little under gravity, the sags added down the chain,
// and the measurement was unambiguous: chest 25 cm low, head 68 cm low, the
// whole figure folded forward over its own hips and face-planted inside four
// seconds without being touched. Nothing was unstable and nothing errored —
// it just slowly lay down. A standing body is actively held at every joint,
// so these are stiff enough to hold it and damped just under critical.
static const BoneDef BONES[BONE_COUNT] = {
  /* PELVIS   */ { -1,            0.000f, 0.530f, 0.000f, 0.075f, 0.045f, 0.055f, 2, AX_UP,    0.00f, 0,  0.0f, 3.0f , 900.0f },
  /* ABDOMEN  */ { BONE_PELVIS,   0.000f, 0.620f, 0.000f, 0.065f, 0.050f, 0.050f, 2, AX_UP,    0.45f, 0, 28.0f, 2.6f , 900.0f },
  /* CHEST    */ { BONE_ABDOMEN,  0.000f, 0.720f, 0.000f, 0.085f, 0.063f, 0.060f, 3, AX_UP,    0.40f, 0, 28.0f, 1.8f , 900.0f },
  /* NECK     */ { BONE_CHEST,    0.000f, 0.845f, 0.000f, 0.026f, 0.023f, 0.026f, 2, AX_UP,    0.50f, 0, 22.0f, 2.2f , 600.0f },
  /* HEAD     */ { BONE_NECK,     0.000f, 0.890f, 0.000f, 0.055f, 0.055f, 0.060f, 2, AX_UP,    0.45f, 0, 22.0f, 1.5f , 650.0f },

  /* L UPARM  */ { BONE_CHEST,   -0.115f, 0.820f, 0.000f, 0.030f, 0.095f, 0.030f, 3, AX_DOWN,  1.45f, 0, 20.0f, 1.6f , 420.0f },
  /* L FOREARM*/ { BONE_L_UPPERARM, -0.115f, 0.630f, 0.000f, 0.026f, 0.073f, 0.026f, 4, AX_HINGE, -2.50f, 0.0f, 20.0f, 1.0f , 380.0f },
  /* L HAND   */ { BONE_L_FOREARM,  -0.115f, 0.485f, 0.000f, 0.030f, 0.055f, 0.022f, 2, AX_DOWN,  0.55f, 0, 16.0f, 1.2f , 340.0f },

  /* R UPARM  */ { BONE_CHEST,    0.115f, 0.820f, 0.000f, 0.030f, 0.095f, 0.030f, 3, AX_DOWN,  1.45f, 0, 20.0f, 1.6f , 420.0f },
  /* R FOREARM*/ { BONE_R_UPPERARM,  0.115f, 0.630f, 0.000f, 0.026f, 0.073f, 0.026f, 4, AX_HINGE, -2.50f, 0.0f, 20.0f, 1.0f , 380.0f },
  /* R HAND   */ { BONE_R_FOREARM,   0.115f, 0.485f, 0.000f, 0.030f, 0.055f, 0.022f, 2, AX_DOWN,  0.55f, 0, 16.0f, 1.2f , 340.0f },

  /* L THIGH  */ { BONE_PELVIS,  -0.045f, 0.530f, 0.000f, 0.045f, 0.123f, 0.045f, 3, AX_DOWN,  1.00f, 0, 28.0f, 2.8f , 620.0f },
  /* L SHIN   */ { BONE_L_THIGH, -0.045f, 0.285f, 0.000f, 0.037f, 0.123f, 0.037f, 4, AX_HINGE, 0.00f, 2.40f, 26.0f, 1.6f , 520.0f },
  /* L FOOT   */ { BONE_L_SHIN,  -0.045f, 0.039f, 0.000f, 0.030f, 0.020f, 0.076f, 4, AX_FWD,   0.50f, 0, 20.0f, 1.8f , 420.0f },

  /* R THIGH  */ { BONE_PELVIS,   0.045f, 0.530f, 0.000f, 0.045f, 0.123f, 0.045f, 3, AX_DOWN,  1.00f, 0, 28.0f, 2.8f , 620.0f },
  /* R SHIN   */ { BONE_R_THIGH,  0.045f, 0.285f, 0.000f, 0.037f, 0.123f, 0.037f, 4, AX_HINGE, 0.00f, 2.40f, 26.0f, 1.6f , 520.0f },
  /* R FOOT   */ { BONE_R_SHIN,   0.045f, 0.039f, 0.000f, 0.030f, 0.020f, 0.076f, 4, AX_FWD,   0.50f, 0, 20.0f, 1.8f , 420.0f },
};

// Arms are softer than legs, and the reason is measured rather than chosen: a
// limb that is free to swing gives way under a blow, so far less of the
// momentum stays in it. At leg toughness an arm took thirty-odd landed hits
// against a thigh's eight, which is backwards — the legs are meant to be the
// efficient way to put it down.
//
// A bone breaks when it is battered down to a third of itself or less. Not
// when the last cell goes: a limb that only comes off once it has been
// entirely erased never reads as coming off, it reads as fading out.
#define FIG_BREAK_NUM 1
#define FIG_BREAK_DEN 3

// Its own collision team, so its guard cannot grind against its own chest.
#define FIG_CATEGORY 0x4ULL

typedef enum {
    FIG_WAIT = 0,   // stands, guard up, sways — the state it starts in
    FIG_STEP,       // closing on you
    FIG_WINDUP,     // the telegraph: the arm draws back, in the pose
    FIG_STRIKE,     // driving the hand through where you are
    FIG_RECOVER,    // open — the punish window
    FIG_FALLING,    // the legs have gone; on its way down
    FIG_DOWN        // finished
} FigState;

static int s_figExists = 0;
static b3BodyId s_figBone[BONE_COUNT];
static b3JointId s_figJoint[BONE_COUNT];      // the joint to the PARENT
static int s_figAttached[BONE_COUNT];
static int s_figGrid[BONE_COUNT];

// How slack each bone's spring currently is: 1 the instant it is struck, back
// to 0 as it gathers itself. Applied at the end of w_fig_update, after the pose
// for the state has been set, so being hit wins over holding a guard.
static float s_figSlack[BONE_COUNT];

// What holds it up is the floor. The leg is a column of hard joint
// constraints — spherical and revolute joints constrain translation rigidly,
// only rotation is sprung — standing on a hard contact, with the knee pinned
// against its own lowerAngle of 0.00. That column carries the figure's whole
// weight without ever being asked to. Measured with the lift removed: the hip
// settles a few millimetres low, the sole rests ON the floor instead of
// hovering a centimetre above it, and the ground reaction under the two soles
// goes from 0 N to the figure's weight. The gravity cancel was never
// load-bearing. It was levitation.
//
// The one thing the column cannot do is WALK. figStride is an open-loop gait
// authored against a weightless pelvis; with the body's weight on the legs it
// topples. So the carry stays, for the stride only. A real step needs foot
// placement, which is a build of its own.
//
// s_figAttached[b] means "my joint to my parent is intact". Being PART OF THE
// BODY is the transitive closure of that: a hand whose forearm has come off is
// not part of the body however intact its own wrist is. w_fig_post clears the
// flag exactly one bone deep, and w_fig_apply was reading it as if it went all
// the way down — that one mismatch is both symptoms the owner reported.
static int s_figCarried[BONE_COUNT];
static float s_figCarry = 0.0f;      // 0 planted, 1 mid-stride
static float s_figDt = 1.0f / 72.0f; // w_fig_apply has no dt of its own

// Walk the parent chain and report how many joints apart two bones are, or -1
// if neither is an ancestor of the other. The skeleton is seventeen bones and
// this runs only when a blow has already landed.
static int figChainSteps(int from, int to)
{
    int p = from;
    for (int n = 0; n < BONE_COUNT && p >= 0; n++)
    {
        if (p == to) return n;
        p = BONES[p].parent;
    }
    return -1;
}

static void figSlacken(int gi, float speed)
{
    if (!s_figExists || gi < 0) return;
    int hit = -1;
    for (int b = 0; b < BONE_COUNT; b++)
        if (s_figGrid[b] == gi) { hit = b; break; }
    if (hit < 0) return;                       // not one of its bones

    // A committed blow lands around 9 m/s. Below that it still gives a little,
    // or a jab reads as nothing happening at all.
    float s = speed * (1.0f / 9.0f);
    if (s > 1.0f) s = 1.0f;
    if (s < 0.25f) s = 0.25f;

    for (int b = 0; b < BONE_COUNT; b++)
    {
        // Everything hanging BELOW the struck bone whips, because what was
        // holding it just let go; everything above gives less the further up
        // the chain it sits. Anything on another limb entirely is unmoved.
        int steps = figChainSteps(b, hit);
        if (steps < 0) steps = figChainSteps(hit, b);
        if (steps < 0) continue;
        float f = s;
        for (int n = 0; n < steps; n++) f *= 0.55f;
        if (f > s_figSlack[b]) s_figSlack[b] = f;
    }
}
static b3Quat s_figFrame[BONE_COUNT];         // the joint frame rotation, R
static float s_figStature = 1.75f;
static float s_figHipY = 0.93f;               // rest hip height, world
static FigState s_figState = FIG_WAIT;
static float s_figTimer = 0.0f;
static int s_figArm = 1;                      // which hand is winding up
static int s_figHold = 0;                     // freeze the will, guard up
static float s_figSway = 0.0f;
static float s_figStride = 0.0f;
static b3Vec3 s_figPlayer;                    // where you are, told each frame
static float s_figPlayerEye = 1.62f;
static float s_figStanceX = 0.0f;             // the spot its feet are holding
static float s_figStanceZ = 0.0f;
static float s_figReach = 0.0f;               // momentum landed on you this step
static int s_figBroke = 0;                    // bones lost this step
static float s_figTempo = 1.0f;
static float s_figDbg[8];

// Everything the contact-limited balance remembers between steps. Cleared in
// both entry points, because persistent state cleared in only one of them
// arrives from the last figure — this project has paid for that twice.
static void figBalReset(void);

static void figWorldReset(void)
{
    s_figExists = 0;
    // Slack too, or the next figure is born with the last one's punches still
    // in its springs — it sags on its first frame and arrives already damaged.
    for (int i = 0; i < BONE_COUNT; i++)
    { s_figAttached[i] = 0; s_figGrid[i] = -1; s_figSlack[i] = 0.0f;
      s_figCarried[i] = 0; }
    s_figCarry = 0.0f; s_figDt = 1.0f / 72.0f;
    s_figState = FIG_WAIT;
    s_figHold = 0;
    figBalReset();
}

// --- small quaternion helpers ----------------------------------------------

static b3Quat qAxisAngle(float ax, float ay, float az, float angle)
{
    const float h = 0.5f * angle;
    const float s = __builtin_sinf(h);
    b3Quat q; q.v.x = ax * s; q.v.y = ay * s; q.v.z = az * s; q.s = __builtin_cosf(h);
    return q;
}

static b3Quat qConj(b3Quat q)
{
    b3Quat r; r.v.x = -q.v.x; r.v.y = -q.v.y; r.v.z = -q.v.z; r.s = q.s;
    return r;
}

// The minimal rotation carrying unit vector a onto unit vector b.
static b3Quat qFromTo(b3Vec3 a, b3Vec3 b)
{
    const float d = a.x*b.x + a.y*b.y + a.z*b.z;
    if (d < -0.99999f)
    {
        // Opposed: any perpendicular axis, half a turn. Pick the one furthest
        // from a so the cross product is well conditioned.
        b3Vec3 t = { 1.0f, 0.0f, 0.0f };
        if (a.x < -0.9f || a.x > 0.9f) { t.x = 0.0f; t.y = 1.0f; }
        b3Vec3 ax = { a.y*t.z - a.z*t.y, a.z*t.x - a.x*t.z, a.x*t.y - a.y*t.x };
        const float l = __builtin_sqrtf(ax.x*ax.x + ax.y*ax.y + ax.z*ax.z);
        return qAxisAngle(ax.x/l, ax.y/l, ax.z/l, 3.14159265f);
    }
    b3Quat q;
    q.v.x = a.y*b.z - a.z*b.y;
    q.v.y = a.z*b.x - a.x*b.z;
    q.v.z = a.x*b.y - a.y*b.x;
    q.s = 1.0f + d;
    return b3NormalizeQuat(q);
}

// The frame rotation R for a bone: it carries the frame's +Z onto the
// direction the bone actually extends, so a cone limit is centred on the rest
// pose instead of ninety degrees off it — and a hinge turns about the axis a
// human elbow turns about.
static b3Quat figFrameFor(BoneAxis a)
{
    switch (a)
    {
        case AX_UP:    return qAxisAngle(1.0f, 0.0f, 0.0f, -1.5707963f); // +Z -> +Y
        case AX_DOWN:  return qAxisAngle(1.0f, 0.0f, 0.0f,  1.5707963f); // +Z -> -Y
        case AX_HINGE: return qAxisAngle(0.0f, 1.0f, 0.0f,  1.5707963f); // +Z -> +X
        default:       return b3Quat_identity;                           // +Z as is
    }
}

// Drive a ball joint to a pose expressed the way a person would say it: the
// child's rotation RELATIVE TO ITS PARENT. The joint wants that same rotation
// expressed in its own frame, which is conj(R) * qRel * R.
static void figAim(int bone, b3Quat qRel)
{
    if (!s_figAttached[bone] || BONES[bone].parent < 0) return;
    if (BONES[bone].axis == AX_HINGE) return;
    const b3Quat R = s_figFrame[bone];
    b3SphericalJoint_SetTargetRotation(s_figJoint[bone],
        b3MulQuat(qConj(R), b3MulQuat(qRel, R)));
}

// Point a limb bone somewhere. `dir` is in the PARENT's frame; the bone's own
// rest direction is straight down its -Y.
static void figPoint(int bone, float dx, float dy, float dz)
{
    const float l = __builtin_sqrtf(dx*dx + dy*dy + dz*dz);
    if (l < 1e-5f) return;
    b3Vec3 down = { 0.0f, -1.0f, 0.0f };
    b3Vec3 d = { dx/l, dy/l, dz/l };
    figAim(bone, qFromTo(down, d));
}

static void figBend(int bone, float radians)
{
    if (!s_figAttached[bone] || BONES[bone].axis != AX_HINGE) return;
    b3RevoluteJoint_SetTargetAngle(s_figJoint[bone], radians);
}

static void figShoulderHertz(int bone, float hz)
{
    if (!s_figAttached[bone]) return;
    b3SphericalJoint_SetSpringHertz(s_figJoint[bone], hz);
}

// --- building it ------------------------------------------------------------

static int figBroken(int b)
{
    if (!s_figAttached[b]) return 1;
    const int g = s_figGrid[b];
    if (g < 0) return 1;
    const VoxGrid* vg = &s_vxG[g];
    if (!vg->used) return 1;
    const int total = vg->n[0] * vg->n[1] * vg->n[2];
    return vg->alive * FIG_BREAK_DEN <= total * FIG_BREAK_NUM;
}

// An arm throws only while its upper arm AND its forearm hold. A hand is
// worth having and it is not what the blow is made of.
static int figArmGone(int side)
{
    return figBroken(ARM(side)) || figBroken(ARM(side) + 1);
}

// A leg carries only while its thigh AND its shin hold. The foot is balance
// and silhouette; a machine on ankle stumps still stands.
static int figLegCarries(int side)
{
    return !figBroken(LEG(side)) && !figBroken(LEG(side) + 1);
}

static float figSupport(void)
{
    return 0.5f * (float)(figLegCarries(0) + figLegCarries(1));
}

// BONES[] is ordered parent-before-child at every row, so one forward pass
// settles the whole chain: a bone is part of the body only if its own joint
// holds AND everything above it does too.
static void figCarry(void)
{
    for (int b = 0; b < BONE_COUNT; b++)
    {
        const int par = BONES[b].parent;
        s_figCarried[b] = par < 0 ? s_figAttached[b]
                                  : (s_figAttached[b] && s_figCarried[par]);
    }
}

static int figSpineBroken(void)
{
    return figBroken(BONE_ABDOMEN) || figBroken(BONE_CHEST);
}

// --- balance, and where its authority comes from ----------------------------
//
// figSupport() answers "are the legs still attached". That is a STRUCTURAL
// question and it stays exactly as it is — it is what stops the walk, what
// drops the hip to a knee, and what the HUD reads. It is not, and never was,
// an answer to "are the feet on the floor", and using it as one is why the
// figure could be swept off its legs, thrown into the air, or knocked flat and
// still behave as though solidly planted.
//
// Everything below measures the floor instead. The rule the whole thing is
// built on: a standing body balances with ANKLE TORQUE, and ankle torque is
// bounded by the size of the foot and the load on it. Put the centre of
// pressure where the body needs it, clamp it inside the sole, and the
// authority is a measured newton count times a lever that cannot leave the
// footprint. In the air both are zero — not gated to zero, ARE zero — so
// there is nothing left that can right it in mid-air.
//
// Once the capture point (where the body would have to put its foot to stop)
// leaves the area between the soles, no admissible pressure brings it back and
// it MUST topple. That is not a rule bolted on top; it is the same inequality
// the controller is already solving, read out.

#define FIG_LOAD_TAU   0.10f  /* s   sole-load RISE; the fall is instant       */
#define FIG_GND_UP     0.030f /* s   grounded licence, believes contact fast   */
#define FIG_GND_DN     0.120f /* s   ... and forgets it slowly                 */
#define FIG_VEL_TAU    0.06f  /* s   CoM velocity: the body rings at 3.5 Hz,
                                     toppling is 0.45 Hz                       */
#define FIG_ICP_K      1.8f   /* -   capture-point gain; k*w*dt = 0.077        */
#define FIG_COP_INSET  0.012f /* m   how far inside the sole the pressure may
                                     be commanded. Never on the boundary: a
                                     foot rolled onto its own edge destroys the
                                     contact that authorises the torque         */
#define FIG_LOAD_MIN   0.06f  /* -   of body weight, for a sole to count as
                                     part of the support polygon                */
#define FIG_MU         0.7f   /* -   the floor's friction (see w_reset)         */
#define FIG_HIP_MAX    0.0f   /* N.m the trunk pair. ZERO, and measured: at 60 and
                                     at 130 N.m the figure took two punches
                                     fewer before it went over, not more. It
                                     borrows momentum it then has to pay back,
                                     and this body has to pay it back through a
                                     hip spring that is already carrying the
                                     trunk. Kept, named, and off              */
#define FIG_SHIFT_TAU  0.15f  /* s   weight-split slew; inert while FIG_SPLIT is 0 */
#define FIG_STANCE_TAU 8.00f  /* s   how fast the held spot forgets where it was
                                     and drifts onto the ANKLE LINE. Slow, and
                                     measured: at 0.6 s the spot followed the
                                     body backwards under a punch, so it had no
                                     memory of where it had been standing and
                                     walked 0.1 m back per blow until it went
                                     over. At 8 s a punch exchange keeps its
                                     ground                                     */
#define FIG_CARRY_OUT  0.45f  /* s   carry ramp-out (in stays at 0.20)          */
#define FIG_OUT_BAND   0.040f /* m   capture point outside the base by          */
#define FIG_OUT_HOLD   0.45f  /* s   ... and for this long, before it is a fall */
#define FIG_AIR_HOLD   0.50f  /* s   no sole loaded at all, and not striding    */
#define FIG_DOWN_COS   0.2588f/* -   chest up.y, i.e. 75 degrees over           */
#define FIG_DOWN_HOLD  0.20f  /* s   contiguous                                 */
#define FIG_STEP_HOLD  0.15f  /* s   a third of the way to falling, and it puts
                                     a foot down instead. 0 disables            */
#define FIG_STEP_COOL  0.00f  /* s   refractory between saving steps. ZERO, and
                                     measured: at 1.1 s it could not take the
                                     second step a walk-in needs and went down
                                     after three landed blows                   */
#define FIG_SHEAR      0.5f   /* -   how much of the shear the commanded
                                     pressure implies is pushed through the
                                     soles rather than left to the leg column
                                     to transmit. Half, and measured: at zero
                                     the figure could not hold its spot while
                                     it turned and toppled backwards inside
                                     two seconds; at one it double-counts what
                                     the column already carries              */
#define FIG_SPLIT      0.0f   /* -   how far the leg push may lean onto one
                                     foot. ZERO, and measured: the pair goes in
                                     at the pelvis CENTRE and comes out under
                                     one sole 79 mm off the midline, so loading
                                     the foot the body is falling toward rolls
                                     it further that way. Positive feedback with
                                     542 N behind it                            */
#define FIG_GATE_W     0.20f  /* -   of body weight a sole must carry before its
                                     leg may push at all; 0 disables            */
#define FIG_STAND_OFF  0.70f  /* -   of its own range, where a stride stops     */
#define FIG_WALK_LEAD  0.40f  /* m   how far ahead of the centre of mass the
                                     stride may aim, which is what sets the
                                     speed it settles at                        */
#define FIG_HOLD_SLACK 0.00f  /* -   how much of the hold a blow takes away. ZERO,
                                     and measured: the joint springs already go
                                     slack where the blow lands, and taking the
                                     station-keeping away on top of that cost
                                     the figure its footing rather than its
                                     stiffness                                  */
#define FIG_ARRIVE_E   0.30f  /* m   how far onto its own feet the hand-back pulls */
#define FIG_HOLD_A     3.5f   /* m/s2 how hard it may hold its spot, at full contact */
#define FIG_ANKLE_K    1.0f   /* -   fraction of the sole's full moment the
                                     ankle is allowed to ask for                */

// totalNormalImpulse accumulates the biased solve AND the relax pass of each
// substep, so the raw sum is about twice the impulse the contact really spent.
// That factor is an internals dependency of the pinned Box3D and it has its
// own check in the suite.
#define FIG_IMPULSE_CAL 0.5f

static float s_figLoad[2] = { 0.0f, 0.0f };   // filtered normal load per sole, N
static int   s_figLoaded[2] = { 0, 0 };       // and whether it counts as support
static float s_figGnd = 0.0f;                 // grounded licence, 0..1
static float s_figMassC = 0.0f;               // carried mass
static float s_figComX = 0.0f, s_figComY = 0.0f, s_figComZ = 0.0f;
static float s_figVelX = 0.0f, s_figVelZ = 0.0f;
static float s_figIcpX = 0.0f, s_figIcpZ = 0.0f;
static float s_figMargin = 0.0f;              // + inside the support polygon
static float s_figCopX = 0.0f, s_figCopZ = 0.0f;   // commanded pressure
static float s_figWantX = 0.0f, s_figWantZ = 0.0f; // ... before it was clamped
static float s_figTgtX = 0.0f, s_figTgtZ = 0.0f;   // the spot it is holding
static float s_figAnkX[2], s_figAnkZ[2];
static float s_figFootPX[2], s_figFootPZ[2];  // per-sole clamped pressure
static float s_figFootEX[2][2], s_figFootEZ[2][2];
static float s_figSoleLo[2][2], s_figSoleHi[2][2]; // alive footprint, inset
static float s_figSoleCX = 0.0f, s_figSoleCZ = 0.0f;
static float s_figFeetX = 0.0f, s_figFeetZ = 0.0f;  // both ankles, load or no load
static int   s_figNQ = 0;                     // corners in the support polygon
static float s_figOmega = 3.07f;
static float s_figOut = 0.0f;                 // how long past the point of no return
static float s_figAir = 0.0f;                 // how long with nothing under it
static float s_figCollapse = 0.0f;            // how long the trunk has been over
static float s_figShift = 0.5f;               // weight split, left share
static float s_figSlackLeg = 0.0f;            // worst slack in either leg
static float s_figSlackAny = 0.0f;            // and anywhere in the body
static float s_figTauA = 0.0f, s_figTauH = 0.0f;
static int   s_figRecover = 0;                // this step is a step to save itself
static float s_figRecX = 0.0f, s_figRecZ = 0.0f;   // and where it is stepping to
static float s_figStepCool = 0.0f;            // and how long before it may take another

static void figBalReset(void)
{
    s_figLoad[0] = s_figLoad[1] = 0.0f;
    s_figLoaded[0] = s_figLoaded[1] = 0;
    s_figGnd = 0.0f; s_figMassC = 0.0f;
    s_figComX = s_figComY = s_figComZ = 0.0f;
    s_figVelX = s_figVelZ = 0.0f;
    s_figIcpX = s_figIcpZ = 0.0f;
    s_figMargin = 0.0f;
    s_figCopX = s_figCopZ = 0.0f;
    s_figWantX = s_figWantZ = 0.0f;
    s_figTgtX = s_figTgtZ = 0.0f;
    s_figSoleCX = s_figSoleCZ = 0.0f;
    s_figFeetX = s_figFeetZ = 0.0f;
    s_figNQ = 0; s_figOmega = 3.07f;
    s_figOut = s_figAir = s_figCollapse = 0.0f;
    s_figShift = 0.5f; s_figSlackLeg = 0.0f; s_figSlackAny = 0.0f;
    s_figTauA = s_figTauH = 0.0f;
    s_figRecover = 0; s_figRecX = 0.0f; s_figRecZ = 0.0f; s_figStepCool = 0.0f;
    for (int s = 0; s < 2; s++)
    {
        s_figAnkX[s] = s_figAnkZ[s] = 0.0f;
        s_figFootPX[s] = s_figFootPZ[s] = 0.0f;
        s_figFootEX[s][0] = 1.0f; s_figFootEX[s][1] = 0.0f;
        s_figFootEZ[s][0] = 0.0f; s_figFootEZ[s][1] = 1.0f;
        s_figSoleLo[s][0] = s_figSoleHi[s][0] = 0.0f;
        s_figSoleLo[s][1] = s_figSoleHi[s][1] = 0.0f;
    }
}

// Normal load through one bone's contacts with whatever it is standing on, and
// where that load acts. Factored out of w_fig_ground so the controller reads
// exactly the number the suite reads — not a second opinion of it.
static int figPartLoad(int bone, float* N, float* cx, float* cy, float* cz)
{
    *N = 0.0f; *cx = 0.0f; *cy = 0.0f; *cz = 0.0f;
    if (!s_figExists || bone < 0 || bone >= BONE_COUNT) return 0;
    if (!s_figCarried[bone]) return 0;
    const b3BodyId body = s_figBone[bone];

    int cap = b3Body_GetContactCapacity(body);
    if (cap <= 0) return 0;
    if (cap > 24) cap = 24;
    b3ContactData cd[24];
    const int n = b3Body_GetContactData(body, cd, cap);
    if (n <= 0) return 0;

    const b3MassData md = b3Body_GetMassData(body);
    const b3Pos com = b3Body_GetWorldPoint(body, md.center);

    float wsum = 0.0f, gsum = 0.0f, sx = 0.0f, sy = 0.0f, sz = 0.0f;
    for (int i = 0; i < n; i++)
    {
        // The manifold normal points from shape A to shape B, so the push
        // ON US flips sign if we are A.
        const int weAreA = B3_ID_EQUALS(b3Shape_GetBody(cd[i].shapeIdA), body);
        for (int mi = 0; mi < cd[i].manifoldCount; mi++)
        {
            const b3Manifold* mf = &cd[i].manifolds[mi];
            const float uy = weAreA ? -mf->normal.y : mf->normal.y;
            if (uy < 0.5f) continue;          // not something we stand on
            for (int k = 0; k < mf->pointCount; k++)
            {
                const b3ManifoldPoint* mp = &mf->points[k];
                if (mp->separation > 0.005f) continue;   // speculative
                const float J = mp->totalNormalImpulse;
                if (J <= 0.0f) continue;
                // anchorA/anchorB are offsets from that body's centre of mass,
                // already in world axes.
                const b3Vec3 a = weAreA ? mp->anchorA : mp->anchorB;
                sx += ((float)com.x + a.x) * J;
                sy += ((float)com.y + a.y) * J;
                sz += ((float)com.z + a.z) * J;
                wsum += J;
                gsum += J * uy;
            }
        }
    }
    if (wsum <= 1e-6f) return 0;
    *N = FIG_IMPULSE_CAL * gsum / s_figDt;
    *cx = sx / wsum; *cy = sy / wsum; *cz = sz / wsum;
    return 1;
}

// How far inside the convex hull of `n` points a query point sits. Positive
// inside, negative outside and equal to the real distance to the boundary.
// n is at most ten, so gift-wrapping is cheaper than being clever.
static float figPolyMargin(const float* px, const float* pz, int n,
                           float qx, float qz)
{
    if (n <= 0) return -1e9f;

    float best = 1e9f;
    for (int i = 0; i < n; i++)
    {
        const float dx = qx - px[i], dz = qz - pz[i];
        const float d = __builtin_sqrtf(dx*dx + dz*dz);
        if (d < best) best = d;
    }
    if (n < 3) return -best;

    int hull[16], hn = 0, start = 0;
    for (int i = 1; i < n; i++)
        if (px[i] < px[start] || (px[i] == px[start] && pz[i] < pz[start])) start = i;
    int cur = start;
    for (int guard = 0; guard < n + 2 && hn < 15; guard++)
    {
        hull[hn++] = cur;
        int next = (cur + 1) % n;
        for (int i = 0; i < n; i++)
        {
            if (i == cur || i == next) continue;
            const float ax = px[next] - px[cur], az = pz[next] - pz[cur];
            const float bx = px[i] - px[cur], bz = pz[i] - pz[cur];
            const float cr = ax * bz - az * bx;
            if (cr < -1e-9f) next = i;
            else if (cr < 1e-9f && bx*bx + bz*bz > ax*ax + az*az) next = i;
        }
        cur = next;
        if (cur == start) break;
    }
    if (hn < 3) return -best;

    float area2 = 0.0f;
    for (int i = 0; i < hn; i++)
    {
        const int j = (i + 1) % hn;
        area2 += px[hull[i]] * pz[hull[j]] - px[hull[j]] * pz[hull[i]];
    }
    const float sgn = area2 >= 0.0f ? 1.0f : -1.0f;

    float minSigned = 1e9f, minDist = 1e9f;
    for (int i = 0; i < hn; i++)
    {
        const int j = (i + 1) % hn;
        const float ax = px[hull[i]], az = pz[hull[i]];
        const float ex = px[hull[j]] - ax, ez = pz[hull[j]] - az;
        const float len2 = ex*ex + ez*ez;
        if (len2 < 1e-12f) continue;
        const float len = __builtin_sqrtf(len2);
        const float sd = (ex * (qz - az) - ez * (qx - ax)) * sgn / len;
        if (sd < minSigned) minSigned = sd;
        float t = ((qx - ax) * ex + (qz - az) * ez) / len2;
        if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
        const float dx = qx - (ax + t * ex), dz = qz - (az + t * ez);
        const float d = __builtin_sqrtf(dx*dx + dz*dz);
        if (d < minDist) minDist = d;
    }
    return minSigned >= 0.0f ? minSigned : -minDist;
}

// Read the floor, once a step, before anything decides anything. Everything
// after this — the will, the forces, whether it is going down — sees one
// consistent picture of what is under it.
static void figSense(float dt)
{
    if (!s_figExists) return;
    const float floorY = s_worldFloorY;

    // --- the body that is actually still a body ---
    float M = 0.0f, cx = 0.0f, cy = 0.0f, cz = 0.0f, vx = 0.0f, vz = 0.0f;
    for (int b = 0; b < BONE_COUNT; b++)
    {
        if (!s_figCarried[b]) continue;
        const float mb = b3Body_GetMass(s_figBone[b]);
        const b3Pos wc = b3Body_GetWorldCenter(s_figBone[b]);
        const b3Vec3 vb = b3Body_GetLinearVelocity(s_figBone[b]);
        M += mb;
        cx += mb * (float)wc.x; cy += mb * (float)wc.y; cz += mb * (float)wc.z;
        vx += mb * vb.x;        vz += mb * vb.z;
    }
    if (M <= 1e-6f) { s_figMassC = 0.0f; s_figNQ = 0; return; }
    cx /= M; cy /= M; cz /= M; vx /= M; vz /= M;
    s_figMassC = M;
    s_figComX = cx; s_figComY = cy; s_figComZ = cz;
    const float av = dt / (dt + FIG_VEL_TAU);
    s_figVelX += (vx - s_figVelX) * av;
    s_figVelZ += (vz - s_figVelZ) * av;
    const float W = M * 9.81f;

    // --- what each sole and each shin is carrying ---
    //
    // The rise is filtered and the FALL IS NOT. A foot that has left the floor
    // has left it this frame, and sixty newton-metres still going into a one
    // kilogram foot a tenth of a second later is ten thousand radians per
    // second squared of nothing useful.
    float rawFoot[2], rawShin[2], scx[2], scz[2];
    int hasShin[2];
    for (int s = 0; s < 2; s++)
    {
        float N, ax, ay, az;
        rawFoot[s] = figPartLoad(LEG(s) + 2, &N, &ax, &ay, &az) ? N : 0.0f;
        hasShin[s] = figPartLoad(LEG(s) + 1, &N, &ax, &ay, &az);
        rawShin[s] = hasShin[s] ? N : 0.0f;
        scx[s] = ax; scz[s] = az;
        if (rawFoot[s] < s_figLoad[s]) s_figLoad[s] = rawFoot[s];
        else s_figLoad[s] += (rawFoot[s] - s_figLoad[s]) * (dt / (dt + FIG_LOAD_TAU));
    }

    // The grounded licence is the opposite filter, and deliberately: it
    // BELIEVES contact fast and forgets it slowly, so two frames of contact
    // dropout cannot read as flight. It scales caps, never magnitudes.
    // The shins count. Without them a figure down on one knee reads as having
    // left the ground entirely.
    float gRaw = (rawFoot[0] + rawFoot[1] + rawShin[0] + rawShin[1]) /
                 (W > 1e-3f ? W : 1.0f);
    if (gRaw > 1.0f) gRaw = 1.0f;
    const float gTau = gRaw > s_figGnd ? FIG_GND_UP : FIG_GND_DN;
    s_figGnd += (gRaw - s_figGnd) * (dt / (dt + gTau));

    // --- the support polygon, from the ALIVE footprints ---
    //
    // A foot beaten down to two cells has two cells' worth of balance. The
    // rectangle is read out of the grid the collision shapes are made of, so
    // it cannot drift from the bone.
    float qx[12], qz[12];
    int nq = 0;
    for (int s = 0; s < 2; s++)
    {
        s_figLoaded[s] = 0;
        const int ft = LEG(s) + 2;
        s_figAnkX[s] = cx; s_figAnkZ[s] = cz;
        s_figFootEX[s][0] = 1.0f; s_figFootEX[s][1] = 0.0f;
        s_figFootEZ[s][0] = 0.0f; s_figFootEZ[s][1] = 1.0f;
        s_figSoleLo[s][0] = s_figSoleHi[s][0] = 0.0f;
        s_figSoleLo[s][1] = s_figSoleHi[s][1] = 0.0f;
        if (!s_figCarried[ft]) continue;

        const b3Pos ap = b3Body_GetPosition(s_figBone[ft]);   // the ankle IS the origin
        s_figAnkX[s] = (float)ap.x; s_figAnkZ[s] = (float)ap.z;
        b3Vec3 fwd = { 0.0f, 0.0f, 1.0f };
        fwd = b3Body_GetWorldVector(s_figBone[ft], fwd);
        float fl = __builtin_sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
        float ezx = 0.0f, ezz = 1.0f;
        if (fl > 1e-4f) { ezx = fwd.x / fl; ezz = fwd.z / fl; }
        const float exx = ezz, exz = -ezx;
        s_figFootEX[s][0] = exx; s_figFootEX[s][1] = exz;
        s_figFootEZ[s][0] = ezx; s_figFootEZ[s][1] = ezz;

        const int g = s_figGrid[ft];
        if (g < 0 || g >= VOX_GRIDS || !s_vxG[g].used) continue;
        const VoxGrid* vg = &s_vxG[g];
        int i0 = 99, i1 = -1, k0 = 99, k1 = -1;
        for (int ix = 0; ix < vg->n[0]; ix++)
            for (int iz = 0; iz < vg->n[2]; iz++)
            {
                int any = 0;
                for (int iy = 0; iy < vg->n[1]; iy++)
                    if (vxAliveAt(vg, ix, iy, iz)) { any = 1; break; }
                if (!any) continue;
                if (ix < i0) i0 = ix;
                if (ix > i1) i1 = ix;
                if (iz < k0) k0 = iz;
                if (iz > k1) k1 = iz;
            }
        if (i1 < 0) continue;
        float ulo = vg->local.x + (float)i0 * vg->size;
        float uhi = vg->local.x + (float)(i1 + 1) * vg->size;
        float wlo = vg->local.z + (float)k0 * vg->size;
        float whi = vg->local.z + (float)(k1 + 1) * vg->size;
        const float um = 0.5f * (ulo + uhi), wm = 0.5f * (wlo + whi);
        ulo += FIG_COP_INSET; uhi -= FIG_COP_INSET;
        wlo += FIG_COP_INSET; whi -= FIG_COP_INSET;
        if (ulo > uhi) { ulo = um; uhi = um; }
        if (wlo > whi) { wlo = wm; whi = wm; }
        s_figSoleLo[s][0] = ulo; s_figSoleHi[s][0] = uhi;
        s_figSoleLo[s][1] = wlo; s_figSoleHi[s][1] = whi;

        if (s_figLoad[s] < FIG_LOAD_MIN * W) continue;
        s_figLoaded[s] = 1;
        for (int a = 0; a < 2; a++)
            for (int c = 0; c < 2; c++)
            {
                const float u = a ? uhi : ulo, wv = c ? whi : wlo;
                qx[nq] = s_figAnkX[s] + u * exx + wv * ezx;
                qz[nq] = s_figAnkZ[s] + u * exz + wv * ezz;
                nq++;
            }
    }
    for (int s = 0; s < 2; s++)
        if (hasShin[s] && rawShin[s] >= FIG_LOAD_MIN * W && nq < 12)
        { qx[nq] = scx[s]; qz[nq] = scz[s]; nq++; }
    s_figNQ = nq;

    // --- the capture point: where it would have to put a foot to stop ---
    float Hc = cy - floorY;
    if (Hc < 0.30f) Hc = 0.30f;
    s_figOmega = __builtin_sqrtf(9.81f / Hc);
    s_figIcpX = cx + s_figVelX / s_figOmega;
    s_figIcpZ = cz + s_figVelZ / s_figOmega;
    s_figMargin = figPolyMargin(qx, qz, nq, s_figIcpX, s_figIcpZ);

    // Where the loaded ANKLES are: the pressure point that costs no ankle
    // moment at all, and therefore the spot this leg column actually rests
    // over. Not the centroid of the soles — that sits 66 mm in front of the
    // ankle line, and holding the body over it means commanding a heel-ward
    // pressure for ever, which measured as a permanent saturated ankle and a
    // slow topple backwards.
    {
        float ax = 0.0f, az = 0.0f, wsum = 0.0f;
        for (int s = 0; s < 2; s++)
        {
            if (!s_figLoaded[s]) continue;
            ax += s_figLoad[s] * s_figAnkX[s];
            az += s_figLoad[s] * s_figAnkZ[s];
            wsum += s_figLoad[s];
        }
        if (wsum > 1e-6f) { s_figSoleCX = ax / wsum; s_figSoleCZ = az / wsum; }
        else { s_figSoleCX = cx; s_figSoleCZ = cz; }
        // And where the feet are whether they are carrying or not. A stride
        // spends a third of itself with a foot in the air, and the walk has to
        // be steered by where the feet ARE — the body leads them by a third of
        // a metre, and a walk aimed with the body stops the FEET a third of a
        // metre past where it meant to.
        int nf = 0; float fx = 0.0f, fz = 0.0f;
        for (int s = 0; s < 2; s++)
        {
            if (!s_figCarried[LEG(s) + 2]) continue;
            fx += s_figAnkX[s]; fz += s_figAnkZ[s]; nf++;
        }
        if (nf > 0) { s_figFeetX = fx / (float)nf; s_figFeetZ = fz / (float)nf; }
        else { s_figFeetX = cx; s_figFeetZ = cz; }
    }
    if (s_figCarry < 0.05f && (s_figLoaded[0] || s_figLoaded[1]))
    {
        const float a = dt / (dt + FIG_STANCE_TAU);
        s_figStanceX += (s_figSoleCX - s_figStanceX) * a;
        s_figStanceZ += (s_figSoleCZ - s_figStanceZ) * a;
    }

    // On top of the spot rides the sway a boxer has — weight moving between
    // the feet, across the line to the player. Two centimetres: a shift of
    // weight, not a step. It used to be a wobble in a position spring; it is
    // now a wobble in where the pressure is put, which is what it always was.
    float tx = s_figStanceX, tz = s_figStanceZ;
    {
        const float bx = s_figPlayer.x - cx, bz = s_figPlayer.z - cz;
        const float bl = __builtin_sqrtf(bx*bx + bz*bz);
        if (bl > 1e-3f)
        {
            const float shift = __builtin_sinf(s_figSway) * 0.02f;
            tx += -bz / bl * shift; tz += bx / bl * shift;
        }
    }
    s_figTgtX = tx; s_figTgtZ = tz;

    // --- the law ---
    //
    // p* = icp + k(icp - target). Closed on the capture point this is FIRST
    // ORDER — icp' = -w k (icp - target), 0.181 s — so there is no spring to
    // ring and no second-order stability condition to satisfy. Each foot then
    // offers only what is inside its own sole. The clamp is PER FOOT and that
    // is a stability requirement, not a refinement: commanded between the two
    // feet, one sole was asked for 163 N.m when it could absorb 18, and it
    // went 0.39 m into the air in four frames.
    s_figWantX = s_figIcpX + FIG_ICP_K * (s_figIcpX - tx);
    s_figWantZ = s_figIcpZ + FIG_ICP_K * (s_figIcpZ - tz);

    // How the two feet share it. The net pressure the body needs is NOT put
    // under each sole separately: a body standing with its weight between its
    // feet has each foot's pressure under its own ankle and the two AVERAGE to
    // the midline, with no ankle moment anywhere. Projecting the net point
    // onto both soles instead asks each foot for its full inward moment for
    // ever — measured as 20 N.m per ankle of permanent inward roll, both feet
    // up on their inner edges, and a topple as soon as anything else happened.
    //
    // So what each ankle is asked for is the SHIFT: how far the pressure has
    // to move from the ankle line, applied about each of its own ankles and
    // clamped to its own sole.
    const float shX = s_figWantX - s_figSoleCX;
    const float shZ = s_figWantZ - s_figSoleCZ;
    float bxs = 0.0f, bzs = 0.0f, lsum = 0.0f;
    for (int s = 0; s < 2; s++)
    {
        s_figFootPX[s] = s_figAnkX[s]; s_figFootPZ[s] = s_figAnkZ[s];
        if (!s_figLoaded[s]) continue;
        const float exx = s_figFootEX[s][0], exz = s_figFootEX[s][1];
        const float ezx = s_figFootEZ[s][0], ezz = s_figFootEZ[s][1];
        const float dx = shX, dz = shZ;
        float du = dx * exx + dz * exz;
        float dw = dx * ezx + dz * ezz;
        if (du < s_figSoleLo[s][0]) du = s_figSoleLo[s][0];
        if (du > s_figSoleHi[s][0]) du = s_figSoleHi[s][0];
        if (dw < s_figSoleLo[s][1]) dw = s_figSoleLo[s][1];
        if (dw > s_figSoleHi[s][1]) dw = s_figSoleHi[s][1];
        s_figFootPX[s] = s_figAnkX[s] + du * exx + dw * ezx;
        s_figFootPZ[s] = s_figAnkZ[s] + du * exz + dw * ezz;
        bxs += s_figLoad[s] * s_figFootPX[s];
        bzs += s_figLoad[s] * s_figFootPZ[s];
        lsum += s_figLoad[s];
    }
    if (lsum > 1e-6f) { s_figCopX = bxs / lsum; s_figCopZ = bzs / lsum; }
    else { s_figCopX = cx; s_figCopZ = cz; }

    // --- going down is a consequence, not a script ---
    //
    // Two doors, because the physics does the toppling either way and the
    // trigger only NAMES the state.
    s_figSlackLeg = 0.0f;
    for (int s = 0; s < 2; s++)
        for (int k = 0; k < 3; k++)
            if (s_figSlack[LEG(s) + k] > s_figSlackLeg) s_figSlackLeg = s_figSlack[LEG(s) + k];
    s_figSlackAny = 0.0f;
    for (int b = 0; b < BONE_COUNT; b++)
        if (s_figCarried[b] && s_figSlack[b] > s_figSlackAny) s_figSlackAny = s_figSlack[b];

    // The capture point has left the base and stayed out. The band, the dwell
    // and both suppressions are measured: the command is a few millimetres
    // noisy so a zero crossing chatters; 35% of stride frames have no sole
    // loaded, because walking IS controlled falling; and being HIT is not the
    // same as being off balance, which is what the slack term says.
    // Asked only while both legs still carry, for the same reason the backstop
    // is: down on one knee the body is permanently outside what one sole can
    // hold, and that is a distinct state this game already has — not a fall.
    if (nq > 0 && figSupport() >= 1.0f && s_figMargin < -FIG_OUT_BAND
        && s_figCarry < 0.05f && s_figSlackLeg < 0.20f)
        s_figOut += dt;
    else if (s_figMargin > 0.005f || s_figCarry >= 0.05f || s_figSlackLeg >= 0.20f)
    { s_figOut -= dt * 2.0f; if (s_figOut < 0.0f) s_figOut = 0.0f; }

    if (nq == 0 && s_figCarry < 0.05f) s_figAir += dt; else s_figAir = 0.0f;

    // The backstop: the trunk has gone over 75 degrees and stayed there.
    // Contiguous, reset rather than decayed. Asked only while both legs still
    // carry, because kneeling on one reads 3.7 s past 75 and is not a fall.
    if (figSupport() >= 1.0f && s_figCarried[BONE_CHEST])
    {
        b3Matrix3 cm = b3MakeMatrixFromQuat(b3Body_GetRotation(s_figBone[BONE_CHEST]));
        if (cm.cy.y < FIG_DOWN_COS) s_figCollapse += dt; else s_figCollapse = 0.0f;
    }
    else s_figCollapse = 0.0f;
}

WASM_EXPORT("w_fig_destroy")
void w_fig_destroy(void)
{
    if (!s_figExists) return;
    for (int b = 0; b < BONE_COUNT; b++)
    {
        const int g = s_figGrid[b];
        if (g >= 0 && g < VOX_GRIDS) s_vxG[g].used = 0;
        // Destroying a body destroys its shapes and every joint on it, so the
        // joints must not be destroyed separately here.
        if (s_figAttached[b] || s_figGrid[b] >= 0) b3DestroyBody(s_figBone[b]);
        s_figAttached[b] = 0;
        s_figGrid[b] = -1;
    }
    s_figExists = 0;
    s_figState = FIG_WAIT;
}

// Build the figure standing at (x, z), `stature` metres tall, out of
// `material`. Every dimension follows from the stature, so the opponent is
// sized against the person in the headset rather than against a constant.
WASM_EXPORT("w_fig_create")
int w_fig_create(float x, float z, float stature, int material)
{
    if (s_figExists) w_fig_destroy();
    // The last one was probably beaten to death, which leaves its springs
    // slack. Carried into this one it is born sagging and hits the floor on
    // its first frame — measured as a fresh figure arriving 85/86 cells.
    for (int i = 0; i < BONE_COUNT; i++) s_figSlack[i] = 0.0f;
    const float H = stature > 1.0f && stature < 2.4f ? stature : 1.75f;
    s_figStature = H;
    s_figHipY = s_worldFloorY + BONES[BONE_PELVIS].oy * H;

    // Every body is created at identity rotation, in the rest pose. That is
    // what makes an identity joint target mean "stand".
    for (int b = 0; b < BONE_COUNT; b++)
    {
        const BoneDef* d = &BONES[b];
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position.x = x + d->ox * H;
        bd.position.y = s_worldFloorY + d->oy * H;
        bd.position.z = z + d->oz * H;
        bd.angularDamping = 1.4f;
        bd.linearDamping = 0.15f;
        s_figBone[b] = b3CreateBody(s_world, &bd);
        s_figAttached[b] = 1;
        s_figGrid[b] = -1;
    }
    // The pelvis carries the whole body on a spring; it must not be spun up
    // by every limb that swings off it.
    b3Body_SetAngularDamping(s_figBone[BONE_PELVIS], 2.5f);

    // The bone itself: a voxel grid in the body's own frame. Its cells ARE
    // the collision shapes, so as the bone is battered away it genuinely gets
    // less there — no separate hull quietly keeping the old silhouette.
    for (int b = 0; b < BONE_COUNT; b++)
    {
        const BoneDef* d = &BONES[b];
        float hx = d->hx * H, hy = d->hy * H, hz = d->hz * H;
        float longest = hx > hy ? hx : hy; if (hz > longest) longest = hz;
        const float cell = 2.0f * longest / (float)d->cells;
        int nx = (int)(2.0f * hx / cell + 0.5f); if (nx < 1) nx = 1;
        int ny = (int)(2.0f * hy / cell + 0.5f); if (ny < 1) ny = 1;
        int nz = (int)(2.0f * hz / cell + 0.5f); if (nz < 1) nz = 1;

        // Where the box sits relative to the bone's origin, which is its
        // joint: a limb hangs BELOW its joint, the spine stands above it, a
        // foot reaches forward of the ankle.
        float cx = 0.0f, cy = 0.0f, cz = 0.0f;
        switch (d->axis)
        {
            case AX_UP:                    cy =  0.5f * ny * cell; break;
            case AX_FWD:  cy = -0.5f * ny * cell; cz = 0.5f * nz * cell - 0.25f * nz * cell; break;
            default:                       cy = -0.5f * ny * cell; break;   // hangs
        }
        b3Vec3 local = { cx - 0.5f * nx * cell, cy - 0.5f * ny * cell,
                         cz - 0.5f * nz * cell };
        s_figGrid[b] = vxBuild(s_figBone[b], local, nx, ny, nz, cell,
                               material, 1, FIG_CATEGORY);
        vxMakePart(s_figGrid[b], d->hpMul);
        // Trunk heavy, limbs light — the mass distribution of a body. It is
        // also what lets the pelvis be the root: a root that weighs a tenth of
        // what it is steering gets steered instead.
        vxPartDensity(s_figGrid[b], d->dens);
        // The page draws every bone as a mesh that craters where it is hit.
        // The engine's box renderer would draw the same cells as boxes.
        w_vox_hide(s_figGrid[b], 1);
    }

    // The joints. Frame A sits on the parent at the joint; frame B is the
    // child's own origin, which IS the joint — so at rest the two frames
    // coincide and the relative rotation is identity.
    for (int b = 0; b < BONE_COUNT; b++)
    {
        const BoneDef* d = &BONES[b];
        if (d->parent < 0) continue;
        const BoneDef* p = &BONES[d->parent];
        const b3Quat R = figFrameFor(d->axis);
        s_figFrame[b] = R;

        b3Transform fa = b3Transform_identity;
        fa.p.x = (d->ox - p->ox) * H;
        fa.p.y = (d->oy - p->oy) * H;
        fa.p.z = (d->oz - p->oz) * H;
        fa.q = R;
        b3Transform fb = b3Transform_identity;
        fb.q = R;

        if (d->axis == AX_HINGE)
        {
            b3RevoluteJointDef rj = b3DefaultRevoluteJointDef();
            rj.base.bodyIdA = s_figBone[d->parent];
            rj.base.bodyIdB = s_figBone[b];
            rj.base.localFrameA = fa;
            rj.base.localFrameB = fb;
            rj.base.collideConnected = false;
            rj.enableLimit = true;
            rj.lowerAngle = d->cone;      // an elbow's range, or a knee's
            rj.upperAngle = d->hi;
            rj.enableSpring = true;
            rj.hertz = d->hertz;
            rj.dampingRatio = 0.9f;
            rj.targetAngle = 0.0f;
            s_figJoint[b] = b3CreateRevoluteJoint(s_world, &rj);
        }
        else
        {
            b3SphericalJointDef sj = b3DefaultSphericalJointDef();
            sj.base.bodyIdA = s_figBone[d->parent];
            sj.base.bodyIdB = s_figBone[b];
            sj.base.localFrameA = fa;
            sj.base.localFrameB = fb;
            sj.base.collideConnected = false;
            sj.enableConeLimit = true;
            // Box3D asserts a quarter turn is the maximum, and the release
            // build compiles asserts out — an over-wide cone silently goes
            // degenerate rather than failing loudly.
            sj.coneAngle = d->cone > 1.5f ? 1.5f : d->cone;
            sj.enableSpring = true;
            sj.hertz = d->hertz;
            sj.dampingRatio = 0.9f;
            sj.targetRotation = b3Quat_identity;
            s_figJoint[b] = b3CreateSphericalJoint(s_world, &sj);
        }
    }

    s_figState = FIG_WAIT;
    s_figTimer = 0.0f;
    s_figSway = 0.0f;
    s_figStride = 0.0f;
    s_figArm = 1;
    s_figReach = 0.0f;
    s_figBroke = 0;
    s_figHold = 0;
    s_figStanceX = x; s_figStanceZ = z;
    s_figPlayer.x = x; s_figPlayer.y = s_worldFloorY + 1.5f; s_figPlayer.z = z + 1.5f;
    // Same reason the slack is cleared above: persistent state cleared in only
    // one of the two entry points arrives from the last figure.
    s_figCarry = 0.0f; s_figDt = 1.0f / 72.0f;
    figBalReset();
    s_figTgtX = x; s_figTgtZ = z;
    s_figSoleCX = x; s_figSoleCZ = z;
    figCarry();                            // so frame 1 is already correct
    s_figExists = 1;
    return BONE_COUNT;
}

WASM_EXPORT("w_fig_bone_count")
int w_fig_bone_count(void) { return BONE_COUNT; }

WASM_EXPORT("w_fig_bone_grid")
int w_fig_bone_grid(int b)
{
    if (b < 0 || b >= BONE_COUNT) return -1;
    return s_figGrid[b];
}

// Freeze its will with its guard up — what it does before a fight starts.
WASM_EXPORT("w_fig_hold")
void w_fig_hold(int on) { s_figHold = on ? 1 : 0; }

// How fast its decisions run. 1.0 is a person; below that it is reading you,
// above it is coming forward.
WASM_EXPORT("w_fig_tempo")
void w_fig_tempo(float t)
{
    s_figTempo = t < 0.3f ? 0.3f : (t > 2.5f ? 2.5f : t);
}

// --- posing it --------------------------------------------------------------

// The guard: hands up in front of the face line, elbows folded, weight even.
// Everything is expressed in the parent's frame, so this is readable as a
// description of a stance rather than as a list of quaternions.
static void figGuard(float sway)
{
    for (int s = 0; s < 2; s++)
    {
        const float side = s == 0 ? -1.0f : 1.0f;
        // Upper arm down and slightly forward, tucked in to the ribs.
        figPoint(ARM(s), side * 0.28f, -0.90f, 0.34f + sway * 0.10f);
        // Elbow folded up so the hand comes to the cheek. Negative is forward.
        figBend(ARM(s) + 1, -2.05f);
        figPoint(ARM(s) + 2, 0.0f, -1.0f, 0.0f);
        figShoulderHertz(ARM(s), 20.0f);
    }
    // The trunk holds. The first cut swayed it by TWISTING the abdomen and
    // chest a few degrees each way, which looked like the right idea and was
    // the reason the figure would not face anybody: the spine springs run at
    // 28 Hz and the pelvis's own yaw controller at under 2, so the trunk wagged
    // the hips instead of the hips turning the trunk. Measured, it swung
    // through two radians of heading while standing still. A real boxer's
    // sway comes from the feet, so this one does too — see the stance shift
    // in w_fig_apply. What is left here is a small forward lean, which is
    // pitch, and pitch does not fight the heading.
    figAim(BONE_ABDOMEN, qAxisAngle(1.0f, 0.0f, 0.0f, 0.03f));
    figAim(BONE_CHEST, qAxisAngle(1.0f, 0.0f, 0.0f, 0.02f));
    figAim(BONE_NECK, qAxisAngle(1.0f, 0.0f, 0.0f, -0.12f));
    figAim(BONE_HEAD, b3Quat_identity);
}

// A stride. The legs are driven and the pelvis is force-carried, so this is a
// driven gait with physics reaction rather than an emergent walk — the
// honest thing to build, and it is what the hips and knees are FOR here:
// take one out and the gait has nowhere to come from.
// The legs have ONE owner, and this is it — standing and walking are the same
// two hips and two knees, so they cannot be written in two places. The stance
// spent a while in figGuard, which is called first and was overwritten on the
// next line by this function every frame.
static void figStride(float phase, float amount)
{
    for (int s = 0; s < 2; s++)
    {
        const float ph = phase + (s == 0 ? 0.0f : 3.14159265f);
        const float swing = __builtin_sinf(ph) * 0.40f * amount;
        const float lift  = __builtin_sinf(ph - 0.9f);
        const float knee  = (lift > 0.0f ? lift : 0.0f) * 1.10f * amount;
        // Standing, the hips set OUT so the feet are wider than the pelvis.
        // Built dead vertical the two legs are 16 cm apart and 14 cm thick:
        // they touch all the way down and read as one column with feet on the
        // bottom. A render showed that in a second; no measurement here ever
        // would have. It is also just what a fighter's legs do.
        const float out = (s == 0 ? -0.14f : 0.14f) * (1.0f - amount);
        figPoint(LEG(s), out, -1.0f, swing);
        figBend(LEG(s) + 1, knee);
        // The foot stays flat to the floor as the shin swings under it.
        figAim(LEG(s) + 2, b3MulQuat(qAxisAngle(1.0f, 0.0f, 0.0f, -swing + knee),
                                     qAxisAngle(0.0f, 0.0f, 1.0f, -out)));
    }
}

// Where the player's chest is, in the chest bone's own frame — which is what
// an arm needs in order to be aimed at it.
static b3Vec3 figPlayerInChest(void)
{
    b3Vec3 out = { 0.0f, 0.0f, 1.0f };
    if (!s_figAttached[BONE_CHEST]) return out;
    b3Pos c = b3Body_GetPosition(s_figBone[BONE_CHEST]);
    b3Quat q = b3Body_GetRotation(s_figBone[BONE_CHEST]);
    b3Vec3 w = { (float)(s_figPlayer.x - c.x),
                 (float)(s_figPlayer.y - c.y),
                 (float)(s_figPlayer.z - c.z) };
    return b3InvRotateVector(q, w);
}

// --- its will ---------------------------------------------------------------

// Range at which it will throw a hand, as a fraction of its own reach.
#define FIG_RANGE 0.92f

// Told where you are, and how long since last time. Runs the decisions; the
// forces are applied separately, so a page can step physics faster than it
// thinks.
WASM_EXPORT("w_fig_update")
void w_fig_update(float px, float py, float pz, float dt)
{
    if (!s_figExists) return;
    // w_fig_apply is called without a dt of its own; this is where one is told.
    s_figDt = dt > 1e-5f ? dt : 1.0f / 72.0f;
    s_figPlayer.x = px; s_figPlayer.y = py; s_figPlayer.z = pz;
    s_figPlayerEye = py;

    // Read the floor first. The will, the forces and the question of whether
    // it is still standing all see one picture of what is under it.
    figSense(s_figDt);

    const float H = s_figStature;
    // Shoulder to fingertip: how far it can actually reach you.
    const float reach = (BONES[BONE_L_UPPERARM].oy - 0.375f) * H;

    b3Pos hip = b3Body_GetPosition(s_figBone[BONE_PELVIS]);
    const float dx = px - (float)hip.x, dz = pz - (float)hip.z;
    const float dist = __builtin_sqrtf(dx*dx + dz*dz);

    if (s_figStepCool > 0.0f) { s_figStepCool -= dt; }
    s_figSway += dt * 1.7f * s_figTempo;
    if (s_figSway > 6.2831853f) s_figSway -= 6.2831853f;
    s_figTimer += dt * s_figTempo;

    // Standing is not a decision it gets to make. Legs gone or spine gone and
    // it is going down, whatever it was in the middle of — and now also when
    // the body itself has gone past the point where any admissible pressure
    // under its feet could bring it back, when there is nothing under it at
    // all, or when the trunk has gone over and stayed there.
    //
    // None of these is "if pushed hard, fall". Each is a CONSEQUENCE, measured
    // after the physics has already done the toppling; the trigger only names
    // the state so the springs can let go and the will can stop.
    if (s_figState != FIG_DOWN && (figSupport() <= 0.0f || figSpineBroken()
        || s_figOut > FIG_OUT_HOLD || s_figAir > FIG_AIR_HOLD
        || s_figCollapse > FIG_DOWN_HOLD))
    {
        if (s_figState != FIG_FALLING) { s_figState = FIG_FALLING; s_figTimer = 0.0f; }
    }
    // A step you take because you had to, not because you were closing. A body
    // whose pressure has run out of sole has exactly one honest answer left,
    // and it is to put a foot down somewhere else — a spring that hauls it
    // back is the invisible hand again. It abandons a punch to do it, which is
    // what a person does; and it comes a third of the way to the fall, so the
    // step gets a chance before the topple is called. Both legs only, and never
    // while it is already stepping.
    else if (FIG_STEP_HOLD > 0.0f && s_figOut > FIG_STEP_HOLD
             && s_figStepCool <= 0.0f
             && s_figState != FIG_STEP && s_figState != FIG_FALLING
             && s_figState != FIG_DOWN
             && figSupport() >= 1.0f && s_figCarry < 0.05f)
    {
        s_figState = FIG_STEP; s_figTimer = 0.0f; s_figRecover = 1;
        // One step, then stand and let the ankles work. Without the cooldown
        // it re-triggered the frame the last one finished and the figure spent
        // the whole fight stepping instead of fighting — 20 blows landed on it
        // where a fight lands 70.
        s_figStepCool = FIG_STEP_COOL; s_figOut = 0.0f;
        // FROZEN at entry, and no further than a stride is worth. Tracked live
        // the capture point runs away from a body that is walking toward it —
        // it is where you are going, so chasing it is a walk with no end, and
        // it measured as the figure striding five metres out of the fight.
        float rx = s_figIcpX - s_figComX, rz = s_figIcpZ - s_figComZ;
        const float rl = __builtin_sqrtf(rx*rx + rz*rz);
        if (rl > 0.40f) { rx *= 0.40f / rl; rz *= 0.40f / rl; }
        s_figRecX = s_figComX + rx; s_figRecZ = s_figComZ + rz;
    }

    switch (s_figState)
    {
        case FIG_FALLING:
            if (s_figTimer > 1.6f) s_figState = FIG_DOWN;
            break;

        case FIG_DOWN:
            break;

        case FIG_WAIT:
            if (s_figHold) break;
            // A step you take because you had to, not because you were
            // closing. A body whose pressure has run out of sole has exactly
            // one honest answer left and it is to put a foot down somewhere
            // else — a spring that hauls it back is the invisible hand again.
            // Only with both legs, only with a polygon to be outside of, never
            // while already stepping, and never while it is still giving from
            // a blow: being HIT is not the same as being off balance.
            // Hysteresis, or it flaps: step out at range, stop well inside it.
            // Without the gap it spent the whole fight alternating STEP and
            // WAIT every few frames on the boundary, which reads as a twitch
            // rather than as footwork.
            //
            // And it does not START a stride it cannot take. FIG_STEP already
            // aborts on one leg, but only on the frame AFTER it is entered, so
            // a one-legged figure standing out of range entered STEP and left
            // it again on every single frame — measured at 2198 state changes
            // in 2200 frames of the scripted fight. That is not a cosmetic
            // twitch. carryWant is 1 in STEP and 0 in WAIT, and the carry ramps
            // IN over 0.20 s and OUT over 0.45 s, so alternating every frame
            // ratchets it: +0.0069 up, -0.0031 down, saturated at 1.0 inside
            // five seconds. A saturated carry hands a kneeling figure the whole
            // vertical hip PD (`ay` is multiplied by it — "while it is planted
            // ay is exactly zero" stopped being true), the whole stride-arrival
            // authority, and it freezes the held spot, which only tracks the
            // feet while the carry is under 0.05. Traced consequence: the leg
            // came off at 7.2 s with the figure 0.76 m away, three punches slid
            // it to 1.20 m, and the spot it was holding was welded there for
            // the remaining 26 s. It never came back into reach, the second leg
            // was never taken, and it was still up at the end of the fight.
            if (dist > reach * FIG_RANGE && figSupport() >= 1.0f)
            { s_figState = FIG_STEP; s_figTimer = 0.0f; }
            else if (s_figTimer > 0.55f && !(figArmGone(0) && figArmGone(1)))
            {
                // Take both its arms off and it cannot throw anything: it
                // stands and it comes forward, and that is all. Without this
                // guard it ping-ponged WAIT to WINDUP forever, winding up
                // with nothing on the end of either shoulder.
                s_figState = FIG_WINDUP; s_figTimer = 0.0f;
                // Alternate, so it does not become one predictable hand.
                s_figArm = !s_figArm;
            }
            break;

        case FIG_STEP:
            // A recovery step ends when it has its balance back, not when it
            // has arrived somewhere — and it is capped, so a figure that
            // cannot recover falls rather than walking for ever.
            if (s_figRecover)
            {
                if (figSupport() < 1.0f || s_figMargin > 0.02f || s_figTimer > 0.80f)
                { s_figState = FIG_WAIT; s_figTimer = 0.0f; s_figRecover = 0; }
                break;
            }
            if (s_figHold) { s_figState = FIG_WAIT; s_figTimer = 0.0f; break; }
            // It cannot step on one leg. Losing a leg is not a debuff, it is
            // the end of walking.
            if (figSupport() < 1.0f) { s_figState = FIG_WAIT; s_figTimer = 0.0f; break; }
            // Stop stepping well outside the spot it is walking to. With the
            // two set a few centimetres apart it walked to a place that was
            // still "too far", and never came out of STEP at all.
            if (dist <= reach * FIG_RANGE * 0.86f) { s_figState = FIG_WAIT; s_figTimer = 0.0f; }
            break;

        case FIG_WINDUP:
            // If the arm it chose has been taken off it, it does not swing a
            // stump. It picks the other one, or waits.
            if (figArmGone(s_figArm))
            {
                s_figArm = !s_figArm;
                if (figArmGone(s_figArm)) { s_figState = FIG_WAIT; s_figTimer = 0.0f; break; }
            }
            if (s_figTimer > 0.26f) { s_figState = FIG_STRIKE; s_figTimer = 0.0f; }
            break;

        case FIG_STRIKE:
            if (s_figTimer > 0.20f) { s_figState = FIG_RECOVER; s_figTimer = 0.0f; }
            break;

        case FIG_RECOVER:
            // Wide open. This is the window, and it is deliberately long
            // enough to use.
            if (s_figTimer > 0.42f) { s_figState = FIG_WAIT; s_figTimer = 0.0f; }
            break;
    }

    if (s_figState != FIG_STEP) s_figRecover = 0;

    // Pose for the state it is now in.
    const float sway = __builtin_sinf(s_figSway);
    switch (s_figState)
    {
        case FIG_DOWN:
        case FIG_FALLING:
            // Nothing holds a pose on the way down. Every spring goes slack
            // and the body falls the way a body falls — a figure that keeps
            // its guard while collapsing was never really hurt.
            for (int b = 0; b < BONE_COUNT; b++)
            {
                if (!s_figAttached[b] || BONES[b].parent < 0) continue;
                if (BONES[b].axis == AX_HINGE)
                    b3RevoluteJoint_SetSpringHertz(s_figJoint[b], 0.6f);
                else
                    b3SphericalJoint_SetSpringHertz(s_figJoint[b], 0.6f);
            }
            break;

        case FIG_STEP:
            s_figStride += dt * 5.0f * s_figTempo;
            figGuard(sway * 0.4f);
            figStride(s_figStride, 1.0f);
            break;

        case FIG_WINDUP:
        {
            figGuard(sway * 0.3f);
            figStride(s_figStride, 0.0f);
            const int a = ARM(s_figArm);
            const float side = s_figArm == 0 ? -1.0f : 1.0f;
            // Drawn back and out. The pose IS the telegraph — there is no
            // separate tell to read, you read the arm.
            figPoint(a, side * 0.55f, -0.55f, -0.62f);
            figBend(a + 1, -2.35f);
            figShoulderHertz(a, 17.0f);
            break;
        }

        case FIG_STRIKE:
        {
            figGuard(0.0f);
            figStride(s_figStride, 0.0f);
            const int a = ARM(s_figArm);
            const b3Vec3 t = figPlayerInChest();
            figPoint(a, t.x, t.y, t.z);
            figBend(a + 1, -0.15f);          // the elbow opens through the blow
            figPoint(a + 2, t.x, t.y, t.z);
            figShoulderHertz(a, 30.0f);      // this is the punch
            break;
        }

        case FIG_RECOVER:
        {
            const int a = ARM(s_figArm);
            const float side = s_figArm == 0 ? -1.0f : 1.0f;
            figGuard(sway * 0.2f);
            figStride(s_figStride, 0.0f);
            // The spent arm hangs out and away, which is what leaves the ribs
            // and the head open.
            figPoint(a, side * 0.75f, -0.62f, 0.20f);
            figBend(a + 1, -0.60f);
            figShoulderHertz(a, 6.0f);
            break;
        }

        default:
            figGuard(sway);
            figStride(s_figStride, 0.0f);
            break;
    }

    // Did a hand of its own reach you? There is no body of yours in the
    // solver to hit, so this is measured rather than collided: a hand inside
    // a head-sized radius of your chest, moving, arriving. The page turns it
    // into a jolt and a buzz.
    s_figReach = 0.0f;
    const float chestY = py - 0.25f;
    for (int s = 0; s < 2; s++)
    {
        const int h = ARM(s) + 2;
        if (!s_figAttached[h]) continue;
        b3Pos hp = b3Body_GetPosition(s_figBone[h]);
        const float ex = (float)hp.x - px, ey = (float)hp.y - chestY, ez = (float)hp.z - pz;
        const float d2 = ex*ex + ey*ey + ez*ez;
        if (d2 > 0.22f * 0.22f) continue;
        b3Vec3 v = b3Body_GetLinearVelocity(s_figBone[h]);
        const float speed = __builtin_sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
        if (speed < 1.4f) continue;
        const float m = b3Body_GetMass(s_figBone[h]) * speed;
        if (m > s_figReach) s_figReach = m;
    }

    // --- taking a punch -----------------------------------------------------
    //
    // Last, so it wins over the pose the state above just set: a body that is
    // being hit is not also holding its guard. Skipped while it is going down,
    // because that case has already gone fully slack and should stay there —
    // which is the difference asked for between recovering like a boxer and
    // going down when knocked out.
    if (s_figState != FIG_FALLING && s_figState != FIG_DOWN)
    {
        for (int b = 0; b < BONE_COUNT; b++)
        {
            if (s_figSlack[b] <= 0.0f) continue;
            s_figSlack[b] -= dt * 3.2f;            // gathered up in ~0.31 s
            if (s_figSlack[b] < 0.0f) s_figSlack[b] = 0.0f;
            if (!s_figAttached[b] || BONES[b].parent < 0) continue;
            // Reaching zero this frame restores the table value exactly, so
            // nothing has to remember to put the stiffness back.
            float hz = BONES[b].hertz * (1.0f - 0.88f * s_figSlack[b]);
            if (hz < 0.6f) hz = 0.6f;
            if (BONES[b].axis == AX_HINGE)
                b3RevoluteJoint_SetSpringHertz(s_figJoint[b], hz);
            else
                b3SphericalJoint_SetSpringHertz(s_figJoint[b], hz);
        }
    }
}

// --- holding it up ----------------------------------------------------------

static float figMass(void)
{
    float m = 0.0f;
    for (int b = 0; b < BONE_COUNT; b++)
        if (s_figAttached[b]) m += b3Body_GetMass(s_figBone[b]);
    return m;
}

// The pelvis's own rotational inertia, for the rig readout.
static float figInertia(void)
{
    b3Matrix3 Ib = b3Body_GetLocalRotationalInertia(s_figBone[BONE_PELVIS]);
    return 0.5f * (Ib.cx.x + Ib.cz.z);
}

// Holding it up, and turning it to face you.
//
// This is applied every step, before b3World_Step, and it went through three
// wrong shapes before this one. Worth recording, because every one of them
// looked reasonable and two of them were the same mistake in different
// clothes:
//
//   1. Whole-body gains, applied to the pelvis. The gain was sized against
//      the inertia of the whole assembly and then handed to one 4 kg bone,
//      which multiplies the loop gain by the ratio between them — about
//      forty. It span through two radians a second while standing still.
//   2. Pelvis-sized gains, applied to the pelvis. Stable, and far too weak to
//      turn a 62 kg body: 10 N m went in every step for three seconds and
//      moved the heading a sixteenth of a radian.
//   3. Pelvis-sized stiffness with assembly-sized damping. The damping term
//      alone then needs c*dt/I_pelvis < 2 to survive an explicit step, and it
//      came out at 3.2. Unstable, exactly as the arm spike found before it.
//
// There is no gain that satisfies both, because the premise is wrong: a body
// does not stand and turn by torquing its own pelvis. Every part of it moves
// together. So the controller solves for the ACCELERATION the whole body
// should have — linear and angular — and then asks each bone for its own
// share: force = its mass times that acceleration, torque = its inertia times
// it. Stability now depends only on the chosen period against the timestep,
// which is a number you can check by eye, and the gains stop being tangled up
// in a mass ratio that changes every time a limb comes off.
//
// The gravity term is a consequence worth naming: the body carries its own
// weight at every joint, so nothing sags. That is the rule this project
// already paid for once — weight is resistance to changing velocity, never
// static droop. A shoulder does not need to be stiff to stop an arm falling
// off a body; it needs to be stiff to stop a PUNCH moving it.
WASM_EXPORT("w_fig_apply")
void w_fig_apply(void)
{
    if (!s_figExists || !s_figAttached[BONE_PELVIS]) return;
    if (s_figState == FIG_DOWN || s_figState == FIG_FALLING) return;

    const float support = figSupport();
    if (support <= 0.0f) return;          // nothing left to stand on

    const float H = s_figStature;
    b3Pos p = b3Body_GetPosition(s_figBone[BONE_PELVIS]);
    b3Vec3 v = b3Body_GetLinearVelocity(s_figBone[BONE_PELVIS]);
    b3Vec3 w = b3Body_GetAngularVelocity(s_figBone[BONE_PELVIS]);

    // --- the acceleration the whole body wants, linear ---
    //
    // One leg gone and the hip drops: it goes down on the good knee, and it
    // carries only the fraction of its own weight the remaining leg can hold.
    // That is the whole reason a leg is worth attacking.
    // Standing, NOTHING lifts it: the leg column stands on the floor and the
    // floor carries it. The whole vertical is scaled by the carry, not just
    // the gravity term — a PD on hip height is an invisible hand too. While it
    // is planted ay is exactly zero, and nothing may be added to it later.
    //
    // The carry rides in over a fifth of a second and OUT over nearly half of
    // one, because the last thing a step does is put the weight over the foot
    // it has just planted. Handed back sooner than that, the body arrived at
    // its new spot still travelling and stood there leaning.
    const float carryWant = (s_figState == FIG_STEP) ? 1.0f : 0.0f;
    const float crate = s_figDt / (s_figCarry < carryWant ? 0.20f : FIG_CARRY_OUT);
    if (s_figCarry < carryWant)
    { s_figCarry += crate; if (s_figCarry > carryWant) s_figCarry = carryWant; }
    else
    { s_figCarry -= crate; if (s_figCarry < carryWant) s_figCarry = carryWant; }

    const float targetY = s_figHipY * (0.45f + 0.55f * support);
    const float wv = 6.2831853f / 0.35f;
    float ay = ((targetY - (float)p.y) * wv * wv - v.y * 2.0f * 0.9f * wv
             + 9.81f * support) * s_figCarry;

    // The horizontal is solved for the CENTRE OF MASS, not for the pelvis.
    // The pelvis is 117 mm above the CoM of a body whose legs weigh as much as
    // its trunk, and holding the pelvis over a spot leaves the body 0.19 m
    // past its own feet.
    float ax = 0.0f, az = 0.0f;
    int planted = 0;
    const float wh = 6.2831853f / 0.50f;
    if (s_figState == FIG_STEP && support >= 1.0f)
    {
        float tx = 0.0f, tz = 0.0f, have = 0;
        if (s_figRecover)
        {
            // A step to save itself walks to the CAPTURE POINT: the place
            // where, if it put a foot, it would come to rest. That is the
            // whole content of "it steps to keep its balance".
            tx = s_figRecX - s_figComX; tz = s_figRecZ - s_figComZ; have = 1;
        }
        else
        {
            const float reach = (BONES[BONE_L_UPPERARM].oy - 0.375f) * H;
            const float ddx = s_figPlayer.x - s_figComX, ddz = s_figPlayer.z - s_figComZ;
            const float d = __builtin_sqrtf(ddx*ddx + ddz*ddz);
            if (d > 1e-3f)
            {
                // Stop a reach short, and weave across the bearing on the way
                // in, so closing reads as being hunted rather than as running
                // on a rail.
                const float want = reach * FIG_RANGE * FIG_STAND_OFF;
                const float along = (d - want) / d;
                const float weave = __builtin_sinf(s_figSway * 0.5f) * 0.22f;
                tx = ddx * along - ddz / d * weave;
                tz = ddz * along + ddx / d * weave;
                have = 1;
            }
        }
        if (have)
        {
            // A WALK, not a sprint. This is a position spring with a cap on
            // its output, so the further away you stood the longer it spent
            // saturated at 9 m/s2 and the faster it arrived: from four metres
            // it reached the player doing 4.3 m/s with a capture point 1.4 m
            // in front of its own feet, and nothing that respects a support
            // polygon can stop that. Nothing could have. Clamping how far
            // ahead the target may sit clamps the speed it settles at —
            // 0.17 m of lead is about 1.2 m/s, which is a person walking.
            {
                const float tl = __builtin_sqrtf(tx*tx + tz*tz);
                if (tl > FIG_WALK_LEAD)
                { const float k = FIG_WALK_LEAD / tl; tx *= k; tz *= k; }
            }
            ax = tx * wh * wh - s_figVelX * 2.0f * 0.9f * wh;
            az = tz * wh * wh - s_figVelZ * 2.0f * 0.9f * wh;
        }
        // Wherever it walks to is where it will stand when it stops, and the
        // spot is where its FEET are, not where its body is. Stopping a walk
        // leaves the body 0.3 m behind its own feet — you lean back when you
        // stop — and the ankles can only reach 55 mm behind them, so a body
        // handed a spot under its trunk is handed one it cannot hold. The spot
        // is a WORLD point, frozen the moment the stride ends: aimed at the
        // feet themselves it drags them, the target moves with them, and the
        // figure walked itself twenty metres out of the room.
        s_figStanceX = s_figComX; s_figStanceZ = s_figComZ;
        const float cap = 9.0f * support;
        const float hsq = ax*ax + az*az;
        if (hsq > cap * cap)
        { const float k = cap / __builtin_sqrtf(hsq); ax *= k; az *= k; }
    }
    else
    {
        // Planted. What used to be here was a position spring on the pelvis at
        // 158 per second squared — twelve to twenty-seven times what two feet
        // can supply, with no contact term anywhere in it. That was the second
        // invisible hand, and half of a permanent thirty-degree lean.
        //
        // What holds the spot now is the pressure under the soles, commanded
        // in figSense and delivered by the ankle pair below. What is left here
        // is the shear that pressure implies — paid for out of friction, and
        // worth exactly zero with no sole on the floor.
        planted = 1;
        const float hc = s_figComY - s_worldFloorY > 0.30f
                       ? s_figComY - s_worldFloorY : 0.30f;
        ax = FIG_SHEAR * (9.81f / hc) * (s_figComX - s_figCopX);
        az = FIG_SHEAR * (9.81f / hc) * (s_figComZ - s_figCopZ);
        // Holding the spot, out of friction.
        //
        // The pressure under the soles is the primary and it is what makes the
        // topple honest, but two feet 0.24 m long can only move the centre of
        // mass at 1.75 m/s^2 and that is not enough for this rig to survive
        // its own walk: stopping a stride leaves the body 0.3 m behind its
        // feet and the ankles reach 55 mm. What was here before was 158 per
        // second squared with no contact term in it at all — the invisible
        // hand the headset kept reporting.
        //
        // This is the same idea at a twentieth of the size and scaled by the
        // MEASURED grounded licence, so it is worth exactly nothing in the
        // air, nothing with the legs swept, and nothing lying on the floor.
        // It is applied as a pair against the loaded soles below, so it sums
        // to zero over the figure and cannot hold anything up.
        if (FIG_HOLD_A > 0.0f)
        {
            const float wp = 6.2831853f;             // 1 s period; wp*dt = 0.087
            float hx = (s_figTgtX - s_figComX) * wp * wp - s_figVelX * 2.0f * 0.9f * wp;
            float hz = (s_figTgtZ - s_figComZ) * wp * wp - s_figVelZ * 2.0f * 0.9f * wp;
            // And it lets go where it has been hit. The joint springs already
            // go slack under a blow so the body GIVES; a station-keeper that
            // did not give with them would haul the body straight back the
            // same frame, which is the complaint this whole build exists to
            // answer, one level down.
            const float hcap = FIG_HOLD_A * s_figGnd * support
                             * (1.0f - FIG_HOLD_SLACK * s_figSlackAny);
            const float hsq2 = hx*hx + hz*hz;
            if (hsq2 > hcap * hcap)
            { const float k = hcap / __builtin_sqrtf(hsq2); hx *= k; hz *= k; }
            ax += hx; az += hz;
        }
        // The last thing a step does is put the weight over the foot it has
        // just planted. This rides out with the carry — it is the STRIDE's
        // authority being handed back, not a standing controller — and without
        // it the body arrived at its new spot still travelling, walked through
        // the player and went over: the walk-in ended 0.9 m past him, down.
        if (s_figCarry > 0.001f)
        {
            // Only so far onto them. Stopping a stride leaves the feet a third
            // of a metre in front of the body, and hauling the body all the
            // way onto them walks it that much closer than the stride meant to
            // stop — measured as the figure standing 0.43 m from the player
            // where it means to stand 0.62, close enough that a punch thrown
            // at it never breaks contact and never registers as a blow at all.
            float ex = s_figSoleCX - s_figComX, ez = s_figSoleCZ - s_figComZ;
            const float el = __builtin_sqrtf(ex*ex + ez*ez);
            if (el > FIG_ARRIVE_E) { ex *= FIG_ARRIVE_E / el; ez *= FIG_ARRIVE_E / el; }
            const float kx = ex * wh * wh - s_figVelX * 2.0f * 0.9f * wh;
            const float kz = ez * wh * wh - s_figVelZ * 2.0f * 0.9f * wh;
            const float kcap = 9.0f * support * s_figCarry;
            const float ksq = kx*kx + kz*kz;
            float kk = 1.0f;
            if (ksq > kcap * kcap) kk = kcap / __builtin_sqrtf(ksq);
            ax += kx * kk; az += kz * kk;
        }
        // Friction is the whole budget: 0.7 of what the soles are carrying.
        // Nothing on the floor, nothing to push against. What the stride is
        // still carrying is exempt, because that is not being paid for out of
        // contact — it is the same carried authority the walk had, handing
        // itself back.
        float cap = FIG_MU * 9.81f * s_figGnd;
        const float carried = 9.0f * support * s_figCarry;
        if (carried > cap) cap = carried;
        if (cap > 9.0f * support) cap = 9.0f * support;
        const float hsq = ax*ax + az*az;
        if (hsq > cap * cap)
        { const float k = cap / __builtin_sqrtf(hsq); ax *= k; az *= k; }
    }

    // --- and angular ---
    //
    // Upright is the cross product of its own up with the world's: the axis
    // that swings it back level, its length the sine of how far over it is.
    // Heading is added deliberately — turning to face you is something it
    // does, not something that happens to it.
    b3Matrix3 m = b3MakeMatrixFromQuat(b3Body_GetRotation(s_figBone[BONE_PELVIS]));
    const float ux = -m.cy.z, uz = m.cy.x;
    const float facing = __builtin_atan2f(m.cz.x, m.cz.z);      // its own +Z
    const float want = __builtin_atan2f(s_figPlayer.x - (float)p.x,
                                        s_figPlayer.z - (float)p.z);
    float err = want - facing;
    while (err > 3.14159265f) err -= 6.2831853f;
    while (err < -3.14159265f) err += 6.2831853f;

    const float wa = 6.2831853f / 0.55f;      // wa*dt = 0.16 at 72 Hz
    // THE MAGIC FORCE, and what is left of it.
    //
    // This term used to be applied unconditionally to every carried bone. It
    // is a torque that swings the body back level, with no contact anywhere in
    // it, so a figure thrown clear of the floor rolled itself upright in
    // mid-air — measured at 355 degrees of pelvis rotation over 1.8 s of free
    // fall, ending vertical with 0 N under it. That is exactly the thing the
    // headset kept reporting: held up by something that is not there.
    //
    // It survives multiplied by the carry, which means it survives ONLY for
    // the stride. figStride is an authored gait against a force-carried pelvis
    // with no foot placement; it cannot balance and never could, and pretending
    // otherwise is what put the invisible hand there in the first place. The
    // carry is exactly zero the moment a foot is planted, so standing gets none
    // of this at all: upright is the leg column, the joint springs, and the
    // ankle pair, all of which need the floor.
    float rx = (ux * wa * wa - w.x * 2.0f * wa) * s_figCarry;
    float rz = (uz * wa * wa - w.z * 2.0f * wa) * s_figCarry;
    // Turning used to be free because the feet were not on the floor. Now
    // they are, and the whole weight through two soles is real friction to
    // scrub — 542 N at 0.7 is 379 N to break before anything turns at all.
    // Proportional gain cannot close a Coulomb deadband: it stalls the moment
    // the torque drops under the friction, and geared four times it still
    // stalled 0.14 rad short. An integrator can, because it keeps winding
    // until the foot lets go. Only while it is planted — a stride turns freely
    // and would wind this up into a spin. The damping term is deliberately NOT
    // geared: it is what a punch pushes against, and multiplying it turned
    // every blow into a whip that took the arm off at the shoulder.
    // Geared FOUR times, and the gearing is monotonic over everything tried:
    // 0.50 rad short at one, 0.138 at four, 0.109 at five, 0.073 at eight.
    // Four is not the tightest — it is the most it can be geared before the
    // figure starts squaring up to you so hard that a scripted pilot working
    // its legs over can no longer reach them: at five the page run left both
    // legs at 100%, at eight it turned into you and landed 324 blows in a
    // run that usually lands 22. An integrator was built for the remaining
    // 0.14 rad and taken out again: it closed the gap and cost half the damage
    // four punches do to a thigh, because it wound on the fraction of a radian
    // a landed blow leaves behind and turned the body away from the next one.
    float ry = err * wa * wa * 4.0f - w.y * 2.0f * wa;
    {
        const float capR = 90.0f * support * s_figCarry;
        const float sq = rx*rx + rz*rz;
        if (sq > capR * capR)
        { const float k = capR / __builtin_sqrtf(sq); rx *= k; rz *= k; }
        // Turning is something it DOES, and what it does it with is friction
        // under its own soles. The gate saturates rather than scaling: at a
        // quarter of body weight on the floor it has all the heading authority
        // it ever had, so standing is untouched — a proportional gate cost
        // 0.014 rad of a 0.15 rad budget that only has 0.008 to give.
        float lic = s_figGnd > s_figCarry ? s_figGnd : s_figCarry;
        lic *= 4.0f;                              // saturates at 0.25
        if (lic > 1.0f) lic = 1.0f;
        const float capY = 90.0f * support * lic;
        if (ry >  capY) ry =  capY;
        if (ry < -capY) ry = -capY;
    }

    s_figDbg[0] = err; s_figDbg[1] = ry; s_figDbg[2] = s_figCarry; s_figDbg[3] = ax;
    s_figDbg[4] = support; s_figDbg[5] = az; s_figDbg[6] = w.y; s_figDbg[7] = (float)p.y;

    // --- every bone takes its own share ---
    for (int b = 0; b < BONE_COUNT; b++)
    {
        // Not "is my own joint intact" — "am I still part of the body". A hand
        // whose forearm has come off has an intact wrist and is not.
        if (!s_figCarried[b]) continue;
        const float mb = b3Body_GetMass(s_figBone[b]);
        b3Vec3 f; f.x = ax * mb; f.y = ay * mb; f.z = az * mb;
        b3Body_ApplyForceToCenter(s_figBone[b], f, true);

        b3Matrix3 Ib = b3Body_GetLocalRotationalInertia(s_figBone[b]);
        const float I = (Ib.cx.x + Ib.cy.y + Ib.cz.z) * (1.0f / 3.0f);
        b3Vec3 t; t.x = rx * I; t.y = ry * I; t.z = rz * I;
        b3Body_ApplyTorque(s_figBone[b], t, true);
    }

    // --- and the soles take the reaction for the shear -----------------------
    //
    // The horizontal above was applied to every carried bone; this is the
    // other half of it, so the pair sums to exactly zero over the figure and
    // the only place it can go is into friction under a loaded sole. Never
    // during a stride: 497 N of shear into a one kilogram swing foot launches
    // it, measured.
    if (planted)
    {
        const float lsum = s_figLoad[0] + s_figLoad[1];
        if (lsum > 1e-3f)
            for (int s = 0; s < 2; s++)
            {
                if (!s_figLoaded[s]) continue;
                const int ft = LEG(s) + 2;
                if (!s_figCarried[ft]) continue;
                const float lam = s_figLoad[s] / lsum;
                b3Vec3 f;
                f.x = -lam * s_figMassC * ax; f.y = 0.0f; f.z = -lam * s_figMassC * az;
                b3Body_ApplyForceToCenter(s_figBone[ft], f, true);
            }
    }

    // --- the ankles ----------------------------------------------------------
    //
    // This is the whole answer to "it is being held up by a magic force".
    //
    // A standing body balances at the ankle, and what an ankle can do is
    // bounded by the size of the foot and the load on it. The torque here IS
    // a measured newton count times a lever that has already been clamped
    // inside the sole in figSense — there is no `if`, no flag, no gate. With
    // nothing under the foot the load is zero and the torque is zero, which is
    // why nothing in the air can be righted by it.
    //
    // It is applied as a PAIR between the two bodies the ankle joins, so it
    // sums to zero over the figure and cannot lift anything. No joint spring
    // sits in its transmission path, so a punch that slackens the leg costs it
    // nothing — a slack ankle actually makes the command more exact, because
    // the passive spring torque that subtracts from it goes to zero.
    s_figTauA = 0.0f;
    for (int s = 0; s < 2; s++)
    {
        if (!s_figLoaded[s]) continue;
        const int ft = LEG(s) + 2, sh = LEG(s) + 1;
        if (!s_figCarried[ft] || !s_figCarried[sh]) continue;
        // Faded out with the carry. A stride is controlled falling: mid-step
        // the capture point is most of a metre in front of the stance foot, so
        // an ankle asked to bring it back saturates against the toe and pitches
        // the foot up under a gait that is not listening. The walk is authored
        // and says so; standing is physics.
        const float N = s_figLoad[s] * FIG_ANKLE_K * (1.0f - s_figCarry);
        const float lx = s_figFootPX[s] - s_figAnkX[s];
        const float lz = s_figFootPZ[s] - s_figAnkZ[s];
        // The moment the ground reaction at that pressure point makes about
        // the ankle: r x (0, N, 0).
        b3Vec3 t; t.x = -lz * N; t.y = 0.0f; t.z = lx * N;
        b3Vec3 nt; nt.x = lz * N; nt.y = 0.0f; nt.z = -lx * N;
        b3Body_ApplyTorque(s_figBone[sh], t, true);
        b3Body_ApplyTorque(s_figBone[ft], nt, true);
        const float mag = __builtin_sqrtf(t.x*t.x + t.z*t.z);
        if (mag > s_figTauA) s_figTauA = mag;
    }

    // --- and the hips, for what the ankles could not reach --------------------
    //
    // Ankles alone give this body 1.75 m/s^2 forward, because that is what a
    // 0.18 m foot in front of the ankle is worth. Past that a person uses
    // their trunk: pitch it and the pressure moves further than the sole
    // allows. Internal pair, zero-sum, multiplied by the measured load so it
    // is identically zero in the air, and faded out as the trunk goes over so
    // the borrowed momentum has to be paid back.
    s_figTauH = 0.0f;
    {
        const float Ntot = s_figLoad[0] + s_figLoad[1];
        int nth = 0;
        for (int s = 0; s < 2; s++) if (s_figCarried[LEG(s)]) nth++;
        if (planted && Ntot > 1e-3f && nth > 0)
        {
            float fx = 0.0f, fz = 0.0f;
            for (int s = 0; s < 2; s++)
            {
                if (!s_figCarried[LEG(s) + 2]) continue;
                fx += s_figFootEZ[s][0]; fz += s_figFootEZ[s][1];
            }
            const float fl = __builtin_sqrtf(fx*fx + fz*fz);
            if (fl > 1e-3f) { fx /= fl; fz /= fl; } else { fx = 0.0f; fz = 1.0f; }
            const float lx = -fz, lz = fx;
            const float res = (s_figWantX - s_figCopX) * fx
                            + (s_figWantZ - s_figCopZ) * fz;
            float th = res * Ntot;
            if (th >  FIG_HIP_MAX) th =  FIG_HIP_MAX;
            if (th < -FIG_HIP_MAX) th = -FIG_HIP_MAX;
            b3Matrix3 pm = b3MakeMatrixFromQuat(b3Body_GetRotation(s_figBone[BONE_PELVIS]));
            float lean = pm.cy.x * fx + pm.cy.z * fz;
            if (lean < 0.0f) lean = -lean;
            float room = 1.0f - lean / 0.55f;
            if (room < 0.0f) room = 0.0f;
            th *= room;
            s_figTauH = th;
            if (th > 1e-4f || th < -1e-4f)
            {
                b3Vec3 tp; tp.x = -th * lx; tp.y = 0.0f; tp.z = -th * lz;
                b3Body_ApplyTorque(s_figBone[BONE_PELVIS], tp, true);
                b3Vec3 tt; tt.x = th * lx / (float)nth; tt.y = 0.0f;
                tt.z = th * lz / (float)nth;
                for (int s = 0; s < 2; s++)
                    if (s_figCarried[LEG(s)]) b3Body_ApplyTorque(s_figBone[LEG(s)], tt, true);
            }
        }
    }

    // --- the legs push ------------------------------------------------------
    //
    // This is what standing IS. Not a lift: an EQUAL AND OPPOSITE pair between
    // the pelvis and each sole. Its sum over the figure is exactly zero, so it
    // cannot levitate anything and cannot hold up a piece that has come off —
    // a severed leg is not in s_figCarried and gets no pair at all.
    //
    // What it does instead is press the sole into the floor. Every joint in
    // the leg constrains translation rigidly, so the pair cannot stretch the
    // leg; the only place it can go is into the contact under the foot, and it
    // is the FLOOR'S REACTION that carries the body up. Measured: the ground
    // reaction under the two soles is the figure's whole weight, and it is
    // that number, not a term in this function, that holds it at hip height.
    //
    // It can push and it cannot pull, and it is capped at twice body weight,
    // so the worst it can ever do is stand up briskly.
    if (s_figCarry < 1.0f)
    {
        const int nl = figLegCarries(0) + figLegCarries(1);
        if (nl > 0)
        {
            const float M = figMass();
            const float ws = 6.2831853f * 4.0f;       // 4 Hz; ws*dt = 0.35
            float a = (targetY - (float)p.y) * ws * ws - v.y * 2.0f * 0.9f * ws;
            if (a < 0.0f) a = 0.0f;
            float F = M * a / (float)nl;
            const float lim = 2.0f * M * 9.81f / (float)nl;
            if (F > lim) F = lim;
            F *= (1.0f - s_figCarry);

            // A leg can only push with what the floor is giving back. Gated on
            // the WHOLE figure's grounded licence, not per sole: gated per sole
            // the pair went in at the pelvis centre and came out under one foot
            // 79 mm off the midline, which rolls the body toward the leg that
            // was already unloaded — measured, one sole read 0 N and its ankle
            // rode 0.145 m clear of the floor while the other carried 462 N.
            // Symmetric, it still does the job it is here for: a figure on its
            // back stops jacking itself up with 542 N a leg.
            if (FIG_GATE_W > 0.0f)
            {
                float gate = s_figGnd / FIG_GATE_W;
                if (gate > 1.0f) gate = 1.0f;
                if (gate < 0.0f) gate = 0.0f;
                F *= gate;
            }

            // WHICH foot takes it. The two-foot lateral reach is only
            // reachable if the load can move between the feet, so the split is
            // biased toward where the pressure is being asked for. Driven from
            // the ANKLE GEOMETRY, never closed on the measured load: closed on
            // the measurement it stood for six seconds and then rolled over.
            // The sum over both legs is unchanged.
            float beta = 0.5f;
            if (nl == 2 && s_figCarried[LEG(0) + 2] && s_figCarried[LEG(1) + 2])
            {
                const float ux2 = s_figAnkX[0] - s_figAnkX[1];
                const float uz2 = s_figAnkZ[0] - s_figAnkZ[1];
                const float d2 = ux2*ux2 + uz2*uz2;
                if (d2 > 1e-4f)
                {
                    beta = ((s_figWantX - s_figAnkX[1]) * ux2 +
                            (s_figWantZ - s_figAnkZ[1]) * uz2) / d2;
                    if (beta < 0.08f) beta = 0.08f;
                    if (beta > 0.92f) beta = 0.92f;
                    beta = 0.5f + (beta - 0.5f) * FIG_SPLIT;
                }
            }
            s_figShift += (beta - s_figShift) * (s_figDt / (s_figDt + FIG_SHIFT_TAU));

            const float Wc = (s_figMassC > 1.0f ? s_figMassC : M) * 9.81f;
            for (int s = 0; s < 2; s++)
            {
                if (!figLegCarries(s)) continue;
                const int ft = LEG(s) + 2;
                if (!s_figCarried[ft]) continue;
                float Fs = F;
                if (nl == 2) Fs = F * 2.0f * (s == 0 ? s_figShift : 1.0f - s_figShift);
                // A leg can only push with what its own sole is carrying. This
                // is saturated with better than twice the margin while it is
                // standing, so it contributes no loop gain there. What it
                // kills is the 542 N per leg jack that a figure lying on its
                // back would otherwise go on applying for ever.
                if (Fs <= 0.0f) continue;
                b3Vec3 up = { 0.0f,  Fs, 0.0f };
                b3Vec3 dn = { 0.0f, -Fs, 0.0f };
                b3Body_ApplyForceToCenter(s_figBone[BONE_PELVIS], up, true);
                b3Body_ApplyForceToCenter(s_figBone[ft], dn, true);
            }
        }
    }
}

// --- what breaking means ----------------------------------------------------

// Called after b3World_Step (and after w_vox_post, which is where the damage
// actually lands). A bone battered past its break point loses the joint that
// held it, and falls — with every bone below it, because those are still
// jointed to IT. Break an elbow and the forearm and the hand go together, and
// nothing in this function says so; the skeleton does.
WASM_EXPORT("w_fig_post")
void w_fig_post(void)
{
    if (!s_figExists) return;
    s_figBroke = 0;
    for (int b = 0; b < BONE_COUNT; b++)
    {
        if (!s_figAttached[b] || BONES[b].parent < 0) continue;
        if (!figBroken(b)) continue;
        b3DestroyJoint(s_figJoint[b], true);
        s_figAttached[b] = 0;
        s_figBroke++;
        // Loose now, and it should look loose: it falls, it tumbles, and it
        // stops belonging to the team that ignores its own limbs.
        b3Body_SetAngularDamping(s_figBone[b], 0.4f);
        b3Body_SetLinearDamping(s_figBone[b], 0.05f);
    }
    // This is the only place attachment changes mid-fight, so it is the only
    // place the closure has to be re-settled.
    figCarry();
}

// [exists, state, bonesOn, cellsAlive, cellsTotal, support, hipX, hipY, hipZ,
//  facing, distance, windup01, arm, reachedYou, brokeThisStep, stature]
WASM_EXPORT("w_fig_state")
float* w_fig_state(void)
{
    static float out[16];
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    if (!s_figExists) return out;

    int on = 0, alive = 0, total = 0;
    for (int b = 0; b < BONE_COUNT; b++)
    {
        if (s_figAttached[b]) on++;
        const int g = s_figGrid[b];
        if (g < 0 || !s_vxG[g].used) continue;
        alive += s_vxG[g].alive;
        total += s_vxG[g].n[0] * s_vxG[g].n[1] * s_vxG[g].n[2];
    }

    b3Pos p = b3Body_GetPosition(s_figBone[BONE_PELVIS]);
    b3Matrix3 m = b3MakeMatrixFromQuat(b3Body_GetRotation(s_figBone[BONE_PELVIS]));
    const float dx = s_figPlayer.x - (float)p.x, dz = s_figPlayer.z - (float)p.z;

    out[0] = 1.0f;
    out[1] = (float)s_figState;
    out[2] = (float)on;
    out[3] = (float)alive;
    out[4] = (float)total;
    out[5] = figSupport();
    out[6] = (float)p.x; out[7] = (float)p.y; out[8] = (float)p.z;
    out[9] = __builtin_atan2f(m.cz.x, m.cz.z);
    out[10] = __builtin_sqrtf(dx*dx + dz*dz);
    out[11] = s_figState == FIG_WINDUP ? (s_figTimer / 0.26f) : 0.0f;
    out[12] = (float)s_figArm;
    out[13] = s_figReach;
    out[14] = (float)s_figBroke;
    out[15] = s_figStature;
    return out;
}

// The numbers the rig is actually built from: total mass, the pelvis's mass
// and its rotational inertia (the two the balance gains are derived from),
// the stand height, and the reach. Exported because every controller fault
// in this project so far has been a gain sized against the wrong quantity,
// and the only cure is being able to read the quantity.
WASM_EXPORT("w_fig_rig")
float* w_fig_rig(void)
{
    static float out[5];
    for (int i = 0; i < 5; i++) out[i] = 0.0f;
    if (!s_figExists) return out;
    out[0] = figMass();
    out[1] = b3Body_GetMass(s_figBone[BONE_PELVIS]);
    out[2] = figInertia();
    out[3] = s_figHipY;
    out[4] = (BONES[BONE_L_UPPERARM].oy - 0.375f) * s_figStature;
    return out;
}

WASM_EXPORT("w_fig_dbg")
float* w_fig_dbg(void) { return s_figDbg; }

// Per bone: [attached, alive, total, damage 0..1]. The page draws from the
// grid poses; this is what tells it which bones are still part of a body and
// how far each one has been beaten in. Ask w_fig_bone_grid for the grid.
WASM_EXPORT("w_fig_bones")
float* w_fig_bones(void)
{
    static float out[BONE_COUNT * 4];
    for (int b = 0; b < BONE_COUNT; b++)
    {
        float* o = &out[b * 4];
        const int g = s_figGrid[b];
        o[0] = (float)s_figAttached[b];
        o[1] = o[2] = o[3] = 0.0f;
        if (g < 0 || g >= VOX_GRIDS || !s_vxG[g].used) continue;
        o[1] = (float)s_vxG[g].alive;
        o[2] = (float)(s_vxG[g].n[0] * s_vxG[g].n[1] * s_vxG[g].n[2]);
        o[3] = vxDamage(&s_vxG[g]);
    }
    return out;
}

// What the floor is actually doing under each foot: [N_left, N_right,
// copL x,y,z, copR x,y,z]. On the build before this one it read 0 N, because
// the soles hovered a centimetre off the ground and the ground carried none of
// the figure's 542 N — every newton of it came from a gravity-cancelling term
// applied bone by bone. On this build it reads the figure's weight.
//
// Not part of the controller. It is called on demand, so it costs nothing per
// step and cannot destabilise anything. It exists because the whole claim of
// this build is "the floor carries it", and that claim was never measurable.
//
WASM_EXPORT("w_fig_ground")
float* w_fig_ground(void)
{
    static float out[8];
    for (int i = 0; i < 8; i++) out[i] = 0.0f;
    if (!s_figExists) return out;

    for (int side = 0; side < 2; side++)
    {
        float N, cx, cy, cz;
        if (!figPartLoad(LEG(side) + 2, &N, &cx, &cy, &cz)) continue;
        out[side] = N;
        out[2 + side * 3 + 0] = cx;
        out[2 + side * 3 + 1] = cy;
        out[2 + side * 3 + 2] = cz;
    }
    return out;
}

// What the balance controller is actually working with. Everything here is
// measured from contact, so this is the readout that says whether the floor —
// and only the floor — is giving it the authority to stand.
//
//  [N_left, N_right, N_total, grounded, comX, comY, comZ, icpX, icpZ,
//   margin, copX, copZ, ankleTorque, hipTorque, outTimer, airTimer,
//   ankLx, ankLz, ankRx, ankRz, polygonCorners, loadedL, loadedR, collapse]
WASM_EXPORT("w_fig_bal")
float* w_fig_bal(void)
{
    static float out[24];
    for (int i = 0; i < 24; i++) out[i] = 0.0f;
    if (!s_figExists) return out;
    out[0] = s_figLoad[0];
    out[1] = s_figLoad[1];
    out[2] = s_figLoad[0] + s_figLoad[1];
    out[3] = s_figGnd;
    out[4] = s_figComX; out[5] = s_figComY; out[6] = s_figComZ;
    out[7] = s_figIcpX; out[8] = s_figIcpZ;
    out[9] = s_figMargin;
    out[10] = s_figCopX; out[11] = s_figCopZ;
    out[12] = s_figTauA; out[13] = s_figTauH;
    out[14] = s_figOut; out[15] = s_figAir;
    out[16] = s_figAnkX[0]; out[17] = s_figAnkZ[0];
    out[18] = s_figAnkX[1]; out[19] = s_figAnkZ[1];
    out[20] = (float)s_figNQ;
    out[21] = (float)s_figLoaded[0]; out[22] = (float)s_figLoaded[1];
    out[23] = s_figCollapse;
    return out;
}

// Where each bone actually is, for anything that needs geometry rather than
// the renderer's grid poses: 8 floats — position, rotation, attached.
WASM_EXPORT("w_fig_pose")
float* w_fig_pose(void)
{
    static float out[BONE_COUNT * 8];
    for (int b = 0; b < BONE_COUNT; b++)
    {
        float* o = &out[b * 8];
        if (!s_figExists) { for (int i = 0; i < 8; i++) o[i] = 0.0f; continue; }
        b3Pos p = b3Body_GetPosition(s_figBone[b]);
        b3Quat q = b3Body_GetRotation(s_figBone[b]);
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
        o[7] = (float)s_figAttached[b];
    }
    return out;
}

// What every joint is actually doing, in radians: the two hinges you can
// name, and the shoulder cones. Guessing whether a joint is pinned at a limit
// has wasted more time on this project than every other diagnosis combined,
// so it is read rather than inferred.
WASM_EXPORT("w_fig_joints")
float* w_fig_joints(void)
{
    static float out[6];
    for (int i = 0; i < 6; i++) out[i] = 0.0f;
    if (!s_figExists) return out;
    for (int s = 0; s < 2; s++)
    {
        if (s_figAttached[ARM(s) + 1])
            out[s] = b3RevoluteJoint_GetAngle(s_figJoint[ARM(s) + 1]);
        if (s_figAttached[LEG(s) + 1])
            out[2 + s] = b3RevoluteJoint_GetAngle(s_figJoint[LEG(s) + 1]);
        if (s_figAttached[ARM(s)])
            out[4 + s] = b3Joint_GetAngularSeparation(s_figJoint[ARM(s)]);
    }
    return out;
}
