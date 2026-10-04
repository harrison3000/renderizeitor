// Dados derivados do terreno calculados na CPU, na carga: malha com cores flat
// iluminadas, paleta pela altura, níveis de mipmap do atlas e esfera envolvente.

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

struct Rgb {
    float r, g, b;
};

struct PaletteBand {
    float lo, hi;       // faixa de altura média, na escala do byte (inclusiva)
    Rgb   from, to;     // gradiente dentro da faixa
};

// Valores provisórios (spec 7.2).
constexpr PaletteBand kBands[] = {
    {   0.0f,  40.0f, {  22.0f,  52.0f, 120.0f }, {  45.0f,  95.0f, 170.0f } },  // água
    {  41.0f,  60.0f, { 194.0f, 178.0f, 128.0f }, { 212.0f, 196.0f, 146.0f } },  // areia
    {  61.0f, 150.0f, {  78.0f, 148.0f,  56.0f }, {  38.0f,  98.0f,  34.0f } },  // grama
    { 151.0f, 210.0f, { 118.0f, 104.0f,  88.0f }, { 152.0f, 142.0f, 132.0f } },  // rocha
    { 211.0f, 255.0f, { 228.0f, 228.0f, 234.0f }, { 255.0f, 255.0f, 255.0f } },  // neve
};

uint32_t packColor(float r, float g, float b) {
    auto channel = [](float v) -> uint32_t {
        if (v <= 0.0f) return 0;
        if (v >= 255.0f) return 255;
        return uint32_t(v + 0.5f);
    };
    return (channel(r) << 16) | (channel(g) << 8) | channel(b);   // byte reservado = 0
}

} // namespace

// Nível L+1 de uma imagem quadrada a partir do nível L: média 2x2 por canal,
// arredondada. Usado no atlas e nas texturas dos objetos.
void downsample(const uint32_t* src, uint32_t* dst, int32_t dstSide) {
    const int32_t srcSide = dstSide * 2;
    for (int32_t y = 0; y < dstSide; ++y) {
        for (int32_t x = 0; x < dstSide; ++x) {
            const uint32_t p0 = src[(2 * y) * srcSide + 2 * x];
            const uint32_t p1 = src[(2 * y) * srcSide + 2 * x + 1];
            const uint32_t p2 = src[(2 * y + 1) * srcSide + 2 * x];
            const uint32_t p3 = src[(2 * y + 1) * srcSide + 2 * x + 1];
            uint32_t out = 0;
            for (int shift = 0; shift <= 16; shift += 8) {
                const uint32_t sum = ((p0 >> shift) & 0xFF) + ((p1 >> shift) & 0xFF)
                                   + ((p2 >> shift) & 0xFF) + ((p3 >> shift) & 0xFF);
                out |= ((sum + 2) >> 2) << shift;
            }
            dst[y * dstSide + x] = out;
        }
    }
}

// Direção (normalizada) para a luz, fixa no mundo. Provisória.
Vec3 lightDirection() {
    Vec3 l = { -0.45f, 0.45f, -0.40f };   // elevação :)
    const float inv = 1.0f / sqrtf(dot(l, l));
    return { l.x * inv, l.y * inv, l.z * inv };
}

// Cor flat de uma face com normal `normal` (não precisa estar normalizada).
// twoSided: ilumina pelos dois lados (|n·L|); hoje ninguém usa (os objetos
// passaram à luz de um lado só quando o winding do legado foi definido).
uint32_t shadeFlat(uint32_t base, Vec3 normal, bool twoSided) {
    const Vec3 light = lightDirection();
    const float len2 = dot(normal, normal);
    float ndotl = 0.0f;
    if (len2 > 0.0f) ndotl = dot(normal, light) / sqrtf(len2);
    if (twoSided && ndotl < 0.0f) ndotl = -ndotl;
    if (ndotl < 0.0f) ndotl = 0.0f;
    const float intensity = kAmbient + (1.0f - kAmbient) * ndotl;
    return packColor(float((base >> 16) & 0xFF) * intensity,
                     float((base >> 8) & 0xFF) * intensity,
                     float(base & 0xFF) * intensity);
}

