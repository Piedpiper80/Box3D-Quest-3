// main.cpp
//
// A minimal, standalone native OpenXR application for the Meta Quest 3 that
// renders a Box3D physics scene in stereo VR. It:
//   * brings up OpenXR (Android loader + instance + system + session),
//   * creates an EGL / OpenGL ES 3 context and per-eye swapchains,
//   * runs the frame loop (wait / begin / locate views / render / end),
//   * steps the Box3D world each frame and draws every body as a lit cube,
//   * lets the player pull either trigger to throw a new box into the scene.
//
// It is deliberately single-file and single-threaded for readability. The
// structure follows the well-trodden Khronos "hello_xr" / Meta
// "VrCubeWorld_NativeActivity" pattern.
//
// Rendering path: we never draw to the Android window. All rendering targets
// OpenXR swapchain textures, so an EGL pbuffer surface is enough to own a GL
// context.

#include <android_native_app_glue.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cmath>
#include <cstring>
#include <vector>

#include "benchmark.h"
#include "gl_helpers.h"
#include "hud.h"
#include "math3d.h"
#include "physics.h"

#include <cstdio>
#include <ctime>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "Box3DQuest", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Box3DQuest", __VA_ARGS__)

// Log-and-continue check for non-fatal OpenXR calls.
#define XR_CHECK(x)                                                                        \
    do                                                                                     \
    {                                                                                      \
        XrResult _res = (x);                                                               \
        if (XR_FAILED(_res))                                                               \
        {                                                                                  \
            LOGE("OpenXR failed (%d) at %s:%d -> %s", (int)_res, __FILE__, __LINE__, #x);  \
        }                                                                                  \
    } while (0)

namespace {

// Physics runs on a fixed step, decoupled from the display rate. 72 Hz matches
// the Quest's default refresh; the solver's behaviour no longer depends on how
// the frame loop happens to be performing.
constexpr float kPhysicsDt        = 1.0f / 72.0f;
constexpr int   kMaxStepsPerFrame = 3;
constexpr float kMaxAccumulated   = 0.25f; // discard anything beyond a quarter second

// Frames to wait after the session starts before the automated benchmark begins,
// giving the compositor and the first swapchain acquisitions time to settle so
// startup cost is not charged to the first ramp level.
constexpr int kFramesBeforeBenchmark = 120;

// Monotonic wall clock in milliseconds. Used only for measurement.
double nowMs()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

struct Swapchain
{
    XrSwapchain                                     handle = XR_NULL_HANDLE;
    int32_t                                         width  = 0;
    int32_t                                         height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR>        images;
    GLuint                                          depth = 0;
};

struct App
{
    android_app* app = nullptr;
    bool         exitRequested = false;
    bool         resumed       = false;

    // EGL
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig  config  = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    // OpenXR
    XrInstance      instance      = XR_NULL_HANDLE;
    XrSystemId      systemId      = XR_NULL_SYSTEM_ID;
    XrSession       session       = XR_NULL_HANDLE;
    XrSessionState  sessionState  = XR_SESSION_STATE_UNKNOWN;
    bool            sessionRunning = false;
    XrSpace         appSpace      = XR_NULL_HANDLE;

    XrViewConfigurationType            viewConfigType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    uint32_t                           viewCount      = 0;
    std::vector<XrViewConfigurationView> configViews;
    std::vector<XrView>                views;
    std::vector<Swapchain>             swapchains;

    // GL
    GLuint   program   = 0;
    GLint    uViewProj = -1;
    GLuint   fbo       = 0;
    CubeMesh cube;

    // Instances uploaded this frame, shared by both eyes.
    int instanceCount = 0;

    // Input
    XrActionSet actionSet      = XR_NULL_HANDLE;
    XrAction    poseAction     = XR_NULL_HANDLE;
    XrAction    triggerAction  = XR_NULL_HANDLE;
    XrPath      handPaths[2]   = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace     handSpace[2]   = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    bool        triggerWasPressed[2] = {false, false};

    // Timing
    XrTime lastDisplayTime    = 0;
    float  physicsAccumulator = 0.0f;

    // Measurement
    double lastFrameStartMs = 0.0;
    long   frameIndex       = 0;
};

// Model matrix and colour arrive per instance rather than as uniforms, so the
// whole scene is one draw call. Only the view-projection stays a uniform — it is
// the one thing that genuinely differs per eye.
const char* kVertexShader = R"GLSL(#version 300 es
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in mat4 aModel;   // occupies locations 2,3,4,5
layout(location = 6) in vec4 aColor;
uniform mat4 uViewProj;
out vec3 vNormal;
out vec3 vColor;
void main()
{
    vec4 world = aModel * vec4(aPos, 1.0);
    vNormal = mat3(aModel) * aNormal;
    vColor = aColor.rgb;
    gl_Position = uViewProj * world;
}
)GLSL";

const char* kFragmentShader = R"GLSL(#version 300 es
precision mediump float;
in vec3 vNormal;
in vec3 vColor;
out vec4 fragColor;
void main()
{
    vec3 n = normalize(vNormal);
    vec3 lightDir = normalize(vec3(0.4, 1.0, 0.3));
    float diff = max(dot(n, lightDir), 0.0);
    float ambient = 0.35;
    vec3 c = vColor * (ambient + 0.75 * diff);
    fragColor = vec4(c, 1.0);
}
)GLSL";

