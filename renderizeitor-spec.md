# Renderizeitor — Especificação v0.2 (OpenGL)

## 1. Visão geral

Renderizeitor é um renderizador 3D em OpenGL 3.3 core, escrito em C++23 no estilo "C com classes". É distribuído como DLL de 32 bits com interface C, para ser conectado a um código legado em C compilado com MinGW.

Ele renderiza um terreno a partir de um heightmap 256×256, com texturas por bloco vindas de um atlas paletizado. Também desenha objetos poligonais que o host atualiza, e a câmera persegue um vértice-alvo.

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
- **Std liberada quando simplifica o código**, desde que linke estática (`-static-libstdc++`). Containers como `std::vector` são o caso principal: buffers do terreno, do atlas e dos objetos, e o contexto inteiro (`new RzContext`, com os padrões nos inicializadores dos membros).
- **Sem exceções** (`-fno-exceptions`): uma falha de alocação aborta o programa em vez de virar `RZ_ERR_NO_MEMORY`. A chance é baixíssima e isso foi aceito.
- **O que não se usa:** streams e RTTI.
- **Estilo "C com classes":**
  - sem herança virtual nem templates elaborados;
  - `operator[](row, col)` com dois índices em `Mat4`;
  - literais float com `f` e nada de `double`.

## 4. Interface C

### 4.1 Regras

- **Header:** `include/renderizeitor.h` compila como C puro e é a referência completa da API.
- **Carga dinâmica (plugin):** `include/renderizeitor_plugin.h` é só header, em C, e independente do `renderizeitor.h` (repete as constantes e o tipo do contexto). `rzPluginLoad(&rz, "renderizeitor.dll")` faz `LoadLibrary` e resolve todas as exportações numa struct `RzPlugin`, e o host chama `rz.rzRender(ctx)` etc., sem linkar com a import library. `rzPluginUnload` descarrega. Ao adicionar uma função à API, acrescente-a também em `RZ_PLUGIN_FUNCTIONS`.
- **Chamadas:** `__cdecl`, nomes sem decoração e nenhum struct passado por valor. As funções devolvem `int32_t` com um código `RZ_*`.
- **Memória:** nunca troca de dono na fronteira. A DLL libera o que alocou em `rzDestroy`.
- **Thread:** o contexto GL pertence à thread que chamou `rzCreate*`. Todas as chamadas do contexto devem vir dela, e no modo janela ela é a thread do loop de mensagens do pai.
- **Alocação:** só nas funções de carga (heightmap, atlas, mapa de blocos, `rzCreateObject`, `rzAddObjectPolygon`). `rzRender` e `rzUpdateObjectVertices` não alocam (os vetores já têm o tamanho final).

### 4.2 Funções

| Grupo | Funções |
|---|---|
| Contexto | `rzCreate(w, h, pixels)` offscreen; `rzCreateWindow(hwndPai, x, y, w, h)` janela filha; `rzSetViewport` (só no modo janela); `rzDestroy` |
| Terreno | `rzSetHeightmap` (256×256); `rzSetTerrainScale(cellSize, heightScale)` |
| Texturas | `rzLoadTileAtlas(caminhoPcx)`; `rzSetTileMap` (256×256, NULL desliga as texturas); `rzSetTextureFilter` |
| Objetos | `rzCreateObject(vertexCount)`, `rzAddObjectPolygon(id, indices, count, paletteIndex)`, `rzAddObjectTexturedPolygon(id, indices, uvs, count)`, `rzLoadObjectTexture(id, caminhoPcx)`, `rzLoadFallbackTexture(caminhoPcx)`, `rzUpdateObjectVertices`, `rzDestroyObject` |
| Câmera | `rzSetCameraTarget(id, vertex)`, `rzSetCameraFollow(distance, height, stiffness)` |
| Cena | `rzSetBackgroundColor(r, g, b)` (0..255; padrão 32, 40, 48) |
| Frame | `rzRender` |

Erros:

