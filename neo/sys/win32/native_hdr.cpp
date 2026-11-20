// Engine-owned FP16 scene and scRGB presenter; no ReShade/addon dependency.
// PsychoV35 GLSL is adapted from the user's RenoDX Doom 3 mod (MIT).
// Keep the newer SDK requirement local to the optional Windows HDR presenter.
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef WINVER
#define WINVER 0x0A00
#undef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>

#include "sys/platform.h"
#include "renderer/tr_local.h"
#include "renderer/hdr_tonemap.h"

using Microsoft::WRL::ComPtr;

static idCVar r_hdr("r_hdr", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_BOOL,
    "Native Windows scRGB HDR (Windows HDR must be enabled); requires vid_restart");
static idCVar r_hdrPeakNits("r_hdrPeakNits", "1000", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "HDR display peak brightness in nits", 80, 10000);
static idCVar r_hdrGameNits("r_hdrGameNits", "203", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "Scene reference white in nits", 48, 500);
static idCVar r_hdrUINits("r_hdrUINits", "203", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "HUD and menu white in nits", 48, 500);
static idCVar r_hdrExposure("r_hdrExposure", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 scene exposure multiplier", 0.001f, 64);
static idCVar r_hdrContrast("r_hdrContrast", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 contrast", 0.01f, 4);
static idCVar r_hdrSaturation("r_hdrSaturation", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "Scene saturation", 0, 2);
static idCVar r_hdrCompression("r_hdrCompression", "0", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 compression; zero is automatic", 0, 4);
static idCVar r_hdrConeResponse("r_hdrConeResponse", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 cone response", 0.01f, 4);
static idCVar r_hdrPurity("r_hdrPurity", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 purity scale", 0, 4);
static idCVar r_hdrGamutCompression("r_hdrGamutCompression", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_FLOAT,
    "PsychoV35 gamut projection strength", 0, 1);
static idCVar r_hdrGamut("r_hdrGamut", "1", CVAR_RENDERER | CVAR_ARCHIVE | CVAR_INTEGER,
    "PsychoV35 target gamut: 0 BT.709, 1 BT.2020", 0, 1);
static idCVar r_hdrDebug("r_hdrDebug", "0", CVAR_RENDERER | CVAR_BOOL,
    "Read back one HDR scene/output frame and log range and GL errors (diagnostic, slow)");

namespace {

// All objects live on the render/context thread and die before SDL deletes GL.
struct NativeHDR {
    PFNGLGENFRAMEBUFFERSPROC genFramebuffers;
    PFNGLDELETEFRAMEBUFFERSPROC deleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC bindFramebuffer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC framebufferTexture2D;
    PFNGLCHECKFRAMEBUFFERSTATUSPROC checkFramebufferStatus;
    PFNGLGENRENDERBUFFERSPROC genRenderbuffers;
    PFNGLDELETERENDERBUFFERSPROC deleteRenderbuffers;
    PFNGLBINDRENDERBUFFERPROC bindRenderbuffer;
    PFNGLRENDERBUFFERSTORAGEPROC renderbufferStorage;
    PFNGLFRAMEBUFFERRENDERBUFFERPROC framebufferRenderbuffer;
    PFNGLCREATESHADERPROC createShader;
    PFNGLSHADERSOURCEPROC shaderSource;
    PFNGLCOMPILESHADERPROC compileShader;
    PFNGLGETSHADERIVPROC getShaderiv;
    PFNGLGETSHADERINFOLOGPROC getShaderInfoLog;
    PFNGLDELETESHADERPROC deleteShader;
    PFNGLCREATEPROGRAMPROC createProgram;
    PFNGLATTACHSHADERPROC attachShader;
    PFNGLLINKPROGRAMPROC linkProgram;
    PFNGLGETPROGRAMIVPROC getProgramiv;
    PFNGLGETPROGRAMINFOLOGPROC getProgramInfoLog;
    PFNGLDELETEPROGRAMPROC deleteProgram;
    PFNGLUSEPROGRAMPROC useProgram;
    PFNGLGETUNIFORMLOCATIONPROC getUniformLocation;
    PFNGLUNIFORM4FPROC uniform4f;
    PFNGLUNIFORM1FPROC uniform1f;
    PFNGLUNIFORM1IPROC uniform1i;
    PFNGLCLAMPCOLORPROC clampColor;
    HANDLE (WINAPI *openDevice)(void*);
    BOOL (WINAPI *closeDevice)(HANDLE);
    HANDLE (WINAPI *registerObject)(HANDLE, void*, GLuint, GLenum, GLenum);
    BOOL (WINAPI *unregisterObject)(HANDLE, HANDLE);
    BOOL (WINAPI *lockObjects)(HANDLE, GLint, HANDLE*);
    BOOL (WINAPI *unlockObjects)(HANDLE, GLint, HANDLE*);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain3> swapchain;
    ComPtr<ID3D11Texture2D> sharedTexture;
    HANDLE interopDevice = NULL;
    HANDLE interopObject = NULL;
    GLuint scene = 0, scratch = 0, shared = 0;
    GLuint sceneFBO = 0, sharedFBO = 0, depth = 0;
    GLuint tonemap = 0, output = 0;
    int width = 0, height = 0;
    bool attempted = false, active = false, frame = false;
    bool sceneSeen = false, mapped = false, guiStarted = false;
    bool loggedScene = false;
};

NativeHDR hdr;

const char* OUTPUT_SHADER = R"(#version 130
uniform sampler2D scene;
uniform float uiNits;
void main() {
    ivec2 size = textureSize(scene, 0);
    // D3D rows run top to bottom; GL scene textures run bottom to top.
    ivec2 p = ivec2(int(gl_FragCoord.x), size.y - 1 - int(gl_FragCoord.y));
    vec3 encoded = texelFetch(scene, p, 0).rgb;
    vec3 linear = sign(encoded) * pow(abs(encoded), vec3(2.2));
    for (int c = 0; c < 3; ++c) {
        if (isnan(linear[c]) || isinf(linear[c])) linear[c] = 0.0;
    }
    // scRGB 1.0 is 80 nits. UI-relative scene scaling was done before the HUD.
    gl_FragColor = vec4(clamp(linear * (uiNits / 80.0), -65504.0, 65504.0), 1.0);
}
)";

GLuint CompileProgram(const char* const* fragment, int count) {
    const GLenum types[] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
    GLuint shaders[2] = {};
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        shaders[i] = hdr.createShader(types[i]);
        if (i == 0) {
            hdr.shaderSource(shaders[i], 1, &native_hdr_shader::TONE_MAP_VERTEX_SHADER, NULL);
        } else {
            hdr.shaderSource(shaders[i], count, fragment, NULL);
        }
        hdr.compileShader(shaders[i]);
        GLint compiled = 0;
        hdr.getShaderiv(shaders[i], GL_COMPILE_STATUS, &compiled);
        if (!compiled) {
            char info[4096] = {};
            hdr.getShaderInfoLog(shaders[i], sizeof(info), NULL, info);
            common->Warning("Native HDR shader compile failed: %s", info);
            ok = false;
        }
    }
    GLuint program = 0;
    if (ok) {
        program = hdr.createProgram();
        hdr.attachShader(program, shaders[0]);
        hdr.attachShader(program, shaders[1]);
        hdr.linkProgram(program);
        GLint linked = 0;
        hdr.getProgramiv(program, GL_LINK_STATUS, &linked);
        if (!linked) {
            char info[4096] = {};
            hdr.getProgramInfoLog(program, sizeof(info), NULL, info);
            common->Warning("Native HDR program link failed: %s", info);
            hdr.deleteProgram(program);
            program = 0;
        }
    }
    for (int i = 0; i < 2; ++i) hdr.deleteShader(shaders[i]);
    return program;
}

// Full-screen draws must restore both actual GL state and the engine's cached
// state. PushAttrib handles fixed-function state; GLSL/FBO bindings are explicit.
void DrawFullscreen(GLuint program, GLuint source, GLuint destination) {
    GLint oldProgram = 0, oldActive = 0, oldTexture = 0, oldFBO = 0;
    qglGetIntegerv(GL_CURRENT_PROGRAM, &oldProgram);
    qglGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    qglGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFBO);
    qglPushAttrib(GL_ALL_ATTRIB_BITS);
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    qglBindTexture(GL_TEXTURE_2D, source);
    hdr.bindFramebuffer(GL_FRAMEBUFFER, destination);
    qglDrawBuffer(GL_COLOR_ATTACHMENT0);
    qglViewport(0, 0, hdr.width, hdr.height);
    qglDisable(GL_BLEND);
    qglDisable(GL_ALPHA_TEST);
    qglDisable(GL_DEPTH_TEST);
    qglDisable(GL_STENCIL_TEST);
    qglDisable(GL_SCISSOR_TEST);
    qglDisable(GL_CULL_FACE);
    qglDisable(GL_COLOR_LOGIC_OP);
    qglDisable(GL_FRAMEBUFFER_SRGB);
    qglDisable(GL_VERTEX_PROGRAM_ARB);
    qglDisable(GL_FRAGMENT_PROGRAM_ARB);
    qglDisable(GL_CLIP_PLANE0);
    hdr.clampColor(GL_CLAMP_FRAGMENT_COLOR, GL_FALSE);
    qglPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    hdr.useProgram(program);
    hdr.uniform1i(hdr.getUniformLocation(program, "scene"), 0);
    if (program == hdr.tonemap) {
        const float game = r_hdrGameNits.GetFloat();
        const float peak = (r_hdrPeakNits.GetFloat() > game ? r_hdrPeakNits.GetFloat() : game);
        hdr.uniform4f(hdr.getUniformLocation(program, "tone"), peak / game, r_hdrExposure.GetFloat(), 1, 1);
        hdr.uniform4f(hdr.getUniformLocation(program, "grade"), r_hdrContrast.GetFloat(), 0, r_hdrSaturation.GetFloat(), 0);
        hdr.uniform4f(hdr.getUniformLocation(program, "extra"), 0, 0, game / r_hdrUINits.GetFloat(), 0);
        hdr.uniform4f(hdr.getUniformLocation(program, "psycho"), r_hdrCompression.GetFloat(),
            r_hdrGamutCompression.GetFloat(), r_hdrConeResponse.GetFloat(), r_hdrPurity.GetFloat());
        hdr.uniform4f(hdr.getUniformLocation(program, "modes"), 1, r_hdrGamut.GetInteger(), 0, 0);
    } else {
        hdr.uniform1f(hdr.getUniformLocation(program, "uiNits"), r_hdrUINits.GetFloat());
    }
    qglRectf(-1, -1, 1, 1);
    hdr.useProgram(oldProgram);
    qglBindTexture(GL_TEXTURE_2D, oldTexture);
    qglActiveTextureARB(oldActive);
    hdr.bindFramebuffer(GL_FRAMEBUFFER, oldFBO);
    qglPopAttrib();
}

void LogRange(GLuint texture, const char* label) {
    GLint active = 0, oldTexture = 0;
    qglGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    qglBindTexture(GL_TEXTURE_2D, texture);
    std::vector<float> pixels(size_t(hdr.width) * hdr.height * 4);
    qglGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data());
    float peak = 0, targetPeak = 0;
    size_t invalid = 0, aboveWhite = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            if (pixels[i+c] > peak) peak = pixels[i+c];
            if (pixels[i+c] > 1) ++aboveWhite;
            if (!_finite(pixels[i+c])) ++invalid;
        }
        if (texture == hdr.shared) {
            const float target[3] = {
                0.627403896f * pixels[i] + 0.329283038f * pixels[i+1] + 0.043313066f * pixels[i+2],
                0.069097289f * pixels[i] + 0.919540395f * pixels[i+1] + 0.011362316f * pixels[i+2],
                0.016391439f * pixels[i] + 0.088013308f * pixels[i+1] + 0.895595253f * pixels[i+2]
            };
            for (int c = 0; c < 3; ++c) if (target[c] > targetPeak) targetPeak = target[c];
        }
    }
    common->Printf("Native HDR diagnostic: %s peak %.6f, %llu channels above 1, %llu invalid; GL error 0x%x\n",
        label, peak, static_cast<unsigned long long>(aboveWhite),
        static_cast<unsigned long long>(invalid), qglGetError());
    if (texture == hdr.shared) common->Printf("Native HDR diagnostic: BT.2020 target peak %.2f nits (configured %.2f)\n",
        targetPeak * 80, r_hdrPeakNits.GetFloat());
    fflush(stdout);
    qglBindTexture(GL_TEXTURE_2D, oldTexture);
    qglActiveTextureARB(active);
}

