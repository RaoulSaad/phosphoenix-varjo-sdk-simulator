////////////////////////////////////////////////////////////////////////////////
// renderer.cpp — OverlayRenderer implementation.
//
// WGL context + GL loader, the two embedded GLSL programs (black scotoma mask
// and gaze-anchored phosphene overlay), the per-eye phosphene textures and the
// swapchain FBOs. The shaders work in "tangent space" rather than pixels: a
// point on the view is represented by tan(x angle) and tan(y angle), which
// lines up with Varjo's per-view FOV tangents even when the headset has
// multiple focus/context views.
////////////////////////////////////////////////////////////////////////////////

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <GL/gl.h>
#include "glext.h"
#include "wglext.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "renderer.h"

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
// Shaders: transparent background + black spot, and phosphene overlay
// ---------------------------------------------------------------------------

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

// 1 = the mask fully occludes the passthrough (normal). Lower values reveal the
// real world underneath, for checking that phosphenes land on their objects.
uniform float maskOpacity;

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

    float alpha = blindnessMask(tanPos, gazeTan) * maskOpacity;

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

static const char* g_passthroughFragSrc = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D texY;
uniform sampler2D texUV;
void main() {
    // BT.601 limited range NV12 -> RGB; matches cv::COLOR_YUV2BGR_NV12.
    float y = (texture(texY, vUV).r - 16.0 / 255.0) * 1.164;
    vec2 uv = texture(texUV, vUV).rg - 0.5;
    float r = y + 1.596 * uv.y;
    float g = y - 0.391 * uv.x - 0.813 * uv.y;
    float b = y + 2.018 * uv.x;
    fragColor = vec4(clamp(vec3(r, g, b), 0.0, 1.0), 1.0);
}
)";

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
// OverlayRenderer
// ---------------------------------------------------------------------------

bool OverlayRenderer::initGL()
{
    // OpenGL must be ready before creating Varjo GL swapchains.
    if (!createGLContext()) { fprintf(stderr, "GL context failed\n"); return false; }
    loadGLFunctions();
    printf("[OK] OpenGL: %s\n", (const char*)glGetString(GL_VERSION));
    return true;
}

bool OverlayRenderer::setupSwapchainFbos(const std::vector<unsigned int>& swapchainTextures,
                                         int atlasWidth, int atlasHeight)
{
    
    m_atlasWidth  = atlasWidth;
    m_atlasHeight = atlasHeight;
    if (swapchainTextures.empty()) {
        // Windowed mode: the display draws straight into the default framebuffer.
        m_defaultFramebuffer = true;
        m_fbos.clear();
        printf("[OK] default framebuffer %dx%d\n", atlasWidth, atlasHeight);
        return true;
    }
    m_defaultFramebuffer = false;

    m_fbos.resize(swapchainTextures.size());
    glGenFramebuffers((GLsizei)m_fbos.size(), m_fbos.data());
    for (size_t i = 0; i < swapchainTextures.size(); i++) {
        // Attach each Varjo swapchain texture to an FBO so normal GL rendering
        // can draw into it.
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swapchainTextures[i], 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "FBO %d incomplete: 0x%x\n", (int)i, status);
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    printf("[OK] FBOs\n");
    return true;
}

