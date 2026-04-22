////////////////////////////////////////////////////////////////////////////////
// main.cpp — Varjo XR-4 Mixed Reality: camera pass-through + gaze dot overlay
// + left camera capture in parallel using Varjo DataStream API.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "Ws2_32.lib")

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
    void* p = (void*)wglGetProcAddress(name);
    if (p == nullptr || p == (void*)0x1 || p == (void*)0x2 || p == (void*)0x3 || p == (void*)-1) {
        static HMODULE module = LoadLibraryA("opengl32.dll");
        p = (void*)GetProcAddress(module, name);
    }
    return p;
}

static void loadGLFunctions()
{
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
    wglMakeCurrent(nullptr, nullptr);
    if (g_hglrc) wglDeleteContext(g_hglrc);
    if (g_hwnd) DestroyWindow(g_hwnd);
}

// ---------------------------------------------------------------------------
// Shader: transparent background + blue gaze dot
// ---------------------------------------------------------------------------

static const char* g_vertSrc = R"(
#version 330 core
out vec2 vUV;
void main() {
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

void main() {
    // Convert this pixel from local viewport UV to view tangent space
    float pxTanX = mix(viewLeft,   viewRight,  vUV.x);
    float pxTanY = mix(viewTop,    viewBottom, vUV.y);

    vec2 diff = vec2(pxTanX - gazeTanX, pxTanY - gazeTanY);
    float d = length(diff);

    float alpha = 1.0 - smoothstep(spotRadiusTan - softEdgeTan,
                                   spotRadiusTan,
                                   d);

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

void main() {
    float pxTanX = mix(viewLeft,  viewRight,  vUV.x);
    float pxTanY = mix(viewTop,   viewBottom, vUV.y);

    vec2 diff = vec2(pxTanX - gazeTanX, pxTanY - gazeTanY);
    float radius = phospheneRadiusTan;

    if (abs(diff.x) > radius || abs(diff.y) > radius) {
        fragColor = vec4(0.0);
        return;
    }

    vec2 uv = diff / (2.0 * radius) + vec2(0.5, 0.5);
    float p = texture(phospheneTex, uv).r * phospheneOpacity;

    float circleMask = 1.0 - smoothstep(radius - 0.01, radius, length(diff));
    float a = clamp(p * circleMask, 0.0, 1.0);
    fragColor = vec4(a, a, a, a);
}
)";

struct CropRect
{
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
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.x + v.width));
    return m;
}
static int32_t getTotalHeight(const std::vector<varjo_Viewport>& vp) {
    int32_t m = 0;
    for (auto& v : vp) m = (std::max)(m, (int32_t)(v.y + v.height));
    return m;
}


// ---------------------------------------------------------------------------
// UDP bridge to Python
// ---------------------------------------------------------------------------

constexpr uint8_t FRAME_START   = 0xFF;
constexpr uint8_t SHUTDOWN_BYTE = 0xFE;
constexpr uint8_t CONFIG_OPCODE = 0xFD;
constexpr uint8_t CONFIG_ACK_OPCODE = 0xFC;   // NEW
constexpr int UDP_CHUNK_SIZE    = 8000;
constexpr int PYTHON_RECV_PORT  = 5000;
constexpr int CPP_RECV_PORT     = 5001;
constexpr float kCameraTanHalfX = 0.6f;
constexpr float kCameraTanHalfY = 0.6f;
constexpr double kPythonSendIntervalMs = 66.0; // ~15 FPS

#pragma pack(push, 1)
struct PythonConfigPacket {
    uint8_t opcode;
    uint8_t eye;              // 0 = left, 1 = right
    int32_t cropWidth;
    int32_t cropHeight;
    int32_t frameWidth;
    int32_t frameHeight;
    int32_t rowStride;
    int32_t intrinsicsModel;
    int32_t intrinsicsValid;
    double focalLengthX;
    double focalLengthY;
    double principalPointX;
    double principalPointY;
    double distortionCoefficients[8];
};

