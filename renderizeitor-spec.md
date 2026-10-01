# Renderizeitor — Especificação v0.1 (Proof of Concept)

## 1. Visão geral

Renderizeitor é um renderizador 3D por software, escrito em C++23 no estilo "C com classes", distribuído como DLL de 32 bits com interface C, para ser conectado a um código legado em C compilado com MinGW.

O objetivo desta versão é um proof of concept: receber um heightmap de 256×256, montar a malha internamente e renderizar o terreno girando, um pequeno passo a cada chamada de `rzRender`, escrevendo num framebuffer RGBQUAD fornecido pelo host. Texturas, controle de câmera pelo host e atualização do heightmap ficam fora do escopo desta versão, mas as decisões que os afetam já estão registradas (seção 13).

## 2. Plataforma e build

A plataforma é x86 em modo 32 bits, em processadores recentes, no Windows. O compilador é MinGW32 com GCC 12 ou superior (necessário para o `operator[]` com múltiplos índices). Host e plugin usam o mesmo compilador.

| Flag | Motivo |
|---|---|
| `-std=c++23` | Recursos de C++23 usados no código |
| `-O2` | Otimização padrão |
| `-msse2 -mfpmath=sse` | Float sempre em SSE2; x87 nunca é usado |
| `-ffp-contract=off` | Sem contração em FMA, resultado de float previsível |
| `-fno-exceptions -fno-rtti` | Sem exceções nem RTTI |
| `-Wall -Wextra -Wdouble-promotion` | Detectar `double` acidental |
| `-static-libgcc -static-libstdc++` (e `-static` se necessário) | Sem DLLs de runtime do MinGW |
| `-Wl,--out-implib,librenderizeitor.dll.a` | Import library para o host linkar |

Critério de aceitação do build: `objdump -p renderizeitor.dll` deve listar apenas DLLs do sistema (`KERNEL32.dll`, `msvcrt.dll`). Nenhuma `libgcc_s_*`, `libstdc++-*` ou `libwinpthread-*`.

Todas as funções exportadas são marcadas com `__attribute__((force_align_arg_pointer))` na definição. O Win32 garante apenas alinhamento de 4 bytes na pilha, o GCC assume 16 ao usar SSE, e chamadas originadas em callbacks do Windows (WndProc, timers, threads) podem chegar desalinhadas mesmo passando pelo código legado compilado com GCC.

O código é organizado em três alvos: uma biblioteca estática com o núcleo, a DLL (núcleo mais as exportações) e um executável de teste que linka o núcleo diretamente (seção 12).

## 3. Dependências

Nenhuma biblioteca de terceiros. Da biblioteca padrão são permitidos apenas headers que não trazem runtime pesado nem alocação implícita: `<cstdint>`, `<cstring>`, `<cmath>` (só `sinf`, `cosf`, `sqrtf`, `tanf`), `<bit>` e `<cstdlib>` (só para `malloc`/`free`). Containers da std, streams, strings e qualquer coisa que aloque por conta própria não são usados.

Toda alocação passa por um par interno `rzAlloc`/`rzFree` sobre `malloc`/`free`, para ser trocável no futuro.

## 4. Estilo de código

O estilo é "C com classes": structs e classes simples, funções membro quando deixam o código mais claro, sem herança virtual, sem templates elaborados, sem metaprogramação, sem ranges. Recursos novos do C++ são bem-vindos quando simplificam o código, em particular o `operator[]` com dois índices, `constexpr`/`consteval`, `std::bit_cast` e `[[assume]]`.

O framebuffer e o depth buffer são acessados por um `operator[](int row, int col)` escrito à mão sobre um buffer linear, com índice `row * width + col`. `std::mdspan` não é usado.

Todo literal de ponto flutuante tem sufixo `f`. `double` não aparece no código.

Divisão só é trocada por shift à mão quando o compilador não faz isso sozinho. Divisão inteira por constante potência de 2 fica com o compilador. O orçamento de divisões do caminho de renderização está na seção 7.4.

## 5. Interface C

### 5.1 Regras do header

O header público `renderizeitor.h` compila como C puro, pois é incluído pelo código legado. Ele usa apenas tipos de `<stdint.h>`, não usa `bool` nem referências, e declara o contexto como struct opaca. A convenção de chamada é `__cdecl` em todas as funções, com nomes sem decoração. Nenhum struct é passado por valor pela fronteira.

