// Rodas dos objetos: quatro cilindros pretos finos, gerados a cada update.
//
// rzSetObjectWheels: para cada roda, o vértice do objeto que é o centro do
// cubo (hub), se é dianteira ou traseira e o diâmetro (tiles). Precisam ser
// duas dianteiras e duas traseiras.
// rzUpdateObjectWheels: ângulo de esterçamento das duas dianteiras (radianos;
// positivo vira para a esquerda, anti-horário visto de cima). Traseiras retas.
//
// Referencial do carro, refeito a cada envio a partir dos quatro cubos:
//   frente f = meio das dianteiras − meio das traseiras;
//   cima   u = perpendicular a f e à linha entre as rodas, apontando para +y;
//   direita r = f x u.
// Cada roda: cilindro de diâmetro d e largura kWheelWidth x d, eixo = r (nas
// dianteiras, girado em torno de u pelo esterçamento). kWheelSegments lados;
// o anel (cos/sen) e as duas orientações (dianteira, traseira) são calculados
// uma vez por envio.
//
// Desenho: com o programa do terreno no modo sem textura (cor flat iluminada,
// sombras e neblina de graça), sem culling (cilindro fechado: o depth resolve).
// Projetam sombra (mapas 1 e 2, junto com o objeto).

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t kWheelVertices = kWheelSegments * 4 * 3;   // lado (2 tri) + 2 tampas (1 tri cada) por segmento

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

Vec3 scale(Vec3 v, float s) { return { v.x * s, v.y * s, v.z * s }; }
Vec3 normalized(Vec3 v) {
    const float l2 = dot(v, v);
    return l2 > 0.0f ? scale(v, 1.0f / sqrtf(l2)) : Vec3{ 0.0f, 0.0f, 0.0f };
}

void emitTriangle(TerrainVertex*& out, Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
    const Vec3 l = lightDirection();
    float ndotl = dot(normal, l);
    if (ndotl < 0.0f) ndotl = 0.0f;
    const float intensity = kAmbient + (1.0f - kAmbient) * ndotl;
    const uint32_t color = shadeFlat(kWheelColor, normal, false);
    const uint8_t light = uint8_t(intensity * 255.0f + 0.5f);
    const Vec3 v[3] = { a, b, c };
    for (const Vec3& p : v) *out++ = { p.x, p.y, p.z, color, 0, 0, 0, light };
}

// Monta os quatro cilindros na posição atual e reenvia o VBO (sem alocar)
void buildWheels(Object& o) {
    Vec3 hub[4];
    for (int32_t i = 0; i < 4; ++i) hub[i] = o.world[o.wheelVertex[i]];
    Vec3 frontMid = { 0, 0, 0 }, rearMid = { 0, 0, 0 };
    Vec3 fr[2], rr[2];
    int32_t nf = 0, nr = 0;
    for (int32_t i = 0; i < 4; ++i) {
        if (o.wheelFront[i]) { frontMid = frontMid + scale(hub[i], 0.5f); fr[nf++] = hub[i]; }
        else                 { rearMid  = rearMid  + scale(hub[i], 0.5f); rr[nr++] = hub[i]; }
    }
    const Vec3 f = normalized(frontMid - rearMid);
    // linha entre as rodas (as duas duplas, no mesmo sentido)
    Vec3 axle = fr[1] - fr[0];
    Vec3 axle2 = rr[1] - rr[0];
    if (dot(axle, axle2) < 0.0f) axle2 = scale(axle2, -1.0f);
    axle = axle + axle2;
    Vec3 u = normalized(cross(axle, f));
    if (u.y < 0.0f) u = scale(u, -1.0f);
    if (dot(u, u) == 0.0f) u = { 0.0f, 1.0f, 0.0f };

    // anel unitário (fechado: ring[kWheelSegments] = ring[0])
    float ringC[kWheelSegments + 1], ringS[kWheelSegments + 1];
    for (int32_t k = 0; k <= kWheelSegments; ++k) {
        const float a = 2.0f * kPi * float(k % kWheelSegments) / float(kWheelSegments);
        ringC[k] = cosf(a);
        ringS[k] = sinf(a);
    }
    // orientações: [0] traseira (reta), [1] dianteira (esterçada; positivo = esquerda)
    const float s = sinf(o.wheelSteer), c = cosf(o.wheelSteer);
    const Vec3 fwd[2]  = { f, scale(f, c) + scale(cross(u, f), s) };
    const Vec3 axis[2] = { normalized(cross(fwd[0], u)), normalized(cross(fwd[1], u)) };

    TerrainVertex* out = o.wheelStaging.data();
    for (int32_t i = 0; i < 4; ++i) {
        const int32_t which = o.wheelFront[i];
        const Vec3 fw = fwd[which], ax = axis[which];
        const float radius = 0.5f * o.wheelDiameter[i];
        const Vec3 half = scale(ax, 0.5f * kWheelWidth * o.wheelDiameter[i]);
        const Vec3 p = hub[i];
        for (int32_t k = 0; k < kWheelSegments; ++k) {
            const Vec3 r0 = scale(fw, ringC[k] * radius)     + scale(u, ringS[k] * radius);
            const Vec3 r1 = scale(fw, ringC[k + 1] * radius) + scale(u, ringS[k + 1] * radius);
            const Vec3 n  = normalized(r0 + r1);               // normal do lado
            const Vec3 o0 = p + half + r0, o1 = p + half + r1; // lado de fora (+eixo)
            const Vec3 i0 = p - half + r0, i1 = p - half + r1; // lado de dentro
            emitTriangle(out, i0, o0, o1, n);
            emitTriangle(out, i0, o1, i1, n);
            emitTriangle(out, p + half, o1, o0, ax);           // tampas
            emitTriangle(out, p - half, i0, i1, scale(ax, -1.0f));
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, o.wheelVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(o.wheelStaging.size() * sizeof(TerrainVertex)),
                    o.wheelStaging.data());
}

} // namespace

