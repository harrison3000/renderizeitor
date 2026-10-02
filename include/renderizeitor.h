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

#if defined(_WIN32)
#  define RZ_CALL __cdecl
#else
#  define RZ_CALL
#endif

typedef struct RzContext RzContext;

#define RZ_OK               0
#define RZ_ERR_INVALID_ARG  1   /* ponteiro nulo ou parâmetro fora de faixa */
#define RZ_ERR_SIZE         2   /* dimensão acima do limite ou não suportada */
#define RZ_ERR_NO_MEMORY    3   /* reservado (falha de alocação; hoje aborta) */
#define RZ_ERR_GL           4   /* OpenGL 3.3 indisponível, ou falha de contexto/shader/janela */
#define RZ_ERR_FILE         5   /* arquivo não abriu ou não pôde ser lido */
#define RZ_ERR_FORMAT       6   /* arquivo em formato não suportado ou corrompido */

/* Limite do modo offscreen (rzCreate). */
#define RZ_MAX_WIDTH   1920
#define RZ_MAX_HEIGHT  1080

/* Versão OpenGL (3.3 core). Dois modos de saída:

   rzCreate        offscreen: renderiza num framebuffer da GPU e, a cada
                   rzRender, copia a imagem para `pixels` (width*height de
                   RGBQUAD, uint32 0x00RRGGBB, top-down, de posse do host,
                   válido até rzDestroy). Igual à versão de software; útil para
                   testes e para rodar sem janela (llvmpipe).

   rzCreateWindow  janela filha: cria uma janela dentro de `parentWindow` (HWND
                   no Windows) em (x, y, width, height), no cliente do pai, e
                   rzRender desenha direto nela (sem cópia). O pai deve ter
                   WS_CLIPCHILDREN. O mouse e o teclado continuam indo para o
                   pai. Deve ser chamada da thread do loop de mensagens do pai.

   Em ambos, o contexto OpenGL pertence à thread que chamou rzCreate*: todas
   as outras funções devem ser chamadas dessa mesma thread. */
RZ_API int32_t RZ_CALL rzCreate(int32_t width, int32_t height,
                                void* pixels, RzContext** outCtx);

RZ_API int32_t RZ_CALL rzCreateWindow(void* parentWindow, int32_t x, int32_t y,
                                      int32_t width, int32_t height, RzContext** outCtx);

/* Só no modo janela: move/redimensiona a janela filha (coordenadas do cliente do pai). */
RZ_API int32_t RZ_CALL rzSetViewport(RzContext* ctx, int32_t x, int32_t y,
                                     int32_t width, int32_t height);

RZ_API void    RZ_CALL rzDestroy(RzContext* ctx);

/* Copia os dados. Nesta versão só aceita width == height == 256. */
RZ_API int32_t RZ_CALL rzSetHeightmap(RzContext* ctx, const uint8_t* data,
                                      int32_t width, int32_t height);

/* cellSize: distância entre pontos da grade; heightScale: altura por unidade do byte.
   Padrão: cellSize = 1, heightScale = 16/255 (byte 255 = 16 tiles; mundo 255 x 255 x 16). */
RZ_API int32_t RZ_CALL rzSetTerrainScale(RzContext* ctx,
                                         float cellSize, float heightScale);

/* Atlas de texturas: lê um arquivo PCX de 8 bits (1 plano, paleta VGA de 256
   cores no fim do arquivo). A imagem precisa ter pelo menos 256x256; se for
   maior, só o canto superior esquerdo (256x256) é usado e o resto é ignorado.
   O atlas tem 16 x 16 blocos de 16x16, numerados da esquerda para a direita e
   de cima para baixo (bloco 0 no canto superior esquerdo).
   pcxPath: caminho no código de página ANSI do sistema (fopen).
   Retornos: RZ_ERR_FILE (não abriu/leu), RZ_ERR_FORMAT (não é PCX de 8 bits
   com paleta, ou está truncado), RZ_ERR_SIZE (menor que 256x256).
   Em caso de erro, o atlas anterior (se houver) continua valendo. */
RZ_API int32_t RZ_CALL rzLoadTileAtlas(RzContext* ctx, const char* pcxPath);

/* Mapa de blocos: 256x256 bytes, data[row*256 + col] = bloco do quad (col, row).
   A última linha e a última coluna (255) não têm quad e são ignoradas.
   data == NULL desliga as texturas (volta às cores flat). Copia os dados. */
RZ_API int32_t RZ_CALL rzSetTileMap(RzContext* ctx, const uint8_t* data,
                                    int32_t width, int32_t height);

/* Filtragem das texturas. A ampliação (perto) é sempre nearest; muda a redução
   (longe), que evita o "shimmering" dos polígonos distantes. */
#define RZ_FILTER_NEAREST     0   /* sem mipmap */
#define RZ_FILTER_MIPMAP      1   /* nearest no nível mais próximo */
#define RZ_FILTER_MIP_DITHER  2   /* nearest, dither ordenado entre os dois níveis vizinhos (padrão) */
#define RZ_FILTER_MIP_LINEAR  3   /* nearest dentro do nível, mistura linear entre níveis */
#define RZ_FILTER_TRILINEAR   4   /* bilinear dentro do nível, linear entre níveis */