void MapScene() {
    if (hdr.mapped || !hdr.sceneSeen) return;
    GLint oldActive = 0, oldTexture = 0;
    qglGetIntegerv(GL_ACTIVE_TEXTURE, &oldActive);
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglGetIntegerv(GL_TEXTURE_BINDING_2D, &oldTexture);
    qglBindTexture(GL_TEXTURE_2D, hdr.scratch);
    qglReadBuffer(GL_COLOR_ATTACHMENT0);
    qglCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, hdr.width, hdr.height);
    if (r_hdrDebug.GetBool()) LogRange(hdr.scratch, "PRE-tonemap gamma scene");
    qglBindTexture(GL_TEXTURE_2D, oldTexture);
    qglActiveTextureARB(oldActive);
    DrawFullscreen(hdr.tonemap, hdr.scratch, hdr.sceneFBO);
    if (r_hdrDebug.GetBool()) LogRange(hdr.scene, "POST-tonemap UI-relative gamma");
    hdr.mapped = true;
    if (!hdr.loggedScene) {
        common->Printf("Native HDR: PsychoV35 mapped the FP16 scene before screen-space GUI.\n");
        hdr.loggedScene = true;
    }
}

bool CreateTargets(int width, int height) {
    GLint active = 0, texture = 0;
    qglGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    qglActiveTextureARB(GL_TEXTURE0_ARB);
    qglGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    qglGenTextures(1, &hdr.scene);
    qglGenTextures(1, &hdr.scratch);
    const GLuint textures[] = {hdr.scene, hdr.scratch};
    for (int i = 0; i < 2; ++i) {
        qglBindTexture(GL_TEXTURE_2D, textures[i]);
        qglTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, NULL);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        qglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    qglBindTexture(GL_TEXTURE_2D, texture);
    qglActiveTextureARB(active);
    hdr.genRenderbuffers(1, &hdr.depth);
    hdr.bindRenderbuffer(GL_RENDERBUFFER, hdr.depth);
    hdr.renderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    hdr.bindRenderbuffer(GL_RENDERBUFFER, 0);
    hdr.genFramebuffers(1, &hdr.sceneFBO);
    hdr.bindFramebuffer(GL_FRAMEBUFFER, hdr.sceneFBO);
    hdr.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdr.scene, 0);
    hdr.framebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, hdr.depth);
    qglDrawBuffer(GL_COLOR_ATTACHMENT0);
    qglReadBuffer(GL_COLOR_ATTACHMENT0);
    if (hdr.checkFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        common->Warning("Native HDR: FP16 scene framebuffer is incomplete");
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = hdr.device->CreateTexture2D(&desc, NULL, hdr.sharedTexture.GetAddressOf());
    if (FAILED(hr)) {
        common->Warning("Native HDR: shared D3D texture failed (0x%08x)", unsigned(hr));
        return false;
    }
    qglGenTextures(1, &hdr.shared);
    hdr.interopObject = hdr.registerObject(hdr.interopDevice, hdr.sharedTexture.Get(),
        hdr.shared, GL_TEXTURE_2D, 0x0002 /* WGL_ACCESS_WRITE_DISCARD_NV */);
    if (!hdr.interopObject || !hdr.lockObjects(hdr.interopDevice, 1, &hdr.interopObject)) {
        common->Warning("Native HDR: GL/D3D texture registration or lock failed (%lu)", GetLastError());
        return false;
    }
    hdr.genFramebuffers(1, &hdr.sharedFBO);
    hdr.bindFramebuffer(GL_FRAMEBUFFER, hdr.sharedFBO);
    hdr.framebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, hdr.shared, 0);
    qglDrawBuffer(GL_COLOR_ATTACHMENT0);
    const bool complete = hdr.checkFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    hdr.bindFramebuffer(GL_FRAMEBUFFER, 0);
    hdr.unlockObjects(hdr.interopDevice, 1, &hdr.interopObject);
    if (!complete) common->Warning("Native HDR: interop framebuffer is incomplete");
    hdr.width = width;
    hdr.height = height;
    return complete;
}