void freeWheels(Object& o) {
    if (o.wheelVbo) glDeleteBuffers(1, &o.wheelVbo);
    if (o.wheelVao) glDeleteVertexArrays(1, &o.wheelVao);
    o.wheelVbo = 0;
    o.wheelVao = 0;
}

// Antes dos passes do frame (junto com prepareObjects)
void prepareWheels(Object& o) {
    if (o.wheelStaging.empty() || !o.positioned || !o.wheelsDirty) return;
    buildWheels(o);
    o.wheelsDirty = false;
}

// Passe de sombra: o programa de profundidade já está ligado
void drawWheelsDepth(const Object& o) {
    if (o.wheelStaging.empty() || !o.positioned) return;
    glBindVertexArray(o.wheelVao);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelStaging.size()));
}

// Passe principal, depois dos objetos: programa do terreno, sem textura
void drawWheels(RzContext* ctx, const Mat4& viewProj) {
    bool bound = false;
    for (const Object& o : ctx->objects) {
        if (!o.alive || !o.positioned || o.wheelStaging.empty() || !objectInRange(ctx, o)) continue;
        if (!bound) {
            const TerrainProgram& t = ctx->terrainProgram;
            glUseProgram(t.program);
            glUniformMatrix4fv(t.viewProj, 1, GL_TRUE, viewProj.e);
            bindShadowMaps(ctx, t.shadow);
            bindFog(ctx, t.fog);
            glUniform1i(t.textured, 0);
            glDisable(GL_CULL_FACE);
            bound = true;
        }
        glBindVertexArray(o.wheelVao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelStaging.size()));
    }
}

} // namespace rz

using namespace rz;

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectWheels(RzContext* ctx, int32_t id, const RzWheel* wheels) {
    if (!validId(ctx, id) || !wheels) return recordError(ctx, "rzSetObjectWheels", RZ_ERR_INVALID_ARG);
    Object& o = ctx->objects[id];
    int32_t fronts = 0;
    for (int32_t i = 0; i < 4; ++i) {
        if (wheels[i].hubVertex >= o.vertexCount()) return recordError(ctx, "rzSetObjectWheels", RZ_ERR_INVALID_ARG);
        if (!(wheels[i].diameter > 0.0f && wheels[i].diameter < 1.0e6f)) return recordError(ctx, "rzSetObjectWheels", RZ_ERR_INVALID_ARG);
        if (wheels[i].front) ++fronts;
    }
    if (fronts != 2) return recordError(ctx, "rzSetObjectWheels", RZ_ERR_INVALID_ARG);   // duas dianteiras e duas traseiras
    if (!platformMakeCurrent(ctx->platform)) return recordError(ctx, "rzSetObjectWheels", RZ_ERR_GL);

    const float cs = ctx->cellSize;
    o.wheelSteer = 0.0f;
    for (int32_t i = 0; i < 4; ++i) {
        o.wheelVertex[i]   = wheels[i].hubVertex;
        o.wheelFront[i]    = wheels[i].front ? 1 : 0;
        o.wheelDiameter[i] = wheels[i].diameter * cs;
    }
    if (!o.wheelVao) {                                          // carga: aloca uma vez
        o.wheelStaging.resize(size_t(4) * kWheelVertices);
        glGenVertexArrays(1, &o.wheelVao);
        glGenBuffers(1, &o.wheelVbo);
        glBindVertexArray(o.wheelVao);
        glBindBuffer(GL_ARRAY_BUFFER, o.wheelVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.wheelStaging.size() * sizeof(TerrainVertex)),
                     nullptr, GL_DYNAMIC_DRAW);
        const GLsizei stride = sizeof(TerrainVertex);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                              reinterpret_cast<void*>(offsetof(TerrainVertex, color)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                              reinterpret_cast<void*>(offsetof(TerrainVertex, u)));
        glEnableVertexAttribArray(2);
        glBindVertexArray(0);
    }
    o.wheelsDirty = true;
    return recordError(ctx, "rzSetObjectWheels", glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzUpdateObjectWheels(RzContext* ctx, int32_t id, float steer) {
    if (!validId(ctx, id) || !std::isfinite(steer)) return recordError(ctx, "rzUpdateObjectWheels", RZ_ERR_INVALID_ARG);
    Object& o = ctx->objects[id];
    if (o.wheelStaging.empty()) return recordError(ctx, "rzUpdateObjectWheels", RZ_ERR_INVALID_ARG);   // sem rzSetObjectWheels
    if (steer != o.wheelSteer) {
        o.wheelSteer = steer;
        o.wheelsDirty = true;
    }
    return RZ_OK;
}

} // extern "C"
