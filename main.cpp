////////////////////////////////////////////////////////////////////////////////
// main.cpp — Varjo XR-4 Mixed Reality: camera pass-through + gaze dot overlay
// + left camera capture in parallel using Varjo DataStream API.
//
// Runtime architecture:
//   1. Create a tiny Win32/WGL OpenGL context so Varjo GL swapchains can be used.
//   2. Start a Varjo session, enable MR video pass-through, and initialize gaze.
//   3. Subscribe to Varjo's distorted-color camera DataStream for both eyes.
//   4. Send captured NV12 camera frames and current gaze coordinates to Python.
//   5. Receive Python-generated grayscale phosphene masks back over UDP.
//   6. Render a transparent overlay layer containing:
//        - a gaze-centered black scotoma mask, and
//        - a gaze-centered phosphene texture from Python.
//
// Coordinate convention used by the overlay:
//   The shaders work in "tangent space" rather than pixels. A point on the view
//   is represented by tan(x angle) and tan(y angle). This makes the overlay line
//   up with Varjo's per-view FOV tangents even when the headset has multiple
//   focus/context views.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>   // shared-memory transport (CreateFileMapping/MapViewOfFile)

#include <GL/gl.h>
#include "glext.h"
#include "wglext.h"

#include <Varjo.h>
#include <Varjo_gl.h>
#include <Varjo_mr.h>
#include <Varjo_datastream.h>
#include <Varjo_types.h>
#include <Varjo_types_layers.h>
#include <Varjo_types_datastream.h>
#include <Varjo_layers.h>
#include <Varjo_math.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>


// ---------------------------------------------------------------------------
// GL function pointers
// ---------------------------------------------------------------------------

// Windows only exposes a very small OpenGL ABI directly from opengl32.dll.
// Everything modern that this file uses (FBOs, shaders, VAOs, etc.) is loaded
// at runtime through WGL extension pointers.
static PFNGLGENFRAMEBUFFERSPROC             glGenFramebuffers = nullptr;
static PFNGLBINDFRAMEBUFFERPROC             glBindFramebuffer = nullptr;
static PFNGLFRAMEBUFFERTEXTURE2DPROC        glFramebufferTexture2D = nullptr;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC      glCheckFramebufferStatus = nullptr;
static PFNGLDELETEFRAMEBUFFERSPROC          glDeleteFramebuffers = nullptr;
static PFNGLCREATESHADERPROC                glCreateShader = nullptr;
static PFNGLSHADERSOURCEPROC                glShaderSource = nullptr;
static PFNGLCOMPILESHADERPROC               glCompileShader = nullptr;
static PFNGLGETSHADERIVPROC                 glGetShaderiv = nullptr;
static PFNGLGETSHADERINFOLOGPROC            glGetShaderInfoLog = nullptr;
static PFNGLCREATEPROGRAMPROC               glCreateProgram = nullptr;
static PFNGLATTACHSHADERPROC                glAttachShader = nullptr;
static PFNGLLINKPROGRAMPROC                 glLinkProgram = nullptr;
static PFNGLGETPROGRAMIVPROC                glGetProgramiv = nullptr;
static PFNGLGETPROGRAMINFOLOGPROC           glGetProgramInfoLog = nullptr;
static PFNGLUSEPROGRAMPROC                  glUseProgram = nullptr;
static PFNGLGETUNIFORMLOCATIONPROC          glGetUniformLocation = nullptr;
static PFNGLUNIFORM1FPROC                   glUniform1f = nullptr;
static PFNGLUNIFORM1IPROC                   glUniform1i = nullptr;
static PFNGLACTIVETEXTUREPROC               glActiveTexture = nullptr;
static PFNGLDELETESHADERPROC                glDeleteShader = nullptr;
static PFNGLDELETEPROGRAMPROC               glDeleteProgram = nullptr;
static PFNGLGENVERTEXARRAYSPROC             glGenVertexArrays = nullptr;
static PFNGLBINDVERTEXARRAYPROC             glBindVertexArray = nullptr;
static PFNGLDELETEVERTEXARRAYSPROC          glDeleteVertexArrays = nullptr;
static PFNGLBLENDFUNCSEPARATEPROC           glBlendFuncSeparate = nullptr;

static void* getGLProcAddressAny(const char* name)
{
    // wglGetProcAddress is the normal path for extension functions. Some core
    // functions may instead be exported by opengl32.dll, so keep a fallback.
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        static HMODULE module = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(module, name);
    }
    return p;
}

static void loadGLFunctions()
{
    // Fail fast here: if any required GL symbol is absent, later rendering
    // errors would be much harder to diagnose.
    #define LOAD(name) name = (decltype(name))getGLProcAddressAny(#name); \
        if (!name) { fprintf(stderr, "Failed to load " #name "\n"); exit(1); }
    LOAD(glGenFramebuffers);
    LOAD(glBindFramebuffer);
    LOAD(glFramebufferTexture2D);
    LOAD(glCheckFramebufferStatus);
    LOAD(glDeleteFramebuffers);
    LOAD(glCreateShader);
    LOAD(glShaderSource);
    LOAD(glCompileShader);
    LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram);
    LOAD(glAttachShader);
    LOAD(glLinkProgram);
    LOAD(glGetProgramiv);
    LOAD(glGetProgramInfoLog);
    LOAD(glUseProgram);
    LOAD(glGetUniformLocation);
    LOAD(glUniform1f);
    LOAD(glUniform1i);
    LOAD(glActiveTexture);
    LOAD(glDeleteShader);
    LOAD(glDeleteProgram);
    LOAD(glGenVertexArrays);
    LOAD(glBindVertexArray);
    LOAD(glDeleteVertexArrays);
    LOAD(glBlendFuncSeparate);
    #undef LOAD
}

// ---------------------------------------------------------------------------
// WGL context
// ---------------------------------------------------------------------------

static HWND g_hwnd = nullptr;
static HDC g_hdc = nullptr;
static HGLRC g_hglrc = nullptr;

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{ return DefWindowProc(h, m, w, l); }

static bool createGLContext()
{
    // Varjo's OpenGL API needs a current GL context before swapchains and GL
    // textures can be created. The program renders to headset swapchain images,
    // not to this 1x1 helper window.
    WNDCLASSA wc{};
    wc.lpfnWndProc  = wndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = "VarjoGL";
    RegisterClassA(&wc);
    g_hwnd = CreateWindowExA(0, "VarjoGL", "VarjoGL", 0, 0, 0, 1, 1,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) return false;
    g_hdc = GetDC(g_hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd); pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA; pfd.cColorBits = 32; pfd.cDepthBits = 24;
    int fmt = ChoosePixelFormat(g_hdc, &pfd);
    SetPixelFormat(g_hdc, fmt, &pfd);
    g_hglrc = wglCreateContext(g_hdc);
    wglMakeCurrent(g_hdc, g_hglrc);
    return true;
}

static void destroyGLContext()
{
    // Release WGL resources in reverse order from creation.
    wglMakeCurrent(nullptr, nullptr);
    if (g_hglrc) wglDeleteContext(g_hglrc);
    if (g_hwnd) DestroyWindow(g_hwnd);
}

// ---------------------------------------------------------------------------
// Shader: transparent background + black spot
// ---------------------------------------------------------------------------

enum BlindnessMode {
    BLINDNESS_MACULAR  = 0,
    BLINDNESS_GLAUCOMA = 1,
    BLINDNESS_FULL     = 2
};

static BlindnessMode gBlindnessMode = BLINDNESS_FULL;

// Overlay geometry for one blindness type, all in tangent space (tan of the
// angle away from gaze). One place so the shader uniforms, the Python crop size
// and the startup texture allocation can never drift apart.
//   spotRadiusTan      : boundary of the scotoma (macular) or clear tunnel
//                        (glaucoma), measured from the gaze point -- this is the
//                        DISEASE parameter and is what differs between modes
//   softEdgeTan        : feather width of that boundary
//   phospheneRadiusTan : half-size of the gaze-centred phosphene disc the fixed
//                        implant produces. This is the DEVICE field and is the
//                        SAME for every mode -- the electrodes do not move with
//                        the diagnosis. It must match dynaphos' field, i.e.
//                        phospheneRadiusTan == tan(view_angle/2) from params.yaml
//                        (view_angle=48 deg -> tan(24) ~= 0.45), and the device
//                        grid generate_device_coords.py reaches the same 24 deg.
// The per-disease scotoma mask then decides which of these fixed phosphenes are
// actually shown (the ones landing in the blind region).
constexpr float kDeviceFieldTan = 0.45f;   // fixed implant's phosphene half-field (~24 deg)

struct OverlayGeometry {
    float spotRadiusTan;
    float softEdgeTan;
    float phospheneRadiusTan;
};

static OverlayGeometry overlayGeometryFor(BlindnessMode mode)
{
    switch (mode) {
    case BLINDNESS_GLAUCOMA:
        // Clear central tunnel (residual vision); the device's phosphenes show in
        // the blind ring between the tunnel edge and the device field
        // (~8.5..24 deg). Widen the tunnel (spotRadiusTan) for milder glaucoma.
        return OverlayGeometry{ 0.15f, 0.04f, kDeviceFieldTan };
    case BLINDNESS_FULL:
        // Full blindness samples the whole frame in screen space, so spot radius
        // is unused; the device field is the same fixed disc.
        return OverlayGeometry{ 0.0f, 0.0f, kDeviceFieldTan };
    case BLINDNESS_MACULAR:
    default:
        // Black central scotoma; the fixed central device fills it.
        return OverlayGeometry{ 0.24f, 0.04f, kDeviceFieldTan };
    }
}


static const char* g_vertSrc = R"(
#version 330 core
out vec2 vUV;
void main() {
    // Fullscreen triangle without a vertex buffer. gl_VertexID produces the
    // three clip-space corners needed to cover the viewport.
    vUV = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(vUV * 2.0 - 1.0, 0.0, 1.0);
    vUV.y = 1.0 - vUV.y;
}
)";