void DeleteTargets() {
    if (hdr.bindFramebuffer) hdr.bindFramebuffer(GL_FRAMEBUFFER, 0);
    if (hdr.sceneFBO) hdr.deleteFramebuffers(1, &hdr.sceneFBO);
    if (hdr.sharedFBO) hdr.deleteFramebuffers(1, &hdr.sharedFBO);
    if (hdr.depth) hdr.deleteRenderbuffers(1, &hdr.depth);
    if (hdr.interopObject) hdr.unregisterObject(hdr.interopDevice, hdr.interopObject);
    hdr.interopObject = NULL;
    if (hdr.scene) qglDeleteTextures(1, &hdr.scene);
    if (hdr.scratch) qglDeleteTextures(1, &hdr.scratch);
    if (hdr.shared) qglDeleteTextures(1, &hdr.shared);
    hdr.sceneFBO = hdr.sharedFBO = hdr.depth = hdr.scene = hdr.scratch = hdr.shared = 0;
    hdr.sharedTexture.Reset();
}

bool Initialize() {
    // The scene FBO is deliberately single-sampled. Refuse an incompatible
    // configuration instead of silently dropping the user's requested MSAA.
    GLint samples = 0;
    qglGetIntegerv(GL_SAMPLES, &samples);
    if (samples > 1) {
        common->Warning("Native HDR needs r_multiSamples 0 and vid_restart; using SDR");
        return false;
    }
    if (glConfig.glVersion < 3.3f) {
        common->Warning("Native HDR requires OpenGL 3.3 or newer; using SDR");
        return false;
    }
    if (!r_gammaInShader.GetBool()) {
        common->Warning("Native HDR needs r_gammaInShader 1 and vid_restart; using SDR");
        return false;
    }
#define LOAD_GL(member, name) \
    hdr.member = reinterpret_cast<decltype(hdr.member)>(GLimp_ExtensionPointer(name)); \
    if (!hdr.member) { common->Warning("Native HDR missing %s; using SDR", name); return false; }
    LOAD_GL(genFramebuffers, "glGenFramebuffers");
    LOAD_GL(deleteFramebuffers, "glDeleteFramebuffers");
    LOAD_GL(bindFramebuffer, "glBindFramebuffer");
    LOAD_GL(framebufferTexture2D, "glFramebufferTexture2D");
    LOAD_GL(checkFramebufferStatus, "glCheckFramebufferStatus");
    LOAD_GL(genRenderbuffers, "glGenRenderbuffers");
    LOAD_GL(deleteRenderbuffers, "glDeleteRenderbuffers");
    LOAD_GL(bindRenderbuffer, "glBindRenderbuffer");
    LOAD_GL(renderbufferStorage, "glRenderbufferStorage");
    LOAD_GL(framebufferRenderbuffer, "glFramebufferRenderbuffer");
    LOAD_GL(createShader, "glCreateShader");
    LOAD_GL(shaderSource, "glShaderSource");
    LOAD_GL(compileShader, "glCompileShader");
    LOAD_GL(getShaderiv, "glGetShaderiv");
    LOAD_GL(getShaderInfoLog, "glGetShaderInfoLog");
    LOAD_GL(deleteShader, "glDeleteShader");
    LOAD_GL(createProgram, "glCreateProgram");
    LOAD_GL(attachShader, "glAttachShader");
    LOAD_GL(linkProgram, "glLinkProgram");
    LOAD_GL(getProgramiv, "glGetProgramiv");
    LOAD_GL(getProgramInfoLog, "glGetProgramInfoLog");
    LOAD_GL(deleteProgram, "glDeleteProgram");
    LOAD_GL(useProgram, "glUseProgram");
    LOAD_GL(getUniformLocation, "glGetUniformLocation");
    LOAD_GL(uniform4f, "glUniform4f");
    LOAD_GL(uniform1f, "glUniform1f");
    LOAD_GL(uniform1i, "glUniform1i");
    LOAD_GL(clampColor, "glClampColor");
    LOAD_GL(openDevice, "wglDXOpenDeviceNV");
    LOAD_GL(closeDevice, "wglDXCloseDeviceNV");
    LOAD_GL(registerObject, "wglDXRegisterObjectNV");
    LOAD_GL(unregisterObject, "wglDXUnregisterObjectNV");
    LOAD_GL(lockObjects, "wglDXLockObjectsNV");
    LOAD_GL(unlockObjects, "wglDXUnlockObjectsNV");
#undef LOAD_GL
    const auto getCurrentDC = reinterpret_cast<HDC (WINAPI*)()>(GLimp_ExtensionPointer("wglGetCurrentDC"));
    HWND window = getCurrentDC ? WindowFromDC(getCurrentDC()) : NULL;
    if (!window) return false;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 1, D3D11_SDK_VERSION, hdr.device.GetAddressOf(), NULL, hdr.context.GetAddressOf());
    if (FAILED(hr)) {
        common->Warning("Native HDR: D3D11 device failed (0x%08x)", unsigned(hr));
        return false;
    }
    hdr.interopDevice = hdr.openDevice(hdr.device.Get());
    if (!hdr.interopDevice) {
        common->Warning("Native HDR: WGL_NV_DX_interop2 device failed (%lu)", GetLastError());
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIFactory2> factory;
    hr = hdr.device.As(&dxgiDevice);
    if (FAILED(hr)) return false;
    hr = dxgiDevice->GetAdapter(adapter.GetAddressOf());
    if (FAILED(hr)) return false;
    DXGI_ADAPTER_DESC adapterDesc = {};
    adapter->GetDesc(&adapterDesc);
    common->Printf("Native HDR: D3D11 adapter %ls; GL renderer %s\n", adapterDesc.Description, glConfig.renderer_string);
    hr = adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) return false;
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = glConfig.vidWidth;
    desc.Height = glConfig.vidHeight;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapchain;
    hr = factory->CreateSwapChainForHwnd(hdr.device.Get(), window, &desc, NULL, NULL, swapchain.GetAddressOf());
    if (FAILED(hr)) {
        common->Warning("Native HDR: DXGI flip swapchain failed (0x%08x)", unsigned(hr));
        return false;
    }
    factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
    hr = swapchain.As(&hdr.swapchain);
    if (FAILED(hr)) return false;
    UINT support = 0;
    const DXGI_COLOR_SPACE_TYPE space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    hr = hdr.swapchain->CheckColorSpaceSupport(space, &support);
    if (FAILED(hr) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
        || FAILED(hdr.swapchain->SetColorSpace1(space))) {
        common->Warning("Native HDR: scRGB presentation unavailable; using SDR");
        return false;
    }
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput6> output6;
    if (SUCCEEDED(hdr.swapchain->GetContainingOutput(output.GetAddressOf())) && SUCCEEDED(output.As(&output6))) {
        DXGI_OUTPUT_DESC1 outputDesc = {};
        if (SUCCEEDED(output6->GetDesc1(&outputDesc))) {
            common->Printf("Native HDR: desktop color space %d; display peak %.0f nits\n",
                int(outputDesc.ColorSpace), outputDesc.MaxLuminance);
            if (outputDesc.ColorSpace != DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
                common->Warning("Native HDR: Windows HDR is off on this display; enable it and vid_restart; using SDR");
                return false;
            }
        }
    }
    hdr.tonemap = CompileProgram(native_hdr_shader::TONE_MAP_FRAGMENT_SHADER,
        sizeof(native_hdr_shader::TONE_MAP_FRAGMENT_SHADER) / sizeof(const char*));
    hdr.output = CompileProgram(&OUTPUT_SHADER, 1);
    if (!hdr.tonemap || !hdr.output) return false;
    if (!CreateTargets(glConfig.vidWidth, glConfig.vidHeight)) return false;
    hdr.active = true;
    common->Printf("Native HDR ACTIVE: RGBA16F scene + D24S8, PsychoV35, DXGI FP16 scRGB (80 nits/unit), %dx%d.\n",
        hdr.width, hdr.height);
    return true;
}

} // namespace