// Rotate vector v by unit quaternion q. (v' = v + 2*w*(q x v) + 2*(q x (q x v)))
void quatRotate(const XrQuaternionf& q, const float v[3], float out[3])
{
    const float tx = 2.0f * (q.y * v[2] - q.z * v[1]);
    const float ty = 2.0f * (q.z * v[0] - q.x * v[2]);
    const float tz = 2.0f * (q.x * v[1] - q.y * v[0]);
    out[0] = v[0] + q.w * tx + (q.y * tz - q.z * ty);
    out[1] = v[1] + q.w * ty + (q.z * tx - q.x * tz);
    out[2] = v[2] + q.w * tz + (q.x * ty - q.y * tx);
}

// ---------------------------------------------------------------------------
// OpenXR loader + instance + system
// ---------------------------------------------------------------------------

bool initLoader(android_app* app)
{
    PFN_xrInitializeLoaderKHR xrInitializeLoaderKHR = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&xrInitializeLoaderKHR)));
    if (xrInitializeLoaderKHR == nullptr)
    {
        LOGE("xrInitializeLoaderKHR unavailable");
        return false;
    }

    XrLoaderInitInfoAndroidKHR init{};
    init.type               = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR;
    init.applicationVM      = app->activity->vm;
    init.applicationContext = app->activity->clazz;
    XR_CHECK(xrInitializeLoaderKHR(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&init)));
    return true;
}

bool createInstance(App& a)
{
    XrInstanceCreateInfoAndroidKHR androidInfo{};
    androidInfo.type                = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR;
    androidInfo.applicationVM       = a.app->activity->vm;
    androidInfo.applicationActivity = a.app->activity->clazz;

    const char* extensions[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };

    XrInstanceCreateInfo ci{};
    ci.type                  = XR_TYPE_INSTANCE_CREATE_INFO;
    ci.next                  = &androidInfo;
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = extensions;
    std::strcpy(ci.applicationInfo.applicationName, "Box3D Quest VR");
    ci.applicationInfo.applicationVersion = 1;
    std::strcpy(ci.applicationInfo.engineName, "Box3D");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion    = XR_CURRENT_API_VERSION;

    XrResult r = xrCreateInstance(&ci, &a.instance);
    if (XR_FAILED(r))
    {
        LOGE("xrCreateInstance failed (%d)", (int)r);
        return false;
    }
    return true;
}

bool getSystem(App& a)
{
    XrSystemGetInfo sgi{};
    sgi.type       = XR_TYPE_SYSTEM_GET_INFO;
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(a.instance, &sgi, &a.systemId);
    if (XR_FAILED(r))
    {
        LOGE("xrGetSystem failed (%d)", (int)r);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// EGL / OpenGL ES context
// ---------------------------------------------------------------------------

bool initEgl(App& a)
{
    // OpenXR requires querying graphics requirements before session creation.
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetReqs = nullptr;
    XR_CHECK(xrGetInstanceProcAddr(a.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetReqs)));
    if (pfnGetReqs != nullptr)
    {
        XrGraphicsRequirementsOpenGLESKHR reqs{};
        reqs.type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR;
        XR_CHECK(pfnGetReqs(a.instance, a.systemId, &reqs));
    }

    a.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (a.display == EGL_NO_DISPLAY)
    {
        LOGE("eglGetDisplay failed");
        return false;
    }
    EGLint major = 0, minor = 0;
    if (eglInitialize(a.display, &major, &minor) == EGL_FALSE)
    {
        LOGE("eglInitialize failed");
        return false;
    }

    const EGLint configAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8,
        EGL_DEPTH_SIZE,      0,
        EGL_NONE,
    };
    EGLint numConfigs = 0;
    if (eglChooseConfig(a.display, configAttribs, &a.config, 1, &numConfigs) == EGL_FALSE ||
        numConfigs == 0)
    {
        LOGE("eglChooseConfig failed");
        return false;
    }

    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE,
    };
    a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, contextAttribs);
    if (a.context == EGL_NO_CONTEXT)
    {
        // Fall back to ES 3.0 if 3.2 is unavailable.
        const EGLint fallback[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
        a.context = eglCreateContext(a.display, a.config, EGL_NO_CONTEXT, fallback);
    }
    if (a.context == EGL_NO_CONTEXT)
    {
        LOGE("eglCreateContext failed");
        return false;
    }

    const EGLint surfaceAttribs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    a.surface = eglCreatePbufferSurface(a.display, a.config, surfaceAttribs);
    if (a.surface == EGL_NO_SURFACE)
    {
        LOGE("eglCreatePbufferSurface failed");
        return false;
    }

    if (eglMakeCurrent(a.display, a.surface, a.surface, a.context) == EGL_FALSE)
    {
        LOGE("eglMakeCurrent failed");
        return false;
    }
    LOGI("EGL initialized (v%d.%d)", major, minor);
    return true;
}