bool OverlayRenderer::initShaders()
{
    m_blackSpotProgram = createProgram(g_fragSrc);
    m_locGazeTanX      = glGetUniformLocation(m_blackSpotProgram, "gazeTanX");
    m_locGazeTanY      = glGetUniformLocation(m_blackSpotProgram, "gazeTanY");
    m_locViewLeft      = glGetUniformLocation(m_blackSpotProgram, "viewLeft");
    m_locViewRight     = glGetUniformLocation(m_blackSpotProgram, "viewRight");
    m_locViewTop       = glGetUniformLocation(m_blackSpotProgram, "viewTop");
    m_locViewBottom    = glGetUniformLocation(m_blackSpotProgram, "viewBottom");
    m_locSpotRadiusTan = glGetUniformLocation(m_blackSpotProgram, "spotRadiusTan");
    m_locSoftEdgeTan   = glGetUniformLocation(m_blackSpotProgram, "softEdgeTan");
    m_locBlindnessMode = glGetUniformLocation(m_blackSpotProgram, "blindnessMode");
    m_locMaskOpacity   = glGetUniformLocation(m_blackSpotProgram, "maskOpacity");

    m_phospheneProgram = createProgram(g_phospheneFragSrc);
    m_locPGazeTanX        = glGetUniformLocation(m_phospheneProgram, "gazeTanX");
    m_locPGazeTanY        = glGetUniformLocation(m_phospheneProgram, "gazeTanY");
    m_locPViewLeft        = glGetUniformLocation(m_phospheneProgram, "viewLeft");
    m_locPViewRight       = glGetUniformLocation(m_phospheneProgram, "viewRight");
    m_locPViewTop         = glGetUniformLocation(m_phospheneProgram, "viewTop");
    m_locPViewBottom      = glGetUniformLocation(m_phospheneProgram, "viewBottom");
    m_locPPhospheneRadius = glGetUniformLocation(m_phospheneProgram, "phospheneRadiusTan");
    m_locPPhospheneOp     = glGetUniformLocation(m_phospheneProgram, "phospheneOpacity");
    m_locPPhospheneTex    = glGetUniformLocation(m_phospheneProgram, "phospheneTex");
    m_locPSpotRadiusTan   = glGetUniformLocation(m_phospheneProgram, "spotRadiusTan");
    m_locPSoftEdgeTan     = glGetUniformLocation(m_phospheneProgram, "softEdgeTan");
    m_locPBlindnessMode   = glGetUniformLocation(m_phospheneProgram, "blindnessMode");

    m_passthroughProgram = createProgram(g_passthroughFragSrc);
    m_locPtTexY  = glGetUniformLocation(m_passthroughProgram, "texY");
    m_locPtTexUV = glGetUniformLocation(m_passthroughProgram, "texUV");
    glUseProgram(m_passthroughProgram);
    glUniform1i(m_locPtTexY, 0);
    glUniform1i(m_locPtTexUV, 1);
    glUseProgram(0);

    printf("[OK] Shaders\n");

    // The phosphene sampler always reads texture unit 0.
    glUseProgram(m_phospheneProgram);
    glUniform1i(m_locPPhospheneTex, 0);
    glUseProgram(0);

    glGenVertexArrays(1, &m_vao);
    return true;
}

void OverlayRenderer::initPhospheneTextures(const int cropWidth[NUM_EYES], const int cropHeight[NUM_EYES])
{
    glGenTextures(NUM_EYES, m_phospheneTexture);
    for (int e = 0; e < NUM_EYES; ++e) {
        // One single-channel texture per eye. Python returns grayscale bytes,
        // so GL_R8 is enough and the shader expands it to RGBA.
        glBindTexture(GL_TEXTURE_2D, m_phospheneTexture[e]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // Upload zeros: glTexImage2D with a null pointer leaves whatever was
        // in that VRAM before, which the shader would happily draw as phosphenes.
        std::vector<uint8_t> zeros((size_t)cropWidth[e] * (size_t)cropHeight[e], 0);
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            cropWidth[e],
            cropHeight[e],
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            zeros.data());
        m_texWidth[e]  = cropWidth[e];
        m_texHeight[e] = cropHeight[e];
    }
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OverlayRenderer::uploadPhosphene(int eye, const uint8_t* data, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, m_phospheneTexture[eye]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    if (width != m_texWidth[eye] || height != m_texHeight[eye]) {
        // Reallocate only when Python changes crop size. Normal frames only need
        // the cheaper sub-image upload below.
        glTexImage2D(
            GL_TEXTURE_2D,
            0,
            GL_R8,
            (GLsizei)width,
            (GLsizei)height,
            0,
            GL_RED,
            GL_UNSIGNED_BYTE,
            nullptr);
        m_texWidth[eye]  = width;
        m_texHeight[eye] = height;
    }
    glTexSubImage2D(
        GL_TEXTURE_2D,
        0,
        0,
        0,
        (GLsizei)width,
        (GLsizei)height,
        GL_RED,
        GL_UNSIGNED_BYTE,
        data);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OverlayRenderer::initPassthroughTextures(int width, int height)
{
    m_passthroughWidth = width; m_passthroughHeight = height;
    GLuint tex[2]; glGenTextures(2, tex);
    m_passthroughTexY = tex[0]; m_passthroughTexUV = tex[1];
    for (int i = 0; i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    glBindTexture(GL_TEXTURE_2D, m_passthroughTexY);
    std::vector<uint8_t> zeros((size_t)width * (size_t)height, 0);   // black frame until the first upload
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, zeros.data());
    glBindTexture(GL_TEXTURE_2D, m_passthroughTexUV);
    std::fill(zeros.begin(), zeros.end(), (uint8_t)128);                 // neutral chroma
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, width / 2, height / 2, 0, GL_RG, GL_UNSIGNED_BYTE, zeros.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OverlayRenderer::uploadPassthrough(const uint8_t* nv12, int width, int height, int rowStride)
{
    if (!m_passthroughTexY || width != m_passthroughWidth || height != m_passthroughHeight) return;
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowStride);
    glBindTexture(GL_TEXTURE_2D, m_passthroughTexY);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, nv12);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, rowStride / 2);   // RG8 texels: stride in texels is half
    glBindTexture(GL_TEXTURE_2D, m_passthroughTexUV);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE,
                    nv12 + (size_t)rowStride * (size_t)height);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OverlayRenderer::drawPassthrough(const Viewport& vp)
{
    if (!m_passthroughTexY) return;
    glDisable(GL_BLEND);                       // opaque background
    glUseProgram(m_passthroughProgram);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_passthroughTexY);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_passthroughTexUV);
    glViewport(vp.x, vp.y, vp.width, vp.height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, 0);
    glEnable(GL_BLEND);                        // beginFrame's blend state for the overlay passes
}