Memória nunca troca de dono na fronteira: o que o host aloca, o host libera; o que a DLL aloca, a DLL libera em `rzDestroy`. As funções de um mesmo contexto devem ser chamadas sempre da mesma thread; a DLL não é thread-safe por contexto.

### 5.2 Header

```c
#ifndef RENDERIZEITOR_H
#define RENDERIZEITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RZ_STATIC)
#  define RZ_API
#elif defined(RZ_BUILD_DLL)
#  define RZ_API __declspec(dllexport)
#else
#  define RZ_API __declspec(dllimport)
#endif
#define RZ_CALL __cdecl

typedef struct RzContext RzContext;

#define RZ_OK               0
#define RZ_ERR_INVALID_ARG  1   /* ponteiro nulo ou parâmetro fora de faixa */
#define RZ_ERR_SIZE         2   /* dimensão acima do limite ou não suportada */
#define RZ_ERR_NO_MEMORY    3   /* falha de alocação */

#define RZ_MAX_WIDTH   1920
#define RZ_MAX_HEIGHT  1080

/* pixels: array width*height de RGBQUAD (uint32 0x00RRGGBB), top-down,
   de posse do host, válido até rzDestroy. */
RZ_API int32_t RZ_CALL rzCreate(int32_t width, int32_t height,
                                void* pixels, RzContext** outCtx);

RZ_API void    RZ_CALL rzDestroy(RzContext* ctx);

/* Copia os dados. Nesta versão só aceita width == height == 256. */
RZ_API int32_t RZ_CALL rzSetHeightmap(RzContext* ctx, const uint8_t* data,
                                      int32_t width, int32_t height);

/* cellSize: distância entre pontos da grade; heightScale: altura por unidade do byte. */
RZ_API int32_t RZ_CALL rzSetTerrainScale(RzContext* ctx,
                                         float cellSize, float heightScale);

/* Avança a rotação, renderiza o frame no buffer do host. Não aloca. */
RZ_API int32_t RZ_CALL rzRender(RzContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
```

### 5.3 Semântica

`rzCreate` valida `1 ≤ width ≤ 1920` e `1 ≤ height ≤ 1080`, aloca o contexto e o depth buffer, e devolve o handle em `outCtx`. O ponteiro `pixels` é `void*` para o host passar um `RGBQUAD*` sem cast.

`rzSetHeightmap` copia os 64 KB do heightmap para um buffer interno e aloca e preenche as estruturas do terreno (seção 8). Pode ser chamada de novo, substituindo o terreno anterior; é uma fase em que alocar é permitido.

`rzSetTerrainScale` pode ser chamada antes ou depois de `rzSetHeightmap`. Se chamada depois, recalcula as cores pré-calculadas (a iluminação depende da escala) sem alocar. Valores padrão: `cellSize = 1.0f`, `heightScale = 0.25f` (provisórios).

`rzRender` sem heightmap carregado apenas limpa o framebuffer e retorna `RZ_OK`.

`rzDestroy` aceita ponteiro nulo e libera tudo que a DLL alocou. Nunca toca no buffer do host.

## 6. Framebuffer

O framebuffer é um array de RGBQUAD de posse do host. Em memória cada pixel tem os bytes B, G, R, reservado; como `uint32_t` em little-endian o valor é `0x00RRGGBB`. O byte reservado é sempre escrito como zero, porque algumas APIs o tratam como alfa.

A orientação é sempre top-down: o ponteiro aponta para a linha 0, que é a linha de cima da imagem. No lado GDI, o host usa `biHeight` negativo no `BITMAPINFOHEADER`. Como cada pixel tem 4 bytes, as linhas de um DIB de 32 bpp já são alinhadas a DWORD, e o stride é sempre igual à largura; não há parâmetro de stride.

A DLL não conhece GDI. Exibir o buffer (por exemplo com `SetDIBitsToDevice`) é responsabilidade do host.

## 7. Política numérica

### 7.1 Domínios

| Domínio | Representação | Onde |
|---|---|---|
| Transformações, clip space, clipping | `float` (SSE2) | Matrizes, cache de vértices, clipper |
| Coordenadas de tela | `int32_t` fixed 28.4 (4 bits de subpixel) | Setup e rasterização |
| Funções de aresta e área | `int32_t` | Setup e rasterização |
| Profundidade | 1/w em `float`, armazenado como bits `uint32_t` | Interpolação e depth buffer |
| Cor | 8 bits por canal, `uint32_t` 0x00RRGGBB | Cores pré-calculadas e framebuffer |
| Ângulo de rotação | `uint32_t`, 2^32 = uma volta | Câmera |