struct PythonConfigAckPacket {
    uint8_t opcode;
    uint8_t eye;   // 0 = left, 1 = right
};

struct PythonFrameChunkHeader {
    uint8_t opcode;
    uint8_t eye;              // 0 = left, 1 = right
    uint32_t frameId;
    uint32_t chunkIndex;
    uint32_t chunkCount;
    float gazeTanX;
    float gazeTanY;
};

struct CppFrameChunkHeader {
    uint8_t opcode;
    uint8_t eye;              // 0 = left, 1 = right
    uint32_t frameId;
    uint32_t chunkIndex;
    uint32_t chunkCount;
};
#pragma pack(pop)

struct PhospheneFrame {
    uint32_t frameId = 0;
    int width = 0;
    int height = 0;
    bool dirty = false;
    std::vector<uint8_t> gray;
};

struct RxAssembly {
    uint32_t total = 0;
    size_t received = 0;
    std::vector<std::vector<uint8_t>> chunks;
};

struct CapturedEyeFrame {
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
    std::atomic<bool> running{false};
    SOCKET sendSock = INVALID_SOCKET;
    SOCKET recvSock = INVALID_SOCKET;
    sockaddr_in pythonAddr{};
    std::thread recvThread;

    std::mutex mutex;
    // Per-eye receive assembly, keyed by frameId per eye (index 0=L, 1=R)
    std::unordered_map<uint32_t, RxAssembly> assemblies[2];
    PhospheneFrame latest[2];

    std::atomic<float> gazeTanX[2];   // [0]=L, [1]=R
    std::atomic<float> gazeTanY[2];

    int cropWidth[2]    = {0, 0};
    int cropHeight[2]   = {0, 0};
    int frameWidth[2]   = {0, 0};
    int frameHeight[2]  = {0, 0};
    int rowStride[2]    = {0, 0};

    std::atomic<bool> configAcked[2];
    PythonConfigPacket lastConfig[2]{};
    bool haveLastConfig[2] = {false, false};

    std::chrono::steady_clock::time_point lastSendTime[2];

    PhospheneBridge() {
        gazeTanX[0].store(0.0f);
        gazeTanX[1].store(0.0f);
        gazeTanY[0].store(0.0f);
        gazeTanY[1].store(0.0f);

        configAcked[0].store(false);
        configAcked[1].store(false);

        lastSendTime[0] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        lastSendTime[1] = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    }
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
        // Handle both pixel-scale and normalized intrinsics
        double fx = frame->focalLengthX;
        double fy = frame->focalLengthY;
        if (fx < 1.0) { fx *= frameWidth; }   // normalized → pixels
        if (fy < 1.0) { fy *= frameHeight; }
        outCropWidth  = (std::max)(32, (int)std::lround(2.0 * fx * cropRadiusTan));
        outCropHeight = (std::max)(32, (int)std::lround(2.0 * fy * cropRadiusTan));
    } else {
        outCropWidth  = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfX) * frameWidth));
        outCropHeight = (std::max)(32, (int)std::lround((cropRadiusTan / kCameraTanHalfY) * frameHeight));
    }
    if (outCropWidth  & 1) ++outCropWidth;
    if (outCropHeight & 1) ++outCropHeight;
}