void OverlayRenderer::beginFrame(int swapchainImageIndex)
{
    // Render into the acquired Varjo swapchain image.
    if (m_defaultFramebuffer || swapchainImageIndex < 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, m_atlasWidth, m_atlasHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);   // opaque: there is no compositor behind us
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbos[swapchainImageIndex]);
        glViewport(0, 0, m_atlasWidth, m_atlasHeight);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
    glClear(GL_COLOR_BUFFER_BIT);

    glEnable(GL_BLEND);
    // Source colors are effectively premultiplied by their alpha in the
    // shaders. This blend mode composites the overlay onto transparent black.
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    glBindVertexArray(m_vao);
    glDisable(GL_DEPTH_TEST);
}

void OverlayRenderer::drawView(const Viewport& vp, const ViewTangents& t,
                               float gazeTanX, float gazeTanY, int eyeIdx,
                               BlindnessMode mode, const OverlayGeometry& geom)
{
    // Black spot
    // Draw first so the phosphene overlay can appear inside/on top of
    // the scotoma region. Skipped entirely at zero opacity, which is the
    // "show me the real world" case and saves a fullscreen pass.
    if (m_maskOpacity > 0.0f) {
        glUseProgram(m_blackSpotProgram);
        glUniform1f(m_locGazeTanX,       gazeTanX);
        glUniform1f(m_locGazeTanY,       gazeTanY);
        glUniform1f(m_locViewLeft,       t.left);
        glUniform1f(m_locViewRight,      t.right);
        glUniform1f(m_locViewTop,        t.top);
        glUniform1f(m_locViewBottom,     t.bottom);
        glUniform1f(m_locSpotRadiusTan,  geom.spotRadiusTan);
        glUniform1f(m_locSoftEdgeTan,    geom.softEdgeTan);
        glUniform1i(m_locBlindnessMode,  (int)mode);
        glUniform1f(m_locMaskOpacity,    m_maskOpacity);
        glViewport(vp.x, vp.y, vp.width, vp.height);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // Phosphene overlay -- draw on ALL views, not just i < 2
    // The shader samples the latest Python-returned grayscale mask for
    // this eye and positions it at the same gaze tangent coordinate.
    glUseProgram(m_phospheneProgram);
    glUniform1f(m_locPGazeTanX,        gazeTanX);
    glUniform1f(m_locPGazeTanY,        gazeTanY);
    glUniform1f(m_locPViewLeft,        t.left);
    glUniform1f(m_locPViewRight,       t.right);
    glUniform1f(m_locPViewTop,         t.top);
    glUniform1f(m_locPViewBottom,      t.bottom);
    glUniform1f(m_locPPhospheneRadius, geom.phospheneRadiusTan);
    glUniform1f(m_locPPhospheneOp,     m_phospheneOpacity);
    glUniform1f(m_locPSpotRadiusTan,   geom.spotRadiusTan);
    glUniform1f(m_locPSoftEdgeTan,     geom.softEdgeTan);
    glUniform1i(m_locPBlindnessMode,   (int)mode);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_phospheneTexture[eyeIdx]);
    glViewport(vp.x, vp.y, vp.width, vp.height);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void OverlayRenderer::endFrame()
{
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void OverlayRenderer::setMaskOpacity(float opacity)
{
    m_maskOpacity = (opacity < 0.0f) ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
}

void OverlayRenderer::setPhospheneOpacity(float opacity)
{
    m_phospheneOpacity = (opacity < 0.0f) ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
}

void OverlayRenderer::shutdownGL()
{
    glDeleteVertexArrays(1, &m_vao);
    glDeleteProgram(m_blackSpotProgram);
    glDeleteProgram(m_phospheneProgram);
    glDeleteTextures(NUM_EYES, m_phospheneTexture);
    if (!m_fbos.empty()) glDeleteFramebuffers((GLsizei)m_fbos.size(), m_fbos.data());
    if (m_passthroughProgram) glDeleteProgram(m_passthroughProgram);
    if (m_passthroughTexY) { GLuint t[2] = {m_passthroughTexY, m_passthroughTexUV}; glDeleteTextures(2, t); }
}

void OverlayRenderer::destroyContext()
{
    destroyGLContext();
}

void* OverlayRenderer::nativeWindow() const { return (void*)g_hwnd; }

