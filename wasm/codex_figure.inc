// codex_figure.inc — the independent red, physics-first comparison figure.
// Included by bridge.c so it shares the real world and voxel primitives.

#define CODEX_CATEGORY 0x8ULL
#define CODEX_BONE_COUNT (BONE_COUNT + 2)
#define CODEX_L_TOE BONE_COUNT
#define CODEX_R_TOE (BONE_COUNT + 1)
#define CODEX_WINDUP 5
#define CODEX_STRIKE 6
#define CODEX_RECOVER 7
#define CODEX_FALLING 8
#define CODEX_DOWN 9
#define CODEX_GETUP 10

static const BoneDef CODEX_TOES[2] = {
    { BONE_L_FOOT, -0.045f, 0.036f, 0.055f,
      0.030f, 0.014f, 0.036f, 2, AX_FWD, -0.55f, 0.65f, 16.0f, 1.4f, 360.0f },
    { BONE_R_FOOT,  0.045f, 0.036f, 0.055f,
      0.030f, 0.014f, 0.036f, 2, AX_FWD, -0.55f, 0.65f, 16.0f, 1.4f, 360.0f },
};

static int s_codexExists = 0;
static b3BodyId s_codexBone[CODEX_BONE_COUNT];
static b3JointId s_codexJoint[CODEX_BONE_COUNT];
static int s_codexAttached[CODEX_BONE_COUNT];
static int s_codexGrid[CODEX_BONE_COUNT];
static float s_codexStature = 1.75f;
static float s_codexDt = 1.0f / 72.0f;
static b3Vec3 s_codexPlayer;
static float s_codexAct[13];
static int s_codexHold = 0;
static int s_codexState = 0;
static int s_codexSwing = 1;
static int s_codexSwingAir = 0;
static float s_codexTimer = 0.0f;
static float s_codexBalanceX = 0.0f, s_codexBalanceZ = 0.0f;
static float s_codexStanceX = 0.0f, s_codexStanceZ = 0.0f;
static int s_codexArm = 1;
static float s_codexReach = 0.0f;
static int s_codexBroke = 0;
static int s_codexRetries = 0;
static float s_codexCollapse = 0.0f;
static int s_codexRecoveryPhase = 0;
static float s_codexArmLoad = 0.0f;
static float s_codexRecoveryFootLoad = 0.0f;
static float s_codexStable = 0.0f;
static float s_codexAir = 0.0f;

static const BoneDef* codexDef(int b)
{
    if (b >= 0 && b < BONE_COUNT) return &BONES[b];
    if (b == CODEX_L_TOE) return &CODEX_TOES[0];
    if (b == CODEX_R_TOE) return &CODEX_TOES[1];
    return 0;
}

static int codexIsHinge(int b)
{
    const BoneDef* d = codexDef(b);
    return d && (d->axis == AX_HINGE || b >= BONE_COUNT);
}

static void codexWorldReset(void)
{
    s_codexExists = 0;
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        s_codexAttached[b] = 0;
        s_codexGrid[b] = -1;
    }
    for (int i = 0; i < 13; i++) s_codexAct[i] = 0.0f;
    s_codexHold = 0;
    s_codexState = 0;
    s_codexSwing = 1;
    s_codexSwingAir = 0;
    s_codexTimer = 0.0f;
    s_codexArm = 1;
    s_codexReach = 0.0f;
    s_codexBroke = 0;
    s_codexRetries = 0;
    s_codexCollapse = 0.0f;
    s_codexRecoveryPhase = 0;
    s_codexArmLoad = 0.0f;
    s_codexRecoveryFootLoad = 0.0f;
    s_codexStable = 0.0f;
    s_codexAir = 0.0f;
}

static int codexBroken(int b)
{
    if (b < 0 || b >= CODEX_BONE_COUNT || !s_codexAttached[b]) return 1;
    const int g = s_codexGrid[b];
    if (g < 0 || g >= VOX_GRIDS || !s_vxG[g].used) return 1;
    const VoxGrid* vg = &s_vxG[g];
    const int total = vg->n[0] * vg->n[1] * vg->n[2];
    return vg->alive * FIG_BREAK_DEN <= total * FIG_BREAK_NUM;
}

static int codexConnected(int b)
{
    for (int n = 0; n < CODEX_BONE_COUNT && b >= 0; n++)
    {
        if (!s_codexAttached[b]) return 0;
        const BoneDef* d = codexDef(b);
        b = d ? d->parent : -1;
    }
    return 1;
}

static int codexArmGone(int side)
{
    const int a = ARM(side);
    return codexBroken(a) || codexBroken(a + 1) ||
           !codexConnected(a) || !codexConnected(a + 1);
}

static int codexAlive(void)
{
    const int vital[3] = { BONE_PELVIS, BONE_ABDOMEN, BONE_CHEST };
    for (int i = 0; i < 3; i++)
        if (codexBroken(vital[i]) || !codexConnected(vital[i])) return 0;
    return 1;
}

static float codexPartLoad(int bone)
{
    if (!s_codexExists || bone < 0 || bone >= CODEX_BONE_COUNT ||
        !codexConnected(bone)) return 0.0f;
    const b3BodyId body = s_codexBone[bone];
    int cap = b3Body_GetContactCapacity(body);
    if (cap <= 0) return 0.0f;
    if (cap > 24) cap = 24;
    b3ContactData cd[24];
    const int n = b3Body_GetContactData(body, cd, cap);
    float impulse = 0.0f;
    for (int i = 0; i < n; i++)
    {
        const int weAreA = B3_ID_EQUALS(b3Shape_GetBody(cd[i].shapeIdA), body);
        for (int mi = 0; mi < cd[i].manifoldCount; mi++)
        {
            const b3Manifold* mf = &cd[i].manifolds[mi];
            const float up = weAreA ? -mf->normal.y : mf->normal.y;
            if (up < 0.5f) continue;
            for (int k = 0; k < mf->pointCount; k++)
            {
                const b3ManifoldPoint* mp = &mf->points[k];
                if (mp->separation <= 0.005f && mp->totalNormalImpulse > 0.0f)
                    impulse += mp->totalNormalImpulse * up;
            }
        }
    }
    return 0.5f * impulse / s_codexDt;
}

