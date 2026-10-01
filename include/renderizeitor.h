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

/* Atlas de texturas: imagem paletizada de 8 bits (como a de um PCX), com
   blocos de 16x16 dispostos em grade, numerados da esquerda para a direita e
   de cima para baixo. width e height múltiplos de 16 (até 4096); só os 256
   primeiros blocos são usados.
   indices: width*height bytes, linha 0 em cima. paletteRGB: 256 x (R, G, B). */
RZ_API int32_t RZ_CALL rzSetTileAtlas(RzContext* ctx, const uint8_t* indices,
                                      int32_t width, int32_t height,
                                      const uint8_t* paletteRGB);

/* Mapa de blocos: 256x256 bytes, data[row*256 + col] = bloco do quad (col, row).
   A última linha e a última coluna (255) não têm quad e são ignoradas.
   data == NULL desliga as texturas (volta às cores flat). Copia os dados. */
RZ_API int32_t RZ_CALL rzSetTileMap(RzContext* ctx, const uint8_t* data,
                                    int32_t width, int32_t height);

/* Filtragem das texturas. Sempre nearest dentro do nível; muda só a escolha
   do nível de mipmap (evita o "shimmering" dos polígonos distantes). */
#define RZ_FILTER_NEAREST     0   /* sem mipmap */
#define RZ_FILTER_MIPMAP      1   /* nível mais próximo */
#define RZ_FILTER_MIP_DITHER  2   /* dither ordenado entre os dois níveis vizinhos (padrão) */

RZ_API int32_t RZ_CALL rzSetTextureFilter(RzContext* ctx, int32_t filter);

/* ------------------------------------------------------------------------ */
/* Objetos                                                                  */
/* ------------------------------------------------------------------------ */

/* Vértices: array de vertexCount * 3 uint32_t (três eixos por vértice), em
   ponto fixo 8.24 sem sinal, coordenadas absolutas no mundo: 1.0 = 1 tile.
   A ordem dos eixos é definida por rzSetObjectAxes (padrão RZ_AXES_Y_UP).

   Polígonos: índices (uint16_t) concatenados; cada polígono termina com uma
   cópia do seu primeiro índice, por exemplo  0 1 2 3 0  4 5 6 4 ...
   Polígonos convexos, triangulados em leque na carga. Menos de 3 vértices:
   ignorado. */

#define RZ_AXES_Y_UP  0   /* (x, y, z) = (coluna, altura, linha) */
#define RZ_AXES_Z_UP  1   /* (x, y, z) = (coluna, linha, altura) */

/* Vale para todos os objetos; mude antes de criar os objetos (as posições já
   carregadas não são reconvertidas). */
RZ_API int32_t RZ_CALL rzSetObjectAxes(RzContext* ctx, int32_t axes);

/* Cria um objeto (fase de carga: aloca). Devolve o id em outId. */
RZ_API int32_t RZ_CALL rzCreateObject(RzContext* ctx,
                                      const uint32_t* vertices, int32_t vertexCount,
                                      const uint16_t* indices, int32_t indexCount,
                                      int32_t* outId);

/* Novas posições dos vértices (mesma quantidade e topologia). Copia e
   recalcula a iluminação; não aloca. */
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

/* Controle de câmera pelo host.
   pitchDegrees: elevação da órbita em graus, limitada a [0, 89]. Padrão 35. */
RZ_API int32_t RZ_CALL rzSetCameraPitch(RzContext* ctx, float pitchDegrees);

/* Alvo da câmera: com um alvo, a câmera passa a perseguir o vértice `vertex`
   do objeto `id` (ver rzSetCameraFollow), sempre olhando para ele e
   acompanhando rzUpdateObjectVertices. O vértice não precisa fazer parte de
   nenhum polígono. id < 0 volta à órbita em torno do centro do terreno
   (padrão), onde valem pitch, distância e rotação; se o objeto for destruído,
   a câmera também volta à órbita. */
RZ_API int32_t RZ_CALL rzSetCameraTarget(RzContext* ctx, int32_t id, int32_t vertex);

/* Câmera de perseguição, como se puxada por uma corda:
   distance  comprimento da corda, em tiles (padrão 12): com o alvo mais longe
             que isso, a câmera é puxada; mais perto que a metade, ela recua;
   height    altura desejada acima do alvo, em tiles (padrão 3);
   stiffness fração do caminho até a posição desejada percorrida a cada
             rzRender, em (0, 1] (padrão 0.08): menor = mais suave e atrasada. */
RZ_API int32_t RZ_CALL rzSetCameraFollow(RzContext* ctx, float distance, float height,
                                         float stiffness);

/* Distância da câmera ao centro, como fator da distância padrão (que enquadra
   o terreno inteiro). Limitada a [0.02, 4]. Padrão 1. */
RZ_API int32_t RZ_CALL rzSetCameraDistance(RzContext* ctx, float factor);

/* Passo de rotação somado ao yaw a cada rzRender (2^32 = uma volta).
   Negativo gira ao contrário, 0 para. Padrão 1 << 22 (1024 frames por volta). */
RZ_API int32_t RZ_CALL rzSetRotationStep(RzContext* ctx, int32_t step);

/* Avança a rotação, renderiza o frame no buffer do host. Não aloca. */
RZ_API int32_t RZ_CALL rzRender(RzContext* ctx);

#ifdef __cplusplus
}
#endif

#endif
