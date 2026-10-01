// Tipos e funções internas do Renderizeitor (versão OpenGL). Não faz parte da
// interface pública.
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdlib>

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
constexpr uint32_t kYawStep         = 1u << 22;                 // 1024 frames por volta

constexpr float kPi              = 3.14159265358979f;
constexpr float kAngleToRadians  = 2.0f * kPi / 4294967296.0f;   // 2π / 2^32
constexpr float kPitchDegrees    = 35.0f;   // padrão
constexpr float kPitchMinDegrees = 0.0f;
constexpr float kPitchMaxDegrees = 89.0f;
constexpr float kFovYDegrees     = 60.0f;
constexpr float kAmbient         = 0.3f;

// Escala do legado: byte 0 = altura 0, byte 255 = 16 tiles (mundo 255 x 255 x 16)
constexpr float kDefaultCellSize    = 1.0f;
constexpr float kDefaultHeightScale = 16.0f / 255.0f;

constexpr float kFollowDistance  = 12.0f;   // padrões da câmera de perseguição
constexpr float kFollowHeight    = 3.0f;
constexpr float kFollowStiffness = 0.08f;
constexpr float kFollowClearance    = 2.0f;   // altura mínima desejada sobre o chão (tiles)
constexpr float kFollowMinClearance = 0.3f;   // limite duro sobre o chão (tiles)
constexpr float kFollowClimb        = 0.2f;   // amortecimento ao subir

constexpr float kDistanceMin = 0.02f;   // fator sobre a distância padrão D = 2R
constexpr float kDistanceMax = 4.0f;

constexpr int32_t kMaxWindowSize = 8192;

// Texturas: blocos 16x16 numa textura array (uma camada por bloco), com
// mipmaps 16, 8, 4, 2, 1 gerados na carga (média 2x2, como no software).
constexpr int32_t kTileSize      = 16;
constexpr int32_t kMipLevels     = 5;
constexpr int32_t kMaxTiles      = 256;                         // índice é um byte
constexpr uint32_t kMissingTileColor = 0x00FF00FFu;             // bloco fora do atlas

// ---------------------------------------------------------------------------
// Alocação (trocável no futuro)
// ---------------------------------------------------------------------------

inline void* rzAlloc(size_t bytes) { return std::malloc(bytes); }
inline void  rzFree(void* p)       { std::free(p); }

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
    static Mat4 translation(float x, float y, float z);
    static Mat4 rotationX(float s, float c);
    static Mat4 rotationY(float s, float c);
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
    uint8_t  u, v, layer, pad; // canto do quad (0/1) e bloco do atlas
};
static_assert(sizeof(TerrainVertex) == 20);

struct Object {
    bool      alive;
    bool      visible;
    int32_t   cull;            // RZ_CULL_*
    uint32_t  baseColor;       // 0x00RRGGBB, provisório até as texturas

    int32_t   vertexCount;
    Vec3*     world;           // posições no mundo (float), atualizadas pelo host

    int32_t   polygonCount;
    int32_t*  polygonStart;    // em `indices`
    int32_t*  polygonLength;   // sem o índice de fechamento
    uint16_t* indices;         // cópia dos índices (sem os fechamentos)
    uint32_t* polygonColors;   // cor sombreada por polígono

    int32_t   triangleCount;
    ObjectTriangle* triangles; // leque de cada polígono, montado na carga
    GpuVertex* staging;        // triangleCount * 3, preenchido a cada update

    GLuint    vao;
    GLuint    vbo;
};

// Locais de uniforms dos programas
struct TerrainProgram {
    GLuint program;
    GLint  viewProj, atlas, textured, filter;
};

struct ObjectProgram {
    GLuint program;
    GLint  viewProj;
};

} // namespace rz

// ---------------------------------------------------------------------------
// Contexto
// ---------------------------------------------------------------------------

struct RzContext {
    rz::Platform* platform;
    bool      windowed;        // janela filha (true) ou offscreen com cópia (false)
    uint32_t* hostPixels;      // offscreen: buffer RGBQUAD do host
    int32_t   width;
    int32_t   height;

    // Offscreen: FBO com cor e profundidade
    rz::GLuint fbo, colorRb, depthRb;

    rz::TerrainProgram terrainProgram;
    rz::ObjectProgram  objectProgram;

    // Projeção
    float focalX;           // f / aspecto
    float focalY;           // f = 1 / tan(fov/2)
    float sinPitch, cosPitch;

    // Terreno
    bool      hasTerrain;
    uint8_t*  heights;      // 256x256, cópia na CPU (malha e câmera de perseguição)
    float     cellSize;
    float     heightScale;
    rz::GLuint terrainVao;
    rz::GLuint terrainVbo;  // 130.050 x 3 TerrainVertex, remontado na carga

    // Texturas (opcionais; sem as duas, desenha com as cores flat)
    rz::GLuint atlasTex;    // array 16x16 x 256 camadas, 5 níveis
    uint8_t*  tileMap;      // 256x256, cópia na CPU (bloco de cada quad)
    bool      hasAtlas;
    bool      hasTileMap;
    int32_t   textureFilter;  // RZ_FILTER_*

    // Câmera
    float distanceFactor;   // 1 = padrão
    float terrainRadius;    // R: raio da esfera envolvente do terreno
    float orbitDistance;    // D = 2R · fator
    float nearPlane;        // calculados a cada frame pela posição da câmera
    float farPlane;
    uint32_t yaw;           // 2^32 = uma volta
    uint32_t yawStep;       // somado a cada frame (wrap natural; negativo = sentido inverso)

    // Alvo da câmera: vértice de um objeto (câmera de perseguição), ou nenhum
    int32_t cameraTargetObject;
    int32_t cameraTargetVertex;

    // Câmera de perseguição ("na corda"); estado avança a cada rzRender
    float    followDistance;
    float    followHeight;
    float    followStiffness;
    bool     followInitialized;
    rz::Vec3 followEye;

    // Objetos (id = índice no array; slots livres são reaproveitados)
    rz::Object* objects;
    int32_t     objectCapacity;
    int32_t     objectAxes;     // RZ_AXES_*
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
