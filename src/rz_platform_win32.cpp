// Plataforma Win32/WGL.
//
// Modo janela: cria uma janela filha (WS_CHILD) dentro da janela do host, com
// pixel format e contexto OpenGL 3.3 core próprios; o DC do host não é tocado.
// A janela filha é "transparente" para o mouse (HTTRANSPARENT) e devolve o
// foco do teclado ao pai, então o host continua recebendo a entrada.
//
// Modo offscreen: janela oculta só para ter um contexto; o renderer desenha
// num FBO e copia para o buffer do host.
//
// Tudo deve ser chamado da thread que tem o loop de mensagens do host (a
// mesma do rzRender). O pai precisa de WS_CLIPCHILDREN para não pintar por cima.

#if defined(_WIN32)

#ifndef _WIN32_WINNT
#  define _WIN32_WINNT 0x0600     // GetModuleHandleExA (o MinGW antigo assume um Windows mais velho)
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdlib>

#include "rz_platform.h"

namespace rz {

namespace {

// WGL_ARB_create_context / WGL_ARB_pixel_format / WGL_EXT_swap_control
constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB    = 0x2091;
constexpr int WGL_CONTEXT_MINOR_VERSION_ARB    = 0x2092;
constexpr int WGL_CONTEXT_PROFILE_MASK_ARB     = 0x9126;
constexpr int WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x0001;
constexpr int WGL_DRAW_TO_WINDOW_ARB           = 0x2001;
constexpr int WGL_SUPPORT_OPENGL_ARB           = 0x2010;
constexpr int WGL_DOUBLE_BUFFER_ARB            = 0x2011;
constexpr int WGL_PIXEL_TYPE_ARB               = 0x2013;
constexpr int WGL_TYPE_RGBA_ARB                = 0x202B;
constexpr int WGL_COLOR_BITS_ARB               = 0x2014;
constexpr int WGL_DEPTH_BITS_ARB               = 0x2022;
constexpr int WGL_STENCIL_BITS_ARB             = 0x2023;

using PFN_wglCreateContextAttribsARB = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
using PFN_wglChoosePixelFormatARB    = BOOL(WINAPI*)(HDC, const int*, const FLOAT*, UINT, int*, UINT*);
using PFN_wglSwapIntervalEXT         = BOOL(WINAPI*)(int);

constexpr const char* kClassName = "RenderizeitorGL";

HMODULE   g_opengl32 = nullptr;
HINSTANCE g_instance = nullptr;
bool      g_classRegistered = false;
bool      g_extensionsLoaded = false;
PFN_wglCreateContextAttribsARB g_createContextAttribs = nullptr;
PFN_wglChoosePixelFormatARB    g_choosePixelFormat = nullptr;

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST:
        return HTTRANSPARENT;               // mouse vai para o pai
    case WM_ERASEBKGND:
        return 1;                           // quem pinta é o rzRender
    case WM_PAINT:
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_SETFOCUS: {
        HWND parent = GetParent(hwnd);      // teclado fica com o pai
        if (parent) SetFocus(parent);
        return 0;
    }
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

// HINSTANCE do módulo onde este código está (a DLL, ou o exe se estático).
HINSTANCE thisModule() {
    HMODULE module = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&windowProc), &module);
    return module;
}

bool registerClass() {
    if (g_classRegistered) return true;
    g_instance = thisModule();
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_OWNDC;
    wc.lpfnWndProc   = windowProc;
    wc.hInstance     = g_instance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassA(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_classRegistered = true;
    return true;
}

void legacyPixelFormat(PIXELFORMATDESCRIPTOR* pfd) {
    ZeroMemory(pfd, sizeof(*pfd));
    pfd->nSize      = sizeof(*pfd);
    pfd->nVersion   = 1;
    pfd->dwFlags    = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd->iPixelType = PFD_TYPE_RGBA;
    pfd->cColorBits = 32;
    pfd->cDepthBits = 24;
    pfd->cStencilBits = 8;
    pfd->iLayerType = PFD_MAIN_PLANE;
}

// wglCreateContextAttribsARB só existe com um contexto corrente: cria uma
// janela e um contexto descartáveis para buscar as extensões (uma vez).
bool loadWglExtensions() {
    if (g_extensionsLoaded) return true;
    if (!g_opengl32) g_opengl32 = LoadLibraryA("opengl32.dll");
    if (!g_opengl32 || !registerClass()) return false;

    HWND dummy = CreateWindowExA(0, kClassName, "", WS_POPUP, 0, 0, 16, 16,
                                 nullptr, nullptr, g_instance, nullptr);
    if (!dummy) return false;
    HDC dc = GetDC(dummy);
    PIXELFORMATDESCRIPTOR pfd;
    legacyPixelFormat(&pfd);
    const int format = ChoosePixelFormat(dc, &pfd);
    bool ok = false;
    if (format && SetPixelFormat(dc, format, &pfd)) {
        HGLRC rc = wglCreateContext(dc);
        if (rc && wglMakeCurrent(dc, rc)) {
            g_createContextAttribs = reinterpret_cast<PFN_wglCreateContextAttribsARB>(
                reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));
            g_choosePixelFormat = reinterpret_cast<PFN_wglChoosePixelFormatARB>(
                reinterpret_cast<void*>(wglGetProcAddress("wglChoosePixelFormatARB")));
            ok = g_createContextAttribs != nullptr;
            wglMakeCurrent(nullptr, nullptr);
        }
        if (rc) wglDeleteContext(rc);
    }
    ReleaseDC(dummy, dc);
    DestroyWindow(dummy);
    g_extensionsLoaded = ok;
    return ok;
}

} // namespace