RZ_API int32_t RZ_CALL rzSetTextureFilter(RzContext* ctx, int32_t filter);

/* ------------------------------------------------------------------------ */
/* Objetos                                                                  */
/* ------------------------------------------------------------------------ */

/* Vértices: array de vertexCount * 3 uint32_t (três eixos por vértice), em
   ponto fixo 8.24 sem sinal, coordenadas absolutas no mundo: 1.0 = 1 tile.
   A ordem dos eixos é definida por rzSetObjectAxes (padrão RZ_AXES_Z_UP, como no legado).

   Montagem de um objeto (fase de carga):
     1. rzCreateObject(ctx, vertexCount, &id)       só a quantidade de vértices
     2. rzAddObjectPolygon(ctx, id, indices, n)     uma vez por polígono
     3. rzUpdateObjectVertices(ctx, id, vertices)   posições (e a cada frame)
   Os passos 2 e 3 podem vir em qualquer ordem e se repetir; o objeto só é
   desenhado depois da primeira rzUpdateObjectVertices. */

#define RZ_AXES_Y_UP  0   /* (x, y, z) = (coluna, altura, linha) */
#define RZ_AXES_Z_UP  1   /* (x, y, z) = (coluna, linha, altura) */

/* Vale para todos os objetos; mude antes de criar os objetos (as posições já
   carregadas não são reconvertidas). */
RZ_API int32_t RZ_CALL rzSetObjectAxes(RzContext* ctx, int32_t axes);

/* Cria um objeto vazio com vertexCount vértices (1 a 65536), ainda sem
   polígonos e sem posições (fase de carga: aloca). Devolve o id em outId. */
RZ_API int32_t RZ_CALL rzCreateObject(RzContext* ctx, int32_t vertexCount, int32_t* outId);

/* Acrescenta um polígono convexo ao objeto (fase de carga: aloca). Pode ser
   chamada quantas vezes for preciso; os índices são copiados.
   indices: count índices de vértices do objeto. Se o último repetir o
   primeiro (fechamento, como no legado: 0 1 2 3 0), ele é descartado.
   Triangulado em leque. Com menos de 3 vértices, é ignorado (RZ_OK).
   Erros: RZ_ERR_INVALID_ARG (índice fora do objeto; nada é acrescentado),
   RZ_ERR_SIZE (mais de 65535 polígonos no objeto). */
RZ_API int32_t RZ_CALL rzAddObjectPolygon(RzContext* ctx, int32_t id,
                                          const uint16_t* indices, int32_t count);

/* Posições de todos os vértices (vertexCount * 3 uint32_t). Copia e recalcula
   a iluminação; não aloca. */
RZ_API int32_t RZ_CALL rzUpdateObjectVertices(RzContext* ctx, int32_t id,
                                              const uint32_t* vertices);

/* Libera o objeto; o id pode ser reaproveitado por rzCreateObject. */
RZ_API int32_t RZ_CALL rzDestroyObject(RzContext* ctx, int32_t id);

/* Cor provisória do objeto inteiro (0x00RRGGBB), sombreada por polígono. */
RZ_API int32_t RZ_CALL rzSetObjectColor(RzContext* ctx, int32_t id, uint32_t rgb);

RZ_API int32_t RZ_CALL rzSetObjectVisible(RzContext* ctx, int32_t id, int32_t visible);

/* Descarte de faces pelo sentido em que os vértices aparecem na tela.
   Padrão RZ_CULL_NONE (desenha os dois lados), enquanto o winding dos
   polígonos do legado não é conhecido. */
#define RZ_CULL_NONE  0
#define RZ_CULL_CW    1   /* descarta faces com vértices em sentido horário */
#define RZ_CULL_CCW   2   /* descarta faces com vértices em sentido anti-horário */

RZ_API int32_t RZ_CALL rzSetObjectCulling(RzContext* ctx, int32_t id, int32_t cull);

/* ------------------------------------------------------------------------ */

/* Câmera. Com um alvo, persegue o vértice `vertex` do objeto `id` (ver
   rzSetCameraFollow), sempre olhando para ele e acompanhando
   rzUpdateObjectVertices; o vértice não precisa fazer parte de nenhum
   polígono. Sem alvo (id < 0, o padrão, ou objeto destruído), mostra uma
   visão geral fixa do terreno inteiro: só para testes e fallback. */
RZ_API int32_t RZ_CALL rzSetCameraTarget(RzContext* ctx, int32_t id, int32_t vertex);

/* Câmera de perseguição, como se puxada por uma corda:
   distance  comprimento da corda, em tiles (padrão 3): com o alvo mais longe
             que isso, a câmera é puxada; mais perto que a metade, ela recua;
   height    altura desejada acima do alvo, em tiles (padrão 1);
   stiffness fração do caminho até a posição desejada percorrida a cada
             rzRender, em (0, 1] (padrão 0.08): menor = mais suave e atrasada. */
RZ_API int32_t RZ_CALL rzSetCameraFollow(RzContext* ctx, float distance, float height,
                                         float stiffness);

/* Avança a câmera e renderiza o frame: offscreen, copia para o buffer do host;
   janela, apresenta (SwapBuffers). Não aloca. */
RZ_API int32_t RZ_CALL rzRender(RzContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
