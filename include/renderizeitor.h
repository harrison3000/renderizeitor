#ifndef RENDERIZEITOR_H
#define RENDERIZEITOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(RZ_STATIC) || !defined(_WIN32)
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

/* Structs da API: passadas só por ponteiro (nunca por valor), layout fixo,
   campos do maior para o menor, padding explícito (zerar). */

/* Posição de um vértice em tiles (1.0 = 1 tile), eixos do legado: x = coluna,
   y = linha, z = altura (para cima). Do 8.24 do legado: valor / 16777216.0f.
   12 bytes. */
typedef struct RzVertex {
    float x, y, z;
} RzVertex;

/* Canto de polígono texturizado: índice do vértice e UV. 12 bytes. */
typedef struct RzTexVertex {
    float    u, v;
    uint16_t index;
    uint16_t pad;
} RzTexVertex;

/* Uma roda (ver rzSetObjectWheels). 8 bytes. */
typedef struct RzWheel {
    float    diameter;     /* tiles */
    uint16_t hubVertex;    /* vértice do objeto no centro da roda */
    uint8_t  front;        /* != 0: dianteira */
    uint8_t  pad;
} RzWheel;

/* Um quadrado no atlas da textura do objeto, por dois cantos opostos em UV
   (0..1); o desenho de uma face da roda (ver rzSetObjectWheelFaces). 16 bytes. */
typedef struct RzWheelFace {
    float u0, v0;          /* um canto */
    float u1, v1;          /* o canto oposto */
} RzWheelFace;

/* Um sprite (ver rzSetSprites). 20 bytes. */
typedef struct RzSprite {
    float    x, y, z;      /* centro, em tiles como RzVertex (z para cima) */
    float    size;         /* diâmetro em tiles */
    uint8_t  color;        /* índice na paleta do jogo */
    uint8_t  pad[3];
} RzSprite;

/* Tamanhos conferidos em compilação (C89: array de tamanho negativo) */
typedef char RzAssertVertexSize[sizeof(RzVertex) == 12 ? 1 : -1];
typedef char RzAssertTexVertexSize[sizeof(RzTexVertex) == 12 ? 1 : -1];
typedef char RzAssertWheelSize[sizeof(RzWheel) == 8 ? 1 : -1];
typedef char RzAssertWheelFaceSize[sizeof(RzWheelFace) == 16 ? 1 : -1];
typedef char RzAssertSpriteSize[sizeof(RzSprite) == 20 ? 1 : -1];

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

/* rzCreateCurrent  adota um contexto OpenGL 3.3 core que o host já criou e
   deixou corrente (ex.: SDL2 com SDL_GL_CreateContext). rzRender desenha
   direto no framebuffer padrão da janela do host (sem cópia); o host é quem
   troca os buffers (SDL_GL_SwapWindow) depois de rzRender e quem trata a
   entrada e o laço de frames. A janela e o contexto continuam do host:
   rzDestroy não os destrói. Deve ser chamada da thread dona do contexto. */
RZ_API int32_t RZ_CALL rzCreateCurrent(int32_t width, int32_t height, RzContext** outCtx);

/* Só no modo janela: move/redimensiona a janela filha (coordenadas do cliente do pai). */
RZ_API int32_t RZ_CALL rzSetViewport(RzContext* ctx, int32_t x, int32_t y,
                                     int32_t width, int32_t height);

RZ_API void    RZ_CALL rzDestroy(RzContext* ctx);

/* Alturas: 256x256 bytes, data[row*256 + col]. Nesta versão só aceita
   width == height == 256. Copia os dados e monta tudo (malha, borda, sombra
   do relevo): fase de carga, custa dezenas de ms. O ponteiro também fica
   guardado para rzUpdateTerrain: o buffer tem que continuar válido até a
   próxima rzSetHeightmap ou rzDestroy. */
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
   Pixels com o índice 255 da paleta são totalmente transparentes (o chão
   fica com buraco ali).
   pcxPath: caminho no código de página ANSI do sistema (fopen).
   Retornos: RZ_ERR_FILE (não abriu/leu), RZ_ERR_FORMAT (não é PCX de 8 bits
   com paleta, ou está truncado), RZ_ERR_SIZE (menor que 256x256).
   Em caso de erro, o atlas anterior (se houver) continua valendo. */