static void codexSense(void)
{
    if (!s_codexExists) return;
    float M = 0.0f, cx = 0.0f, cy = 0.0f, cz = 0.0f;
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        if (!codexConnected(b)) continue;
        const float m = b3Body_GetMass(s_codexBone[b]);
        const b3Pos p = b3Body_GetWorldCenter(s_codexBone[b]);
        const b3Vec3 v = b3Body_GetLinearVelocity(s_codexBone[b]);
        M += m;
        cx += m * (float)p.x; cy += m * (float)p.y; cz += m * (float)p.z;
        vx += m * v.x; vy += m * v.y; vz += m * v.z;
    }
    if (M > 1e-6f)
    {
        cx /= M; cy /= M; cz /= M;
        vx /= M; vy /= M; vz /= M;
    }
    const float nl = codexPartLoad(BONE_L_FOOT) + codexPartLoad(CODEX_L_TOE);
    const float nr = codexPartLoad(BONE_R_FOOT) + codexPartLoad(CODEX_R_TOE);
    s_codexAct[0] = 0.0f; // no root actuator exists
    s_codexAct[1] = 0.0f;
    s_codexAct[3] = nl; s_codexAct[4] = nr;
    s_codexAct[5] = cx; s_codexAct[6] = cy; s_codexAct[7] = cz;
    s_codexAct[8] = vx; s_codexAct[9] = vy; s_codexAct[10] = vz;
    s_codexAct[11] = 0.0f;
    s_codexAct[12] = (nl + nr) < M * 9.81f * 0.06f ? 1.0f : 0.0f;
}

static float codexMass(void)
{
    float mass = 0.0f;
    if (s_codexExists)
        for (int b = 0; b < CODEX_BONE_COUNT; b++)
            if (codexConnected(b)) mass += b3Body_GetMass(s_codexBone[b]);
    return mass;
}

static void codexAim(int bone, b3Quat qRel)
{
    const BoneDef* d = codexDef(bone);
    if (!d || !codexConnected(bone) || d->parent < 0 || codexIsHinge(bone)) return;
    const b3Quat R = figFrameFor(d->axis);
    b3SphericalJoint_SetTargetRotation(s_codexJoint[bone],
        b3MulQuat(qConj(R), b3MulQuat(qRel, R)));
}

static void codexPoint(int bone, float dx, float dy, float dz)
{
    const float l = __builtin_sqrtf(dx*dx + dy*dy + dz*dz);
    if (l < 1e-5f) return;
    const b3Vec3 down = { 0.0f, -1.0f, 0.0f };
    const b3Vec3 dir = { dx/l, dy/l, dz/l };
    codexAim(bone, qFromTo(down, dir));
}

static void codexPointWorld(int bone, float dx, float dy, float dz)
{
    const BoneDef* d = codexDef(bone);
    if (!d || d->parent < 0 || !codexConnected(bone)) return;
    const b3Quat parentQ = b3Body_GetRotation(s_codexBone[d->parent]);
    const b3Vec3 world = { dx, dy, dz };
    const b3Vec3 local = b3InvRotateVector(parentQ, world);
    codexPoint(bone, local.x, local.y, local.z);
}

static void codexBend(int bone, float angle)
{
    if (bone <= 0 || bone >= CODEX_BONE_COUNT || !codexConnected(bone) ||
        !codexIsHinge(bone)) return;
    b3RevoluteJoint_SetTargetAngle(s_codexJoint[bone], angle);
}

static void codexNeutralLegs(void)
{
    codexPoint(BONE_L_THIGH, 0.0f, -1.0f, 0.0f);
    codexPoint(BONE_R_THIGH, 0.0f, -1.0f, 0.0f);
    codexBend(BONE_L_SHIN, 0.0f); codexBend(BONE_R_SHIN, 0.0f);
    codexBend(CODEX_L_TOE, 0.0f); codexBend(CODEX_R_TOE, 0.0f);
}

static void codexJointHertz(float scale)
{
    for (int b = 1; b < CODEX_BONE_COUNT; b++)
    {
        if (!codexConnected(b)) continue;
        const BoneDef* d = codexDef(b);
        float hz = d->hertz * scale;
        if (hz < 0.6f) hz = 0.6f;
        if (codexIsHinge(b)) b3RevoluteJoint_SetSpringHertz(s_codexJoint[b], hz);
        else b3SphericalJoint_SetSpringHertz(s_codexJoint[b], hz);
    }
}

static void codexBoneHertz(int bone, float hz)
{
    if (!codexConnected(bone) || bone <= 0) return;
    if (codexIsHinge(bone)) b3RevoluteJoint_SetSpringHertz(s_codexJoint[bone], hz);
    else b3SphericalJoint_SetSpringHertz(s_codexJoint[bone], hz);
}

static void codexGuard(void)
{
    for (int side = 0; side < 2; side++)
    {
        const int a = ARM(side);
        const float out = side ? 0.28f : -0.28f;
        codexPoint(a, out, -0.90f, 0.34f);
        codexBend(a + 1, -2.05f);
        codexPoint(a + 2, 0.0f, -1.0f, 0.0f);
    }
    codexAim(BONE_ABDOMEN, qAxisAngle(1.0f, 0.0f, 0.0f, 0.03f));
    codexAim(BONE_CHEST, qAxisAngle(1.0f, 0.0f, 0.0f, 0.02f));
    codexAim(BONE_NECK, qAxisAngle(1.0f, 0.0f, 0.0f, -0.10f));
    codexAim(BONE_HEAD, b3Quat_identity);
}

static b3Vec3 codexPlayerInChest(void)
{
    b3Vec3 out = { 0.0f, 0.0f, 1.0f };
    if (!codexConnected(BONE_CHEST)) return out;
    const b3Pos c = b3Body_GetPosition(s_codexBone[BONE_CHEST]);
    const b3Quat q = b3Body_GetRotation(s_codexBone[BONE_CHEST]);
    const b3Vec3 w = { (float)(s_codexPlayer.x - c.x),
                       (float)(s_codexPlayer.y - c.y),
                       (float)(s_codexPlayer.z - c.z) };
    return b3InvRotateVector(q, w);
}