bool createSession(App& a)
{
    XrGraphicsBindingOpenGLESAndroidKHR binding{};
    binding.type    = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR;
    binding.display = a.display;
    binding.config  = a.config;
    binding.context = a.context;

    XrSessionCreateInfo sci{};
    sci.type     = XR_TYPE_SESSION_CREATE_INFO;
    sci.next     = &binding;
    sci.systemId = a.systemId;
    XrResult r = xrCreateSession(a.instance, &sci, &a.session);
    if (XR_FAILED(r))
    {
        LOGE("xrCreateSession failed (%d)", (int)r);
        return false;
    }
    return true;
}

bool createReferenceSpace(App& a)
{
    // Prefer STAGE (floor-aligned, matching the player's real floor); fall back
    // to LOCAL if the runtime/guardian has no stage set up.
    uint32_t spaceCount = 0;
    XR_CHECK(xrEnumerateReferenceSpaces(a.session, 0, &spaceCount, nullptr));
    std::vector<XrReferenceSpaceType> spaces(spaceCount);
    XR_CHECK(xrEnumerateReferenceSpaces(a.session, spaceCount, &spaceCount, spaces.data()));

    XrReferenceSpaceType chosen = XR_REFERENCE_SPACE_TYPE_LOCAL;
    for (XrReferenceSpaceType s : spaces)
    {
        if (s == XR_REFERENCE_SPACE_TYPE_STAGE)
        {
            chosen = XR_REFERENCE_SPACE_TYPE_STAGE;
            break;
        }
    }

    XrReferenceSpaceCreateInfo rsci{};
    rsci.type                 = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType   = chosen;
    rsci.poseInReferenceSpace.orientation.w = 1.0f; // identity pose

    XrResult r = xrCreateReferenceSpace(a.session, &rsci, &a.appSpace);
    if (XR_FAILED(r))
    {
        LOGE("xrCreateReferenceSpace failed (%d)", (int)r);
        return false;
    }
    LOGI("Using %s reference space", chosen == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL");
    return true;
}

bool createSwapchains(App& a)
{
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.systemId, a.viewConfigType, 0,
                                               &a.viewCount, nullptr));
    if (a.viewCount == 0)
    {
        LOGE("No view configuration views (stereo unsupported?)");
        return false;
    }
    a.configViews.resize(a.viewCount);
    for (auto& v : a.configViews)
    {
        v = {};
        v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    }
    XR_CHECK(xrEnumerateViewConfigurationViews(a.instance, a.systemId, a.viewConfigType, a.viewCount,
                                               &a.viewCount, a.configViews.data()));

    a.views.resize(a.viewCount);
    for (auto& v : a.views)
    {
        v = {};
        v.type = XR_TYPE_VIEW;
    }

    // Choose a color format: prefer sRGB8_alpha8, else the runtime's first.
    uint32_t formatCount = 0;
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XR_CHECK(xrEnumerateSwapchainFormats(a.session, formatCount, &formatCount, formats.data()));
    int64_t chosenFormat = formats.empty() ? GL_RGBA8 : formats[0];
    for (int64_t f : formats)
    {
        if (f == GL_SRGB8_ALPHA8)
        {
            chosenFormat = f;
            break;
        }
    }

    a.swapchains.resize(a.viewCount);
    for (uint32_t i = 0; i < a.viewCount; ++i)
    {
        Swapchain& sc = a.swapchains[i];
        sc.width  = static_cast<int32_t>(a.configViews[i].recommendedImageRectWidth);
        sc.height = static_cast<int32_t>(a.configViews[i].recommendedImageRectHeight);

        XrSwapchainCreateInfo ci{};
        ci.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
        ci.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        ci.format      = chosenFormat;
        ci.sampleCount = 1;
        ci.width       = a.configViews[i].recommendedImageRectWidth;
        ci.height      = a.configViews[i].recommendedImageRectHeight;
        ci.faceCount   = 1;
        ci.arraySize   = 1;
        ci.mipCount    = 1;
        XR_CHECK(xrCreateSwapchain(a.session, &ci, &sc.handle));

        uint32_t imageCount = 0;
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, 0, &imageCount, nullptr));
        sc.images.resize(imageCount);
        for (auto& img : sc.images)
        {
            img = {};
            img.type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        }
        XR_CHECK(xrEnumerateSwapchainImages(sc.handle, imageCount, &imageCount,
                                            reinterpret_cast<XrSwapchainImageBaseHeader*>(sc.images.data())));

        glGenRenderbuffers(1, &sc.depth);
        glBindRenderbuffer(GL_RENDERBUFFER, sc.depth);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, sc.width, sc.height);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);
    }
    LOGI("Created %u swapchains (%dx%d)", a.viewCount, a.swapchains[0].width, a.swapchains[0].height);
    return true;
}