// Paleta indexada pela soma das 3 alturas de um triângulo (0..765).
void buildPalette(uint32_t* palette) {
    constexpr float third = 1.0f / 3.0f;
    for (int sum = 0; sum < kPaletteSize; ++sum) {
        const float avg = float(sum) * third;
        const PaletteBand* band = &kBands[0];
        for (const PaletteBand& b : kBands) {
            if (avg >= b.lo - 0.5f) band = &b;    // bandas em ordem crescente
        }
        float t = (avg - band->lo) / (band->hi - band->lo);
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        palette[sum] = packColor(band->from.r + (band->to.r - band->from.r) * t,
                                 band->from.g + (band->to.g - band->from.g) * t,
                                 band->from.b + (band->to.b - band->from.b) * t);
    }
}

// Malha do terreno (spec 7.1), um vértice por canto de triângulo:
// diagonal do legado, (c+1, r)–(c, r+1). Cantos  1 2  ->  A = 1 3 2, B = 2 3 4:
//                                                  3 4
//   A = (c, r), (c, r+1), (c+1, r)      B = (c+1, r), (c, r+1), (c+1, r+1)
// Cor flat: paleta pela soma das 3 alturas, iluminada pela normal da face.
// u ao longo da coluna, v ao longo da linha; bloco do quad pelo mapa de blocos.
// A malha inteira fica também na CPU (ctx->terrainStaging, row-major, 6
// vértices por quad), para rzUpdateTerrain refazer só um retângulo de quads
// e reenviar só a faixa de linhas dele.
namespace {

// Quads c0..c1 x r0..r1 (inclusivos) -> staging
void buildQuads(RzContext* ctx, int32_t c0, int32_t r0, int32_t c1, int32_t r1) {
    uint32_t palette[kPaletteSize];
    buildPalette(palette);
    const Vec3 light = lightDirection();
    const uint8_t* h = ctx->heights.data();
    const float cs = ctx->cellSize;
    const float hs = ctx->heightScale;

    struct Corner { int32_t dc, dr; };
    constexpr Corner kCorners[2][3] = { { { 0, 0 }, { 0, 1 }, { 1, 0 } },     // A = 1 3 2
                                        { { 1, 0 }, { 0, 1 }, { 1, 1 } } };   // B = 2 3 4
    for (int32_t r = r0; r <= r1; ++r) {
        TerrainVertex* v = ctx->terrainStaging.data() + size_t(r * kQuadsPerSide + c0) * 6;
        for (int32_t c = c0; c <= c1; ++c) {
            const uint8_t layer = ctx->tileMap.empty() ? 0 : ctx->tileMap[r * kGridSize + c];
            for (const auto& tri : kCorners) {
                Vec3 p[3];
                int32_t sum = 0;
                for (int32_t k = 0; k < 3; ++k) {
                    const int32_t gc = c + tri[k].dc, gr = r + tri[k].dr;
                    const int32_t height = h[gr * kGridSize + gc];
                    sum += height;
                    p[k] = { float(gc) * cs, float(height) * hs, float(gr) * cs };
                }
                const Vec3 n = cross(p[1] - p[0], p[2] - p[0]);
                const float len2 = dot(n, n);
                float ndotl = len2 > 0.0f ? dot(n, light) / sqrtf(len2) : 0.0f;
                if (ndotl < 0.0f) ndotl = 0.0f;
                const float intensity = kAmbient + (1.0f - kAmbient) * ndotl;
                const uint8_t lightByte = uint8_t(intensity * 255.0f + 0.5f);
                const uint32_t base = palette[sum];
                const uint32_t color = packColor(float((base >> 16) & 0xFF) * intensity,
                                                 float((base >> 8) & 0xFF) * intensity,
                                                 float(base & 0xFF) * intensity);
                for (int32_t k = 0; k < 3; ++k) {
                    *v++ = { p[k].x, p[k].y, p[k].z, color,
                             uint8_t(tri[k].dc), uint8_t(tri[k].dr), layer, lightByte };
                }
            }
        }
    }
}

// Reenvia do quad (c0, r0) ao (c1, r1) na ordem row-major (faixa contígua)
void uploadQuads(RzContext* ctx, int32_t c0, int32_t r0, int32_t c1, int32_t r1) {
    const size_t first = size_t(r0 * kQuadsPerSide + c0) * 6;
    const size_t last  = size_t(r1 * kQuadsPerSide + c1 + 1) * 6;
    glBindBuffer(GL_ARRAY_BUFFER, ctx->terrainVbo);
    glBufferSubData(GL_ARRAY_BUFFER, GLintptr(first * sizeof(TerrainVertex)),
                    GLsizeiptr((last - first) * sizeof(TerrainVertex)), ctx->terrainStaging.data() + first);
}

constexpr int32_t kLastQuad = kQuadsPerSide - 1;   // 254

} // namespace

