// Sprites (fumaça, detritos, água espirrando): quads virados para a câmera.
//
// O legado tem um array compacto (um sprite que morre faz os seguintes
// andarem uma posição), então não há id: rzSetSprites recebe o array inteiro
// e substitui o anterior. O renderer não guarda estado por sprite.
//
// Cada sprite: centro (8.24), diâmetro (tiles) e índice de cor na paleta do
// jogo (a do atlas de blocos; sem atlas, uma rampa de cinza). A forma é uma
// textura de ruído "spray do Paint" (kSpriteClicks cliques de pontinhos
// perto do centro), com kSpriteFrames variações numa textura array; o quadro
// de cada sprite gira a cada kSpriteFrameTicks frames, deslocado pela
// posição no array, para não ficar estático.
//
// Desenho: depois do vidro (fumaça na frente de um vidro não sai tingida) e
// antes da parede de limite. Blending alfa comum, ordenado de trás para
// frente na CPU (até kMaxSprites), sem gravar profundidade nem alfa de
// destino, sem culling. Cor = paleta x luz de uma face virada para cima (como
// o chão plano), com neblina. Não projeta nem recebe sombra.
//
// Memória: o VBO (kMaxSprites x 6 vértices) e os vetores de trabalho são
// alocados na criação do contexto; rzSetSprites e o desenho não alocam.

#include <algorithm>
#include <cmath>

#include "rz_internal.h"
#include "rz_shaders.h"