| Código | Significado |
|---|---|
| `RZ_ERR_INVALID_ARG` | Argumento inválido |
| `RZ_ERR_SIZE` | Tamanho fora do suportado |
| `RZ_ERR_NO_MEMORY` | Reservado: hoje uma falha de alocação aborta |
| `RZ_ERR_GL` | Sem OpenGL 3.3, ou falha de contexto, shader ou janela |
| `RZ_ERR_FILE` | Arquivo não abriu ou não pôde ser lido |
| `RZ_ERR_FORMAT` | Arquivo em formato não suportado ou corrompido |

Limites:

- **Offscreen:** até 1920×1080.
- **Janela:** até 8192 por lado.
- **Atlas:** PCX de pelo menos 256×256; de um maior, só o canto 256×256 é usado.

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
- **Objetos:** chegam no eixo do legado, (x, y, z) = (coluna, linha, altura), z para cima, e são convertidos internamente para (x, z, y). Fixo (`rzSetObjectAxes` saiu).

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

- **Atlas:** lido de um PCX por `rzLoadTileAtlas` (`src/rz_pcx.cpp`).
  - Formato aceito: ZSoft com 8 bits por pixel, 1 plano, RLE (ou sem compressão) e paleta VGA de 256 cores no fim do arquivo (marcador `0x0C`). Uma sequência RLE pode atravessar o fim da linha.
  - Usa só o canto superior esquerdo de 256×256: 16 × 16 blocos de 16×16, numerados da esquerda para a direita e de cima para baixo.
  - O arquivo é lido e validado inteiro antes de tocar na textura; se der erro, o atlas anterior continua.
  - Futuro: texturas high-res em PNG.
- **Mapa de blocos:** diz qual bloco cobre cada quad, com o bloco inteiro esticado sobre o quad. A textura recebe um sombreamento leve: a mesma luz flat do triângulo, atenuada para `mix(1, luz, 0.35)` (`kTexturedShading`). Com a luz mínima (ambient 0,3), a textura escurece até ~76%.
- **Na GPU:** `GL_TEXTURE_2D_ARRAY` 16×16×256 com 5 níveis (16, 8, 4, 2, 1), gerados na CPU por média 2×2 arredondada.
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
- **Montagem:** `rzCreateObject` recebe só a quantidade de vértices; cada polígono entra com uma chamada a `rzAddObjectPolygon`, que copia os índices (a lista é do chamador). As posições chegam por `rzUpdateObjectVertices`, antes ou depois dos polígonos; o objeto só é desenhado (e só serve de alvo da câmera) depois da primeira.
- **Polígonos:** convexos, índices `uint16`. Se o último índice repetir o primeiro (fechamento do legado), ele é descartado. Com menos de 3 vértices, o polígono é ignorado; com índice fora do objeto, dá `RZ_ERR_INVALID_ARG` e nada é acrescentado. Até 65535 polígonos por objeto. Triangulados em leque.
- **Cor e luz:** todo polígono é texturizado; a cor é a textura × a luz flat do polígono (sem a atenuação do chão), de um lado só: `0,3 + 0,7 · max(0, n·L)`, com n = −(normal de Newell), porque no sentido do legado (horário visto de fora) a de Newell aponta para dentro. Face de costas para a luz fica só com o ambiente.
- **Textura (uma por objeto):** `rzLoadObjectTexture` lê um PCX de 8 bits (`src/rz_texture.cpp`).
  - Quadrada, W×W: W é a maior potência de 2 que cabe na largura (mínimo 256; máximo 4096 ou o limite do driver). O que sobra à direita é descartado; na vertical, corta em W linhas ou completa embaixo repetindo a última linha.
  - UVs (`rzAddObjectTexturedPolygon`): um par de `float` por índice, de 0 a 1, os dois na escala da largura (v = 1 é a linha W). (0, 0) é o canto superior esquerdo; fora de [0, 1], repete a borda. Com fechamento, o último par é descartado junto.
  - Sem textura carregada, ou se a carga falhar, usa a **textura fallback**: xadrez magenta 256×256 gerado na criação do contexto, ou um PCX de `rzLoadFallbackTexture` (mesmas regras de tamanho; vale para todos os objetos, inclusive os já criados; se falhar, a atual continua; `NULL` volta ao xadrez).
  - **Cor sólida (gambiarra da paleta):** as últimas 16 linhas de toda textura de objeto (o padding) viram 256 bloquinhos 4×4, um por cor da paleta do PCX, da esquerda para a direita e de cima para baixo (W/4 blocos por linha de blocos). `rzAddObjectPolygon(..., paletteIndex)` põe os três cantos de cada triângulo no centro do bloco da cor: UV constante, derivada zero, sempre o nível 0 do mipmap, então a cor sai exata em qualquer filtro. O que a imagem tiver nessas linhas é sobrescrito; UVs de `rzAddObjectTexturedPolygon` devem ficar acima delas. O UV do bloco depende de W, então é calculado no envio ao VBO (que é refeito quando a textura do objeto, ou a fallback, muda de tamanho). No xadrez gerado (sem paleta), a faixa continua xadrez.
  - `rzSetObjectColor` saiu: a cor sólida vem da paleta.
  - Mipmaps na CPU (média 2×2) e os mesmos filtros do chão (`rzSetTextureFilter`), inclusive o dither, que usa o tamanho da textura no shader.
