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
//   w_mech_hand(i,x,y,z,active)    haul wrist i toward the player's hand
//   w_mech_apply()                 apply leg + arm forces (call before w_step)
//   w_mech_state()                 7 floats each: torso, then upper/fore/hand x2
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
static b3BodyId s_mHand[MECH_ARMS];
static b3JointId s_mWristJ[MECH_ARMS];
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

static float s_mUpperLen, s_mForeLen, s_mHandHalf;

WASM_EXPORT("w_mech_create")
void w_mech_create(float x, float y, float z, float upperLen, float foreLen,
                   float thickness, float density, float shoulderTorque,
                   float elbowTorque, float coneAngle)
{
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
    b3BoxHull thull = b3MakeBoxHull(0.26f, 0.30f, 0.16f);
    b3CreateHullShape(s_mTorso, &tsd, &thull.base);

    for (int i = 0; i < MECH_ARMS; i++)
    {
        const float side = (i == 0) ? -1.0f : 1.0f;
        const float sx = side * 0.26f;   // shoulder, in torso local space
        const float sy = 0.20f;

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
        sj.base.localFrameB = b3Transform_identity;
        sj.base.collideConnected = false;
        sj.enableConeLimit = true;
        sj.coneAngle = coneAngle;
        sj.enableMotor = true;
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

        // Elbow: a hinge about the arm's local X, which bends the way an elbow
        // bends and cannot hyperextend.
        b3RevoluteJointDef rj = b3DefaultRevoluteJointDef();
        rj.base.bodyIdA = s_mUpper[i];
        rj.base.bodyIdB = s_mFore[i];
        rj.base.localFrameA = b3Transform_identity;
        rj.base.localFrameA.p.z = -upperLen;
        rj.base.localFrameB = b3Transform_identity;
        rj.base.collideConnected = false;
        rj.enableLimit = true;
        rj.lowerAngle = -2.4f;    // fully folded
        rj.upperAngle = 0.10f;    // just short of straight
        rj.enableMotor = true;
        rj.maxMotorTorque = elbowTorque;
        rj.motorSpeed = 0.0f;
        s_mElbow[i] = b3CreateRevoluteJoint(s_world, &rj);

        // --- hand ---
        //
        // The controller sits in the player's hand, not their wrist, so the
        // chain needs one more link or the mapping is off by a hand's length and
        // the fist is not what lands a punch.
        b3BodyDef hd = b3DefaultBodyDef();
        hd.type = b3_dynamicBody;
        hd.position.x = x + sx; hd.position.y = y + sy;
        hd.position.z = z - upperLen - foreLen;
        hd.angularDamping = 0.2f;
        s_mHand[i] = b3CreateBody(s_world, &hd);

        b3ShapeDef hsd = b3DefaultShapeDef();
        hsd.density = density;
        hsd.baseMaterial.friction = 0.8f;
        const float handHalf = thickness * 1.25f;
        s_mHandHalf = handHalf;
        b3BoxHull hhull = b3MakeBoxHull(handHalf, handHalf, handHalf);
        b3Transform hoff = b3Transform_identity;
        hoff.p.z = -handHalf;
        b3CreateTransformedHullShape(s_mHand[i], &hsd, &hhull.base, hoff, one);

        // Wrist: a spherical joint with a tight cone, since a wrist bends a lot
        // less than a shoulder.
        b3SphericalJointDef wj = b3DefaultSphericalJointDef();
        wj.base.bodyIdA = s_mFore[i];
        wj.base.bodyIdB = s_mHand[i];
        wj.base.localFrameA = b3Transform_identity;
        wj.base.localFrameA.p.z = -foreLen;
        wj.base.localFrameB = b3Transform_identity;
        wj.base.collideConnected = false;
        wj.enableConeLimit = true;
        wj.coneAngle = 1.0f;
        wj.enableMotor = true;
        wj.maxMotorTorque = elbowTorque * 0.4f;
        s_mWristJ[i] = b3CreateSphericalJoint(s_world, &wj);

        // The grip point is the middle of the hand, in its local frame.
        s_mWrist[i].x = 0.0f; s_mWrist[i].y = 0.0f; s_mWrist[i].z = -handHalf;
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
void w_mech_hand(int i, float x, float y, float z, int active)
{
    if (i < 0 || i >= MECH_ARMS) return;
    s_mHandTarget[i].x = x; s_mHandTarget[i].y = y; s_mHandTarget[i].z = z;
    s_mHandActive[i] = active;
}

// How hard you can haul the wrist. This is the pilot's grip on the controls,
// not the joint strength — the joints have their own ceilings and will give way
// first if they are the weaker link.
static float s_mPullK = 900.0f;
static float s_mPullC = 60.0f;
static float s_mPullMax = 2500.0f;

WASM_EXPORT("w_mech_tune")
void w_mech_tune(float pullK, float pullC, float pullMax, float torsoMaxF)
{
    s_mPullK = pullK; s_mPullC = pullC; s_mPullMax = pullMax;
    s_mTorsoMaxF = torsoMaxF;
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

    // Arms: haul each wrist toward the hand. Applied at the wrist, not the
    // centre of mass, so it swings the forearm and torques back through the
    // elbow and shoulder the way pulling a real arm does.
    for (int i = 0; i < MECH_ARMS; i++)
    {
        if (!s_mHandActive[i]) continue;

        b3Pos bp = b3Body_GetPosition(s_mHand[i]);
        b3Quat bq = b3Body_GetRotation(s_mHand[i]);
        b3Matrix3 m = b3MakeMatrixFromQuat(bq);

        // The grip point, in world space — where the controller actually is.
        float wx = (float)bp.x + m.cz.x * s_mWrist[i].z;
        float wy = (float)bp.y + m.cz.y * s_mWrist[i].z;
        float wz = (float)bp.z + m.cz.z * s_mWrist[i].z;

        b3Vec3 v = b3Body_GetLinearVelocity(s_mHand[i]);
        float fx = (s_mHandTarget[i].x - wx) * s_mPullK - v.x * s_mPullC;
        float fy = (s_mHandTarget[i].y - wy) * s_mPullK - v.y * s_mPullC;
        float fz = (s_mHandTarget[i].z - wz) * s_mPullK - v.z * s_mPullC;
        float sq = fx*fx + fy*fy + fz*fz;
        if (sq > s_mPullMax * s_mPullMax)
        {
            float k = s_mPullMax / __builtin_sqrtf(sq);
            fx *= k; fy *= k; fz *= k;
        }

        b3Vec3 f; f.x = fx; f.y = fy; f.z = fz;
        b3Pos at; at.x = wx; at.y = wy; at.z = wz;
        b3Body_ApplyForce(s_mHand[i], f, at, true);
    }
}

// 7 floats each for: torso, then upper/fore/hand for each arm — 49 total.
WASM_EXPORT("w_mech_state")
float* w_mech_state(void)
{
    static float out[7 * 7];
    if (!s_mExists) return out;
    b3BodyId ids[7] = { s_mTorso,
                        s_mUpper[0], s_mFore[0], s_mHand[0],
                        s_mUpper[1], s_mFore[1], s_mHand[1] };
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
    out[0] = s_mUpperLen; out[1] = s_mForeLen; out[2] = s_mHandHalf;
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