static void codexMeasureReach(void)
{
    s_codexReach = 0.0f;
    const float chestY = s_codexPlayer.y - 0.25f;
    for (int side = 0; side < 2; side++)
    {
        const int h = ARM(side) + 2;
        if (!codexConnected(h)) continue;
        const b3Pos hp = b3Body_GetPosition(s_codexBone[h]);
        const float dx = (float)hp.x - s_codexPlayer.x;
        const float dy = (float)hp.y - chestY;
        const float dz = (float)hp.z - s_codexPlayer.z;
        if (dx*dx + dy*dy + dz*dz > 0.22f * 0.22f) continue;
        const b3Vec3 v = b3Body_GetLinearVelocity(s_codexBone[h]);
        const float speed = __builtin_sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
        if (speed < 1.4f) continue;
        const float momentum = b3Body_GetMass(s_codexBone[h]) * speed;
        if (momentum > s_codexReach) s_codexReach = momentum;
    }
}

static int codexIsUpright(void);

// A punch may start only from a base that can carry its equal reaction. Read
// the current sensed COM/velocity directly: s_codexAct[11] is written later by
// codexBalance(), so using it here would make the decision one frame stale.
static int codexPunchBaseReady(void)
{
    if (!codexIsUpright()) return 0;
    const float W = codexMass() * 9.81f;
    if (s_codexAct[3] < 0.12f * W || s_codexAct[4] < 0.12f * W) return 0;
    const b3Pos lf = b3Body_GetPosition(s_codexBone[BONE_L_FOOT]);
    const b3Pos rf = b3Body_GetPosition(s_codexBone[BONE_R_FOOT]);
    const float midX = 0.5f * ((float)lf.x + (float)rf.x);
    const float midZ = 0.5f * ((float)lf.z + (float)rf.z);
    const float icpX = s_codexAct[5] + s_codexAct[8] / 3.1f;
    const float icpZ = s_codexAct[7] + s_codexAct[10] / 3.1f;
    const float ex = icpX - midX, ez = icpZ - midZ;
    return ex*ex + ez*ez < 0.045f * 0.045f;
}

// Fighting is a sequence of reachable joint targets. Nothing is placed at the
// player and no impulse is injected into the fist: the shoulder winds it back,
// the elbow opens, and the equal reaction has to be caught by the feet.
static void codexCombat(float dt)
{
    s_codexTimer += dt;
    const b3Pos lf = b3Body_GetPosition(s_codexBone[BONE_L_FOOT]);
    const b3Pos rf = b3Body_GetPosition(s_codexBone[BONE_R_FOOT]);
    s_codexBalanceX = 0.5f * ((float)lf.x + (float)rf.x);
    s_codexBalanceZ = 0.5f * ((float)lf.z + (float)rf.z);
    codexNeutralLegs();
    codexGuard();

    if (s_codexHold)
    {
        s_codexState = 0;
        s_codexTimer = 0.0f;
        return;
    }
    if (s_codexState < CODEX_WINDUP || s_codexState > CODEX_RECOVER)
    {
        if (s_codexTimer < 0.45f) return;
        if (!codexPunchBaseReady()) return;
        if (codexArmGone(0) && codexArmGone(1)) return;
        s_codexArm = !s_codexArm;
        if (codexArmGone(s_codexArm)) s_codexArm = !s_codexArm;
        s_codexState = CODEX_WINDUP;
        s_codexTimer = 0.0f;
    }

    const int a = ARM(s_codexArm);
    const float side = s_codexArm ? 1.0f : -1.0f;
    if (s_codexState == CODEX_WINDUP)
    {
        if (!codexPunchBaseReady())
        {
            s_codexState = 0;
            s_codexTimer = 0.0f;
            return;
        }
        codexPoint(a, side * 0.55f, -0.55f, -0.62f);
        codexBend(a + 1, -2.35f);
        b3SphericalJoint_SetSpringHertz(s_codexJoint[a], 17.0f);
        if (s_codexTimer > 0.30f)
        { s_codexState = CODEX_STRIKE; s_codexTimer = 0.0f; }
    }
    else if (s_codexState == CODEX_STRIKE)
    {
        const b3Vec3 t = codexPlayerInChest();
        codexPoint(a, t.x, t.y, t.z);
        codexBend(a + 1, -0.08f);
        codexPoint(a + 2, t.x, t.y, t.z);
        b3SphericalJoint_SetSpringHertz(s_codexJoint[a], 30.0f);
        if (s_codexTimer > 0.24f)
        { s_codexState = CODEX_RECOVER; s_codexTimer = 0.0f; }
    }
    else
    {
        codexPoint(a, side * 0.70f, -0.65f, 0.18f);
        codexBend(a + 1, -0.55f);
        b3SphericalJoint_SetSpringHertz(s_codexJoint[a], 7.0f);
        if (s_codexTimer > 0.46f)
        { s_codexState = 0; s_codexTimer = 0.0f; }
    }
}

static int codexIsUpright(void)
{
    if (!codexConnected(BONE_HEAD)) return 0;
    const b3Pos hip = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    const b3Pos head = b3Body_GetPosition(s_codexBone[BONE_HEAD]);
    return hip.y > s_worldFloorY + 0.43f * s_codexStature &&
           head.y > hip.y + 0.14f * s_codexStature;
}

static int codexIsDown(void)
{
    if (!codexConnected(BONE_HEAD)) return 1;
    const b3Pos hip = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    const b3Pos head = b3Body_GetPosition(s_codexBone[BONE_HEAD]);
    return hip.y < s_worldFloorY + 0.34f * s_codexStature ||
           head.y < hip.y + 0.08f * s_codexStature;
}

static void codexBalance(void);