static const char* g_fragSrc = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;

uniform float gazeTanX;
uniform float gazeTanY;

uniform float viewLeft;
uniform float viewRight;
uniform float viewTop;
uniform float viewBottom;

uniform float spotRadiusTan;
uniform float softEdgeTan;

uniform int blindnessMode; // 0 = macular, 1 = glaucoma, 2 = full

float blindnessMask(vec2 tanPos, vec2 gazeTan)
{
    float d = distance(tanPos, gazeTan);

    // Macular degeneration:
    // black in the center, transparent outside
    if (blindnessMode == 0) {
        return 1.0 - smoothstep(
            spotRadiusTan - softEdgeTan,
            spotRadiusTan,
            d
        );
    }

    // Glaucoma:
    // transparent in central island, black in periphery
    if (blindnessMode == 1) {
        return smoothstep(
            spotRadiusTan,
            spotRadiusTan + softEdgeTan,
            d
        );
    }

    // Full blindness:
    // black everywhere
    if (blindnessMode == 2) {
        return 1.0;
    }

    return 0.0;
}

void main() {
    float pxTanX = mix(viewLeft,  viewRight,  vUV.x);
    float pxTanY = mix(viewTop,   viewBottom, vUV.y);

    vec2 tanPos = vec2(pxTanX, pxTanY);
    vec2 gazeTan = vec2(gazeTanX, gazeTanY);

    float alpha = blindnessMask(tanPos, gazeTan);

    fragColor = vec4(0.0, 0.0, 0.0, alpha);
}
)";

static const char* g_phospheneFragSrc = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;

uniform float gazeTanX;
uniform float gazeTanY;

uniform float viewLeft;
uniform float viewRight;
uniform float viewTop;
uniform float viewBottom;

uniform float phospheneRadiusTan;
uniform float phospheneOpacity;
uniform sampler2D phospheneTex;

uniform float spotRadiusTan;
uniform float softEdgeTan;
uniform int blindnessMode; // 0 = macular, 1 = glaucoma, 2 = full

float blindnessMask(vec2 tanPos, vec2 gazeTan)
{
    float d = distance(tanPos, gazeTan);

    // Macular degeneration:
    // phosphenes only in center blind spot
    if (blindnessMode == 0) {
        return 1.0 - smoothstep(
            spotRadiusTan - softEdgeTan,
            spotRadiusTan,
            d
        );
    }

    // Glaucoma:
    // phosphenes only in peripheral blind region
    if (blindnessMode == 1) {
        return smoothstep(
            spotRadiusTan,
            spotRadiusTan + softEdgeTan,
            d
        );
    }

    // Full blindness:
    // phosphenes everywhere
    if (blindnessMode == 2) {
        return 1.0;
    }

    return 0.0;
}

void main() {
    float pxTanX = mix(viewLeft,  viewRight,  vUV.x);
    float pxTanY = mix(viewTop,   viewBottom, vUV.y);

    vec2 tanPos = vec2(pxTanX, pxTanY);
    vec2 gazeTan = vec2(gazeTanX, gazeTanY);

    float mask = blindnessMask(tanPos, gazeTan);

    if (mask <= 0.001) {
        fragColor = vec4(0.0);
        return;
    }

    // Every mode feeds the fixed implant the same gaze-centred crop, so the
    // phosphenes are always sampled relative to gaze and follow the eye. The
    // blindness mask above decides where they show: the central disc (macular),
    // the peripheral ring (glaucoma), or the whole device field (full blindness).
    vec2 diff = tanPos - gazeTan;
    float radius = phospheneRadiusTan;

    if (abs(diff.x) > radius || abs(diff.y) > radius) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 uv = diff / (2.0 * radius) + vec2(0.5, 0.5);
    float p = texture(phospheneTex, uv).r;

    float a = clamp(p * mask * phospheneOpacity, 0.0, 1.0);

    fragColor = vec4(a, a, a, a);
}
)";

struct CropRect
{
    // Pixel-space rectangle inside a Varjo viewport, plus the unclamped gaze
    // center. This is useful when extracting only the scotoma/phosphene region.
    int x0, y0, x1, y1;
    int width, height;
    float cx, cy;
};

static CropRect computeScotomaCropUV(
    const varjo_Viewport& vp,
    float gazeU,
    float gazeV,
    float spotRadius,
    float softEdge,
    bool solidCoreOnly = false)
{
    // The solid core is the area inside the soft fade. Python uses the core
    // size for phosphene generation, while the renderer can still draw the
    // larger black spot with a feathered edge.
    const float r = solidCoreOnly
        ? (std::max)(0.0f, spotRadius - softEdge)
        : spotRadius;

    const float viewAspect = (float)vp.width / (float)vp.height;

    const float cx = (float)vp.x + gazeU * (float)vp.width;
    const float cy = (float)vp.y + gazeV * (float)vp.height;

    const float halfW = (r / viewAspect) * (float)vp.width;
    const float halfH = r * (float)vp.height;

    int x0 = (int)std::floor(cx - halfW);
    int y0 = (int)std::floor(cy - halfH);
    int x1 = (int)std::ceil (cx + halfW);
    int y1 = (int)std::ceil (cy + halfH);

    // Clamp to this viewport
    x0 = (std::max)(x0, (int)vp.x);
    y0 = (std::max)(y0, (int)vp.y);
    x1 = (std::min)(x1, (int)(vp.x + vp.width));
    y1 = (std::min)(y1, (int)(vp.y + vp.height));

    CropRect out{};
    out.x0 = x0;
    out.y0 = y0;
    out.x1 = x1;
    out.y1 = y1;
    out.width  = x1 - x0;
    out.height = y1 - y0;
    out.cx = cx;
    out.cy = cy;
    return out;
}

// ---------------------------------------------------------------------------
// Shader helpers
// ---------------------------------------------------------------------------

static GLuint compileShader(GLenum type, const char* src)
{
    // Small helper for the two embedded GLSL programs. Any compile error is
    // fatal because rendering cannot continue without these overlay shaders.
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        fprintf(stderr, "Shader error:\n%s\n", log); exit(1);
    }
    return s;
}

static GLuint createProgram(const char* fragSrc)
{
    // Both overlay passes use the same fullscreen-triangle vertex shader and
    // differ only in the fragment shader.
    GLuint vs = compileShader(GL_VERTEX_SHADER, g_vertSrc);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragSrc);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512]; glGetProgramInfoLog(p, sizeof(log), nullptr, log);
        fprintf(stderr, "Link error:\n%s\n", log); exit(1);
    }
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ---------------------------------------------------------------------------
// Atlas viewports
// ---------------------------------------------------------------------------

static std::vector<varjo_Viewport> calculateViewports(
    varjo_Session* session, varjo_TextureSize_Type type)
{
    // Varjo swapchains are rendered as an atlas: multiple eye/focus/context
    // views are packed into one large texture. This function asks Varjo for each
    // view's recommended texture size, then lays them out two per row.
    const int32_t viewCount = 4;
    std::vector<varjo_Viewport> viewports;
    viewports.reserve(viewCount);
    int x = 0, y = 0;
    for (int32_t i = 0; i < viewCount; i++) {
        int32_t width = 0, height = 0;
        varjo_GetTextureSize(session, type, i, &width, &height);
        const varjo_Viewport viewport = varjo_Viewport{x, y, width, height};
        viewports.push_back(viewport);
        x += viewport.width;
        if (i > 0 && viewports.size() % 2 == 0) {
            x = 0;
            y += viewport.height;
        }
    }
    return viewports;
}

static int32_t getTotalWidth(const std::vector<varjo_Viewport>& vp) {
    // Atlas width is the largest right edge among all packed viewports.
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.x + v.width));
    return m;
}
static int32_t getTotalHeight(const std::vector<varjo_Viewport>& vp) {
    // Atlas height is the largest bottom edge among all packed viewports.
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.y + v.height));
    return m;
}


