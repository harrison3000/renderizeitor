// Sombras: três shadow maps da mesma luz direcional (projeções ortográficas
// com a mesma view da luz, fixa, olhando para o centro do terreno).
//
//   0 terreno   só o terreno projeta; cobre o terreno inteiro; refeito só
//               quando o terreno muda (terrainShadowDirty): inteiro na carga
//               (markTerrainShadowAll) ou, em rzUpdateTerrain, só o retângulo
//               do mapa onde cai a caixa alterada (markTerrainShadowBox, com
//               scissor: o terreno inteiro é enviado, mas só esses texels são
//               limpos e rasterizados). Sombra do relevo em qualquer distância e
//               sobre os objetos (carro entrando na sombra do morro).
//   1 próximos  objetos numa caixa à frente do olho (kShadowNearAhead), que
//               cobre a parte visível antes da neblina, MENOS o alvo da câmera
//               (senão a sombra grossa dele vaza em volta da fina do mapa 2);
//               refeito todo frame (só objetos: barato). Objetos além da
//               neblina não entram (objectInRange).
//   2 alvo      só o objeto que a câmera segue, numa caixa ajustada à esfera
//               envolvente dele; a mais alta resolução; refeito todo frame.
//
// Iluminado = mínimo dos três; fora da caixa de um mapa, ele não sombreia.
// Objetos fora da caixa do mapa 1 não projetam sombra (aceito).
// Na visão geral (sem alvo), o mapa 1 cobre o terreno inteiro e o 2 é desligado.
//
// RESSALVA (para o futuro): o mapa 1 não inclui o terreno, então o relevo
// perto do carro só faz sombra pela resolução do mapa 0 (~0,18 tile/texel;
// na escala dos triângulos de 1 tile do terreno flat). Se a sombra de
// detalhes pequenos do relevo (degraus de meio tile) perto do carro ficar
// serrilhada demais, dá para desenhar no mapa 1 também o pedaço de terreno
// da caixa: a malha é montada em linhas (row-major, 255 quads x 2 triângulos
// por linha), então seria um glDrawArrays por linha da caixa (ou um
// glMultiDrawArrays), ~7 mil triângulos para uma caixa de 60 tiles.
//
// As caixas dos mapas 1 e 2 andam em passos de um texel (e o lado da caixa
// do alvo é arredondado para cima em passos fixos), para a sombra não tremer.
// Os tamanhos são limitados a GL_MAX_TEXTURE_SIZE (o GL 3.3 só garante 1024).
// Em profundidade, os três cobrem a esfera do terreno inteiro com folga: os
// receptores (o chão embaixo do carro) também ficam dentro da faixa.

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t kShadowRequested[kShadowMaps] = {
    kShadowTerrainSize, kShadowNearSize, kShadowTargetSize };

// Caixa de um mapa no plano da luz: centro (cx, cy) na view da luz e meia-largura
struct ShadowBox {
    float cx, cy, half;
};

float shadowRadius(const RzContext* ctx) {
    return ctx->terrainRadius * 1.1f + 4.0f * ctx->cellSize;   // folga para objetos acima do terreno
}

Vec3 terrainCenter(const RzContext* ctx) {
    const float cs = ctx->cellSize;
    return { 127.5f * cs, 127.5f * ctx->heightScale, 127.5f * cs };
}

Mat4 lightView(const RzContext* ctx) {
    const float radius = shadowRadius(ctx);
    const Vec3 c = terrainCenter(ctx);
    const Vec3 l = lightDirection();
    const Vec3 eye = c + Vec3{ l.x * 2.0f * radius, l.y * 2.0f * radius, l.z * 2.0f * radius };
    return lookAt(eye, c);
}

// Ortográfica da caixa; profundidade de R a 3R (a esfera do terreno, vista a 2R)
Mat4 boxProjection(const RzContext* ctx, ShadowBox b) {
    const float radius = shadowRadius(ctx);
    const float nearPlane = radius, farPlane = 3.0f * radius;
    const float invHalf = 1.0f / b.half;
    Mat4 p = Mat4::identity();
    p[0, 0] = invHalf;  p[0, 3] = -b.cx * invHalf;
    p[1, 1] = invHalf;  p[1, 3] = -b.cy * invHalf;
    p[2, 2] = -2.0f / (farPlane - nearPlane);
    p[2, 3] = -(farPlane + nearPlane) / (farPlane - nearPlane);
    return p;
}

// Caixa centrada no ponto p do mundo, com o centro preso à grade de texels
ShadowBox snappedBox(const Mat4& view, Vec3 p, float half, int32_t size) {
    float cx = view[0, 0] * p.x + view[0, 1] * p.y + view[0, 2] * p.z + view[0, 3];
    float cy = view[1, 0] * p.x + view[1, 1] * p.y + view[1, 2] * p.z + view[1, 3];
    const float texel = 2.0f * half / float(size);
    cx = floorf(cx / texel + 0.5f) * texel;
    cy = floorf(cy / texel + 0.5f) * texel;
    return { cx, cy, half };
}

