# Renderizeitor — Especificação v0.2 (OpenGL)

## 1. Visão geral

Renderizeitor é um renderizador 3D em OpenGL 3.3 core, escrito em C++23 no estilo "C com classes". É distribuído como DLL de 32 bits com interface C, para ser conectado a um código legado em C compilado com MinGW.

Ele renderiza um terreno a partir de um heightmap 256×256, com texturas por bloco vindas de um atlas paletizado. Também desenha objetos poligonais que o host atualiza, e a câmera pode orbitar o terreno ou perseguir um vértice-alvo.

Há dois modos de saída:

- janela filha ancorada numa janela do host;
- offscreen, com cópia para um buffer RGBQUAD do host.

## 2. Plataforma e build

| Item | Valor |
|---|---|
| Alvo | Windows, x86 32 bits, MinGW32 com GCC 12+ (host e plugin com o mesmo compilador) |
| GPU | OpenGL 3.3 core (WGL). No Linux, só para testes: EGL surfaceless (Mesa/llvmpipe) |
| Flags | `-std=c++23 -O2 -msse2 -mfpmath=sse -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Wdouble-promotion` |
| Link | `-static-libgcc -static-libstdc++ -static -lopengl32 -lgdi32`, `-Wl,--out-implib,librenderizeitor.dll.a` |
| Build | `build.bat` (Windows, sem make), `build_linux.sh` (EGL, `-ldl`); Makefile em `meiquifaiou.txt` |

**Critério de aceitação do build:** `objdump -p renderizeitor.dll` lista apenas DLLs do sistema: KERNEL32, USER32, GDI32, OPENGL32 e msvcrt.

As funções exportadas usam `__attribute__((force_align_arg_pointer))`. O Win32 só garante pilha alinhada em 4 bytes, enquanto o SSE quer 16.

## 3. Dependências e estilo

- **Sem bibliotecas de terceiros, sem headers de GL do sistema.** `rz_gl.h/.cpp` declara o subconjunto do GL 3.3 usado e o carrega por X-macro. No Win32, os ponteiros usam `__stdcall` (`RZ_GLAPI`), e as funções do GL 1.1 vêm de `opengl32.dll` via `GetProcAddress`.
- **Da std, só:**
  - `<cstdint>`, `<cstring>`, `<cmath>` e `<bit>`;
  - `<cstdlib>`, apenas para `malloc` e `free`, via `rzAlloc`/`rzFree`.
- **O que não se usa:** containers, streams, strings e exceções.
- **Estilo "C com classes":**
  - sem herança virtual nem templates elaborados;
  - `operator[](row, col)` com dois índices em `Mat4`;
  - literais float com `f` e nada de `double`.

## 4. Interface C

### 4.1 Regras

- **Header:** `include/renderizeitor.h` compila como C puro e é a referência completa da API.
- **Chamadas:** `__cdecl`, nomes sem decoração e nenhum struct passado por valor. As funções devolvem `int32_t` com um código `RZ_*`.
- **Memória:** nunca troca de dono na fronteira. A DLL libera o que alocou em `rzDestroy`.
- **Thread:** o contexto GL pertence à thread que chamou `rzCreate*`. Todas as chamadas do contexto devem vir dela, e no modo janela ela é a thread do loop de mensagens do pai.
- **Alocação:** só nas funções de carga (heightmap, atlas, mapa de blocos, criação de objetos). `rzRender` e `rzUpdateObjectVertices` não alocam.

### 4.2 Funções

| Grupo | Funções |
|---|---|
| Contexto | `rzCreate(w, h, pixels)` offscreen; `rzCreateWindow(hwndPai, x, y, w, h)` janela filha; `rzSetViewport` (só no modo janela); `rzDestroy` |
| Terreno | `rzSetHeightmap` (256×256); `rzSetTerrainScale(cellSize, heightScale)` |
| Texturas | `rzSetTileAtlas(indices, w, h, paletteRGB)`; `rzSetTileMap` (256×256, NULL desliga as texturas); `rzSetTextureFilter` |
| Objetos | `rzSetObjectAxes`, `rzCreateObject`, `rzUpdateObjectVertices`, `rzDestroyObject`, `rzSetObjectColor`, `rzSetObjectVisible`, `rzSetObjectCulling` |
| Câmera | `rzSetCameraPitch`, `rzSetCameraDistance`, `rzSetRotationStep`, `rzSetCameraTarget(id, vertex)`, `rzSetCameraFollow(distance, height, stiffness)` |
| Frame | `rzRender` |

Erros:

| Código | Significado |
|---|---|
| `RZ_ERR_INVALID_ARG` | Argumento inválido |
| `RZ_ERR_SIZE` | Tamanho fora do suportado |
| `RZ_ERR_NO_MEMORY` | Falha de alocação |
| `RZ_ERR_GL` | Sem OpenGL 3.3, ou falha de contexto, shader ou janela |

Limites:

- **Offscreen:** até 1920×1080.
- **Janela:** até 8192 por lado.
- **Atlas:** até 4096 por lado, em múltiplos de 16.

## 5. Saída

### 5.1 Janela filha (Windows)