// Malha inteira + borda; mapa de sombra do terreno inteiro refeito.
// rzSetHeightmap e rzSetTerrainScale (carga).
bool buildTerrainMesh(RzContext* ctx) {
    ctx->terrainStaging.resize(size_t(kTriangleCount) * 3);   // carga: aloca na primeira vez
    buildQuads(ctx, 0, 0, kLastQuad, kLastQuad);
    uploadQuads(ctx, 0, 0, kLastQuad, kLastQuad);
    buildBorder(ctx);                        // continuação e parede dependem das alturas/blocos
    markTerrainShadowAll(ctx);
    return glGetError() == GL_NO_ERROR;
}

// Só os blocos mudaram (rzSetTileMap): bloco dos quads e da continuação; as
// alturas, a borda de relevo e a sombra ficam.
bool rebuildTerrainTiles(RzContext* ctx) {
    ctx->terrainStaging.resize(size_t(kTriangleCount) * 3);
    buildQuads(ctx, 0, 0, kLastQuad, kLastQuad);
    uploadQuads(ctx, 0, 0, kLastQuad, kLastQuad);
    rebuildSkirtMesh(ctx);
    return glGetError() == GL_NO_ERROR;
}

// rzUpdateTerrain: relê os buffers do host (ponteiros guardados), compara com
// as cópias e, para cada ponto/quad que mudou (no jogo são poucos):
//   - altura: refaz e reenvia os 4 quads em volta do ponto, atualiza a cópia
//     estendida (câmera) e marca a sombra do relevo só ali (com as alturas
//     antiga e nova); se o ponto é da borda do mapa, só move junto o y das
//     vértices do terreno de fora que estão em cima dele (moveSkirtEdgeVertex;
//     o resto de fora não é recalculado: aceito, rzSetHeightmap refaz tudo);
//   - bloco: refaz e reenvia o quad; se está a menos de kSkirtTileBand quads
//     da borda, refaz a malha da continuação (ela copia/sorteia dessa faixa).
// Sem mudança, não faz nada. Não aloca.
bool updateTerrain(RzContext* ctx) {
    const float cs = ctx->cellSize, hs = ctx->heightScale;
    auto refreshQuads = [&](int32_t c0, int32_t r0, int32_t c1, int32_t r1) {   // inclusivos, recortados
        if (c0 < 0) c0 = 0;
        if (r0 < 0) r0 = 0;
        if (c1 > kLastQuad) c1 = kLastQuad;
        if (r1 > kLastQuad) r1 = kLastQuad;
        buildQuads(ctx, c0, r0, c1, r1);
        for (int32_t r = r0; r <= r1; ++r) uploadQuads(ctx, c0, r, c1, r);
    };

    if (ctx->heightSource) {
        const uint8_t* src = ctx->heightSource;
        uint8_t* h = ctx->heights.data();
        for (int32_t i = 0; i < kVertexCount; ++i) {
            if (src[i] == h[i]) continue;
            const int32_t gc = i % kGridSize, gr = i / kGridSize;
            const float yOld = float(h[i]) * hs, yNew = float(src[i]) * hs;
            h[i] = src[i];
            setExtendedHeight(ctx, gc, gr, float(src[i]));
            refreshQuads(gc - 1, gr - 1, gc, gr);
            if (gc == 0 || gr == 0 || gc == kQuadsPerSide || gr == kQuadsPerSide) moveSkirtEdgeVertex(ctx, gc, gr);
            // sombra: caixa dos quads em volta (1 tile para cada lado), com a
            // altura antiga, a nova e a dos 8 vizinhos
            float lo = yOld < yNew ? yOld : yNew, hi = yOld < yNew ? yNew : yOld;
            for (int32_t dr = -1; dr <= 1; ++dr)
                for (int32_t dc = -1; dc <= 1; ++dc) {
                    const int32_t c = gc + dc, r = gr + dr;
                    if (c < 0 || r < 0 || c > kQuadsPerSide || r > kQuadsPerSide) continue;
                    const float y = float(h[r * kGridSize + c]) * hs;
                    if (y < lo) lo = y;
                    if (y > hi) hi = y;
                }
            markTerrainShadowBox(ctx, { float(gc - 1) * cs, lo, float(gr - 1) * cs },
                                      { float(gc + 1) * cs, hi, float(gr + 1) * cs });
        }
    }
    if (ctx->tileSource && ctx->hasTileMap) {
        const uint8_t* src = ctx->tileSource;
        uint8_t* t = ctx->tileMap.data();
        bool edgeTile = false;
        for (int32_t r = 0; r < kQuadsPerSide; ++r) {
            for (int32_t c = 0; c < kQuadsPerSide; ++c) {
                const int32_t i = r * kGridSize + c;
                if (src[i] == t[i]) continue;
                t[i] = src[i];
                refreshQuads(c, r, c, r);
                if (c < kSkirtTileBand || r < kSkirtTileBand ||
                    c > kLastQuad - kSkirtTileBand || r > kLastQuad - kSkirtTileBand) edgeTile = true;
            }
        }
        // ~3 ms (llvmpipe). Otimização futura possível: só os triângulos de fora
        // que copiam deste quad (ver spec 12 e o branch super_complicated_border_update)
        if (edgeTile) rebuildSkirtMesh(ctx);
    }
    return glGetError() == GL_NO_ERROR;
}

