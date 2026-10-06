// Sombras: cascatas (cascaded shadow maps) da luz direcional. Projeções
// ortográficas com a mesma view da luz, fixa, olhando para o centro do
// terreno; cada cascata é uma caixa nessa view.
//
// Seguindo um alvo (com neblina), por distância 3D ao olho:
//   0  0 .. kCascadeSplit0 (6 tiles)    512   terreno + objetos, todo frame
//   1  .. kCascadeSplit1 (20 tiles)     1024  terreno + objetos, todo frame
//   2  .. fim da neblina                2048  só terreno
// As caixas 0 e 1 envolvem a esfera da fatia do frustum da câmera (raio só
// depende do FOV e dos limites: não muda quando a câmera gira) e andam em
// passos de um texel, para a sombra não tremer. Delas, só as linhas do
// terreno que podem cair na caixa são desenhadas (a malha é row-major).
// A 2 é um quadrado em volta do olho (meia-largura = fim da neblina +
// recentragem + margem), desenhado só quando o olho anda kCascadeFarRecenter
// tiles desde o último desenho, a neblina muda de tamanho ou o terreno muda
// (terrainShadowDirty: inteira na carga, markTerrainShadowAll; em
// rzUpdateTerrain, só o retângulo onde cai a caixa alterada,
// markTerrainShadowBox, com scissor). Objetos além de kCascadeSplit1 não
// projetam sombra (aceito: a neblina começa logo depois).
//
// O shader escolhe a cascata pela distância ao olho, com uma faixa de
// kCascadeBlend tiles antes de cada divisa misturando as duas vizinhas. Fora
// da caixa de uma cascata, ela não sombreia.
// Na visão geral (sem alvo, sem neblina): só a cascata 2, cobrindo o terreno
// inteiro, com terreno e objetos, refeita todo frame.
// Os tamanhos são limitados a GL_MAX_TEXTURE_SIZE (o GL 3.3 só garante 1024).
// Em profundidade, todas cobrem a esfera do terreno inteiro com folga.

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t kShadowRequested[kShadowMaps] = {
    kCascadeNearSize, kCascadeMidSize, kCascadeFarSize };

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


// Esfera que envolve a parte do frustum da câmera a distância (3D) entre d0 e
// d1 do olho: a fatia de profundidade [d0 · cos(meia-diagonal), d1]. O raio
// só depende do FOV e dos limites (estável com a câmera girando).
float sliceSphere(const RzContext* ctx, float d0, float d1, Vec3* center) {
    const float tx = 1.0f / ctx->focalX, ty = 1.0f / ctx->focalY;
    const float k2 = tx * tx + ty * ty;                // (meia-diagonal / profundidade)^2
    const float n = d0 / sqrtf(1.0f + k2), f = d1;
    float z = 0.5f * (1.0f + k2) * (f + n);             // centro que iguala os cantos
    if (z > f) z = f;
    if (z < n) z = n;
    const float rFar  = sqrtf((f - z) * (f - z) + f * f * k2);
    const float rNear = sqrtf((z - n) * (z - n) + n * n * k2);
    const Vec3 fw = ctx->camForward;
    *center = { ctx->eyePos.x + fw.x * z, ctx->eyePos.y + fw.y * z, ctx->eyePos.z + fw.z * z };
    return rFar > rNear ? rFar : rNear;
}