void beginPass(const RzContext* ctx, int32_t map, const Mat4& matrix) {
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->shadowFbo[map]);
    glViewport(0, 0, ctx->shadowSize[map], ctx->shadowSize[map]);
    glClear(GL_DEPTH_BUFFER_BIT);
    glUniformMatrix4fv(ctx->depthProgram.lightViewProj, 1, GL_TRUE, matrix.e);
}

} // namespace

// Texturas de profundidade com comparação (sampler2DShadow, PCF 2x2 com
// GL_LINEAR), cada uma num FBO sem cor.
bool createShadowMaps(RzContext* ctx) {
    GLint driverMax = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &driverMax);
    for (int32_t i = 0; i < kShadowMaps; ++i) {
        int32_t size = kShadowRequested[i];
        if (driverMax > 0 && size > driverMax) size = driverMax;
        ctx->shadowSize[i] = size;
        glGenTextures(1, &ctx->shadowTex[i]);
        glBindTexture(GL_TEXTURE_2D, ctx->shadowTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, size, size, 0,
                     GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);

        glGenFramebuffers(1, &ctx->shadowFbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, ctx->shadowFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, ctx->shadowTex[i], 0);
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
        const bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        glClear(GL_DEPTH_BUFFER_BIT);                   // vazio = tudo iluminado
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (!ok) return false;
    }
    markTerrainShadowAll(ctx);
    return true;
}

void markTerrainShadowAll(RzContext* ctx) {
    ctx->terrainShadowDirty = true;
    ctx->terrainShadowAll = true;
}

// Acumula (união) até o próximo frame. As alturas antigas entram na caixa:
// um morro que baixou também apaga a profundidade de onde ele estava.
void markTerrainShadowBox(RzContext* ctx, Vec3 lo, Vec3 hi) {
    if (ctx->terrainShadowDirty) {
        if (ctx->terrainShadowAll) return;
        Vec3& a = ctx->terrainShadowLo;
        Vec3& b = ctx->terrainShadowHi;
        a = { fminf(a.x, lo.x), fminf(a.y, lo.y), fminf(a.z, lo.z) };
        b = { fmaxf(b.x, hi.x), fmaxf(b.y, hi.y), fmaxf(b.z, hi.z) };
        return;
    }
    ctx->terrainShadowDirty = true;
    ctx->terrainShadowAll = false;
    ctx->terrainShadowLo = lo;
    ctx->terrainShadowHi = hi;
}

void destroyShadowMaps(RzContext* ctx) {
    for (int32_t i = 0; i < kShadowMaps; ++i) {
        if (ctx->shadowFbo[i]) glDeleteFramebuffers(1, &ctx->shadowFbo[i]);
        if (ctx->shadowTex[i]) glDeleteTextures(1, &ctx->shadowTex[i]);
        ctx->shadowFbo[i] = 0;
        ctx->shadowTex[i] = 0;
    }
}

