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
//                                  last arg is half the shoulder width, measured
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
static b3BodyId s_groundBody;
static float s_groundY = 0.0f;
static int s_mExists = 0;
// The world is being torn down; forget every voxel grid that lived in it.
static void vxWorldReset(void);
static int s_dummyExists = 0;
static int s_eExists = 0;
static float s_eScale = 1.0f;      // body size multiplier
static float s_eTempo = 1.0f;      // how fast its will runs
static int s_eHover = 0;           // the God does not stand on the ground
static float s_eStandY = 1.15f;
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

    // Kept for the locomotion spike: planting a fist means jointing the arm's
    // tip to this body, so the world itself is what the machine hauls against.
    s_groundBody = ground;
    s_groundY = groundY;
    s_mExists = 0;
    s_dummyExists = 0;
    s_eExists = 0;
    vxWorldReset();
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
// How far the mount can turn away from the forearm before the joint stops it.
static float s_mWristCone = 1.35f;
static float s_mShoulderHalf = 0.20f;   // set from the player's measured span

// Every part of the machine carries this. It accepts collisions from everything
// except its own category, so the arms still hit blocks, the ground and another
// mech — they just cannot jam against their own torso.
#define MECH_CATEGORY 0x2ull
#define ENEMY_CATEGORY 0x4ull
#define MECH_FILTER ((b3Filter){ .categoryBits = MECH_CATEGORY, \
                                 .maskBits = ~MECH_CATEGORY, \
                                 .groupIndex = 0 })

static float s_mJointHertz = 4.0f;

// The actuator's stiffness, in newton metres per radian.
//
// Box3D's springs are mass-normalised by design: a 3 kg arm and a 26 kg arm
// given the same hertz track a punch identically, measured at 19.1 cm against
// 17.9 cm. That is correct for a spring and useless for this game, where the
// whole point is that a heavy arm is harder to swing.
//
// A real actuator has a torque, not a frequency. Holding stiffness fixed and
// solving k = I*w^2 for each joint's own inertia gives a heavy limb a lower
// natural frequency for free — which is exactly what being heavy means.
static float s_mActStiffness = 8000.0f;
// The elbow's spring pushes back on the upper arm, so the shoulder has to be
// the stiffer of the two or the pair settles on a compromise pose that is
// neither of their targets.
static float s_mShoulderStiffMul = 1.0f;
static void tuneJointSprings(void);
static float s_mJointDamping = 1.0f;
static b3Vec3 s_mIKElbow[MECH_ARMS];
static b3Vec3 s_mIKWrist[MECH_ARMS];
static b3Quat s_mShoulderFrame[MECH_ARMS];

// --- knuckle-walk locomotion -------------------------------------------------
//
// A mech with no legs moves the way a gorilla does: plant a fist, haul the body
// past it, swing, plant the other. Planting is a real joint between the arm's
// tip and the ground body, so the machine is genuinely hauling against the
// world — same IK, same springs, same mass. Pull your hand half a metre and the
// machine advances half a metre, minus lag, plus momentum. Nothing kinematic
// moves it.
static b3JointId s_mAnchorJ[MECH_ARMS];
static int s_mAnchored[MECH_ARMS];
static int s_mSlipped[MECH_ARMS];       // grip broke from over-reach, until re-grip
static int s_mCoastFrames = 0;          // how long the machine has been near-still
static int s_mDragMode = 0;             // 1 while anchored or still sliding

// A fist can only grab the ground when it is at the ground.
#define ANCHOR_REACH_Y 0.16f

