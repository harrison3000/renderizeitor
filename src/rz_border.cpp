// Borda do mundo: "continuação" do terreno além das extremidades do mapa e a
// parede de limite da área jogável.
//
// Continuação: até kSkirtExtent tiles além de cada borda (o alcance da
// neblina + folga), gerada na carga a partir do próprio mapa:
//   - altura: parte da altura da borda (o ponto mais próximo do mapa) e, ao
//     longo de kSkirtBlend tiles, vai para (média das bordas + ruído de valor
//     em duas oitavas). Na emenda com o mapa, a altura é a da borda: sem degrau.
//   - bloco de textura: em geral continua o da borda logo "na frente", com o
//     ponto de cópia serpenteando ao longo da borda (rios continuam como
//     faixas que fazem curvas); uma parte das células sorteia um bloco perto
//     da borda (skirtTile).
//   - malha em faixas (kSkirtBands): células de 1 tile até 8 tiles da borda,
//     de 2 até 16, de 4 até 32 e de 16 até kSkirtExtent (128); longe, a
//     neblina cobre. Nas linhas onde duas faixas se encontram, as alturas dos
//     pontos intermediários são interpoladas, para não abrir frestas
//     (T-junctions).
//   - só é desenhada seguindo um alvo (com neblina); projeta sombra no mapa 0.
//   - groundHeight (câmera) usa as mesmas alturas, com a mesma resolução.
//
// Parede: em cima das quatro bordas do mapa, do chão até kBorderWallHeight
// tiles acima, vermelha translúcida, com círculos vermelhos de borda branca
// e um X vazado no meio (um por célula de kBorderXSize tiles), num padrão
// preso ao mundo (ao longo da parede e na altura do mundo: a parede sobe e
// desce com o terreno, o desenho não). Só
// aparece perto do alvo da câmera: alfa = 1 - smoothstep(near, far, distância
// horizontal do alvo ao ponto da parede). Desenhada depois do opaco, com
// blending e sem gravar profundidade; não projeta nem recebe sombra.

#include <cmath>

#include "rz_internal.h"
#include "rz_shaders.h"

namespace rz {

namespace {

constexpr int32_t kExtLo  = -kSkirtExtent;                           // -128 (origem das grades)
constexpr int32_t kExtHi  = kQuadsPerSide + kSkirtExtent + 1;        // 384
constexpr int32_t kExtSide = kExtHi - kExtLo + 1;                    // pontos por lado (513)
// Contorno da faixa i: de bandLo(i) a bandHi(i) nos dois eixos
constexpr int32_t bandLo(int32_t i) { return -kSkirtBands[i].limit; }
constexpr int32_t bandHi(int32_t i) { return kQuadsPerSide + kSkirtBands[i].limit + 1; }
// grades alinhadas a kExtLo; o contorno de cada faixa fecha em células
// inteiras dela e da de fora; passos crescentes e múltiplos
constexpr bool bandsAligned() {
    for (int32_t i = 0; i < kSkirtBandCount; ++i) {
        const int32_t s = kSkirtBands[i].step;
        const int32_t outer = i + 1 < kSkirtBandCount ? kSkirtBands[i + 1].step : s;
        if ((bandLo(i) - kExtLo) % outer != 0 || (bandHi(i) - kExtLo) % outer != 0) return false;
        if (outer % s != 0) return false;
        if (i > 0 && kSkirtBands[i].limit <= kSkirtBands[i - 1].limit) return false;
    }
    return kSkirtBands[0].step == 1;
}
static_assert(bandsAligned(), "faixas da continuação desalinhadas");

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

// Alturas (unidade do byte, float) de -kSkirtExtent a 255 + kSkirtExtent + 1
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

