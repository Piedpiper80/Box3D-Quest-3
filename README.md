# Box3D Quest VR

A **standalone Meta Quest 3 VR game** built around the brand-new
[**Box3D**](https://github.com/erincatto/box3d) physics engine (from Erin
Catto, creator of Box2D). This repo is the complete home for the project: a
**playable browser build** and the **native OpenXR app** source.

## ▶ Play it right now — headset only, nothing to install

Open **<https://piedpiper80.github.io/Box3D-Quest-3/>** in the Meta Quest
Browser and tap **Enter VR**. Boxes tumble and bounce around your room; pull
either trigger to throw more.

That build is [`docs/index.html`](docs/index.html) + [`docs/box3d.wasm`](docs/box3d.wasm):
the **real Box3D engine compiled to WebAssembly** (~360 KB, scalar,
single-threaded) drives the scene, with an automatic stand-in fallback if the
wasm can't load. The engine badge on the page shows which one is running.
Published via GitHub Pages from the `gh-pages` branch; see [`wasm/`](wasm/)
for the C bridge, build command, and simulation test harness.

## The native app (this repo's C/C++ source)

The rest of this repo is the full native version: a real installed Quest app
written against OpenXR in C/C++ with genuine Box3D physics. It installs onto
the headset and runs entirely on-device — no PC, no cable, no browser — but
**building it requires a computer** with Android Studio (steps below).

> **Status: foundation / vertical slice.** Real Box3D physics rendered in
> stereo VR, plus controller interaction. Built to grow into a full game.

## What you'll see

- A floor (aligned to your real floor via the VR guardian/stage) and a **tower
  of colored cubes** that topples and settles under Box3D gravity — real 3D
  rigid-body physics, collisions, and stacking.
- **Pull either trigger to throw a new cube** out of your controller. Bury the
  scene in boxes and watch them pile up.

## Controls

| Input | Action |
|-------|--------|
| Move / look | Walk around physically (roomscale) |
| Left or Right **Trigger** | Throw a cube in the direction you're pointing |

---

## Requirements

- A **Meta Quest 3** (also works on Quest 2 / Pro / 3S) with **Developer Mode**
  enabled (see below).
