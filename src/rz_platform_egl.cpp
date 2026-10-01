// Plataforma EGL (Linux): contexto OpenGL 3.3 core surfaceless, só offscreen.
// Usada pelos testes automáticos (llvmpipe) e para rodar em Linux sem GPU.
// libEGL é carregada com dlopen; nenhum header de EGL é necessário.

#if !defined(_WIN32)

#include <dlfcn.h>
#include <cstdlib>

#include "rz_platform.h"

namespace rz {

namespace {

using EGLDisplay = void*;
using EGLConfig  = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLint     = int32_t;
using EGLenum    = unsigned int;
using EGLBoolean = unsigned int;

constexpr EGLint EGL_NONE                          = 0x3038;
constexpr EGLint EGL_RENDERABLE_TYPE               = 0x3040;
constexpr EGLint EGL_OPENGL_BIT                    = 0x0008;
constexpr EGLenum EGL_OPENGL_API                   = 0x30A2;
constexpr EGLint EGL_CONTEXT_MAJOR_VERSION         = 0x3098;
constexpr EGLint EGL_CONTEXT_MINOR_VERSION         = 0x30FB;
constexpr EGLint EGL_CONTEXT_OPENGL_PROFILE_MASK   = 0x30FD;
constexpr EGLint EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x0001;
constexpr EGLenum EGL_PLATFORM_SURFACELESS_MESA    = 0x31DD;

struct Egl {
    void* lib = nullptr;
    void* (*getProcAddress)(const char*) = nullptr;
    EGLDisplay (*getPlatformDisplay)(EGLenum, void*, const EGLint*) = nullptr;
    EGLBoolean (*initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean (*terminate)(EGLDisplay) = nullptr;
    EGLBoolean (*chooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLBoolean (*bindApi)(EGLenum) = nullptr;
    EGLContext (*createContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean (*destroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLBoolean (*makeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
};

Egl g_egl;

bool loadEgl() {
    if (g_egl.lib) return true;
    void* lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) return false;
    g_egl.getProcAddress = reinterpret_cast<void* (*)(const char*)>(dlsym(lib, "eglGetProcAddress"));
    if (!g_egl.getProcAddress) return false;
    g_egl.getPlatformDisplay = reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(
        g_egl.getProcAddress("eglGetPlatformDisplayEXT"));
    g_egl.initialize     = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLint*, EGLint*)>(dlsym(lib, "eglInitialize"));
    g_egl.terminate      = reinterpret_cast<EGLBoolean (*)(EGLDisplay)>(dlsym(lib, "eglTerminate"));
    g_egl.chooseConfig   = reinterpret_cast<EGLBoolean (*)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*)>(
        dlsym(lib, "eglChooseConfig"));
    g_egl.bindApi        = reinterpret_cast<EGLBoolean (*)(EGLenum)>(dlsym(lib, "eglBindAPI"));
    g_egl.createContext  = reinterpret_cast<EGLContext (*)(EGLDisplay, EGLConfig, EGLContext, const EGLint*)>(
        dlsym(lib, "eglCreateContext"));
    g_egl.destroyContext = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLContext)>(dlsym(lib, "eglDestroyContext"));
    g_egl.makeCurrent    = reinterpret_cast<EGLBoolean (*)(EGLDisplay, EGLSurface, EGLSurface, EGLContext)>(
        dlsym(lib, "eglMakeCurrent"));
    if (!g_egl.getPlatformDisplay || !g_egl.initialize || !g_egl.chooseConfig || !g_egl.bindApi ||
        !g_egl.createContext || !g_egl.makeCurrent) {
        return false;
    }
    g_egl.lib = lib;
    return true;
}

} // namespace

struct Platform {
    EGLDisplay display;
    EGLContext context;
};

Platform* platformCreateOffscreen() {
    if (!loadEgl()) return nullptr;
    EGLDisplay display = g_egl.getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, nullptr, nullptr);
    if (!display) return nullptr;
    EGLint major, minor;
    if (!g_egl.initialize(display, &major, &minor)) return nullptr;

    // Surfaceless: config pode não existir; contexto sem config (EGL_KHR_no_config_context)
    const EGLint configAttribs[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig config = nullptr;
    EGLint count = 0;
    g_egl.chooseConfig(display, configAttribs, &config, 1, &count);
    if (count == 0) config = nullptr;

    if (!g_egl.bindApi(EGL_OPENGL_API)) return nullptr;
    const EGLint contextAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE,
    };
    EGLContext context = g_egl.createContext(display, config, nullptr, contextAttribs);
    if (!context) return nullptr;

    Platform* p = static_cast<Platform*>(std::malloc(sizeof(Platform)));
    if (!p) {
        g_egl.destroyContext(display, context);
        return nullptr;
    }
    p->display = display;
    p->context = context;
    if (!platformMakeCurrent(p)) {
        platformDestroy(p);
        return nullptr;
    }
    return p;
}

Platform* platformCreateChildWindow(void*, int32_t, int32_t, int32_t, int32_t) {
    return nullptr;     // só offscreen nesta plataforma
}

void platformDestroy(Platform* p) {
    if (!p) return;
    g_egl.makeCurrent(p->display, nullptr, nullptr, nullptr);
    g_egl.destroyContext(p->display, p->context);
    std::free(p);
}

bool platformMakeCurrent(Platform* p) {
    return g_egl.makeCurrent(p->display, nullptr, nullptr, p->context) != 0;
}

void platformSwapBuffers(Platform*) {}

bool platformMoveWindow(Platform*, int32_t, int32_t, int32_t, int32_t) {
    return false;
}

void* platformGetProc(const char* name) {
    return g_egl.getProcAddress ? g_egl.getProcAddress(name) : nullptr;
}

} // namespace rz

#endif