void HDR_DebugStage(const char* label) {
    if (HDR_Active() && hdr.sceneSeen && r_hdrDebug.GetBool()) LogRange(hdr.scene, label);
}

bool HDR_Requested() { return r_hdr.GetBool(); }
bool HDR_Active() { return hdr.active && hdr.frame; }

void HDR_BeginFrame() {
    if (!r_hdr.GetBool()) {
        if (hdr.active) HDR_Shutdown();
        return;
    }
    if (!hdr.attempted) {
        hdr.attempted = true;
        if (!Initialize()) {
            HDR_Shutdown();
            hdr.attempted = true;
            return;
        }
    }
    if (!hdr.active) return;
    if (hdr.width != glConfig.vidWidth || hdr.height != glConfig.vidHeight) {
        DeleteTargets();
        hdr.context->Flush();
        HRESULT hr = hdr.swapchain->ResizeBuffers(0, glConfig.vidWidth, glConfig.vidHeight, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr) || !CreateTargets(glConfig.vidWidth, glConfig.vidHeight)) {
            common->Warning("Native HDR resize failed; using SDR until vid_restart");
            HDR_Shutdown();
            hdr.attempted = true;
            return;
        }
    }
    hdr.frame = true;
    hdr.sceneSeen = hdr.mapped = hdr.guiStarted = false;
    hdr.bindFramebuffer(GL_FRAMEBUFFER, hdr.sceneFBO);
    qglDrawBuffer(GL_COLOR_ATTACHMENT0);
    qglReadBuffer(GL_COLOR_ATTACHMENT0);
    // A fresh full-frame clear also initializes bars outside a reduced viewport.
    qglPushAttrib(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT | GL_SCISSOR_BIT);
    qglDisable(GL_SCISSOR_TEST);
    qglColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    qglDepthMask(GL_TRUE);
    qglStencilMask(~0u);
    qglClearColor(0, 0, 0, 1);
    qglClearDepth(1);
    qglClearStencil(128);
    qglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    qglPopAttrib();
}