static bool startPhospheneBridge(PhospheneBridge& bridge)
{
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[UDP] WSAStartup failed\n");
        return false;
    }

    bridge.sendSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bridge.recvSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (bridge.sendSock == INVALID_SOCKET || bridge.recvSock == INVALID_SOCKET) {
        fprintf(stderr, "[UDP] socket creation failed\n");
        return false;
    }

    int sendBuf = 4 * 1024 * 1024;
    setsockopt(bridge.sendSock, SOL_SOCKET, SO_SNDBUF, (const char*)&sendBuf, sizeof(sendBuf));
    int recvBuf = 4 * 1024 * 1024;
    setsockopt(bridge.recvSock, SOL_SOCKET, SO_RCVBUF, (const char*)&recvBuf, sizeof(recvBuf));

    DWORD timeoutMs = 100;
    setsockopt(bridge.recvSock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeoutMs, sizeof(timeoutMs));

    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    bindAddr.sin_port = htons(CPP_RECV_PORT);
    if (bind(bridge.recvSock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        fprintf(stderr, "[UDP] bind failed on port %d\n", CPP_RECV_PORT);
        return false;
    }

    bridge.pythonAddr.sin_family = AF_INET;
    bridge.pythonAddr.sin_port = htons(PYTHON_RECV_PORT);
    inet_pton(AF_INET, "127.0.0.1", &bridge.pythonAddr.sin_addr);

    bridge.running.store(true);
    bridge.recvThread = std::thread([&bridge]() {
        std::vector<uint8_t> packet(UDP_CHUNK_SIZE + 64);

        while (bridge.running.load()) {
            sockaddr_in from{};
            int fromLen = sizeof(from);
            int bytes = recvfrom(
                bridge.recvSock,
                reinterpret_cast<char*>(packet.data()),
                (int)packet.size(),
                0,
                (sockaddr*)&from,
                &fromLen);

            if (bytes == SOCKET_ERROR) {
                const int err = WSAGetLastError();
                if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
                    continue;
                }
                if (bridge.running.load()) {
                    fprintf(stderr, "[UDP] recvfrom failed: %d\n", err);
                }
                continue;
            }

            if (bytes == 1 && packet[0] == SHUTDOWN_BYTE) {
                continue;
            }

            if (bytes == (int)sizeof(PythonConfigAckPacket)) {
                PythonConfigAckPacket ack{};
                std::memcpy(&ack, packet.data(), sizeof(ack));

                if (ack.opcode == CONFIG_ACK_OPCODE && ack.eye < 2) {
                    bridge.configAcked[ack.eye].store(true);
                    printf("[UDP] received config ACK for eye=%d\n", (int)ack.eye);
                }
                continue;
            }

            if (bytes < (int)sizeof(CppFrameChunkHeader)) {
                continue;
            }

            CppFrameChunkHeader hdr{};
            std::memcpy(&hdr, packet.data(), sizeof(hdr));
            if (hdr.opcode != FRAME_START) {
                continue;
            }

            const int payloadBytes = bytes - (int)sizeof(CppFrameChunkHeader);
            if (payloadBytes <= 0) {
                continue;
            }

            const int eye = (hdr.eye == 0) ? 0 : 1;

            std::lock_guard<std::mutex> lock(bridge.mutex);

            RxAssembly& assembly = bridge.assemblies[eye][hdr.frameId];
            if (assembly.total == 0) {
                assembly.total = hdr.chunkCount;
                assembly.received = 0;
                assembly.chunks.resize(hdr.chunkCount);
            }

            if (hdr.chunkIndex >= assembly.chunks.size()) {
                continue;
            }

            if (assembly.chunks[hdr.chunkIndex].empty()) {
                assembly.chunks[hdr.chunkIndex].assign(
                    packet.data() + sizeof(CppFrameChunkHeader),
                    packet.data() + bytes);
                assembly.received++;
            }

            if (assembly.received == assembly.total) {
                std::vector<uint8_t> joined;
                size_t totalBytes = 0;
                for (const auto& c : assembly.chunks) totalBytes += c.size();
                joined.reserve(totalBytes);
                for (const auto& c : assembly.chunks) {
                    joined.insert(joined.end(), c.begin(), c.end());
                }

                if ((int)joined.size() == bridge.cropWidth[eye] * bridge.cropHeight[eye]) {
                    bridge.latest[eye].frameId = hdr.frameId;
                    bridge.latest[eye].width  = bridge.cropWidth[eye];
                    bridge.latest[eye].height = bridge.cropHeight[eye];
                    bridge.latest[eye].gray   = std::move(joined);
                    bridge.latest[eye].dirty  = true;
                }

                bridge.assemblies[eye].erase(hdr.frameId);
                if (bridge.assemblies[eye].size() > 32) {
                    bridge.assemblies[eye].clear();
                }
            }
        }
    });

    printf("[UDP] C++ receiver on 127.0.0.1:%d, sender to 127.0.0.1:%d\n",
           CPP_RECV_PORT, PYTHON_RECV_PORT);
    return true;
}