// Quads da malha do terreno que podem cair na caixa: os 4 cantos da caixa,
// seguidos na direção da luz até a menor e a maior altura do terreno (ele
// todo fica entre elas), dão um retângulo no plano xz (linhas r0..r1, colunas
// c0..c1). *outside: a caixa passa da borda do mapa (a continuação também entra).
void terrainQuadsInBox(const RzContext* ctx, const Mat4& view, ShadowBox b,
                       int32_t* r0, int32_t* r1, int32_t* c0, int32_t* c1, bool* outside) {
    const Vec3 rx = { view[0, 0], view[0, 1], view[0, 2] };
    const Vec3 uy = { view[1, 0], view[1, 1], view[1, 2] };
    const Vec3 fw = { -view[2, 0], -view[2, 1], -view[2, 2] };     // da luz para o terreno
    // olho da luz: view = [R | -R·eye] → eye = -(R^T · t)
    const Vec3 t = { view[0, 3], view[1, 3], view[2, 3] };
    const Vec3 eye = { -(rx.x * t.x + uy.x * t.y - fw.x * t.z),
                       -(rx.y * t.x + uy.y * t.y - fw.y * t.z),
                       -(rx.z * t.x + uy.z * t.y - fw.z * t.z) };
    const float cs = ctx->cellSize;
    const float heights[2] = { float(ctx->terrainMinHeight) * ctx->heightScale,
                               float(ctx->terrainMaxHeight) * ctx->heightScale };
    float x0 = 1.0e30f, z0 = 1.0e30f, x1 = -1.0e30f, z1 = -1.0e30f;
    for (int32_t k = 0; k < 4; ++k) {
        const float lx = b.cx + ((k & 1) ? b.half : -b.half);
        const float ly = b.cy + ((k & 2) ? b.half : -b.half);
        const Vec3 p0 = { eye.x + rx.x * lx + uy.x * ly, eye.y + rx.y * lx + uy.y * ly, eye.z + rx.z * lx + uy.z * ly };
        for (const float h : heights) {
            const float s = (h - p0.y) / fw.y;               // a luz desce: fw.y < 0
            const float x = p0.x + fw.x * s, z = p0.z + fw.z * s;
            x0 = fminf(x0, x); x1 = fmaxf(x1, x);
            z0 = fminf(z0, z); z1 = fmaxf(z1, z);
        }
    }
    const float edge = float(kQuadsPerSide) * cs;
    *outside = x0 < 0.0f || z0 < 0.0f || x1 > edge || z1 > edge;
    auto clampQuad = [](int32_t q) { return q < 0 ? 0 : (q > kQuadsPerSide - 1 ? kQuadsPerSide - 1 : q); };
    *r0 = clampQuad(int32_t(floorf(z0 / cs)) - 1);
    *r1 = clampQuad(int32_t(ceilf(z1 / cs)) + 1);
    *c0 = clampQuad(int32_t(floorf(x0 / cs)) - 1);
    *c1 = clampQuad(int32_t(ceilf(x1 / cs)) + 1);
    if (z1 < 0.0f || x1 < 0.0f || z0 > edge || x0 > edge) { *r0 = 1; *r1 = 0; }   // caixa toda fora do mapa
}