- **Na GPU:** um VBO por objeto (posição, cor e UV; 24 bytes por vértice), realocado a cada polígono acrescentado. Update, cor e polígono novo só marcam o objeto como alterado; o VBO é reenviado (sem alocar) no `rzRender` seguinte. Um `glDrawArrays` por objeto visível.
- **Culling:** fixo, na convenção do legado: vista de fora, a face está em sentido horário; as faces em sentido anti-horário na tela são descartadas (o antigo `RZ_CULL_CCW`, validado no legado; `rzSetObjectCulling` saiu).
- **Ids:** são índices num `std::vector`; os slots livres são reaproveitados.

## 9. Câmera

Tudo na CPU, uma vez por `rzRender`, gerando a matriz view-projection. A projeção é perspectiva, com FOV vertical de 60° e profundidade no estilo GL.

O modo normal é a perseguição (9.2): o host sempre define um alvo.

### 9.1 Visão geral (fallback, sem alvo)

- **Quando:** sem alvo (o padrão, `id < 0`) ou com o objeto-alvo destruído. Serve para testes, como fallback e como base de um futuro modo de câmera livre.
- **Enquadramento:** olha para o centro do terreno pelo lado +z, com inclinação fixa de 50° (`kOverviewPitchDegrees`), na distância `R · focal` (R é o raio da esfera envolvente, e focal é o do eixo de FOV menor).

### 9.2 Perseguição "na corda" (`rzSetCameraTarget(id, vertex)`)

- **Alvo:** o vértice `vertex` do objeto `id`, que não precisa estar em polígono. A câmera olha para ele (lookAt), suavizado.
- **Alvo suavizado:** a cada frame, o ponto olhado anda 50% do caminho até o vértice na horizontal (`kFollowLookXZ`) e 10% na vertical (`kFollowLookY`). Os pulos do alvo em terreno esburacado não fazem a câmera tremer; o atraso na horizontal fica em cerca de um frame de movimento. É esse ponto suavizado que puxa a corda e define a altura da câmera.
- **Movimento horizontal:**
  - além do comprimento da corda (padrão 3 tiles), a câmera é puxada;
  - abaixo da metade dele, recua;
  - entre os dois, a corda fica frouxa.
- **Altura:** `height` acima do alvo (padrão 1 tile).
- **Suavidade:** os dois movimentos são amortecidos por `stiffness` (padrão 0.08).
- **Chão:** a altura desejada respeita uma folga de 0,5 tile sobre o terreno, com piso duro de 0,1. A subida é amortecida e mais rápida que a descida.
- **Mudança na corda:** a distância horizontal muda na hora, na mesma proporção, para o zoom responder no mesmo frame.
- **Início:** a câmera começa já em repouso, no lado +z do alvo. Quando o alvo anda, a corda a leva para trás dele.
- **Saída:** `id < 0`, ou o objeto destruído, vai para a visão geral. Ao voltar a um alvo, a câmera recomeça do lado +z.

