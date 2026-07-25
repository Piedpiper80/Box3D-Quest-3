// physics.cpp
//
// Box3D (github.com/erincatto/box3d) integration. Box3D is a physics-only
// engine: it computes where bodies are and how they collide, but draws
// nothing. We create a world, populate it with box-shaped bodies, step it, and
// each frame read back every body's position/orientation as a model matrix the
// GL renderer can use.
//
// Box3D API notes (from include/box3d, main branch):
//   * A box collision shape is a convex hull. b3MakeBoxHull(hx,hy,hz) returns a
//     b3BoxHull whose first member is a b3HullData `base`; we pass &hull.base to
//     b3CreateHullShape.
//   * Positions are b3Pos (single precision x,y,z in the default build),
//     rotations are b3Quat ({b3Vec3 v; float s}).
//   * b3MakeMatrixFromQuat(q) yields a column-major b3Matrix3 (columns cx,cy,cz).
#include "physics.h"

#include <box3d/box3d.h>

#include <vector>

namespace {

struct BodyRec
{
    b3BodyId id;
    float    half[3];
    float    color[3];
};

b3WorldId            g_world = b3_nullWorldId;
std::vector<BodyRec> g_bodies;

// kMaxBodies is declared in physics.h — it is shared with the renderer, which
// sizes its instance buffer from the same constant.

// Build a column-major GL model matrix for a body: rotation (from the body's
// quaternion) scaled by its half extents, with the body position as translation.
// The cube mesh spans [-1,1] on each axis, so scaling the rotation columns by
// the half extents gives the correct box size.
void makeModel(const BodyRec& rec, float out[16])
{
    b3Pos     p = b3Body_GetPosition(rec.id);
    b3Quat    q = b3Body_GetRotation(rec.id);
    b3Matrix3 r = b3MakeMatrixFromQuat(q);

    out[0]  = r.cx.x * rec.half[0];
    out[1]  = r.cx.y * rec.half[0];
    out[2]  = r.cx.z * rec.half[0];
    out[3]  = 0.0f;

    out[4]  = r.cy.x * rec.half[1];
    out[5]  = r.cy.y * rec.half[1];
    out[6]  = r.cy.z * rec.half[1];
    out[7]  = 0.0f;

    out[8]  = r.cz.x * rec.half[2];
    out[9]  = r.cz.y * rec.half[2];
    out[10] = r.cz.z * rec.half[2];
    out[11] = 0.0f;

    out[12] = static_cast<float>(p.x);
    out[13] = static_cast<float>(p.y);
    out[14] = static_cast<float>(p.z);
    out[15] = 1.0f;
}

// Create a box body and register it for rendering.
void addBox(b3BodyType type, const float pos[3], const float half[3], float density,
            const float vel[3], const float color[3])
{
    if (static_cast<int>(g_bodies.size()) >= kMaxBodies)
    {
        return;
    }

    b3BodyDef bd = b3DefaultBodyDef();
    bd.type       = type;
    bd.position.x = pos[0];
    bd.position.y = pos[1];
    bd.position.z = pos[2];
    if (vel != nullptr)
    {
        bd.linearVelocity.x = vel[0];
        bd.linearVelocity.y = vel[1];
        bd.linearVelocity.z = vel[2];
    }

    b3BodyId body = b3CreateBody(g_world, &bd);

    b3ShapeDef sd            = b3DefaultShapeDef();
    sd.density               = density;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;

    b3BoxHull hull = b3MakeBoxHull(half[0], half[1], half[2]);
    b3CreateHullShape(body, &sd, &hull.base);

    BodyRec rec;
    rec.id      = body;
    rec.half[0] = half[0];
    rec.half[1] = half[1];
    rec.half[2] = half[2];
    rec.color[0] = color[0];
    rec.color[1] = color[1];
    rec.color[2] = color[2];
    g_bodies.push_back(rec);
}

} // namespace

void Physics_Init()
{
    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = -9.81f;
    wd.gravity.z  = 0.0f;

    // Multithreaded solve. Leaving enqueueTask/finishTask null makes Box3D spawn
    // and own its worker threads, which is all this needs — a custom task system
    // would only be worth it to share workers with other subsystems or to pin
    // thread affinity.
    //
    // 4 workers: the Quest 3's XR2 Gen 2 has 8 cores, and the render thread plus
    // the OpenXR runtime need room. Measured in CI (bench/): threading is worth
    // up to 2.5x at high body counts, but is a small net *loss* below ~100
    // bodies where synchronisation costs more than it saves. Real scenes are far
    // past that crossover.
    wd.workerCount = 4;

    g_world = b3CreateWorld(&wd);

    g_bodies.clear();
    g_bodies.reserve(kMaxBodies);

    // Ground: a large, thin static box centered at the floor plane (y = 0).
    const float groundPos[3]  = {0.0f, -0.05f, 0.0f};
    const float groundHalf[3] = {6.0f, 0.05f, 6.0f};
    const float groundColor[3] = {0.35f, 0.37f, 0.42f};
    addBox(b3_staticBody, groundPos, groundHalf, 1.0f, nullptr, groundColor);

    // A tower of cubes ~1.5 m in front of the player that settles under gravity.
    // A tiny per-layer horizontal offset makes the stack lean and tumble so the
    // physics is immediately visible.
    const float cube       = 0.10f; // 20 cm cubes (half extent 0.10)
    const int   levels     = 12;
    const float palette[6][3] = {
        {0.90f, 0.30f, 0.24f}, {0.95f, 0.61f, 0.20f}, {0.96f, 0.85f, 0.30f},
        {0.30f, 0.69f, 0.44f}, {0.26f, 0.52f, 0.86f}, {0.55f, 0.36f, 0.78f},
    };
    for (int i = 0; i < levels; ++i)
    {
        const float lean = 0.012f * static_cast<float>(i);
        const float pos[3]  = {lean, 0.12f + (2.0f * cube + 0.01f) * static_cast<float>(i), -1.5f};
        const float half[3] = {cube, cube, cube};
        addBox(b3_dynamicBody, pos, half, 1.0f, nullptr, palette[i % 6]);
    }
}

void Physics_Shutdown()
{
    if (b3World_IsValid(g_world))
    {
        b3DestroyWorld(g_world);
    }
    g_world = b3_nullWorldId;
    g_bodies.clear();
}

void Physics_Step(float dt)
{
    if (!b3World_IsValid(g_world))
    {
        return;
    }
    // 4 solver sub-steps is Box3D's usual sweet spot for stability vs cost.
    b3World_Step(g_world, dt, 4);
}

void Physics_SpawnBox(const float pos[3], const float vel[3], float halfExtent, const float color[3])
{
    const float half[3] = {halfExtent, halfExtent, halfExtent};
    addBox(b3_dynamicBody, pos, half, 1.0f, vel, color);
}

int Physics_BuildRenderItems(RenderItem* items, int maxItems)
{
    int count = 0;
    for (const BodyRec& rec : g_bodies)
    {
        if (count >= maxItems)
        {
            break;
        }
        makeModel(rec, items[count].model);
        items[count].color[0] = rec.color[0];
        items[count].color[1] = rec.color[1];
        items[count].color[2] = rec.color[2];
        items[count].color[3] = 1.0f;
        ++count;
    }
    return count;
}
