# Renderizeitor — versão OpenGL

Reimplementação do renderer de software em OpenGL 3.3 core, mantendo a API C.
A versão de software (`rendgerong`) foi descontinuada e removida; a tabela
abaixo fica como registro do que mudou. A spec atual é `renderizeitor-spec.md`.

## Mudanças na API

| Item | Software | OpenGL |
|---|---|---|
| Saída | `rzCreate(w, h, pixels)` | igual (offscreen: FBO + `glReadPixels` para o buffer do host) **ou** `rzCreateWindow(hwndPai, x, y, w, h)`: janela filha, sem cópia |
| Redimensionar | — | `rzSetViewport(ctx, x, y, w, h)` (só janela) |
| Erro novo | — | `RZ_ERR_GL` (sem OpenGL 3.3, falha de contexto/shader/janela) |
| Filtros | 0 nearest, 1 mipmap, 2 mip+dither | + 3 `RZ_FILTER_MIP_LINEAR` (nearest no nível, linear entre níveis), 4 `RZ_FILTER_TRILINEAR`; ampliação sempre nearest |
| `heightScale` padrão | 0,25 | 16/255 (byte 255 = 16 tiles, como no legado) |
| Eixos dos objetos | `RZ_AXES_Y_UP` | `RZ_AXES_Z_UP` (como no legado) |
| Thread | qualquer, uma por contexto | a mesma que criou o contexto (o contexto GL é dela) |

## Janela filha (Windows)

- Criada com `WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS` dentro da janela do host, com pixel format e contexto próprios: o DC do host não é tocado.
- O host precisa ter `WS_CLIPCHILDREN`, senão o GDI dele pinta por cima.
- Mouse: a janela filha responde `HTTRANSPARENT`, então os cliques vão para o pai. Teclado: devolve o foco ao pai.
- Sem v-sync (`wglSwapIntervalEXT(0)`): o host controla o ritmo.
- Chame `rzDestroy` no `WM_DESTROY` do pai, antes de a janela filha ser destruída junto.
- GDI não aparece por cima da janela GL: um HUD do legado sobre a área 3D vai precisar de uma função de overlay.

## Organização

| Arquivo | Conteúdo |
|---|---|
| `src/rz_gl.h/.cpp` | Subconjunto do GL 3.3 sem headers do sistema; ponteiros com `__stdcall` no Win32 |
| `src/rz_platform_win32.cpp` | WGL: janela filha ou janela oculta (offscreen) |
| `src/rz_platform_egl.cpp` | EGL surfaceless (Linux, só offscreen): testes e llvmpipe |
| `src/rz_camera.cpp` | Órbita e câmera de perseguição (igual ao software) |
| `src/rz_terrain.cpp` | Malha do terreno montada na CPU na carga, paleta, mipmaps do atlas |
| `src/rz_render.cpp`, `rz_shaders.h` | Programas, FBO e o frame |
| `src/rz_object.cpp` | Objetos (um vertex buffer por objeto, reenviado no update) |
| `src/rz_pcx.cpp` | Leitura do atlas em PCX de 8 bits (`rzLoadTileAtlas`) |

## Desempenho (máquina de testes, 2 núcleos, 1280x720)

| | Software (`rendgerong`) | OpenGL via llvmpipe |
|---|---|---|
| Frame completo | ~5–12 ms | ~45–65 ms |

O llvmpipe gasta por triângulo, e o terreno tem 130 mil triângulos de 1–2 px.
Numa GPU real isso cai para frações de milissegundo. Para rodar em CPU fraca
(Allwinner D1), a próxima otimização seria descartar blocos do terreno fora do
frustum e reduzir detalhe ao longe (LOD por blocos).