namespace rz {

namespace {

constexpr float kFixed824ToFloat = 1.0f / 16777216.0f;   // 2^-24

uint32_t hashU32(uint32_t h) {
    h ^= h >> 16; h *= 0x7FEB352Du;
    h ^= h >> 15; h *= 0x846CA68Bu;
    return h ^ (h >> 16);
}

// Ruído "spray": kSpriteClicks cliques, cada um uma nuvem de pontinhos
// uniformes num disco, com o centro do clique sorteado perto do meio.
// Mipmaps na CPU (média 2x2 do alfa). Uma camada por quadro.
void buildNoiseTexture(RzContext* ctx) {
    constexpr int32_t N = kSpriteNoiseSize;
    constexpr int32_t kLevels = 7;                         // 64 .. 1
    std::vector<uint8_t> level0(size_t(N) * N * kSpriteFrames, 0);
    uint32_t seed = 12345u;
    auto rnd = [&]() { seed = hashU32(seed + 0x9E3779B9u); return float(seed & 0xFFFFFFu) / 16777216.0f; };
    for (int32_t f = 0; f < kSpriteFrames; ++f) {
        uint8_t* img = level0.data() + size_t(f) * N * N;
        for (int32_t click = 0; click < kSpriteClicks; ++click) {
            const float ca = rnd() * 2.0f * kPi, cr = rnd() * kSpriteClickJitter * N;
            const float cx = 0.5f * N + cosf(ca) * cr, cy = 0.5f * N + sinf(ca) * cr;
            const float radius = kSpriteClickRadius * N;
            for (int32_t d = 0; d < kSpriteDotsPerClick; ++d) {
                const float a = rnd() * 2.0f * kPi, r = sqrtf(rnd()) * radius;   // uniforme no disco
                const int32_t px = int32_t(cx + cosf(a) * r), py = int32_t(cy + sinf(a) * r);
                if (px >= 0 && px < N && py >= 0 && py < N) img[py * N + px] = 255;
            }
        }
    }

    glGenTextures(1, &ctx->spriteNoiseTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx->spriteNoiseTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    std::vector<uint8_t> cur = level0, next;
    int32_t side = N;
    for (int32_t level = 0; level < kLevels; ++level) {
        glTexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_R8, side, side, kSpriteFrames, 0,
                     GL_RED, GL_UNSIGNED_BYTE, cur.data());
        if (side == 1) break;
        const int32_t half = side / 2;
        next.assign(size_t(half) * half * kSpriteFrames, 0);
        for (int32_t f = 0; f < kSpriteFrames; ++f) {
            const uint8_t* s = cur.data() + size_t(f) * side * side;
            uint8_t* d = next.data() + size_t(f) * half * half;
            for (int32_t y = 0; y < half; ++y)
                for (int32_t x = 0; x < half; ++x)
                    d[y * half + x] = uint8_t((s[(2 * y) * side + 2 * x] + s[(2 * y) * side + 2 * x + 1] +
                                               s[(2 * y + 1) * side + 2 * x] + s[(2 * y + 1) * side + 2 * x + 1] + 2) / 4);
        }
        cur.swap(next);
        side = half;
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, kLevels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);           // pontinhos de perto
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); // macio de longe
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

} // namespace

// Paleta dos sprites: a do atlas (rzLoadTileAtlas); antes dele, rampa de cinza.
// Já com a luz de uma face virada para cima, como o chão plano.
void setSpritePalette(RzContext* ctx, const uint8_t* paletteRGB) {
    const Vec3 l = lightDirection();
    const float light = kAmbient + (1.0f - kAmbient) * (l.y > 0.0f ? l.y : 0.0f);
    for (int32_t i = 0; i < 256; ++i) {
        const float r = paletteRGB ? paletteRGB[i * 3 + 0] : float(i);
        const float g = paletteRGB ? paletteRGB[i * 3 + 1] : float(i);
        const float b = paletteRGB ? paletteRGB[i * 3 + 2] : float(i);
        auto byte = [](float c) { return uint32_t(c < 0.0f ? 0.0f : (c > 255.0f ? 255.0f : c + 0.5f)); };
        ctx->spritePalette[i] = (byte(r * light) << 16) | (byte(g * light) << 8) | byte(b * light);
    }
}

bool createSprites(RzContext* ctx) {
    SpriteProgram& p = ctx->spriteProgram;
    p.program = linkProgram(kSpriteVertexShader, kSpriteFragmentShader);
    if (!p.program) return false;
    p.viewProj = glGetUniformLocation(p.program, "uViewProj");
    p.right    = glGetUniformLocation(p.program, "uRight");
    p.up       = glGetUniformLocation(p.program, "uUp");
    p.noise    = glGetUniformLocation(p.program, "uNoise");
    initFogUniforms(p.program, p.fog);

    ctx->sprites.reserve(kMaxSprites);
    ctx->spriteOrder.reserve(kMaxSprites);
    ctx->spriteStaging.resize(size_t(kMaxSprites) * 6);
    setSpritePalette(ctx, nullptr);
    buildNoiseTexture(ctx);

    glGenVertexArrays(1, &ctx->spriteVao);
    glGenBuffers(1, &ctx->spriteVbo);
    glBindVertexArray(ctx->spriteVao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->spriteVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(ctx->spriteStaging.size() * sizeof(SpriteVertex)),
                 nullptr, GL_DYNAMIC_DRAW);
    const GLsizei stride = sizeof(SpriteVertex);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));   // centro + raio
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_BYTE, GL_FALSE, stride,                                 // canto, quadro
                          reinterpret_cast<void*>(offsetof(SpriteVertex, cornerX)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,                         // cor
                          reinterpret_cast<void*>(offsetof(SpriteVertex, color)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);
    return glGetError() == GL_NO_ERROR;
}

void destroySprites(RzContext* ctx) {
    if (ctx->spriteProgram.program) glDeleteProgram(ctx->spriteProgram.program);
    if (ctx->spriteVbo) glDeleteBuffers(1, &ctx->spriteVbo);
    if (ctx->spriteVao) glDeleteVertexArrays(1, &ctx->spriteVao);
    if (ctx->spriteNoiseTex) glDeleteTextures(1, &ctx->spriteNoiseTex);
}