// Calcula as três matrizes do frame e redesenha os mapas que precisam.
// Usa o foco da câmera do frame (updateCamera antes) e os VBOs já enviados
// (prepareObjects antes). Sem culling (as faces de trás também projetam),
// com polygon offset contra o "acne".
void renderShadowMaps(RzContext* ctx) {
    const Mat4 view = lightView(ctx);
    const float radius = shadowRadius(ctx);
    const ShadowBox whole = { 0.0f, 0.0f, radius };   // view centrada no terreno

    const int32_t target = ctx->cameraTargetObject;
    const bool following = !ctx->shadowWholeTerrain;

    // Matrizes
    ctx->shadowMatrix[0] = boxProjection(ctx, whole) * view;

    float nearHalf = kShadowNearHalfExtent * ctx->cellSize;
    if (nearHalf > radius) nearHalf = radius;
    const ShadowBox nearBox = following
        ? snappedBox(view, ctx->shadowFocus, nearHalf, ctx->shadowSize[1]) : whole;
    ctx->shadowMatrix[1] = boxProjection(ctx, nearBox) * view;

    ctx->shadowTargetOn = following;
    if (following) {
        const Vec3  center = ctx->objects[target].center;
        const float r      = ctx->objects[target].radius;
        // lado arredondado para cima em passos fixos: estável com o objeto girando
        const float step = kShadowTargetStep * ctx->cellSize;
        const float half = ceilf((r + kShadowTargetMargin * ctx->cellSize) / step) * step;
        ctx->shadowMatrix[2] = boxProjection(ctx, snappedBox(view, center, half, ctx->shadowSize[2])) * view;
    } else {
        ctx->shadowMatrix[2] = ctx->shadowMatrix[1];
    }

    // Passes
    glUseProgram(ctx->depthProgram.program);
    // Teste de profundidade ligado aqui mesmo: sem ele o GL não grava
    // profundidade (no 1º frame ele ainda não foi ligado pelo passe principal,
    // e o mapa do terreno, que só é desenhado uma vez, ficava vazio)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(kShadowOffsetFactor, kShadowOffsetUnits);

    if (ctx->terrainShadowDirty) {
        bool scissor = false;
        if (!ctx->terrainShadowAll) {
            // retângulo de texels onde a caixa (8 cantos) cai no mapa, + folga
            // para o PCF e o polygon offset
            const Mat4& m = ctx->shadowMatrix[0];
            const int32_t size = ctx->shadowSize[0];
            float x0 = 1.0e30f, y0 = 1.0e30f, x1 = -1.0e30f, y1 = -1.0e30f;
            for (int32_t k = 0; k < 8; ++k) {
                const Vec3 p = { (k & 1) ? ctx->terrainShadowHi.x : ctx->terrainShadowLo.x,
                                 (k & 2) ? ctx->terrainShadowHi.y : ctx->terrainShadowLo.y,
                                 (k & 4) ? ctx->terrainShadowHi.z : ctx->terrainShadowLo.z };
                const float sx = (m[0, 0] * p.x + m[0, 1] * p.y + m[0, 2] * p.z + m[0, 3]) * 0.5f + 0.5f;
                const float sy = (m[1, 0] * p.x + m[1, 1] * p.y + m[1, 2] * p.z + m[1, 3]) * 0.5f + 0.5f;
                x0 = fminf(x0, sx); x1 = fmaxf(x1, sx);
                y0 = fminf(y0, sy); y1 = fmaxf(y1, sy);
            }
            const int32_t ix0 = int32_t(floorf(x0 * float(size))) - kShadowScissorMargin;
            const int32_t iy0 = int32_t(floorf(y0 * float(size))) - kShadowScissorMargin;
            const int32_t ix1 = int32_t(ceilf(x1 * float(size))) + kShadowScissorMargin;
            const int32_t iy1 = int32_t(ceilf(y1 * float(size))) + kShadowScissorMargin;
            glEnable(GL_SCISSOR_TEST);
            glScissor(ix0, iy0, ix1 - ix0, iy1 - iy0);
            scissor = true;
        }
        beginPass(ctx, 0, ctx->shadowMatrix[0]);    // o clear respeita o scissor
        if (ctx->hasTerrain()) {
            glBindVertexArray(ctx->terrainVao);
            glDrawArrays(GL_TRIANGLES, 0, kTriangleCount * 3);
            drawSkirtDepth(ctx);                 // a continuação também projeta
        }
        if (scissor) glDisable(GL_SCISSOR_TEST);
        ctx->terrainShadowDirty = false;
        ctx->terrainShadowAll = false;
    }

    beginPass(ctx, 1, ctx->shadowMatrix[1]);
    drawObjectsDepth(ctx, -1, following ? target : -1);

    if (following) {
        beginPass(ctx, 2, ctx->shadowMatrix[2]);
        drawObjectsDepth(ctx, target, -1);
    }

    glDisable(GL_POLYGON_OFFSET_FILL);
}

void initShadowUniforms(GLuint program, ShadowUniforms& u) {
    u.matrices = glGetUniformLocation(program, "uShadowMatrix");
    u.targetOn = glGetUniformLocation(program, "uShadowTargetOn");
    const char* names[kShadowMaps] = { "uShadow0", "uShadow1", "uShadow2" };
    glUseProgram(program);
    for (int32_t i = 0; i < kShadowMaps; ++i) {
        u.maps[i] = glGetUniformLocation(program, names[i]);
        glUniform1i(u.maps[i], kUnitShadowFirst + i);
    }
}

// Programa já em uso. Deixa a unidade de textura 0 ativa.
void bindShadowMaps(const RzContext* ctx, const ShadowUniforms& u) {
    for (int32_t i = 0; i < kShadowMaps; ++i) {
        glActiveTexture(GL_TEXTURE0 + GLenum(kUnitShadowFirst + i));
        glBindTexture(GL_TEXTURE_2D, ctx->shadowTex[i]);
    }
    glActiveTexture(GL_TEXTURE0);
    glUniformMatrix4fv(u.matrices, kShadowMaps, GL_TRUE, ctx->shadowMatrix[0].e);
    glUniform1i(u.targetOn, ctx->shadowTargetOn ? 1 : 0);
}

} // namespace rz
