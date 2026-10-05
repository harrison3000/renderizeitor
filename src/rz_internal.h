// Tipos e funções internas do Renderizeitor (versão OpenGL). Não faz parte da
// interface pública.
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

#include "renderizeitor.h"
#include "rz_gl.h"
#include "rz_platform.h"

// Exportações: pilha realinhada na entrada (Win32 só garante 4 bytes, SSE2 quer 16).
#if defined(__i386__)
#  define RZ_ENTRY __attribute__((force_align_arg_pointer))
#else
#  define RZ_ENTRY
#endif

namespace rz {

// ---------------------------------------------------------------------------
// Constantes
// ---------------------------------------------------------------------------

constexpr int32_t kGridSize      = 256;                         // vértices por lado
constexpr int32_t kQuadsPerSide  = kGridSize - 1;               // 255
constexpr int32_t kVertexCount   = kGridSize * kGridSize;       // 65.536
constexpr int32_t kTriangleCount = kQuadsPerSide * kQuadsPerSide * 2; // 130.050
constexpr int32_t kPaletteSize   = 766;                         // soma de 3 alturas: 0..765

// Cor de fundo padrão (rzSetBackgroundColor), já em float para o glClearColor
constexpr float kBackgroundR = 32.0f / 255.0f;
constexpr float kBackgroundG = 40.0f / 255.0f;
constexpr float kBackgroundB = 48.0f / 255.0f;

constexpr float kPi              = 3.14159265358979f;
constexpr float kFovYDegrees     = 60.0f;
constexpr float kAmbient         = 0.3f;
constexpr float kTexturedShading = 0.35f;   // força da luz sobre o chão texturizado (0 = textura pura)

// Visão geral (sem alvo): terreno inteiro, visto do lado +z, inclinação fixa
constexpr float kOverviewPitchDegrees = 50.0f;

// Escala do legado: byte 0 = altura 0, byte 255 = 16 tiles (mundo 255 x 255 x 16)
constexpr float kDefaultCellSize    = 1.0f;
constexpr float kDefaultHeightScale = 16.0f / 255.0f;

// Câmera de perseguição, na escala de um carro de ~0,85 tile
constexpr float kFollowDistance  = 3.0f;    // corda (tiles)
constexpr float kFollowHeight    = 1.0f;    // altura acima do alvo (tiles)
constexpr float kFollowStiffness = 0.08f;
constexpr float kFollowClearance    = 0.5f;   // altura mínima desejada sobre o chão (tiles)
constexpr float kFollowMinClearance = 0.1f;   // limite duro sobre o chão (tiles)
constexpr float kFollowClimb        = 0.2f;   // amortecimento ao subir
// Ponto olhado (alvo suavizado): o tremor do alvo em terreno esburacado não
// chega à câmera. Forte na vertical, leve na horizontal (pouco atraso).
constexpr float kFollowLookXZ       = 0.5f;
constexpr float kFollowLookY        = 0.1f;

constexpr int32_t kMaxWindowSize = 8192;

// Neblina por distância (só seguindo um alvo; a visão geral fica sem): limpa
// até o início, some na cor de fundo no fim (tiles, distância 3D ao olho,
// curva smoothstep). Além do fim nada é desenhado. Padrão 30..65; muda em
// runtime com rzSetFog (RzContext::fogStart/fogEnd), fim limitado a kFogMaxEnd.
constexpr float kFogStartDefault = 30.0f;
constexpr float kFogEndDefault   = 65.0f;
constexpr float kFogCullMargin = 10.0f;   // tiles: objetos além do fim + raio + margem
                                          // não entram nem na tela nem nas sombras
                                          // (a margem cobre sombras longas para dentro)

// Borda do mundo (rz_border.cpp): continuação do terreno além do mapa, até o
// alcance da neblina + folga, e parede de limite com X vermelhos.
// Faixas da malha da continuação, de dentro para fora: até `limit` tiles da
// borda, células de `step` tiles. A última vai até kSkirtExtent.
struct SkirtBand { int32_t limit, step; };
constexpr SkirtBand kSkirtBands[] = { { 8, 1 }, { 16, 2 }, { 32, 4 }, { 128, 16 } };
constexpr int32_t kSkirtBandCount = int32_t(sizeof(kSkirtBands) / sizeof(kSkirtBands[0]));
constexpr int32_t kSkirtExtent   = kSkirtBands[kSkirtBandCount - 1].limit;   // 128: alinha as células de 16
constexpr float   kSkirtBlend    = 24.0f;  // tiles da altura da borda até a gerada
constexpr float   kSkirtNoiseLow  = 60.0f; // amplitude do ruído (unidade do byte), período 32
constexpr float   kSkirtNoiseHigh = 18.0f; // período 12
constexpr int32_t kSkirtTileBand  = 8;     // sorteio: quads a até 8 tiles do ponto da borda mais próximo
constexpr int32_t kSkirtJitterPercent = 12;   // % das células que sorteiam em vez de continuar a borda
constexpr float   kSkirtMeanderGrowth = 0.35f; // deslocamento ao longo da borda: até 0,35 x distância...
constexpr float   kSkirtMeanderMax    = 12.0f; // ...limitado a 12 tiles
constexpr int32_t kSkirtMeanderPeriod = 24;    // período do ruído do serpenteio (tiles)
constexpr float   kBorderWallHeight = 6.0f;   // tiles acima do chão
constexpr float   kBorderFadeFar    = 25.0f;  // alvo a 25 tiles: começa a aparecer
constexpr float   kBorderFadeNear   = 6.0f;   // a 6 tiles: totalmente visível
constexpr float   kBorderXSize      = 2.0f;   // célula de cada círculo (tiles), presa ao mundo
constexpr float   kFogMaxEnd = 120.0f;   // rzSetFog (a continuação vai a kSkirtExtent, com folga)
static_assert(kFogEndDefault <= kFogMaxEnd && kFogMaxEnd <= float(kSkirtExtent),
              "a continuação tem que ir até o fim da neblina");

// Sprites (rz_sprites.cpp): fumaça/detritos/água, quads virados para a câmera
constexpr int32_t kMaxSprites          = 256;    // máximo validado no legado
constexpr int32_t kSpriteFrames        = 8;      // variações do ruído (textura array)
constexpr int32_t kSpriteFrameTicks    = 4;      // frames de render por quadro
constexpr int32_t kSpriteNoiseSize     = 64;     // lado da textura de ruído
constexpr int32_t kSpriteClicks        = 2;      // "cliques do spray" por quadro
constexpr int32_t kSpriteDotsPerClick  = 260;    // pontinhos por clique
constexpr float   kSpriteClickRadius   = 0.36f;  // raio do clique (fração do lado)
constexpr float   kSpriteClickJitter   = 0.10f;  // deslocamento do centro do clique

// Sombras: três shadow maps da luz direcional (detalhes em rz_shadow.cpp):
// 0 terreno inteiro (só o terreno), 1 objetos próximos, 2 objeto seguido.
constexpr int32_t kShadowMaps           = 3;
// Tamanhos pedidos; limitados a GL_MAX_TEXTURE_SIZE na criação (ctx->shadowSize).
// Resoluções parecidas entre os mapas, para a sombra do alvo não destoar das outras.
constexpr int32_t kShadowTerrainSize    = 4096;   // ~0,09 tile/texel
constexpr int32_t kShadowScissorMargin  = 4;     // texels em volta da área refeita do mapa 0
constexpr int32_t kShadowNearSize       = 2048;
constexpr float   kShadowNearHalfExtent = 48.0f;  // tiles: 96 x 96, ~0,047 tile/texel
constexpr float   kShadowNearAhead      = 32.0f;  // centro da caixa: 32 tiles à frente do olho
                                                  // (cobre de ~16 atrás a ~80 à frente: até o fim da neblina)
constexpr int32_t kShadowTargetSize     = 256;
constexpr float   kShadowTargetMargin   = 0.25f;  // tiles além da esfera do objeto seguido
constexpr float   kShadowTargetStep     = 0.25f;  // passo da meia-largura da caixa do alvo
constexpr int32_t kUnitShadowFirst      = 1;      // unidades de textura 1..3 (0: atlas/objeto)
constexpr float   kShadowTexturedDim  = 0.48f;  // chão texturizado na sombra (era 0,6; 20 % mais escuro)
constexpr float   kShadowLight        = kAmbient * 0.8f;   // luz na sombra (cores flat e objetos):
                                                         // 20 % abaixo do ambiente
constexpr float   kShadowOffsetFactor = 2.0f;   // glPolygonOffset no passe de profundidade
constexpr float   kShadowOffsetUnits  = 4.0f;

// Texturas: blocos 16x16 numa textura array (uma camada por bloco), com
// mipmaps 16, 8, 4, 2, 1 gerados na carga (média 2x2, como no software).
constexpr int32_t kTileSize      = 16;
constexpr int32_t kMipLevels     = 5;
constexpr int32_t kMaxTiles      = 256;                         // índice é um byte
constexpr int32_t kAtlasSize     = 256;                         // atlas: 16 x 16 blocos de 16x16

// ---------------------------------------------------------------------------
// Matemática
// ---------------------------------------------------------------------------

struct Vec3 {
    float x, y, z;
};

inline Vec3  operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3  operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float dot(Vec3 a, Vec3 b)       { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3  cross(Vec3 a, Vec3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// Matriz 4x4, vetores coluna: v' = M · v. Acesso m[linha, coluna].
// Enviada ao OpenGL com transpose = GL_TRUE.
struct Mat4 {
    float e[16];

    float&       operator[](int row, int col)       { return e[row * 4 + col]; }
    const float& operator[](int row, int col) const { return e[row * 4 + col]; }

    static Mat4 identity();
};

Mat4 operator*(const Mat4& a, const Mat4& b);

// ---------------------------------------------------------------------------
// Objetos
// ---------------------------------------------------------------------------

// Triângulo de objeto: índices de vértice e o polígono de origem (para a cor).
// Cantos do triângulo como posições em Object::indices (dali saem o vértice e o UV).
struct ObjectTriangle {
    int32_t  a, b, c;
    uint16_t polygon;
};

// Vértice de objeto enviado à GPU: posição no mundo, luz flat do polígono
// (cinza 0x00LLLLLL; em memória B, G, R, 0, lido como vec4 normalizado), que
// multiplica a textura, e UV.
struct GpuVertex {
    float    x, y, z;
    uint32_t color;
    float    u, v;
};
static_assert(sizeof(GpuVertex) == 24);

// Faixa de amostras da paleta no pé de toda textura de objeto: 256 blocos de
// kSwatchBlock x kSwatchBlock, um por cor, da esquerda para a direita e de cima
// para baixo, nas últimas kSwatchRows linhas (side/4 blocos por linha de blocos;
// com side >= 256, cabem em 4 linhas de blocos).
constexpr int32_t kSwatchBlock = 4;
constexpr int32_t kSwatchRows  = 16;

// Vértice do terreno: um por canto de triângulo (malha não indexada, porque a
// cor flat e o bloco são do triângulo/quad, não do ponto da grade).
struct TerrainVertex {
    float    x, y, z;
    uint32_t color;            // cor flat do triângulo (iluminada), 0x00RRGGBB
    uint8_t  u, v, layer;      // canto do quad (0/1) e bloco do atlas
    uint8_t  light;            // intensidade da luz no triângulo, 0..255 (para o chão texturizado)
};
static_assert(sizeof(TerrainVertex) == 20);

// Vértice do vidro (rz_glass.cpp): posição, normal de fora do polígono, cinza
struct GlassVertex {
    float x, y, z;
    float nx, ny, nz;
    float tint;                // 0 (preto) .. 1 (transparente) = tom / 15
};
static_assert(sizeof(GlassVertex) == 28);

// Rodas (rz_wheels.cpp): cilindros pretos finos
constexpr int32_t  kWheelSegments = 12;
constexpr float    kWheelWidth    = 0.4f;           // largura = 0,4 x diâmetro (12:30)
constexpr uint32_t kWheelColor    = 0x001C1C1Cu;    // quase preto (para a luz ainda aparecer)

// Vidro: brilho especular "embaçado" (Blinn-Phong de expoente baixo)
constexpr float kGlassSpecular  = 0.45f;
constexpr float kGlassShininess = 12.0f;

struct Object {
    bool      alive   = false;
    bool      positioned = false;   // já recebeu rzUpdateObjectVertices
    bool      gpuDirty   = false;   // staging/VBO desatualizado: reenviar no próximo frame
    int32_t   uploadedSide = 0;     // lado da textura usado no último envio (UV das cores)
    Vec3      center = { 0.0f, 0.0f, 0.0f };   // esfera envolvente (refeita em cada update)
    float     radius = 0.0f;

    std::vector<Vec3>     world;          // posições no mundo, atualizadas pelo host
    std::vector<int32_t>  polygonStart;   // em `indices`
    std::vector<int32_t>  polygonLength;  // sem o índice de fechamento
    std::vector<uint16_t> indices;        // cópia dos índices dos polígonos (sem os fechamentos)
    std::vector<float>    uvs;            // (u, v) de cada entrada de `indices`; 0 nos de cor sólida
    std::vector<int16_t>  polygonPalette; // índice de cor (rzAddObjectPolygon) ou -1 (UVs próprios)
    std::vector<uint32_t> polygonColors;  // cor sombreada por polígono
    std::vector<ObjectTriangle> triangles; // leque de cada polígono, montado na carga
    std::vector<GpuVertex> staging;       // triangles.size() * 3, preenchido no envio

    GLuint    vao = 0;
    GLuint    vbo = 0;
    // Vidro (rz_glass.cpp): polígonos translúcidos, VBO próprio
    std::vector<int32_t>  glassStart, glassLength;
    std::vector<uint8_t>  glassTone;      // 0..15
    std::vector<uint16_t> glassIndices;
    std::vector<GlassVertex> glassStaging; // triângulos do leque, preenchido no envio
    GLuint    glassVao = 0, glassVbo = 0;
    bool      glassDirty = false;

    // Rodas (rz_wheels.cpp): staging vazio = sem rodas
    uint16_t  wheelVertex[4] = {};
    uint8_t   wheelFront[4] = {};
    float     wheelDiameter[4] = {};    // mundo
    float     wheelSteer = 0.0f;        // dianteiras; radianos, positivo = esquerda
    std::vector<TerrainVertex> wheelStaging;
    GLuint    wheelVao = 0, wheelVbo = 0;
    bool      wheelsDirty = false;

    GLuint    texture = 0;          // 0: usa a textura fallback do contexto
    int32_t   textureSize = 0;      // lado da textura (potência de 2)

    int32_t vertexCount() const   { return int32_t(world.size()); }
    int32_t triangleCount() const { return int32_t(triangles.size()); }
};

// Locais de uniforms dos programas
struct FogUniforms {
    GLint on = -1, start = -1, end = -1, color = -1, eye = -1;
};

struct ShadowUniforms {
    GLint matrices = -1, targetOn = -1;
    GLint maps[kShadowMaps] = { -1, -1, -1 };
};

struct TerrainProgram {
    GLuint program = 0;
    GLint  viewProj = -1, atlas = -1, textured = -1, filter = -1, shading = -1, ambient = -1;
    ShadowUniforms shadow;
    FogUniforms    fog;
};

struct ObjectProgram {
    GLuint program = 0;
    GLint  viewProj = -1, texture = -1, filter = -1, texSize = -1, maxLevel = -1, ambient = -1;
    ShadowUniforms shadow;
    FogUniforms    fog;
};

struct GlassProgram {
    GLuint program = 0;
    GLint  viewProj = -1, light = -1, specular = -1, shininess = -1;
    ShadowUniforms shadow;
    FogUniforms    fog;
};

struct WallProgram {
    GLuint program = 0;
    GLint  viewProj = -1, target = -1, fadeNear = -1, fadeFar = -1, xSize = -1, cellSize = -1;
    FogUniforms fog;
};

struct SpriteProgram {
    GLuint program = 0;
    GLint  viewProj = -1, right = -1, up = -1, noise = -1;
    FogUniforms fog;
};

// Sprite no mundo (rz_sprites.cpp), convertido em rzSetSprites
struct Sprite {
    Vec3     center;
    float    radius;       // mundo
    uint32_t color;        // 0x00RRGGBB, já com luz
};
struct SpriteKey { float depth; int32_t index; };    // ordenação de trás para frente
struct SpriteVertex {      // 24 bytes; seis por sprite
    float    x, y, z, radius;
    int8_t   cornerX, cornerY, frame, pad;
    uint32_t color;
};

struct DepthProgram {
    GLuint program = 0;
    GLint  lightViewProj = -1;
};

// Imagem paletizada lida de um PCX (rz_pcx.cpp)
struct PcxImage {
    int32_t width = 0, height = 0;
    std::vector<uint8_t> pixels;   // width * height, linha 0 em cima
    uint8_t palette[768];          // 256 x (R, G, B)
};

} // namespace rz

// ---------------------------------------------------------------------------
// Contexto
// ---------------------------------------------------------------------------

struct RzContext {
    rz::Platform* platform = nullptr;
    bool      windowed   = false;     // janela filha (true) ou offscreen com cópia (false)
    uint32_t* hostPixels = nullptr;   // offscreen: buffer RGBQUAD do host (não é nosso)
    int32_t   width  = 0;
    int32_t   height = 0;

    // Offscreen: FBO com cor e profundidade
    rz::GLuint fbo = 0, colorRb = 0, depthRb = 0;

    // Cor de fundo em float (0..1), convertida só em rzSetBackgroundColor
    float     fogStart = rz::kFogStartDefault;   // tiles (rzSetFog)
    float     fogEnd   = rz::kFogEndDefault;
    float     backgroundR = rz::kBackgroundR;
    float     backgroundG = rz::kBackgroundG;
    float     backgroundB = rz::kBackgroundB;

    rz::TerrainProgram terrainProgram;
    rz::ObjectProgram  objectProgram;
    rz::DepthProgram   depthProgram;
    rz::WallProgram    wallProgram;
    rz::GlassProgram   glassProgram;
    rz::SpriteProgram  spriteProgram;

    // Sprites (rz_sprites.cpp): o array do último rzSetSprites; vetores
    // reservados na criação (kMaxSprites), sem alocar depois
    std::vector<rz::Sprite>       sprites;
    std::vector<rz::SpriteKey>    spriteOrder;
    std::vector<rz::SpriteVertex> spriteStaging;
    uint32_t   spritePalette[256] = {};              // 0x00RRGGBB com luz (atlas ou cinza)
    rz::GLuint spriteVao = 0, spriteVbo = 0, spriteNoiseTex = 0;
    uint32_t   frameCount = 0;                       // rzRender chamados (quadro do ruído)

    // Borda do mundo (rz_border.cpp), refeita com a malha do terreno
    std::vector<float> extHeights;      // (255 + 2 x 128 + 2)^2, unidade do byte
    rz::GLuint skirtVao = 0, skirtVbo = 0;
    std::vector<rz::TerrainVertex> skirtStaging;    // malha da continuação na CPU (reaproveitada)
    int32_t    skirtVertexCount = 0;
    int32_t    skirtFirst[4] = {}, skirtCount[4] = {};   // regiões N, L, S, O (drawSkirt)
    rz::GLuint wallVao = 0, wallVbo = 0;
    int32_t    wallVertexCount = 0;

    // Sombras (rz_shadow.cpp): três mapas, FBO só com profundidade cada
    rz::GLuint shadowFbo[rz::kShadowMaps] = {};
    rz::GLuint shadowTex[rz::kShadowMaps] = {};
    rz::Mat4   shadowMatrix[rz::kShadowMaps] = {};     // do frame; contíguas (glUniformMatrix4fv)
    int32_t    shadowSize[rz::kShadowMaps] = {};      // lado de cada mapa (já limitado pelo driver)
    bool       terrainShadowDirty = true;            // mapa 0 precisa ser refeito...
    bool       terrainShadowAll   = true;            // ...inteiro, ou só a caixa abaixo (mundo)
    rz::Vec3   terrainShadowLo = { 0.0f, 0.0f, 0.0f }, terrainShadowHi = { 0.0f, 0.0f, 0.0f };
    bool       shadowTargetOn = false;               // mapa 2 em uso (câmera seguindo)
    rz::Vec3   shadowFocus = { 0.0f, 0.0f, 0.0f };   // centro da caixa do mapa 1 (do frame)

    // Do frame (updateCamera): olho e neblina
    rz::Vec3   eyePos = { 0.0f, 0.0f, 0.0f };
    rz::Vec3   targetPos = { 0.0f, 0.0f, 0.0f };     // vértice-alvo (parede de limite)
    rz::Vec3   camRight   = { 1.0f, 0.0f, 0.0f };    // eixos da câmera (sprites)
    rz::Vec3   camUp      = { 0.0f, 1.0f, 0.0f };
    rz::Vec3   camForward = { 0.0f, 0.0f, -1.0f };
    bool       fogOn = false;                        // seguindo um alvo
    bool       shadowWholeTerrain = true;           // visão geral: mapa 1 = terreno inteiro

    // Projeção
    float focalX = 1.0f;     // f / aspecto
    float focalY = 1.0f;     // f = 1 / tan(fov/2)

    // Terreno
    std::vector<uint8_t> heights;      // 256x256, cópia na CPU (malha e câmera de perseguição)
    float     cellSize    = rz::kDefaultCellSize;
    float     heightScale = rz::kDefaultHeightScale;
    rz::GLuint terrainVao = 0;
    rz::GLuint terrainVbo = 0;         // 130.050 x 3 TerrainVertex, remontado na carga
    std::vector<rz::TerrainVertex> terrainStaging;  // a mesma malha na CPU (rzUpdateTerrain)
    const uint8_t* heightSource = nullptr;  // buffers do host (rzSetHeightmap/rzSetTileMap),
    const uint8_t* tileSource   = nullptr;  // relidos por rzUpdateTerrain
    bool hasTerrain() const { return !heights.empty(); }

    // Texturas (opcionais; sem as duas, desenha com as cores flat)
    rz::GLuint atlasTex = 0;           // array 16x16 x 256 camadas, 5 níveis
    std::vector<uint8_t> tileMap;      // 256x256, cópia na CPU (bloco de cada quad)
    bool      hasAtlas   = false;
    bool      hasTileMap = false;
    int32_t   textureFilter = RZ_FILTER_MIP_DITHER;   // chão e objetos

    // Textura dos objetos sem textura própria (ou cuja carga falhou): xadrez
    // gerado ou PCX de rzLoadFallbackTexture
    rz::GLuint fallbackTex = 0;
    int32_t    fallbackSize = 0;

    // Câmera
    float terrainRadius = 0.0f;   // R: raio da esfera envolvente do terreno
    float nearPlane = 0.0f;       // calculados a cada frame pela posição da câmera
    float farPlane  = 0.0f;

    // Alvo da câmera: vértice de um objeto (câmera de perseguição), ou nenhum
    // (visão geral fixa do terreno)
    int32_t cameraTargetObject = -1;
    int32_t cameraTargetVertex = 0;

    // Câmera de perseguição ("na corda"); estado avança a cada rzRender
    float    followDistance  = rz::kFollowDistance;
    float    followHeight    = rz::kFollowHeight;
    float    followStiffness = rz::kFollowStiffness;
    bool     followInitialized = false;
    float    followAppliedDistance = 0.0f;   // corda com que followEye foi calculado
    rz::Vec3 followEye = { 0.0f, 0.0f, 0.0f };
    rz::Vec3 followLook = { 0.0f, 0.0f, 0.0f };   // alvo suavizado: para onde a câmera olha

    // Objetos (id = índice no vetor; slots livres são reaproveitados)
    std::vector<rz::Object> objects;
};

namespace rz {

// rz_terrain.cpp
void buildPalette(uint32_t* palette);
void updateTerrainBounds(RzContext* ctx);
bool buildTerrainMesh(RzContext* ctx);      // malha inteira + borda + sombra (carga)
bool rebuildTerrainTiles(RzContext* ctx);   // só blocos (rzSetTileMap)
bool updateTerrain(RzContext* ctx);         // rzUpdateTerrain: só o que mudou
void buildAtlasLevels(uint32_t* tiles, const uint8_t* indices, const uint8_t* paletteRGB);
void downsample(const uint32_t* src, uint32_t* dst, int32_t dstSide);
uint32_t shadeFlat(uint32_t base, Vec3 normal, bool twoSided);
Vec3 lightDirection();

// rz_pcx.cpp: lê um PCX de 8 bits inteiro. Devolve RZ_OK, RZ_ERR_FILE,
// RZ_ERR_FORMAT ou RZ_ERR_SIZE.
int32_t loadPcx(const char* path, PcxImage& img);
// rz_pcx.cpp: lê o canto 256x256 de um PCX de 8 bits (indices: 256*256 bytes,
// paletteRGB: 768 bytes). Devolve RZ_OK, RZ_ERR_FILE, RZ_ERR_FORMAT ou RZ_ERR_SIZE.
int32_t loadPcxAtlas(const char* path, uint8_t* indices, uint8_t* paletteRGB);

// rz_camera.cpp: avança a câmera um frame e devolve view-projection (convenção GL);
// também guarda o foco da sombra.
Mat4 updateCamera(RzContext* ctx);
Mat4 lookAt(Vec3 eye, Vec3 at);

// rz_shadow.cpp
bool createShadowMaps(RzContext* ctx);
void destroyShadowMaps(RzContext* ctx);
void renderShadowMaps(RzContext* ctx);
void initShadowUniforms(GLuint program, ShadowUniforms& u);
void markTerrainShadowAll(RzContext* ctx);                     // mapa 0 inteiro no próximo frame
void markTerrainShadowBox(RzContext* ctx, Vec3 lo, Vec3 hi);   // só a caixa (mundo), acumulando
void bindShadowMaps(const RzContext* ctx, const ShadowUniforms& u);

// rz_border.cpp
bool  createBorder(RzContext* ctx);
void  destroyBorder(RzContext* ctx);
void  buildBorder(RzContext* ctx);                 // carga, no fim de buildTerrainMesh
void  rebuildSkirtMesh(RzContext* ctx);            // só a malha da continuação (blocos)
void  moveSkirtEdgeVertex(RzContext* ctx, int32_t gc, int32_t gr);   // ponto da borda mudou: só o y
void  setExtendedHeight(RzContext* ctx, int32_t gc, int32_t gr, float height);   // ponto do mapa
uint8_t skirtTile(const RzContext* ctx, int32_t c, int32_t r);   // bloco de uma célula da continuação
float extendedGroundHeight(const RzContext* ctx, float x, float z);
void  drawSkirt(const RzContext* ctx);             // programa do terreno em uso
void  drawSkirtDepth(const RzContext* ctx);        // passe de sombra
void  drawWall(const RzContext* ctx, const Mat4& viewProj);

// rz_wheels.cpp
void freeWheels(Object& o);
void prepareWheels(Object& o);
void drawWheelsDepth(const Object& o);
void drawWheels(RzContext* ctx, const Mat4& viewProj);

// rz_glass.cpp
bool createGlassProgram(RzContext* ctx);
void freeGlass(Object& o);
void drawGlass(RzContext* ctx, const Mat4& viewProj);

// rz_sprites.cpp
bool createSprites(RzContext* ctx);
void destroySprites(RzContext* ctx);
void setSpritePalette(RzContext* ctx, const uint8_t* paletteRGB);   // nullptr: rampa de cinza
void drawSprites(RzContext* ctx, const Mat4& viewProj);

// rz_render.cpp
GLuint linkProgram(const char* vertexSource, const char* fragmentSource);

// rz_render.cpp: neblina (programa já em uso)
void initFogUniforms(GLuint program, FogUniforms& u);
void bindFog(const RzContext* ctx, const FogUniforms& u);

// rz_render.cpp
bool createRenderer(RzContext* ctx);
void destroyRenderer(RzContext* ctx);
bool resizeTargets(RzContext* ctx);
void applyTextureFilter(RzContext* ctx);
void renderFrame(RzContext* ctx);

// rz_texture.cpp: texturas 2D quadradas dos objetos (mipmaps na CPU)
GLuint uploadSquareTexture(const uint32_t* rgb, int32_t side, int32_t filter);
void   applyFilter2D(GLuint texture, int32_t side, int32_t filter);
bool   createFallbackTexture(RzContext* ctx);
int32_t mipLevels(int32_t side);   // log2(side) + 1
void   swatchUV(int32_t paletteIndex, int32_t side, float* u, float* v);   // centro do bloco

// rz_object.cpp
bool objectInRange(const RzContext* ctx, const Object& o);   // não está todo além da neblina
void prepareObjects(RzContext* ctx);          // reenvia os VBOs alterados
// passe da sombra (programa já ligado): onlyId >= 0 desenha só ele; skipId >= 0 pula ele
void drawObjectsDepth(RzContext* ctx, int32_t onlyId, int32_t skipId);
void drawObjects(RzContext* ctx, const Mat4& viewProj);
void destroyAllObjects(RzContext* ctx);

} // namespace rz