### 9.3 Planos near/far (os dois modos)

`near = dist(câmera, centro) − R` e `far = dist(câmera, centro) + R`. O near é limitado a no máximo metade da distância ao ponto observado e a no mínimo 0.002·R. Seguindo um alvo (com neblina), o far fica em no máximo 80 tiles × 1,02.

### 9.4 Neblina por distância

Sem API por enquanto (constantes `kFogStart`, `kFogEnd` em `rz_internal.h`).

- **Faixa:** limpa até 40 tiles; de 40 a 80, `smoothstep`; além de 80, só a cor de fundo (`rzSetBackgroundColor`). (Primeiro foi 80–130; 40–80 ficou melhor nos testes do usuário.)
- **Distância:** 3D, do pixel até o olho, calculada por pixel nos dois shaders (terreno e objetos).
- **Só seguindo um alvo.** Na visão geral não há neblina (a câmera fica longe demais).
- **Cortes:** pixel só de neblina sai direto, sem textura nem sombra (as derivadas da textura são calculadas antes desse desvio; a leitura usa `textureGrad`). Objetos com a esfera envolvente toda além de 80 + 10 tiles não são enviados nem desenhados, na tela e nas sombras.

## 10. Frame (`rzRender`)

1. Atualiza a câmera e monta a view-projection e a matriz da luz (sombra).
2. Reenvia os VBOs de objetos alterados.
3. Sombras (10.1): refaz os mapas que precisam (terreno só se mudou; objetos próximos se algo mudou; alvo sempre).
4. Faz bind do FBO (offscreen) ou do framebuffer padrão (janela) e limpa com a cor de fundo (`rzSetBackgroundColor`, padrão 32, 40, 48), com profundidade em `GL_LESS`.
5. Desenha o terreno em um draw: textura se houver atlas e mapa de blocos, senão as cores flat.
6. Desenha os objetos.
7. Faz `SwapBuffers` (janela) ou `glReadPixels` para o buffer do host (offscreen).

Sem heightmap, só limpa e apresenta.

### 10.1 Sombras (três shadow maps)

Sem API por enquanto: tudo fixo em constantes (`rz_internal.h`); o código fica em `src/rz_shadow.cpp`.

- **Luz:** a mesma direcional fixa da iluminação flat (`lightDirection`). Os três mapas usam a mesma view da luz (fixa, olhando para o centro do terreno) e projeções ortográficas diferentes.

| Mapa | Projeta | Caixa | Tamanho | Refeito |
|---|---|---|---|---|
| 0 terreno | só o terreno | terreno inteiro | 4096 (~0,09 tile/texel) | só quando o terreno muda (`buildTerrainMesh` marca `terrainShadowDirty`) |
| 1 próximos | objetos, **menos o alvo da câmera** | 96 × 96 tiles, centrada 32 tiles à frente do olho (até o fim da neblina) | 2048 (~0,047 tile/texel) | todo frame |
| 2 alvo | só o objeto seguido | esfera do objeto + 0,25 tile, lado em passos de 0,25 tile | 256 (~0,008 tile/texel num carro) | todo frame (desligado na visão geral) |

Tamanhos limitados a `GL_MAX_TEXTURE_SIZE` (o GL 3.3 só garante 1024). As resoluções são próximas de propósito: o mapa 2 bem mais nítido que os outros destoava. Memória: ~64 + 16 + 0,25 MB.

