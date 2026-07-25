// bridge.c — flat JS-facing API over the real Box3D engine (wasm build).
//
// Exports (called from the WebXR page):
//   w_init()                       create world, ground, cube tower + scatter
//   w_step(dt)                     advance the simulation (4 sub-steps)
//   w_spawn(px,py,pz,vx,vy,vz,h,c) throw a new cube; recycles oldest when full
//   w_count()                      number of dynamic cubes
//   w_reset(sleep, groundY)        empty world + ground at groundY (default 0)
//   w_fill(n)                      drop n cubes into a pile (benchmark scenes)
//   w_state()                      pointer to packed floats, 9 per cube:
//                                  [x,y,z, qx,qy,qz,qw, halfExtent, colorIdx]
//   w_hand_create(i,...)           follow test: one free body chasing your hand
//   w_hand_target(i,x,y,z)         drive body i toward the controller
//   w_hand_limits(i,k,c,maxF)      retune body i's spring constant
//   w_hand_apply()                 apply the follow forces (call before w_step)
//   w_hand_state()                 8 floats per body: [x,y,z, qx,qy,qz,qw, half]
//   w_mech_create(...)             the real machine: torso + two jointed arms
//   w_mech_stand(x,y,z)            where the machine tries to stand
//   w_mech_hand(i,x,y,z,active,q)  haul attachment i to the controller, and aim it
//   w_mech_apply()                 apply leg + arm forces (call before w_step)
//   w_mech_state()                 7 bodies x 7 floats: torso, then upper /
//                                  forearm / attachment for each arm
//   w_mech_lengths()               upper length, forearm length, attachment half
//   w_mech_joints()                shoulder cone, elbow angle, wrist cone, per arm
//   w_mech_tune(k,zeta,maxF,footF) how hard the hand hauls the arm
//   w_mech_upright(period,zeta,max) how the legs hold the machine up
//   w_mech_aim(period,zeta,max)    how the attachment holds the angle you point
//   w_mech_pin(on)                 bolt the torso down, to study an arm alone
//   w_torso_create(...)            piloting spike: the body the arms hang from
//   w_torso_update(x,y,z,yaw)      move the torso with the head, every frame
//   w_arm_create(i,...)            an arm pivoting at a shoulder on the torso
//   w_arm_target(i,x,y,z)          swing arm i toward the hand
//   w_arm_relax(i)                 let arm i hang slack (grip released)
//   w_arm_limits(i,torque)         retune arm i's actuator (the upgrade slider)
//   w_arm_state()                  8 floats per arm: [x,y,z, qx,qy,qz,qw, length]
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
// `groundY` is the top surface of the floor slab. Defaults to 0 when omitted
// (JS passes 0 for missing numeric args), which is the old behaviour.
//
// It is a parameter because the page cannot assume Y=0 is the floor: if the
// runtime declines a local-floor reference space and falls back to local, the
// origin sits at the viewer instead, and a floor built at 0 ends up at head
// height with the whole scene stacked above it.
WASM_EXPORT("w_reset")
void w_reset(int enableSleep, float groundY)
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
    bd.position.y = groundY - 0.1f;
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

// ---------------------------------------------------------------------------
// Follow test — one physics body chasing your hand
//
// The layer below the arm, and the one that should have been proven first. No
// shoulder, no joint chain: a single free box driven toward the controller by a
// motor with a force ceiling. If this feels wrong, nothing built on top of it
// could feel right, and the arm rig was never the problem.
//
// Uses the same motor-joint mechanism the arm does — velocity request, capped by
// maxVelocityForce — so what is learned here transfers directly.
// ---------------------------------------------------------------------------

#define HAND_COUNT 2
static b3BodyId s_handBody[HAND_COUNT];
static float s_handHalf[HAND_COUNT];
static int s_handExists[HAND_COUNT] = {0, 0};

// Fixed-stiffness force, applied directly. No joint at all.
//
// Two mechanisms were tried and measured before this one, and both turned out to
// be blind to mass:
//
//   Velocity motor with a force cap — a cliff, not a curve. A 3 kg body tracked
//   identically at 50 N and 1500 N, and below the threshold simply fell to the
//   floor. Nothing usable in between.
//
//   Spring specified in hertz — mass-normalised by design. 3 kg and 27 kg lagged
//   by exactly the same amount at every stiffness, ratio 1.00. A frequency-based
//   constraint behaves at that frequency whatever it is pulling; that is the
//   point of expressing stiffness that way.
//
// A spring constant in newtons per metre is not normalised, so F = ma does the
// work: the same pull on twice the mass gives half the acceleration, so it
// trails further, overshoots more and takes longer to settle. That is what
// weight feels like, and it grades continuously.
static float s_handStiffness[HAND_COUNT];
static float s_handDamping[HAND_COUNT];
static float s_handMaxForce[HAND_COUNT];
static b3Vec3 s_handTarget[HAND_COUNT];

WASM_EXPORT("w_hand_create")
void w_hand_create(int i, float x, float y, float z, float half, float density,
                   float stiffness, float damping, float maxForce)
{
    if (i < 0 || i >= HAND_COUNT)
    {
        return;
    }
    s_handHalf[i] = half;
    s_handStiffness[i] = stiffness;
    s_handDamping[i] = damping;
    s_handMaxForce[i] = maxForce;
    s_handTarget[i].x = x; s_handTarget[i].y = y; s_handTarget[i].z = z;

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position.x = x; bd.position.y = y; bd.position.z = z;
    // Gravity would drag it down and mask the effect being measured; the mech
    // arm will want gravity back, but this test is about tracking alone.
    bd.gravityScale = 0.0f;
    s_handBody[i] = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = density;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;
    b3BoxHull hull = b3MakeBoxHull(half, half, half);
    b3CreateHullShape(s_handBody[i], &sd, &hull.base);

    s_handExists[i] = 1;
}