static void stopPhospheneBridge(PhospheneBridge& bridge)
{
    if (bridge.sendSock != INVALID_SOCKET) {
        uint8_t shutdown = SHUTDOWN_BYTE;
        sendto(
            bridge.sendSock,
            reinterpret_cast<const char*>(&shutdown),
            1,
            0,
            (sockaddr*)&bridge.pythonAddr,
            sizeof(bridge.pythonAddr));
    }

    bridge.running.store(false);

    if (bridge.recvSock != INVALID_SOCKET) {
        closesocket(bridge.recvSock);
        bridge.recvSock = INVALID_SOCKET;
    }

    if (bridge.recvThread.joinable()) {
        bridge.recvThread.join();
    }

    if (bridge.sendSock != INVALID_SOCKET) {
        closesocket(bridge.sendSock);
        bridge.sendSock = INVALID_SOCKET;
    }

    WSACleanup();
}

static PythonConfigPacket buildPythonConfigPacket(
    const PhospheneBridge& bridge,
    const CapturedEyeFrame* frame,
    int eye)
{
    PythonConfigPacket cfg{};
    cfg.opcode = CONFIG_OPCODE;
    cfg.eye = (uint8_t)eye;
    cfg.cropWidth = bridge.cropWidth[eye];
    cfg.cropHeight = bridge.cropHeight[eye];
    cfg.frameWidth = bridge.frameWidth[eye];
    cfg.frameHeight = bridge.frameHeight[eye];
    cfg.rowStride = bridge.rowStride[eye];

    if (frame && frame->hasIntrinsics) {
        cfg.intrinsicsModel = frame->intrinsicsModel;
        cfg.intrinsicsValid = 1;
        cfg.focalLengthX = frame->focalLengthX;
        cfg.focalLengthY = frame->focalLengthY;
        cfg.principalPointX = frame->principalPointX;
        cfg.principalPointY = frame->principalPointY;
        for (int i = 0; i < 8; ++i) {
            cfg.distortionCoefficients[i] = frame->distortionCoefficients[i];
        }
    }

    return cfg;
}

static bool sendPythonConfigPacket(PhospheneBridge& bridge, const PythonConfigPacket& cfg)
{
    const int sent = sendto(
        bridge.sendSock,
        reinterpret_cast<const char*>(&cfg),
        sizeof(cfg),
        0,
        (sockaddr*)&bridge.pythonAddr,
        sizeof(bridge.pythonAddr));

    if (sent != (int)sizeof(cfg)) {
        fprintf(stderr, "[UDP] failed to send config packet (eye=%d)\n", (int)cfg.eye);
        return false;
    }

    printf("[UDP] sent config eye=%d: crop=%dx%d frame=%dx%d stride=%d intrinsics=%s model=%d fx=%.1f fy=%.1f cx=%.1f cy=%.1f\n",
           (int)cfg.eye, cfg.cropWidth, cfg.cropHeight, cfg.frameWidth, cfg.frameHeight,
           cfg.rowStride, cfg.intrinsicsValid ? "Y" : "N", cfg.intrinsicsModel,
           cfg.focalLengthX, cfg.focalLengthY, cfg.principalPointX, cfg.principalPointY);

    return true;
}