RZ_API int32_t RZ_CALL rzLoadTileAtlas(RzContext* ctx, const char* pcxPath);

/* Mapa de blocos: 256x256 bytes, data[row*256 + col] = bloco do quad (col, row).
   A última linha e a última coluna (255) não têm quad e são ignoradas.
   data == NULL desliga as texturas (volta às cores flat). Copia os dados e
   refaz os blocos de todos os quads (mais barata que rzSetHeightmap: não
   mexe no relevo nem na sombra). O ponteiro fica guardado para
   rzUpdateTerrain (mesma regra de validade de rzSetHeightmap). */
RZ_API int32_t RZ_CALL rzSetTileMap(RzContext* ctx, const uint8_t* data,
                                    int32_t width, int32_t height);

/* Atualização rápida do terreno durante o jogo: relê os buffers passados a
   rzSetHeightmap e rzSetTileMap (o host altera os próprios arrays e chama
   esta) e refaz só o que mudou, ponto a ponto (feita para poucas mudanças
   por vez): os 4 quads em volta de cada altura alterada (ou o quad de cada
   bloco alterado) e, na sombra do relevo, só a área deles. Mudança
   de altura na borda do mapa (linha/coluna 0 ou 255) só move junto a vértice
   do terreno de fora que está em cima daquele ponto (o resto de fora não é
   recalculado: para mudanças pequenas; rzSetHeightmap refaz tudo); bloco a
   menos de 8 quads da borda refaz a malha da continuação.
   Custo: proporcional ao número de pontos alterados (comparar os 2 x 64 KB
   é desprezível);
   sem mudança, nada. Não aloca. Pode ser chamada todo frame.
   Erros: RZ_ERR_INVALID_ARG sem rzSetHeightmap antes. */
RZ_API int32_t RZ_CALL rzUpdateTerrain(RzContext* ctx);

/* Filtragem das texturas (chão e objetos): fixa. Ampliação (perto) nearest;
   redução (longe) nearest dentro do nível de mipmap e mistura linear entre
   níveis (evita o "shimmering" dos polígonos distantes), mais filtro
   anisotrópico 4x sempre que o driver suporta (ou o máximo dele, se menor).
   Em renderer de software (llvmpipe) o anisotrópico custa caro: lá, uma
   neblina mais curta (rzSetFog) compensa. */

/* Cor de fundo (onde não há terreno nem objeto), 0 a 255 por canal. Vale a
   partir do próximo rzRender. Padrão (32, 40, 48). */
RZ_API int32_t RZ_CALL rzSetBackgroundColor(RzContext* ctx, uint8_t r, uint8_t g, uint8_t b);

/* Neblina (draw distance), em tiles de distância 3D ao olho: limpa até
   start, some na cor de fundo em end; além de end nada é desenhado. Só vale
   seguindo um alvo (rzSetCameraTarget). Padrão 30 e 65.
   Custo: desprezível (só guarda os dois valores; nada é refeito na CPU nem
   na GPU), vale a partir do próximo rzRender. Pode ser chamada a qualquer
   momento, inclusive todo frame ao longo do percurso (ex.: fechar a neblina
   num trecho).
   Erros: RZ_ERR_INVALID_ARG se não for 0 <= start < end <= 120 (o terreno
   continua 120 tiles além da borda do mapa; mais que isso faltaria chão). */
RZ_API int32_t RZ_CALL rzSetFog(RzContext* ctx, float start, float end);

/* ------------------------------------------------------------------------ */
/* Objetos                                                                  */
/* ------------------------------------------------------------------------ */

