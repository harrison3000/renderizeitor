// Camada de plataforma: cria o contexto OpenGL 3.3 core, troca buffers e
// carrega funções. Uma implementação por sistema:
//   rz_platform_egl.cpp    Linux: EGL surfaceless (offscreen) e contexto próprio
//                          numa janela SDL2 do host
#pragma once

#include <cstdint>

namespace rz {

struct Platform;    // definido em cada implementação

// Offscreen: contexto sem janela visível; o renderer desenha num FBO.
Platform* platformCreateOffscreen();

// Janela SDL2 do host (SDL_Window*, com SDL_WINDOW_OPENGL): cria um contexto
// 3.3 core nela e devolve o tamanho do drawable. A janela continua do host.
Platform* platformCreateSdl(void* sdlWindow, int32_t* outWidth, int32_t* outHeight);

void  platformDestroy(Platform* p);
bool  platformMakeCurrent(Platform* p);
void* platformGetProc(const char* name);           // para loadGl (com contexto corrente)

} // namespace rz
