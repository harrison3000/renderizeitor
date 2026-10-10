// Camada de plataforma: cria/adota o contexto OpenGL 3.3 core, troca buffers e
// carrega funções. Uma implementação por sistema:
//   rz_platform_egl.cpp    Linux: EGL surfaceless (offscreen) e adoção de um
//                          contexto já corrente criado pelo host (ex.: SDL2)
#pragma once

#include <cstdint>

namespace rz {

struct Platform;    // definido em cada implementação

// Offscreen: contexto sem janela visível; o renderer desenha num FBO.
Platform* platformCreateOffscreen();

// Janela filha de `parent` em (x, y, w, h). nullptr se não suportado.
Platform* platformCreateChildWindow(void* parent, int32_t x, int32_t y, int32_t w, int32_t h);

// Adota o contexto OpenGL que o host já criou e deixou corrente (SDL2 etc.):
// só resolve as funções do GL; não cria janela, não troca buffers, não destrói.
Platform* platformCreateAdopted();

void  platformDestroy(Platform* p);
bool  platformMakeCurrent(Platform* p);
void  platformSwapBuffers(Platform* p);           // só janela
bool  platformMoveWindow(Platform* p, int32_t x, int32_t y, int32_t w, int32_t h);
void* platformGetProc(const char* name);           // para loadGl (com contexto corrente)

} // namespace rz