/* Vértices: array de vertexCount RzVertex, em float, coordenadas absolutas
   no mundo: 1.0 = 1 tile. NaN, infinito ou |valor| >= 1e6 em qualquer vértice:
   RZ_ERR_INVALID_ARG e nada muda.
   Eixos do legado: (x, y, z) = (coluna, linha, altura), z para cima.

   Montagem de um objeto (fase de carga):
     1. rzCreateObject(ctx, vertexCount, &id)       só a quantidade de vértices
     2. rzAddObjectPolygon(ctx, id, indices, n, cor)  uma vez por polígono, ou
        rzAddObjectTexturedPolygon(ctx, id, corners, n)  (com UVs)
     3. rzUpdateObjectVertices(ctx, id, vertices)   posições (e a cada frame)
   Os passos 2 e 3 podem vir em qualquer ordem e se repetir; o objeto só é
   desenhado depois da primeira rzUpdateObjectVertices. */

/* Cria um objeto vazio com vertexCount vértices (1 a 65536), ainda sem
   polígonos e sem posições (fase de carga: aloca). Devolve o id em outId. */
RZ_API int32_t RZ_CALL rzCreateObject(RzContext* ctx, int32_t vertexCount, int32_t* outId);

/* Acrescenta um polígono convexo de cor sólida ao objeto (fase de carga:
   aloca). Pode ser chamada quantas vezes for preciso; os índices são copiados.
   indices: count índices de vértices do objeto. Se o último repetir o
   primeiro (fechamento, como no legado: 0 1 2 3 0), ele é descartado.
   paletteIndex (0..255): a cor é a dessa entrada da paleta do PCX da textura
   do objeto (ver rzLoadObjectTexture), com a luz flat do polígono.
   Triangulado em leque. Com menos de 3 vértices, é ignorado (RZ_OK).
   Erros: RZ_ERR_INVALID_ARG (índice fora do objeto ou paletteIndex fora de
   0..255; nada é acrescentado), RZ_ERR_SIZE (mais de 65535 polígonos). */
RZ_API int32_t RZ_CALL rzAddObjectPolygon(RzContext* ctx, int32_t id,
                                          const uint16_t* indices, int32_t count,
                                          int32_t paletteIndex);

/* Igual a rzAddObjectPolygon, mas com UVs próprios em vez da cor: corners
   tem count cantos (índice do vértice + UV). Se o último canto repetir o
   índice do primeiro (fechamento), ele é descartado. u e v vão de 0 a 1,
   ambos na escala da LARGURA da textura (ver rzLoadObjectTexture): v = 1 é a
   linha W, não a altura da imagem. (0, 0) é o canto superior esquerdo. Fora
   de [0, 1], repete a borda. UV não finito: RZ_ERR_INVALID_ARG. A cor do
   polígono é a textura vezes a luz flat. Os UVs devem ficar fora das últimas
   16 linhas (faixa de paleta). */
RZ_API int32_t RZ_CALL rzAddObjectTexturedPolygon(RzContext* ctx, int32_t id,
                                                  const RzTexVertex* corners, int32_t count);

/* Polígono translúcido (vidro) em tom de cinza. tone 0..15: o que está atrás
   é multiplicado por tone/15 (0 = preto, 15 = transparente), mais um brilho
   especular embaçado da luz (some na sombra). Não depende da ordem de desenho;
   não projeta sombra; só a face de fora aparece (como os outros polígonos).
   Mesmas regras de índices/fechamento de rzAddObjectPolygon; sem textura.
   Erros: RZ_ERR_INVALID_ARG (índice fora do objeto ou tone fora de 0..15). */
RZ_API int32_t RZ_CALL rzAddObjectTranslucentPolygon(RzContext* ctx, int32_t id,
                                                     const uint16_t* indices, int32_t count,
                                                     int32_t tone);