// ---------------------------------------------------------------------------
// Shared-memory bridge to Python (replaces the old UDP transport)
// ---------------------------------------------------------------------------
//
// Two named, pagefile-backed sections, one per direction:
//   cam  (C++ -> Python) : raw NV12 camera frames + per-frame gaze/metadata
//   phos (Python -> C++) : 8-bit grayscale phosphene images
//
// Each section is [ Header | eye0 channel | eye1 channel ]; a channel is a
// lock-free double buffer [ Ctrl | slot0 | slot1 ], slot = [ SlotMeta | bytes ].
// Publish (single-producer/single-consumer, latest-wins): the producer writes
// the free slot, stores latestIndex, then bumps publishSeq (release). The
// consumer reads publishSeq, copies slot[latestIndex], re-reads publishSeq; if
// it moved mid-copy it retries (seqlock). Double buffering means the producer
// never overwrites the slot being read unless it laps the consumer, which the
// re-check catches. No locks, no kernel events -- consumers poll publishSeq.
//
// The layout is fixed at compile time so both sides agree without negotiating;
// mappings are sized to generous maxima and only touched pages use RAM.

static const char* kCamMapName  = "Local\\VarjoPhospheneCam";   // C++  -> Python
static const char* kPhosMapName = "Local\\VarjoPhospheneOut";   // Python -> C++

constexpr uint32_t SHM_MAGIC   = 0x50484D31;   // 'PHM1'
constexpr uint32_t SHM_VERSION = 1;

constexpr uint32_t SHM_FLAG_READY    = 1u << 0;
constexpr uint32_t SHM_FLAG_SHUTDOWN = 1u << 1;

constexpr uint32_t SHM_NUM_EYES  = 2;
constexpr uint32_t SHM_NUM_SLOTS = 2;          // double buffer

constexpr uint32_t SHM_HEADER_SIZE   = 512;
constexpr uint32_t SHM_CTRL_SIZE     = 64;
constexpr uint32_t SHM_SLOTMETA_SIZE = 64;

// Per-slot payload capacities (generous upper bounds; the actual bytes per frame
// are carried in SlotMeta.byteSize). NV12 ~ w*h*3/2; grayscale ~ w*h.
constexpr uint32_t CAM_SLOT_CAP  = 8u * 1024 * 1024;
constexpr uint32_t PHOS_SLOT_CAP = 4u * 1024 * 1024;

constexpr uint32_t CAM_SLOT_STRIDE   = SHM_SLOTMETA_SIZE + CAM_SLOT_CAP;
constexpr uint32_t CAM_CHANNEL_SIZE  = SHM_CTRL_SIZE + SHM_NUM_SLOTS * CAM_SLOT_STRIDE;
constexpr uint32_t CAM_MAP_SIZE      = SHM_HEADER_SIZE + SHM_NUM_EYES * CAM_CHANNEL_SIZE;

constexpr uint32_t PHOS_SLOT_STRIDE  = SHM_SLOTMETA_SIZE + PHOS_SLOT_CAP;
constexpr uint32_t PHOS_CHANNEL_SIZE = SHM_CTRL_SIZE + SHM_NUM_SLOTS * PHOS_SLOT_STRIDE;
constexpr uint32_t PHOS_MAP_SIZE     = SHM_HEADER_SIZE + SHM_NUM_EYES * PHOS_CHANNEL_SIZE;

// Fallback camera angular half-extents used only when Varjo intrinsics are not
// available. These approximate how a tangent-space radius maps to camera pixels.
constexpr float kCameraTanHalfX = 0.6f;
constexpr float kCameraTanHalfY = 0.6f;
constexpr double kPythonSendIntervalMs = 0.0; // ~15 FPS --> switch to 16.6 for 60 FPS if needed

#pragma pack(push, 1)
struct ShmEyeConfig {
    // Per-eye config, carried in the cam header (replaces the old config packet).
    int32_t cropWidth;
    int32_t cropHeight;
    int32_t frameWidth;
    int32_t frameHeight;
    int32_t rowStride;
    int32_t intrinsicsModel;
    int32_t intrinsicsValid;
    int32_t reserved;
    double  focalLengthX;
    double  focalLengthY;
    double  principalPointX;
    double  principalPointY;
    double  distortionCoefficients[8];
};

struct ShmHeader {
    // Lives at offset 0 of each mapping. The cam header additionally carries the
    // per-eye config that used to travel in the UDP config packet.
    uint32_t magic;
    uint32_t version;
    uint32_t flags;            // SHM_FLAG_READY | SHM_FLAG_SHUTDOWN
    uint32_t configSeq;        // bumped when any eye config changes
    int32_t  blindnessMode;    // C++ is the single source of truth
    uint32_t reserved0;
    ShmEyeConfig eye[SHM_NUM_EYES];
};

struct ShmCtrl {
    // One per channel, at the channel's base offset.
    uint32_t latestIndex;      // index of the most-recently published slot
    uint32_t publishSeq;       // monotonically increasing; consumer compares
    uint32_t reserved[14];
};

struct ShmSlotMeta {
    // Prefix of each slot; describes the payload bytes that follow it.
    uint32_t frameId;
    uint32_t eye;
    uint32_t width;
    uint32_t height;
    uint32_t rowStride;
    uint32_t byteSize;         // actual payload bytes in this slot
    float    gazeTanX;
    float    gazeTanY;
    uint32_t reserved[8];
};
#pragma pack(pop)

static_assert(sizeof(ShmHeader)   <= SHM_HEADER_SIZE,   "ShmHeader exceeds SHM_HEADER_SIZE");
static_assert(sizeof(ShmCtrl)     == SHM_CTRL_SIZE,     "ShmCtrl must equal SHM_CTRL_SIZE");
static_assert(sizeof(ShmSlotMeta) == SHM_SLOTMETA_SIZE, "ShmSlotMeta must equal SHM_SLOTMETA_SIZE");

// --- lock-free double-buffer helpers (x86 TSO; fences are the compiler barrier) ---
static inline uint8_t* shmChannel(uint8_t* base, int eye, uint32_t channelSize) {
    return base + SHM_HEADER_SIZE + (uint32_t)eye * channelSize;
}
static inline uint8_t* shmSlot(uint8_t* channel, uint32_t slot, uint32_t slotStride) {
    return channel + SHM_CTRL_SIZE + slot * slotStride;
}

// Producer: write meta + payload into the free slot, then publish it.
static void shmPublish(uint8_t* channel, uint32_t slotStride, uint32_t slotCap,
                       uint32_t& writeIndex, const ShmSlotMeta& metaIn,
                       const void* payload, uint32_t byteSize)
{
    ShmCtrl* ctrl = reinterpret_cast<ShmCtrl*>(channel);
    const uint32_t w = writeIndex;
    uint8_t* slot = shmSlot(channel, w, slotStride);
    const uint32_t n = (byteSize <= slotCap) ? byteSize : slotCap;

    ShmSlotMeta* meta = reinterpret_cast<ShmSlotMeta*>(slot);
    *meta = metaIn;
    meta->byteSize = n;
    std::memcpy(slot + SHM_SLOTMETA_SIZE, payload, n);

    // Ensure payload + meta are visible before we advertise the slot.
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->latestIndex = w;
    std::atomic_thread_fence(std::memory_order_release);
    ctrl->publishSeq  = ctrl->publishSeq + 1;   // the publish

    writeIndex = (w + 1) % SHM_NUM_SLOTS;
}

// Consumer: return the latest published frame if it is newer than lastSeq.
static bool shmConsume(uint8_t* channel, uint32_t slotStride,
                       uint32_t& lastSeq, ShmSlotMeta& metaOut,
                       std::vector<uint8_t>& payloadOut)
{
    ShmCtrl* ctrl = reinterpret_cast<ShmCtrl*>(channel);
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint32_t s1 = ctrl->publishSeq;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (s1 == lastSeq) return false;                 // nothing new
        const uint32_t idx = ctrl->latestIndex;
        if (idx >= SHM_NUM_SLOTS) return false;

        uint8_t* slot = shmSlot(channel, idx, slotStride);
        ShmSlotMeta m = *reinterpret_cast<const ShmSlotMeta*>(slot);
        const uint32_t n = (m.byteSize <= slotStride - SHM_SLOTMETA_SIZE)
                               ? m.byteSize : 0;
        payloadOut.resize(n);
        if (n) std::memcpy(payloadOut.data(), slot + SHM_SLOTMETA_SIZE, n);

        std::atomic_thread_fence(std::memory_order_acquire);
        if (ctrl->publishSeq == s1) {                    // no tear during copy
            lastSeq = s1;
            metaOut = m;
            return true;
        }
        // producer published mid-copy; retry for a consistent snapshot
    }
    return false;   // producer racing very fast; pick it up next poll
}