void HDR_BeforeView(bool scene, bool subview) {
    if (!HDR_Active() || subview) return;
    if (scene && !hdr.guiStarted) {
        hdr.sceneSeen = true;
    } else if (!scene) {
        MapScene();
        hdr.guiStarted = true;
    }
}

bool HDR_Present() {
    if (!HDR_Active()) return false;
    MapScene(); // Covers frames with no HUD draw; UI-only frames skip mapping.
    if (!hdr.lockObjects(hdr.interopDevice, 1, &hdr.interopObject)) {
        common->Warning("Native HDR: interop lock failed (%lu); reverting to SDR", GetLastError());
        HDR_Shutdown();
        hdr.attempted = true;
        return false;
    }
    if (r_hdrDebug.GetBool() && hdr.sceneSeen) LogRange(hdr.scene, "POST-GUI gamma");
    DrawFullscreen(hdr.output, hdr.scene, hdr.sharedFBO);
    if (r_hdrDebug.GetBool() && hdr.sceneSeen) {
        LogRange(hdr.shared, "scRGB (multiply by 80 for channel nits)");
        r_hdrDebug.SetBool(false);
    }
    const BOOL unlocked = hdr.unlockObjects(hdr.interopDevice, 1, &hdr.interopObject);
    ComPtr<ID3D11Texture2D> backbuffer;
    HRESULT hr = hdr.swapchain->GetBuffer(0, IID_PPV_ARGS(backbuffer.GetAddressOf()));
    if (unlocked && SUCCEEDED(hr)) {
        hdr.context->CopyResource(backbuffer.Get(), hdr.sharedTexture.Get());
        hr = hdr.swapchain->Present(r_swapInterval.GetInteger() != 0 ? 1 : 0, 0);
    }
    if (!unlocked || FAILED(hr)) {
        common->Warning("Native HDR: presentation failed (0x%08x); reverting to SDR", unsigned(hr));
        HDR_Shutdown();
        hdr.attempted = true;
        return false;
    }
    // Leave the scene FBO bound for engine readbacks; the next frame rebinds it.
    return true;
}

void HDR_Shutdown() {
    DeleteTargets();
    if (hdr.tonemap) hdr.deleteProgram(hdr.tonemap);
    if (hdr.output) hdr.deleteProgram(hdr.output);
    hdr.tonemap = hdr.output = 0;
    if (hdr.interopDevice) hdr.closeDevice(hdr.interopDevice);
    hdr.interopDevice = NULL;
    if (hdr.context) {
        hdr.context->ClearState();
        hdr.context->Flush();
    }
    hdr.swapchain.Reset();
    hdr.context.Reset();
    hdr.device.Reset();
    hdr.active = hdr.frame = hdr.attempted = false;
    hdr.sceneSeen = hdr.mapped = hdr.guiStarted = hdr.loggedScene = false;
    hdr.width = hdr.height = 0;
}