A passagem de float para fixed acontece em um único ponto: depois da divisão perspectiva e do viewport transform, ao arredondar a coordenada de tela multiplicada por 16. Daí em diante a rasterização é inteira e determinística.

Todo valor fixed cabe em 32 bits. Produtos de 64 bits não são necessários (seção 7.3), e divisão de 64 bits é proibida (em x86-32 ela vira chamada de biblioteca).

### 7.2 Convenções de rasterização

Os centros de pixel ficam em (x + 0,5, y + 0,5), ou seja, em subpixel `x·16 + 8`. A regra de preenchimento é top-left, implementada com bias de −1 nas funções de aresta que não são top nem left. Com isso, triângulos que compartilham arestas não deixam buracos nem pintam pixels duas vezes.

Depois do snap para 28.4, as coordenadas são clampadas para `[0, W·16] × [0, H·16]`. O clipping em float garante que os vértices estão na viewport, mas o arredondamento pode ultrapassar a borda por um subpixel; o clamp mantém a garantia da seção 7.3.

Triângulos com área com sinal menor ou igual a zero são descartados, o que cobre ao mesmo tempo backface culling e triângulos degenerados após o snap.

### 7.3 Prova de que as funções de aresta cabem em 32 bits

Com o limite rígido de 1920×1080 e 4 bits de subpixel, as coordenadas ficam em x ∈ [0, 30720] e y ∈ [0, 17280]. A função de aresta é

E(p) = (bx − ax)·(py − ay) − (by − ay)·(px − ax)

Cada produto é limitado por 30720 × 17280 = 530.841.600, então |E| ≤ 1.061.683.200, abaixo de 2^31 − 1 = 2.147.483.647. O incremento por pixel é o coeficiente multiplicado por 16, também dentro da faixa, e como os pontos avaliados estão sempre dentro da viewport o valor acumulado respeita o mesmo limite. A área com sinal (duas vezes a área) tem a mesma cota.

Essa prova depende de duas condições que não podem ser relaxadas sem refazer a conta: o limite de 1920×1080 e o clipping contra os seis planos do frustum, sem guard band.

### 7.4 Orçamento de divisões no caminho de renderização

| Estágio | Divisões permitidas |
|---|---|
| Vértice | Uma recíproca (1/w) por vértice visível |
| Triângulo | Uma recíproca (1/área) no setup, para os gradientes de profundidade |
| Pixel | Nenhuma |
| Frame | As necessárias para montar matrizes (constantes) |

Nenhuma divisão inteira no caminho de renderização. Funções de carga (`rzSetHeightmap`, `rzSetTerrainScale`) não estão sujeitas a este orçamento, mas devem continuar evitando operações caras sem necessidade.

## 8. Terreno

### 8.1 Dados e coordenadas

O heightmap tem 256×256 bytes, com o índice `data[row * 256 + col]`. `rzSetHeightmap` copia os dados; o host pode reutilizar o array logo depois.

O ponto (0, 0) da grade fica na origem do mundo. O vértice da grade (col, row) está na posição

(col · cellSize, h · heightScale, row · cellSize)

de modo que o terreno se estende para +x e +z até 255 · cellSize, com +y para cima.

### 8.2 Malha implícita

A grade tem 255×255 quads, totalizando 130.050 triângulos e 65.536 vértices. A topologia não é armazenada: os índices dos vértices saem de (col, row). Cada quad é dividido sempre pela diagonal que liga (c, r) a (c+1, r+1), gerando os triângulos

A = (c, r), (c, r+1), (c+1, r+1)
B = (c, r), (c+1, r+1), (c+1, r)

Nessa ordem, a normal geométrica `(v1 − v0) × (v2 − v0)` de um quad plano aponta para +y. O índice do triângulo é `(r · 255 + c) · 2` para A e o mesmo mais 1 para B.

### 8.3 Cores pré-calculadas

Com cor flat por triângulo e uma luz direcional fixa no mundo, a iluminação não depende da câmera. A cor final de cada triângulo é calculada em `rzSetHeightmap` (e em `rzSetTerrainScale`) e armazenada como `uint32_t`; durante o frame o triângulo só lê sua cor.