struct Platform {
    HWND  hwnd;
    HDC   dc;
    HGLRC rc;
};

namespace {

// Pixel format + contexto 3.3 core numa janela já criada.
Platform* createOnWindow(HWND hwnd) {
    HDC dc = GetDC(hwnd);
    if (!dc) return nullptr;

    int format = 0;
    if (g_choosePixelFormat) {
        const int attribs[] = {
            WGL_DRAW_TO_WINDOW_ARB, 1, WGL_SUPPORT_OPENGL_ARB, 1, WGL_DOUBLE_BUFFER_ARB, 1,
            WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB, WGL_COLOR_BITS_ARB, 32,
            WGL_DEPTH_BITS_ARB, 24, WGL_STENCIL_BITS_ARB, 8, 0,
        };
        UINT count = 0;
        if (!g_choosePixelFormat(dc, attribs, nullptr, 1, &format, &count) || count == 0) format = 0;
    }
    PIXELFORMATDESCRIPTOR pfd;
    legacyPixelFormat(&pfd);
    if (!format) format = ChoosePixelFormat(dc, &pfd);
    if (!format) { ReleaseDC(hwnd, dc); return nullptr; }
    DescribePixelFormat(dc, format, sizeof(pfd), &pfd);
    if (!SetPixelFormat(dc, format, &pfd)) { ReleaseDC(hwnd, dc); return nullptr; }

    const int contextAttribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3, WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB, 0,
    };
    HGLRC rc = g_createContextAttribs(dc, nullptr, contextAttribs);
    if (!rc) { ReleaseDC(hwnd, dc); return nullptr; }

    Platform* p = static_cast<Platform*>(std::malloc(sizeof(Platform)));
    if (!p) { wglDeleteContext(rc); ReleaseDC(hwnd, dc); return nullptr; }
    p->hwnd = hwnd;
    p->dc   = dc;
    p->rc   = rc;
    if (!platformMakeCurrent(p)) { platformDestroy(p); return nullptr; }

    // Sem v-sync: o host controla o ritmo dos frames
    auto swapInterval = reinterpret_cast<PFN_wglSwapIntervalEXT>(
        reinterpret_cast<void*>(wglGetProcAddress("wglSwapIntervalEXT")));
    if (swapInterval) swapInterval(0);
    return p;
}

} // namespace

Platform* platformCreateOffscreen() {
    if (!loadWglExtensions()) return nullptr;
    HWND hwnd = CreateWindowExA(0, kClassName, "", WS_POPUP, 0, 0, 16, 16,
                                nullptr, nullptr, g_instance, nullptr);
    if (!hwnd) return nullptr;
    Platform* p = createOnWindow(hwnd);
    if (!p) DestroyWindow(hwnd);
    return p;
}

Platform* platformCreateChildWindow(void* parent, int32_t x, int32_t y, int32_t w, int32_t h) {
    if (!parent || !loadWglExtensions()) return nullptr;
    HWND hwnd = CreateWindowExA(0, kClassName, "",
                                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
                                x, y, w, h, static_cast<HWND>(parent), nullptr, g_instance, nullptr);
    if (!hwnd) return nullptr;
    Platform* p = createOnWindow(hwnd);
    if (!p) DestroyWindow(hwnd);
    return p;
}

void platformDestroy(Platform* p) {
    if (!p) return;
    if (wglGetCurrentContext() == p->rc) wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(p->rc);
    ReleaseDC(p->hwnd, p->dc);
    DestroyWindow(p->hwnd);
    std::free(p);
}

bool platformMakeCurrent(Platform* p) {
    if (wglGetCurrentContext() == p->rc) return true;
    return wglMakeCurrent(p->dc, p->rc) != FALSE;
}

void platformSwapBuffers(Platform* p) {
    SwapBuffers(p->dc);
}

bool platformMoveWindow(Platform* p, int32_t x, int32_t y, int32_t w, int32_t h) {
    return MoveWindow(p->hwnd, x, y, w, h, TRUE) != FALSE;
}

// wglGetProcAddress não devolve as funções do OpenGL 1.1 (glClear etc.):
// essas vêm direto do opengl32.dll. Alguns drivers devolvem 1, 2, 3 ou -1 em
// vez de NULL quando falham.
void* platformGetProc(const char* name) {
    PROC proc = wglGetProcAddress(name);
    const intptr_t value = reinterpret_cast<intptr_t>(proc);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1) {
        proc = GetProcAddress(g_opengl32, name);
    }
    return reinterpret_cast<void*>(proc);
}

} // namespace rz

#endif