// A get-up is deliberately allowed to fail. These stages only change joint
// targets: tuck, plant the arms, then extend the legs. The body rises only if
// those limbs find the floor and the contact solver supplies a reaction.
static void codexRecovery(float dt)
{
    s_codexTimer += dt;
    s_codexArmLoad = codexPartLoad(BONE_L_HAND) + codexPartLoad(BONE_R_HAND) +
                     codexPartLoad(BONE_L_FOREARM) + codexPartLoad(BONE_R_FOREARM);
    s_codexRecoveryFootLoad = codexPartLoad(BONE_L_FOOT) + codexPartLoad(CODEX_L_TOE) +
                              codexPartLoad(BONE_R_FOOT) + codexPartLoad(CODEX_R_TOE);
    if (!codexAlive())
    {
        s_codexState = CODEX_DOWN;
        codexJointHertz(0.03f);
        return;
    }
    if (s_codexState == CODEX_FALLING)
    {
        codexJointHertz(0.03f);
        if (s_codexTimer > 0.55f && codexIsDown())
        {
            s_codexState = CODEX_GETUP;
            s_codexTimer = 0.0f;
            s_codexRetries++;
            s_codexRecoveryPhase = 0;
            s_codexStable = 0.0f;
        }
        return;
    }
    if (s_codexState == CODEX_DOWN) return;

    codexJointHertz(0.35f);
    codexBoneHertz(BONE_L_UPPERARM, 5.0f);
    codexBoneHertz(BONE_R_UPPERARM, 5.0f);
    codexBoneHertz(BONE_L_FOREARM, 4.5f);
    codexBoneHertz(BONE_R_FOREARM, 4.5f);
    codexBoneHertz(BONE_L_HAND, 3.5f);
    codexBoneHertz(BONE_R_HAND, 3.5f);

    // Every attempt first reaches actual limbs toward world-down. That world
    // conversion matters when the chest is on its side: a fixed chest-local
    // pose only waves the arms sideways and can never find the floor.
    codexPointWorld(BONE_L_UPPERARM, 0.0f, -1.0f, 0.0f);
    codexPointWorld(BONE_R_UPPERARM, 0.0f, -1.0f, 0.0f);
    codexPointWorld(BONE_L_HAND, 0.0f, -1.0f, 0.0f);
    codexPointWorld(BONE_R_HAND, 0.0f, -1.0f, 0.0f);
    codexPointWorld(BONE_L_THIGH, 0.0f, -1.0f, 0.0f);
    codexPointWorld(BONE_R_THIGH, 0.0f, -1.0f, 0.0f);
    codexBend(CODEX_L_TOE, 0.0f); codexBend(CODEX_R_TOE, 0.0f);

    if (s_codexRecoveryPhase == 0)
    {
        // Find the ground with arms long and knees tucked. The phase cannot
        // advance merely because time passed: a measured hand/forearm reaction
        // is the permission to push.
        codexBend(BONE_L_FOREARM, -0.05f); codexBend(BONE_R_FOREARM, -0.05f);
        codexBend(BONE_L_SHIN, 1.45f); codexBend(BONE_R_SHIN, 1.45f);
        if (s_codexArmLoad > 8.0f && s_codexTimer > 0.12f)
        {
            s_codexRecoveryPhase = 1;
            s_codexTimer = 0.0f;
        }
        else if (s_codexTimer > 1.10f)
        {
            s_codexTimer = 0.0f;
            s_codexRetries++;
        }
    }
    else if (s_codexRecoveryPhase == 1)
    {
        // Push the chest clear and unfold the knees only while the arms are
        // carrying load. Lose the plant and return to finding it.
        codexBend(BONE_L_FOREARM, -0.05f); codexBend(BONE_R_FOREARM, -0.05f);
        codexBend(BONE_L_SHIN, 0.85f); codexBend(BONE_R_SHIN, 0.85f);
        if (s_codexRecoveryFootLoad > 8.0f &&
                 b3Body_GetPosition(s_codexBone[BONE_PELVIS]).y >
                    s_worldFloorY + 0.22f * s_codexStature)
        {
            s_codexRecoveryPhase = 2;
            s_codexTimer = 0.0f;
        }
    }
    else if (s_codexRecoveryPhase == 2)
    {
        // Feet have taken load, so extend the two leg columns. The arms keep
        // their floor target until the chest is upright; no root force helps.
        codexNeutralLegs();
        codexBend(BONE_L_FOREARM, -0.05f); codexBend(BONE_R_FOREARM, -0.05f);
        if (s_codexRecoveryFootLoad < 2.0f && s_codexTimer > 0.25f)
        {
            s_codexRecoveryPhase = 0;
            s_codexTimer = 0.0f;
            s_codexRetries++;
        }
    }

    else
    {
        // The body has passed through an upright pose. Do not call that a
        // recovery while it is still flying upward: return to the ordinary
        // planted stance, use ankle reactions, and demand a quiet interval.
        if (s_codexRecoveryFootLoad > 2.0f)
            codexNeutralLegs();
        else
        {
            codexPointWorld(BONE_L_THIGH, 0.0f, -1.0f, 0.0f);
            codexPointWorld(BONE_R_THIGH, 0.0f, -1.0f, 0.0f);
            codexBend(BONE_L_SHIN, 0.35f); codexBend(BONE_R_SHIN, 0.35f);
        }
        codexGuard();
        const b3Pos lf = b3Body_GetPosition(s_codexBone[BONE_L_FOOT]);
        const b3Pos rf = b3Body_GetPosition(s_codexBone[BONE_R_FOOT]);
        s_codexBalanceX = 0.5f * ((float)lf.x + (float)rf.x);
        s_codexBalanceZ = 0.5f * ((float)lf.z + (float)rf.z);
        codexBalance();
        const float speed = __builtin_sqrtf(s_codexAct[8]*s_codexAct[8] +
                                             s_codexAct[9]*s_codexAct[9] +
                                             s_codexAct[10]*s_codexAct[10]);
        if (codexIsUpright() && s_codexRecoveryFootLoad > 2.0f && speed < 2.5f)
            s_codexStable += dt;
        else
            s_codexStable = 0.0f;

        if (s_codexStable > 0.05f)
        {
            codexJointHertz(1.0f);
            s_codexState = 0;
            s_codexTimer = 0.0f;
            s_codexCollapse = 0.0f;
            s_codexAir = 0.0f;
            s_codexRecoveryPhase = 0;
            s_codexStable = 0.0f;
            return;
        }
        if (s_codexTimer > 3.0f || (s_codexTimer > 0.80f && codexIsDown()))
        {
            s_codexRecoveryPhase = 0;
            s_codexTimer = 0.0f;
            s_codexStable = 0.0f;
            s_codexRetries++;
        }
        return;
    }

    if (codexIsUpright() && s_codexAct[3] + s_codexAct[4] > 2.0f)
    {
        const float speed = __builtin_sqrtf(s_codexAct[8]*s_codexAct[8] +
                                             s_codexAct[9]*s_codexAct[9] +
                                             s_codexAct[10]*s_codexAct[10]);
        if (speed < 2.5f)
        {
            codexJointHertz(1.0f);
            codexNeutralLegs();
            codexGuard();
            s_codexState = 0;
            s_codexTimer = 0.0f;
            s_codexCollapse = 0.0f;
            s_codexAir = 0.0f;
            s_codexRecoveryPhase = 0;
            s_codexStable = 0.0f;
            return;
        }
        s_codexRecoveryPhase = 3;
        s_codexTimer = 0.0f;
        s_codexStable = 0.0f;
    }
    else if (s_codexRecoveryPhase > 0 && s_codexTimer > 2.0f)
    {
        s_codexRecoveryPhase = 0;
        s_codexTimer = 0.0f;
        s_codexRetries++;
    }
}