struct CapturedEyeFrame {
    // CPU copy of one Varjo camera frame. The DataStream callback copies into
    // this object because Varjo owns and recycles the original buffer.
    int eye = 0;              // 0 = left, 1 = right
    int width = 0;
    int height = 0;
    int rowStride = 0;
    int64_t frameNumber = 0;
    varjo_Nanoseconds timestamp = 0;
    bool hasIntrinsics = false;
    int intrinsicsModel = 0;
    double focalLengthX = 0.0;
    double focalLengthY = 0.0;
    double principalPointX = 0.0;
    double principalPointY = 0.0;
    double distortionCoefficients[8] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    std::vector<uint8_t> nv12;
};

struct PhospheneBridge {
    // Owns the two shared-memory sections and the state shared between:
    //   - camera saver/sender threads (produce into the cam section),
    //   - the render loop (consumes the phos section, writes gaze).
    std::atomic<bool> running{false};

    HANDLE   camMap  = nullptr;   // C++ -> Python (producer)
    HANDLE   phosMap = nullptr;   // Python -> C++ (consumer)
    uint8_t* camBase  = nullptr;
    uint8_t* phosBase = nullptr;

    // Per-eye publish/consume cursors (process-local, not shared).
    uint32_t camWriteIndex[2] = {0, 0};   // producer's next cam slot
    uint32_t phosLastSeq[2]   = {0, 0};   // last phos publishSeq we consumed

    // Gaze values are atomics because the render loop writes them while the
    // camera sender threads read them to annotate outgoing frames.
    std::atomic<float> gazeTanX[2];   // [0]=L, [1]=R
    std::atomic<float> gazeTanY[2];

    int cropWidth[2]    = {0, 0};
    int cropHeight[2]   = {0, 0};
    int frameWidth[2]   = {0, 0};
    int frameHeight[2]  = {0, 0};
    int rowStride[2]    = {0, 0};

    // Last config mirrored into the cam header, to detect changes (bump configSeq).
    ShmEyeConfig lastConfig[2]{};
    bool haveLastConfig[2] = {false, false};

    std::chrono::steady_clock::time_point lastSendTime[2];

    PhospheneBridge() {
        gazeTanX[0].store(0.0f);
        gazeTanX[1].store(0.0f);
        gazeTanY[0].store(0.0f);
        gazeTanY[1].store(0.0f);

        lastSendTime[0] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        lastSendTime[1] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }

    ShmHeader* camHeader()  { return reinterpret_cast<ShmHeader*>(camBase); }
    ShmHeader* phosHeader() { return reinterpret_cast<ShmHeader*>(phosBase); }
};

static void computePythonCropSize(
    int frameWidth,
    int frameHeight,
    float cropRadiusTan,
    const CapturedEyeFrame* frame,
    int& outCropWidth,
    int& outCropHeight)
{
    if (frame && frame->hasIntrinsics && frame->focalLengthX > 0.0 && frame->focalLengthY > 0.0) {
        // Preferred path: project tangent radius through the camera intrinsics.
        // A tangent of x corresponds to approximately fx * x pixels from center.
        // Handle both pixel-scale and normalized intrinsics
        double fx = frame->focalLengthX;
        double fy = frame->focalLengthY;
        if (fx < 1.0) { fx *= frameWidth; }   // normalized → pixels
        if (fy < 1.0) { fy *= frameHeight; }
        outCropWidth  = (std::max)(32, (int)std::lround(2.0 * fx * cropRadiusTan));
        outCropHeight = (std::max)(32, (int)std::lround(2.0 * fy * cropRadiusTan));
    } else {
        // Fallback path for early startup or missing intrinsics: scale by an
        // approximate camera tangent half-FOV.
        outCropWidth  = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfX) * frameWidth));
        outCropHeight = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfY) * frameHeight));
    }
    if (outCropWidth  & 1) ++outCropWidth;
    if (outCropHeight & 1) ++outCropHeight;
}

static uint8_t* shmCreate(HANDLE& outMap, const char* name, uint32_t size)
{
    // Pagefile-backed named section. Either process may create it first; with a
    // fixed size both agree, and the READY flag (set by C++ last) gates use.
    outMap = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                0, size, name);
    if (!outMap) {
        fprintf(stderr, "[SHM] CreateFileMapping('%s') failed: %lu\n", name, GetLastError());
        return nullptr;
    }
    uint8_t* view = reinterpret_cast<uint8_t*>(
        MapViewOfFile(outMap, FILE_MAP_ALL_ACCESS, 0, 0, size));
    if (!view) {
        fprintf(stderr, "[SHM] MapViewOfFile('%s') failed: %lu\n", name, GetLastError());
        CloseHandle(outMap); outMap = nullptr;
        return nullptr;
    }
    return view;
}

// Mirror the per-eye config into the cam header, bumping configSeq only when it
// actually changes (so Python re-reads intrinsics/crop sizes on demand).
static void writeHeaderConfig(PhospheneBridge& bridge, const CapturedEyeFrame* frame, int eye)
{
    if (!bridge.camBase) return;

    ShmEyeConfig cfg{};
    cfg.cropWidth   = bridge.cropWidth[eye];
    cfg.cropHeight  = bridge.cropHeight[eye];
    cfg.frameWidth  = bridge.frameWidth[eye];
    cfg.frameHeight = bridge.frameHeight[eye];
    cfg.rowStride   = bridge.rowStride[eye];
    if (frame && frame->hasIntrinsics) {
        cfg.intrinsicsModel = frame->intrinsicsModel;
        cfg.intrinsicsValid = 1;
        cfg.focalLengthX = frame->focalLengthX;
        cfg.focalLengthY = frame->focalLengthY;
        cfg.principalPointX = frame->principalPointX;
        cfg.principalPointY = frame->principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = frame->distortionCoefficients[i];
    } else if (bridge.haveLastConfig[eye]) {
        // No intrinsics on this frame: keep the last known ones.
        const ShmEyeConfig& prev = bridge.lastConfig[eye];
        cfg.intrinsicsModel = prev.intrinsicsModel;
        cfg.intrinsicsValid = prev.intrinsicsValid;
        cfg.focalLengthX = prev.focalLengthX;
        cfg.focalLengthY = prev.focalLengthY;
        cfg.principalPointX = prev.principalPointX;
        cfg.principalPointY = prev.principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = prev.distortionCoefficients[i];
    }

    ShmHeader* h = bridge.camHeader();
    const bool modeChanged = (h->blindnessMode != (int)gBlindnessMode);
    if (bridge.haveLastConfig[eye] && !modeChanged &&
        std::memcmp(&cfg, &bridge.lastConfig[eye], sizeof(cfg)) == 0) {
        return;   // nothing changed
    }

    h->eye[eye] = cfg;
    h->blindnessMode = (int)gBlindnessMode;
    std::atomic_thread_fence(std::memory_order_release);
    h->configSeq = h->configSeq + 1;
    bridge.lastConfig[eye] = cfg;
    bridge.haveLastConfig[eye] = true;

    printf("[SHM] config eye=%d: blindness=%d crop=%dx%d frame=%dx%d stride=%d intrinsics=%s fx=%.1f fy=%.1f\n",
           eye, h->blindnessMode, cfg.cropWidth, cfg.cropHeight, cfg.frameWidth, cfg.frameHeight,
           cfg.rowStride, cfg.intrinsicsValid ? "Y" : "N", cfg.focalLengthX, cfg.focalLengthY);
}

static bool startPhospheneBridge(PhospheneBridge& bridge)
{
    bridge.camBase  = shmCreate(bridge.camMap,  kCamMapName,  CAM_MAP_SIZE);
    bridge.phosBase = shmCreate(bridge.phosMap, kPhosMapName, PHOS_MAP_SIZE);
    if (!bridge.camBase || !bridge.phosBase) return false;

    // Reset control blocks for both directions/eyes (fresh pages are zero-filled,
    // but a stale mapping surviving a crash would not be).
    for (int eye = 0; eye < (int)SHM_NUM_EYES; ++eye) {
        ShmCtrl* cc = reinterpret_cast<ShmCtrl*>(shmChannel(bridge.camBase,  eye, CAM_CHANNEL_SIZE));
        ShmCtrl* pc = reinterpret_cast<ShmCtrl*>(shmChannel(bridge.phosBase, eye, PHOS_CHANNEL_SIZE));
        cc->latestIndex = 0; cc->publishSeq = 0;
        pc->latestIndex = 0; pc->publishSeq = 0;
        bridge.camWriteIndex[eye] = 0;
        bridge.phosLastSeq[eye]   = 0;
    }

    // Fill the initial config (fallback sizes from main), then publish READY last.
    bridge.camHeader()->configSeq = 0;
    bridge.camHeader()->blindnessMode = (int)gBlindnessMode;
    for (int eye = 0; eye < (int)SHM_NUM_EYES; ++eye) writeHeaderConfig(bridge, nullptr, eye);

    for (uint8_t* base : {bridge.camBase, bridge.phosBase}) {
        ShmHeader* h = reinterpret_cast<ShmHeader*>(base);
        h->version = SHM_VERSION;
        std::atomic_thread_fence(std::memory_order_release);
        h->magic = SHM_MAGIC;
        std::atomic_thread_fence(std::memory_order_release);
        h->flags = SHM_FLAG_READY;
    }

    bridge.running.store(true);
    printf("[SHM] ready: cam='%s' (%.1f MB), phos='%s' (%.1f MB)\n",
           kCamMapName, CAM_MAP_SIZE / 1048576.0,
           kPhosMapName, PHOS_MAP_SIZE / 1048576.0);
    return true;
}