static bool sendPythonConfig(PhospheneBridge& bridge, const CapturedEyeFrame* frame, int eye)
{
    PythonConfigPacket cfg{};
    cfg.opcode = CONFIG_OPCODE;
    cfg.eye = (uint8_t)eye;
    cfg.cropWidth = bridge.cropWidth[eye];
    cfg.cropHeight = bridge.cropHeight[eye];
    cfg.frameWidth = bridge.frameWidth[eye];
    cfg.frameHeight = bridge.frameHeight[eye];
    cfg.rowStride = bridge.rowStride[eye];
    if (frame && frame->hasIntrinsics) {
        cfg.intrinsicsModel = frame->intrinsicsModel;
        cfg.intrinsicsValid = 1;
        cfg.focalLengthX = frame->focalLengthX;
        cfg.focalLengthY = frame->focalLengthY;
        cfg.principalPointX = frame->principalPointX;
        cfg.principalPointY = frame->principalPointY;
        for (int i = 0; i < 8; ++i) cfg.distortionCoefficients[i] = frame->distortionCoefficients[i];
    }

    const int sent = sendto(
        bridge.sendSock,
        reinterpret_cast<const char*>(&cfg),
        sizeof(cfg),
        0,
        (sockaddr*)&bridge.pythonAddr,
        sizeof(bridge.pythonAddr));

    if (sent != (int)sizeof(cfg)) {
        fprintf(stderr, "[UDP] failed to send config packet (eye=%d)\n", eye);
        return false;
    }

    printf("[UDP] sent config eye=%d: crop=%dx%d frame=%dx%d stride=%d intrinsics=%s model=%d fx=%.1f fy=%.1f cx=%.1f cy=%.1f\n",
           eye, cfg.cropWidth, cfg.cropHeight, cfg.frameWidth, cfg.frameHeight,
           cfg.rowStride, cfg.intrinsicsValid ? "Y" : "N", cfg.intrinsicsModel,
           cfg.focalLengthX, cfg.focalLengthY, cfg.principalPointX, cfg.principalPointY);
    return true;
}

static void sendFrameToPython(
    PhospheneBridge& bridge,
    const CapturedEyeFrame& frame,
    int eye)
{
    if (!bridge.running.load()) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsedMs = std::chrono::duration<double, std::milli>(now - bridge.lastSendTime[eye]).count();
    if (elapsedMs < kPythonSendIntervalMs) {
        return;
    }
    bridge.lastSendTime[eye] = now;

    bridge.frameWidth[eye] = frame.width;
    bridge.frameHeight[eye] = frame.height;
    bridge.rowStride[eye] = frame.rowStride;
    computePythonCropSize(
        bridge.frameWidth[eye],
        bridge.frameHeight[eye],
        0.24f - 0.04f,
        &frame,
        bridge.cropWidth[eye],
        bridge.cropHeight[eye]);
    
        
    if (!bridge.configAcked[eye].load()) {
        sendPythonConfig(bridge, &frame, eye);
    }

    const uint32_t frameId = (uint32_t)(frame.frameNumber & 0xffffffffu);
    const float gx = bridge.gazeTanX[eye].load();
    const float gy = bridge.gazeTanY[eye].load();

    const size_t totalBytes = frame.nv12.size();
    const uint32_t chunkCount = (uint32_t)((totalBytes + UDP_CHUNK_SIZE - 1) / UDP_CHUNK_SIZE);

    std::vector<uint8_t> packet(sizeof(PythonFrameChunkHeader) + UDP_CHUNK_SIZE);

    for (uint32_t i = 0; i < chunkCount; ++i) {
        const size_t off = (size_t)i * UDP_CHUNK_SIZE;
        const size_t payloadBytes = (std::min)((size_t)UDP_CHUNK_SIZE, totalBytes - off);

        PythonFrameChunkHeader hdr{};
        hdr.opcode = FRAME_START;
        hdr.eye = (uint8_t)eye;
        hdr.frameId = frameId;
        hdr.chunkIndex = i;
        hdr.chunkCount = chunkCount;
        hdr.gazeTanX = gx;
        hdr.gazeTanY = gy;

        std::memcpy(packet.data(), &hdr, sizeof(hdr));
        std::memcpy(packet.data() + sizeof(hdr), frame.nv12.data() + off, payloadBytes);

        sendto(
            bridge.sendSock,
            reinterpret_cast<const char*>(packet.data()),
            (int)(sizeof(hdr) + payloadBytes),
            0,
            (sockaddr*)&bridge.pythonAddr,
            sizeof(bridge.pythonAddr));
    }
}