/* Linha entre os vértices a e b do objeto (fase de carga: aloca), exibida
   como uma barra de seção quadrada de lado `thickness` (tiles), na cor
   paletteIndex (0..255) da paleta da textura do objeto, como em
   rzAddObjectPolygon, com luz e sombra. Acompanha rzUpdateObjectVertices.
   a == b é ignorada (RZ_OK). Erros: RZ_ERR_INVALID_ARG (vértice fora do
   objeto, thickness <= 0 ou não finita, paletteIndex fora de 0..255),
   RZ_ERR_SIZE (mais de 65535 linhas). */
RZ_API int32_t RZ_CALL rzAddObjectLine(RzContext* ctx, int32_t id, uint16_t a, uint16_t b,
                                       float thickness, int32_t paletteIndex);

/* Rodas: cilindros pretos finos (as faces podem receber textura com
   rzSetObjectWheelFaces). wheels: 4 RzWheel, cada uma:
   hubVertex  vértice do objeto que é o centro da roda (acompanha
              rzUpdateObjectVertices);
   front      != 0 se dianteira (precisam ser duas dianteiras e duas
              traseiras: daí sai a frente do carro e a linha das rodas);
   diameter   diâmetro em tiles.
   Chamar de novo troca as rodas (e zera o esterçamento). Fase de carga.
   Erros: RZ_ERR_INVALID_ARG (vértice fora do objeto, diâmetro <= 0 ou não
   finito, ou não forem 2 + 2). */
RZ_API int32_t RZ_CALL rzSetObjectWheels(RzContext* ctx, int32_t id, const RzWheel* wheels);

/* Texturiza as duas faces (os discos) de cada roda com a textura do objeto
   (a mesma de rzLoadObjectTexture): dois quadrados do atlas, um desenhado na
   face EXTERNA (a que aponta para fora do carro) e outro na INTERNA. Cada
   quadrado é dado por dois cantos opostos em UV (0..1); o disco da face é
   mapeado no círculo inscrito nele. O pneu (a lateral do cilindro) continua
   preto. As quatro rodas usam o mesmo par de faces.
   Precisa de rzSetObjectWheels antes. Chamar de novo troca as faces; não aloca.
   Erros: RZ_ERR_INVALID_ARG (sem rodas, ponteiro nulo ou UV não finito). */
RZ_API int32_t RZ_CALL rzSetObjectWheelFaces(RzContext* ctx, int32_t id,
                                             const RzWheelFace* outer, const RzWheelFace* inner);

/* Velocidade do carro (tiles por frame, = distância andada neste frame, na
   mesma escala dos vértices). A cada frame renderizado as rodas giram por
   distância / raio, cada uma pelo seu próprio diâmetro (dianteiras e traseiras
   podem diferir); positivo = para a frente. Fica guardada: chame uma vez por
   frame (ou quando mudar). 0 para parar. Não aloca.
   RZ_ERR_INVALID_ARG sem rzSetObjectWheels antes ou com valor não finito. */
RZ_API int32_t RZ_CALL rzSetObjectWheelSpin(RzContext* ctx, int32_t id, float speed);

/* Esterçamento das duas rodas dianteiras (radianos; positivo vira para a
   esquerda, anti-horário visto de cima); as traseiras ficam retas. Não aloca.
   RZ_ERR_INVALID_ARG sem rzSetObjectWheels antes ou com ângulo não finito. */
RZ_API int32_t RZ_CALL rzUpdateObjectWheels(RzContext* ctx, int32_t id, float steer);

/* Textura do objeto (uma por objeto), lida de um PCX de 8 bits (fase de carga).
   A textura é quadrada, W x W: W é a maior potência de 2 que cabe na largura
   da imagem (mínimo 256, máximo 4096 ou o limite do driver); o que sobra à
   direita é descartado. Na vertical, a imagem é cortada em W linhas ou
   completada embaixo (repetindo a última linha).
   Pixels com o índice 255 da paleta são totalmente transparentes (inclusive
   a cor sólida 255 de rzAddObjectPolygon).
   As últimas 16 linhas da textura são reservadas: viram 256 bloquinhos 4x4,
   um por cor da paleta do PCX, para os polígonos de cor sólida
   (rzAddObjectPolygon). O conteúdo da imagem nessas linhas é sobrescrito.
   Sem textura carregada, ou se esta função falhar (RZ_ERR_FILE, RZ_ERR_FORMAT,
   RZ_ERR_SIZE para largura < 256), o objeto usa a textura fallback (xadrez
   magenta; os polígonos de cor sólida também saem no xadrez). Chamar de novo
   troca a textura. */
