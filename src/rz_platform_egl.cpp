// Plataforma Linux. Dois modos:
//
//   OFFSCREEN  contexto OpenGL 3.3 core surfaceless via EGL (sem janela).
//              Usado pelos testes automáticos (llvmpipe) e para rodar sem GPU.
//              libEGL é carregada com dlopen; nenhum header de EGL é preciso.
//
//   SDL        janela SDL2 criada pelo host (com SDL_WINDOW_OPENGL): cria um
//              contexto OpenGL 3.3 core próprio nela e destrói o contexto no
//              fim. A janela e a troca de buffers (SDL_GL_SwapWindow) são do host.

#if !defined(_WIN32)

#include <dlfcn.h>
#include <cstdlib>

#include <SDL.h>

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

// Modo OFFSCREEN: as funções do GL vêm do EGL corrente.
void* eglProc(const char* name) {
    return g_egl.getProcAddress ? g_egl.getProcAddress(name) : nullptr;
}

// Modo SDL: as funções do GL vêm do SDL (GLX ou EGL, conforme o driver de vídeo).
void* sdlProc(const char* name) {
    return SDL_GL_GetProcAddress(name);
}

enum Mode { kOffscreen, kSdl };

} // namespace

struct Platform {
    Mode          mode;
    EGLDisplay    display;     // só OFFSCREEN
    EGLContext    context;     // só OFFSCREEN
    SDL_Window*   window;      // só SDL (do host)
    SDL_GLContext glContext;   // só SDL (nosso)
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
    p->mode      = kOffscreen;
    p->display   = display;
    p->context   = context;
    p->window    = nullptr;
    p->glContext = nullptr;
    g_getProc = eglProc;
    if (!platformMakeCurrent(p)) {
        platformDestroy(p);
        return nullptr;
    }
    return p;
}

Platform* platformCreateSdl(void* sdlWindow, int32_t* outWidth, int32_t* outHeight) {
    SDL_Window* window = static_cast<SDL_Window*>(sdlWindow);
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_OPENGL)) return nullptr;

    // Pede 3.3 core só para este contexto: guarda e devolve os atributos do host
    int major = 0, minor = 0, profile = 0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, &profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GLContext glContext = SDL_GL_CreateContext(window);     // já fica corrente
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
    if (!glContext) return nullptr;

    Platform* p = static_cast<Platform*>(std::malloc(sizeof(Platform)));
    if (!p) {
        SDL_GL_DeleteContext(glContext);
        return nullptr;
    }
    p->mode      = kSdl;
    p->display   = nullptr;
    p->context   = nullptr;
    p->window    = window;
    p->glContext = glContext;
    g_getProc = sdlProc;
    if (!platformMakeCurrent(p)) {
        platformDestroy(p);
        return nullptr;
    }

    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(window, &w, &h);
    *outWidth  = w;
    *outHeight = h;
    return p;
}

void platformDestroy(Platform* p) {
    if (!p) return;
    if (p->mode == kOffscreen) {
        g_egl.makeCurrent(p->display, nullptr, nullptr, nullptr);
        g_egl.destroyContext(p->display, p->context);
    } else {
        if (SDL_GL_GetCurrentContext() == p->glContext) SDL_GL_MakeCurrent(p->window, nullptr);
        SDL_GL_DeleteContext(p->glContext);     // a janela é do host
    }
    std::free(p);
}

bool platformMakeCurrent(Platform* p) {
    if (p->mode == kOffscreen)
        return g_egl.makeCurrent(p->display, nullptr, nullptr, p->context) != 0;
    if (SDL_GL_GetCurrentContext() == p->glContext) return true;
    return SDL_GL_MakeCurrent(p->window, p->glContext) == 0;
}

void* platformGetProc(const char* name) {
    return g_getProc ? g_getProc(name) : nullptr;
}

} // namespace rz

#endif