// Contact advances every phase. Time shapes a reachable swing pose and acts
// only as an abort guard; it never manufactures a footfall.
static void codexGait(float dt)
{
    const b3Pos lf = b3Body_GetPosition(s_codexBone[BONE_L_FOOT]);
    const b3Pos rf = b3Body_GetPosition(s_codexBone[BONE_R_FOOT]);
    const float midX = 0.5f * ((float)lf.x + (float)rf.x);
    const float midZ = 0.5f * ((float)lf.z + (float)rf.z);
    const b3Pos hip = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    const float dx = s_codexPlayer.x - (float)hip.x;
    const float dz = s_codexPlayer.z - (float)hip.z;
    const float dist = __builtin_sqrtf(dx*dx + dz*dz);
    const float dl = dist > 1e-5f ? 1.0f / dist : 0.0f;
    const float dirX = dx * dl, dirZ = dz * dl;
    const float W = codexMass() * 9.81f;
    s_codexTimer += dt;

    if (s_codexHold || dist < 0.92f)
    {
        s_codexState = 0; s_codexTimer = 0.0f;
        s_codexBalanceX = midX; s_codexBalanceZ = midZ;
        codexNeutralLegs();
        return;
    }

    if (s_codexState == 0)
    {
        s_codexBalanceX = midX; s_codexBalanceZ = midZ;
        codexNeutralLegs();
        // Do not start the next weight shift merely because a timer elapsed.
        // Both feet and the capture point must first have settled over the
        // double-support polygon left by the previous landing.
        const float settleX = s_codexAct[5] + s_codexAct[8] / 3.1f - midX;
        const float settleZ = s_codexAct[7] + s_codexAct[10] / 3.1f - midZ;
        const float settleError = __builtin_sqrtf(settleX*settleX + settleZ*settleZ);
        if (settleError > 0.035f || s_codexAct[3] < 0.10f * W ||
            s_codexAct[4] < 0.10f * W)
        {
            s_codexTimer = 0.0f;
            return;
        }
        if (s_codexTimer < 0.65f) return;
        const b3Pos firstStance = s_codexSwing ? lf : rf;
        s_codexStanceX = (float)firstStance.x;
        s_codexStanceZ = (float)firstStance.z;
        s_codexState = 1; s_codexTimer = 0.0f;
    }

    const int swingFoot = s_codexSwing ? BONE_R_FOOT : BONE_L_FOOT;
    const int swingThigh = s_codexSwing ? BONE_R_THIGH : BONE_L_THIGH;
    const int swingShin = s_codexSwing ? BONE_R_SHIN : BONE_L_SHIN;
    const int swingToe = s_codexSwing ? CODEX_R_TOE : CODEX_L_TOE;
    const float swingLoad = s_codexAct[s_codexSwing ? 4 : 3];
    const float stanceLoad = s_codexAct[s_codexSwing ? 3 : 4];

    if (s_codexState == 1)
    {
        codexNeutralLegs();
        const float outside = s_codexSwing ? -0.004f : 0.004f;
        s_codexBalanceX = s_codexStanceX + outside;
        s_codexBalanceZ = s_codexStanceZ;
        if (swingLoad < 0.23f * W && stanceLoad > 0.60f * W)
        { s_codexState = 2; s_codexTimer = 0.0f; s_codexSwingAir = 0; }
        return;
    }

    if (s_codexState == 2)
    {
        const float outside = s_codexSwing ? -0.004f : 0.004f;
        s_codexBalanceX = s_codexStanceX + outside;
        s_codexBalanceZ = s_codexStanceZ + 0.015f * dirZ;
        // If unloading has not produced an airborne foot before the capture
        // point escapes the stance sole, put the foot back and recover. A
        // failed human step becomes double support, not a committed fall.
        const float swingIcpX = s_codexAct[5] + s_codexAct[8] / 3.1f;
        const float swingIcpZ = s_codexAct[7] + s_codexAct[10] / 3.1f;
        const float swingErrX = swingIcpX - s_codexBalanceX;
        const float swingErrZ = swingIcpZ - s_codexBalanceZ;
        const float swingError = __builtin_sqrtf(swingErrX*swingErrX + swingErrZ*swingErrZ);
        if (((!s_codexSwingAir && s_codexTimer > 0.25f) || s_codexTimer > 0.90f) &&
            swingError > 0.035f)
        {
            s_codexState = 0;
            s_codexTimer = 0.0f;
            s_codexBalanceX = midX;
            s_codexBalanceZ = midZ;
            codexNeutralLegs();
            return;
        }
        if (swingLoad < 0.04f * W) s_codexSwingAir = 1;
        float knee = s_codexTimer < 0.16f ? 0.65f
                     : 0.65f - (s_codexTimer - 0.16f) * 4.5f;
        if (knee < 0.02f) knee = 0.02f;
        float reach = s_codexTimer < 0.15f ? 0.55f
                      : 0.55f - (s_codexTimer - 0.15f) * 2.50f;
        if (reach < -0.10f) reach = -0.10f;
        if (s_codexSwingAir) reach = 0.12f;
        codexPoint(swingThigh, dirX * reach, -1.0f, dirZ * reach);
        codexBend(swingShin, knee);
        codexBend(swingToe, 0.28f);
        const b3Pos swingPosition = b3Body_GetPosition(s_codexBone[swingFoot]);
        const float stride = ((float)swingPosition.x - s_codexStanceX) * dirX +
                             ((float)swingPosition.z - s_codexStanceZ) * dirZ;
        if (s_codexSwingAir && s_codexTimer > 0.35f && stride > 0.04f &&
            swingLoad > 0.10f * W)
        { s_codexState = 3; s_codexTimer = 0.0f; }
        return;
    }

    if (s_codexState == 3)
    {
        float transfer = s_codexTimer / 1.50f;
        if (transfer > 1.0f) transfer = 1.0f;
        s_codexBalanceX = s_codexStanceX + (midX - s_codexStanceX) * transfer;
        s_codexBalanceZ = s_codexStanceZ + (midZ - s_codexStanceZ) * transfer;
        codexBend(swingShin, 0.08f); codexBend(swingToe, 0.08f);
        if (transfer >= 1.0f && swingLoad > 0.18f * W)
        { s_codexState = 4; s_codexTimer = 0.0f; }
        return;
    }

    s_codexBalanceX = midX; s_codexBalanceZ = midZ;
    codexNeutralLegs();
    if (s_codexTimer > 0.18f && s_codexAct[3] > 0.12f * W && s_codexAct[4] > 0.12f * W)
    {
        s_codexSwing = !s_codexSwing;
        s_codexState = 0; s_codexTimer = 0.0f;
    }
}

