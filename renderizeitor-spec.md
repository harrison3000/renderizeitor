# Renderizeitor — Especificação v0.2 (OpenGL)

## 1. Visão geral

Renderizeitor é um renderizador 3D em OpenGL 3.3 core, escrito em C++23 no estilo "C com classes". É uma biblioteca estática com interface C, linkada direto no host.

Ele renderiza um terreno a partir de um heightmap 256×256, com texturas por bloco vindas de um atlas paletizado. Também desenha objetos poligonais que o host atualiza, e a câmera persegue um vértice-alvo.

Há dois modos de saída:

- janela: adota um contexto OpenGL que o host já criou (ex.: SDL2) e desenha direto nele;
- offscreen, com cópia para um buffer RGBQUAD do host.

## 2. Plataforma e build

| Item | Valor |
|---|---|
| Alvo | Linux, x86-64, GCC (C++23) |
| GPU | OpenGL 3.3 core. Offscreen via EGL surfaceless (Mesa/llvmpipe); janela via SDL2 (contexto do host) |
| Flags | `-std=c++23 -O2 -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Wdouble-promotion` |
| Link | biblioteca estática `librenderizeitor_static.a`; `rz_test` liga com `-ldl`; `rz_viewer` com `pkg-config sdl2` + `-lstdc++ -ldl -lm` |
| Build | `build_linux.sh` ou `make` (alvos: `lib`, `test`, `viewer`, `check`) |

Suporte a Windows (DLL de 32 bits, WGL/GDI, MinGW32) e o carregamento como plugin foram removidos.

## 3. Dependências e estilo

- **Única dependência externa é o SDL2, e só no `rz_viewer` (janela e entrada).** O núcleo não usa bibliotecas de terceiros nem headers de GL do sistema: `rz_gl.h/.cpp` declara o subconjunto do GL 3.3 usado e o carrega por X-macro, resolvendo os ponteiros via `libGL` (GLX/`dlsym`) ou EGL.
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
- **Chamadas:** nenhum struct passado por valor. As funções devolvem `int32_t` com um código `RZ_*`.
- **Memória:** nunca troca de dono na fronteira. A biblioteca libera o que alocou em `rzDestroy`.
- **Thread:** o contexto GL pertence à thread que chamou `rzCreate*`. Todas as chamadas do contexto devem vir dela, e no modo janela ela é a thread que criou o contexto e trata o laço de frames.
- **Alocação:** só nas funções de carga (heightmap, atlas, mapa de blocos, `rzCreateObject`, `rzAddObjectPolygon`). `rzRender` e `rzUpdateObjectVertices` não alocam (os vetores já têm o tamanho final).

### 4.2 Funções

| Grupo | Funções |
|---|---|
| Contexto | `rzCreate(w, h, pixels)` offscreen; `rzCreateCurrent(w, h)` adota o contexto do host (janela); `rzDestroy` |
| Terreno | `rzSetHeightmap` (256×256; guarda o ponteiro); `rzSetTerrainScale(cellSize, heightScale)`; `rzUpdateTerrain()` (relê os buffers e refaz só o que mudou) |
| Texturas | `rzLoadTileAtlas(caminhoPcx)`; `rzSetTileMap` (256×256, NULL desliga as texturas; guarda o ponteiro) |
| Objetos | `rzCreateObject(vertexCount)`, `rzAddObjectPolygon(id, indices, count, paletteIndex)`, `rzAddObjectTexturedPolygon(id, RzTexVertex* corners, count)`, `rzAddObjectTranslucentPolygon(id, indices, count, tone)`, `rzAddObjectLine(id, a, b, thickness, paletteIndex)`, `rzLoadObjectTexture(id, caminhoPcx)`, `rzLoadFallbackTexture(caminhoPcx)`, `rzUpdateObjectVertices(id, RzVertex*)`, `rzDestroyObject` |
| Rodas | `rzSetObjectWheels(id, RzWheel[4])`, `rzUpdateObjectWheels(id, steer)` |
| Sprites | `rzSetSprites(RzSprite*, count)` (array inteiro a cada chamada, até 256) |
| Câmera | `rzSetCameraTarget(id, vertex)`, `rzSetCameraFollow(distance, height, stiffness)` |
| Cena | `rzSetBackgroundColor(r, g, b)` (0..255; padrão 32, 40, 48), `rzSetFog(start, end)` (tiles; padrão 30, 65) |
| Frame | `rzRender` |
| Erros | `rzGetError(ctx, &funcao)` |

