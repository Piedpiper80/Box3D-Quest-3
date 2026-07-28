// benchmark.h
//
// Automated, unattended on-device performance measurement — Phase 0 Step 2b.
//
// The point of this file is that measuring the engine must not require a person
// throwing cubes until it feels bad. That is not reproducible, it depends on how
// fast someone can pull a trigger, and it yields one fuzzy data point instead of
// a curve.
//
// Instead: launch the app, put the headset down, come back in a couple of
// minutes. The benchmark ramps the body count, holds each level in two regimes,
// and stops when frame time consistently misses budget.
//
// Results go to **logcat**, not to the on-screen HUD. That is deliberate: the
// HUD is unverified VR text rendering, and if it draws garbage the measurement
// must still survive. Capture a run with:
//
//     adb logcat -s Box3DQuest:I
//
// Lines are CSV with a stable `BENCH,` prefix, so the run can be grepped out of
// surrounding log noise and pasted straight into a spreadsheet.
#pragma once

// Begin an automated run. Safe to call more than once; ignored while a run is
// already in progress.
void Benchmark_Start();

// True while a run is in progress. The app suppresses normal interaction (and
// the tower scene) during a run so nothing perturbs the measurement.
bool Benchmark_Active();

// Drive the run. Called once per frame with the timings just measured, after
// physics has stepped. `frameMs` is wall-clock between frame starts; `stepMs` is
// time spent inside the solver alone, so the two can be compared to see whether
// physics or rendering hit the wall first.
void Benchmark_Update(double frameMs, double stepMs);

// Human-readable one-line status for the HUD, e.g. "bench 800 awake 43%".
// Returns nullptr when no run is in progress.
const char* Benchmark_StatusLine();
