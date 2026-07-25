// bench.c — headless Box3D physics benchmark.
//
// Box3D is a physics-only library: it computes motion and collisions and draws
// nothing. So the physics half of "how much can a Quest 3 hold" can be measured
// on any machine, with no VR, no rendering and no Android involved. This is the
// desktop harness for that — the native counterpart to wasm/test.js.
//
// What it answers:
//   * does step time grow linearly with body count, or worse?
//   * does SIMD actually help, and by how much?
//   * does threading actually scale, and at what worker count does it stop
//     paying?
//   * did a change to the physics code cause a regression?
//
// What it CANNOT answer: absolute Quest numbers. A desktop or CI CPU is far
// faster than the XR2 Gen 2, and this says nothing about GPU or render cost.
// It gives relative scaling and regression detection. The absolute ceiling comes
// from the on-device benchmark.
//
// Usage: bench [workerCount] [maxBodies]
// Output: one CSV row per body count on stdout, plus a header line.
#include <box3d/box3d.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Scene parameters chosen to mirror the real thing: 20 cm cubes, the same
// material settings as physics.cpp, dropped so they land in a contact-rich pile
// rather than settling into a tidy grid.
#define HALF_EXTENT   0.10f
#define SUB_STEPS     4
#define DT            (1.0f / 72.0f)
#define WARMUP_STEPS  30
#define MEASURE_STEPS 240

static double nowSeconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int compareDouble(const void* a, const void* b)
{
    const double x = *(const double*)a;
    const double y = *(const double*)b;
    return (x > y) - (x < y);
}

// Deterministic PRNG so every run builds an identical scene. Without this the
// numbers wander between runs and regressions hide in the noise.
static unsigned s_rng = 0x9e3779b9u;
static float frnd(float lo, float hi)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return lo + (hi - lo) * ((float)(s_rng >> 8) / 16777216.0f);
}

static void addGround(b3WorldId world)
{
    b3BodyDef bd = b3DefaultBodyDef();
    bd.type = b3_staticBody;
    bd.position.x = 0.0f;
    bd.position.y = -0.05f;
    bd.position.z = 0.0f;

    b3BodyId body = b3CreateBody(world, &bd);

    b3ShapeDef sd = b3DefaultShapeDef();
    sd.density = 1.0f;
    sd.baseMaterial.friction = 0.6f;
    sd.baseMaterial.restitution = 0.05f;

    b3BoxHull hull = b3MakeBoxHull(20.0f, 0.05f, 20.0f);
    b3CreateHullShape(body, &sd, &hull.base);
}

// Stack `count` cubes into a loose cuboid with jitter, high enough that they
// fall, collide and pile up. Contact-rich motion is the expensive case and the
// one that matters — a settled pile is nearly free once bodies sleep.
static void addCubes(b3WorldId world, int count)
{
    const float spacing = HALF_EXTENT * 2.6f;
    const int perRow = 12;

    for (int i = 0; i < count; ++i)
    {
        const int x = i % perRow;
        const int z = (i / perRow) % perRow;
        const int y = i / (perRow * perRow);

        b3BodyDef bd = b3DefaultBodyDef();
        bd.type = b3_dynamicBody;
        bd.position.x = ((float)x - perRow * 0.5f) * spacing + frnd(-0.01f, 0.01f);
        bd.position.y = 0.5f + (float)y * spacing;
        bd.position.z = ((float)z - perRow * 0.5f) * spacing + frnd(-0.01f, 0.01f);
        bd.angularVelocity.x = frnd(-1.0f, 1.0f);
        bd.angularVelocity.y = frnd(-1.0f, 1.0f);
        bd.angularVelocity.z = frnd(-1.0f, 1.0f);

        b3BodyId body = b3CreateBody(world, &bd);

        b3ShapeDef sd = b3DefaultShapeDef();
        sd.density = 1.0f;
        sd.baseMaterial.friction = 0.6f;
        sd.baseMaterial.restitution = 0.05f;

        b3BoxHull hull = b3MakeBoxHull(HALF_EXTENT, HALF_EXTENT, HALF_EXTENT);
        b3CreateHullShape(body, &sd, &hull.base);
    }
}

// Run one body count and print a CSV row. `sleep` selects the regime:
//   enabled  — bodies settle and drop out of the solver (the flattering number)
//   disabled — nothing ever sleeps, so every body stays in the solver every
//              step (the fight, and the number that matters)
static void runCase(int bodyCount, unsigned workerCount, bool enableSleep)
{
    s_rng = 0x9e3779b9u; // identical scene every case

    b3WorldDef wd = b3DefaultWorldDef();
    wd.gravity.x = 0.0f;
    wd.gravity.y = -9.81f;
    wd.gravity.z = 0.0f;
    wd.workerCount = workerCount;
    wd.enableSleep = enableSleep;

    b3WorldId world = b3CreateWorld(&wd);

    addGround(world);
    addCubes(world, bodyCount);

    for (int i = 0; i < WARMUP_STEPS; ++i)
    {
        b3World_Step(world, DT, SUB_STEPS);
    }

    double* samples = (double*)malloc(sizeof(double) * MEASURE_STEPS);
    if (samples == NULL)
    {
        fprintf(stderr, "out of memory\n");
        exit(1);
    }

    double total = 0.0;
    for (int i = 0; i < MEASURE_STEPS; ++i)
    {
        const double t0 = nowSeconds();
        b3World_Step(world, DT, SUB_STEPS);
        const double ms = (nowSeconds() - t0) * 1000.0;
        samples[i] = ms;
        total += ms;
    }

    qsort(samples, MEASURE_STEPS, sizeof(double), compareDouble);
    const double mean = total / MEASURE_STEPS;
    const double p50 = samples[MEASURE_STEPS / 2];
    const double p99 = samples[(int)(MEASURE_STEPS * 0.99)];

    printf("%d,%u,%s,%.4f,%.4f,%.4f\n", bodyCount, workerCount,
           enableSleep ? "sleep" : "awake", mean, p50, p99);
    fflush(stdout);

    free(samples);
    b3DestroyWorld(world);
}

int main(int argc, char** argv)
{
    const unsigned workerCount = (argc > 1) ? (unsigned)atoi(argv[1]) : 1u;
    const int maxBodies = (argc > 2) ? atoi(argv[2]) : 4000;

    // The ramp. Doubling keeps the run short while still showing the shape of
    // the curve and where it bends.
    const int ramp[] = {50, 100, 200, 400, 800, 1600, 3200, 6400};
    const int rampCount = (int)(sizeof(ramp) / sizeof(ramp[0]));

    printf("bodies,workers,regime,mean_ms,p50_ms,p99_ms\n");

    for (int i = 0; i < rampCount; ++i)
    {
        if (ramp[i] > maxBodies)
        {
            break;
        }
        runCase(ramp[i], workerCount, false); // awake: the number that matters
        runCase(ramp[i], workerCount, true);  // sleep: the flattering number
    }

    return 0;
}