Erros:

| Código | Significado |
|---|---|
| `RZ_ERR_INVALID_ARG` | Argumento inválido |
| `RZ_ERR_SIZE` | Tamanho fora do suportado |
| `RZ_ERR_NO_MEMORY` | Reservado: hoje uma falha de alocação aborta |
| `RZ_ERR_GL` | Sem OpenGL 3.3, ou falha de contexto, shader ou janela |
| `RZ_ERR_FILE` | Arquivo não abriu ou não pôde ser lido |
| `RZ_ERR_FORMAT` | Arquivo em formato não suportado ou corrompido |

**Checagem em bloco (`rzGetError`):** as funções que devolvem código continuam devolvendo, mas também guardam no contexto o **primeiro** erro desde a última leitura e o nome da função que falhou. Assim o host pode inicializar tudo sem olhar retorno nenhum e checar uma vez no fim:

- **Leitura:** `rzGetError(ctx, &funcao)` devolve esse erro (`RZ_OK` se nada falhou), põe em `funcao` o nome da função (string estática, ou NULL) e zera o registro.
- **Contexto que não foi criado:** se `rzCreate` ou `rzCreateCurrent` falhou, o `ctx` fica NULL e as chamadas seguintes só devolvem `RZ_ERR_INVALID_ARG`, sem ter onde registrar. `rzGetError(NULL, ...)` devolve o erro da criação; sem falha de criação registrada, devolve `RZ_ERR_INVALID_ARG`.
- **Durante o jogo:** erros do `rzRender` e dos updates também entram no registro. Quem não lê o registro não é afetado.
- **Implementação:** todo `return` de erro de uma função exportada passa por `recordError(ctx, "rzNome", erro)`. As de criação usam `recordCreateError`. Função nova ou `return` novo de erro precisa seguir o mesmo padrão.

Limites:

- **Offscreen:** até 1920×1080.
- **Janela:** até 8192 por lado.
- **Atlas:** PCX de pelo menos 256×256; de um maior, só o canto 256×256 é usado.

## 5. Saída

### 5.1 Janela (contexto adotado)

- **Criação:** o host cria a janela e o contexto OpenGL 3.3 core (ex.: SDL2 com `SDL_GL_CreateContext`), deixa o contexto corrente e chama `rzCreateCurrent(w, h, &ctx)`. A plataforma (`rz_platform_egl.cpp`, modo ADOPTED) só resolve os ponteiros do GL via `libGL` (GLX/`dlsym`, com EGL de reserva para Wayland); não cria janela, não troca buffers nem destrói o contexto.
- **Desenho:** `rzRender` desenha direto no framebuffer padrão da janela (sem cópia), com o winding de frente `GL_CCW`.
- **Ritmo e apresentação:** o host desliga o v-sync (`SDL_GL_SetSwapInterval(0)`), controla o ritmo dos frames e, depois de `rzRender`, apresenta com `SDL_GL_SwapWindow`.
- **Entrada e ciclo de vida:** a janela e o contexto são do host; a entrada e o laço de eventos também. O host chama `rzDestroy` ao sair (que não destrói a janela nem o contexto do host) e depois libera o contexto e a janela pelo SDL.

### 5.2 Offscreen

- **Destino:** o desenho vai para um FBO com cor RGBA8 e profundidade de 24 bits.
- **Cópia:** `glReadPixels(GL_BGRA)` copia direto para o buffer do host, que é RGBQUAD `0x00RRGGBB`, top-down, com stride igual à largura e byte reservado 0.
- **Orientação:** a projeção espelha o Y, então a linha 0 lida já é a de cima. Com o espelhamento, o winding de frente vira `GL_CW`.
- **Contexto:** EGL surfaceless (sem janela); o renderer desenha num FBO.

## 6. Mundo e unidades

- **Unidade:** 1 tile, o tamanho de um quad da grade com `cellSize = 1`.
- **Eixos internos:** y para cima. O vértice da grade (col, row) fica em `(col·cellSize, h·heightScale, row·cellSize)`.
- **Escala padrão:** `heightScale = 16/255`, ou seja, byte 0 é altura 0 e byte 255 é 16 tiles. O mundo mede 255 × 255 × 16.
- **Objetos:** chegam no eixo do legado, (x, y, z) = (coluna, linha, altura), z para cima, e são convertidos internamente para (x, z, y). Fixo (`rzSetObjectAxes` saiu).

