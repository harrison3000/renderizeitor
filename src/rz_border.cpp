// Borda do mundo: "continuação" do terreno além das extremidades do mapa e a
// parede de limite da área jogável.
//
// Continuação: até kSkirtExtent tiles além de cada borda (o alcance da
// neblina + folga), gerada na carga a partir do próprio mapa:
//   - altura: parte da altura da borda (o ponto mais próximo do mapa) e, ao
//     longo de kSkirtBlend tiles, vai para (média das bordas + ruído de valor
//     em duas oitavas). Na emenda com o mapa, a altura é a da borda: sem degrau.
//   - bloco de textura: escolhido entre os blocos que o próprio mapa usa na
//     mesma faixa de altura (16 faixas), por hash da célula.
//   - malha: células de 1 tile numa faixa de kSkirtFine tiles em volta do mapa
//     e de kSkirtCoarse tiles daí em diante (longe, a neblina cobre). Na linha
//     onde as duas se encontram, as alturas dos pontos intermediários são
//     interpoladas, para não abrir frestas (T-junctions).
//   - só é desenhada seguindo um alvo (com neblina); projeta sombra no mapa 0.
//   - groundHeight (câmera) usa as mesmas alturas, com a mesma resolução.
//
// Parede: em cima das quatro bordas do mapa, do chão até kBorderWallHeight
// tiles acima, semitransparente, com X vermelhos de kBorderXSize tiles. Só
// aparece perto do alvo da câmera: alfa = 1 - smoothstep(near, far, distância
// horizontal do alvo ao ponto da parede). Desenhada depois do opaco, com
// blending e sem gravar profundidade; não projeta nem recebe sombra.

#include <cmath>

#include "rz_internal.h"
#include "rz_shaders.h"

