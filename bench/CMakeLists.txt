# Headless physics benchmark — desktop/CI build, no Android, no VR, no GL.
#
# Standalone on purpose: it does not participate in the Gradle build and pulls
# its own copy of Box3D, so it can be built and run anywhere a C compiler and
# network access exist. See bench.c for what it can and cannot tell you.
#
# Build:
#   cmake -S bench -B build/bench -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/bench
#   ./build/bench/bench <workerCount> <maxBodies>
#
# SIMD is on by default here. Pass -DBOX3D_DISABLE_SIMD=ON to measure the
# scalar path — that comparison is the point of Step 5.
cmake_minimum_required(VERSION 3.22)
project(box3dbench LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(FetchContent)

set(BOX3D_SAMPLES    OFF CACHE BOOL "" FORCE)
set(BOX3D_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(BOX3D_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(BOX3D_DOCS       OFF CACHE BOOL "" FORCE)
set(BOX3D_PROFILE    OFF CACHE BOOL "" FORCE)
set(BOX3D_VALIDATE   OFF CACHE BOOL "" FORCE)

# Must match the revision in app/src/main/cpp/CMakeLists.txt, or the benchmark
# measures a different engine than the one the app ships. Bump both together.
FetchContent_Declare(
    box3d
    GIT_REPOSITORY https://github.com/erincatto/box3d.git
    GIT_TAG        c52908c9a907714e4d3a8a30be5272a1761158e1
)
FetchContent_MakeAvailable(box3d)

add_executable(bench bench.c)
target_link_libraries(bench PRIVATE box3d)

# clock_gettime lives in librt on older glibc.
include(CheckLibraryExists)
check_library_exists(rt clock_gettime "" HAVE_CLOCK_GETTIME_RT)
if(HAVE_CLOCK_GETTIME_RT)
    target_link_libraries(bench PRIVATE rt)
endif()

find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
    target_link_libraries(bench PRIVATE ${MATH_LIBRARY})
endif()