// The root has no actuator. Balance is produced at the two ankle joints as an
// equal-and-opposite torque pair between shin and foot. The pair sums to zero
// over the robot; only a loaded sole can turn it into an external floor
// reaction, so it cannot lift or right the body in the air.
static void codexBalance(void)
{
    if (!s_codexExists) return;
    const float loaded = s_codexAct[3] + s_codexAct[4];
    if (loaded < 1.0f) return;
    const b3Pos lf = b3Body_GetPosition(s_codexBone[BONE_L_FOOT]);
    const b3Pos rf = b3Body_GetPosition(s_codexBone[BONE_R_FOOT]);
    const float sx = s_codexBalanceX;
    const float sz = s_codexBalanceZ;
    const float omega = 3.1f;
    const float icpX = s_codexAct[5] + s_codexAct[8] / omega;
    const float icpZ = s_codexAct[7] + s_codexAct[10] / omega;
    float wantX = icpX + 1.8f * (icpX - sx);
    float wantZ = icpZ + 1.8f * (icpZ - sz);
    const float halfX = 0.042f * s_codexStature / 1.75f;
    const float heel = 0.070f * s_codexStature / 1.75f;
    const float toe = 0.105f * s_codexStature / 1.75f;
    float work = 0.0f;
    for (int side = 0; side < 2; side++)
    {
        const int foot = side == 0 ? BONE_L_FOOT : BONE_R_FOOT;
        const int shin = side == 0 ? BONE_L_SHIN : BONE_R_SHIN;
        const float N = s_codexAct[3 + side];
        if (N < 1.0f) continue;
        const b3Pos ap = b3Body_GetPosition(s_codexBone[foot]);
        float lx = wantX - (float)ap.x;
        float lz = wantZ - (float)ap.z;
        if (lx < -halfX) lx = -halfX; if (lx > halfX) lx = halfX;
        if (lz < -heel) lz = -heel; if (lz > toe) lz = toe;
        b3Vec3 t = { -lz * N, 0.0f, lx * N };
        float mag = __builtin_sqrtf(t.x*t.x + t.z*t.z);
        const float cap = 75.0f;
        if (mag > cap)
        {
            const float k = cap / mag;
            t.x *= k; t.z *= k; mag = cap;
        }
        b3Vec3 nt = { -t.x, 0.0f, -t.z };
        b3Body_ApplyTorque(s_codexBone[shin], t, true);
        b3Body_ApplyTorque(s_codexBone[foot], nt, true);
        work += mag;
    }

    // A human does not balance on one foot with the ankle alone. During
    // unloading and swing, use the loaded hip to move the trunk over the
    // stance leg. This is still a purely internal actuator: every torque on
    // the pelvis has an equal and opposite torque on the stance thigh.
    if (s_codexState == 1 || s_codexState == 2)
    {
        const int stanceThigh = s_codexSwing ? BONE_L_THIGH : BONE_R_THIGH;
        const float stanceLoad = s_codexAct[s_codexSwing ? 3 : 4];
        if (stanceLoad > 0.20f * codexMass() * 9.81f)
        {
            const b3Vec3 pelvisOmega = b3Body_GetAngularVelocity(s_codexBone[BONE_PELVIS]);
            const float lateralError = sx - s_codexAct[5];
            float hipZ = -180.0f * lateralError + 18.0f * s_codexAct[8]
                         - 7.0f * pelvisOmega.z;
            if (hipZ < -48.0f) hipZ = -48.0f;
            if (hipZ > 48.0f) hipZ = 48.0f;
            const b3Vec3 pelvisTorque = { 0.0f, 0.0f, hipZ };
            const b3Vec3 thighTorque = { 0.0f, 0.0f, -hipZ };
            b3Body_ApplyTorque(s_codexBone[BONE_PELVIS], pelvisTorque, true);
            b3Body_ApplyTorque(s_codexBone[stanceThigh], thighTorque, true);
            work += __builtin_fabsf(hipZ);
        }
    }

    s_codexAct[2] = work;
    s_codexAct[11] = __builtin_sqrtf((icpX-sx)*(icpX-sx) + (icpZ-sz)*(icpZ-sz));
}

WASM_EXPORT("w_codex_destroy")
void w_codex_destroy(void)
{
    if (!s_codexExists) return;
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        const int g = s_codexGrid[b];
        if (g >= 0 && g < VOX_GRIDS) s_vxG[g].used = 0;
        if (g >= 0) b3DestroyBody(s_codexBone[b]);
        s_codexAttached[b] = 0;
        s_codexGrid[b] = -1;
    }
    s_codexExists = 0;
}