A cor base vem de uma paleta indexada pela soma das três alturas do triângulo (0 a 765), com 766 entradas, o que evita a divisão por 3 da média. A paleta é gerada a partir de faixas de altura (valores provisórios):

| Altura média | Cor base |
|---|---|
| 0 – 40 | Água, azul |
| 41 – 60 | Areia, bege |
| 61 – 150 | Grama, verde |
| 151 – 210 | Rocha, cinza-marrom |
| 211 – 255 | Neve, branco |

A intensidade é `ambient + (1 − ambient) · max(0, n · L)`, com `n` a normal da face normalizada no espaço do mundo, `L` a direção da luz normalizada, e `ambient = 0.3f` (provisório). A cor final é a cor base multiplicada pela intensidade em cada canal.

### 8.4 Atualização futura (não implementada nesta versão)

A assinatura prevista é `rzUpdateHeightmap(ctx, data, x, y, w, h)`, em que `data` aponta para o array completo de 256×256 no mesmo layout e o retângulo indica a região alterada. A função copia só a região e recalcula as cores dos triângulos cujos quads tocam os vértices alterados, ou seja, quads com col em [x − 1, x + w − 1] e row em [y − 1, y + h − 1], limitados a [0, 254]. Não aloca. O cache de vértices não precisa de tratamento, pois é recalculado a cada frame.

## 9. Câmera

### 9.1 Órbita

A câmera orbita em torno do centro do terreno, (127,5 · cellSize, 127,5 · heightScale, 127,5 · cellSize), girando em torno do eixo y. O ângulo de yaw é um `uint32_t` no contexto em que 2^32 representa uma volta completa; cada `rzRender` soma um passo constante de `1 << 22` (1024 frames por volta). O overflow faz o wrap sozinho, sem drift e sem normalização.

Para converter em radianos, o ângulo é reinterpretado como `int32_t` (faixa de −π a π) e multiplicado por 2π / 2^32. Seno e cosseno do yaw são calculados com `sinf`/`cosf` uma vez por frame. A velocidade depende da taxa de chamadas do host, o que é aceito nesta versão.

### 9.2 View

A matriz de view é composta diretamente, sem lookAt:

V = T(0, 0, −D) · Rx(pitch) · Ry(yaw) · T(−centro)

com pitch fixo de 35° (seno e cosseno calculados no init). A câmera olha para −z no espaço de view, sistema destro.

### 9.3 Projeção e distâncias

A projeção é perspectiva com FOV vertical de 60° e aspecto W/H. O fator `f = 1 / tan(fov/2)` é calculado no init.

As distâncias saem da esfera envolvente do terreno, de raio R calculado em `rzSetHeightmap`/`rzSetTerrainScale` a partir da meia diagonal da caixa (127,5 · cellSize, 127,5 · heightScale, 127,5 · cellSize). Para a esfera caber no FOV vertical de 60°, a distância da órbita é D = 2R. Então near = D − R = R e far = D + R = 3R, uma razão far/near de 3, muito confortável para a profundidade.

### 9.4 Clip space e viewport

Convenção de clip space: −w ≤ x ≤ w, −w ≤ y ≤ w, 0 ≤ z ≤ w, com w igual à distância da câmera ao longo de −z. O viewport transform, com o eixo y invertido para a imagem top-down, é

sx = (x · (1/w) · 0,5 + 0,5) · W
sy = (0,5 − y · (1/w) · 0,5) · H

seguido do arredondamento de sx·16 e sy·16 para 28.4 e do clamp da seção 7.2.

## 10. Pipeline por frame

| Etapa | O que faz |
|---|---|
| Rotação | Soma o passo ao ângulo, calcula seno e cosseno |
| Matrizes | Monta M = P · V (o model é identidade, pois o terreno já está em coordenadas de mundo) |
| Tabela de alturas | Calcula Ty[h] = M · (0, h · heightScale, 0, 0) para os 256 valores de h |
| Vetores de grade | Calcula T = M · (0, 0, 0, 1), Cx = M · (cellSize, 0, 0, 0), Cz = M · (0, 0, cellSize, 0) |
| Cache de vértices | Para cada vértice: clip = T + row · Cz + col · Cx + Ty[h]; outcode dos 6 planos; se outcode = 0, calcula 1/w e as coordenadas de tela em 28.4 |
| Limpeza | Framebuffer com a cor de fundo, depth buffer com 0 |
| Triângulos | Para cada um dos 130.050 triângulos: rejeição trivial, aceitação trivial ou clipping; culling; setup; rasterização |