    for (int32_t gz = kExtLo; gz <= kExtHi; ++gz) {            // kExtHi: canto de fora da última célula
        for (int32_t gx = kExtLo; gx <= kExtHi; ++gx) {
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

    // Emendas entre faixas: nas linhas do contorno de uma faixa (lo..hi), os
    // pontos entre os cantos das células da faixa de fora (passo step) ficam
    // na reta entre eles (sem frestas)
    auto fixLine = [&](bool alongX, int32_t fixed, int32_t lo, int32_t hi, int32_t step) {
        for (int32_t s = lo; s < hi; s += step) {
            const float a = alongX ? ext(ctx, s, fixed) : ext(ctx, fixed, s);
            const float b = alongX ? ext(ctx, s + step, fixed) : ext(ctx, fixed, s + step);
            for (int32_t k = 1; k < step; ++k) {
                const float v = a + (b - a) * float(k) / float(step);
                if (alongX) ext(ctx, s + k, fixed) = v; else ext(ctx, fixed, s + k) = v;
            }
        }
    };
    auto fixRing = [&](int32_t lo, int32_t hi, int32_t step) {
        fixLine(true, lo, lo, hi, step);  fixLine(true, hi, lo, hi, step);
        fixLine(false, lo, lo, hi, step); fixLine(false, hi, lo, hi, step);
    };
    for (int32_t i = 0; i + 1 < kSkirtBandCount; ++i)     // faixa i / faixa i+1
        fixRing(bandLo(i), bandHi(i), kSkirtBands[i + 1].step);
}


// Uma célula (c, r)-(c+s, r+s): dois triângulos com a diagonal do mapa
void emitCell(const RzContext* ctx, std::vector<TerrainVertex>& out, const uint32_t* palette,
              int32_t c, int32_t r, int32_t s, uint8_t layer) {
    const float cs = ctx->cellSize, hs = ctx->heightScale;
    struct Corner { int32_t dc, dr; };
    constexpr Corner kCorners[2][3] = { { { 0, 0 }, { 0, 1 }, { 1, 0 } },     // mesma do mapa (1 3 2, 2 3 4)
                                        { { 1, 0 }, { 0, 1 }, { 1, 1 } } };
    for (const auto& tri : kCorners) {
        Vec3 p[3];
        float sum = 0.0f;
        for (int32_t k = 0; k < 3; ++k) {
            const int32_t gc = c + tri[k].dc * s, gr = r + tri[k].dr * s;
            const float height = ctx->extHeights[size_t(gr + kSkirtExtent) * kExtSide + size_t(gc + kSkirtExtent)];
            sum += height;
            p[k] = { float(gc) * cs, height * hs, float(gr) * cs };
        }
        const uint32_t normal = packNormal(cross(p[1] - p[0], p[2] - p[0]));
        int32_t idx = int32_t(sum + 0.5f);
        if (idx > kPaletteSize - 1) idx = kPaletteSize - 1;
        const uint32_t color = palette[idx];
        for (int32_t k = 0; k < 3; ++k) {
            out.push_back({ p[k].x, p[k].y, p[k].z, color,
                            uint8_t(tri[k].dc), uint8_t(tri[k].dr), layer, 0, normal });
        }
    }
}

void buildSkirtMesh(RzContext* ctx) {
    uint32_t palette[kPaletteSize];
    buildPalette(palette);

    std::vector<TerrainVertex>& mesh = ctx->skirtStaging;   // reaproveitado (aloca só na 1ª)
    mesh.clear();
    mesh.reserve(size_t(60000) * 3);
    // Região da célula: 0 norte (r < 0, com os cantos), 2 sul (r >= 255),
    // 3 oeste (c < 0), 1 leste (c >= 255). Cada uma fica contígua no buffer,
    // para drawSkirt pular as que estão além da neblina.
    auto regionOf = [](int32_t c, int32_t r) {
        if (r < 0) return 0;
        if (r >= kQuadsPerSide) return 2;
        return c < 0 ? 3 : 1;
    };
    for (int32_t region = 0; region < 4; ++region) {
        ctx->skirtFirst[region] = int32_t(mesh.size());
        // Cada faixa, menos o que está dentro da anterior (a primeira: menos o mapa)
        for (int32_t i = 0; i < kSkirtBandCount; ++i) {
            const int32_t step = kSkirtBands[i].step;
            const int32_t lo = i + 1 < kSkirtBandCount ? bandLo(i) : kExtLo;
            const int32_t hi = i + 1 < kSkirtBandCount ? bandHi(i) : kExtHi;
            const int32_t inLo = i > 0 ? bandLo(i - 1) : 0;
            const int32_t inHi = i > 0 ? bandHi(i - 1) : kQuadsPerSide;
            for (int32_t r = lo; r < hi; r += step) {
                for (int32_t c = lo; c < hi; c += step) {
                    if (r >= inLo && r < inHi && c >= inLo && c < inHi) continue;
                    if (regionOf(c, r) != region) continue;
                    emitCell(ctx, mesh, palette, c, r, step, skirtTile(ctx, c, r));
                }
            }
        }
        ctx->skirtCount[region] = int32_t(mesh.size()) - ctx->skirtFirst[region];
    }
    glBindBuffer(GL_ARRAY_BUFFER, ctx->skirtVbo);
    if (ctx->skirtVertexCount == int32_t(mesh.size())) {     // mesmo tamanho: sem realocar
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(mesh.size() * sizeof(TerrainVertex)), mesh.data());
    } else {
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(mesh.size() * sizeof(TerrainVertex)), mesh.data(),
                     GL_STATIC_DRAW);
    }
    ctx->skirtVertexCount = int32_t(mesh.size());
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

// Bloco de uma célula da continuação. Em geral continua o bloco do quad da
// borda que está "na frente" da célula (rio continua rio), mas o ponto de onde
// se copia vai serpenteando: ao longo da borda, ele se desloca por um ruído de
// valor suave em (posição ao longo, distância para fora), com amplitude que
// cresce com a distância (até kSkirtMeanderMax tiles). Células vizinhas têm
// deslocamentos parecidos, então as faixas continuam juntas e fazem curvas em
// vez de seguir em linha reta. Com chance kSkirtJitterPercent %, a célula
// pega um bloco sorteado da janela perto da borda (kSkirtTileBand), para
// quebrar a repetição. Nos cantos (fora nos dois eixos), copia o quad do canto.
uint8_t skirtTile(const RzContext* ctx, int32_t c, int32_t r) {
    if (ctx->tileMap.empty()) return 0;
    const int32_t last = kQuadsPerSide - 1;                  // 254: último quad
    const int32_t outC = c < 0 ? -c : (c > last ? c - last : 0);
    const int32_t outR = r < 0 ? -r : (r > last ? r - last : 0);
    const uint32_t h = hash2(c, r, 77u);

    int32_t qc, qr;
    if (h % 100u < uint32_t(kSkirtJitterPercent)) {
        // sorteio na janela perto do ponto da borda mais próximo
        const int32_t along  = int32_t((h >> 8) % uint32_t(2 * kSkirtTileBand + 1)) - kSkirtTileBand;
        const int32_t inward = int32_t((h >> 20) % uint32_t(kSkirtTileBand));
        qc = c < 0 ? inward : (c > last ? last - inward : c + along);
        qr = r < 0 ? inward : (r > last ? last - inward : r + along);
    } else {
        // continuação, serpenteando ao longo da borda
        auto meander = [](int32_t along, int32_t out, uint32_t seed) {
            float amp = float(out) * kSkirtMeanderGrowth;
            if (amp > kSkirtMeanderMax) amp = kSkirtMeanderMax;
            const float n = valueNoise(float(along), float(out), kSkirtMeanderPeriod, seed) - 0.5f;
            return int32_t(floorf(n * 2.0f * amp + 0.5f));
        };
        qc = c < 0 ? 0 : (c > last ? last : c);
        qr = r < 0 ? 0 : (r > last ? last : r);
        if (outR > 0 && outC == 0) qc += meander(c, outR, r < 0 ? 31u : 37u);   // norte/sul
        if (outC > 0 && outR == 0) qr += meander(r, outC, c < 0 ? 41u : 43u);   // oeste/leste
    }
    qc = qc < 0 ? 0 : (qc > last ? last : qc);
    qr = qr < 0 ? 0 : (qr > last ? last : qr);
    return ctx->tileMap[qr * kGridSize + qc];
}

bool createBorder(RzContext* ctx) {
    // Continuação: mesmo formato de vértice do terreno
    glGenVertexArrays(1, &ctx->skirtVao);
    glGenBuffers(1, &ctx->skirtVbo);
    glBindVertexArray(ctx->skirtVao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->skirtVbo);
    setTerrainVertexLayout();

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
    w.cellSize   = glGetUniformLocation(w.program, "uCellSize");
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

// Um ponto da borda do mapa mudou de altura (rzUpdateTerrain): só as vértices
// da continuação que estão em cima dele acompanham (sem fresta com o mapa).
// O resto do terreno de fora, as cores e a parede ficam como estavam
// (aceito: as mudanças no jogo são raras e pequenas; rzSetHeightmap refaz tudo).
void moveSkirtEdgeVertex(RzContext* ctx, int32_t gc, int32_t gr) {
    const float x = float(gc) * ctx->cellSize, z = float(gr) * ctx->cellSize;
    const float y = float(ctx->heights[size_t(gr) * kGridSize + size_t(gc)]) * ctx->heightScale;
    std::vector<TerrainVertex>& mesh = ctx->skirtStaging;
    glBindBuffer(GL_ARRAY_BUFFER, ctx->skirtVbo);
    for (size_t i = 0; i < mesh.size(); ++i) {
        if (mesh[i].x != x || mesh[i].z != z) continue;     // mesma conta da montagem: igualdade exata
        mesh[i].y = y;
        glBufferSubData(GL_ARRAY_BUFFER, GLintptr(i * sizeof(TerrainVertex)), GLsizeiptr(sizeof(float) * 3),
                        &mesh[i]);
    }
}

// Só a malha da continuação (blocos perto da borda mudaram; rzUpdateTerrain)
void rebuildSkirtMesh(RzContext* ctx) {
    buildSkirtMesh(ctx);
}

// Um ponto do mapa (dentro) na cópia estendida, que é igual à altura dele
// (rzUpdateTerrain, mudança no interior: a borda de fora não depende dele)
void setExtendedHeight(RzContext* ctx, int32_t gc, int32_t gr, float height) {
    if (!ctx->extHeights.empty()) ext(ctx, gc, gr) = height;
}

// Carga (chamada no fim de buildTerrainMesh): alturas estendidas, malha da
// continuação e da parede
void buildBorder(RzContext* ctx) {
    buildExtendedHeights(ctx);
    buildSkirtMesh(ctx);
    buildWallMesh(ctx);
}

// Altura estendida (mundo) em (x, z), bilinear na resolução da malha
// (o passo da faixa onde o ponto está; 1 tile no mapa)
float extendedGroundHeight(const RzContext* ctx, float x, float z) {
    const float invCell = 1.0f / ctx->cellSize;
    float gx = x * invCell, gz = z * invCell;
    const float lo = float(-kSkirtExtent), hi = float(kQuadsPerSide + kSkirtExtent) - 0.001f;
    gx = fminf(fmaxf(gx, lo), hi);
    gz = fminf(fmaxf(gz, lo), hi);
    auto inside = [&](int32_t a, int32_t b) { return gx >= float(a) && gx < float(b) && gz >= float(a) && gz < float(b); };
    float step = float(kSkirtBands[kSkirtBandCount - 1].step);
    for (int32_t i = kSkirtBandCount - 2; i >= 0; --i)
        if (inside(bandLo(i), bandHi(i))) step = float(kSkirtBands[i].step);
    const float ox = float(kExtLo);                            // grades alinhadas a kExtLo
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
// regiões a menos do fim da neblina do olho (na horizontal): no meio do mapa, nenhuma.
void drawSkirt(const RzContext* ctx) {
    if (!ctx->fogOn || ctx->skirtVertexCount == 0) return;
    const float cs = ctx->cellSize;
    const float lo = float(-kSkirtExtent) * cs, hi = float(kQuadsPerSide + kSkirtExtent) * cs;
    const float m0 = 0.0f, m1 = float(kQuadsPerSide) * cs;
    // retângulos (x0, x1, z0, z1) das regiões 0 norte, 1 leste, 2 sul, 3 oeste
    const float rect[4][4] = { { lo, hi, lo, m0 }, { m1, hi, m0, m1 },
                               { lo, hi, m1, hi }, { lo, m0, m0, m1 } };
    const float reach = ctx->fogFar;
    const Vec3 o = ctx->fogOrigin;
    glBindVertexArray(ctx->skirtVao);
    for (int32_t i = 0; i < 4; ++i) {
        const float dx = fmaxf(fmaxf(rect[i][0] - o.x, 0.0f), o.x - rect[i][1]);
        const float dz = fmaxf(fmaxf(rect[i][2] - o.z, 0.0f), o.z - rect[i][3]);
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
    if (!ctx->wallOn || !ctx->hasTerrain() || ctx->wallVertexCount == 0) return;
    const WallProgram& w = ctx->wallProgram;
    glUseProgram(w.program);
    glUniformMatrix4fv(w.viewProj, 1, GL_TRUE, viewProj.e);
    glUniform3f(w.target, ctx->targetPos.x, ctx->targetPos.y, ctx->targetPos.z);
    glUniform1f(w.fadeNear, kBorderFadeNear * ctx->cellSize);
    glUniform1f(w.fadeFar, kBorderFadeFar * ctx->cellSize);
    glUniform1f(w.xSize, kBorderXSize);
    glUniform1f(w.cellSize, ctx->cellSize);
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