static bool uploadLatestPhospheneTexture(PhospheneBridge& bridge, int eye, GLuint tex, int& texWidth, int& texHeight)
{
    PhospheneFrame latest{};
    {
        std::lock_guard<std::mutex> lock(bridge.mutex);
        if (!bridge.latest[eye].dirty) {
            return false;
        }
        latest = bridge.latest[eye];
        bridge.latest[eye].dirty = false;
    }

    if (latest.gray.empty()) return false;

    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (latest.width != texWidth || latest.height != texHeight) {
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            latest.width,
            latest.height,
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            nullptr);
        texWidth = latest.width;
        texHeight = latest.height;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        latest.width,
        latest.height,
        GL_RED,
        GL_UNSIGNED_BYTE,
        latest.gray.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}

// ---------------------------------------------------------------------------
// Left camera capture pipeline
// ---------------------------------------------------------------------------

struct EyeCameraCapture {
    std::mutex mutex;
    std::condition_variable cv;
    CapturedEyeFrame latest;
    bool hasNewFrame = false;
    bool firstFrameSaved = false;
    std::atomic<bool> running{false};
    std::thread saverThread;
    varjo_StreamId streamId = varjo_InvalidId;
    int streamWidth = 0;
    int streamHeight = 0;
    int streamRowStride = 0;
    int eye = 0;              // 0 = left, 1 = right
    struct PhospheneBridge* bridge = nullptr;
};

static inline uint8_t clampToByte(int v)
{
    return (uint8_t)((v < 0) ? 0 : (v > 255) ? 255 : v);
}

static std::vector<uint8_t> nv12ToBgra(const uint8_t* src, int width, int height, int rowStride)
{
    const uint8_t* yPlane = src;
    const uint8_t* uvPlane = src + rowStride * height;
    std::vector<uint8_t> bgra(width * height * 4);

    for (int y = 0; y < height; ++y) {
        const uint8_t* yRow = yPlane + y * rowStride;
        const uint8_t* uvRow = uvPlane + (y / 2) * rowStride;
        uint8_t* dstRow = bgra.data() + y * width * 4;

        for (int x = 0; x < width; ++x) {
            int Y = yRow[x];
            int U = uvRow[(x & ~1) + 0];
            int V = uvRow[(x & ~1) + 1];

            int C = Y - 16;
            int D = U - 128;
            int E = V - 128;
            if (C < 0) C = 0;

            int R = (298 * C + 409 * E + 128) >> 8;
            int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
            int B = (298 * C + 516 * D + 128) >> 8;

            dstRow[x * 4 + 0] = clampToByte(B);
            dstRow[x * 4 + 1] = clampToByte(G);
            dstRow[x * 4 + 2] = clampToByte(R);
            dstRow[x * 4 + 3] = 255;
        }
    }

    return bgra;
}

#pragma pack(push, 1)
struct BmpFileHeader {
    uint16_t bfType;
    uint32_t bfSize;
    uint16_t bfReserved1;
    uint16_t bfReserved2;
    uint32_t bfOffBits;
};

struct BmpInfoHeader {
    uint32_t biSize;
    int32_t  biWidth;
    int32_t  biHeight;
    uint16_t biPlanes;
    uint16_t biBitCount;
    uint32_t biCompression;
    uint32_t biSizeImage;
    int32_t  biXPelsPerMeter;
    int32_t  biYPelsPerMeter;
    uint32_t biClrUsed;
    uint32_t biClrImportant;
};
#pragma pack(pop)

