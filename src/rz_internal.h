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

constexpr uint32_t kBackgroundColor = 0x00202830u;

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

constexpr int32_t kMaxWindowSize = 8192;

// Texturas: blocos 16x16 numa textura array (uma camada por bloco), com
// mipmaps 16, 8, 4, 2, 1 gerados na carga (média 2x2, como no software).
constexpr int32_t kTileSize      = 16;
constexpr int32_t kMipLevels     = 5;
constexpr int32_t kMaxTiles      = 256;                         // índice é um byte
constexpr uint32_t kMissingTileColor = 0x00FF00FFu;             // bloco fora do atlas

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
struct ObjectTriangle {
    uint16_t a, b, c;
    uint16_t polygon;
};

// Vértice enviado à GPU: posição no mundo e cor flat (0x00RRGGBB; em memória
// B, G, R, 0, lido no shader como vec4 normalizado e trocado para .bgr).
struct GpuVertex {
    float    x, y, z;
    uint32_t color;
};
static_assert(sizeof(GpuVertex) == 16);

// Vértice do terreno: um por canto de triângulo (malha não indexada, porque a
// cor flat e o bloco são do triângulo/quad, não do ponto da grade).
struct TerrainVertex {
    float    x, y, z;
    uint32_t color;            // cor flat do triângulo (iluminada), 0x00RRGGBB
    uint8_t  u, v, layer;      // canto do quad (0/1) e bloco do atlas
    uint8_t  light;            // intensidade da luz no triângulo, 0..255 (para o chão texturizado)
};
static_assert(sizeof(TerrainVertex) == 20);

struct Object {
    bool      alive   = false;
    bool      visible = false;
    int32_t   cull    = RZ_CULL_NONE;
    uint32_t  baseColor = 0;   // 0x00RRGGBB, provisório até as texturas

    std::vector<Vec3>     world;          // posições no mundo, atualizadas pelo host
    std::vector<int32_t>  polygonStart;   // em `indices`
    std::vector<int32_t>  polygonLength;  // sem o índice de fechamento
    std::vector<uint16_t> indices;        // cópia dos índices (sem os fechamentos)
    std::vector<uint32_t> polygonColors;  // cor sombreada por polígono
    std::vector<ObjectTriangle> triangles; // leque de cada polígono, montado na carga
    std::vector<GpuVertex> staging;       // triangles.size() * 3, preenchido a cada update

    GLuint    vao = 0;
    GLuint    vbo = 0;

    int32_t vertexCount() const   { return int32_t(world.size()); }
    int32_t triangleCount() const { return int32_t(triangles.size()); }
};

// Locais de uniforms dos programas
struct TerrainProgram {
    GLuint program = 0;
    GLint  viewProj = -1, atlas = -1, textured = -1, filter = -1, shading = -1;
};

struct ObjectProgram {
    GLuint program = 0;
    GLint  viewProj = -1;
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

    rz::TerrainProgram terrainProgram;
    rz::ObjectProgram  objectProgram;

    // Projeção
    float focalX = 1.0f;     // f / aspecto
    float focalY = 1.0f;     // f = 1 / tan(fov/2)

    // Terreno
    std::vector<uint8_t> heights;      // 256x256, cópia na CPU (malha e câmera de perseguição)
    float     cellSize    = rz::kDefaultCellSize;
    float     heightScale = rz::kDefaultHeightScale;
    rz::GLuint terrainVao = 0;
    rz::GLuint terrainVbo = 0;         // 130.050 x 3 TerrainVertex, remontado na carga
    bool hasTerrain() const { return !heights.empty(); }

    // Texturas (opcionais; sem as duas, desenha com as cores flat)
    rz::GLuint atlasTex = 0;           // array 16x16 x 256 camadas, 5 níveis
    std::vector<uint8_t> tileMap;      // 256x256, cópia na CPU (bloco de cada quad)
    bool      hasAtlas   = false;
    bool      hasTileMap = false;
    int32_t   textureFilter = RZ_FILTER_MIP_DITHER;

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

    // Objetos (id = índice no vetor; slots livres são reaproveitados)
    std::vector<rz::Object> objects;
    int32_t objectAxes = RZ_AXES_Z_UP;
};

namespace rz {

// rz_terrain.cpp
void buildPalette(uint32_t* palette);
void updateTerrainBounds(RzContext* ctx);
bool buildTerrainMesh(RzContext* ctx);
void buildAtlasLevels(uint32_t* tiles, const uint8_t* indices, int32_t width, int32_t height,
                      const uint8_t* paletteRGB);
uint32_t shadeFlat(uint32_t base, Vec3 normal, bool twoSided);
Vec3 lightDirection();

// rz_camera.cpp: avança a câmera um frame e devolve view-projection (convenção GL)
Mat4 updateCamera(RzContext* ctx);

// rz_render.cpp
bool createRenderer(RzContext* ctx);
void destroyRenderer(RzContext* ctx);
bool resizeTargets(RzContext* ctx);
void applyTextureFilter(RzContext* ctx);
void renderFrame(RzContext* ctx);

// rz_object.cpp
void drawObjects(RzContext* ctx, const Mat4& viewProj, bool flipped);
void destroyAllObjects(RzContext* ctx);

} // namespace rz