## 7. Terreno

### 7.1 Malha

- **Tamanho:** 255×255 quads, ou 130.050 triângulos. Cada quad é dividido pela diagonal (c+1, r)–(c, r+1), igual ao legado (cantos 1 2 / 3 4 → triângulos 1 3 2 e 2 3 4):
  - A = (c, r), (c, r+1), (c+1, r)
  - B = (c+1, r), (c, r+1), (c+1, r+1)
- **Montagem:** a malha é montada na CPU (`buildTerrainMesh`) e enviada a um VBO estático, sem índices. São 20 bytes por vértice:
  - posição float;
  - cor flat do triângulo;
  - (u, v) do canto do quad;
  - camada do atlas.
- **Por que sem índices:** a cor e o bloco são do triângulo e do quad, não do ponto da grade.
- **Quando é remontada:** inteira (com a borda e a sombra do relevo) em `rzSetHeightmap` e `rzSetTerrainScale`; `rzSetTileMap` só refaz os blocos dos quads e da continuação (sem relevo nem sombra).
- **Mudança durante o jogo (`rzUpdateTerrain`):** `rzSetHeightmap`/`rzSetTileMap` guardam o ponteiro do host (tem que continuar válido); o host altera os próprios arrays e chama `rzUpdateTerrain`, que compara com as cópias (2 × 64 KB) e trata ponto a ponto (no jogo são raros e pequenos): altura → refaz e reenvia os 4 quads em volta, a cópia estendida (câmera) e a sombra do relevo só ali (scissor na cascata 2); bloco → refaz o quad. Ponto na borda do mapa: só o y das vértices do terreno de fora que estão em cima dele acompanha (sem fresta); o resto de fora, cores e parede não são recalculados (imperfeito, aceito; `rzSetHeightmap` refaz tudo). Bloco a menos de 8 quads da borda refaz a malha da continuação. Mudanças no interior: resultado idêntico pixel a pixel ao de refazer tudo (testado). Custo (llvmpipe): ~0,2 ms por ponto, contra ~30 ms + sombra inteira de `rzSetHeightmap` + `rzSetTileMap`. (Uma versão que recalculava a vizinhança de fora com exatidão ficou no branch `super_complicated_border_update`.)
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
- **Transparência (PCX, chão e objetos):** o índice 255 da paleta vira alfa 0 (o resto, alfa 255) e o shader descarta o pixel com alfa < 0,5: o chão e os objetos ficam com buraco ali (o que está atrás aparece; no chão, o fundo/neblina). A cor sólida 255 de `rzAddObjectPolygon` também é transparente. Mipmaps com média ponderada pelo alfa; o RGB dos texels transparentes é preenchido com a cor dos opacos vizinhos em todo nível (`fillTransparent`), para o anisotrópico e a mistura entre níveis não puxarem a cor do 255 nas bordas. A sombra ignora a transparência (os buracos ainda projetam sombra; de propósito: shader mais barato). A textura fallback gerada é opaca.
- **Na GPU:** `GL_TEXTURE_2D_ARRAY` 16×16×256 com 5 níveis (16, 8, 4, 2, 1), gerados na CPU por média 2×2 arredondada.
- **Filtro (fixo, sem API):** ampliação nearest; redução nearest dentro do nível e mistura linear entre níveis (`GL_NEAREST_MIPMAP_LINEAR`, o antigo `RZ_FILTER_MIP_LINEAR`). `rzSetTextureFilter` e os outros filtros (nearest, mipmap, mip+dither, trilinear) saíram para enxugar a API. Mais filtro anisotrópico 4x (`kAnisotropy`, ou o máximo do driver se for menor) sempre que o driver tem `GL_EXT/ARB_texture_filter_anisotropic`, detectado na criação do contexto; sem a extensão, só o filtro normal. Na prática o driver filtra a redução (o meio-campo fica mais liso, menos pixelado); de perto continua nearest. Em software custa caro (llvmpipe: ~+25–40% no frame): lá, neblina mais curta (`rzSetFog`). A textura de ruído dos sprites fica sem.

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
  - Mipmaps na CPU (média 2×2) e o mesmo filtro fixo do chão.
