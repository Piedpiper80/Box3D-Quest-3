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

// One drawable object handed to the renderer each frame.
struct RenderItem
{
    float model[16]; // column-major 4x4 model matrix (rotation * scale, then translation)
    float color[3];  // rgb in [0,1]
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