// Depois do vidro, antes da parede
void drawSprites(RzContext* ctx, const Mat4& viewProj) {
    if (ctx->sprites.empty()) return;

    // Ordem de trás para frente (profundidade ao longo da frente da câmera);
    // os que estão todos além da neblina ficam de fora
    const Vec3 eye = ctx->eyePos, fwd = ctx->camForward;
    const float reach = ctx->fogFar;
    ctx->spriteOrder.clear();
    for (int32_t i = 0; i < int32_t(ctx->sprites.size()); ++i) {
        const Sprite& s = ctx->sprites[size_t(i)];
        const Vec3 d = s.center - eye;
        if (ctx->fogOn) {
            const Vec3 dFog = s.center - ctx->fogOrigin;
            const float lim = reach + s.radius;
            if (dot(dFog, dFog) > lim * lim) continue;
        }
        const float depth = dot(d, fwd);
        if (depth < -s.radius) continue;                      // todo atrás do olho
        ctx->spriteOrder.push_back({ depth, i });
    }
    if (ctx->spriteOrder.empty()) return;
    std::sort(ctx->spriteOrder.begin(), ctx->spriteOrder.end(),
              [](const SpriteKey& a, const SpriteKey& b) { return a.depth > b.depth; });

    // Seis vértices por sprite (dois triângulos); o shader abre o quad
    constexpr int8_t kCorners[6][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, -1 }, { 1, 1 }, { -1, 1 } };
    const uint32_t tick = ctx->frameCount / uint32_t(kSpriteFrameTicks);
    SpriteVertex* v = ctx->spriteStaging.data();
    for (const SpriteKey& k : ctx->spriteOrder) {
        const Sprite& s = ctx->sprites[size_t(k.index)];
        const int8_t frame = int8_t((tick + uint32_t(k.index) * 3u) % uint32_t(kSpriteFrames));
        for (const auto& c : kCorners) {
            *v++ = { s.center.x, s.center.y, s.center.z, s.radius, c[0], c[1], frame, 0, s.color };
        }
    }
    const GLsizei count = GLsizei(ctx->spriteOrder.size() * 6);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->spriteVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(size_t(count) * sizeof(SpriteVertex)),
                    ctx->spriteStaging.data());

    const SpriteProgram& p = ctx->spriteProgram;
    glUseProgram(p.program);
    glUniformMatrix4fv(p.viewProj, 1, GL_TRUE, viewProj.e);
    glUniform3f(p.right, ctx->camRight.x, ctx->camRight.y, ctx->camRight.z);
    glUniform3f(p.up, ctx->camUp.x, ctx->camUp.y, ctx->camUp.z);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx->spriteNoiseTex);
    glUniform1i(p.noise, 0);
    bindFog(ctx, p.fog);

    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);   // alfa do destino = byte reservado
    glDepthMask(GL_FALSE);
    glBindVertexArray(ctx->spriteVao);
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDepthMask(GL_TRUE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDisable(GL_BLEND);
}

} // namespace rz

using namespace rz;

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetSprites(RzContext* ctx, const RzSprite* sprites, int32_t count) {
    if (!ctx || count < 0 || (count > 0 && !sprites)) return RZ_ERR_INVALID_ARG;
    if (count > kMaxSprites) return RZ_ERR_SIZE;
    for (int32_t i = 0; i < count; ++i) {
        if (!(sprites[i].size > 0.0f && sprites[i].size < 1.0e6f)) return RZ_ERR_INVALID_ARG;   // pega NaN
    }
    const float scale = ctx->cellSize * kFixed824ToFloat;
    ctx->sprites.clear();                                      // capacidade reservada: não aloca
    for (int32_t i = 0; i < count; ++i) {
        const RzSprite& s = sprites[i];
        Sprite d;
        d.center = { float(s.x) * scale, float(s.z) * scale, float(s.y) * scale };   // (coluna, altura, linha)
        d.radius = 0.5f * s.size * ctx->cellSize;
        d.color  = ctx->spritePalette[s.color];
        ctx->sprites.push_back(d);
    }
    return RZ_OK;
}

} // extern "C"