- **Vidro (`rzAddObjectTranslucentPolygon`, `src/rz_glass.cpp`):** polígonos translúcidos em 16 tons de cinza (`tone` 0..15, cinza = tom/15).
  - Filtro multiplicativo (o que está atrás × cinza) + brilho especular embaçado (Blinn-Phong, expoente 12, força 0,45; `kGlassShininess`, `kGlassSpecular`) da luz direcional, que some na sombra (cascatas) e de costas para a luz.
  - Um passe, `glBlendFunc(GL_ONE, GL_SRC_ALPHA)` com saída (brilho, cinza): destino = brilho + destino × cinza. Não precisa de ordenação (o filtro comuta; dois vidros sobrepostos em ordens diferentes diferem em no máximo 1 nível por arredondamento).
  - Depois dos objetos opacos e antes da parede de limite; sem gravar profundidade nem alfa de destino; mesmo culling dos objetos (só a face de fora); não projeta sombra; neblina (filtro → 1, brilho → 0). VBO próprio por objeto, criado no primeiro vidro.
- **Rodas (`rzSetObjectWheels`, `rzUpdateObjectWheels`, `src/rz_wheels.cpp`):** 4 por objeto, por enquanto cilindros pretos finos (12 segmentos, largura 0,4 × diâmetro, aspecto 12:30; `kWheelSegments`, `kWheelWidth`, `kWheelColor`).
  - Definição: 4 índices de vértice (centro do cubo), 4 flags dianteira/traseira (`uint8_t`, exatamente 2 dianteiras) e 4 diâmetros em tiles (> 0). Chamar de novo redefine e zera o esterço.
  - Referencial do carro tirado dos 4 cubos: frente = meio das dianteiras − meio das traseiras; eixo lateral = diferença dentro de cada par; "cima" = perpendicular aos dois, sempre com y ≥ 0. Vale para qualquer ordem dos índices.
  - Esterço (`rzUpdateObjectWheels`): um `float` em radianos para as duas dianteiras, positivo = à esquerda (anti-horário visto de cima), aplicado em volta do "cima" do carro; traseiras sempre retas. Só refaz a malha se o ângulo mudar.
  - Malha refeita na CPU quando o esterço ou os vértices mudam; desenhada com o programa do chão sem textura (cor chapada com luz, sombra e neblina), sem culling; projeta sombra (cascatas 0 e 1).
- **Linhas (`rzAddObjectLine`):** entre dois vértices do objeto, exibidas como uma barra de seção quadrada de lado `thickness` (tiles), cor sólida `paletteIndex` da paleta da textura do objeto (bloquinho da faixa de amostras, como `rzAddObjectPolygon`). Geradas a cada envio do VBO do próprio objeto, depois dos polígonos (4 lados + 2 tampas, 36 vértices), então ganham luz flat por face, sombra, neblina e o mesmo culling (faces no sentido do legado). A seção é orientada pelo "cima" do mundo (não gira com o objeto em volta da linha). Fase de carga (realoca o VBO); a == b é ignorada. Exemplo: antena do carro de teste (`rztdAddVehicleAntenna`).
- **Na GPU:** um VBO por objeto (posição, cor e UV; 24 bytes por vértice), realocado a cada polígono acrescentado. Update, cor e polígono novo só marcam o objeto como alterado; o VBO é reenviado (sem alocar) no `rzRender` seguinte. Um `glDrawArrays` por objeto visível.
- **Culling:** fixo, na convenção do legado: vista de fora, a face está em sentido horário; as faces em sentido anti-horário na tela são descartadas (o antigo `RZ_CULL_CCW`, validado no legado; `rzSetObjectCulling` saiu).
- **Ids:** são índices num `std::vector`; os slots livres são reaproveitados.
- **Structs da API:** `RzVertex` (x, y, z 8.24; 12 bytes), `RzTexVertex` (u, v, índice; 12), `RzWheel` (diâmetro, vértice do cubo, dianteira; 8), `RzSprite` (x, y, z 8.24, diâmetro, cor; 20). Só por ponteiro, layout fixo do maior para o menor com padding explícito, tamanho conferido em compilação nos dois cabeçalhos.

### 8.1 Sprites (`rzSetSprites`, `src/rz_sprites.cpp`)

