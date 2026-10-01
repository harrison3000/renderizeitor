// Camada de plataforma: cria o contexto OpenGL 3.3 core, troca buffers e
// carrega funções. Uma implementação por sistema:
//   rz_platform_win32.cpp  WGL: janela filha ancorada na do host, ou janela
//                          oculta para o modo offscreen
//   rz_platform_egl.cpp    EGL surfaceless (Linux, só offscreen): testes e llvmpipe
#pragma once

#include <cstdint>

namespace rz {

struct Platform;    // definido em cada implementação

// Offscreen: contexto sem janela visível; o renderer desenha num FBO.
Platform* platformCreateOffscreen();

// Janela filha de `parent` (HWND no Windows) em (x, y, w, h), coordenadas do
// cliente do pai. nullptr se não suportado ou em caso de erro.
Platform* platformCreateChildWindow(void* parent, int32_t x, int32_t y, int32_t w, int32_t h);

void  platformDestroy(Platform* p);
bool  platformMakeCurrent(Platform* p);
void  platformSwapBuffers(Platform* p);           // só janela
bool  platformMoveWindow(Platform* p, int32_t x, int32_t y, int32_t w, int32_t h);
void* platformGetProc(const char* name);           // para loadGl (com contexto corrente)

} // namespace rz