void initGL(App& a)
{
    a.program   = glhCreateProgram(kVertexShader, kFragmentShader);
    a.uViewProj = glGetUniformLocation(a.program, "uViewProj");
    a.cube      = glhCreateCube(kMaxBodies);
    glGenFramebuffers(1, &a.fbo);
}

// ---------------------------------------------------------------------------
// Input actions
// ---------------------------------------------------------------------------

void suggestBindings(App& a, const char* profile, XrPath aim[2], XrPath trig[2])
{
    XrPath profilePath;
    XR_CHECK(xrStringToPath(a.instance, profile, &profilePath));

    XrActionSuggestedBinding bindings[] = {
        {a.poseAction, aim[0]},  {a.poseAction, aim[1]},
        {a.triggerAction, trig[0]}, {a.triggerAction, trig[1]},
    };
    XrInteractionProfileSuggestedBinding sb{};
    sb.type                  = XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING;
    sb.interactionProfile    = profilePath;
    sb.countSuggestedBindings = 4;
    sb.suggestedBindings     = bindings;
    XR_CHECK(xrSuggestInteractionProfileBindings(a.instance, &sb));
}

void initActions(App& a)
{
    XrActionSetCreateInfo asci{};
    asci.type     = XR_TYPE_ACTION_SET_CREATE_INFO;
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    asci.priority = 0;
    XR_CHECK(xrCreateActionSet(a.instance, &asci, &a.actionSet));

    XR_CHECK(xrStringToPath(a.instance, "/user/hand/left", &a.handPaths[0]));
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/right", &a.handPaths[1]));

    XrActionCreateInfo pose{};
    pose.type = XR_TYPE_ACTION_CREATE_INFO;
    std::strcpy(pose.actionName, "hand_pose");
    pose.actionType          = XR_ACTION_TYPE_POSE_INPUT;
    pose.countSubactionPaths = 2;
    pose.subactionPaths      = a.handPaths;
    std::strcpy(pose.localizedActionName, "Hand Pose");
    XR_CHECK(xrCreateAction(a.actionSet, &pose, &a.poseAction));

    XrActionCreateInfo trig{};
    trig.type = XR_TYPE_ACTION_CREATE_INFO;
    std::strcpy(trig.actionName, "spawn_box");
    trig.actionType          = XR_ACTION_TYPE_FLOAT_INPUT;
    trig.countSubactionPaths = 2;
    trig.subactionPaths      = a.handPaths;
    std::strcpy(trig.localizedActionName, "Throw Box");
    XR_CHECK(xrCreateAction(a.actionSet, &trig, &a.triggerAction));

    // Aim pose + trigger source paths, per hand.
    XrPath aim[2];
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/left/input/aim/pose", &aim[0]));
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/right/input/aim/pose", &aim[1]));

    // Simple controller: boolean select (runtime converts bool -> float).
    XrPath sel[2];
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/left/input/select/click", &sel[0]));
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/right/input/select/click", &sel[1]));
    suggestBindings(a, "/interaction_profiles/khr/simple_controller", aim, sel);

    // Oculus Touch: analog trigger value.
    XrPath trigVal[2];
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/left/input/trigger/value", &trigVal[0]));
    XR_CHECK(xrStringToPath(a.instance, "/user/hand/right/input/trigger/value", &trigVal[1]));
    suggestBindings(a, "/interaction_profiles/oculus/touch_controller", aim, trigVal);

    // Pose action spaces, one per hand.
    for (int i = 0; i < 2; ++i)
    {
        XrActionSpaceCreateInfo asc{};
        asc.type            = XR_TYPE_ACTION_SPACE_CREATE_INFO;
        asc.action          = a.poseAction;
        asc.subactionPath   = a.handPaths[i];
        asc.poseInActionSpace.orientation.w = 1.0f;
        XR_CHECK(xrCreateActionSpace(a.session, &asc, &a.handSpace[i]));
    }

    XrSessionActionSetsAttachInfo attach{};
    attach.type            = XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO;
    attach.countActionSets = 1;
    attach.actionSets      = &a.actionSet;
    XR_CHECK(xrAttachSessionActionSets(a.session, &attach));
}