// Whether the machine still has legs. Knuckle-hauling is the fallback for when
// it does not: with the legs gone the hull drops and lies on the ground, real
// contact friction resists the drag, and the shoulders sit low enough that the
// fists reach the floor. Standing at full height the fists cannot reach the
// ground at all — measured, the tip bottoms out 19 cm up even with the pilot
// crouching — which is the geometry telling us what this mechanic is for.
static int s_mLegsWork = 1;
static b3Quat s_mIKUpper[MECH_ARMS];
static b3Quat s_mIKSpringTarget[MECH_ARMS];
static b3Vec3 s_mHandTarget[MECH_ARMS];
static int s_mHandActive[MECH_ARMS] = {0, 0};

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
                   float elbowTorque, float coneAngle, float shoulderHalf)
{
    // How far out the shoulders sit. Measured off the player's own arm span
    // rather than assumed, so the machine's joints line up with theirs.
    if (!(shoulderHalf > 0.06f)) shoulderHalf = 0.20f;
    if (shoulderHalf > 0.40f) shoulderHalf = 0.40f;
    s_mShoulderHalf = shoulderHalf;

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
    // The machine does not collide with itself.
    //
    // This is what was pulling the arm off the pose IK asked for, and it hid
    // very well: with the torso pinned and the arm reaching down and away the
    // shoulder hit its target to within one degree, while a folded arm was 36
    // to 56 degrees out — worse the tighter it folded. No joint limit was
    // binding and no controller was fighting; the forearm and the mount were
    // simply pressing into the torso, and a contact can hold a spring off its
    // target all day. Only the shoulder pair had collision disabled, because
    // that is all collideConnected does.
    tsd.filter = MECH_FILTER;
    // Deliberately small. An earlier version was 0.52 x 0.60 x 0.32 m centred at
    // a fixed height, which swallowed the player's head.
    // Torso spans the shoulders, so it widens with the player too.
    b3BoxHull thull = b3MakeBoxHull(shoulderHalf * 0.92f, 0.22f, 0.13f);
    b3CreateHullShape(s_mTorso, &tsd, &thull.base);

    for (int i = 0; i < MECH_ARMS; i++)
    {
        const float side = (i == 0) ? -1.0f : 1.0f;
        const float sx = side * shoulderHalf;   // shoulder, in torso local space
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
        usd.filter = MECH_FILTER;
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
        s_mShoulderFrame[i] = sj.base.localFrameA.q;
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
        // The shoulder is driven by a spring toward the orientation IK asks
        // for. Box3D solves this implicitly, so unlike a torque applied from
        // outside it cannot be stiffened into instability — which is what put a
        // ceiling on the previous attempt and left the arm short of its pose.
        sj.enableSpring = true;
        sj.hertz = s_mJointHertz;
        sj.dampingRatio = s_mJointDamping;
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
        fsd.filter = MECH_FILTER;
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
        rj.enableSpring = true;
        rj.hertz = s_mJointHertz;
        rj.dampingRatio = s_mJointDamping;
        s_mElbow[i] = b3CreateRevoluteJoint(s_world, &rj);

        // --- attachment, on a wrist ---
        b3BodyDef kd = b3DefaultBodyDef();
        kd.type = b3_dynamicBody;
        kd.position.x = x + sx; kd.position.y = y + sy;
        kd.position.z = z - upperLen - foreLen;
        // The pull acts on the far face of this block, which torques it hard
        // against the wrist's cone limit — up to a few hundred newton metres.
        // Riding a hard stop with almost no rotational damping is what made the
        // mount buzz. This is the mount's own bearing friction.
        kd.angularDamping = 6.0f;
        s_mTool[i] = b3CreateBody(s_world, &kd);

        b3ShapeDef ksd = b3DefaultShapeDef();
        ksd.density = density;
        ksd.baseMaterial.friction = 0.8f;
        ksd.filter = MECH_FILTER;
        // The mount is part of the arm's reach, so an oversized block here
        // lengthens the whole limb. At 1.3x thickness it added 18 cm to an arm
        // that was already twice as long as it should be.
        const float toolHalf = thickness * 1.0f;
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
        // Measured pinned against this limit in every pose while the aim
        // controller kept pushing past it. A controller shoving against a hard
        // constraint chatters, which is the attachment buzzing. Widened so the
        // wrist has somewhere to go; the aim is also clamped to what the wrist
        // can actually reach, below, so the two stop fighting.
        wj.coneAngle = s_mWristCone;
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
    tuneJointSprings();
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
static float s_mAimPeriod = 0.35f;    // seconds — softened; 0.25 buzzed
static float s_mAimZeta = 1.0f;
static float s_mAimMax = 12.0f;       // N m per kg of attachment

// How much of the arm's own weight the machine carries. 1 is all of it, which
// is what a powered joint does; 0 is a dead limb hanging off a shoulder.
static float s_mArmLift = 1.0f;


// Which way round the elbow sits is no longer a controller fighting the
// physics — the IK solve picks it directly, straight down, and the joints are
// driven to that.

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

WASM_EXPORT("w_mech_joint_spring")
void w_mech_joint_spring(float stiffness, float damping)
{
    s_mActStiffness = stiffness; s_mJointDamping = damping;
    if (s_mExists) tuneJointSprings();
}

// Target orientation for each upper arm, and the relative rotation actually
// handed to the shoulder spring. Comparing these against what the body does
// separates "the spring is not reaching its target" from "the target is wrong".
// What the joints actually ended up with, read back from the engine rather
// than from what was asked for.
WASM_EXPORT("w_mech_spring_state")
float* w_mech_spring_state(void)
{
    static float out[4];
    if (!s_mExists) return out;
    out[0] = b3SphericalJoint_GetSpringHertz(s_mShoulder[0]);
    out[1] = b3SphericalJoint_GetSpringDampingRatio(s_mShoulder[0]);
    out[2] = b3RevoluteJoint_GetSpringHertz(s_mElbow[0]);
    out[3] = b3SphericalJoint_IsSpringEnabled(s_mShoulder[0]) ? 1.0f : 0.0f;
    return out;
}

WASM_EXPORT("w_mech_ik_quat")
float* w_mech_ik_quat(void)
{
    static float out[MECH_ARMS * 8];
    for (int i = 0; i < MECH_ARMS; i++)
    {
        out[i*8+0] = s_mIKUpper[i].v.x; out[i*8+1] = s_mIKUpper[i].v.y;
        out[i*8+2] = s_mIKUpper[i].v.z; out[i*8+3] = s_mIKUpper[i].s;
        out[i*8+4] = s_mIKSpringTarget[i].v.x; out[i*8+5] = s_mIKSpringTarget[i].v.y;
        out[i*8+6] = s_mIKSpringTarget[i].v.z; out[i*8+7] = s_mIKSpringTarget[i].s;
    }
    return out;
}

WASM_EXPORT("w_mech_shoulder_mul")
void w_mech_shoulder_mul(float mul)
{
    s_mShoulderStiffMul = mul;
    if (s_mExists) tuneJointSprings();
}

// Where IK wants the elbow and wrist, so a pose that is merely unreachable can
// be told apart from a pose that was solved wrongly.
WASM_EXPORT("w_mech_ik_target")
float* w_mech_ik_target(void)
{
    static float out[MECH_ARMS * 6];
    for (int i = 0; i < MECH_ARMS; i++)
    {
        out[i*6+0] = s_mIKElbow[i].x; out[i*6+1] = s_mIKElbow[i].y; out[i*6+2] = s_mIKElbow[i].z;
        out[i*6+3] = s_mIKWrist[i].x; out[i*6+4] = s_mIKWrist[i].y; out[i*6+5] = s_mIKWrist[i].z;
    }
    return out;
}

WASM_EXPORT("w_mech_lift")
void w_mech_lift(float fraction)
{
    s_mArmLift = fraction;
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

// ---------------------------------------------------------------------------
// Two-bone inverse kinematics for an arm
//
// Dragging the far end of a jointed chain with a force and hoping the joints
// work out the rest is not how anyone builds a VR arm, and this spike spent
// several rounds proving it: the elbow has no defined answer, the pose the
// player sees has no relationship to their own, and every fix was a new
// controller fighting the last one.
//
// Every VR body rig — VRIK, FinalIK, the Movement SDK — solves the arm
// analytically instead. A shoulder, an elbow and a wrist is the textbook
// two-bone problem: the distance from shoulder to wrist fixes the elbow's bend
// exactly by the law of cosines, and the one remaining freedom, where the elbow
// sits on its circle, is chosen deliberately rather than left to chance.
//
// What is *not* copied from those rigs is how the result is applied. They pose
// the skeleton directly, which would throw away the mass, the collisions and
// the momentum this whole game is built on. Here the solution is a target, and
// capped torques drive the real bodies toward it — so the machine still has to
// physically get there, a heavy arm still takes longer, and a punch still
// shoves the torso. That split is the point: IK decides the pose, physics
// decides what it costs.
// ---------------------------------------------------------------------------

typedef struct { float x, y, z; } V3;

static V3 v3(float x, float y, float z) { V3 r = {x, y, z}; return r; }
static V3 v3sub(V3 a, V3 b) { return v3(a.x-b.x, a.y-b.y, a.z-b.z); }
static V3 v3add(V3 a, V3 b) { return v3(a.x+b.x, a.y+b.y, a.z+b.z); }
static V3 v3mul(V3 a, float s) { return v3(a.x*s, a.y*s, a.z*s); }
static float v3dot(V3 a, V3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
static V3 v3cross(V3 a, V3 b) {
    return v3(a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x);
}
static float v3len(V3 a) { return __builtin_sqrtf(v3dot(a, a)); }
static V3 v3norm(V3 a) {
    const float l = v3len(a);
    return (l > 1e-6f) ? v3mul(a, 1.0f / l) : v3(0.0f, 0.0f, 1.0f);
}

// Orientation of a bone that runs along its own -Z from origin toward `tip`,
// hinging about `hinge`. Returned as a quaternion in world space.
static b3Quat boneQuat(V3 from, V3 tip, V3 hinge)
{
    const V3 zAxis = v3norm(v3sub(from, tip));     // bone extends along -Z
    V3 xAxis = v3sub(hinge, v3mul(zAxis, v3dot(hinge, zAxis)));
    if (v3len(xAxis) < 1e-4f)
    {
        // Hinge parallel to the bone: any perpendicular will do.
        V3 alt = (__builtin_fabsf(zAxis.y) < 0.9f) ? v3(0,1,0) : v3(1,0,0);
        xAxis = v3sub(alt, v3mul(zAxis, v3dot(alt, zAxis)));
    }
    xAxis = v3norm(xAxis);
    const V3 yAxis = v3cross(zAxis, xAxis);

    // Built by hand rather than through b3MakeQuatFromMatrix. Handing that
    // function a basis produced an orientation that left both the explicit
    // torque drive and the joint springs applying exactly zero — the arm sat at
    // its construction pose, to the millimetre, no matter how hard either was
    // pushed. A target that produces no error is a target equal to the current
    // rotation, which is what pointed here. This is Shepperd's method on the
    // columns, and it is unambiguous about which way round the basis goes.
    const float m00 = xAxis.x, m10 = xAxis.y, m20 = xAxis.z;
    const float m01 = yAxis.x, m11 = yAxis.y, m21 = yAxis.z;
    const float m02 = zAxis.x, m12 = zAxis.y, m22 = zAxis.z;

    b3Quat q;
    const float tr = m00 + m11 + m22;
    if (tr > 0.0f)
    {
        const float sc = __builtin_sqrtf(tr + 1.0f) * 2.0f;
        q.s = 0.25f * sc;
        q.v.x = (m21 - m12) / sc;
        q.v.y = (m02 - m20) / sc;
        q.v.z = (m10 - m01) / sc;
    }
    else if (m00 > m11 && m00 > m22)
    {
        const float sc = __builtin_sqrtf(1.0f + m00 - m11 - m22) * 2.0f;
        q.s = (m21 - m12) / sc;
        q.v.x = 0.25f * sc;
        q.v.y = (m01 + m10) / sc;
        q.v.z = (m02 + m20) / sc;
    }
    else if (m11 > m22)
    {
        const float sc = __builtin_sqrtf(1.0f + m11 - m00 - m22) * 2.0f;
        q.s = (m02 - m20) / sc;
        q.v.x = (m01 + m10) / sc;
        q.v.y = 0.25f * sc;
        q.v.z = (m12 + m21) / sc;
    }
    else
    {
        const float sc = __builtin_sqrtf(1.0f + m22 - m00 - m11) * 2.0f;
        q.s = (m10 - m01) / sc;
        q.v.x = (m02 + m20) / sc;
        q.v.y = (m12 + m21) / sc;
        q.v.z = 0.25f * sc;
    }
    return b3NormalizeQuat(q);
}

// Set each joint's spring rate from what it actually has to swing.
static void tuneJointSprings(void)
{
    for (int i = 0; i < MECH_ARMS; i++)
    {
        const float mU = b3Body_GetMass(s_mUpper[i]);
        const float mF = b3Body_GetMass(s_mFore[i]);
        const float mT = b3Body_GetMass(s_mTool[i]);
        const float L1 = s_mUpperLen, L2 = s_mForeLen;

        b3Matrix3 iu = b3Body_GetLocalRotationalInertia(s_mUpper[i]);
        b3Matrix3 ifo = b3Body_GetLocalRotationalInertia(s_mFore[i]);

        // The shoulder swings the whole arm; the elbow only swings what is
        // past it.
        float Ishoulder = iu.cx.x + mU * (L1*0.5f)*(L1*0.5f)
                        + mF * (L1 + L2*0.5f)*(L1 + L2*0.5f)
                        + mT * (L1 + L2)*(L1 + L2);
        float Ielbow = ifo.cx.x + mF * (L2*0.5f)*(L2*0.5f) + mT * L2*L2;
        if (Ishoulder < 1e-3f) Ishoulder = 1e-3f;
        if (Ielbow < 1e-3f) Ielbow = 1e-3f;

        float hz1 = __builtin_sqrtf(s_mActStiffness * s_mShoulderStiffMul / Ishoulder) / 6.2831853f;
        float hz2 = __builtin_sqrtf(s_mActStiffness / Ielbow) / 6.2831853f;
        // Below about 1.5 Hz the arm is unusable rather than heavy; above 60 it
        // is past what the step can resolve cleanly.
        if (hz1 < 1.5f) hz1 = 1.5f; if (hz1 > 60.0f) hz1 = 60.0f;
        if (hz2 < 1.5f) hz2 = 1.5f; if (hz2 > 60.0f) hz2 = 60.0f;

        b3SphericalJoint_SetSpringHertz(s_mShoulder[i], hz1);
        b3SphericalJoint_SetSpringDampingRatio(s_mShoulder[i], s_mJointDamping);
        b3RevoluteJoint_SetSpringHertz(s_mElbow[i], hz2);
        b3RevoluteJoint_SetSpringDampingRatio(s_mElbow[i], s_mJointDamping);
    }
}

// Solve arm i and drive its two bones toward the solution.
static void armIK(int i)
{
    b3WorldTransform tx = b3Body_GetTransform(s_mTorso);
    const b3Vec3 localShoulder = { (i == 0) ? -s_mShoulderHalf : s_mShoulderHalf, 0.17f, 0.0f };
    const b3Vec3 rotated = b3RotateVector(tx.q, localShoulder);
    const V3 S = v3((float)tx.p.x + rotated.x, (float)tx.p.y + rotated.y, (float)tx.p.z + rotated.z);

    // Where the end of the mount has to be, and which way it points.
    const V3 H = v3(s_mHandTarget[i].x, s_mHandTarget[i].y, s_mHandTarget[i].z);
    const b3Vec3 fwdLocal = { 0.0f, 0.0f, -1.0f };
    const b3Vec3 fwd = b3RotateVector(s_mToolAim[i], fwdLocal);
    const float toolLen = 2.0f * s_mToolHalf;

    // The wrist sits one mount-length back along the aim, so the far face of
    // the mount — not the wrist — is what lands on your hand.
    const V3 W = v3sub(H, v3mul(v3(fwd.x, fwd.y, fwd.z), toolLen));

    const float L1 = s_mUpperLen, L2 = s_mForeLen;
    V3 toW = v3sub(W, S);
    float d = v3len(toW);

    // Out of reach is normal — you can hold a controller further away than the
    // machine's arm goes. It stretches out toward you and stops, which is what
    // an arm does.
    const float dMin = __builtin_fabsf(L1 - L2) + 0.02f;
    const float dMax = L1 + L2 - 0.01f;
    if (d < dMin) d = dMin;
    if (d > dMax) d = dMax;
    const V3 dir = v3norm(toW);

    // Law of cosines: how far along the shoulder-to-wrist line the elbow sits,
    // and how far off it.
    const float a = (d*d + L1*L1 - L2*L2) / (2.0f * d);
    float hSq = L1*L1 - a*a;
    if (hSq < 0.0f) hSq = 0.0f;
    const float h = __builtin_sqrtf(hSq);

    // The one free choice in the whole solve: which way round the circle the
    // elbow sits. Straight down, which is where a person's elbow is, and the
    // reason it no longer ends up pointing at the ceiling.
    V3 pole = v3sub(v3(0.0f, -1.0f, 0.0f), v3mul(dir, v3dot(v3(0.0f, -1.0f, 0.0f), dir)));
    if (v3len(pole) < 1e-3f)
    {
        // Arm pointing straight up or down; use forward instead.
        pole = v3sub(v3(0.0f, 0.0f, -1.0f), v3mul(dir, v3dot(v3(0.0f, 0.0f, -1.0f), dir)));
    }
    pole = v3norm(pole);

    const V3 E = v3add(S, v3add(v3mul(dir, a), v3mul(pole, h)));

    // The elbow hinges about the normal to the bend plane.
    // Negated deliberately.
    //
    // cross(upper, forearm) points the way that makes the bend a *positive*
    // rotation about it, and the elbow's limits only allow negative — folding
    // is negative, straight is zero. Left unflipped the shoulder still aimed
    // correctly, so the elbow landed within a couple of centimetres, while the
    // forearm was asked to bend into its own hard stop and the wrist ended up
    // 38 cm out. The elbow being right while the wrist got worse as the spring
    // stiffened is what gave it away.
    V3 hinge = v3mul(v3cross(v3sub(E, S), v3sub(W, E)), -1.0f);
    if (v3len(hinge) < 1e-4f) hinge = v3cross(pole, dir);
    hinge = v3norm(hinge);

    s_mIKElbow[i].x = E.x; s_mIKElbow[i].y = E.y; s_mIKElbow[i].z = E.z;
    s_mIKWrist[i].x = W.x; s_mIKWrist[i].y = W.y; s_mIKWrist[i].z = W.z;

    const b3Quat qUpper = boneQuat(S, E, hinge);
    s_mIKUpper[i] = qUpper;
    const b3Quat qFore = boneQuat(E, W, hinge);

    // Hand the solution to the joints as spring targets.
    //
    // The shoulder spring works in joint-frame terms: frame B relative to frame
    // A, where A carries the tilt built into the shoulder mount, so the target
    // has to be expressed in that frame rather than in world.
    const b3Quat qFrameA = b3MulQuat(tx.q, s_mShoulderFrame[i]);
    s_mIKSpringTarget[i] = b3NormalizeQuat(b3MulQuat(b3Conjugate(qFrameA), qUpper));
    b3SphericalJoint_SetTargetRotation(s_mShoulder[i], s_mIKSpringTarget[i]);

    // The elbow only has one angle to be told about, and the law of cosines
    // already gave it: straight is zero, folded is negative.
    const float cosInterior = (L1*L1 + L2*L2 - d*d) / (2.0f * L1 * L2);
    const float interior = __builtin_acosf(cosInterior < -1.0f ? -1.0f
                                         : (cosInterior > 1.0f ? 1.0f : cosInterior));
    b3RevoluteJoint_SetTargetAngle(s_mElbow[i], interior - 3.14159265f);
}


// World position of arm i's far face — the knuckles.
static b3Vec3 mechTip(int i)
{
    b3Pos p = b3Body_GetPosition(s_mTool[i]);
    b3Quat q = b3Body_GetRotation(s_mTool[i]);
    b3Matrix3 m = b3MakeMatrixFromQuat(q);
    const float d = -2.0f * s_mToolHalf;
    b3Vec3 out;
    out.x = (float)p.x + m.cz.x * d;
    out.y = (float)p.y + m.cz.y * d;
    out.z = (float)p.z + m.cz.z * d;
    return out;
}

// Called every frame with the grip button's state. The engine decides whether a
// grab actually takes hold: squeezing in mid-air does nothing, because a fist
// with nothing under it has nothing to hold.
WASM_EXPORT("w_mech_legs")
void w_mech_legs(int working)
{
    s_mLegsWork = working ? 1 : 0;
}

WASM_EXPORT("w_mech_anchor")
void w_mech_anchor(int i, int wantAnchor)
{
    if (i < 0 || i >= MECH_ARMS || !s_mExists) return;

    if (!wantAnchor)
    {
        if (s_mAnchored[i])
        {
            b3DestroyJoint(s_mAnchorJ[i], true);
            s_mAnchored[i] = 0;
        }
        s_mSlipped[i] = 0;   // opening the hand resets a slipped grip
        return;
    }

    if (s_mAnchored[i] || s_mSlipped[i]) return;

    b3Vec3 tip = mechTip(i);
    if (tip.y > s_groundY + ANCHOR_REACH_Y) return;   // not at the ground

    // A planted fist is a pivot: the tip cannot translate, the arm can still
    // rotate about it. Grounded fists really do work like this.
    b3Pos gp = b3Body_GetPosition(s_groundBody);
    b3SphericalJointDef aj = b3DefaultSphericalJointDef();
    aj.base.bodyIdA = s_groundBody;
    aj.base.bodyIdB = s_mTool[i];
    aj.base.localFrameA = b3Transform_identity;
    aj.base.localFrameA.p.x = tip.x - (float)gp.x;
    aj.base.localFrameA.p.y = tip.y - (float)gp.y;
    aj.base.localFrameA.p.z = tip.z - (float)gp.z;
    aj.base.localFrameB = b3Transform_identity;
    aj.base.localFrameB.p.z = -2.0f * s_mToolHalf;
    aj.base.collideConnected = false;
    s_mAnchorJ[i] = b3CreateSphericalJoint(s_world, &aj);
    s_mAnchored[i] = 1;
}

// [anchoredL, anchoredR, slippedL, slippedR, dragMode, speed]
WASM_EXPORT("w_mech_drag_state")
float* w_mech_drag_state(void)
{
    static float out[6];
    out[0] = (float)s_mAnchored[0];
    out[1] = (float)s_mAnchored[1];
    out[2] = (float)s_mSlipped[0];
    out[3] = (float)s_mSlipped[1];
    out[4] = (float)s_mDragMode;
    if (s_mExists)
    {
        b3Vec3 v = b3Body_GetLinearVelocity(s_mTorso);
        out[5] = __builtin_sqrtf(v.x*v.x + v.z*v.z);
    }
    return out;
}

WASM_EXPORT("w_mech_apply")
void w_mech_apply(void)
{
    if (!s_mExists) return;

    // Drag mode.
    //
    // While a fist is planted — and while the machine is still sliding after
    // the last one let go — the feet give way and go along instead of holding
    // their spot, exactly as knuckle-walking needs. It ends only when the
    // machine has actually come to rest, so releasing mid-haul lets momentum
    // carry it and the feet catch the stop at the end.
    {
        const int anyAnchor = s_mAnchored[0] || s_mAnchored[1];
        b3Vec3 tv = b3Body_GetLinearVelocity(s_mTorso);
        const float speed = __builtin_sqrtf(tv.x*tv.x + tv.z*tv.z);
        if (anyAnchor)
        {
            s_mDragMode = 1;
            s_mCoastFrames = 0;
        }
        else if (s_mDragMode)
        {
            if (speed < 0.10f) s_mCoastFrames++;
            else s_mCoastFrames = 0;
            if (s_mCoastFrames > 10) s_mDragMode = 0;   // planted again
        }

        // An over-stretched grip breaks. Without this, walking away from a
        // planted fist loads an unbounded joint spring against a hard anchor,
        // and the arm ends up violently rubber-banded to a spot on the floor.
        for (int i = 0; i < MECH_ARMS; i++)
        {
            if (!s_mAnchored[i]) continue;
            b3WorldTransform tx0 = b3Body_GetTransform(s_mTorso);
            const b3Vec3 ls = { (i == 0) ? -s_mShoulderHalf : s_mShoulderHalf, 0.17f, 0.0f };
            const b3Vec3 r = b3RotateVector(tx0.q, ls);
            b3Vec3 tip = mechTip(i);
            const float dx = tip.x - ((float)tx0.p.x + r.x);
            const float dy = tip.y - ((float)tx0.p.y + r.y);
            const float dz = tip.z - ((float)tx0.p.z + r.z);
            const float reach = s_mUpperLen + s_mForeLen + 2.0f * s_mToolHalf;
            if (__builtin_sqrtf(dx*dx + dy*dy + dz*dz) > reach * 0.97f)
            {
                b3DestroyJoint(s_mAnchorJ[i], true);
                s_mAnchored[i] = 0;
                s_mSlipped[i] = 1;
            }
        }
    }

    // Legs.
    //
    // Only while the machine still has them. With the legs gone no leg force
    // exists at all — the hull falls, rests on the ground, and everything the
    // machine does from then on is done against real contact friction.
    if (s_mLegsWork)
    {
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

        // In drag mode the feet stop holding a spot: the spring term goes and
        // only the damping stays, which is the drag of the feet over the
        // ground. That resistance is also what brings the machine to rest when
        // the fist lets go, so momentum ends in a slide rather than a glide.
        float fx, fz;
        if (s_mDragMode)
        {
            fx = -v.x * s_mTorsoC * 0.6f;
            fz = -v.z * s_mTorsoC * 0.6f;
        }
        else
        {
            fx = (s_mTorsoTarget.x - (float)p.x) * s_mTorsoK - v.x * s_mTorsoC;
            fz = (s_mTorsoTarget.z - (float)p.z) * s_mTorsoK - v.z * s_mTorsoC;
        }

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

    // Arms.
    //
    // Each arm is solved as a two-bone IK problem and then physically driven to
    // that solution. Nothing here drags the hand end and hopes: the pose is
    // worked out first, the bodies have to earn their way there.
    for (int i = 0; i < MECH_ARMS; i++)
    {
        // The machine carries its own arm, so the drive torque below is spent
        // moving the limb rather than holding it up against gravity.
        {
            b3BodyId parts[3] = { s_mUpper[i], s_mFore[i], s_mTool[i] };
            for (int k = 0; k < 3; k++)
            {
                b3Vec3 lift;
                lift.x = 0.0f;
                lift.y = b3Body_GetMass(parts[k]) * 9.81f * s_mArmLift;
                lift.z = 0.0f;
                b3Body_ApplyForceToCenter(parts[k], lift, true);
            }
        }

        if (!s_mHandActive[i]) continue;

        armIK(i);

        // Aim. The attachment turns to face where the controller faces, which is
        // what lets a sword be pointed rather than merely carried. Torque-based
        // so it can be resisted or knocked off line.
        {
            // Shortest rotation from current to target, as a quaternion.
            b3Quat cur = b3Body_GetRotation(s_mTool[i]);
            b3Quat tgt = s_mToolAim[i];

            // Never ask the wrist for an angle it cannot hold.
            //
            // The target is your controller's orientation, and it is often well
            // outside the wrist's cone — measured, the joint was jammed hard
            // against that limit in every single frame while the aim kept
            // pushing further. A controller shoving at a hard constraint is a
            // fight neither side wins, and it chatters: that is the mount
            // spasming. Folding the request back to the edge of what the joint
            // can reach ends the fight rather than damping it.
            {
                b3Matrix3 fm = b3MakeMatrixFromQuat(b3Body_GetRotation(s_mFore[i]));
                b3Matrix3 tm = b3MakeMatrixFromQuat(tgt);
                float d = fm.cz.x*tm.cz.x + fm.cz.y*tm.cz.y + fm.cz.z*tm.cz.z;
                if (d < -1.0f) d = -1.0f;
                if (d > 1.0f) d = 1.0f;
                const float ang = __builtin_acosf(d);
                const float room = s_mWristCone * 0.9f;   // stop shy of the stop
                if (ang > room)
                {
                    float ax[3] = {
                        fm.cz.y*tm.cz.z - fm.cz.z*tm.cz.y,
                        fm.cz.z*tm.cz.x - fm.cz.x*tm.cz.z,
                        fm.cz.x*tm.cz.y - fm.cz.y*tm.cz.x };
                    const float al = __builtin_sqrtf(ax[0]*ax[0] + ax[1]*ax[1] + ax[2]*ax[2]);
                    if (al > 1e-4f)
                    {
                        ax[0] /= al; ax[1] /= al; ax[2] /= al;
                        const float h = -(ang - room) * 0.5f;
                        const float sh = __builtin_sinf(h);
                        b3Quat qc;
                        qc.v.x = ax[0]*sh; qc.v.y = ax[1]*sh; qc.v.z = ax[2]*sh;
                        qc.s = __builtin_cosf(h);
                        tgt = b3MulQuat(qc, tgt);
                    }
                }
            }

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

#define VOX_GRIDS 8
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
    { 7800.0f, 26.0f, 2.0f, 4.0f },
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
} VoxGrid;
static VoxGrid s_vxG[VOX_GRIDS];
static int s_vxLastHits = 0;

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
    if (g->category)
    {
        // Armour belongs to a team: its own machine's fists and clubs pass
        // through it, everyone else's connect. Without this the player's own
        // punches battered their own chest plate on the way to the enemy.
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
    g->local = local;
    g->body = body;
    g->alive = nx * ny * nz;
    g->killed = 0;

    const float hp = VOX_MATS[material].hp;
    for (int i = 0; i < nx * ny * nz; i++) g->hp[i] = hp;
    for (int r = 0; r < ny * nz; r++) { g->rowShapes[r] = 0; g->rowDirty[r] = 0; }

    for (int z = 0; z < nz; z++)
        for (int y = 0; y < ny; y++)
            vxMeshRow(g, y, z);

    // Anything slower than a shove should not chip a wall.
    b3World_SetHitEventThreshold(s_world, 1.0f);
    return (int)(g - s_vxG);
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
    int killedBefore[VOX_GRIDS];
    for (int i = 0; i < VOX_GRIDS; i++) killedBefore[i] = s_vxG[i].killed;

    for (int i = 0; i < ev.hitCount; i++)
    {
        const b3ContactHitEvent* h = &ev.hitEvents[i];
        b3BodyId bodyA = b3Shape_GetBody(h->shapeIdA);
        b3BodyId bodyB = b3Shape_GetBody(h->shapeIdB);

        VoxGrid* g = 0;
        b3BodyId other;
        for (int k = 0; k < VOX_GRIDS; k++)
        {
            if (!s_vxG[k].used) continue;
            if (B3_ID_EQUALS(bodyA, s_vxG[k].body)) { g = &s_vxG[k]; other = bodyB; break; }
            if (B3_ID_EQUALS(bodyB, s_vxG[k].body)) { g = &s_vxG[k]; other = bodyA; break; }
        }
        if (!g) continue;

        float mass = b3Body_GetMass(other);
        if (mass <= 0.0f) continue;

        // A fist does not arrive alone: the whole arm is rigidly driving it,
        // so the impactor's effective mass is the arm's, not the little block
        // on the end. Without this a committed heavy punch chipped one cell
        // where it should crater.
        if (s_mExists)
        {
            for (int a = 0; a < MECH_ARMS; a++)
            {
                if (B3_ID_EQUALS(other, s_mTool[a]) || B3_ID_EQUALS(other, s_mFore[a]))
                {
                    mass = b3Body_GetMass(s_mUpper[a]) + b3Body_GetMass(s_mFore[a])
                         + b3Body_GetMass(s_mTool[a]);
                    break;
                }
            }
        }

        s_vxLastHits++;
        vxDamageAt(g, (float)h->point.x, (float)h->point.y, (float)h->point.z,
                   h->approachSpeed * mass);
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
        const float maxHp = VOX_MATS[g->material].hp;
        for (int z = 0; z < g->n[2]; z++)
        for (int y = 0; y < g->n[1]; y++)
        {
            int x = 0;
            while (x < g->n[0])
            {
                if (!vxAliveAt(g, x, y, z)) { x++; continue; }
                const int hurt0 = g->hp[vxIdx(g, x, y, z)] < 0.5f * maxHp;
                int x0 = x;
                while (x < g->n[0] && vxAliveAt(g, x, y, z) &&
                       (g->hp[vxIdx(g, x, y, z)] < 0.5f * maxHp) == hurt0) x++;
                if (s_vxRunCount >= cap) return;
                float* o = &s_vxRunBuf[s_vxRunCount * 12];
                const int len = x - x0;
                b3Vec3 c;
                c.x = g->local.x + (x0 + 0.5f * len) * g->size;
                c.y = g->local.y + (y + 0.5f) * g->size;
                c.z = g->local.z + (z + 0.5f) * g->size;
                const b3Vec3 wc = b3TransformPoint(t, c);
                o[0] = wc.x; o[1] = wc.y; o[2] = wc.z;
                o[3] = t.q.v.x; o[4] = t.q.v.y; o[5] = t.q.v.z; o[6] = t.q.s;
                o[7] = len * half; o[8] = half; o[9] = half;
                o[10] = hurt0 ? 1.0f : 0.0f;
                o[11] = VOX_MATS[g->material].colorIdx;
                s_vxRunCount++;
            }
        }
    }
}

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

// Per-grid: [alive, killed, total] — the page reports each wall separately.
// The player's own plate is worn a hand-span from their eyes: real and
// damageable, but drawn in their face it is a wall across the bottom of the
// view. Hidden grids keep simulating and taking hits; they just stay out of
// the draw list, and the page shows their state as an instrument instead.
// Restore every surviving structure of a grid to full health and refill the
// holes — the repair pad's other half.
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

WASM_EXPORT("w_vox_hide")
void w_vox_hide(int grid, int hide)
{
    if (grid < 0 || grid >= VOX_GRIDS) return;
    s_vxG[grid].hidden = hide ? 1 : 0;
}

WASM_EXPORT("w_vox_grid_stats")
float* w_vox_grid_stats(int grid)
{
    static float out[3];
    out[0] = out[1] = out[2] = 0.0f;
    if (grid < 0 || grid >= VOX_GRIDS || !s_vxG[grid].used) return out;
    const VoxGrid* g = &s_vxG[grid];
    out[0] = (float)g->alive;
    out[1] = (float)g->killed;
    out[2] = (float)(g->n[0] * g->n[1] * g->n[2]);
    return out;
}


// ---------------------------------------------------------------------------
// The dummy — armour worn by something that moves
//
// A heavy core hangs from a pivot like a punching bag, wearing a plate of
// voxel armour on its front face. Punch it and the whole body swings; the
// plate cracks, sheds cells, and slabs of it tear off mid-swing, flying with
// the body's own velocity. This is the fight in miniature: strip the armour,
// then hit the thing inside.
// ---------------------------------------------------------------------------

static b3BodyId s_dummyCore;

WASM_EXPORT("w_dummy_create")
void w_dummy_create(float x, float z, int material)
{
    // The gantry the bag hangs from.
    b3BodyDef gd = b3DefaultBodyDef();
    gd.type = b3_staticBody;
    gd.position.x = x; gd.position.y = 2.05f; gd.position.z = z;
    b3BodyId gantry = b3CreateBody(s_world, &gd);

    // The core: heavy, bare metal underneath.
    b3BodyDef cd = b3DefaultBodyDef();
    cd.type = b3_dynamicBody;
    cd.position.x = x; cd.position.y = 1.15f; cd.position.z = z;
    cd.angularDamping = 0.8f;
    cd.linearDamping = 0.2f;
    s_dummyCore = b3CreateBody(s_world, &cd);

    b3ShapeDef csd = b3DefaultShapeDef();
    csd.density = 380.0f;   // light enough that punches visibly swing it
    csd.baseMaterial.friction = 0.6f;
    b3BoxHull chull = b3MakeBoxHull(0.28f, 0.42f, 0.12f);
    b3CreateHullShape(s_dummyCore, &csd, &chull.base);

    // The rope: a pivot at the gantry.
    b3SphericalJointDef sj = b3DefaultSphericalJointDef();
    sj.base.bodyIdA = gantry;
    sj.base.bodyIdB = s_dummyCore;
    sj.base.localFrameA = b3Transform_identity;
    sj.base.localFrameB = b3Transform_identity;
    sj.base.localFrameB.p.y = 0.90f;      // hangs from above its head
    sj.base.collideConnected = false;
    b3CreateSphericalJoint(s_world, &sj);

    // The armour: an 8 x 8 plate, one cell thick, bolted to the face toward
    // the player (+z side — the fists arrive travelling -z). Anchor axis z:
    // the layer against the core is the foundation, so cells that lose their
    // connection to the body fall off it, wherever it is swinging.
    b3Vec3 local = { -0.28f, -0.28f, 0.12f };
    vxBuild(s_dummyCore, local, 8, 8, 1, 0.07f, material, 2, 0);
    s_dummyExists = 1;
}

// [px,py,pz, qx,qy,qz,qw, hx,hy,hz] — so the page can draw the bare core.
WASM_EXPORT("w_dummy_state")
float* w_dummy_state(void)
{
    static float out[10];
    if (!s_dummyExists) return out;
    b3Pos p = b3Body_GetPosition(s_dummyCore);
    b3Quat q = b3Body_GetRotation(s_dummyCore);
    out[0] = (float)p.x; out[1] = (float)p.y; out[2] = (float)p.z;
    out[3] = q.v.x; out[4] = q.v.y; out[5] = q.v.z; out[6] = q.s;
    out[7] = 0.28f; out[8] = 0.42f; out[9] = 0.12f;
    return out;
}

// ---------------------------------------------------------------------------
// The enemy — the other machine in the arena
//
// Everything it is made of already exists: a torso held up by the same
// gravity-compensated legs and inertia-derived uprighting the player's
// machine uses, voxel armour riding its body through the grid system, and
// punches that are momentum like every other impact in the world.
//
// What is new is only the will: a small state machine that walks it toward
// the player, squares it up, telegraphs, and swings a club arm through the
// player's torso. Strip its plate and the core underneath takes the hits;
// enough of them and the drives cut out — the machine drops, and what is
// left of its armour bursts off.
// ---------------------------------------------------------------------------

typedef enum { E_IDLE = 0, E_APPROACH, E_WINDUP, E_SWING, E_RECOVER, E_DEAD } EnemyState;

static b3BodyId s_eTorso;
static b3BodyId s_eArm[2];
static b3JointId s_eShoulder[2];
static EnemyState s_eState = E_IDLE;
static float s_eTimer = 0.0f;
static int s_eSwingArm = 0;
static float s_eCoreHp = 0.0f;
static float s_eCoreHpMax = 260.0f;
static int s_eGrid = -1;                  // its armour plate
static b3Vec3 s_ePlayerPos;               // told by the page each frame
static int s_ePlayerPlateGrid = -1;
static float s_eHitPlayerCore = 0.0f;     // momentum landed on the bare player torso
static float s_eLastCoreHit = 0.0f;       // for the page's sound hooks
static float s_eLastPlayerHit = 0.0f;

// Height the enemy stands at, and how far away it wants to fight from.
#define ENEMY_STAND_Y s_eStandY
// Close enough that both machines' arms genuinely reach each other: the
// player's fist reaches 0.68 m and the enemy's club 0.60, so at 0.72 m
// centre-to-centre the fight actually connects both ways. The first probe had
// this at 1.05 and every player punch whiffed short.
#define ENEMY_RANGE 0.72f

static int w_enemy_create_inner(float x, float z, int material);

// The full-fat constructor: size scales the whole machine, tempo speeds its
// decisions, coreHp sets how much punishment the bared core takes, and hover
// floats it — the last boss does not deign to stand.
WASM_EXPORT("w_enemy_create_ex")
int w_enemy_create_ex(float x, float z, int material, float scale,
                      float coreHp, int hover, float tempo)
{
    s_eScale = scale < 0.5f ? 0.5f : (scale > 2.0f ? 2.0f : scale);
    s_eTempo = tempo < 0.4f ? 0.4f : (tempo > 2.5f ? 2.5f : tempo);
    s_eHover = hover ? 1 : 0;
    s_eCoreHpMax = coreHp > 20.0f ? coreHp : 260.0f;
    s_eStandY = s_eHover ? 1.45f : 1.15f * (0.7f + 0.3f * s_eScale);
    return w_enemy_create_inner(x, z, material);
}

WASM_EXPORT("w_enemy_create")
int w_enemy_create(float x, float z, int material)
{
    s_eScale = 1.0f; s_eTempo = 1.0f; s_eHover = 0;
    s_eCoreHpMax = 260.0f; s_eStandY = 1.15f;
    return w_enemy_create_inner(x, z, material);
}

static int w_enemy_create_inner(float x, float z, int material)
{
    b3BodyDef td = b3DefaultBodyDef();
    td.type = b3_dynamicBody;
    td.position.x = x; td.position.y = ENEMY_STAND_Y; td.position.z = z;
    td.angularDamping = 4.0f;
    td.linearDamping = 0.8f;
    s_eTorso = b3CreateBody(s_world, &td);

    b3ShapeDef tsd = b3DefaultShapeDef();
    tsd.density = 700.0f;
    tsd.baseMaterial.friction = 0.5f;
    tsd.enableHitEvents = true;           // core hits are how it dies
    tsd.filter.categoryBits = ENEMY_CATEGORY;
    tsd.filter.maskBits = ~ENEMY_CATEGORY;
    const float sc = s_eScale;
    b3BoxHull thull = b3MakeBoxHull(0.24f * sc, 0.34f * sc, 0.14f * sc);
    b3CreateHullShape(s_eTorso, &tsd, &thull.base);

    // Club arms: single rigid limbs on spherical joints with spring drives.
    // Not the player's two-bone rig — an opponent reads through its swings,
    // not its elbows, and clubs keep the whole machine cheap.
    for (int i = 0; i < 2; i++)
    {
        const float side = i == 0 ? -1.0f : 1.0f;
        b3BodyDef ad = b3DefaultBodyDef();
        ad.type = b3_dynamicBody;
        ad.position.x = x + side * 0.32f * sc;
        ad.position.y = ENEMY_STAND_Y + 0.10f * sc;
        ad.position.z = z;
        ad.angularDamping = 1.0f;
        s_eArm[i] = b3CreateBody(s_world, &ad);

        b3ShapeDef asd = b3DefaultShapeDef();
        asd.density = 1100.0f;
        asd.baseMaterial.friction = 0.5f;
        asd.enableHitEvents = true;
        asd.filter.categoryBits = ENEMY_CATEGORY;
        asd.filter.maskBits = ~ENEMY_CATEGORY;
        b3BoxHull ahull = b3MakeBoxHull(0.07f * sc, 0.07f * sc, 0.30f * sc);
        b3Transform aoff = b3Transform_identity;
        // The club extends along its body's +z. The cone limit constrains the
        // club's +z to stay near the torso's +z (the facing direction), so the
        // shape must extend along +z or the limit and the shape fight: hung
        // along -z, pointing the club at the player put its +z at 180 degrees
        // from the cone axis — maximally outside — and the spring spent every
        // frame pinned sideways against the limit. That was the 56 m/s of
        // chatter and every swing whiffing half a metre short.
        aoff.p.z = 0.30f * sc;
        b3Vec3 one = { 1.0f, 1.0f, 1.0f };
        b3CreateTransformedHullShape(s_eArm[i], &asd, &ahull.base, aoff, one);

        b3SphericalJointDef sj = b3DefaultSphericalJointDef();
        sj.base.bodyIdA = s_eTorso;
        sj.base.bodyIdB = s_eArm[i];
        sj.base.localFrameA = b3Transform_identity;
        sj.base.localFrameA.p.x = side * 0.32f * sc;
        sj.base.localFrameA.p.y = 0.10f * sc;
        sj.base.localFrameB = b3Transform_identity;
        sj.base.collideConnected = false;
        sj.enableConeLimit = true;
        sj.coneAngle = 1.5f;
        sj.enableSpring = true;
        sj.hertz = 3.0f;
        sj.dampingRatio = 0.8f;
        s_eShoulder[i] = b3CreateSphericalJoint(s_world, &sj);
    }

    // Its armour: a plate over the chest, facing the player side (+z).
    b3Vec3 local = { -0.245f * sc, -0.245f * sc, 0.14f * sc };
    s_eGrid = vxBuild(s_eTorso, local, 7, 7, 1, 0.07f * sc, material, 2, ENEMY_CATEGORY);

    s_eCoreHp = s_eCoreHpMax;
    s_eState = E_APPROACH;
    s_eTimer = 0.0f;
    s_eSwingArm = 0;
    s_eHitPlayerCore = 0.0f;
    s_eExists = 1;
    return s_eGrid;
}

// The player's machine wears a plate too, once there is someone to hit back.
WASM_EXPORT("w_player_plate")
int w_player_plate(int material)
{
    if (!s_mExists) return -1;
    // The player's machine faces -z (the arms hang that way), so its chest —
    // and its plate — face the enemy at -z. The first cut bolted it on the
    // player's back.
    b3Vec3 local = { -0.21f, -0.21f, -0.17f };
    s_ePlayerPlateGrid = vxBuild(s_mTorso, local, 6, 6, 1, 0.07f, material, 2, MECH_CATEGORY);
    return s_ePlayerPlateGrid;
}

// Aim a shoulder spring so the club points from the shoulder toward a world
// target — the swing is the spring chasing a target driven through the player.
static void eAimArm(int i, b3Vec3 worldTarget, float hertz)
{
    b3WorldTransform tt = b3Body_GetTransform(s_eTorso);
    const float side = i == 0 ? -1.0f : 1.0f;
    b3Vec3 shoulderLocal = { side * 0.32f, 0.10f, 0.0f };
    b3Vec3 sw = b3RotateVector(tt.q, shoulderLocal);
    sw.x += (float)tt.p.x; sw.y += (float)tt.p.y; sw.z += (float)tt.p.z;

    b3Vec3 d = { worldTarget.x - sw.x, worldTarget.y - sw.y, worldTarget.z - sw.z };
    const float len = __builtin_sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
    if (len < 1e-4f) return;
    d.x /= len; d.y /= len; d.z /= len;

    // World orientation whose +Z points along d — the club extends along +z,
    // matching the cone. boneQuat points -Z at its tip argument, so hand it
    // the point BEHIND the shoulder.
    V3 from = { sw.x, sw.y, sw.z };
    V3 back = { sw.x - d.x, sw.y - d.y, sw.z - d.z };
    V3 hingeGuess = { 1.0f, 0.0f, 0.0f };
    b3Quat qWorld = boneQuat(from, back, hingeGuess);

    // Spring target is frame B relative to frame A (torso frame, identity).
    b3Quat rel = b3NormalizeQuat(b3MulQuat(b3Conjugate(tt.q), qWorld));
    b3SphericalJoint_SetTargetRotation(s_eShoulder[i], rel);
    b3SphericalJoint_SetSpringHertz(s_eShoulder[i], hertz);
}

// Once per frame, before w_step. dt at 72 Hz.
WASM_EXPORT("w_enemy_update")
void w_enemy_update(float px, float py, float pz, float dt)
{
    if (!s_eExists) return;
    s_ePlayerPos.x = px; s_ePlayerPos.y = py; s_ePlayerPos.z = pz;
    if (s_eState == E_DEAD) return;

    b3Pos tp = b3Body_GetPosition(s_eTorso);
    b3Vec3 tv = b3Body_GetLinearVelocity(s_eTorso);
    const float mass = b3Body_GetMass(s_eTorso);

    // Legs: hold height, close to fighting range, stop there.
    {
        b3Vec3 toP = { px - (float)tp.x, 0.0f, pz - (float)tp.z };
        const float dist = __builtin_sqrtf(toP.x*toP.x + toP.z*toP.z);
        float gx = (float)tp.x, gz = (float)tp.z;
        const float wantRange = ENEMY_RANGE + (s_eScale - 1.0f) * 0.35f;
        if (dist > wantRange)
        {
            const float step = dist - wantRange;
            gx += toP.x / dist * step;
            gz += toP.z / dist * step;
        }
        float fy = mass * 9.81f + (ENEMY_STAND_Y - (float)tp.y) * 5200.0f - tv.y * 900.0f;
        float fx = (gx - (float)tp.x) * 1400.0f - tv.x * 620.0f;
        float fz = (gz - (float)tp.z) * 1400.0f - tv.z * 620.0f;
        const float cap = 2600.0f;
        const float hsq = fx*fx + fz*fz;
        if (hsq > cap * cap) { const float k = cap / __builtin_sqrtf(hsq); fx *= k; fz *= k; }
        b3Vec3 f = { fx, fy, fz };
        b3Body_ApplyForceToCenter(s_eTorso, f, true);
    }

    // Stay upright, and turn to face the player (so the armour faces the fight).
    {
        b3Quat q = b3Body_GetRotation(s_eTorso);
        b3Matrix3 m = b3MakeMatrixFromQuat(q);
        b3Matrix3 Ib = b3Body_GetLocalRotationalInertia(s_eTorso);
        const float I = (Ib.cx.x + Ib.cy.y + Ib.cz.z) / 3.0f;
        const float w0 = 6.2831853f / 0.6f;
        const float k = I * w0 * w0, c = 2.0f * I * w0;
        b3Vec3 w = b3Body_GetAngularVelocity(s_eTorso);

        // Pitch/roll: local up toward world up.
        float tx = -m.cy.z * k - w.x * c;
        float tz = m.cy.x * k - w.z * c;

        // Yaw: local +z toward the player bearing.
        b3Vec3 toP = { px - (float)tp.x, 0.0f, pz - (float)tp.z };
        const float dl = __builtin_sqrtf(toP.x*toP.x + toP.z*toP.z);
        float ty = -w.y * c;
        if (dl > 0.05f)
        {
            const float bearing = __builtin_atan2f(toP.x / dl, toP.z / dl);
            const float facing = __builtin_atan2f(m.cz.x, m.cz.z);
            float err = bearing - facing;
            while (err > 3.14159265f) err -= 6.2831853f;
            while (err < -3.14159265f) err += 6.2831853f;
            ty += err * k * 0.6f;
        }
        const float cap = 60.0f * mass;
        const float tsq = tx*tx + ty*ty + tz*tz;
        if (tsq > cap * cap) { const float s = cap / __builtin_sqrtf(tsq); tx *= s; ty *= s; tz *= s; }
        b3Vec3 t = { tx, ty, tz };
        b3Body_ApplyTorque(s_eTorso, t, true);
    }

    // The will.
    //
    // Every target the clubs are given sits comfortably INSIDE the shoulder
    // cone. The first cut pointed the windup straight up — exactly on the cone
    // edge — and the spring chattered against the limit at 55 m/s of tip speed
    // while the swing never got within half a metre of the player. Measured,
    // not guessed: the probe tracked the tip.
    s_eTimer += dt * s_eTempo;
    b3Vec3 toP = { px - (float)tp.x, py - (float)tp.y, pz - (float)tp.z };
    const float pl = __builtin_sqrtf(toP.x*toP.x + toP.y*toP.y + toP.z*toP.z);
    b3Vec3 pHat = { 1.0f, 0.0f, 0.0f };
    if (pl > 1e-3f) { pHat.x = toP.x / pl; pHat.y = toP.y / pl; pHat.z = toP.z / pl; }
    b3Vec3 guard = { px, py + 0.10f, pz };
    switch (s_eState)
    {
        case E_APPROACH:
        {
            eAimArm(0, guard, 2.5f);
            eAimArm(1, guard, 2.5f);
            const float reach = ENEMY_RANGE + (s_eScale - 1.0f) * 0.35f;
            const float d2 = toP.x*toP.x + toP.z*toP.z;
            if (d2 < (reach + 0.15f) * (reach + 0.15f) && s_eTimer > 0.8f)
            {
                s_eState = E_WINDUP; s_eTimer = 0.0f;
                s_eSwingArm = 1 - s_eSwingArm;
            }
            break;
        }
        case E_WINDUP:
        {
            // The telegraph: drawn up and pulled back, ~55 degrees off the
            // cone axis — clearly readable, comfortably inside the limit.
            b3Vec3 up = { (float)tp.x - pHat.x * 0.35f,
                          (float)tp.y + 0.80f,
                          (float)tp.z - pHat.z * 0.35f };
            eAimArm(s_eSwingArm, up, 5.0f);
            eAimArm(1 - s_eSwingArm, guard, 2.5f);
            if (s_eTimer > 0.45f) { s_eState = E_SWING; s_eTimer = 0.0f; }
            break;
        }
        case E_SWING:
        {
            // Driven THROUGH the chest, not at it: the target sits 0.35 m past
            // the player, so the club's equilibrium is inside them and contact
            // is guaranteed rather than grazed.
            b3Vec3 through = { px + pHat.x * 0.45f,
                               py - 0.05f,
                               pz + pHat.z * 0.45f };
            eAimArm(s_eSwingArm, through, 10.0f);
            if (s_eTimer > 0.40f) { s_eState = E_RECOVER; s_eTimer = 0.0f; }
            break;
        }
        case E_RECOVER:
        {
            eAimArm(0, guard, 2.0f);
            eAimArm(1, guard, 2.0f);
            if (s_eTimer > 0.55f) { s_eState = E_APPROACH; s_eTimer = 0.4f; }
            break;
        }
        default: break;
    }
}

// After w_step: read the step's impacts for the two cores.
WASM_EXPORT("w_enemy_post")
void w_enemy_post(void)
{
    if (!s_eExists) return;
    s_eLastCoreHit = 0.0f;
    s_eLastPlayerHit = 0.0f;
    if (s_eState == E_DEAD) return;

    b3ContactEvents ev = b3World_GetContactEvents(s_world);
    for (int i = 0; i < ev.hitCount; i++)
    {
        const b3ContactHitEvent* h = &ev.hitEvents[i];
        b3BodyId a = b3Shape_GetBody(h->shapeIdA);
        b3BodyId b = b3Shape_GetBody(h->shapeIdB);

        // Player fist into the enemy's bare core.
        const int aCore = B3_ID_EQUALS(a, s_eTorso), bCore = B3_ID_EQUALS(b, s_eTorso);
        if ((aCore || bCore) && s_mExists)
        {
            b3BodyId other = aCore ? b : a;
            for (int arm = 0; arm < MECH_ARMS; arm++)
            {
                if (B3_ID_EQUALS(other, s_mTool[arm]) || B3_ID_EQUALS(other, s_mFore[arm]))
                {
                    const float armMass = b3Body_GetMass(s_mUpper[arm])
                        + b3Body_GetMass(s_mFore[arm]) + b3Body_GetMass(s_mTool[arm]);
                    const float hit = h->approachSpeed * armMass;
                    s_eCoreHp -= hit;
                    if (hit > s_eLastCoreHit) s_eLastCoreHit = hit;
                }
            }
        }

        // Enemy club into the player's bare torso.
        if (s_mExists)
        {
            const int aP = B3_ID_EQUALS(a, s_mTorso), bP = B3_ID_EQUALS(b, s_mTorso);
            if (aP || bP)
            {
                b3BodyId other = aP ? b : a;
                for (int arm = 0; arm < 2; arm++)
                {
                    if (B3_ID_EQUALS(other, s_eArm[arm]))
                    {
                        const float hit = h->approachSpeed * b3Body_GetMass(s_eArm[arm]);
                        s_eHitPlayerCore += hit;
                        if (hit > s_eLastPlayerHit) s_eLastPlayerHit = hit;
                    }
                }
            }
        }
    }

    if (s_eCoreHp <= 0.0f)
    {
        // The drives cut out. The machine drops, and what is left of its
        // plate bursts off it.
        s_eState = E_DEAD;
        // Drives off: the arms go limp and the legs stop holding, so the
        // machine drops where it stands.
        for (int a = 0; a < 2; a++) b3SphericalJoint_SetSpringHertz(s_eShoulder[a], 0.0f);
        if (s_eGrid >= 0 && s_vxG[s_eGrid].used)
        {
            VoxGrid* g = &s_vxG[s_eGrid];
            const b3Transform t = vxPose(g);
            for (int z = 0; z < g->n[2]; z++)
            for (int y = 0; y < g->n[1]; y++)
            for (int x = 0; x < g->n[0]; x++)
            {
                if (!vxAliveAt(g, x, y, z)) continue;
                const b3Vec3 wc = b3TransformPoint(t, vxCellLocal(g, x, y, z));
                vxDamageAt(g, wc.x, wc.y, wc.z, 10000.0f);
            }
            vxRemesh(g);
        }
    }
}

// [x,y,z,qx,qy,qz,qw, state, coreFrac, armL(7), armR(7), lastCoreHit,
//  lastPlayerHit, playerCoreDamage] — everything the page draws and sounds.
// The repair pad wipes the hull damage the enemy has scored on the player.
WASM_EXPORT("w_player_repair")
void w_player_repair(void)
{
    s_eHitPlayerCore = 0.0f;
}

// Direct core damage, for tests and future weapons.
WASM_EXPORT("w_enemy_damage_core")
void w_enemy_damage_core(float amount)
{
    if (s_eExists && s_eState != E_DEAD) s_eCoreHp -= amount;
}

WASM_EXPORT("w_enemy_state")
float* w_enemy_state(void)
{
    static float out[26];
    if (!s_eExists) return out;
    b3Pos p = b3Body_GetPosition(s_eTorso);
    b3Quat q = b3Body_GetRotation(s_eTorso);
    out[0] = (float)p.x; out[1] = (float)p.y; out[2] = (float)p.z;
    out[3] = q.v.x; out[4] = q.v.y; out[5] = q.v.z; out[6] = q.s;
    out[7] = (float)s_eState;
    out[8] = s_eCoreHp > 0.0f ? s_eCoreHp / s_eCoreHpMax : 0.0f;
    for (int i = 0; i < 2; i++)
    {
        b3Pos ap = b3Body_GetPosition(s_eArm[i]);
        b3Quat aq = b3Body_GetRotation(s_eArm[i]);
        float* o = &out[9 + i * 7];
        o[0] = (float)ap.x; o[1] = (float)ap.y; o[2] = (float)ap.z;
        o[3] = aq.v.x; o[4] = aq.v.y; o[5] = aq.v.z; o[6] = aq.s;
    }
    out[23] = s_eLastCoreHit;
    out[24] = s_eLastPlayerHit;
    out[25] = s_eHitPlayerCore;
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