- **Sem id:** o legado guarda um array compacto (quem morre faz os seguintes andarem), então cada chamada manda o array inteiro e substitui o anterior; o renderer não guarda estado por sprite. Até 256 (`kMaxSprites`, o máximo validado no legado; mais é `RZ_ERR_SIZE`); size ≤ 0 ou não finito é `RZ_ERR_INVALID_ARG` (nada muda). Não aloca: feita para todo frame.
- **Forma:** quad virado para a câmera, diâmetro `size` tiles, com o alfa de um ruído "spray do Paint" (2 cliques de pontinhos perto do centro) em 8 variações numa textura array 64×64 gerada na criação. O quadro de cada sprite avança a cada 4 `rzRender`, deslocado pela posição no array. Ampliação nearest (pontinhos de perto), redução trilinear.
- **Cor:** índice na paleta do jogo, a do PCX de `rzLoadTileAtlas` (antes dele, cinza = índice), vezes a luz de uma face virada para cima (como o chão plano). Fumaça, detrito ou água: só a cor muda.
- **Desenho:** depois do vidro (fumaça na frente de um vidro não fica tingida) e antes da parede; blending alfa comum, ordenado de trás para frente na CPU; sem gravar profundidade nem alfa de destino; sem culling; com neblina (os que estão todos além do fim nem entram). Não projeta nem recebe sombra.
- **Exemplo:** `rztdStepParticles` (`rz_testdata.h`) imita o legado (array compacto): fumaça saindo do meio das rodas traseiras do carro (sobe, cresce, clareia) e detritos marrons que caem; no `rz_test` e no viewer.

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

`near = dist(câmera, centro) − R` e `far = dist(câmera, centro) + R`. O near é limitado a no máximo metade da distância ao ponto observado e a no mínimo 0.002·R. Seguindo um alvo (com neblina), o far fica em no máximo o fim da neblina × 1,02.

### 9.4 Neblina por distância

`rzSetFog(start, end)` em tiles; padrão 30 e 65 (`kFogStartDefault`, `kFogEndDefault`). Custo desprezível (só guarda os valores; uniforms, far plane e cortes são lidos a cada `rzRender`): pode mudar a qualquer momento, até todo frame. Válido: 0 ≤ start < end ≤ 120 (`kFogMaxEnd`; a continuação do terreno vai até 128).

- **Faixa:** limpa até start; de start a end, `smoothstep`; além de end, só a cor de fundo (`rzSetBackgroundColor`). (Histórico do padrão: 80–130, 40–80, agora 30–65.)
- **Distância:** 3D, do pixel até o olho, calculada por pixel nos dois shaders (terreno e objetos).
- **Só seguindo um alvo.** Na visão geral não há neblina (a câmera fica longe demais).
- **Cortes:** pixel só de neblina sai direto, sem textura nem sombra (as derivadas da textura são calculadas antes desse desvio; a leitura usa `textureGrad`). Objetos com a esfera envolvente toda além de end + 10 tiles não são enviados nem desenhados, na tela e nas sombras.

### 9.5 Borda do mundo (continuação e parede)

Sem API; constantes `kSkirt*`/`kBorder*` em `rz_internal.h`, código em `src/rz_border.cpp`. Montada na carga, junto com a malha do terreno (heightmap, escala ou mapa de blocos novos).

- **Continuação:** terreno gerado até 88 tiles além de cada borda (o fim da neblina + 8).
  - Altura: a da borda (ponto mais próximo do mapa) indo, ao longo de 24 tiles, para a média das bordas + ruído de valor (duas oitavas, períodos 32 e 12). Sem degrau na emenda.
  - Blocos de textura (`skirtTile`): em geral, a célula continua o bloco do quad da borda logo "na frente" (um rio continua rio), mas o ponto de cópia serpenteia ao longo da borda: deslocamento por ruído de valor suave em (posição ao longo, distância para fora), período 24, amplitude 0,35 × distância até 12 tiles. Vizinhas se deslocam juntas, então as faixas seguem coesas e fazem curvas. 12 % das células sorteiam um bloco numa janela de 8 tiles perto da borda, para quebrar a repetição. Nos cantos, o quad do canto. A altura não entra.
  - Malha em faixas (`kSkirtBands`): células de 1 tile até 8 tiles da borda, de 2 até 16, de 4 até 32 e de 16 até 128 (`kSkirtExtent`; 128 para as células de 16 fecharem alinhadas; a neblina vai até 120). Nas linhas entre faixas, os pontos intermediários ficam na reta entre os cantos das células de fora (sem frestas). ~26 mil triângulos, em 4 regiões (N, L, S, O); só as regiões a menos do fim da neblina do olho são desenhadas.
  - Só seguindo um alvo (com neblina). Projeta sombra (na cascata 2 e, perto, nas 0 e 1). A câmera usa as mesmas alturas fora do mapa.