- **Criação:** `WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS` dentro do pai, com a classe `CS_OWNDC` e pixel format e contexto próprios. O DC do host não é tocado.
- **Contexto 3.3 core:** é criado com `wglCreateContextAttribsARB`. Essa função só existe com um contexto corrente, então ela é buscada uma vez por uma janela e um contexto descartáveis.
- **Entrada:** o mouse vai para o pai (`HTTRANSPARENT`), e o foco do teclado é devolvido ao pai.
- **Ritmo:** sem v-sync (`wglSwapIntervalEXT(0)`); o host controla o ritmo, e `rzRender` termina com `SwapBuffers`.
- **Requisitos do pai:** precisa de `WS_CLIPCHILDREN`. O host chama `rzDestroy` no `WM_DESTROY` do pai.
- **Limitação:** GDI não desenha por cima da área GL, então um HUD sobre o 3D vai precisar de overlay próprio (seção 12).

### 5.2 Offscreen

- **Destino:** o desenho vai para um FBO com cor RGBA8 e profundidade de 24 bits.
- **Cópia:** `glReadPixels(GL_BGRA)` copia direto para o buffer do host, que é RGBQUAD `0x00RRGGBB`, top-down, com stride igual à largura e byte reservado 0.
- **Orientação:** a projeção espelha o Y, então a linha 0 lida já é a de cima. Com o espelhamento, o winding de frente vira `GL_CW`.
- **No Windows:** usa uma janela oculta só para ter o contexto.

## 6. Mundo e unidades

- **Unidade:** 1 tile, o tamanho de um quad da grade com `cellSize = 1`.
- **Eixos internos:** y para cima. O vértice da grade (col, row) fica em `(col·cellSize, h·heightScale, row·cellSize)`.
- **Escala padrão:** `heightScale = 16/255`, ou seja, byte 0 é altura 0 e byte 255 é 16 tiles. O mundo mede 255 × 255 × 16.
- **Objetos:** chegam no eixo do legado, `RZ_AXES_Z_UP` (padrão): (x, y, z) = (coluna, linha, altura), convertido internamente para (x, z, y). `RZ_AXES_Y_UP` também existe.

## 7. Terreno

### 7.1 Malha

- **Tamanho:** 255×255 quads, ou 130.050 triângulos. Cada quad é dividido pela diagonal (c, r)–(c+1, r+1):
  - A = (c, r), (c, r+1), (c+1, r+1)
  - B = (c, r), (c+1, r+1), (c+1, r)
- **Montagem:** a malha é montada na CPU (`buildTerrainMesh`) e enviada a um VBO estático, sem índices. São 20 bytes por vértice:
  - posição float;
  - cor flat do triângulo;
  - (u, v) do canto do quad;
  - camada do atlas.
- **Por que sem índices:** a cor e o bloco são do triângulo e do quad, não do ponto da grade.
- **Quando é remontada:** em `rzSetHeightmap`, `rzSetTerrainScale` e `rzSetTileMap`.
- **Culling:** de face traseira, `GL_BACK` com frente CCW.

### 7.2 Cor e luz

- **Cor base:** vem de uma paleta de 766 entradas, indexada pela soma das três alturas do triângulo. São faixas com gradiente (valores provisórios):

  | Altura média | Faixa |
  |---|---|
  | 0–40 | Água |
  | 41–60 | Areia |
  | 61–150 | Grama |
  | 151–210 | Rocha |
  | 211–255 | Neve |

- **Luz:** direcional fixa (`lightDirection`), com intensidade `0.3 + 0.7 · max(0, n·L)`. A cor é calculada na carga, por triângulo.

### 7.3 Texturas

- **Atlas:** blocos de 16×16 em grade, numerados da esquerda para a direita e de cima para baixo. Só os 256 primeiros são usados.
- **Mapa de blocos:** diz qual bloco cobre cada quad, com o bloco inteiro esticado sobre o quad. A textura é pura, sem a iluminação do terreno.
- **Na GPU:** `GL_TEXTURE_2D_ARRAY` 16×16×256 com 5 níveis (16, 8, 4, 2, 1), gerados na CPU por média 2×2 arredondada. Um bloco fora do atlas sai magenta.
- **Filtros:** a ampliação é sempre nearest; o filtro muda só a redução.

  | Filtro | Redução |
  |---|---|
  | `RZ_FILTER_NEAREST` | Sem mipmap |
  | `RZ_FILTER_MIPMAP` | Nível mais próximo |
  | `RZ_FILTER_MIP_DITHER` (padrão) | No shader: lod = log2 da maior derivada de uv em texels; nível = floor(lod + limiar Bayer 4×4) |
  | `RZ_FILTER_MIP_LINEAR` | Nearest no nível, linear entre níveis |
  | `RZ_FILTER_TRILINEAR` | Bilinear no nível e linear entre níveis |

## 8. Objetos