static void stopPhospheneBridge(PhospheneBridge& bridge)
{
    // Signal shutdown to Python via both headers, then tear down the mappings.
    if (bridge.camBase)  bridge.camHeader()->flags  |= SHM_FLAG_SHUTDOWN;
    if (bridge.phosBase) bridge.phosHeader()->flags |= SHM_FLAG_SHUTDOWN;
    std::atomic_thread_fence(std::memory_order_release);

    bridge.running.store(false);

    if (bridge.camBase)  { UnmapViewOfFile(bridge.camBase);  bridge.camBase  = nullptr; }
    if (bridge.phosBase) { UnmapViewOfFile(bridge.phosBase); bridge.phosBase = nullptr; }
    if (bridge.camMap)   { CloseHandle(bridge.camMap);  bridge.camMap  = nullptr; }
    if (bridge.phosMap)  { CloseHandle(bridge.phosMap); bridge.phosMap = nullptr; }
}

static void sendFrameToPython(
    PhospheneBridge& bridge,
    const CapturedEyeFrame& frame,
    int eye)
{
    if (!bridge.running.load() || !bridge.camBase) return;

    // Throttle Python work so camera capture can run faster than the phosphene
    // processing loop (currently 0 = publish every frame).
    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration<double, std::milli>(now - bridge.lastSendTime[eye]).count();
    if (elapsedMs < kPythonSendIntervalMs) {
        return;
    }
    bridge.lastSendTime[eye] = now;

    bridge.frameWidth[eye] = frame.width;
    bridge.frameHeight[eye] = frame.height;
    bridge.rowStride[eye] = frame.rowStride;
    {
        // Every mode uses the same gaze-centred crop sized to the fixed device
        // field, so the phosphenes follow the eye. The per-disease mask (C++
        // shader) decides which of them are shown.
        const OverlayGeometry geom = overlayGeometryFor(gBlindnessMode);
        computePythonCropSize(
            bridge.frameWidth[eye],
            bridge.frameHeight[eye],
            geom.phospheneRadiusTan,
            &frame,
            bridge.cropWidth[eye],
            bridge.cropHeight[eye]);
    }

    // Publish current config (crop size / intrinsics / mode) into the cam header;
    // this no-ops unless something changed and bumps configSeq when it does.
    writeHeaderConfig(bridge, &frame, eye);

    if (frame.nv12.empty()) return;
    if (frame.nv12.size() > CAM_SLOT_CAP) {
        fprintf(stderr, "[SHM] cam frame eye=%d too large (%zu > %u); dropping\n",
                eye, frame.nv12.size(), CAM_SLOT_CAP);
        return;
    }

    ShmSlotMeta meta{};
    meta.frameId   = (uint32_t)(frame.frameNumber & 0xffffffffu);
    meta.eye       = (uint32_t)eye;
    meta.width     = (uint32_t)frame.width;
    meta.height    = (uint32_t)frame.height;
    meta.rowStride = (uint32_t)frame.rowStride;
    // Latest gaze written by the render loop; tells Python where to crop.
    meta.gazeTanX  = bridge.gazeTanX[eye].load();
    meta.gazeTanY  = bridge.gazeTanY[eye].load();

    uint8_t* channel = shmChannel(bridge.camBase, eye, CAM_CHANNEL_SIZE);
    shmPublish(channel, CAM_SLOT_STRIDE, CAM_SLOT_CAP,
               bridge.camWriteIndex[eye], meta,
               frame.nv12.data(), (uint32_t)frame.nv12.size());
}