// Esfera envolvente: raio = meia diagonal da caixa do terreno. Usada pela
// visão geral e pelos planos near/far.
void updateTerrainBounds(RzContext* ctx) {
    const float hx = 127.5f * ctx->cellSize;
    const float hy = 127.5f * ctx->heightScale;
    ctx->terrainRadius = sqrtf(hx * hx + hy * hy + hx * hx);
}

// Atlas paletizado 256x256 (canto do PCX) -> 256 blocos 16x16 em 0x00RRGGBB
// com os 5 níveis de mipmap, organizados por nível para subir com
// glTexImage3D: nível L começa em tiles + 256 · kLevelOffset[L].
void buildAtlasLevels(uint32_t* tiles, const uint8_t* indices, const uint8_t* paletteRGB) {
    constexpr int32_t kLevelOffset[kMipLevels] = { 0, 256, 320, 336, 340 };   // texels por bloco
    constexpr int32_t kTilesPerRow = kAtlasSize / kTileSize;

    uint32_t pal[256];
    for (int i = 0; i < 256; ++i) {
        pal[i] = (uint32_t(paletteRGB[i * 3 + 0]) << 16)
               | (uint32_t(paletteRGB[i * 3 + 1]) << 8)
               |  uint32_t(paletteRGB[i * 3 + 2]);
    }

    for (int32_t t = 0; t < kMaxTiles; ++t) {
        uint32_t* level0 = tiles + t * 256;
        const int32_t tx = (t % kTilesPerRow) * kTileSize;
        const int32_t ty = (t / kTilesPerRow) * kTileSize;
        for (int32_t y = 0; y < kTileSize; ++y) {
            const uint8_t* src = indices + (ty + y) * kAtlasSize + tx;
            for (int32_t x = 0; x < kTileSize; ++x) level0[y * kTileSize + x] = pal[src[x]];
        }
        for (int32_t level = 1; level < kMipLevels; ++level) {
            const int32_t side = kTileSize >> level;
            const int32_t prevSide = side * 2;
            const uint32_t* prev = tiles + kMaxTiles * kLevelOffset[level - 1] + t * prevSide * prevSide;
            uint32_t* cur = tiles + kMaxTiles * kLevelOffset[level] + t * side * side;
            downsample(prev, cur, side);
        }
    }
}

} // namespace rz