- **Vértices:** `uint32` em ponto fixo 8.24 sem sinal, em coordenadas absolutas do mundo (1.0 = 1 tile), três por vértice.
- **Polígonos:** convexos, com os índices `uint16` concatenados. Cada polígono termina repetindo o primeiro índice; com menos de 3 vértices, é ignorado. São triangulados em leque na carga.
- **Cor:** provisória por objeto, com sombreamento flat por polígono pela normal de Newell. A luz é de dois lados enquanto o winding do legado for desconhecido.
- **Na GPU:** um VBO por objeto, reenviado em `rzUpdateObjectVertices`, e um `glDrawArrays` por objeto visível.
- **Culling:** por objeto, com `RZ_CULL_NONE` (padrão), `RZ_CULL_CW` ou `RZ_CULL_CCW`.
- **Ids:** são índices num array; os slots livres são reaproveitados.

## 9. Câmera

Tudo na CPU, uma vez por `rzRender`, gerando a matriz view-projection. A projeção é perspectiva, com FOV vertical de 60° e profundidade no estilo GL.

### 9.1 Órbita (padrão, sem alvo)

- **Centro:** o centro do terreno.
- **Yaw:** um `uint32_t` em que 2^32 é uma volta. Avança `rotationStep` por frame (padrão `1 << 22`, 1024 frames por volta), com wrap natural.
- **Pitch:** 35° por padrão, limitado a [0, 89].
- **Distância:** `D = 2R · fator`, com R o raio da esfera envolvente do terreno e o fator em [0.02, 4].
- **Planos:** `near = D − R` e `far = D + R`. O near é limitado a no máximo metade da distância ao foco e a no mínimo 0.002·R.

### 9.2 Perseguição "na corda" (`rzSetCameraTarget(id, vertex)`)

- **Alvo:** o vértice `vertex` do objeto `id`, que não precisa estar em polígono. A câmera sempre olha para ele (lookAt).
- **Movimento horizontal:**
  - além do comprimento da corda (padrão 3 tiles), a câmera é puxada;
  - abaixo da metade dele, recua;
  - entre os dois, a corda fica frouxa.
- **Altura:** `height` acima do alvo (padrão 1 tile).
- **Suavidade:** os dois movimentos são amortecidos por `stiffness` (padrão 0.08).
- **Chão:** a altura desejada respeita uma folga de 0,5 tile sobre o terreno, com piso duro de 0,1. A subida é amortecida e mais rápida que a descida.
- **Mudança na corda:** a distância horizontal muda na hora, na mesma proporção, para o zoom responder no mesmo frame.
- **Saída:** `id < 0`, ou o objeto destruído, volta à órbita.

## 10. Frame (`rzRender`)

1. Atualiza a câmera e monta a view-projection.
2. Faz bind do FBO (offscreen) ou do framebuffer padrão (janela) e limpa com a cor de fundo `0x00202830`, com profundidade em `GL_LESS`.
3. Desenha o terreno em um draw: textura se houver atlas e mapa de blocos, senão as cores flat.
4. Desenha os objetos.
5. Faz `SwapBuffers` (janela) ou `glReadPixels` para o buffer do host (offscreen).

Sem heightmap, só limpa e apresenta.

## 11. Testes

- **`test/rz_test.cpp`:**
  - linka o núcleo estático (`RZ_STATIC`) e renderiza offscreen;
  - grava `.ppm`;
  - registra hashes FNV-1a por frame. Eles só são comparáveis na mesma máquina e driver, então não há regressão bit a bit entre GPUs.
- **`test/rz_testdata.h`:** dados procedurais.
  - Ilha, atlas e mapa de blocos.
  - Casas e torres.
  - Um veículo de ~0,85 tile que segue o terreno.
  - Percurso automático.
- **`test/rz_viewer.c`:** aplicação Win32 que usa `rzCreateWindow`.

  | Tecla | Ação |
  |---|---|
  | WASD | Dirige o carro |
  | C | Câmera segue o carro |
  | O | Liga/desliga os objetos |
  | T | Liga/desliga as texturas |
  | F | Alterna o filtro |
  | PgUp / PgDn | Comprimento da corda |
  | ↑ / ↓ | Altura |

- **Linux:** com `build_linux.sh`, roda no llvmpipe via EGL.

## 12. Roadmap e decisões em aberto

- **`rzUpdateHeightmap(ctx, data, x, y, w, h)`:** atualizaria só a região alterada da malha (`glBufferSubData` dos quads com col em [x−1, x+w−1] e row em [y−1, y+h−1], limitados a [0, 254]).
- **Sombras e iluminação por pixel:** o motivo principal da migração para GL.
- **Texturas e cores reais nos objetos:** hoje a cor é provisória.
- **Overlay/HUD:** para o legado desenhar por cima da janela GL.
- **Desempenho em CPU fraca:** no Allwinner D1, via llvmpipe, os 130 mil triângulos pequenos dominam. O próximo passo seria descarte de blocos do terreno fora do frustum e LOD por blocos.
- **Escolha da diagonal do quad pela altura dos cantos**, de forma determinística.
- **Winding dos polígonos do legado:** ainda desconhecido; define o culling e a luz de um lado só.
- **Valores provisórios:** paleta, direção da luz, ambient, pitch, FOV, passo de rotação, cor de fundo e cor dos objetos.