- **Combinação:** iluminado = mínimo dos três; fora da caixa de um mapa, ele não sombreia. O alvo fica fora do mapa 1 para a sombra grossa dele não vazar em volta da fina do mapa 2.
- **Quem recebe:** terreno e objetos, dos três mapas (o carro entra na sombra do morro pelo mapa 0).
- **Fora de alcance:** objetos fora da caixa do mapa 1 ou além da neblina não projetam sombra. Na visão geral, o mapa 1 cobre o terreno inteiro.
- **Ressalva:** o mapa 1 não inclui o terreno; o relevo perto do carro faz sombra só pela resolução do mapa 0. Se ficar serrilhado demais, dá para desenhar no mapa 1 o pedaço de terreno da caixa (comentário em `rz_shadow.cpp`).
- **Profundidade:** os três cobrem a esfera do terreno com folga (receptores dentro da faixa), 24 bits, `sampler2DShadow` com PCF 2×2 (`GL_LINEAR`), `glPolygonOffset(2, 4)`, sem culling. As caixas andam em passos inteiros de texels, para a sombra não tremer.
- **Efeito:** na sombra, a luz flat cai para o ambiente (0,3) nas cores flat do terreno e nos objetos; no chão texturizado, a cor é multiplicada por 0,6 (`kShadowTexturedDim`).
- **Custo (llvmpipe, `rz_test` 800×450, 120 frames):** ~40 ms/frame com a neblina (~37 sem ela, com a caixa do mapa 1 menor); redesenhando o terreno no mapa de sombra todo frame, ~61 ms.

## 11. Testes

- **`test/rz_test.cpp`:**
  - linka o núcleo estático (`RZ_STATIC`) e renderiza offscreen;
  - grava `.ppm`; `-c 0` testa a visão geral;
  - `-x 0` não carrega texturas nos objetos (tudo cai no fallback) (com ela ligada, grava `rz_wall.pcx` e `rz_car.pcx` na pasta de saída; o cubo pede um arquivo inexistente e mostra o fallback);
  - `-a atlas.pcx` usa um atlas próprio; sem ele, grava o procedural em `atlas.pcx` na pasta de saída e carrega de lá;
  - registra hashes FNV-1a por frame. Eles só são comparáveis na mesma máquina e driver, então não há regressão bit a bit entre GPUs.
- **`test/rz_testdata.h`:** dados procedurais.
  - Ilha, atlas e mapa de blocos; `rztdWritePcx` grava o atlas como PCX para o teste passar por `rzLoadTileAtlas`.
  - Casas e torres.
  - Um veículo de ~0,85 tile que segue o terreno.
  - Percurso automático.
- **`test/rz_viewer.c`:** aplicação Win32 que usa `rzCreateWindow`. Uso: `rz_viewer.exe [heightmap.raw] [atlas.pcx]`, em qualquer ordem; sem PCX, grava o atlas procedural em `%TEMP%\rz_atlas_teste.pcx`.

  | Tecla | Ação |
  |---|---|
  | WASD | Dirige o carro |
  | C | Câmera segue o carro / visão geral |
  | T | Liga/desliga as texturas |
  | F | Alterna o filtro |
  | PgUp / PgDn | Comprimento da corda |
  | ↑ / ↓ | Altura |

- **Linux:** com `build_linux.sh`, roda no llvmpipe via EGL.

## 12. Roadmap e decisões em aberto

- **`rzUpdateHeightmap(ctx, data, x, y, w, h)`:** atualizaria só a região alterada da malha (`glBufferSubData` dos quads com col em [x−1, x+w−1] e row em [y−1, y+h−1], limitados a [0, 254]).
- **Sombras:** APIs de configuração (tamanho do mapa, caixa, força) e otimizações (só redesenhar quando a luz/caixa muda, terreno simplificado no passe da sombra, cascatas). Iluminação por pixel.
- **Terreno em blocos:** reorganizar a malha em blocos (ex.: 16×16 quads) e desenhar só os que estão dentro da neblina e do campo de visão; o maior ganho possível no llvmpipe.
- **Overlay/HUD:** para o legado desenhar por cima da janela GL.
- **Desempenho em CPU fraca:** no Allwinner D1, via llvmpipe, os 130 mil triângulos pequenos dominam. O próximo passo seria descarte de blocos do terreno fora do frustum e LOD por blocos.
- **Escolha da diagonal do quad pela altura dos cantos**, de forma determinística.
- **Valores provisórios:** paleta, direção da luz, ambient, força do sombreamento das texturas, FOV, inclinação da visão geral.
