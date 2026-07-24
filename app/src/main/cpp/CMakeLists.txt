# Native build for the Box3D Quest VR app.
#
# Produces libbox3dquest.so, which the NativeActivity loads (see the
# android.app.lib_name meta-data in AndroidManifest.xml). Box3D is pulled from
# source with FetchContent so there is no submodule to initialize; the OpenXR
# loader comes from the org.khronos.openxr AAR via Prefab.
cmake_minimum_required(VERSION 3.22.1)
project(box3dquest LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Box3D physics engine (built from source)
# ---------------------------------------------------------------------------
include(FetchContent)

# Build only the core library — no samples, tests, benchmarks or docs.
set(BOX3D_SAMPLES    OFF CACHE BOOL "" FORCE)
set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX3D_DOCS       OFF CACHE BOOL "" FORCE)
set(BOX3D_PROFILE    OFF CACHE BOOL "" FORCE)
set(BOX3D_VALIDATE   OFF CACHE BOOL "" FORCE)

# Disable SIMD for the first build so it is guaranteed to compile on arm64.
# Once you confirm the app runs, you can try setting this to OFF for a little
# extra performance and rebuild.
set(BOX3D_DISABLE_SIMD ON CACHE BOOL "" FORCE)

# Pinned to a known-good commit: this is the exact Box3D revision the physics
# code was written against. Box3D is brand new and its 'main' branch moves, so
# bump this SHA deliberately (and re-test) rather than tracking a branch.
FetchContent_Declare(
    box3d
    GIT_REPOSITORY https://github.com/erincatto/box3d.git
    GIT_TAG        c52908c9a907714e4d3a8a30be5272a1761158e1
)
FetchContent_MakeAvailable(box3d)

# ---------------------------------------------------------------------------
# OpenXR loader (provided by the AAR dependency through Prefab)
# ---------------------------------------------------------------------------
find_package(OpenXR REQUIRED CONFIG)

# ---------------------------------------------------------------------------
# Android native activity glue (ships with the NDK)
# ---------------------------------------------------------------------------
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# ---------------------------------------------------------------------------
# The application shared library
# ---------------------------------------------------------------------------
add_library(box3dquest SHARED
    main.cpp
    physics.cpp)

target_include_directories(box3dquest PRIVATE
    ${ANDROID_NDK}/sources/android/native_app_glue)

target_link_libraries(box3dquest
    native_app_glue
    box3d
    OpenXR::openxr_loader
    android
    EGL
    GLESv3
    log)

# native_app_glue provides ANativeActivity_onCreate; force the linker to keep it
# so the NativeActivity can find the entry point in the stripped .so.
target_link_options(box3dquest PRIVATE "-u" "ANativeActivity_onCreate")