void handleInput(App& a, XrTime predictedTime)
{
    XrActiveActionSet active{};
    active.actionSet     = a.actionSet;
    active.subactionPath = XR_NULL_PATH;

    XrActionsSyncInfo sync{};
    sync.type                  = XR_TYPE_ACTIONS_SYNC_INFO;
    sync.countActiveActionSets = 1;
    sync.activeActionSets      = &active;
    if (XR_FAILED(xrSyncActions(a.session, &sync)))
    {
        return;
    }

    static const float kForward[3] = {0.0f, 0.0f, -1.0f};
    for (int i = 0; i < 2; ++i)
    {
        XrActionStateGetInfo gi{};
        gi.type          = XR_TYPE_ACTION_STATE_GET_INFO;
        gi.action        = a.triggerAction;
        gi.subactionPath = a.handPaths[i];

        XrActionStateFloat state{};
        state.type = XR_TYPE_ACTION_STATE_FLOAT;
        XR_CHECK(xrGetActionStateFloat(a.session, &gi, &state));

        const bool pressed = state.isActive == XR_TRUE && state.currentState > 0.6f;
        if (pressed && !a.triggerWasPressed[i])
        {
            XrSpaceLocation loc{};
            loc.type = XR_TYPE_SPACE_LOCATION;
            XR_CHECK(xrLocateSpace(a.handSpace[i], a.appSpace, predictedTime, &loc));

            const XrSpaceLocationFlags need =
                XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            if ((loc.locationFlags & need) == need)
            {
                const float pos[3] = {loc.pose.position.x, loc.pose.position.y, loc.pose.position.z};
                float fwd[3];
                quatRotate(loc.pose.orientation, kForward, fwd);
                const float speed  = 6.0f;
                const float vel[3] = {fwd[0] * speed, fwd[1] * speed, fwd[2] * speed};
                const float color[3] = {0.92f, 0.92f, 0.96f};
                Physics_SpawnBox(pos, vel, 0.06f, color);
            }
        }
        a.triggerWasPressed[i] = pressed;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

// Build the debug readout and append it to this frame's instances.
//
// Placed in world space just in front of the head rather than in view space, so
// that a single instance buffer serves both eyes (see hud.h). The panel is
// rebuilt every frame from the current head pose, which makes it head-locked
// without any per-eye special casing.
//
// Returns the new instance count. Failure here must never cost a measurement,
// so nothing in this path can abort the frame — worst case the text is absent.
int appendHud(App& a, RenderItem* items, int count, int maxItems, uint32_t viewCount,
              double frameMs, double stepMs)
{
    if (viewCount == 0)
    {
        return count;
    }

    // Head position: midpoint of the eyes. Orientation is taken from the left
    // eye — the two differ only by IPD convergence, far below what a debug panel
    // cares about.
    float headPos[3] = {a.views[0].pose.position.x, a.views[0].pose.position.y,
                        a.views[0].pose.position.z};
    if (viewCount > 1)
    {
        headPos[0] = 0.5f * (headPos[0] + a.views[1].pose.position.x);
        headPos[1] = 0.5f * (headPos[1] + a.views[1].pose.position.y);
        headPos[2] = 0.5f * (headPos[2] + a.views[1].pose.position.z);
    }

    static const float kRightLocal[3] = {1.0f, 0.0f, 0.0f};
    static const float kUpLocal[3]    = {0.0f, 1.0f, 0.0f};
    // View space looks down -Z, so +Z points back toward the viewer.
    static const float kBackLocal[3]  = {0.0f, 0.0f, 1.0f};

    float right[3], up[3], back[3];
    quatRotate(a.views[0].pose.orientation, kRightLocal, right);
    quatRotate(a.views[0].pose.orientation, kUpLocal, up);
    quatRotate(a.views[0].pose.orientation, kBackLocal, back);

    // Forward for placement is simply the other way.
    const float forward[3] = {-back[0], -back[1], -back[2]};

    // A metre ahead, offset down and left so it sits out of the way rather than
    // over the middle of the scene.
    constexpr float kDistance  = 1.0f;
    constexpr float kPixel     = 0.006f;
    const float     originX    = -0.28f;
    const float     originY    = -0.14f;

    float origin[3];
    for (int i = 0; i < 3; ++i)
    {
        origin[i] = headPos[i] + forward[i] * kDistance + right[i] * originX + up[i] * originY;
    }

    const float fps = (frameMs > 0.0) ? static_cast<float>(1000.0 / frameMs) : 0.0f;

    char line1[64];
    char line2[64];
    snprintf(line1, sizeof(line1), "FPS %d.%d  FRAME %d.%02d MS", static_cast<int>(fps),
             static_cast<int>(fps * 10) % 10, static_cast<int>(frameMs),
             static_cast<int>(frameMs * 100) % 100);
    snprintf(line2, sizeof(line2), "STEP %d.%02d MS  BODIES %d", static_cast<int>(stepMs),
             static_cast<int>(stepMs * 100) % 100, Physics_BodyCount());

    // Amber when the frame missed 72 Hz, green when it did not — readable at a
    // glance without parsing the digits.
    const bool  missed    = frameMs > (1000.0 / 72.0) * 1.05;
    const float okCol[3]  = {0.35f, 0.90f, 0.45f};
    const float badCol[3] = {0.98f, 0.68f, 0.20f};
    const float* col      = missed ? badCol : okCol;

    const float lineDrop = 9.0f * kPixel;
    float       cursor[3];

    for (int i = 0; i < 3; ++i) cursor[i] = origin[i];
    count = Hud_AppendText(items, count, maxItems, cursor, right, up, back, kPixel, col, line1);

    for (int i = 0; i < 3; ++i) cursor[i] = origin[i] - up[i] * lineDrop;
    count = Hud_AppendText(items, count, maxItems, cursor, right, up, back, kPixel, col, line2);

    if (const char* status = Benchmark_StatusLine())
    {
        const float benchCol[3] = {0.55f, 0.75f, 0.98f};
        for (int i = 0; i < 3; ++i) cursor[i] = origin[i] - up[i] * lineDrop * 2.0f;
        count = Hud_AppendText(items, count, maxItems, cursor, right, up, back, kPixel,
                               benchCol, status);
    }

    return count;
}

// Push this frame's instance data to the GPU. Called once per frame, not once
// per eye — the transforms are identical for both views, only the camera differs.
void uploadInstances(App& a, const RenderItem* items, int count)
{
    if (count > a.cube.maxInstances)
    {
        count = a.cube.maxInstances;
    }
    a.instanceCount = count;
    if (count <= 0)
    {
        return;
    }

    const GLsizeiptr bytes = (GLsizeiptr)count * kFloatsPerInstance * sizeof(float);
    glBindBuffer(GL_ARRAY_BUFFER, a.cube.instanceVbo);
    // Orphan the previous contents so the driver need not stall waiting for the
    // last frame's draw to finish reading them.
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)a.cube.maxInstances * kFloatsPerInstance * sizeof(float), nullptr,
                 GL_DYNAMIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, bytes, items);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void renderView(App& a, const Swapchain& sc, const XrView& view)
{
    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acq{};
    acq.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
    XR_CHECK(xrAcquireSwapchainImage(sc.handle, &acq, &imageIndex));

    XrSwapchainImageWaitInfo wait{};
    wait.type    = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
    wait.timeout = XR_INFINITE_DURATION;
    XR_CHECK(xrWaitSwapchainImage(sc.handle, &wait));

    // Projection * view for this eye.
    float proj[16];
    m4_projection_fov(proj, view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp,
                      view.fov.angleDown, 0.05f, 100.0f);
    const float q[4] = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                        view.pose.orientation.w};
    const float p[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    float viewMat[16];
    m4_view_from_pose(viewMat, q, p);
    float viewProj[16];
    m4_mul(viewProj, proj, viewMat);

    const GLuint colorTex = sc.images[imageIndex].image;
    glBindFramebuffer(GL_FRAMEBUFFER, a.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, sc.depth);

    glViewport(0, 0, sc.width, sc.height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // Back faces are never visible on a closed box, so culling them is free fill
    // rate. All six faces of the cube mesh in gl_helpers.h are wound *clockwise*
    // when viewed from outside — verified per face against the outward normals,
    // not assumed. Hence GL_CW; if the scene ever renders inside-out, this line
    // is the first suspect.
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    glFrontFace(GL_CW);
    glClearColor(0.05f, 0.06f, 0.09f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glUseProgram(a.program);
    glUniformMatrix4fv(a.uViewProj, 1, GL_FALSE, viewProj);
    glBindVertexArray(a.cube.vao);
    // The entire scene, whatever its size, in one call.
    glDrawElementsInstanced(GL_TRIANGLES, a.cube.indexCount, GL_UNSIGNED_SHORT, nullptr,
                            a.instanceCount);
    glBindVertexArray(0);

    XrSwapchainImageReleaseInfo rel{};
    rel.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
    XR_CHECK(xrReleaseSwapchainImage(sc.handle, &rel));
}

void renderFrame(App& a)
{
    XrFrameWaitInfo waitInfo{};
    waitInfo.type = XR_TYPE_FRAME_WAIT_INFO;
    XrFrameState frameState{};
    frameState.type = XR_TYPE_FRAME_STATE;
    XR_CHECK(xrWaitFrame(a.session, &waitInfo, &frameState));

    XrFrameBeginInfo beginInfo{};
    beginInfo.type = XR_TYPE_FRAME_BEGIN_INFO;
    XR_CHECK(xrBeginFrame(a.session, &beginInfo));

    // Fixed timestep with an accumulator.
    //
    // Stepping the solver with whatever dt the frame loop happens to produce
    // makes it behave differently under load — which is exactly the condition
    // being measured — and makes results irreproducible between runs. A fixed
    // step keeps behaviour identical whether the frame rate is steady or not.
    if (a.lastDisplayTime != 0)
    {
        float elapsed = static_cast<float>(frameState.predictedDisplayTime - a.lastDisplayTime) * 1e-9f;
        if (elapsed < 0.0f) elapsed = 0.0f;
        // A long hitch (or a resumed session) must not be paid back all at once.
        if (elapsed > kMaxAccumulated) elapsed = kMaxAccumulated;
        a.physicsAccumulator += elapsed;
    }
    a.lastDisplayTime = frameState.predictedDisplayTime;

    // Capped so a slow frame cannot spiral: falling behind must never cause more
    // physics work, which would make the next frame slower still.
    const double stepStartMs = nowMs();
    int steps = 0;
    while (a.physicsAccumulator >= kPhysicsDt && steps < kMaxStepsPerFrame)
    {
        Physics_Step(kPhysicsDt);
        a.physicsAccumulator -= kPhysicsDt;
        ++steps;
    }
    if (steps == kMaxStepsPerFrame)
    {
        // Drop the backlog rather than carry a debt that can never be repaid.
        a.physicsAccumulator = 0.0f;
    }
    const double stepMs = nowMs() - stepStartMs;

    // Wall-clock between frame starts, which is what the player actually feels —
    // it includes compositor wait, render and everything else, not just our work.
    const double frameStartMs = nowMs();
    const double frameMs =
        (a.lastFrameStartMs > 0.0) ? (frameStartMs - a.lastFrameStartMs) : 0.0;
    a.lastFrameStartMs = frameStartMs;
    ++a.frameIndex;

    // Kick off the automated run once the session has settled. Deliberately
    // unconditional: no menu to navigate and no button to find, so a broken HUD
    // or an unmapped controller cannot prevent the measurement from happening.
    if (a.frameIndex == kFramesBeforeBenchmark)
    {
        Benchmark_Start();
    }
    if (frameMs > 0.0)
    {
        Benchmark_Update(frameMs, stepMs);
    }

    // Throwing cubes during a run would corrupt the body count being measured.
    if (a.sessionState == XR_SESSION_STATE_FOCUSED && !Benchmark_Active())
    {
        handleInput(a, frameState.predictedDisplayTime);
    }

    std::vector<XrCompositionLayerProjectionView> projViews;
    XrCompositionLayerProjection layer{};
    layer.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
    bool rendered = false;

    if (frameState.shouldRender == XR_TRUE)
    {
        XrViewLocateInfo locate{};
        locate.type                  = XR_TYPE_VIEW_LOCATE_INFO;
        locate.viewConfigurationType = a.viewConfigType;
        locate.displayTime           = frameState.predictedDisplayTime;
        locate.space                 = a.appSpace;

        XrViewState viewState{};
        viewState.type = XR_TYPE_VIEW_STATE;
        uint32_t viewCountOut = 0;
        XR_CHECK(xrLocateViews(a.session, &locate, &viewState, a.viewCount, &viewCountOut,
                               a.views.data()));

        const bool posesValid = (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
                                (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
        if (posesValid)
        {
            static RenderItem items[kMaxBodies];
            int count = Physics_BuildRenderItems(items, kMaxBodies);
            count = appendHud(a, items, count, kMaxBodies, viewCountOut, frameMs, stepMs);
            // Uploaded once, drawn by both eyes.
            uploadInstances(a, items, count);

            projViews.resize(viewCountOut);
            for (uint32_t i = 0; i < viewCountOut; ++i)
            {
                renderView(a, a.swapchains[i], a.views[i]);

                projViews[i] = {};
                projViews[i].type                    = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projViews[i].pose                    = a.views[i].pose;
                projViews[i].fov                     = a.views[i].fov;
                projViews[i].subImage.swapchain      = a.swapchains[i].handle;
                projViews[i].subImage.imageRect.offset = {0, 0};
                projViews[i].subImage.imageRect.extent = {a.swapchains[i].width, a.swapchains[i].height};
                projViews[i].subImage.imageArrayIndex  = 0;
            }

            layer.space     = a.appSpace;
            layer.viewCount = static_cast<uint32_t>(projViews.size());
            layer.views     = projViews.data();
            rendered        = true;
        }
    }

    const XrCompositionLayerBaseHeader* layers[1] = {
        reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer)};
    XrFrameEndInfo endInfo{};
    endInfo.type                 = XR_TYPE_FRAME_END_INFO;
    endInfo.displayTime          = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount           = rendered ? 1 : 0;
    endInfo.layers               = rendered ? layers : nullptr;
    XR_CHECK(xrEndFrame(a.session, &endInfo));
}

// ---------------------------------------------------------------------------
// Session lifecycle
// ---------------------------------------------------------------------------

void pollXrEvents(App& a)
{
    XrEventDataBuffer event{};
    event.type = XR_TYPE_EVENT_DATA_BUFFER;
    while (xrPollEvent(a.instance, &event) == XR_SUCCESS)
    {
        switch (event.type)
        {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            const auto* changed = reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            a.sessionState = changed->state;
            if (changed->state == XR_SESSION_STATE_READY)
            {
                XrSessionBeginInfo begin{};
                begin.type = XR_TYPE_SESSION_BEGIN_INFO;
                begin.primaryViewConfigurationType = a.viewConfigType;
                XR_CHECK(xrBeginSession(a.session, &begin));
                a.sessionRunning = true;
                LOGI("Session running");
            }
            else if (changed->state == XR_SESSION_STATE_STOPPING)
            {
                a.sessionRunning = false;
                XR_CHECK(xrEndSession(a.session));
                LOGI("Session stopped");
            }
            else if (changed->state == XR_SESSION_STATE_EXITING ||
                     changed->state == XR_SESSION_STATE_LOSS_PENDING)
            {
                a.exitRequested = true;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
            a.exitRequested = true;
            break;
        default:
            break;
        }
        event = {};
        event.type = XR_TYPE_EVENT_DATA_BUFFER;
    }
}

void onAppCmd(android_app* app, int32_t cmd)
{
    auto* a = static_cast<App*>(app->userData);
    if (a == nullptr)
    {
        return;
    }
    // Track foreground state so the main loop knows when to poll OpenXR events
    // (which arrive on the OpenXR queue, not the Android ALooper).
    switch (cmd)
    {
    case APP_CMD_RESUME:
        a->resumed = true;
        break;
    case APP_CMD_PAUSE:
        a->resumed = false;
        break;
    case APP_CMD_DESTROY:
        a->exitRequested = true;
        break;
    default:
        break;
    }
}

} // namespace

void android_main(android_app* app)
{
    JNIEnv* env = nullptr;
    app->activity->vm->AttachCurrentThread(&env, nullptr);

    App a{};
    a.app          = app;
    app->userData  = &a;
    app->onAppCmd  = onAppCmd;

    if (!initLoader(app) || !createInstance(a) || !getSystem(a) || !initEgl(a) ||
        !createSession(a) || !createReferenceSpace(a) || !createSwapchains(a))
    {
        LOGE("Initialization failed; exiting");
        app->activity->vm->DetachCurrentThread();
        return;
    }
    initGL(a);
    initActions(a);
    Physics_Init();
    LOGI("Initialization complete");

    while (!app->destroyRequested && !a.exitRequested)
    {
        // Pump Android events. Block indefinitely only while the app is both
        // paused and not running — otherwise use a 0 timeout so xrPollEvent runs
        // every iteration and can catch the IDLE->READY (and later STOPPING/
        // EXITING) transitions, which are delivered on the OpenXR event queue.
        int timeout = (!a.resumed && !a.sessionRunning && !app->destroyRequested) ? -1 : 0;
        int events;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(timeout, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0)
        {
            if (source != nullptr)
            {
                source->process(app, source);
            }
            timeout = 0;
            if (app->destroyRequested)
            {
                break;
            }
        }

        pollXrEvents(a);

        if (a.sessionRunning)
        {
            renderFrame(a);
        }
    }

    Physics_Shutdown();
    app->activity->vm->DetachCurrentThread();
}