- **[Android Studio](https://developer.android.com/studio)** (latest stable).
- Android **NDK** and **CMake** (installed from inside Android Studio — steps
  below). No manual OpenXR SDK download is needed; the loader comes from Maven.
- A USB-C cable to connect the headset for installing.
- Internet on the build machine the first time (Gradle downloads the OpenXR
  loader; CMake downloads the Box3D source).

---

## Build & install (recommended: Android Studio)

1. **Get the code onto your computer**
   ```bash
   git clone <this-repo-url>
   cd box3d-quest3-vr
   ```

2. **Open it in Android Studio** — `File ▸ Open` and select the
   `box3d-quest3-vr` folder. Let it finish the initial Gradle sync. If it offers
   to create a Gradle wrapper or use a bundled Gradle, accept.

3. **Install the NDK + CMake** (one time): `Tools ▸ SDK Manager ▸ SDK Tools`,
   tick **NDK (Side by side)** and **CMake**, click Apply. Android Studio will
   also download the Box3D source and the OpenXR loader during the next build.

4. **Enable Developer Mode on the Quest** (one time):
   - In the **Meta Horizon** phone app: `Menu ▸ Devices ▸` your headset `▸
     Headset Settings ▸ Developer Mode ▸ On`. (Creating a free Meta developer
     account/organization at <https://developer.meta.com> is required.)
   - Put the headset on, plug it into the computer, and **Allow USB debugging**
     when the prompt appears inside the headset (check "Always allow").

5. **Build & run**: pick your Quest in the device dropdown at the top of Android
   Studio and press **▶ Run** (or `Build ▸ Build App Bundle(s) / APK(s) ▸ Build
   APK(s)` to just produce the file). The app installs and launches on the
   headset.

6. **Find it later on the headset**: it appears in your app library under
   **Unknown Sources** (the dropdown filter in the Quest's app grid), named
   **Box3D Quest VR**.

### Alternative: command line

If you prefer the terminal (requires a local Gradle or the wrapper, plus
`adb` from Android platform-tools):

```bash
./gradlew assembleDebug          # build the APK
adb install -r app/build/outputs/apk/debug/app-debug.apk   # install to the connected Quest
```

---

## Project structure

```
box3d-quest3-vr/
├── app/
│   ├── build.gradle                 # Android module config, OpenXR loader dependency
│   └── src/main/
│       ├── AndroidManifest.xml      # Quest/OpenXR VR app manifest
│       ├── res/values/strings.xml
│       └── cpp/
│           ├── CMakeLists.txt        # fetches Box3D, links OpenXR + GLES
│           ├── main.cpp              # OpenXR session, EGL/GLES render loop, input
│           ├── physics.cpp / .h      # Box3D world, ground, boxes, throw logic
│           ├── gl_helpers.h          # shader + cube-mesh helpers
│           └── math3d.h              # VR projection / view matrix math
├── build.gradle                     # top-level, plugin versions
├── settings.gradle
└── gradle.properties
```

## How it works

Box3D and the renderer are two separate jobs:

- **Box3D** (`physics.cpp`) owns the *simulation*: it creates a world with
  gravity, a static ground box, and dynamic box bodies, steps them each frame,
  and reports every body's position + orientation. It draws nothing.
- **OpenXR + OpenGL ES** (`main.cpp`) owns the *headset and the picture*: it
  opens a VR session, renders each eye, and each frame asks Box3D for the
  current transforms and draws a cube for each body.

Boxes in Box3D are convex **hull** shapes — `b3MakeBoxHull(hx,hy,hz)` builds one,
and its embedded `b3HullData base` is handed to `b3CreateHullShape`.

## Tuning & next steps

- **The scene** lives in `Physics_Init()` in `physics.cpp` — change the tower
  height, cube sizes, colors, or gravity there.
- **Throw force / cube size** for the trigger are in `handleInput()` in
  `main.cpp` (`speed` and the `0.06f` half-extent).
- Natural next features: grab & hold boxes, spawn different shapes (Box3D also
  has spheres and capsules), targets/scoring, sound.

## Troubleshooting

- **Gradle can't resolve `openxr_loader_for_android:1.1.43`** — bump the version
  in `app/build.gradle` to the latest on
  [Maven Central](https://central.sonatype.com/artifact/org.khronos.openxr/openxr_loader_for_android).
- **Box3D fails to compile (SIMD/intrinsics errors)** — SIMD is already disabled
  by default (`BOX3D_DISABLE_SIMD ON` in `CMakeLists.txt`) for exactly this
  reason. If you see a different Box3D error, note that it's fetched from the
  `main` branch; you can pin a specific commit via `GIT_TAG` in `CMakeLists.txt`.
- **CMake error: unknown target `box3d`** — the Box3D library target is `box3d`;
  if a future version renames it, check `third_party` build output / the
  fetched `box3d/src/CMakeLists.txt` and update the name in `target_link_libraries`.
- **App installs but shows a black screen** — check `adb logcat -s Box3DQuest`
  for the `OpenXR failed (...)` lines; they pinpoint which call failed.
- **Triggers don't spawn boxes** — make sure the controllers are on and tracked;
  the app binds both the simple-controller `select` and Touch `trigger` inputs.
- **Can't see the headset in Android Studio** — confirm Developer Mode is on,
  the USB cable carries data, and you accepted the USB-debugging prompt inside
  the headset. `adb devices` should list it.

## Credits

- Physics: [Box3D](https://github.com/erincatto/box3d) by Erin Catto (MIT).
- VR: [OpenXR](https://www.khronos.org/openxr/) with the Khronos Android loader.