- **Parede de limite:** em cima das quatro bordas, do chão (−0,5) até 6 tiles acima. Fundo vermelho translúcido escuro (alfa 0,20) com círculos vermelhos (alfa 0,85) de borda branca e um X vazado no meio (mostra o fundo), um por célula de 2 tiles, com 1 tile de diâmetro (o espaço entre vizinhos é do tamanho de um círculo). O padrão é preso ao mundo (ao longo da parede e na altura do mundo, `vWorld.y`): a parede sobe e desce com o terreno, o desenho não. Aparece só perto do alvo: alfa × (1 − smoothstep(6, 25, distância horizontal do alvo ao ponto da parede)) (`kBorderFadeNear`, `kBorderFadeFar`). Desenhada depois do opaco, com blending, sem gravar profundidade nem alfa de destino; com neblina; não projeta nem recebe sombra.

## 10. Frame (`rzRender`)

1. Atualiza a câmera e monta a view-projection e a matriz da luz (sombra).
2. Reenvia os VBOs de objetos alterados.
3. Sombras (10.1): refaz as cascatas 0 e 1; a 2 só se o olho andou 10 tiles, a neblina mudou ou o terreno mudou.
4. Faz bind do FBO (offscreen) ou do framebuffer padrão (janela) e limpa com a cor de fundo (`rzSetBackgroundColor`, padrão 32, 40, 48), com profundidade em `GL_LESS`.
5. Desenha o terreno em um draw: textura se houver atlas e mapa de blocos, senão as cores flat. Seguindo um alvo, também a continuação (9.5).
6. Desenha os objetos e, por último, a parede de limite (semitransparente).
7. Janela: deixa o frame pronto no framebuffer (quem apresenta é o host, com `SDL_GL_SwapWindow`). Offscreen: `glReadPixels` para o buffer do host.

Sem heightmap, só limpa e apresenta.

### 10.1 Sombras (cascatas)

Sem API: tudo fixo em constantes (`kCascade*`, `rz_internal.h`); o código fica em `src/rz_shadow.cpp`.

- **Luz:** a mesma direcional fixa da iluminação flat (`lightDirection`). As três cascatas usam a mesma view da luz (fixa, olhando para o centro do terreno) e projeções ortográficas diferentes.

| Cascata | Distância do olho | Projeta | Caixa | Tamanho | Refeita |
|---|---|---|---|---|---|
| 0 | 0 – 6 tiles | terreno + objetos | esfera da fatia do frustum (meia-largura ~7 tiles) | 512 (~0,028 tile/texel) | todo frame |
| 1 | 6 – 20 | terreno + objetos | esfera da fatia (~23,5 tiles) | 1024 (~0,046) | todo frame |
| 2 | 20 – fim da neblina | **só terreno** | quadrado em volta do olho, meia-largura = fim da neblina + 10 + 4 tiles | 2048 (~0,077 com neblina 65) | quando o olho anda 10 tiles, a neblina muda de tamanho, ou o terreno muda (`rzUpdateTerrain`: só o retângulo alterado, com scissor; terreno todo novo: no mesmo centro) |

