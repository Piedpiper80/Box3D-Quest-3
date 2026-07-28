// physics.h
//
// Thin, Box3D-free interface to the physics world. main.cpp (the OpenXR /
// renderer side) talks to the simulation only through these functions, so it
// never has to include or know about Box3D types. All Box3D usage is contained
// in physics.cpp.
//
// Coordinate system: right-handed, Y-up, meters — the same convention OpenXR
// uses for its reference spaces, so physics coordinates map 1:1 onto the world
// the headset renders. The floor plane sits at y = 0, matching the player's
// real floor when a STAGE reference space is available.
#pragma once

// Body budget. This is also the render instance budget — every body is drawn,
// so the two are the same number by definition and must not be allowed to drift
// apart. The renderer sizes its instance buffer from this.
constexpr int kMaxBodies = 4096;

// One drawable object handed to the renderer each frame.
//
// The layout is also the GPU instance-buffer layout: `Physics_BuildRenderItems`
// writes an array of these straight into a vertex buffer and the whole scene is
// drawn in one instanced call. Hence the padded colour — a vec4 keeps the
// 80-byte stride 16-byte aligned, which is what GL wants for the mat4 attribute
// that precedes it.
struct RenderItem
{
    float model[16]; // column-major 4x4 model matrix (rotation * scale, then translation)
    float color[4];  // rgb in [0,1]; w is padding, unused by the shader
};

// Create the world, gravity, ground, and the initial stack of boxes.
void Physics_Init();

// Destroy the world and free all bodies.
void Physics_Shutdown();

// Advance the simulation by dt seconds (internally sub-stepped by Box3D).
void Physics_Step(float dt);

// Spawn a dynamic cube of the given half extent at world position `pos`,
// launched with linear velocity `vel`. Used when the player pulls a trigger.
// Silently ignored once the body budget is full.
void Physics_SpawnBox(const float pos[3], const float vel[3], float halfExtent, const float color[3]);

// Fill `items` with the current transform + color of every body (ground first).
// Returns the number written, never exceeding maxItems.
int Physics_BuildRenderItems(RenderItem* items, int maxItems);

// ---------------------------------------------------------------------------
// Benchmark support
//
// Used by the automated on-device benchmark (benchmark.h) to build controlled
// scenes. Not used by normal gameplay.
// ---------------------------------------------------------------------------

// Tear the world down and rebuild it with only the ground, leaving it empty and
// ready to be filled to a specific body count.
//
// `enableSleep` selects the regime being measured. With sleep on, settled bodies
// leave the solver and cost almost nothing — the flattering number. With it off,
// every body is solved every step regardless of whether it has come to rest,
// which is what a fight actually looks like and the number that matters.
void Physics_Reset(bool enableSleep);

// Drop `count` dynamic cubes in a loose jittered cuboid, high enough that they
// fall, collide and pile up. Mirrors the headless harness in bench/bench.c so
// the two sets of numbers describe the same scene.
void Physics_SpawnPile(int count);

// Total bodies in the world, including the static ground.
int Physics_BodyCount();