static bool writeBmp32(const char* path, int width, int height, const std::vector<uint8_t>& bgra)
{
    FILE* f = nullptr;
#ifdef _MSC_VER
    fopen_s(&f, path, "wb");
#else
    f = fopen(path, "wb");
#endif
    if (!f) return false;

    const uint32_t imageSize = (uint32_t)bgra.size();

    BmpFileHeader fileHeader{};
    fileHeader.bfType = 0x4D42; // BM
    fileHeader.bfOffBits = sizeof(BmpFileHeader) + sizeof(BmpInfoHeader);
    fileHeader.bfSize = fileHeader.bfOffBits + imageSize;

    BmpInfoHeader infoHeader{};
    infoHeader.biSize = sizeof(BmpInfoHeader);
    infoHeader.biWidth = width;
    infoHeader.biHeight = -height; // top-down
    infoHeader.biPlanes = 1;
    infoHeader.biBitCount = 32;
    infoHeader.biCompression = 0; // BI_RGB
    infoHeader.biSizeImage = imageSize;

    fwrite(&fileHeader, sizeof(fileHeader), 1, f);
    fwrite(&infoHeader, sizeof(infoHeader), 1, f);
    fwrite(bgra.data(), 1, bgra.size(), f);
    fclose(f);
    return true;
}

