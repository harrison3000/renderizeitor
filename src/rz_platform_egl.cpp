// Plataforma Linux. Dois modos:
//
//   OFFSCREEN  contexto OpenGL 3.3 core surfaceless via EGL (sem janela).
//              Usado pelos testes automáticos (llvmpipe) e para rodar sem GPU.
//              libEGL é carregada com dlopen; nenhum header de EGL é preciso.
//
//   ADOPTED    o contexto OpenGL já existe e está corrente (quem cria a janela
//              e o contexto é o host, ex.: SDL2). A plataforma não cria, não
//              troca buffers nem destrói nada: só resolve os ponteiros das
//              funções do OpenGL (via libGL). O host faz o swap.

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

// Resolvedor das funções do OpenGL para o modo ADOPTED: libGL (GLX) resolve
// tanto contextos GLX (X11) quanto, via glXGetProcAddress, os símbolos do core.
struct GlLib {
    void* lib = nullptr;
    void* (*getProcAddress)(const unsigned char*) = nullptr;   // glXGetProcAddressARB
};

GlLib g_gl;

// Qual resolvedor usar em platformGetProc (depende do modo criado).
void* (*g_getProc)(const char* name) = nullptr;

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

bool loadGlLib() {
    if (g_gl.lib) return true;
    void* lib = dlopen("libGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib) return false;
    g_gl.getProcAddress = reinterpret_cast<void* (*)(const unsigned char*)>(dlsym(lib, "glXGetProcAddressARB"));
    if (!g_gl.getProcAddress)
        g_gl.getProcAddress = reinterpret_cast<void* (*)(const unsigned char*)>(dlsym(lib, "glXGetProcAddress"));
    g_gl.lib = lib;
    return true;
}

// Modo OFFSCREEN: as funções do GL vêm do EGL corrente.
void* eglProc(const char* name) {
    return g_egl.getProcAddress ? g_egl.getProcAddress(name) : nullptr;
}

// Modo ADOPTED: glXGetProcAddress resolve o core; dlsym do libGL cobre o que
// falta; eglGetProcAddress ajuda quando a janela do host usa EGL (Wayland).
void* adoptedProc(const char* name) {
    if (g_gl.getProcAddress) {
        void* p = g_gl.getProcAddress(reinterpret_cast<const unsigned char*>(name));
        if (p) return p;
    }
    if (g_gl.lib) {
        void* p = dlsym(g_gl.lib, name);
        if (p) return p;
    }
    if (g_egl.getProcAddress) return g_egl.getProcAddress(name);
    return nullptr;
}

enum Mode { kOffscreen, kAdopted };

} // namespace

struct Platform {
    Mode       mode;
    EGLDisplay display;     // só OFFSCREEN
    EGLContext context;     // só OFFSCREEN
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
    p->mode    = kOffscreen;
    p->display = display;
    p->context = context;
    g_getProc = eglProc;
    if (!platformMakeCurrent(p)) {
        platformDestroy(p);
        return nullptr;
    }
    return p;
}

Platform* platformCreateAdopted() {
    if (!loadGlLib()) return nullptr;
    Platform* p = static_cast<Platform*>(std::malloc(sizeof(Platform)));
    if (!p) return nullptr;
    p->mode    = kAdopted;
    p->display = nullptr;
    p->context = nullptr;
    g_getProc = adoptedProc;
    return p;
}

void platformDestroy(Platform* p) {
    if (!p) return;
    if (p->mode == kOffscreen) {
        g_egl.makeCurrent(p->display, nullptr, nullptr, nullptr);
        g_egl.destroyContext(p->display, p->context);
    }
    // ADOPTED: o contexto e a janela são do host; nada a destruir aqui.
    std::free(p);
}

bool platformMakeCurrent(Platform* p) {
    if (p->mode == kOffscreen)
        return g_egl.makeCurrent(p->display, nullptr, nullptr, p->context) != 0;
    return true;    // ADOPTED: o host mantém o contexto corrente
}

void platformSwapBuffers(Platform*) {}   // OFFSCREEN: nada; ADOPTED: quem troca é o host

void* platformGetProc(const char* name) {
    return g_getProc ? g_getProc(name) : nullptr;
}

} // namespace rz

#endif
