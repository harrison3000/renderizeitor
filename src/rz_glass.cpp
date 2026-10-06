// Polígonos translúcidos (vidro) dos objetos.
//
// Cada objeto pode ter, além dos polígonos opacos, polígonos de vidro em tons
// de cinza (16 tons: tom/15, 0 = preto, 15 = transparente). O vidro é um filtro
// multiplicativo (o que está atrás é multiplicado pelo cinza) mais um brilho
// especular "embaçado" (Blinn-Phong de expoente baixo) da luz direcional, que
// some na sombra. Em um passe só, com glBlendFunc(GL_ONE, GL_SRC_ALPHA):
//     destino = especular + destino x cinza
// O filtro não depende da ordem de desenho, então não há ordenação (com dois
// vidros sobrepostos, só o brilho do de trás fica um pouco escurecido pelo da
// frente, ou não: diferença pequena).
//
// Desenhado depois dos objetos opacos, sem gravar profundidade nem alfa de
// destino, com o mesmo culling dos objetos (só a face de fora). Não projeta
// sombra; recebe (no brilho). Com neblina: o filtro vai para 1 e o brilho para 0.
// Normal: a de fora (−Newell, sentido do legado), por polígono.

#include <cmath>

#include "rz_internal.h"
#include "rz_shaders.h"

namespace rz {

namespace {

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

// Monta os vértices (normal de fora e cinza por polígono) e reenvia o VBO
void uploadGlass(Object& o) {
    GlassVertex* v = o.glassStaging.data();
    for (size_t p = 0; p < o.glassStart.size(); ++p) {
        const uint16_t* idx = o.glassIndices.data() + o.glassStart[p];
        const int32_t n = o.glassLength[p];
        Vec3 nn = { 0.0f, 0.0f, 0.0f };
        for (int32_t i = 0; i < n; ++i) {
            const Vec3 cur  = o.world[idx[i]];
            const Vec3 next = o.world[idx[i + 1 == n ? 0 : i + 1]];
            nn.x += (cur.y - next.y) * (cur.z + next.z);
            nn.y += (cur.z - next.z) * (cur.x + next.x);
            nn.z += (cur.x - next.x) * (cur.y + next.y);
        }
        const float len = sqrtf(dot(nn, nn));
        const float inv = len > 0.0f ? -1.0f / len : 0.0f;          // de fora = −Newell
        const Vec3 normal = { nn.x * inv, nn.y * inv, nn.z * inv };
        const float tint = float(o.glassTone[p]) * (1.0f / 15.0f);
        for (int32_t i = 1; i + 1 < n; ++i) {
            const uint16_t corners[3] = { idx[0], idx[i], idx[i + 1] };
            for (uint16_t k : corners) {
                const Vec3 q = o.world[k];
                *v++ = { q.x, q.y, q.z, normal.x, normal.y, normal.z, tint };
            }
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, o.glassVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(o.glassStaging.size() * sizeof(GlassVertex)),
                    o.glassStaging.data());
}

} // namespace

bool createGlassProgram(RzContext* ctx) {
    GlassProgram& g = ctx->glassProgram;
    g.program = linkProgram(kGlassVertexShader, kGlassFragmentShader);
    if (!g.program) return false;
    g.viewProj  = glGetUniformLocation(g.program, "uViewProj");
    g.light     = glGetUniformLocation(g.program, "uLight");
    g.specular  = glGetUniformLocation(g.program, "uSpecular");
    g.shininess = glGetUniformLocation(g.program, "uShininess");
    initShadowUniforms(g.program, g.shadow);
    initFogUniforms(g.program, g.fog);
    return true;
}

void freeGlass(Object& o) {
    if (o.glassVbo) glDeleteBuffers(1, &o.glassVbo);
    if (o.glassVao) glDeleteVertexArrays(1, &o.glassVao);
    o.glassVbo = 0;
    o.glassVao = 0;
}

// Passe do vidro: depois dos objetos opacos
void drawGlass(RzContext* ctx, const Mat4& viewProj) {
    bool programBound = false;
    for (Object& o : ctx->objects) {
        if (!o.alive || !o.positioned || o.glassStaging.empty() || !objectInRange(ctx, o)) continue;
        if (!programBound) {
            const GlassProgram& g = ctx->glassProgram;
            glUseProgram(g.program);
            glUniformMatrix4fv(g.viewProj, 1, GL_TRUE, viewProj.e);
            const Vec3 l = lightDirection();
            glUniform3f(g.light, l.x, l.y, l.z);
            glUniform1f(g.specular, kGlassSpecular);
            glUniform1f(g.shininess, kGlassShininess);
            bindShadowMaps(ctx, g.shadow);
            bindFog(ctx, g.fog);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);                     // como os objetos: só a face de fora
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_SRC_ALPHA);        // especular + destino x cinza
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            glDepthMask(GL_FALSE);
            programBound = true;
        }
        if (o.glassDirty) {
            uploadGlass(o);
            o.glassDirty = false;
        }
        glBindVertexArray(o.glassVao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.glassStaging.size()));
    }
    if (programBound) {
        glDepthMask(GL_TRUE);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDisable(GL_BLEND);
    }
}

} // namespace rz

using namespace rz;

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzAddObjectTranslucentPolygon(RzContext* ctx, int32_t id,
                                                              const uint16_t* indices, int32_t count,
                                                              int32_t tone) {
    if (!validId(ctx, id) || !indices || count < 0) return recordError(ctx, "rzAddObjectTranslucentPolygon", RZ_ERR_INVALID_ARG);
    if (tone < 0 || tone > 15) return recordError(ctx, "rzAddObjectTranslucentPolygon", RZ_ERR_INVALID_ARG);
    Object& o = ctx->objects[id];

    int32_t n = count;
    if (n > 1 && indices[n - 1] == indices[0]) --n;          // fechamento do legado
    for (int32_t i = 0; i < n; ++i) {
        if (indices[i] >= o.vertexCount()) return recordError(ctx, "rzAddObjectTranslucentPolygon", RZ_ERR_INVALID_ARG);
    }
    if (n < 3) return RZ_OK;                                  // degenerado: ignorado
    if (!platformMakeCurrent(ctx->platform)) return recordError(ctx, "rzAddObjectTranslucentPolygon", RZ_ERR_GL);

    if (!o.glassVao) {                                        // primeiro vidro do objeto
        glGenVertexArrays(1, &o.glassVao);
        glGenBuffers(1, &o.glassVbo);
        glBindVertexArray(o.glassVao);
        glBindBuffer(GL_ARRAY_BUFFER, o.glassVbo);
        const GLsizei stride = sizeof(GlassVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(GlassVertex, nx)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(GlassVertex, tint)));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }

    // Cópia (fase de carga: os vetores crescem aqui)
    o.glassStart.push_back(int32_t(o.glassIndices.size()));
    o.glassLength.push_back(n);
    o.glassTone.push_back(uint8_t(tone));
    o.glassIndices.insert(o.glassIndices.end(), indices, indices + n);
    o.glassStaging.resize(o.glassStaging.size() + size_t(n - 2) * 3);

    glBindBuffer(GL_ARRAY_BUFFER, o.glassVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.glassStaging.size() * sizeof(GlassVertex)),
                 nullptr, GL_DYNAMIC_DRAW);
    o.glassDirty = true;
    return recordError(ctx, "rzAddObjectTranslucentPolygon", glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL);
}

} // extern "C"