WASM_EXPORT("w_codex_create")
int w_codex_create(float x, float z, float stature, int material)
{
    if (s_codexExists) w_codex_destroy();
    const float H = stature > 1.0f && stature < 2.4f ? stature : 1.75f;
    s_codexStature = H;
    s_codexDt = 1.0f / 72.0f;
    for (int i = 0; i < 13; i++) s_codexAct[i] = 0.0f;
    s_codexHold = 0;
    s_codexState = 0;
    s_codexSwing = 1;
    s_codexSwingAir = 0;
    s_codexTimer = 0.0f;
    s_codexArm = 1;
    s_codexReach = 0.0f;
    s_codexBroke = 0;
    s_codexRetries = 0;
    s_codexCollapse = 0.0f;
    s_codexRecoveryPhase = 0;
    s_codexArmLoad = 0.0f;
    s_codexRecoveryFootLoad = 0.0f;
    s_codexStable = 0.0f;
    s_codexAir = 0.0f;
    s_codexBalanceX = x; s_codexBalanceZ = z;
    s_codexStanceX = x; s_codexStanceZ = z;

    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        const BoneDef* d = codexDef(b);
        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position.x = x + d->ox * H;
        bd.position.y = s_worldFloorY + d->oy * H;
        bd.position.z = z + d->oz * H;
        bd.angularDamping = 1.4f;
        bd.linearDamping = 0.15f;
        s_codexBone[b] = b3CreateBody(s_world, &bd);
        s_codexAttached[b] = 1;
        s_codexGrid[b] = -1;
    }

    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        const BoneDef* d = codexDef(b);
        float hx = d->hx * H, hy = d->hy * H, hz = d->hz * H;
        float longest = hx > hy ? hx : hy;
        if (hz > longest) longest = hz;
        const float cell = 2.0f * longest / (float)d->cells;
        int nx = (int)(2.0f * hx / cell + 0.5f); if (nx < 1) nx = 1;
        int ny = (int)(2.0f * hy / cell + 0.5f); if (ny < 1) ny = 1;
        int nz = (int)(2.0f * hz / cell + 0.5f); if (nz < 1) nz = 1;
        float cy = 0.0f, cz = 0.0f;
        if (d->axis == AX_UP) cy = 0.5f * ny * cell;
        else if (d->axis == AX_FWD)
        {
            cy = -0.5f * ny * cell;
            cz = 0.25f * nz * cell;
        }
        else cy = -0.5f * ny * cell;
        b3Vec3 local = { -0.5f * nx * cell, cy - 0.5f * ny * cell,
                         cz - 0.5f * nz * cell };
        s_codexGrid[b] = vxBuild(s_codexBone[b], local, nx, ny, nz, cell,
                                 material, 1, CODEX_CATEGORY);
        vxMakePart(s_codexGrid[b], d->hpMul);
        vxPartDensity(s_codexGrid[b], d->dens);
        w_vox_hide(s_codexGrid[b], 1);
    }

    for (int b = 1; b < CODEX_BONE_COUNT; b++)
    {
        const BoneDef* d = codexDef(b);
        const BoneDef* p = codexDef(d->parent);
        const b3Quat R = figFrameFor(codexIsHinge(b) ? AX_HINGE : d->axis);
        b3Transform fa = b3Transform_identity;
        fa.p.x = (d->ox - p->ox) * H;
        fa.p.y = (d->oy - p->oy) * H;
        fa.p.z = (d->oz - p->oz) * H;
        fa.q = R;
        b3Transform fb = b3Transform_identity;
        fb.q = R;

        if (codexIsHinge(b))
        {
            b3RevoluteJointDef rj = b3DefaultRevoluteJointDef();
            rj.base.bodyIdA = s_codexBone[d->parent];
            rj.base.bodyIdB = s_codexBone[b];
            rj.base.localFrameA = fa;
            rj.base.localFrameB = fb;
            rj.base.collideConnected = false;
            rj.enableLimit = true;
            rj.lowerAngle = d->cone;
            rj.upperAngle = d->hi;
            rj.enableSpring = true;
            rj.hertz = d->hertz;
            rj.dampingRatio = 0.9f;
            rj.targetAngle = 0.0f;
            s_codexJoint[b] = b3CreateRevoluteJoint(s_world, &rj);
        }
        else
        {
            b3SphericalJointDef sj = b3DefaultSphericalJointDef();
            sj.base.bodyIdA = s_codexBone[d->parent];
            sj.base.bodyIdB = s_codexBone[b];
            sj.base.localFrameA = fa;
            sj.base.localFrameB = fb;
            sj.base.collideConnected = false;
            sj.enableConeLimit = true;
            sj.coneAngle = d->cone > 1.5f ? 1.5f : d->cone;
            sj.enableSpring = true;
            sj.hertz = d->hertz;
            sj.dampingRatio = 0.9f;
            sj.targetRotation = b3Quat_identity;
            s_codexJoint[b] = b3CreateSphericalJoint(s_world, &sj);
        }
    }

    s_codexPlayer.x = x;
    s_codexPlayer.y = s_worldFloorY + 1.5f;
    s_codexPlayer.z = z + 1.5f;
    s_codexExists = 1;
    return CODEX_BONE_COUNT;
}

WASM_EXPORT("w_codex_update")
void w_codex_update(float px, float py, float pz, float dt)
{
    if (!s_codexExists) return;
    s_codexDt = dt > 1e-5f ? dt : 1.0f / 72.0f;
    s_codexPlayer.x = px; s_codexPlayer.y = py; s_codexPlayer.z = pz;
    codexSense();
    codexMeasureReach();

    if (s_codexState < CODEX_FALLING)
    {
        if (codexIsDown()) s_codexCollapse += s_codexDt;
        else if (s_codexCollapse > 0.0f) s_codexCollapse -= 2.0f * s_codexDt;
        if (s_codexAct[12] > 0.5f) s_codexAir += s_codexDt;
        else if (s_codexAir > 0.0f) s_codexAir -= 2.0f * s_codexDt;
        if (s_codexCollapse > 0.18f || s_codexAir > 0.65f)
        {
            s_codexState = CODEX_FALLING;
            s_codexTimer = 0.0f;
        }
    }

    if (s_codexState >= CODEX_FALLING)
    {
        codexRecovery(s_codexDt);
        return;
    }

    const b3Pos hip = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    const float dx = px - (float)hip.x, dz = pz - (float)hip.z;
    const float dist = __builtin_sqrtf(dx*dx + dz*dz);
    if ((dist < 0.92f && s_codexState == 0) ||
        (s_codexState >= CODEX_WINDUP && s_codexState <= CODEX_RECOVER))
        codexCombat(s_codexDt);
    else
        codexGait(s_codexDt);
    codexBalance();
}