static bool uploadLatestPhospheneTexture(PhospheneBridge& bridge, int eye, GLuint tex, int& texWidth, int& texHeight)
{
    // Consume the latest phosphene image from shared memory (render thread only),
    // then upload it to the eye's GL texture.
    if (!bridge.phosBase) return false;

    static std::vector<uint8_t> gray;   // reused scratch; render thread is single
    ShmSlotMeta meta{};
    uint8_t* channel = shmChannel(bridge.phosBase, eye, PHOS_CHANNEL_SIZE);
    if (!shmConsume(channel, PHOS_SLOT_STRIDE, bridge.phosLastSeq[eye], meta, gray)) {
        return false;   // nothing newer than what we last uploaded
    }
    if (gray.empty() || meta.width == 0 || meta.height == 0) return false;
    if (gray.size() != (size_t)meta.width * meta.height) return false;   // size mismatch

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if ((int)meta.width != texWidth || (int)meta.height != texHeight) {
        // Reallocate only when Python changes crop size. Normal frames only need
        // the cheaper sub-image upload below.
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            (GLsizei)meta.width,
            (GLsizei)meta.height,
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            nullptr);
        texWidth = (int)meta.width;
        texHeight = (int)meta.height;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        (GLsizei)meta.width,
        (GLsizei)meta.height,
        GL_RED,
        GL_UNSIGNED_BYTE,
        gray.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Eye camera capture pipeline
// ---------------------------------------------------------------------------

struct EyeCameraCapture {
    // One capture state per eye. The Varjo callback writes "latest"; the saver
    // thread waits on cv, copies the frame, and forwards it to Python.
    std::mutex mutex;
    std::condition_variable cv;
    CapturedEyeFrame latest;
    bool hasNewFrame = false;
    std::atomic<bool> running{false};
    std::thread saverThread;
    varjo_StreamId streamId = varjo_InvalidId;
    int streamWidth = 0;
    int streamHeight = 0;
    int streamRowStride = 0;
    int eye = 0;              // 0 = left, 1 = right
    struct PhospheneBridge* bridge = nullptr;
};

static void cameraSaverThreadMain(EyeCameraCapture* capture)
{
    // Runs once per eye. It intentionally performs UDP sending away from the
    // Varjo DataStream callback so the callback can return quickly.

    while (capture->running.load()) {
        CapturedEyeFrame frame;
        {
            std::unique_lock<std::mutex> lock(capture->mutex);
            // Sleep until the callback publishes a new CPU copy or shutdown is
            // requested.
            capture->cv.wait(lock, [&]() {
                return !capture->running.load() || capture->hasNewFrame;
            });
            if (!capture->running.load()) break;
            frame = capture->latest;
            capture->hasNewFrame = false;
        }

        if (frame.nv12.empty() || frame.width <= 0 || frame.height <= 0) {
            continue;
        }

        if (capture->bridge) {
            // The same latest camera frame is also the input to Python's
            // phosphene generation pipeline.
            // std::chrono::milliseconds time_start = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            sendFrameToPython(*capture->bridge, frame, capture->eye);
            // std::chrono::milliseconds time_end = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
            // std::chrono::milliseconds time_diff = time_end - time_start;
            // printf("[TIMING] Eye %d: sendFrameToPython took %lld ms\n", capture->eye, time_diff.count());
        }
    }
}

// Wraps the 2-eye capture array so a single stream subscription can fan
// out to both eyes. Varjo only allows one subscriber per streamId, and the
// DistortedColor stream delivers both channels in one callback.
struct EyeCameraCaptureSet {
    EyeCameraCapture* captures[2] = {nullptr, nullptr};
};

static void onEyeCameraFrame(
    const varjo_StreamFrame* frame,
    varjo_Session* session,
    void* userData)
{
    // Varjo invokes this callback from its DataStream machinery whenever a
    // distorted-color camera frame is available. Keep work here minimal: copy
    // the frame, capture metadata/intrinsics, notify the worker thread.
    if (!frame || !userData) return;
    if (frame->type != varjo_StreamType_DistortedColor) return;
    if (!(frame->dataFlags & varjo_DataFlag_Buffer)) return;

    EyeCameraCaptureSet* set = reinterpret_cast<EyeCameraCaptureSet*>(userData);

    for (int eye = 0; eye < 2; ++eye) {
        // One callback can contain both left and right channels. Fan the frame
        // out to whichever per-eye captures are active.
        EyeCameraCapture* capture = set->captures[eye];
        if (!capture || !capture->running.load()) continue;

        const varjo_ChannelFlag wantFlag =
            (eye == 0) ? varjo_ChannelFlag_Left : varjo_ChannelFlag_Right;
        const varjo_ChannelIndex channel =
            (eye == 0) ? varjo_ChannelIndex_Left : varjo_ChannelIndex_Right;

        if (!(frame->channels & wantFlag)) continue;

        const varjo_BufferId bufferId =
            varjo_GetBufferId(session, frame->id, frame->frameNumber, channel);
        if (bufferId == varjo_InvalidId) continue;

        // Lock while reading Varjo's buffer. The data pointer is only valid for
        // the locked interval, so copy it before unlocking.
        varjo_LockDataStreamBuffer(session, bufferId);
        varjo_BufferMetadata meta = varjo_GetBufferMetadata(session, bufferId);

        if (meta.type == varjo_BufferType_CPU && meta.format == varjo_TextureFormat_NV12) {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(
                varjo_GetBufferCPUData(session, bufferId));

            if (src && meta.byteSize > 0) {
                std::lock_guard<std::mutex> lock(capture->mutex);
                // Publish a full CPU-owned copy for the saver/sender thread.
                capture->latest.eye = capture->eye;
                capture->latest.width = meta.width;
                capture->latest.height = meta.height;
                capture->latest.rowStride = meta.rowStride;
                capture->latest.frameNumber = frame->frameNumber;
                capture->latest.timestamp = frame->metadata.distortedColor.timestamp;
                capture->latest.nv12.resize((size_t)meta.byteSize);
                std::memcpy(capture->latest.nv12.data(), src, (size_t)meta.byteSize);
                capture->latest.hasIntrinsics = false;
                if (frame->dataFlags & varjo_DataFlag_Intrinsics) {
                    // Intrinsics let Python convert gaze tangent radius to exact
                    // crop pixels instead of using the fallback FOV estimate.
                    varjo_CameraIntrinsics2 intr = varjo_GetCameraIntrinsics2(session, frame->id, frame->frameNumber, channel);
                    capture->latest.hasIntrinsics = true;
                    capture->latest.intrinsicsModel = (int)intr.model;
                    capture->latest.focalLengthX = intr.focalLengthX;
                    capture->latest.focalLengthY = intr.focalLengthY;
                    capture->latest.principalPointX = intr.principalPointX;
                    capture->latest.principalPointY = intr.principalPointY;
                    for (int i = 0; i < 8; ++i) capture->latest.distortionCoefficients[i] = intr.distortionCoefficients[i];
                }
                capture->hasNewFrame = true;
            }
        }

        varjo_UnlockDataStreamBuffer(session, bufferId);
        capture->cv.notify_one();
    }
}

// Single-subscription start: one varjo_StartDataStream with both channel
// flags. Varjo rejects a second subscription to the same stream with
// "Requested stream already in use", so we must share the subscription.
static bool startEyeCameraCaptures(
    varjo_Session* session,
    EyeCameraCapture& leftCapture,
    EyeCameraCapture& rightCapture,
    EyeCameraCaptureSet& set,
    varjo_StreamId& outStreamId)
{
    // Find the best available CPU-readable NV12 distorted-color stream that has
    // both eye channels. CPU/NV12 is chosen because it can be copied directly to
    // Python.
    outStreamId = varjo_InvalidId;

    const int32_t count = varjo_GetDataStreamConfigCount(session);
    if (count <= 0) {
        fprintf(stderr, "[CAM] no data stream configs available\n");
        return false;
    }

    std::vector<varjo_StreamConfig> configs((size_t)count);
    varjo_GetDataStreamConfigs(session, configs.data(), count);

    const varjo_ChannelFlag wantFlags = varjo_ChannelFlag_Left | varjo_ChannelFlag_Right;

    const varjo_StreamConfig* best = nullptr;
    int64_t bestScore = -1;

    for (const auto& cfg : configs) {
        // Score prefers higher resolution, with frame rate as a small tie-break.
        if (cfg.streamType != varjo_StreamType_DistortedColor) continue;
        if (cfg.bufferType != varjo_BufferType_CPU) continue;
        if ((cfg.channelFlags & wantFlags) != wantFlags) continue;
        if (cfg.format != varjo_TextureFormat_NV12) continue;

        const int64_t score = (int64_t)cfg.width * (int64_t)cfg.height * 1000 + cfg.frameRate;
        if (score > bestScore) {
            best = &cfg;
            bestScore = score;
        }
    }

    if (!best) {
        fprintf(stderr, "[CAM] could not find a CPU/NV12 distorted-color stream with both channels\n");
        return false;
    }

    // Both eye captures share the chosen stream metadata.
    for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
        // Start worker threads before starting the stream so the first callback
        // can immediately hand off frames.
        cap->streamId = best->streamId;
        cap->streamWidth = best->width;
        cap->streamHeight = best->height;
        cap->streamRowStride = best->rowStride;
        cap->running.store(true);
        cap->saverThread = std::thread(cameraSaverThreadMain, cap);
    }

    set.captures[0] = &leftCapture;
    set.captures[1] = &rightCapture;

    printf("[CAM] starting shared camera stream (L+R): %dx%d @ %d Hz (rowStride=%d)\n",
           best->width, best->height, best->frameRate, best->rowStride);

    varjo_StartDataStream(
        session,
        best->streamId,
        wantFlags,
        onEyeCameraFrame,
        &set);

    varjo_Error e = varjo_GetError(session);
    if (e != varjo_NoError) {
        fprintf(stderr, "[CAM] StartDataStream failed: %s\n", varjo_GetErrorDesc(e));
        for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
            cap->running.store(false);
            cap->cv.notify_all();
            if (cap->saverThread.joinable()) cap->saverThread.join();
            cap->streamId = varjo_InvalidId;
        }
        set.captures[0] = nullptr;
        set.captures[1] = nullptr;
        return false;
    }

    outStreamId = best->streamId;
    return true;
}

static void stopEyeCameraCaptures(
    varjo_Session* session,
    EyeCameraCapture& leftCapture,
    EyeCameraCapture& rightCapture,
    EyeCameraCaptureSet& set,
    varjo_StreamId streamId)
{
    // Stop the Varjo stream first so no new callbacks race with teardown.
    if (streamId != varjo_InvalidId) {
        varjo_StopDataStream(session, streamId);
    }

    set.captures[0] = nullptr;
    set.captures[1] = nullptr;

    for (EyeCameraCapture* cap : {&leftCapture, &rightCapture}) {
        cap->streamId = varjo_InvalidId;
        cap->running.store(false);
        cap->cv.notify_all();
        if (cap->saverThread.joinable()) {
            cap->saverThread.join();
        }
    }
}

// ---------------------------------------------------------------------------
// MAIN
// ---------------------------------------------------------------------------