WASM_EXPORT("w_hand_target")
void w_hand_target(int i, float x, float y, float z)
{
    if (i < 0 || i >= HAND_COUNT || !s_handExists[i])
    {
        return;
    }
    s_handTarget[i].x = x; s_handTarget[i].y = y; s_handTarget[i].z = z;
}

WASM_EXPORT("w_hand_limits")
void w_hand_limits(int i, float stiffness, float damping, float maxForce)
{
    if (i < 0 || i >= HAND_COUNT || !s_handExists[i])
    {
        return;
    }
    s_handStiffness[i] = stiffness;
    s_handDamping[i] = damping;
    s_handMaxForce[i] = maxForce;
}

// Applied every step, before b3World_Step.
WASM_EXPORT("w_hand_apply")
void w_hand_apply(void)
{
    for (int i = 0; i < HAND_COUNT; i++)
    {
        if (!s_handExists[i]) continue;

        b3Pos p = b3Body_GetPosition(s_handBody[i]);
        b3Vec3 v = b3Body_GetLinearVelocity(s_handBody[i]);

        float fx = (s_handTarget[i].x - (float)p.x) * s_handStiffness[i] - v.x * s_handDamping[i];
        float fy = (s_handTarget[i].y - (float)p.y) * s_handStiffness[i] - v.y * s_handDamping[i];
        float fz = (s_handTarget[i].z - (float)p.z) * s_handStiffness[i] - v.z * s_handDamping[i];

        // Cap the magnitude so a large snap of the hand cannot fling the body
        // across the room; well above the working range, so it rarely binds.
        float sq = fx*fx + fy*fy + fz*fz;
        float mx = s_handMaxForce[i];
        if (sq > mx * mx)
        {
            float k = mx / __builtin_sqrtf(sq);
            fx *= k; fy *= k; fz *= k;
        }

        b3Vec3 f; f.x = fx; f.y = fy; f.z = fz;
        b3Body_ApplyForceToCenter(s_handBody[i], f, true);
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

// ---------------------------------------------------------------------------
// The mech — an actual jointed machine, not a feel that was tuned
//
// Everything before this faked weight: a spring constant chosen so that lag
// looked right, a damping dial chosen so that impacts looked right. That is what
// you do when you have no physics engine. With one, you build the machine and
// the feel is whatever the machine does.
//
//   torso (dynamic, held up by a limited spring — the legs)
//     |
//     +-- spherical joint (shoulder, cone-limited)
//           |
//           upper arm (dynamic)
//             |
//             +-- revolute joint (elbow, angle-limited)
//                   |
//                   forearm (dynamic)
//
// The player's hand is not a target the arm is told to match. A force pulls the
// *wrist* toward it, capped, exactly as if you had hold of the wrist and were
// dragging it. Every other behaviour is a consequence:
//
//   - weight comes from the segments' mass, not a tuned constant
//   - lag comes from having to accelerate that mass through joints
//   - reach limits come from the arm's length and its cone limit
//   - overshoot has nowhere to go but the torso, so a committed punch drags the
//     whole machine forward and off balance
//   - joint strength is a real quantity: the motors' torque ceiling, and what
//     the joint gives way under
// ---------------------------------------------------------------------------

#define MECH_ARMS 2
static b3BodyId s_mTorso;
static b3BodyId s_mUpper[MECH_ARMS];
static b3BodyId s_mFore[MECH_ARMS];
// The attachment at the end of the arm. Not a hand — a mount. Today it is a
// blunt block; the point is that it is a separate articulated body on a wrist,
// so a sword, a gripper or a wrecking ball can be bolted there later and will
// aim, swing and hit on its own terms.
static b3BodyId s_mTool[MECH_ARMS];
static b3JointId s_mWristJ[MECH_ARMS];
static b3Quat s_mToolAim[MECH_ARMS];
static b3JointId s_mShoulder[MECH_ARMS];
static b3JointId s_mElbow[MECH_ARMS];
static b3Vec3 s_mWrist[MECH_ARMS];      // wrist offset in forearm local space
static b3Vec3 s_mHandTarget[MECH_ARMS];
static int s_mHandActive[MECH_ARMS] = {0, 0};
static int s_mExists = 0;

// The torso is held at a target by a spring with a hard force ceiling. That
// ceiling is the machine's footing: push the arms hard enough and the legs
// cannot hold it, so it staggers. It is not a cheat to keep the torso upright,
// it is the legs having finite strength.
static b3Vec3 s_mTorsoTarget;
static float s_mTorsoK = 6000.0f;
static float s_mTorsoC = 900.0f;
static float s_mTorsoMaxF = 9000.0f;

static float s_mUpperLen, s_mForeLen, s_mToolHalf;

WASM_EXPORT("w_mech_create")
void w_mech_create(float x, float y, float z, float upperLen, float foreLen,
                   float thickness, float density, float shoulderTorque,
                   float elbowTorque, float coneAngle)
{
    // Box3D asserts a spherical cone limit of at most a quarter turn, and this
    // is built with NDEBUG so that assert is gone. Handing it 2.2 rad, as this
    // did, sails straight past without complaint and leaves the cone geometry
    // degenerate — cos(cone) goes negative — which showed up as the whole
    // machine thrashing and no amount of tuning helping. Clamped here so a
    // number out of range can never reach the solver again.
    const float coneMax = 1.5707963f;
    if (!(coneAngle >= 0.0f)) coneAngle = 0.0f;      // also catches NaN
    if (coneAngle > coneMax) coneAngle = coneMax;

    s_mUpperLen = upperLen;
    s_mForeLen = foreLen;
    s_mTorsoTarget.x = x; s_mTorsoTarget.y = y; s_mTorsoTarget.z = z;

    // Torso: a real dynamic body. Everything the arms do pushes back on this.
    b3BodyDef td = b3DefaultBodyDef();
    td.type = b3_dynamicBody;
    td.position.x = x; td.position.y = y; td.position.z = z;
    td.angularDamping = 5.0f;   // a mech is not a spinning top
    td.linearDamping = 1.0f;
    s_mTorso = b3CreateBody(s_world, &td);

    b3ShapeDef tsd = b3DefaultShapeDef();
    tsd.density = density * 1.5f;   // the body outweighs the arms
    tsd.baseMaterial.friction = 0.6f;
    // Deliberately small. An earlier version was 0.52 x 0.60 x 0.32 m centred at
    // a fixed height, which swallowed the player's head.
    b3BoxHull thull = b3MakeBoxHull(0.20f, 0.22f, 0.13f);
    b3CreateHullShape(s_mTorso, &tsd, &thull.base);

    for (int i = 0; i < MECH_ARMS; i++)
    {
        const float side = (i == 0) ? -1.0f : 1.0f;
        const float sx = side * 0.22f;   // shoulder, in torso local space
        const float sy = 0.17f;

        // --- upper arm ---
        b3BodyDef ud = b3DefaultBodyDef();
        ud.type = b3_dynamicBody;
        ud.position.x = x + sx; ud.position.y = y + sy; ud.position.z = z;
        ud.angularDamping = 0.15f;   // loose enough to swing
        s_mUpper[i] = b3CreateBody(s_world, &ud);

        b3ShapeDef usd = b3DefaultShapeDef();
        usd.density = density;
        usd.baseMaterial.friction = 0.6f;
        b3BoxHull uhull = b3MakeBoxHull(thickness, thickness, upperLen * 0.5f);
        b3Transform uoff = b3Transform_identity;
        uoff.p.z = -upperLen * 0.5f;      // hangs forward from the shoulder
        b3Vec3 one = {1.0f, 1.0f, 1.0f};
        b3CreateTransformedHullShape(s_mUpper[i], &usd, &uhull.base, uoff, one);

        b3SphericalJointDef sj = b3DefaultSphericalJointDef();
        sj.base.bodyIdA = s_mTorso;
        sj.base.bodyIdB = s_mUpper[i];
        sj.base.localFrameA = b3Transform_identity;
        sj.base.localFrameA.p.x = sx;
        sj.base.localFrameA.p.y = sy;
        // Tilt the cone axis back by 45 degrees.
        //
        // Box3D measures the cone from body A's local +Z, and a quarter turn is
        // as wide as the limit may legally be. Left pointing straight along Z,
        // that puts "arm straight out in front" at the centre of the cone and
        // "arm hanging at your side" exactly on its edge — measured, the arm
        // could not get back to its side at all, ending up 0.38 m adrift.
        // Splitting the difference puts forward and down each 45 degrees off
        // centre, so both are comfortably inside and the arm can also swing
        // across the body.
        sj.base.localFrameA.q.v.x = -0.38268343f;   // -45 deg about X
        sj.base.localFrameA.q.v.y = 0.0f;
        sj.base.localFrameA.q.v.z = 0.0f;
        sj.base.localFrameA.q.s = 0.92387953f;
        sj.base.localFrameB = b3Transform_identity;
        sj.base.collideConnected = false;
        sj.enableConeLimit = true;
        sj.coneAngle = coneAngle;
        // Friction, not a motor. A spherical motor with no target velocity
        // holds the joint still, and at the torque this used to carry (1400 N m)
        // that is a lock, not a shoulder — the arm was being hauled by the hand
        // and braked by its own joint at the same time, which is what made it
        // thrash instead of swing. What is left is the stiffness of the
        // mechanism itself: enough to feel like machinery, far too little to
        // stop the arm moving.
        sj.enableMotor = shoulderTorque > 0.0f;
        sj.maxMotorTorque = shoulderTorque;
        s_mShoulder[i] = b3CreateSphericalJoint(s_world, &sj);

        // --- forearm ---
        b3BodyDef fd = b3DefaultBodyDef();
        fd.type = b3_dynamicBody;
        fd.position.x = x + sx; fd.position.y = y + sy; fd.position.z = z - upperLen;
        fd.angularDamping = 0.15f;
        s_mFore[i] = b3CreateBody(s_world, &fd);

        b3ShapeDef fsd = b3DefaultShapeDef();
        fsd.density = density;
        fsd.baseMaterial.friction = 0.6f;
        b3BoxHull fhull = b3MakeBoxHull(thickness * 0.9f, thickness * 0.9f, foreLen * 0.5f);
        b3Transform foff = b3Transform_identity;
        foff.p.z = -foreLen * 0.5f;
        b3CreateTransformedHullShape(s_mFore[i], &fsd, &fhull.base, foff, one);

        // Elbow.
        //
        // Box3D hinges about the joint frame's local +Z, and these frames were
        // left as identity — which pointed the hinge straight down the arm's
        // own length. That is a twist, not an elbow: the forearm could spin
        // about the arm but could not fold at all, so the whole limb behaved as
        // one rigid stick. It is why reaching for a point half an arm away left
        // the arm stretched out full length beside it, and why the thing read
        // as two poles rather than as an arm.
        //
        // Turning both frames a quarter turn about Y swings that axis onto the
        // arm's local X — sideways, across the limb — which is what a hinge
        // needs to be. Both frames get the same rotation, so the joint's zero
        // angle is still "straight".
        const float rt2 = 0.70710678f;
        b3Transform elbowFrame = b3Transform_identity;
        elbowFrame.q.v.x = 0.0f; elbowFrame.q.v.y = rt2; elbowFrame.q.v.z = 0.0f;
        elbowFrame.q.s = rt2;

        b3RevoluteJointDef rj = b3DefaultRevoluteJointDef();
        rj.base.bodyIdA = s_mUpper[i];
        rj.base.bodyIdB = s_mFore[i];
        rj.base.localFrameA = elbowFrame;
        rj.base.localFrameA.p.z = -upperLen;
        rj.base.localFrameB = elbowFrame;
        rj.base.collideConnected = false;
        rj.enableLimit = true;
        // Measured, not assumed: with the hinge on the right axis the arm folds
        // toward negative angles. Read off w_mech_joints, the elbow sat pinned
        // at its limit in every pose, arm stretched to 0.85 m for a target
        // 0.28 m away, because the fold direction was fenced off.
        rj.lowerAngle = -2.40f;   // fully folded
        rj.upperAngle = 0.10f;    // just short of straight — no hyperextending
        // Same as the shoulder: this is the hinge's own stiffness, not a motor
        // driving the arm anywhere. The limits above are what stop the elbow
        // bending the wrong way; the torque here only resists.
        rj.enableMotor = elbowTorque > 0.0f;
        rj.maxMotorTorque = elbowTorque;
        rj.motorSpeed = 0.0f;
        s_mElbow[i] = b3CreateRevoluteJoint(s_world, &rj);

        // --- attachment, on a wrist ---
        b3BodyDef kd = b3DefaultBodyDef();
        kd.type = b3_dynamicBody;
        kd.position.x = x + sx; kd.position.y = y + sy;
        kd.position.z = z - upperLen - foreLen;
        kd.angularDamping = 0.25f;
        s_mTool[i] = b3CreateBody(s_world, &kd);

        b3ShapeDef ksd = b3DefaultShapeDef();
        ksd.density = density;
        ksd.baseMaterial.friction = 0.8f;
        const float toolHalf = thickness * 1.3f;
        s_mToolHalf = toolHalf;
        b3BoxHull khull = b3MakeBoxHull(toolHalf, toolHalf, toolHalf);
        b3Transform koff = b3Transform_identity;
        koff.p.z = -toolHalf;
        b3CreateTransformedHullShape(s_mTool[i], &ksd, &khull.base, koff, one);

        // Wrist: spherical, with a tighter cone than the shoulder. It has to
        // hold an aim, so its motor is what resists a sword being knocked aside.
        b3SphericalJointDef wj = b3DefaultSphericalJointDef();
        wj.base.bodyIdA = s_mFore[i];
        wj.base.bodyIdB = s_mTool[i];
        wj.base.localFrameA = b3Transform_identity;
        wj.base.localFrameA.p.z = -foreLen;
        wj.base.localFrameB = b3Transform_identity;
        wj.base.collideConnected = false;
        wj.enableConeLimit = true;
        wj.coneAngle = 1.1f;
        // No motor here. A spherical motor with a zero target velocity is a
        // brake: it was holding the mount still with several hundred newton
        // metres and swamping the aim torque entirely, so the attachment sat at
        // whatever angle the arm's geometry left it at no matter where the
        // controller pointed. The aim torque now works against the cone limit
        // alone, which is what should resist it.
        wj.enableMotor = false;
        s_mWristJ[i] = b3CreateSphericalJoint(s_world, &wj);
        s_mToolAim[i] = b3Quat_identity;

        // The point that tracks the controller: the middle of the attachment.
        //
        // There is no separate hand body — the mech does not need one. What
        // matters is that this point ends up where the player's hand actually
        // is, so the arm's total length has to match their reach rather than
        // stopping a hand's length short at a wrist.
        // Pull from the far face of the attachment, not its middle. That point
        // is the end of the arm, and the end of the arm is what has to arrive
        // where your hand is.
        s_mWrist[i].x = 0.0f; s_mWrist[i].y = 0.0f; s_mWrist[i].z = -2.0f * toolHalf;
        s_mHandActive[i] = 0;
    }
    s_mExists = 1;
}

// Where the machine is trying to stand. Follows the player.
WASM_EXPORT("w_mech_stand")
void w_mech_stand(float x, float y, float z)
{
    s_mTorsoTarget.x = x; s_mTorsoTarget.y = y; s_mTorsoTarget.z = z;
}

WASM_EXPORT("w_mech_hand")
void w_mech_hand(int i, float x, float y, float z, int active,
                 float qx, float qy, float qz, float qw)
{
    if (i < 0 || i >= MECH_ARMS) return;
    s_mHandTarget[i].x = x; s_mHandTarget[i].y = y; s_mHandTarget[i].z = z;
    s_mHandActive[i] = active;
    s_mToolAim[i].v.x = qx; s_mToolAim[i].v.y = qy; s_mToolAim[i].v.z = qz;
    s_mToolAim[i].s = qw;
}

// How hard you can haul the wrist. This is the pilot's grip on the controls,
// not the joint strength — the joints have their own ceilings and will give way
// first if they are the weaker link.
// Loose enough that the arm is dragged rather than commanded, damped enough
// that it does not flail. Beyond this, feel is not something measurement here
// can settle — it needs a person in the headset.
static float s_mPullK = 2000.0f;
static float s_mPullMax = 3000.0f;

// Damping is given as a ratio, not a coefficient.
//
// The stiffness above stays a fixed spring constant on purpose: measured
// earlier, a mass-normalised spring lags by exactly the same amount at 3 kg and
// at 27 kg, so it cannot convey weight at all, while a fixed N/m grades it
// properly. That finding is the reason this spike exists and it is not being
// given up.
//
// Damping is a separate question. It is not there to shape the feel, only to
// stop the arm ringing, and how much is needed genuinely does depend on how
// much arm there is to bring to a halt — so it is derived from the arm's own
// mass each frame. 1.0 is critical: it settles without bouncing back.
static float s_mPullZeta = 1.0f;

// How quickly the attachment swings round to the angle you are pointing, and
// how hard it will hold that angle against a knock.
static float s_mAimPeriod = 0.25f;    // seconds
static float s_mAimZeta = 1.0f;
static float s_mAimMax = 12.0f;       // N m per kg of attachment

// How the machine holds itself up, as a response rather than as raw gains.
//
// Written as gains scaled by mass this was unstable at every setting: angular
// dynamics answers to rotational inertia, and the torso's is about 1.2 kg m^2
// against 55 kg of mass, so a damping term scaled by mass came out roughly
// fifty times too strong and an explicit integrator at 72 Hz blew up. Stiffen
// it and the tilt got worse, not better, which is the giveaway.
//
// Period is how long a full correction takes — larger reads as heavier.
// Damping of 1.0 is critical: it comes back level without rebounding past.
static float s_mUprightPeriod = 0.60f;   // seconds
static float s_mUprightZeta = 1.0f;
static float s_mUprightMax = 26.0f;      // N m per kg of machine — leg strength

// Effective rotational inertia about the torso's centre, arms included.
//
// The arms are most of what has to be hauled back upright, and how much they
// weigh is the entire point of the experiment, so leaving them out would tune
// the machine for a body that does not exist.
static float mechInertia(void)
{
    b3Matrix3 Ib = b3Body_GetLocalRotationalInertia(s_mTorso);
    float I = 0.5f * (Ib.cx.x + Ib.cz.z);   // pitch and roll, averaged

    b3Pos c = b3Body_GetPosition(s_mTorso);
    for (int i = 0; i < MECH_ARMS; i++)
    {
        b3BodyId parts[3] = { s_mUpper[i], s_mFore[i], s_mTool[i] };
        for (int k = 0; k < 3; k++)
        {
            b3Pos p = b3Body_GetPosition(parts[k]);
            const float dx = (float)(p.x - c.x);
            const float dy = (float)(p.y - c.y);
            const float dz = (float)(p.z - c.z);
            I += b3Body_GetMass(parts[k]) * (dx*dx + dy*dy + dz*dz);
        }
    }
    return I;
}

static float mechMass(void)
{
    float m = b3Body_GetMass(s_mTorso);
    for (int i = 0; i < MECH_ARMS; i++)
    {
        m += b3Body_GetMass(s_mUpper[i]) + b3Body_GetMass(s_mFore[i]) + b3Body_GetMass(s_mTool[i]);
    }
    return m;
}

WASM_EXPORT("w_mech_tune")
void w_mech_tune(float pullK, float pullZeta, float pullMax, float torsoMaxF)
{
    s_mPullK = pullK; s_mPullZeta = pullZeta; s_mPullMax = pullMax;
    s_mTorsoMaxF = torsoMaxF;
}

// Bolt the torso to the world, so an arm can be studied on its own.
//
// Two things were going wrong at once — the machine could not stand up and the
// arms could not reach — and each was hiding the other. Pinning the torso
// separates them: whatever the arm does with this on is the arm's own doing.
// It is also what a test rig or a wall-mounted arm would be.
// What every joint is actually doing, in radians: shoulder cone angle, elbow
// angle, wrist cone angle, for each arm. Guessing at whether a joint is pinned
// against a limit wastes far more time than reading it off.
WASM_EXPORT("w_mech_joints")
float* w_mech_joints(void)
{
    static float out[MECH_ARMS * 3];
    if (!s_mExists) return out;
    for (int i = 0; i < MECH_ARMS; i++)
    {
        out[i*3 + 0] = b3SphericalJoint_GetConeAngle(s_mShoulder[i]);
        out[i*3 + 1] = b3RevoluteJoint_GetAngle(s_mElbow[i]);
        out[i*3 + 2] = b3SphericalJoint_GetConeAngle(s_mWristJ[i]);
    }
    return out;
}

WASM_EXPORT("w_mech_aim")
void w_mech_aim(float period, float zeta, float maxTorquePerKg)
{
    s_mAimPeriod = period; s_mAimZeta = zeta; s_mAimMax = maxTorquePerKg;
}

WASM_EXPORT("w_mech_pin")
void w_mech_pin(int on)
{
    if (!s_mExists) return;
    b3Body_SetType(s_mTorso, on ? b3_staticBody : b3_dynamicBody);
}

WASM_EXPORT("w_mech_upright")
void w_mech_upright(float period, float zeta, float maxTorquePerKg)
{
    s_mUprightPeriod = period; s_mUprightZeta = zeta; s_mUprightMax = maxTorquePerKg;
}

WASM_EXPORT("w_mech_apply")
void w_mech_apply(void)
{
    if (!s_mExists) return;

    // Legs.
    //
    // They hold the machine's height, full stop. An earlier version made the
    // legs a plain spring that had to fight gravity, so heavier arms dragged the
    // whole mech downwards until it sank out from under the player and the arms
    // could not be swung at all. Legs do not work like that: yours hold you at
    // your height whether or not you are carrying something heavy, and they give
    // out entirely rather than sagging in proportion to the load.
    //
    // So the vertical is gravity-compensated and stiff — the machine stands. The
    // horizontal is a limited spring, which is what lets a committed punch drag
    // it off its footing.
    {
        b3Pos p = b3Body_GetPosition(s_mTorso);
        b3Vec3 v = b3Body_GetLinearVelocity(s_mTorso);
        const float mass = b3Body_GetMass(s_mTorso);

        // Cancel the torso's own weight, then hold the height stiffly.
        float fy = mass * 9.81f
                 + (s_mTorsoTarget.y - (float)p.y) * s_mTorsoK * 3.0f
                 - v.y * s_mTorsoC * 2.0f;

        float fx = (s_mTorsoTarget.x - (float)p.x) * s_mTorsoK - v.x * s_mTorsoC;
        float fz = (s_mTorsoTarget.z - (float)p.z) * s_mTorsoK - v.z * s_mTorsoC;

        // Only the horizontal is capped — that is the footing giving way.
        float hsq = fx*fx + fz*fz;
        if (hsq > s_mTorsoMaxF * s_mTorsoMaxF)
        {
            float k = s_mTorsoMaxF / __builtin_sqrtf(hsq);
            fx *= k; fz *= k;
        }

        b3Vec3 f; f.x = fx; f.y = fy; f.z = fz;
        b3Body_ApplyForceToCenter(s_mTorso, f, true);
    }

    // Staying upright.
    //
    // Without this the machine tumbles. The legs held its height but nothing
    // held its attitude, so every swing torqued the torso back and it rolled
    // over — measured at 300 steps, the right shoulder had swung across to the
    // left side of the body. The arms were fine; the thing they hang from was
    // cartwheeling.
    //
    // Legs and stabilisers really do this, so it is the machine working rather
    // than a correction bolted on. It is capped for the same reason the footing
    // is: a hard enough swing gets to tip you over, and that is the drama. Only
    // pitch and roll are restored — the cross product has no yaw term — so the
    // machine is still free to turn.
    {
        b3Quat q = b3Body_GetRotation(s_mTorso);
        b3Matrix3 m = b3MakeMatrixFromQuat(q);

        // Local up, in world space, crossed with world up. The result is the
        // axis that swings the machine back level, and its length is the sine
        // of how far over it is.
        const float ax = -m.cy.z, az = m.cy.x;

        b3Vec3 w = b3Body_GetAngularVelocity(s_mTorso);
        const float I = mechInertia();
        const float w0 = 6.2831853f / s_mUprightPeriod;
        const float k = I * w0 * w0;                       // torque per radian
        const float c = 2.0f * s_mUprightZeta * I * w0;    // torque per rad/s

        float tx = ax * k - w.x * c;
        float ty =        - w.y * c;
        float tz = az * k - w.z * c;

        const float cap = s_mUprightMax * mechMass();
        float tsq = tx*tx + ty*ty + tz*tz;
        if (tsq > cap * cap)
        {
            float k = cap / __builtin_sqrtf(tsq);
            tx *= k; ty *= k; tz *= k;
        }

        b3Vec3 t; t.x = tx; t.y = ty; t.z = tz;
        b3Body_ApplyTorque(s_mTorso, t, true);
    }

    // Arms: haul each wrist toward the hand. Applied at the wrist, not the
    // centre of mass, so it swings the forearm and torques back through the
    // elbow and shoulder the way pulling a real arm does.
    for (int i = 0; i < MECH_ARMS; i++)
    {
        if (!s_mHandActive[i]) continue;

        b3Pos bp = b3Body_GetPosition(s_mTool[i]);
        b3Quat bq = b3Body_GetRotation(s_mTool[i]);
        b3Matrix3 m = b3MakeMatrixFromQuat(bq);

        // Wrist in world space.
        float wx = (float)bp.x + m.cz.x * s_mWrist[i].z;
        float wy = (float)bp.y + m.cz.y * s_mWrist[i].z;
        float wz = (float)bp.z + m.cz.z * s_mWrist[i].z;

        // What the pull actually has to accelerate is the whole arm, not just
        // the block on the end of it.
        const float armMass = b3Body_GetMass(s_mUpper[i]) + b3Body_GetMass(s_mFore[i])
                            + b3Body_GetMass(s_mTool[i]);
        const float pullC = 2.0f * s_mPullZeta * __builtin_sqrtf(s_mPullK * armMass);

        b3Vec3 v = b3Body_GetLinearVelocity(s_mTool[i]);
        float fx = (s_mHandTarget[i].x - wx) * s_mPullK - v.x * pullC;
        float fy = (s_mHandTarget[i].y - wy) * s_mPullK - v.y * pullC;
        float fz = (s_mHandTarget[i].z - wz) * s_mPullK - v.z * pullC;
        float sq = fx*fx + fy*fy + fz*fz;
        if (sq > s_mPullMax * s_mPullMax)
        {
            float k = s_mPullMax / __builtin_sqrtf(sq);
            fx *= k; fy *= k; fz *= k;
        }

        b3Vec3 f; f.x = fx; f.y = fy; f.z = fz;
        b3Pos at; at.x = wx; at.y = wy; at.z = wz;
        b3Body_ApplyForce(s_mTool[i], f, at, true);

        // Aim. The attachment turns to face where the controller faces, which is
        // what lets a sword be pointed rather than merely carried. Torque-based
        // so it can be resisted or knocked off line.
        {
            // Shortest rotation from current to target, as a quaternion.
            b3Quat cur = b3Body_GetRotation(s_mTool[i]);
            b3Quat tgt = s_mToolAim[i];
            float dot = cur.v.x*tgt.v.x + cur.v.y*tgt.v.y + cur.v.z*tgt.v.z + cur.s*tgt.s;
            if (dot < 0.0f) { tgt.v.x = -tgt.v.x; tgt.v.y = -tgt.v.y; tgt.v.z = -tgt.v.z; tgt.s = -tgt.s; }
            // err = tgt * conj(cur)
            float cx = -cur.v.x, cy = -cur.v.y, cz = -cur.v.z, cw = cur.s;
            float ex = tgt.s*cx + tgt.v.x*cw + tgt.v.y*cz - tgt.v.z*cy;
            float ey = tgt.s*cy - tgt.v.x*cz + tgt.v.y*cw + tgt.v.z*cx;
            float ez = tgt.s*cz + tgt.v.x*cy - tgt.v.y*cx + tgt.v.z*cw;

            // Gains from the attachment's own rotational inertia, not from
            // numbers picked by eye.
            //
            // This was the thing wrecking the arm. At a damping of 22 against
            // an inertia of about 0.027 kg m^2, the damping term came out
            // around eleven times what an explicit step at 72 Hz can take —
            // anything past 2 diverges — so the aim pumped energy in every
            // frame. With the torso bolted down the arm still swept steadily
            // across the body and never settled, which is what gave it away:
            // a fixed target cannot cause drift, only a controller adding
            // energy can.
            b3Matrix3 Ik = b3Body_GetLocalRotationalInertia(s_mTool[i]);
            const float Ia = (Ik.cx.x + Ik.cy.y + Ik.cz.z) / 3.0f;
            const float wAim = 6.2831853f / s_mAimPeriod;
            const float kAim = Ia * wAim * wAim;
            const float cAim = 2.0f * s_mAimZeta * Ia * wAim;

            b3Vec3 w = b3Body_GetAngularVelocity(s_mTool[i]);
            float tx = ex * kAim - w.x * cAim;
            float ty = ey * kAim - w.y * cAim;
            float tz = ez * kAim - w.z * cAim;

            // Capped, so a heavy enough blow knocks the aim off line rather
            // than the mount holding its angle through anything.
            const float aimCap = s_mAimMax * b3Body_GetMass(s_mTool[i]);
            const float tsq = tx*tx + ty*ty + tz*tz;
            if (tsq > aimCap * aimCap)
            {
                const float s = aimCap / __builtin_sqrtf(tsq);
                tx *= s; ty *= s; tz *= s;
            }

            b3Vec3 t; t.x = tx; t.y = ty; t.z = tz;
            b3Body_ApplyTorque(s_mTool[i], t, true);
        }
    }
}

// 7 floats each: torso, then upper/fore/attachment for each arm — 49 total.
WASM_EXPORT("w_mech_state")
float* w_mech_state(void)
{
    static float out[7 * 7];
    if (!s_mExists) return out;
    b3BodyId ids[7] = { s_mTorso,
                        s_mUpper[0], s_mFore[0], s_mTool[0],
                        s_mUpper[1], s_mFore[1], s_mTool[1] };
    for (int i = 0; i < 7; i++)
    {
        float* o = &out[i * 7];
        b3Pos p = b3Body_GetPosition(ids[i]);
        b3Quat q = b3Body_GetRotation(ids[i]);
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
    }
    return out;
}

WASM_EXPORT("w_mech_lengths")
float* w_mech_lengths(void)
{
    static float out[3];
    out[0] = s_mUpperLen; out[1] = s_mForeLen; out[2] = s_mToolHalf;
    return out;
}

// ---------------------------------------------------------------------------
// Piloting spike — an arm that swings from a shoulder
//
// The first version of this failed the only test that matters: it did not read
// as an arm. Two boxes were tethered to invisible anchors by motor joints that
// *translated* them toward the hand, so they drifted through space attached to
// nothing. Correct dynamics, no embodiment — and embodiment is the whole point.
//
// This version is built the other way round. An arm is a limb that pivots at a
// shoulder on a body:
//
//   torso (static)  --spherical joint-->  upper arm (dynamic)
//
// The shoulder is the pivot, the segment's shape is offset so it hangs outward
// from that pivot rather than being centred on it, and the motor applies torque
// to swing it toward the hand. It cannot drift, because it is attached, and it
// swings rather than slides, because that is what a joint does.
//
// The force limit still does the work the design needs: maxMotorTorque against
// the segment's mass decides whether the arm can keep up, so the lag is still
// F = ma and the upgrade is still a bigger number.
// ---------------------------------------------------------------------------

#define ARM_COUNT 2
static b3JointId s_armJoint[ARM_COUNT];
static b3BodyId s_armBody[ARM_COUNT];
static b3Vec3 s_shoulderLocal[ARM_COUNT];  // offset from the torso origin
static b3Vec3 s_shoulder[ARM_COUNT];       // same point in world space, updated each frame
static float s_armLength[ARM_COUNT];
static int s_armExists[ARM_COUNT] = {0, 0};
static b3BodyId s_torso;
static int s_torsoExists = 0;

// Build the torso the arms hang from.
//
// Kinematic rather than static, because shoulders belong to the player, not to
// the room. A static torso has to be placed once and then never moves, so any
// lean, step or change of posture leaves the mech behind and needs a manual
// recalibration to fix. Driven from the head every frame instead, the shoulders
// simply are where the player's shoulders are, and the height question stops
// existing rather than being repeatedly patched.
WASM_EXPORT("w_torso_create")
void w_torso_create(float x, float y, float z, float hx, float hy, float hz)
{
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_kinematicBody;
    bd.position.x = x;
    bd.position.y = y;
    bd.position.z = z;
    s_torso = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 1.0f;
    b3BoxHull hull = b3MakeBoxHull(hx, hy, hz);
    b3CreateHullShape(s_torso, &sd, &hull.base);
    s_torsoExists = 1;
}

// Move the torso to follow the head. Called every frame.
//
// Only yaw is applied, not full head rotation: the body should turn with the
// player but not pitch and roll with their neck, or looking down would tip the
// whole mech over.
WASM_EXPORT("w_torso_update")
void w_torso_update(float x, float y, float z, float yaw)
{
    if (!s_torsoExists)
    {
        return;
    }

    const float half = yaw * 0.5f;
    b3Quat q;
    q.v.x = 0.0f;
    q.v.y = __builtin_sinf(half);
    q.v.z = 0.0f;
    q.s = __builtin_cosf(half);

    b3Pos p;
    p.x = x; p.y = y; p.z = z;
    b3Body_SetTransform(s_torso, p, q);

    // Shoulders ride with the torso, so their world positions have to follow —
    // the arm targeting works from these.
    const float c = q.s * q.s - q.v.y * q.v.y;      // cos(yaw)
    const float sn = 2.0f * q.s * q.v.y;            // sin(yaw)
    for (int i = 0; i < ARM_COUNT; i++)
    {
        const float lx = s_shoulderLocal[i].x, ly = s_shoulderLocal[i].y, lz = s_shoulderLocal[i].z;
        s_shoulder[i].x = x + (lx * c + lz * sn);
        s_shoulder[i].y = y + ly;
        s_shoulder[i].z = z + (-lx * sn + lz * c);
    }
}

// One arm: a segment pivoting at (sx, sy, sz) and reaching `length` forward.
WASM_EXPORT("w_arm_create")
void w_arm_create(int i, float sx, float sy, float sz, float length, float thickness,
                  float density, float maxTorque, float coneAngle)
{
    if (i < 0 || i >= ARM_COUNT || !s_torsoExists)
    {
        return;
    }

    const float half = length * 0.5f;
    b3Pos torsoPos0 = b3Body_GetPosition(s_torso);
    s_shoulderLocal[i].x = sx - (float)torsoPos0.x;
    s_shoulderLocal[i].y = sy - (float)torsoPos0.y;
    s_shoulderLocal[i].z = sz - (float)torsoPos0.z;
    s_shoulder[i].x = sx;
    s_shoulder[i].y = sy;
    s_shoulder[i].z = sz;
    s_armLength[i] = length;

    // The body's origin sits at the shoulder; the shape is pushed out along -Z
    // so the limb extends forward from the pivot instead of straddling it.
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_dynamicBody;
    bd.position.x = sx;
    bd.position.y = sy;
    bd.position.z = sz;
    s_armBody[i] = b3CreateBody(s_world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = density;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;

    b3BoxHull hull = b3MakeBoxHull(thickness, thickness, half);
    b3Transform offset = b3Transform_identity;
    offset.p.z = -half;
    b3Vec3 scale = {1.0f, 1.0f, 1.0f}; // the hull is already the right size
    b3CreateTransformedHullShape(s_armBody[i], &sd, &hull.base, offset, scale);

    // Pivot at the shoulder.
    //
    // localFrameA is expressed in the TORSO's local space, not world space, so
    // it must be the shoulder *relative to the torso origin*. Passing the world
    // position here adds the torso's height to the shoulder's and puts the joint
    // roughly twice as high as intended — which is exactly what it did.
    //
    // localFrameB is in the arm's own space, and the arm body's origin is the
    // shoulder by construction, so identity is correct there.
    b3SphericalJointDef jd = b3DefaultSphericalJointDef();
    jd.base.bodyIdA = s_torso;
    jd.base.bodyIdB = s_armBody[i];
    jd.base.localFrameA = b3Transform_identity;
    jd.base.localFrameA.p = s_shoulderLocal[i];
    jd.base.localFrameB = b3Transform_identity;
    jd.base.collideConnected = false;
    jd.enableMotor = true;
    jd.maxMotorTorque = maxTorque;
    jd.enableConeLimit = true;
    jd.coneAngle = coneAngle;
    s_armJoint[i] = b3CreateSphericalJoint(s_world, &jd);

    s_armExists[i] = 1;
}

// Gain on the angular error, in 1/second.
#define ARM_GAIN 9.0f
#define ARM_MAX_RATE 12.0f

// Swing the arm toward the hand.
//
// Only a *request*: the motor honours it up to maxMotorTorque, and against a
// heavy segment that is not far. The shortfall is the sensation being tested.
WASM_EXPORT("w_arm_target")
void w_arm_target(int i, float x, float y, float z)
{
    if (i < 0 || i >= ARM_COUNT || !s_armExists[i])
    {
        return;
    }

    // Where the limb currently points: its own -Z axis in world space.
    b3Quat q = b3Body_GetRotation(s_armBody[i]);
    b3Matrix3 m = b3MakeMatrixFromQuat(q);
    float dx = -m.cz.x, dy = -m.cz.y, dz = -m.cz.z;

    // Where it should point: from shoulder toward the hand.
    float tx = x - s_shoulder[i].x;
    float ty = y - s_shoulder[i].y;
    float tz = z - s_shoulder[i].z;
    float tl = __builtin_sqrtf(tx*tx + ty*ty + tz*tz);
    if (tl < 1e-4f)
    {
        return;
    }
    tx /= tl; ty /= tl; tz /= tl;

    // Rotation carrying the current direction onto the target: axis is their
    // cross product, angle is the arc between them.
    float ax = dy*tz - dz*ty;
    float ay = dz*tx - dx*tz;
    float az = dx*ty - dy*tx;
    float sinA = __builtin_sqrtf(ax*ax + ay*ay + az*az);
    float cosA = dx*tx + dy*ty + dz*tz;
    float angle = __builtin_atan2f(sinA, cosA);

    b3Vec3 w;
    if (sinA < 1e-5f)
    {
        w.x = 0.0f; w.y = 0.0f; w.z = 0.0f;
    }
    else
    {
        float rate = angle * ARM_GAIN;
        if (rate > ARM_MAX_RATE) rate = ARM_MAX_RATE;
        float k = rate / sinA;
        w.x = ax * k; w.y = ay * k; w.z = az * k;
    }
    b3SphericalJoint_SetMotorVelocity(s_armJoint[i], w);
}

// Let the limb go slack: stop driving it and let it hang under its own weight.
// Released grip should feel like letting go, not like a frozen arm.
WASM_EXPORT("w_arm_relax")
void w_arm_relax(int i)
{
    if (i < 0 || i >= ARM_COUNT || !s_armExists[i])
    {
        return;
    }
    b3Vec3 zero = {0.0f, 0.0f, 0.0f};
    b3SphericalJoint_SetMotorVelocity(s_armJoint[i], zero);
}

// Retune the actuator — the upgrade slider.
WASM_EXPORT("w_arm_limits")
void w_arm_limits(int i, float maxTorque)
{
    if (i < 0 || i >= ARM_COUNT || !s_armExists[i])
    {
        return;
    }
    b3SphericalJoint_SetMaxMotorTorque(s_armJoint[i], maxTorque);
}

// Per arm: shoulder position, rotation quaternion, and length. The renderer
// draws the segment hanging off the shoulder along the rotated -Z.
WASM_EXPORT("w_arm_state")
float* w_arm_state(void)
{
    static float out[ARM_COUNT * 8];
    for (int i = 0; i < ARM_COUNT; i++)
    {
        float* o = &out[i * 8];
        if (!s_armExists[i])
        {
            continue;
        }
        b3Pos p = b3Body_GetPosition(s_armBody[i]);
        b3Quat q = b3Body_GetRotation(s_armBody[i]);
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
        o[7] = s_armLength[i];
    }
    return out;
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