// Terreno (só os quads da caixa: um glDrawArrays por linha) e, se ela passa
// da borda, a continuação
void drawTerrainDepth(const RzContext* ctx, const Mat4& view, ShadowBox b) {
    if (!ctx->hasTerrain()) return;
    int32_t r0, r1, c0, c1;
    bool outside;
    terrainQuadsInBox(ctx, view, b, &r0, &r1, &c0, &c1, &outside);
    glBindVertexArray(ctx->terrainVao);
    if (c0 == 0 && c1 == kQuadsPerSide - 1) {               // linhas inteiras: um envio só
        if (r0 <= r1) glDrawArrays(GL_TRIANGLES, r0 * kQuadsPerSide * 6, (r1 - r0 + 1) * kQuadsPerSide * 6);
    } else {
        for (int32_t r = r0; r <= r1; ++r)
            glDrawArrays(GL_TRIANGLES, (r * kQuadsPerSide + c0) * 6, (c1 - c0 + 1) * 6);
    }
    if (outside) drawSkirtDepth(ctx);
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

// Calcula as matrizes das cascatas e redesenha as que precisam. Usa o olho
// e a câmera do frame (updateCamera antes) e os VBOs já enviados
// (prepareObjects antes). Sem culling (as faces de trás também projetam),
// com polygon offset contra o "acne".
void renderShadowMaps(RzContext* ctx) {
    const Mat4 view = lightView(ctx);
    const float radius = shadowRadius(ctx);
    const float cs = ctx->cellSize;
    const bool overview = ctx->shadowWholeTerrain;

    glUseProgram(ctx->depthProgram.program);
    // Teste de profundidade ligado aqui mesmo: sem ele o GL não grava
    // profundidade (no 1º frame ele ainda não foi ligado pelo passe principal)
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDisable(GL_CULL_FACE);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(kShadowOffsetFactor, kShadowOffsetUnits);

    if (overview) {
        // Visão geral: só a cascata 2, terreno inteiro + objetos, todo frame
        const ShadowBox whole = { 0.0f, 0.0f, radius };
        ctx->shadowMatrix[2] = boxProjection(ctx, whole) * view;
        beginPass(ctx, 2, ctx->shadowMatrix[2]);
        if (ctx->hasTerrain()) {
            glBindVertexArray(ctx->terrainVao);
            glDrawArrays(GL_TRIANGLES, 0, kTriangleCount * 3);
            drawSkirtDepth(ctx);
        }
        drawObjectsDepth(ctx, -1, -1);
        ctx->cascadeFarValid = true;
        ctx->cascadeFarOverview = true;
        ctx->terrainShadowDirty = false;
        ctx->terrainShadowAll = false;
        ctx->shadowMatrix[0] = ctx->shadowMatrix[1] = ctx->shadowMatrix[2];
        glDisable(GL_POLYGON_OFFSET_FILL);
        return;
    }

    // Cascatas 0 e 1: esfera da fatia, terreno (linhas da caixa) + objetos
    const float splits[3] = { 0.0f, kCascadeSplit0 * cs, kCascadeSplit1 * cs };
    const float blend = kCascadeBlend * cs;
    for (int32_t i = 0; i < 2; ++i) {
        Vec3 center;
        const float d0 = i == 0 ? 0.0f : splits[i] - blend;
        float half = sliceSphere(ctx, d0, splits[i + 1], &center);
        half += 2.0f * half / float(ctx->shadowSize[i]);        // + texels do PCF
        const ShadowBox box = snappedBox(view, center, half, ctx->shadowSize[i]);
        ctx->shadowMatrix[i] = boxProjection(ctx, box) * view;
        beginPass(ctx, i, ctx->shadowMatrix[i]);
        drawTerrainDepth(ctx, view, box);
        drawObjectsDepth(ctx, -1, -1);
    }

    // Cascata 2: só terreno, quadrado em volta do olho, refeito só quando precisa
    // alcance da neblina a partir do olho (na transição da visão geral a
    // origem dela sai do olho)
    const Vec3 toFog = ctx->fogOrigin - ctx->eyePos;
    const float farHalf = ctx->fogFar + sqrtf(dot(toFog, toFog)) + (kCascadeFarRecenter + kCascadeFarMargin) * cs;
    const float dx = ctx->eyePos.x - ctx->cascadeFarCenter.x, dz = ctx->eyePos.z - ctx->cascadeFarCenter.z;
    const float recenter = kCascadeFarRecenter * cs;
    // recentra quando o olho andou demais, a neblina mudou ou vem da visão
    // geral; terreno todo novo só redesenha (no mesmo centro: a grade de
    // texels não pula)
    const bool moveBox = !ctx->cascadeFarValid || ctx->cascadeFarOverview || farHalf != ctx->cascadeFarHalf ||
                         dx * dx + dz * dz > recenter * recenter;
    const bool redrawAll = moveBox || (ctx->terrainShadowDirty && ctx->terrainShadowAll);
    if (redrawAll) {
        if (moveBox) {
            ctx->cascadeFarCenter = ctx->eyePos;
            ctx->cascadeFarHalf = farHalf;
            ++ctx->cascadeFarRedraws;
        }
        const ShadowBox box = snappedBox(view, ctx->cascadeFarCenter, ctx->cascadeFarHalf, ctx->shadowSize[2]);
        ctx->shadowMatrix[2] = boxProjection(ctx, box) * view;
        beginPass(ctx, 2, ctx->shadowMatrix[2]);
        drawTerrainDepth(ctx, view, box);
        ctx->cascadeFarValid = true;
        ctx->cascadeFarOverview = false;
    } else if (ctx->terrainShadowDirty) {
        // rzUpdateTerrain: só o retângulo de texels onde a caixa alterada cai
        // (8 cantos) + folga para o PCF e o polygon offset
        const Mat4& m = ctx->shadowMatrix[2];
        const int32_t size = ctx->shadowSize[2];
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
        if (ix1 > 0 && iy1 > 0 && ix0 < size && iy0 < size) {   // fora da cascata: nada a fazer
            glEnable(GL_SCISSOR_TEST);
            glScissor(ix0, iy0, ix1 - ix0, iy1 - iy0);
            beginPass(ctx, 2, ctx->shadowMatrix[2]);             // o clear respeita o scissor
            if (ctx->hasTerrain()) {
                glBindVertexArray(ctx->terrainVao);
                glDrawArrays(GL_TRIANGLES, 0, kTriangleCount * 3);
                drawSkirtDepth(ctx);
            }
            glDisable(GL_SCISSOR_TEST);
        }
    }
    ctx->terrainShadowDirty = false;
    ctx->terrainShadowAll = false;

    glDisable(GL_POLYGON_OFFSET_FILL);
}

void initShadowUniforms(GLuint program, ShadowUniforms& u) {
    u.matrices = glGetUniformLocation(program, "uShadowMatrix");
    u.cascade  = glGetUniformLocation(program, "uCascade");
    u.onlyFar  = glGetUniformLocation(program, "uCascadeOnlyFar");
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
    const float cs = ctx->cellSize;
    glUniform3f(u.cascade, kCascadeSplit0 * cs, kCascadeSplit1 * cs, kCascadeBlend * cs);
    glUniform1i(u.onlyFar, ctx->shadowWholeTerrain ? 1 : 0);
}

} // namespace rz