int main()
{
    // OpenGL must be ready before creating Varjo GL swapchains.
    if (!createGLContext()) { fprintf(stderr, "GL context failed\n"); return 1; }
    loadGLFunctions();
    printf("[OK] OpenGL: %s\n", (const char*)glGetString(GL_VERSION));

    varjo_Session* session = varjo_SessionInit();
    if (!session) { fprintf(stderr, "SessionInit failed\n"); return 1; }
    printf("[OK] Varjo session\n");

    bool mrAvailable = false;
    if (varjo_HasProperty(session, varjo_PropertyKey_MRAvailable)) {
        mrAvailable = varjo_GetPropertyBool(session, varjo_PropertyKey_MRAvailable);
    }
    if (mrAvailable) {
        // This asks Varjo Base to composite the real camera video behind our
        // submitted transparent layer.
        varjo_MRSetVideoRender(session, varjo_True);
        varjo_Error e = varjo_GetError(session);
        if (e != varjo_NoError)
            printf("[WARN] MRSetVideoRender: %s\n", varjo_GetErrorDesc(e));
        else
            printf("[OK] Mixed Reality video pass-through enabled\n");
    } else {
        printf("[WARN] MR not available — will render VR-only (black + dot)\n");
    }

    varjo_GazeInit(session);
    {
        // Gaze is optional for session startup, but the overlay is gaze-driven,
        // so report any initialization issue clearly.
        varjo_Error e = varjo_GetError(session);
        if (e != varjo_NoError)
            printf("[WARN] GazeInit: %s\n", varjo_GetErrorDesc(e));
        else
            printf("[OK] Gaze initialised\n");
    }

    PhospheneBridge phospheneBridge{};

    // Two per-eye camera states share one UDP bridge. Each worker thread will
    // send its eye's camera frames to Python and receive eye-specific masks.
    EyeCameraCapture cameraCapture[2];
    cameraCapture[0].eye = 0;
    cameraCapture[0].bridge = &phospheneBridge;
    cameraCapture[1].eye = 1;
    cameraCapture[1].bridge = &phospheneBridge;

    EyeCameraCaptureSet cameraSet{};
    varjo_StreamId cameraStreamId = varjo_InvalidId;

    const bool capStarted = startEyeCameraCaptures(
        session, cameraCapture[0], cameraCapture[1], cameraSet, cameraStreamId);
    if (!capStarted) {
        printf("[WARN] camera capture not started\n");
    } else {
        printf("[OK] camera capture started (L+R on shared stream)\n");
    }

    // Overlay geometry depends on the blindness type (see overlayGeometryFor):
    //   Macular  -> black scotoma + phosphenes at the gaze centre.
    //   Glaucoma -> clear central tunnel + phosphenes in the surrounding ring.
    // Both keep the phosphenes anchored to gaze; only the mask/radii differ.
    const OverlayGeometry geom      = overlayGeometryFor(gBlindnessMode);
    const float kSpotRadiusTan      = geom.spotRadiusTan;
    const float kSoftEdgeTan        = geom.softEdgeTan;
    const float kPhospheneRadiusTan = geom.phospheneRadiusTan;

    for (int e = 0; e < 2; ++e) {
        // Before real frames arrive, initialize crop/texture sizes from stream
        // metadata. Later frames with intrinsics may refine these values.
        computePythonCropSize(
            cameraCapture[e].streamWidth,
            cameraCapture[e].streamHeight,
            kPhospheneRadiusTan,
            nullptr,
            phospheneBridge.cropWidth[e],
            phospheneBridge.cropHeight[e]);

        phospheneBridge.frameWidth[e]  = cameraCapture[e].streamWidth;
        phospheneBridge.frameHeight[e] = cameraCapture[e].streamHeight;
        phospheneBridge.rowStride[e]   = cameraCapture[e].streamRowStride;
    }

    if (!startPhospheneBridge(phospheneBridge)) {
        printf("[WARN] UDP phosphene bridge not started\n");
    } else {
        /* config will be sent with the first camera frame (per eye) */
    }

    const int viewCount = varjo_GetViewCount(session);
    printf("[OK] viewCount=%d\n", viewCount);

    // Dynamic foveation can expose four views: left/right context and
    // left/right focus. All of them need overlay rendering.
    std::vector<varjo_Viewport> viewports = calculateViewports(
        session, varjo_TextureSize_Type_DynamicFoveation);
    // std::vector<varjo_Viewport> viewports = calculateViewports(
    // session, varjo_TextureSize_Type_Stereo);
    int32_t totalW = getTotalWidth(viewports);
    int32_t totalH = getTotalHeight(viewports);
    printf("[OK] Atlas %dx%d\n", totalW, totalH);

    varjo_SwapChainConfig2 cfg{};
    // Triple buffering lets Varjo consume one image while the app renders into
    // another, reducing stalls.
    cfg.numberOfTextures = 3;
    cfg.textureWidth     = totalW;
    cfg.textureHeight    = totalH;
    cfg.textureFormat    = varjo_TextureFormat_R8G8B8A8_SRGB;
    cfg.textureArraySize = 1;

    varjo_SwapChain* swapchain = varjo_GLCreateSwapChain(session, &cfg);
    {
        varjo_Error e = varjo_GetError(session);
        if (e != varjo_NoError) {
            fprintf(stderr, "Swapchain: %s\n", varjo_GetErrorDesc(e));
            stopEyeCameraCaptures(session, cameraCapture[0], cameraCapture[1], cameraSet, cameraStreamId);
            return 1;
        }
    }
    printf("[OK] GL swap chain\n");

    std::vector<GLuint> textures(cfg.numberOfTextures);
    std::vector<GLuint> fbos(cfg.numberOfTextures);
    glGenFramebuffers(cfg.numberOfTextures, fbos.data());
    for (int i = 0; i < cfg.numberOfTextures; i++) {
        // Varjo owns the swapchain images; convert each one to its OpenGL
        // texture handle and attach it to an FBO so normal GL rendering can draw
        // into it.
        varjo_Texture vTex = varjo_GetSwapChainImage(swapchain, i);
        textures[i] = varjo_ToGLTexture(vTex);
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[i], 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO %d incomplete: 0x%x\n", i, status);
            stopEyeCameraCaptures(session, cameraCapture[0], cameraCapture[1], cameraSet, cameraStreamId);
            return 1;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("[OK] FBOs\n");

    GLuint blackSpotProgram = createProgram(g_fragSrc);
    GLint locGazeTanX      = glGetUniformLocation(blackSpotProgram, "gazeTanX");
    GLint locGazeTanY      = glGetUniformLocation(blackSpotProgram, "gazeTanY");
    GLint locViewLeft      = glGetUniformLocation(blackSpotProgram, "viewLeft");
    GLint locViewRight     = glGetUniformLocation(blackSpotProgram, "viewRight");
    GLint locViewTop       = glGetUniformLocation(blackSpotProgram, "viewTop");
    GLint locViewBottom    = glGetUniformLocation(blackSpotProgram, "viewBottom");
    GLint locSpotRadiusTan = glGetUniformLocation(blackSpotProgram, "spotRadiusTan");
    GLint locSoftEdgeTan   = glGetUniformLocation(blackSpotProgram, "softEdgeTan");
    GLint locBlindnessMode = glGetUniformLocation(blackSpotProgram, "blindnessMode");

    GLuint phospheneProgram = createProgram(g_phospheneFragSrc);
    GLint locPGazeTanX        = glGetUniformLocation(phospheneProgram, "gazeTanX");
    GLint locPGazeTanY        = glGetUniformLocation(phospheneProgram, "gazeTanY");
    GLint locPViewLeft        = glGetUniformLocation(phospheneProgram, "viewLeft");
    GLint locPViewRight       = glGetUniformLocation(phospheneProgram, "viewRight");
    GLint locPViewTop         = glGetUniformLocation(phospheneProgram, "viewTop");
    GLint locPViewBottom      = glGetUniformLocation(phospheneProgram, "viewBottom");
    GLint locPPhospheneRadius = glGetUniformLocation(phospheneProgram, "phospheneRadiusTan");
    GLint locPPhospheneOp     = glGetUniformLocation(phospheneProgram, "phospheneOpacity");
    GLint locPPhospheneTex    = glGetUniformLocation(phospheneProgram, "phospheneTex");
    GLint locPSpotRadiusTan = glGetUniformLocation(phospheneProgram, "spotRadiusTan");
    GLint locPSoftEdgeTan   = glGetUniformLocation(phospheneProgram, "softEdgeTan");
    GLint locPBlindnessMode = glGetUniformLocation(phospheneProgram, "blindnessMode");

    printf("[OK] Shaders\n");

    GLuint phospheneTexture[2] = {0, 0};
    glGenTextures(2, phospheneTexture);
    for (int e = 0; e < 2; ++e) {
        // One single-channel texture per eye. Python returns grayscale bytes,
        // so GL_R8 is enough and the shader expands it to RGBA.
        glBindTexture(GL_TEXTURE_2D, phospheneTexture[e]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            phospheneBridge.cropWidth[e],
            phospheneBridge.cropHeight[e],
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            nullptr);
    }
    glBindTexture(GL_TEXTURE_2D, 0);

    glUseProgram(phospheneProgram);
    glUniform1i(locPPhospheneTex, 0);
    glUseProgram(0);

    int phospheneTexWidth[2]  = {phospheneBridge.cropWidth[0],  phospheneBridge.cropWidth[1]};
    int phospheneTexHeight[2] = {phospheneBridge.cropHeight[0], phospheneBridge.cropHeight[1]};

    GLuint vao;
    glGenVertexArrays(1, &vao);

    float gazeTanX = 0.0f;
    float gazeTanY = 0.0f;


    std::vector<varjo_LayerMultiProjView> views(viewCount);
    for (int i = 0; i < viewCount; i++) {
        // Each Varjo view points at a sub-rectangle of the same swapchain atlas.
        // Projection and view matrices are filled every frame below.
        memset(&views[i], 0, sizeof(views[i]));
        const varjo_Viewport& vp = viewports[i];

        views[i].viewport = varjo_SwapChainViewport{
            swapchain, vp.x, vp.y, vp.width, vp.height, 0, 0};
        views[i].extension = nullptr;
    }

    varjo_LayerMultiProj projLayer{};
    // The layer is transparent except where the shaders draw alpha. With MR
    // video pass-through enabled, transparent pixels reveal the real cameras.
    projLayer.header.type  = varjo_LayerMultiProjType;
    projLayer.header.flags = varjo_LayerFlag_BlendMode_AlphaBlend;
    projLayer.space        = varjo_SpaceLocal;
    projLayer.viewCount    = viewCount;
    projLayer.views        = views.data();

    varjo_LayerHeader* layerPtrs[] = {&projLayer.header};
    varjo_SubmitInfoLayers submitInfo{};
    submitInfo.layerCount = 1;
    submitInfo.layers     = layerPtrs;

    varjo_FrameInfo* frameInfo = varjo_CreateFrameInfo(session);

    printf("     Parallel camera capture forwards frames to Python over UDP\n");
    printf("     Press ESC to quit.\n");

    int fc = 0;
    while (true) {
        // Simple local exit condition for the sample program.
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        varjo_Event evt{};
        while (varjo_PollEvent(session, &evt)) {
            // MR camera status changes are useful diagnostics when pass-through
            // suddenly disappears.
            if (evt.header.type == varjo_EventType_MRDeviceStatus) {
                if (evt.data.mrDeviceStatus.status == varjo_MRDeviceStatus_Connected)
                    printf("[EVT] MR cameras connected\n");
                else if (evt.data.mrDeviceStatus.status == varjo_MRDeviceStatus_Disconnected)
                    printf("[EVT] MR cameras disconnected!\n");
            }
        }

        varjo_WaitSync(session, frameInfo);

        varjo_Gaze gaze{};
        bool gazeValid = false;
        {
            // Grab the latest combined/per-eye gaze state. If invalid, the code
            // keeps the previous tangent values so the overlay does not jump.
            varjo_Gaze g = varjo_GetGaze(session);
            if (g.status == varjo_GazeStatus_Valid) { gaze = g; gazeValid = true; }
        }

        varjo_BeginFrameWithLayers(session);

        int sci = 0;
        varjo_AcquireSwapChainImage(swapchain, &sci);

        // Render into the acquired Varjo swapchain image.
        glBindFramebuffer(GL_FRAMEBUFFER, fbos[sci]);
        glViewport(0, 0, totalW, totalH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        // Source colors are effectively premultiplied by their alpha in the
        // shaders. This blend mode composites the overlay onto transparent black.
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(vao);
        glDisable(GL_DEPTH_TEST);

        float gazeTanXPerEye[2] = {gazeTanX, gazeTanX};
        float gazeTanYPerEye[2] = {gazeTanY, gazeTanY};

        if (gazeValid) {
            // Convert a 3D gaze ray into tangent space. This is roughly the same
            // coordinate system Varjo uses to describe each view frustum:
            // tanX = x / z, tanY = y / z.

            const float kMinGz = 0.35f;
            const float kMaxTan = 1.5f;

            auto safeTan = [&](double fx, double fy, double fz, float& outX, float& outY) -> bool {
                // Reject near-parallel or extreme rays; otherwise one bad gaze
                // sample could fling the overlay far outside the useful view.
                if (fabs(fz) < kMinGz) return false;
                const float tx = (float)(fx / fz);
                const float ty = (float)(fy / fz);
                if (fabs(tx) > kMaxTan || fabs(ty) > kMaxTan) return false;
                outX = tx; outY = ty;
                return true;
            };

            // fallback from combined gaze direction
            float cx = 0.0f, cy = 0.0f;
            if (safeTan(gaze.gaze.forward[0], gaze.gaze.forward[1], gaze.gaze.forward[2], cx, cy)) {
                gazeTanX = cx;
                gazeTanY = cy;
                gazeTanXPerEye[0] = gazeTanXPerEye[1] = cx;
                gazeTanYPerEye[0] = gazeTanYPerEye[1] = cy;
            }

            // better binocular fixation if focus distance is usable
            if (gaze.focusDistance > 0.05 && gaze.focusDistance < 2.0) {
                // If Varjo provides a usable fixation distance, compute the 3D
                // fixation point and then express that same point relative to
                // each eye. This improves binocular alignment.
                const double px = gaze.gaze.origin[0] + gaze.gaze.forward[0] * gaze.focusDistance;
                const double py = gaze.gaze.origin[1] + gaze.gaze.forward[1] * gaze.focusDistance;
                const double pz = gaze.gaze.origin[2] + gaze.gaze.forward[2] * gaze.focusDistance;

                auto pointToEyeTan = [&](const varjo_Ray& eyeRay, float& tx, float& ty) -> bool {
                    const double dx = px - eyeRay.origin[0];
                    const double dy = py - eyeRay.origin[1];
                    const double dz = pz - eyeRay.origin[2];
                    return safeTan(dx, dy, dz, tx, ty);
                };

                if (gaze.leftStatus == varjo_GazeEyeStatus_Tracked) {
                    pointToEyeTan(gaze.leftEye, gazeTanXPerEye[0], gazeTanYPerEye[0]);
                }
                if (gaze.rightStatus == varjo_GazeEyeStatus_Tracked) {
                    pointToEyeTan(gaze.rightEye, gazeTanXPerEye[1], gazeTanYPerEye[1]);
                }
            }
        }

        // Share per-eye gaze with bridge (camera capture threads read these)
        phospheneBridge.gazeTanX[0].store(gazeTanXPerEye[0]);
        phospheneBridge.gazeTanY[0].store(gazeTanYPerEye[0]);
        phospheneBridge.gazeTanX[1].store(gazeTanXPerEye[1]);
        phospheneBridge.gazeTanY[1].store(gazeTanYPerEye[1]);

        // Upload latest phosphene texture for each eye
        uploadLatestPhospheneTexture(phospheneBridge, 0, phospheneTexture[0], phospheneTexWidth[0], phospheneTexHeight[0]);
        uploadLatestPhospheneTexture(phospheneBridge, 1, phospheneTexture[1], phospheneTexWidth[1], phospheneTexHeight[1]);

        auto viewIndexToEye = [&](int viewIdx) -> int {
                if (viewCount == 2) {
                    // [LEFT, RIGHT]
                    return viewIdx;
                }
                if (viewCount == 4) {
                    // [LEFT_CONTEXT, RIGHT_CONTEXT, LEFT_FOCUS, RIGHT_FOCUS]
                    return (viewIdx == 0 || viewIdx == 2) ? 0 : 1;
                }
                return (viewIdx & 1);
            };

        for (int i = 0; i < viewCount; i++) {
            // The FOV tangents and frameInfo matrices are per-view, so every
            // atlas viewport gets its own projection/view metadata.
            varjo_FovTangents tangents = varjo_GetFovTangents(session, i);
            varjo_Matrix proj = varjo_GetProjectionMatrix(&tangents);
            varjo_UpdateNearFarPlanes(proj.value, varjo_ClipRangeZeroToOne, 0.01, 300.0);

            std::copy(proj.value, proj.value + 16, views[i].projection.value);
            std::copy(frameInfo->views[i].viewMatrix,
                    frameInfo->views[i].viewMatrix + 16,
                    views[i].view.value);

            const varjo_Viewport& vp = viewports[i];
            const int eyeIdx = viewIndexToEye(i);

            const float eyeGazeTanX = gazeTanXPerEye[eyeIdx];
            const float eyeGazeTanY = gazeTanYPerEye[eyeIdx];

            // Black spot
            // Draw first so the phosphene overlay can appear inside/on top of
            // the scotoma region.
            glUseProgram(blackSpotProgram);
            glUniform1f(locGazeTanX,       eyeGazeTanX);
            glUniform1f(locGazeTanY,       eyeGazeTanY);
            glUniform1f(locViewLeft,       (float)tangents.left);
            glUniform1f(locViewRight,      (float)tangents.right);
            glUniform1f(locViewTop,        (float)tangents.top);
            glUniform1f(locViewBottom,     (float)tangents.bottom);
            glUniform1f(locSpotRadiusTan,  kSpotRadiusTan);
            glUniform1f(locSoftEdgeTan,    kSoftEdgeTan);
            glUniform1i(locBlindnessMode, (int)gBlindnessMode);
            glViewport(vp.x, vp.y, vp.width, vp.height);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Phosphene overlay -- draw on ALL views, not just i < 2
            // The shader samples the latest Python-returned grayscale mask for
            // this eye and positions it at the same gaze tangent coordinate.
            glUseProgram(phospheneProgram);
            glUniform1f(locPGazeTanX,        eyeGazeTanX);
            glUniform1f(locPGazeTanY,        eyeGazeTanY);
            glUniform1f(locPViewLeft,        (float)tangents.left);
            glUniform1f(locPViewRight,       (float)tangents.right);
            glUniform1f(locPViewTop,         (float)tangents.top);
            glUniform1f(locPViewBottom,      (float)tangents.bottom);
            glUniform1f(locPPhospheneRadius, kPhospheneRadiusTan);
            glUniform1f(locPPhospheneOp,     1.0f);
            glUniform1f(locPSpotRadiusTan,   kSpotRadiusTan);
            glUniform1f(locPSoftEdgeTan,     kSoftEdgeTan);
            glUniform1i(locPBlindnessMode,   (int)gBlindnessMode);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, phospheneTexture[eyeIdx]);
            glViewport(vp.x, vp.y, vp.width, vp.height);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        varjo_ReleaseSwapChainImage(swapchain);

        projLayer.header.flags = varjo_LayerFlag_BlendMode_AlphaBlend;

        // Submit this frame's transparent overlay layer to Varjo.
        submitInfo.frameNumber = frameInfo->frameNumber;
        varjo_EndFrameWithLayers(session, &submitInfo);

        fc++;
    }

    printf("Shutting down...\n");

    // Tear down threads/streams before destroying the Varjo session or GL
    // objects they depend on.
    stopEyeCameraCaptures(session, cameraCapture[0], cameraCapture[1], cameraSet, cameraStreamId);
    stopPhospheneBridge(phospheneBridge);

    if (mrAvailable) {
        varjo_MRSetVideoRender(session, varjo_False);
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteProgram(blackSpotProgram);
    glDeleteProgram(phospheneProgram);
    glDeleteTextures(2, phospheneTexture);
    glDeleteFramebuffers(cfg.numberOfTextures, fbos.data());
    varjo_FreeFrameInfo(frameInfo);
    varjo_FreeSwapChain(swapchain);
    varjo_SessionShutDown(session);
    destroyGLContext();
    printf("Done.\n");
    return 0;
}