No cache de vértices, `row · Cz` e `col · Cx` são calculados por multiplicação direta, não por soma acumulada, para não acumular erro de float ao longo da grade.

Para cada triângulo: se o AND dos outcodes é diferente de zero, é rejeitado. Se o OR é zero, segue direto com os dados do cache. Caso contrário, vai para o clipper, que recorta em clip space contra os seis planos (Sutherland-Hodgman homogêneo, até 9 vértices de saída), projeta os novos vértices e triangula em leque.

O setup calcula a área com sinal em inteiros, descarta se área ≤ 0, calcula o bounding box em pixels, os coeficientes das funções de aresta com o bias top-left e os gradientes de 1/w usando a recíproca da área.

A rasterização percorre o bounding box linha a linha, com as funções de aresta avançando por soma inteira. Num pixel coberto, 1/w é interpolado em float (valor no início da linha mais gradiente em x por pixel), seus bits são comparados como `uint32_t` com o depth buffer (para floats positivos a ordem dos bits é a ordem numérica; maior é mais perto), e se passar, escreve profundidade e cor. Como os triângulos do terreno são pequenos, percorrer o bounding box é suficiente nesta versão; a travessia por spans fica para quando as texturas entrarem.

Cor de fundo provisória: `0x00202830`.

## 11. Memória

| Buffer | Tamanho | Alocado em |
|---|---|---|
| Contexto | Pequeno | `rzCreate` |
| Depth buffer | W · H · 4 bytes (até ~8,3 MB) | `rzCreate` |
| Heightmap copiado | 64 KB | `rzSetHeightmap` |
| Cache de vértices (4 floats, 2 × int32 de tela, 1/w, outcode) | ~2 MB | `rzSetHeightmap` |
| Cores dos triângulos | 130.050 · 4 bytes (~520 KB) | `rzSetHeightmap` |
| Paleta | 766 · 4 bytes | `rzSetHeightmap` |

Regra: nenhuma alocação dentro de `rzRender`. O clipper usa buffers fixos na pilha ou no contexto.

## 12. Executável de teste

`rz_test.exe` linka o núcleo estaticamente (com `RZ_STATIC`), usando a mesma API C da DLL. Ele cria um framebuffer próprio, gera ou carrega um heightmap, chama `rzRender` por um número fixo de frames e grava imagens em `.ppm`.

Ele serve para validar o renderer sem o código legado e como teste de regressão: com rasterização inteira e float em SSE2 sem contração, a saída de uma mesma entrada deve ser idêntica bit a bit entre execuções e builds com as mesmas flags. O teste registra um hash de cada imagem para comparar com a referência.

## 13. Roadmap

M0: rasterizador 2D. Um triângulo em coordenadas de tela 28.4, regra top-left, saída em `.ppm`. Testes com triângulos adjacentes para verificar ausência de buracos e sobreposição.

M1: 3D. Transformação, clipping contra o frustum, 1/w, depth buffer, alguns triângulos em cena.

M2: terreno no executável de teste. Malha implícita, cache de vértices, cores pré-calculadas, câmera em órbita.

M3: DLL. Exportações, verificação de dependências com `objdump -p`, integração com o host legado.

Depois do PoC, na ordem prevista: `rzUpdateHeightmap` com retângulo sujo; controle de câmera pelo host; texturas com filtragem sempre nearest e correção de perspectiva por subdivisão de span (u/w, v/w e 1/w interpolados em float, divisão a cada N = 16 pixels, afim em fixed 16.16 entre os pontos, texturas com lados em potência de 2 para wrap por máscara); escolha de diagonal do quad pela altura dos cantos, de forma determinística.

## 14. Decisões em aberto

A profundidade como 1/w está adotada como proposta; a alternativa é z em fixed de 32 bits interpolado linearmente, e a decisão final pode sair do M1.

Os valores provisórios desta espec (escala padrão do terreno, paleta, direção da luz, ambient, pitch, FOV, passo de rotação, cor de fundo) devem ser ajustados visualmente no M2.