namespace rz {

namespace {

constexpr int32_t kExtSide = kQuadsPerSide + 2 * kSkirtExtent + 1;   // pontos por lado (433)
constexpr int32_t kFineLo = -kSkirtFine;                             // -16
constexpr int32_t kFineHi = kQuadsPerSide + kSkirtFine + 1;          // 272 (288 = 72 x 4)
static_assert((kFineHi - kFineLo) % kSkirtCoarse == 0, "faixa fina alinhada às células grossas");
static_assert((kSkirtExtent - kSkirtFine) % kSkirtCoarse == 0, "faixa grossa alinhada");

struct WallVertex {
    float x, y, z;
    float u, v;            // tiles ao longo da parede / acima da base
};

uint32_t hash2(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// Ruído de valor (bilinear suavizado), 0..1
float valueNoise(float x, float y, int32_t period, uint32_t seed) {
    const float fx = x / float(period), fy = y / float(period);
    const float x0f = floorf(fx), y0f = floorf(fy);
    const int32_t x0 = int32_t(x0f), y0 = int32_t(y0f);
    float tx = fx - x0f, ty = fy - y0f;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    auto at = [&](int32_t ix, int32_t iy) { return float(hash2(ix, iy, seed) & 0xFFFFu) / 65535.0f; };
    const float a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

float& ext(RzContext* ctx, int32_t gx, int32_t gz) {
    return ctx->extHeights[size_t(gz + kSkirtExtent) * kExtSide + size_t(gx + kSkirtExtent)];
}

// Alturas (unidade do byte, float) de -kSkirtExtent a 255 + kSkirtExtent
void buildExtendedHeights(RzContext* ctx) {
    ctx->extHeights.assign(size_t(kExtSide) * kExtSide, 0.0f);
    const uint8_t* h = ctx->heights.data();

    // Altura base: média das bordas
    float sum = 0.0f;
    for (int32_t i = 0; i < kGridSize; ++i) {
        sum += float(h[i]) + float(h[kQuadsPerSide * kGridSize + i])
             + float(h[i * kGridSize]) + float(h[i * kGridSize + kQuadsPerSide]);
    }
    const float base = sum / float(4 * kGridSize);

    for (int32_t gz = -kSkirtExtent; gz <= kQuadsPerSide + kSkirtExtent; ++gz) {
        for (int32_t gx = -kSkirtExtent; gx <= kQuadsPerSide + kSkirtExtent; ++gx) {
            const int32_t cx = gx < 0 ? 0 : (gx > kQuadsPerSide ? kQuadsPerSide : gx);
            const int32_t cz = gz < 0 ? 0 : (gz > kQuadsPerSide ? kQuadsPerSide : gz);
            const float edge = float(h[cz * kGridSize + cx]);
            if (cx == gx && cz == gz) {               // dentro do mapa
                ext(ctx, gx, gz) = edge;
                continue;
            }
            const float dx = float(gx - cx), dz = float(gz - cz);
            const float d = sqrtf(dx * dx + dz * dz);
            float w = d / kSkirtBlend;
            if (w > 1.0f) w = 1.0f;
            w = w * w * (3.0f - 2.0f * w);
            const float noise = (valueNoise(float(gx), float(gz), 32, 11u) - 0.5f) * kSkirtNoiseLow
                              + (valueNoise(float(gx), float(gz), 12, 23u) - 0.5f) * kSkirtNoiseHigh;
            float v = edge + (base + noise - edge) * w;
            if (v < 0.0f) v = 0.0f;
            if (v > 255.0f) v = 255.0f;
            ext(ctx, gx, gz) = v;
        }
    }

    // Emenda fina/grossa: nas linhas da borda da faixa fina, os pontos entre
    // os cantos das células grossas ficam na reta entre eles (sem frestas)
    auto fixLine = [&](bool alongX, int32_t fixed) {
        for (int32_t s = kFineLo; s < kFineHi; s += kSkirtCoarse) {
            const float a = alongX ? ext(ctx, s, fixed) : ext(ctx, fixed, s);
            const float b = alongX ? ext(ctx, s + kSkirtCoarse, fixed) : ext(ctx, fixed, s + kSkirtCoarse);
            for (int32_t k = 1; k < kSkirtCoarse; ++k) {
                const float v = a + (b - a) * float(k) / float(kSkirtCoarse);
                if (alongX) ext(ctx, s + k, fixed) = v; else ext(ctx, fixed, s + k) = v;
            }
        }
    };
    fixLine(true, kFineLo);  fixLine(true, kFineHi);
    fixLine(false, kFineLo); fixLine(false, kFineHi);
}

// Blocos usados pelo mapa em cada faixa de altura (16 faixas de 16)
struct TileBuckets {
    std::vector<uint8_t> tiles[16];
};

void buildTileBuckets(const RzContext* ctx, TileBuckets& b) {
    if (ctx->tileMap.empty()) return;
    bool seen[16][256] = {};
    const uint8_t* h = ctx->heights.data();
    for (int32_t r = 0; r < kQuadsPerSide; ++r) {
        for (int32_t c = 0; c < kQuadsPerSide; ++c) {
            const int32_t avg = (h[r * kGridSize + c] + h[r * kGridSize + c + 1]
                               + h[(r + 1) * kGridSize + c] + h[(r + 1) * kGridSize + c + 1]) / 4;
            const uint8_t t = ctx->tileMap[r * kGridSize + c];
            if (!seen[avg >> 4][t]) {
                seen[avg >> 4][t] = true;
                b.tiles[avg >> 4].push_back(t);
            }
        }
    }
}

uint8_t pickTile(const TileBuckets& b, float avgHeight, int32_t c, int32_t r) {
    int32_t bucket = int32_t(avgHeight) >> 4;
    if (bucket < 0) bucket = 0;
    if (bucket > 15) bucket = 15;
    for (int32_t dist = 0; dist < 16; ++dist) {          // faixa mais próxima que tenha blocos
        for (int32_t s = -1; s <= 1; s += 2) {
            const int32_t k = bucket + s * dist;
            if (k < 0 || k > 15 || b.tiles[k].empty()) continue;
            return b.tiles[k][hash2(c, r, 77u) % b.tiles[k].size()];
        }
    }
    return 0;
}

// Uma célula (c, r)-(c+s, r+s): dois triângulos com a diagonal do mapa
void emitCell(const RzContext* ctx, std::vector<TerrainVertex>& out, const uint32_t* palette,
              int32_t c, int32_t r, int32_t s, uint8_t layer) {
    const float cs = ctx->cellSize, hs = ctx->heightScale;
    const Vec3 light = lightDirection();
    struct Corner { int32_t dc, dr; };
    constexpr Corner kCorners[2][3] = { { { 0, 0 }, { 0, 1 }, { 1, 1 } },
                                        { { 0, 0 }, { 1, 1 }, { 1, 0 } } };
    for (const auto& tri : kCorners) {
        Vec3 p[3];
        float sum = 0.0f;
        for (int32_t k = 0; k < 3; ++k) {
            const int32_t gc = c + tri[k].dc * s, gr = r + tri[k].dr * s;
            const float height = ctx->extHeights[size_t(gr + kSkirtExtent) * kExtSide + size_t(gc + kSkirtExtent)];
            sum += height;
            p[k] = { float(gc) * cs, height * hs, float(gr) * cs };
        }
        const Vec3 n = cross(p[1] - p[0], p[2] - p[0]);
        const float len2 = dot(n, n);
        float ndotl = len2 > 0.0f ? dot(n, light) / sqrtf(len2) : 0.0f;
        if (ndotl < 0.0f) ndotl = 0.0f;
        const float intensity = kAmbient + (1.0f - kAmbient) * ndotl;
        int32_t idx = int32_t(sum + 0.5f);
        if (idx > kPaletteSize - 1) idx = kPaletteSize - 1;
        const uint32_t color = shadeFlat(palette[idx], n, false);
        const uint8_t l = uint8_t(intensity * 255.0f + 0.5f);
        for (int32_t k = 0; k < 3; ++k) {
            out.push_back({ p[k].x, p[k].y, p[k].z, color,
                            uint8_t(tri[k].dc), uint8_t(tri[k].dr), layer, l });
        }
    }
}

void buildSkirtMesh(RzContext* ctx) {
    uint32_t palette[kPaletteSize];
    buildPalette(palette);
    TileBuckets buckets;
    buildTileBuckets(ctx, buckets);

    std::vector<TerrainVertex> mesh;                         // carga: aloca
    mesh.reserve(size_t(60000) * 3);
    auto avgOf = [&](int32_t c, int32_t r, int32_t s) {
        return 0.25f * (ext(ctx, c, r) + ext(ctx, c + s, r) + ext(ctx, c, r + s) + ext(ctx, c + s, r + s));
    };
    // Região da célula: 0 norte (r < 0, com os cantos), 2 sul (r >= 255),
    // 3 oeste (c < 0), 1 leste (c >= 255). Cada uma fica contígua no buffer,
    // para drawSkirt pular as que estão além da neblina.
    auto regionOf = [](int32_t c, int32_t r) {
        if (r < 0) return 0;
        if (r >= kQuadsPerSide) return 2;
        return c < 0 ? 3 : 1;
    };
    const int32_t lo = -kSkirtExtent, hi = kQuadsPerSide + kSkirtExtent;
    for (int32_t region = 0; region < 4; ++region) {
        ctx->skirtFirst[region] = int32_t(mesh.size());
        // Faixa fina (células de 1 tile), menos o próprio mapa
        for (int32_t r = kFineLo; r < kFineHi; ++r) {
            for (int32_t c = kFineLo; c < kFineHi; ++c) {
                if (r >= 0 && r < kQuadsPerSide && c >= 0 && c < kQuadsPerSide) continue;
                if (regionOf(c, r) != region) continue;
                emitCell(ctx, mesh, palette, c, r, 1, pickTile(buckets, avgOf(c, r, 1), c, r));
            }
        }
        // Faixa grossa (células de kSkirtCoarse tiles), menos a faixa fina
        for (int32_t r = lo; r < hi; r += kSkirtCoarse) {
            for (int32_t c = lo; c < hi; c += kSkirtCoarse) {
                if (r >= kFineLo && r < kFineHi && c >= kFineLo && c < kFineHi) continue;
                if (regionOf(c, r) != region) continue;
                emitCell(ctx, mesh, palette, c, r, kSkirtCoarse,
                         pickTile(buckets, avgOf(c, r, kSkirtCoarse), c, r));
            }
        }
        ctx->skirtCount[region] = int32_t(mesh.size()) - ctx->skirtFirst[region];
    }
    ctx->skirtVertexCount = int32_t(mesh.size());
    glBindBuffer(GL_ARRAY_BUFFER, ctx->skirtVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(TerrainVertex)), mesh.data(),
                 GL_STATIC_DRAW);
}

// Parede: um quad por tile em cada um dos quatro lados, da altura da borda
// (um pouco abaixo, para não deixar fresta) até kBorderWallHeight acima
void buildWallMesh(RzContext* ctx) {
    const float cs = ctx->cellSize, hs = ctx->heightScale;
    const uint8_t* h = ctx->heights.data();
    std::vector<WallVertex> mesh;
    mesh.reserve(size_t(4) * kQuadsPerSide * 6);
    const float sink = 0.5f * cs, top = kBorderWallHeight * cs;
    // lado: ponto i da borda -> (x, z, altura)
    auto point = [&](int32_t side, int32_t i, float* x, float* z, float* y) {
        int32_t gx = 0, gz = 0;
        switch (side) {
            case 0: gx = i;             gz = 0;             break;
            case 1: gx = kQuadsPerSide; gz = i;             break;
            case 2: gx = i;             gz = kQuadsPerSide; break;
            default: gx = 0;            gz = i;             break;
        }
        *x = float(gx) * cs;
        *z = float(gz) * cs;
        *y = float(h[gz * kGridSize + gx]) * hs - sink;
    };
    for (int32_t side = 0; side < 4; ++side) {
        for (int32_t i = 0; i < kQuadsPerSide; ++i) {
            float x0, z0, y0, x1, z1, y1;
            point(side, i, &x0, &z0, &y0);
            point(side, i + 1, &x1, &z1, &y1);
            const float u0 = float(i), u1 = float(i + 1);
            const float vt = (top + sink) / cs;
            const WallVertex a = { x0, y0, z0, u0, 0.0f }, b = { x1, y1, z1, u1, 0.0f };
            const WallVertex c = { x1, y1 + top + sink, z1, u1, vt }, d = { x0, y0 + top + sink, z0, u0, vt };
            mesh.push_back(a); mesh.push_back(b); mesh.push_back(c);
            mesh.push_back(a); mesh.push_back(c); mesh.push_back(d);
        }
    }
    ctx->wallVertexCount = int32_t(mesh.size());
    glBindBuffer(GL_ARRAY_BUFFER, ctx->wallVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(WallVertex)), mesh.data(),
                 GL_STATIC_DRAW);
}

} // namespace

bool createBorder(RzContext* ctx) {
    // Continuação: mesmo formato de vértice do terreno
    glGenVertexArrays(1, &ctx->skirtVao);
    glGenBuffers(1, &ctx->skirtVbo);
    glBindVertexArray(ctx->skirtVao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->skirtVbo);
    const GLsizei stride = sizeof(TerrainVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          reinterpret_cast<void*>(offsetof(TerrainVertex, color)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(TerrainVertex, u)));
    glEnableVertexAttribArray(2);

    // Parede
    glGenVertexArrays(1, &ctx->wallVao);
    glGenBuffers(1, &ctx->wallVbo);
    glBindVertexArray(ctx->wallVao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->wallVbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(WallVertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(WallVertex),
                          reinterpret_cast<void*>(offsetof(WallVertex, u)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    WallProgram& w = ctx->wallProgram;
    w.program = linkProgram(kWallVertexShader, kWallFragmentShader);
    if (!w.program) return false;
    w.viewProj   = glGetUniformLocation(w.program, "uViewProj");
    w.target     = glGetUniformLocation(w.program, "uTarget");
    w.fadeNear   = glGetUniformLocation(w.program, "uFadeNear");
    w.fadeFar    = glGetUniformLocation(w.program, "uFadeFar");
    w.xSize      = glGetUniformLocation(w.program, "uXSize");
    initFogUniforms(w.program, w.fog);
    return glGetError() == GL_NO_ERROR;
}

void destroyBorder(RzContext* ctx) {
    if (ctx->skirtVao) glDeleteVertexArrays(1, &ctx->skirtVao);
    if (ctx->skirtVbo) glDeleteBuffers(1, &ctx->skirtVbo);
    if (ctx->wallVao) glDeleteVertexArrays(1, &ctx->wallVao);
    if (ctx->wallVbo) glDeleteBuffers(1, &ctx->wallVbo);
    if (ctx->wallProgram.program) glDeleteProgram(ctx->wallProgram.program);
}

// Carga (chamada no fim de buildTerrainMesh): alturas estendidas, malha da
// continuação e da parede
void buildBorder(RzContext* ctx) {
    buildExtendedHeights(ctx);
    buildSkirtMesh(ctx);
    buildWallMesh(ctx);
}

// Altura estendida (mundo) em (x, z), bilinear na resolução da malha
// (1 tile no mapa e na faixa fina, kSkirtCoarse tiles depois)
float extendedGroundHeight(const RzContext* ctx, float x, float z) {
    const float invCell = 1.0f / ctx->cellSize;
    float gx = x * invCell, gz = z * invCell;
    const float lo = float(-kSkirtExtent), hi = float(kQuadsPerSide + kSkirtExtent) - 0.001f;
    gx = fminf(fmaxf(gx, lo), hi);
    gz = fminf(fmaxf(gz, lo), hi);
    const bool fine = gx >= float(kFineLo) && gx < float(kFineHi) && gz >= float(kFineLo) && gz < float(kFineHi);
    const float step = fine ? 1.0f : float(kSkirtCoarse);
    const float ox = float(-kSkirtExtent);                     // grade grossa alinhada a -kSkirtExtent
    const float cx = floorf((gx - ox) / step) * step + ox;
    const float cz = floorf((gz - ox) / step) * step + ox;
    const float fx = (gx - cx) / step, fz = (gz - cz) / step;
    auto at = [&](float px, float pz) {
        return ctx->extHeights[size_t(int32_t(pz) + kSkirtExtent) * kExtSide + size_t(int32_t(px) + kSkirtExtent)];
    };
    const float a = at(cx, cz), b = at(cx + step, cz), c = at(cx, cz + step), d = at(cx + step, cz + step);
    const float top = a + (b - a) * fx, bot = c + (d - c) * fx;
    return (top + (bot - top) * fz) * ctx->heightScale;
}

// Continuação (programa do terreno já em uso, com os mesmos uniforms). Só as
// regiões a menos de kFogEnd do olho (na horizontal): no meio do mapa, nenhuma.
void drawSkirt(const RzContext* ctx) {
    if (!ctx->fogOn || ctx->skirtVertexCount == 0) return;
    const float cs = ctx->cellSize;
    const float lo = float(-kSkirtExtent) * cs, hi = float(kQuadsPerSide + kSkirtExtent) * cs;
    const float m0 = 0.0f, m1 = float(kQuadsPerSide) * cs;
    // retângulos (x0, x1, z0, z1) das regiões 0 norte, 1 leste, 2 sul, 3 oeste
    const float rect[4][4] = { { lo, hi, lo, m0 }, { m1, hi, m0, m1 },
                               { lo, hi, m1, hi }, { lo, m0, m0, m1 } };
    const float reach = kFogEnd * cs;
    glBindVertexArray(ctx->skirtVao);
    for (int32_t i = 0; i < 4; ++i) {
        const float dx = fmaxf(fmaxf(rect[i][0] - ctx->eyePos.x, 0.0f), ctx->eyePos.x - rect[i][1]);
        const float dz = fmaxf(fmaxf(rect[i][2] - ctx->eyePos.z, 0.0f), ctx->eyePos.z - rect[i][3]);
        if (dx * dx + dz * dz > reach * reach || ctx->skirtCount[i] == 0) continue;
        glDrawArrays(GL_TRIANGLES, ctx->skirtFirst[i], ctx->skirtCount[i]);
    }
}

void drawSkirtDepth(const RzContext* ctx) {
    if (ctx->skirtVertexCount == 0) return;
    glBindVertexArray(ctx->skirtVao);
    glDrawArrays(GL_TRIANGLES, 0, ctx->skirtVertexCount);
}

// Parede: depois de tudo que é opaco
void drawWall(const RzContext* ctx, const Mat4& viewProj) {
    if (!ctx->fogOn || !ctx->hasTerrain() || ctx->wallVertexCount == 0) return;
    const WallProgram& w = ctx->wallProgram;
    glUseProgram(w.program);
    glUniformMatrix4fv(w.viewProj, 1, GL_TRUE, viewProj.e);
    glUniform3f(w.target, ctx->targetPos.x, ctx->targetPos.y, ctx->targetPos.z);
    glUniform1f(w.fadeNear, kBorderFadeNear * ctx->cellSize);
    glUniform1f(w.fadeFar, kBorderFadeFar * ctx->cellSize);
    glUniform1f(w.xSize, kBorderXSize);
    bindFog(ctx, w.fog);

    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   // alfa do destino = byte reservado
    glDepthMask(GL_FALSE);
    glBindVertexArray(ctx->wallVao);
    glDrawArrays(GL_TRIANGLES, 0, ctx->wallVertexCount);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

} // namespace rz