- **Escolha no shader:** pela distância 3D ao olho (`uCascade`), com 1 tile de transição antes de cada divisa misturando as duas vizinhas. Fora da caixa de uma cascata, ela não sombreia.
- **Estabilidade:** a esfera da fatia só depende do FOV e dos limites (não muda quando a câmera gira); as caixas andam em passos inteiros de texel.
- **Custo:** das cascatas 0 e 1, só os quads do terreno que podem cair na caixa são desenhados (um `glDrawArrays` por linha, colunas recortadas pela altura mínima/máxima do terreno seguida na direção da luz); a continuação só se a caixa passa da borda.
- **Objetos além de 20 tiles não projetam sombra** (a cascata 2 só tem terreno); aceito, a neblina começa logo depois. Recebem a sombra do relevo normalmente.
- **Visão geral (sem alvo):** só a cascata 2, cobrindo o terreno inteiro, com terreno e objetos, refeita todo frame.
- **Comparação com os três mapas antigos (terreno 4096 inteiro, objetos 2048 numa caixa de 96 tiles, alvo 256):** sombra do relevo perto bem mais nítida; a do carro, com a cascata 0 a 512, fica mais borrada que a do antigo mapa do alvo (0,006 tile/texel); 1024 quase iguala e 2048 iguala (llvmpipe: +2,5 / +6 ms por frame). `kCascadeNearSize`.
- **Profundidade:** todas cobrem a esfera do terreno com folga (receptores dentro da faixa), 24 bits, `sampler2DShadow` com PCF 2×2 (`GL_LINEAR`), `glPolygonOffset(2, 4)`, sem culling. Tamanhos limitados a `GL_MAX_TEXTURE_SIZE`. A transparência do índice 255 é ignorada no passe de sombra (de propósito: shader mais barato).
- **Efeito:** na sombra, a luz flat cai para 0,24 (`kShadowLight`, 80 % do ambiente) nas cores flat do terreno e nos objetos; no chão texturizado, a cor é multiplicada por 0,48 (`kShadowTexturedDim`).
- **Custo (llvmpipe, `rz_test` 1280×720, 60 frames):** ~64 ms/frame com as cascatas (com os três mapas antigos, ~62).

## 11. Testes

- **`test/rz_test.cpp`:**
  - linka o núcleo estático e renderiza offscreen;
  - grava `.ppm`; `-c 0` testa a visão geral;
  - `-x 0` não carrega texturas nos objetos (tudo cai no fallback) (com ela ligada, grava `rz_wall.pcx` e `rz_car.pcx` na pasta de saída; o cubo pede um arquivo inexistente e mostra o fallback);
  - `-a atlas.pcx` usa um atlas próprio; sem ele, grava o procedural em `atlas.pcx` na pasta de saída e carrega de lá;
  - registra hashes FNV-1a por frame. Eles só são comparáveis na mesma máquina e driver, então não há regressão bit a bit entre GPUs.
- **`test/rz_testdata.h`:** dados procedurais.
  - Ilha, atlas e mapa de blocos; `rztdWritePcx` grava o atlas como PCX para o teste passar por `rzLoadTileAtlas`.
  - Casas e torres.
  - Um veículo de ~0,85 tile que segue o terreno.
  - Percurso automático.
- **`test/rz_viewer.c`:** aplicação SDL2 que cria a janela e o contexto OpenGL e usa `rzCreateCurrent`. Uso: `rz_viewer [heightmap.raw] [atlas.pcx]`, em qualquer ordem; sem PCX, grava o atlas procedural em `$TMPDIR/rz_atlas_teste.pcx` (padrão `/tmp`).

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

- **Bloco perto da borda no `rzUpdateTerrain` (~3 ms):** hoje um bloco alterado a menos de 8 quads da borda refaz a malha inteira da continuação (`rebuildSkirtMesh`). Otimização possível: refazer só o bloco dos triângulos de fora que copiam/sorteiam daquele quad (faixa ao longo da borda a até meandro + 8, e o quadrante do canto); uma versão disso (com índice espacial) está no branch `super_complicated_border_update`.
- **Sombras:** APIs de configuração (tamanho do mapa, caixa, força) e otimizações (só redesenhar quando a luz/caixa muda, terreno simplificado no passe da sombra, cascatas). Iluminação por pixel.
- **Terreno em blocos:** reorganizar a malha em blocos (ex.: 16×16 quads) e desenhar só os que estão dentro da neblina e do campo de visão; o maior ganho possível no llvmpipe.
- **Overlay/HUD:** para o legado desenhar por cima da janela GL.
- **Desempenho em CPU fraca:** no Allwinner D1, via llvmpipe, os 130 mil triângulos pequenos dominam. O próximo passo seria descarte de blocos do terreno fora do frustum e LOD por blocos.
- **Escolha da diagonal do quad pela altura dos cantos**, de forma determinística.
- **Valores provisórios:** paleta, direção da luz, ambient, força do sombreamento das texturas, FOV, inclinação da visão geral.
