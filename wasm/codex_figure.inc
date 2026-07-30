// codex_figure.inc — the independent red, physics-first comparison figure.
// Included by bridge.c so it shares the real world and voxel primitives.

#define CODEX_CATEGORY 0x8ULL
#define CODEX_BONE_COUNT (BONE_COUNT + 2)
#define CODEX_L_TOE BONE_COUNT
#define CODEX_R_TOE (BONE_COUNT + 1)

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
}

static float codexPartLoad(int bone)
{
    if (!s_codexExists || bone < 0 || bone >= CODEX_BONE_COUNT ||
        !s_codexAttached[bone]) return 0.0f;
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
        if (!s_codexAttached[b]) continue;
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
    const float sx = 0.5f * ((float)lf.x + (float)rf.x);
    const float sz = 0.5f * ((float)lf.z + (float)rf.z);
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
    s_codexDt = dt > 1e-5f ? dt : 1.0f / 72.0f;
    s_codexPlayer.x = px; s_codexPlayer.y = py; s_codexPlayer.z = pz;
    codexSense();
    codexBalance();
}

WASM_EXPORT("w_codex_post")
void w_codex_post(void) {}

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
        if (s_codexAttached[b]) on++;
        const int g = s_codexGrid[b];
        if (g < 0 || !s_vxG[g].used) continue;
        alive += s_vxG[g].alive;
        total += s_vxG[g].n[0] * s_vxG[g].n[1] * s_vxG[g].n[2];
    }
    const b3Pos p = b3Body_GetPosition(s_codexBone[BONE_PELVIS]);
    out[0] = 1.0f; out[1] = 0.0f; out[2] = (float)on;
    out[3] = (float)alive; out[4] = (float)total;
    out[6] = (float)p.x; out[7] = (float)p.y; out[8] = (float)p.z;
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
    float mass = 0.0f;
    if (s_codexExists)
        for (int b = 0; b < CODEX_BONE_COUNT; b++)
            if (s_codexAttached[b]) mass += b3Body_GetMass(s_codexBone[b]);
    out[0] = mass;
    out[1] = s_worldFloorY + BONES[BONE_PELVIS].oy * s_codexStature;
    return out;
}