WASM_EXPORT("w_codex_post")
void w_codex_post(void)
{
    if (!s_codexExists) return;
    s_codexBroke = 0;
    for (int b = 1; b < CODEX_BONE_COUNT; b++)
    {
        if (!s_codexAttached[b] || !codexBroken(b)) continue;
        b3DestroyJoint(s_codexJoint[b], true);
        s_codexAttached[b] = 0;
        s_codexBroke++;
        b3Body_SetAngularDamping(s_codexBone[b], 0.4f);
        b3Body_SetLinearDamping(s_codexBone[b], 0.05f);
    }
}

// External blows are impulses on a selected physical body. This is used by
// deterministic knock-down tests and is the same sort of input a collision
// supplies; it is never called by the controller to move itself.
WASM_EXPORT("w_codex_impulse")
void w_codex_impulse(int bone, float ix, float iy, float iz)
{
    if (!s_codexExists || bone < 0 || bone >= CODEX_BONE_COUNT) return;
    const b3Vec3 impulse = { ix, iy, iz };
    b3Body_ApplyLinearImpulseToCenter(s_codexBone[bone], impulse, true);
}

WASM_EXPORT("w_codex_bone_count")
int w_codex_bone_count(void) { return CODEX_BONE_COUNT; }

WASM_EXPORT("w_codex_bone_grid")
int w_codex_bone_grid(int b)
{
    return b >= 0 && b < CODEX_BONE_COUNT ? s_codexGrid[b] : -1;
}

WASM_EXPORT("w_codex_state")
float* w_codex_state(void)
{
    static float out[16];
    for (int i = 0; i < 16; i++) out[i] = 0.0f;
    if (!s_codexExists) return out;
    int on = 0, alive = 0, total = 0;
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        if (codexConnected(b)) on++;
        const int g = s_codexGrid[b];
        if (g < 0 || !s_vxG[g].used) continue;
        alive += s_vxG[g].alive;
        total += s_vxG[g].n[0] * s_vxG[g].n[1] * s_vxG[g].n[2];
    }
    const b3Pos p = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    out[0] = 1.0f; out[1] = (float)s_codexState; out[2] = (float)on;
    out[3] = (float)alive; out[4] = (float)total;
    out[5] = (float)s_codexRetries;
    out[6] = (float)p.x; out[7] = (float)p.y; out[8] = (float)p.z;
    const float dx = s_codexPlayer.x - (float)p.x;
    const float dz = s_codexPlayer.z - (float)p.z;
    out[10] = __builtin_sqrtf(dx*dx + dz*dz);
    out[9] = (float)s_codexRecoveryPhase;
    out[11] = s_codexArmLoad;
    out[12] = s_codexRecoveryFootLoad;
    out[13] = s_codexReach;
    out[14] = (float)s_codexBroke;
    out[15] = s_codexStature;
    return out;
}

WASM_EXPORT("w_codex_pose")
float* w_codex_pose(void)
{
    static float out[CODEX_BONE_COUNT * 8];
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        float* o = &out[b * 8];
        if (!s_codexExists)
        {
            for (int i = 0; i < 8; i++) o[i] = 0.0f;
            continue;
        }
        const b3Pos p = b3Body_GetPosition(s_codexBone[b]);
        const b3Quat q = b3Body_GetRotation(s_codexBone[b]);
        o[0] = (float)p.x; o[1] = (float)p.y; o[2] = (float)p.z;
        o[3] = q.v.x; o[4] = q.v.y; o[5] = q.v.z; o[6] = q.s;
        o[7] = (float)s_codexAttached[b];
    }
    return out;
}

WASM_EXPORT("w_codex_bones")
float* w_codex_bones(void)
{
    static float out[CODEX_BONE_COUNT * 4];
    for (int b = 0; b < CODEX_BONE_COUNT; b++)
    {
        float* o = &out[b * 4];
        const int g = s_codexGrid[b];
        o[0] = (float)s_codexAttached[b];
        o[1] = o[2] = o[3] = 0.0f;
        if (g < 0 || g >= VOX_GRIDS || !s_vxG[g].used) continue;
        o[1] = (float)s_vxG[g].alive;
        o[2] = (float)(s_vxG[g].n[0] * s_vxG[g].n[1] * s_vxG[g].n[2]);
        o[3] = vxDamage(&s_vxG[g]);
    }
    return out;
}

WASM_EXPORT("w_codex_actuation")
float* w_codex_actuation(void) { return s_codexAct; }

WASM_EXPORT("w_codex_rig")
float* w_codex_rig(void)
{
    static float out[2];
    out[0] = codexMass();
    out[1] = s_worldFloorY + BONES[BONE_PELVIS].oy * s_codexStature;
    return out;
}

WASM_EXPORT("w_codex_hold")
void w_codex_hold(int on) { s_codexHold = on ? 1 : 0; }

WASM_EXPORT("w_codex_joints")
float* w_codex_joints(void)
{
    static float out[4];
    for (int i = 0; i < 4; i++) out[i] = 0.0f;
    if (!s_codexExists) return out;
    out[0] = b3RevoluteJoint_GetAngle(s_codexJoint[BONE_L_SHIN]);
    out[1] = b3RevoluteJoint_GetAngle(s_codexJoint[BONE_R_SHIN]);
    out[2] = b3RevoluteJoint_GetAngle(s_codexJoint[CODEX_L_TOE]);
    out[3] = b3RevoluteJoint_GetAngle(s_codexJoint[CODEX_R_TOE]);
    return out;
}
