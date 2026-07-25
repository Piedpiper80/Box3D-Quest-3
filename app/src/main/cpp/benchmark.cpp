// benchmark.cpp
//
// The ramp state machine described in benchmark.h.
//
// Structure mirrors bench/bench.c deliberately: same cube size, same pile
// layout, same ramp, same two regimes. The headless harness measures the solver
// on a fast desktop core; this measures the whole frame on the actual device.
// Keeping the scenes identical is what makes the two sets of numbers comparable,
// and therefore what lets the headless harness stand in for the device during
// day-to-day work.
#include "benchmark.h"

#include "physics.h"

#include <android/log.h>

#include <cstdio>

#define BLOGI(...) __android_log_print(ANDROID_LOG_INFO, "Box3DQuest", __VA_ARGS__)

namespace {

// Body counts to sweep. Doubling keeps a full run to a couple of minutes while
// still showing the shape of the curve and where it bends.
const int kRamp[]   = {50, 100, 200, 400, 800, 1600, 3200};
const int kRampSize = static_cast<int>(sizeof(kRamp) / sizeof(kRamp[0]));

// Frames discarded at each level before measuring, to let the pile form and to
// let any allocation spike from spawning wash out.
constexpr int kWarmupFrames = 45;

// Frames measured per level. At 72 Hz this is about three seconds — long enough
// for a stable 99th percentile without making a full run tedious.
constexpr int kMeasureFrames = 216;

// Frame budget at 72 Hz. A level whose 99th-percentile frame time exceeds this
// has missed, and two consecutive misses end the run: past that point the
// numbers only describe how badly it fails.
constexpr double kFrameBudgetMs   = 1000.0 / 72.0;
constexpr int    kMissesToGiveUp  = 2;

enum class Regime
{
    Awake, // nothing sleeps: every body solved every step. The fight.
    Sleep, // settled bodies leave the solver. The flattering number.
};

struct State
{
    bool   active       = false;
    int    rampIndex    = 0;
    Regime regime       = Regime::Awake;
    int    warmupLeft   = 0;
    int    sampleCount  = 0;
    int    misses       = 0;

    double frameMs[kMeasureFrames] = {};
    double stepMs[kMeasureFrames]  = {};

    char status[64] = {0};
};

State g_state;

const char* regimeName(Regime r)
{
    return r == Regime::Awake ? "awake" : "sleep";
}

// Insertion sort. n is a few hundred and this runs once per level, so the
// simplest correct thing is the right thing — no need for qsort's indirection.
void sortAscending(double* v, int n)
{
    for (int i = 1; i < n; ++i)
    {
        const double key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key)
        {
            v[j + 1] = v[j];
            --j;
        }
        v[j + 1] = key;
    }
}

void summarise(const double* values, int n, double& mean, double& p50, double& p99)
{
    static double sorted[kMeasureFrames];
    double total = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sorted[i] = values[i];
        total += values[i];
    }
    sortAscending(sorted, n);
    mean = total / n;
    p50  = sorted[n / 2];
    p99  = sorted[static_cast<int>(n * 0.99)];
}

// Build the scene for the current ramp index and regime, and reset counters.
void beginLevel()
{
    const int bodies = kRamp[g_state.rampIndex];

    Physics_Reset(g_state.regime == Regime::Sleep);
    Physics_SpawnPile(bodies);

    g_state.warmupLeft  = kWarmupFrames;
    g_state.sampleCount = 0;

    snprintf(g_state.status, sizeof(g_state.status), "bench %d %s warmup", bodies,
             regimeName(g_state.regime));
    BLOGI("BENCH,level,%d,%s", bodies, regimeName(g_state.regime));
}

void finish()
{
    g_state.active = false;
    snprintf(g_state.status, sizeof(g_state.status), "bench done");
    BLOGI("BENCH,end");
    BLOGI("BENCH,note,frame_ms is the whole frame; step_ms is the solver alone");

    // Leave the device in the normal interactive scene rather than on the last
    // benchmark pile, so the app is usable the moment the run ends.
    Physics_Init();
}

// Advance to the next (regime, body count) pair, or end the run.
void advance()
{
    if (g_state.regime == Regime::Awake)
    {
        g_state.regime = Regime::Sleep;
        beginLevel();
        return;
    }

    g_state.regime = Regime::Awake;
    ++g_state.rampIndex;

    if (g_state.rampIndex >= kRampSize || g_state.misses >= kMissesToGiveUp)
    {
        finish();
        return;
    }
    beginLevel();
}

} // namespace

void Benchmark_Start()
{
    if (g_state.active)
    {
        return;
    }

    g_state           = State{};
    g_state.active    = true;
    g_state.rampIndex = 0;
    g_state.regime    = Regime::Awake;

    BLOGI("BENCH,begin");
    BLOGI("BENCH,budget_ms,%.3f", kFrameBudgetMs);
    BLOGI("BENCH,header,bodies,regime,frame_mean,frame_p50,frame_p99,step_mean,step_p50,step_p99");
    beginLevel();
}

bool Benchmark_Active()
{
    return g_state.active;
}

const char* Benchmark_StatusLine()
{
    return g_state.active ? g_state.status : nullptr;
}

void Benchmark_Update(double frameMs, double stepMs)
{
    if (!g_state.active)
    {
        return;
    }

    if (g_state.warmupLeft > 0)
    {
        --g_state.warmupLeft;
        return;
    }

    if (g_state.sampleCount < kMeasureFrames)
    {
        g_state.frameMs[g_state.sampleCount] = frameMs;
        g_state.stepMs[g_state.sampleCount]  = stepMs;
        ++g_state.sampleCount;

        snprintf(g_state.status, sizeof(g_state.status), "bench %d %s %d%%",
                 kRamp[g_state.rampIndex], regimeName(g_state.regime),
                 (100 * g_state.sampleCount) / kMeasureFrames);
    }

    if (g_state.sampleCount < kMeasureFrames)
    {
        return;
    }

    double fMean, fP50, fP99;
    double sMean, sP50, sP99;
    summarise(g_state.frameMs, g_state.sampleCount, fMean, fP50, fP99);
    summarise(g_state.stepMs, g_state.sampleCount, sMean, sP50, sP99);

    BLOGI("BENCH,%d,%s,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", Physics_BodyCount(),
          regimeName(g_state.regime), fMean, fP50, fP99, sMean, sP50, sP99);

    // Only the awake regime counts toward giving up. A sleeping pile that misses
    // budget is a genuine surprise worth recording, but it is not the condition
    // the ramp exists to find.
    if (g_state.regime == Regime::Awake)
    {
        if (fP99 > kFrameBudgetMs)
        {
            ++g_state.misses;
            BLOGI("BENCH,miss,%d,%d", kRamp[g_state.rampIndex], g_state.misses);
        }
        else
        {
            g_state.misses = 0;
        }
    }

    advance();
}