RZ_API int32_t RZ_CALL rzLoadObjectTexture(RzContext* ctx, int32_t id, const char* pcxPath);

/* Troca a textura fallback (a dos objetos sem textura ou cuja carga falhou),
   com as mesmas regras de rzLoadObjectTexture. Vale para todos os objetos,
   inclusive os já criados. Em caso de erro, a fallback atual continua.
   pcxPath == NULL volta ao xadrez magenta gerado pela DLL. */
RZ_API int32_t RZ_CALL rzLoadFallbackTexture(RzContext* ctx, const char* pcxPath);

/* Posições de todos os vértices (vertexCount RzVertex; mesmo layout de um
   array de vertexCount * 3 uint32_t). Copia e recalcula a iluminação; não
   aloca. */
RZ_API int32_t RZ_CALL rzUpdateObjectVertices(RzContext* ctx, int32_t id,
                                              const RzVertex* vertices);

/* Libera o objeto; o id pode ser reaproveitado por rzCreateObject. */
RZ_API int32_t RZ_CALL rzDestroyObject(RzContext* ctx, int32_t id);

/* ------------------------------------------------------------------------ */
/* Sprites (fumaça, detritos, água espirrando)                              */
/* ------------------------------------------------------------------------ */

/* Substitui todos os sprites pelos count de `sprites` (copiados; 0 limpa).
   Sem id: cada chamada é o array inteiro, na ordem que for (o legado pode
   compactar o array quando um sprite morre). Cada um vira um quad virado
   para a câmera, do tamanho `size`, com a forma de um ruído de spray
   (8 variações que vão se alternando sozinhas a cada poucos frames) na cor
   `color` da paleta do jogo (a do PCX de rzLoadTileAtlas; antes dele, cinza
   = índice). Translúcidos, ordenados pelo renderer; com neblina; sem sombra.
   Custo: baixo, não aloca; feita para ser chamada todo frame.
   Erros (nada muda): RZ_ERR_INVALID_ARG (sprites nulo com count > 0, count
   < 0, size <= 0 ou não finito), RZ_ERR_SIZE (count > 256). */
RZ_API int32_t RZ_CALL rzSetSprites(RzContext* ctx, const RzSprite* sprites, int32_t count);


/* Faces: as que aparecem em sentido anti-horário na tela são descartadas
   (convenção do legado: vista de fora, a face está em sentido horário). */


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

/* Checagem de erros em bloco: todas as funções que devolvem código guardam o
   primeiro erro no contexto. Dá para inicializar tudo sem olhar os retornos e
   checar uma vez no fim:

       rzCreateWindow(hwnd, 0, 0, w, h, &ctx);
       rzSetHeightmap(ctx, ...);  rzLoadTileAtlas(ctx, ...);  ...
       const char* where;
       if (rzGetError(ctx, &where) != RZ_OK) { ... where = "rzLoadTileAtlas" ... }

   Devolve o primeiro erro desde a última chamada (RZ_OK se nada falhou) e
   zera o registro. outFunction (pode ser NULL) recebe o nome da função que
   falhou primeiro (string estática), ou NULL.
   ctx NULL (rzCreate/rzCreateWindow falhou): devolve o erro da criação, ou
   RZ_ERR_INVALID_ARG se não houve falha de criação registrada.
   Chamadas com ctx NULL não têm onde registrar: só devolvem o erro. */
RZ_API int32_t RZ_CALL rzGetError(RzContext* ctx, const char** outFunction);

#ifdef __cplusplus
}
#endif

#endif