static void cameraSaverThreadMain(EyeCameraCapture* capture)
{
    CreateDirectoryA("captures", nullptr);

    auto lastWrite = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    const char* eyeName = (capture->eye == 0) ? "left" : "right";

    while (capture->running.load()) {
        CapturedEyeFrame frame;
        {
            std::unique_lock<std::mutex> lock(capture->mutex);
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

        auto now = std::chrono::steady_clock::now();
        const bool timeToRefreshLatest =
            (now - lastWrite) >= std::chrono::milliseconds(500);

        if (!capture->firstFrameSaved || timeToRefreshLatest) {
            std::vector<uint8_t> bgra = nv12ToBgra(
                frame.nv12.data(), frame.width, frame.height, frame.rowStride);

            if (!capture->firstFrameSaved) {
                char firstPath[256];
                std::snprintf(firstPath, sizeof(firstPath),
                              "captures/%s_first_frame_%lld.bmp",
                              eyeName, (long long)frame.frameNumber);
                if (writeBmp32(firstPath, frame.width, frame.height, bgra)) {
                    printf("[CAM] saved %s\n", firstPath);
                    capture->firstFrameSaved = true;
                }
            }

            char latestPath[256];
            std::snprintf(latestPath, sizeof(latestPath),
                          "captures/%s_latest.bmp", eyeName);
            if (writeBmp32(latestPath, frame.width, frame.height, bgra)) {
                lastWrite = now;
            }
        }

        if (capture->bridge) {
            sendFrameToPython(*capture->bridge, frame, capture->eye);
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
    if (!frame || !userData) return;
    if (frame->type != varjo_StreamType_DistortedColor) return;
    if (!(frame->dataFlags & varjo_DataFlag_Buffer)) return;

    EyeCameraCaptureSet* set = reinterpret_cast<EyeCameraCaptureSet*>(userData);

    for (int eye = 0; eye < 2; ++eye) {
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

        varjo_LockDataStreamBuffer(session, bufferId);
        varjo_BufferMetadata meta = varjo_GetBufferMetadata(session, bufferId);

        if (meta.type == varjo_BufferType_CPU && meta.format == varjo_TextureFormat_NV12) {
            const uint8_t* src = reinterpret_cast<const uint8_t*>(
                varjo_GetBufferCPUData(session, bufferId));

            if (src && meta.byteSize > 0) {
                std::lock_guard<std::mutex> lock(capture->mutex);
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
        varjo_Error e = varjo_GetError(session);
        if (e != varjo_NoError)
            printf("[WARN] GazeInit: %s\n", varjo_GetErrorDesc(e));
        else
            printf("[OK] Gaze initialised\n");
    }

    PhospheneBridge phospheneBridge{};

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

    const float kSpotRadiusTan = 0.24f;
    const float kSoftEdgeTan   = 0.04f;
    const float kPhospheneRadiusTan = kSpotRadiusTan - kSoftEdgeTan;

    for (int e = 0; e < 2; ++e) {
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

    std::vector<varjo_Viewport> viewports = calculateViewports(
        session, varjo_TextureSize_Type_DynamicFoveation);
    // std::vector<varjo_Viewport> viewports = calculateViewports(
    // session, varjo_TextureSize_Type_Stereo);
    int32_t totalW = getTotalWidth(viewports);
    int32_t totalH = getTotalHeight(viewports);
    printf("[OK] Atlas %dx%d\n", totalW, totalH);

    varjo_SwapChainConfig2 cfg{};
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
    printf("[OK] Shaders\n");

    GLuint phospheneTexture[2] = {0, 0};
    glGenTextures(2, phospheneTexture);
    for (int e = 0; e < 2; ++e) {
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
        memset(&views[i], 0, sizeof(views[i]));
        const varjo_Viewport& vp = viewports[i];

        views[i].viewport = varjo_SwapChainViewport{
            swapchain, vp.x, vp.y, vp.width, vp.height, 0, 0};
        views[i].extension = nullptr;
    }

    varjo_LayerMultiProj projLayer{};
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

    printf("     Parallel left-camera capture writes captures/left_latest.bmp\n");
    printf("     Press ESC to quit.\n");

    int fc = 0;
    while (true) {
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) break;

        varjo_Event evt{};
        while (varjo_PollEvent(session, &evt)) {
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
            varjo_Gaze g = varjo_GetGaze(session);
            if (g.status == varjo_GazeStatus_Valid) { gaze = g; gazeValid = true; }
        }

        varjo_BeginFrameWithLayers(session);

        int sci = 0;
        varjo_AcquireSwapChainImage(swapchain, &sci);

        glBindFramebuffer(GL_FRAMEBUFFER, fbos[sci]);
        glViewport(0, 0, totalW, totalH);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        glBindVertexArray(vao);
        glDisable(GL_DEPTH_TEST);

        float gazeTanXPerEye[2] = {gazeTanX, gazeTanX};
        float gazeTanYPerEye[2] = {gazeTanY, gazeTanY};

       if (gazeValid) {
            float gazeTanXPerEye[2] = {gazeTanX, gazeTanX};
            float gazeTanYPerEye[2] = {gazeTanY, gazeTanY};

            const float kMinGz = 0.35f;
            const float kMaxTan = 1.5f;

            auto safeTan = [&](double fx, double fy, double fz, float& outX, float& outY) -> bool {
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
            glUseProgram(blackSpotProgram);
            glUniform1f(locGazeTanX,       eyeGazeTanX);
            glUniform1f(locGazeTanY,       eyeGazeTanY);
            glUniform1f(locViewLeft,       (float)tangents.left);
            glUniform1f(locViewRight,      (float)tangents.right);
            glUniform1f(locViewTop,        (float)tangents.top);
            glUniform1f(locViewBottom,     (float)tangents.bottom);
            glUniform1f(locSpotRadiusTan,  kSpotRadiusTan);
            glUniform1f(locSoftEdgeTan,    kSoftEdgeTan);
            glViewport(vp.x, vp.y, vp.width, vp.height);
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Phosphene overlay -- draw on ALL views, not just i < 2
            glUseProgram(phospheneProgram);
            glUniform1f(locPGazeTanX,        eyeGazeTanX);
            glUniform1f(locPGazeTanY,        eyeGazeTanY);
            glUniform1f(locPViewLeft,        (float)tangents.left);
            glUniform1f(locPViewRight,       (float)tangents.right);
            glUniform1f(locPViewTop,         (float)tangents.top);
            glUniform1f(locPViewBottom,      (float)tangents.bottom);
            glUniform1f(locPPhospheneRadius, kPhospheneRadiusTan);
            glUniform1f(locPPhospheneOp,     1.0f);

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

        submitInfo.frameNumber = frameInfo->frameNumber;
        varjo_EndFrameWithLayers(session, &submitInfo);

        fc++;
    }

    printf("Shutting down...\n");

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