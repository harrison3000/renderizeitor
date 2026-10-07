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

constexpr int32_t kWheelVertices = kWheelSideVerts + kWheelCapVerts;   // por roda

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

Vec3 scale(Vec3 v, float s) { return { v.x * s, v.y * s, v.z * s }; }
Vec3 normalized(Vec3 v) {
    const float l2 = dot(v, v);
    return l2 > 0.0f ? scale(v, 1.0f / sqrtf(l2)) : Vec3{ 0.0f, 0.0f, 0.0f };
}

void emitTriangle(TerrainVertex*& out, Vec3 a, Vec3 b, Vec3 c, Vec3 normal) {
    const uint32_t n = packNormal(normal);
    const Vec3 v[3] = { a, b, c };
    for (const Vec3& p : v) *out++ = { p.x, p.y, p.z, kWheelColor, 0, 0, 0, 0, n };
}

// Um canto do disco de uma face: a posição p e, no quadrado do atlas, o ponto
// (cx, cy) do círculo unitário (centro = 0). face[]: u0, v0, u1, v1. O v cresce
// para baixo na textura, então o "cima" do disco (cy > 0) fica no topo (v menor);
// a face interna é espelhada em u (é vista pelo outro lado).
GpuVertex capCorner(Vec3 p, uint32_t normal, const float face[4], float cx, float cy, bool mirror) {
    const float uc = 0.5f * (face[0] + face[2]), vc = 0.5f * (face[1] + face[3]);
    const float hu = 0.5f * (face[2] - face[0]), hv = 0.5f * (face[3] - face[1]);
    const float u = uc + (mirror ? -cx : cx) * hu;
    const float v = vc - cy * hv;
    return { p.x, p.y, p.z, normal, u, v };
}

void emitCap(GpuVertex*& out, Vec3 center, Vec3 a, Vec3 b, uint32_t normal,
             const float face[4], float ca, float sa, float cb, float sb, bool mirror) {
    *out++ = capCorner(center, normal, face, 0.0f, 0.0f, mirror);
    *out++ = capCorner(a, normal, face, ca, sa, mirror);
    *out++ = capCorner(b, normal, face, cb, sb, mirror);
}

// Monta os quatro cilindros na posição atual e reenvia os VBOs (sem alocar).
// O pneu (a lateral) vai para o VBO plano; as duas tampas vão para o VBO das
// faces com a textura do objeto quando há faces (rzSetObjectWheelFaces), ou
// para o VBO plano, pretas, quando não há.
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
    const Vec3 carMid = scale(frontMid + rearMid, 0.5f);   // centro do carro (p/ saber a face externa)
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

    // Decide uma vez (e fixa) qual tampa de cada roda é a externa, pelo eixo
    // RETO (axis[0], o lado direito do carro), não pelo esterçado: assim o
    // esterçamento, por maior que seja, não troca mais interna por externa.
    if (!o.wheelOuterResolved) {
        for (int32_t i = 0; i < 4; ++i)
            o.wheelPlusOuter[i] = dot(hub[i] - carMid, axis[0]) > 0.0f ? 1 : 0;
        o.wheelOuterResolved = true;
    }

    const bool textured = o.wheelFacesSet;
    TerrainVertex* out = o.wheelStaging.data();
    GpuVertex*     cap = o.wheelCapStaging.data();
    for (int32_t i = 0; i < 4; ++i) {
        const int32_t which = o.wheelFront[i];
        const Vec3 fw = fwd[which], ax = axis[which];
        const float radius = 0.5f * o.wheelDiameter[i];
        const Vec3 half = scale(ax, 0.5f * kWheelWidth * o.wheelDiameter[i]);
        const Vec3 p = hub[i];
        const bool plusIsOuter = o.wheelPlusOuter[i] != 0;   // tampa +eixo é a externa?
        const uint32_t nOut = packNormal(ax), nIn = packNormal(scale(ax, -1.0f));
        // Giro da roda (rzSetObjectWheelSpin): roda o anel em torno do eixo, no
        // plano (fw, u). Afeta a geometria e a UV da face, então o desenho da
        // face gira junto. cr/sr: rotação do giro acumulado desta roda.
        const float cr = cosf(o.wheelRoll[i]), sr = sinf(o.wheelRoll[i]);
        for (int32_t k = 0; k < kWheelSegments; ++k) {
            const float c0 = ringC[k] * cr - ringS[k] * sr,     s0 = ringS[k] * cr + ringC[k] * sr;
            const float c1 = ringC[k + 1] * cr - ringS[k + 1] * sr, s1 = ringS[k + 1] * cr + ringC[k + 1] * sr;
            const Vec3 r0 = scale(fw, c0 * radius) + scale(u, s0 * radius);
            const Vec3 r1 = scale(fw, c1 * radius) + scale(u, s1 * radius);
            const Vec3 n  = normalized(r0 + r1);               // normal do lado
            const Vec3 o0 = p + half + r0, o1 = p + half + r1; // lado de fora (+eixo)
            const Vec3 i0 = p - half + r0, i1 = p - half + r1; // lado de dentro
            emitTriangle(out, i0, o0, o1, n);                  // pneu (lateral): sempre plano
            emitTriangle(out, i0, o1, i1, n);
            if (textured) {
                // A UV usa o anel SEM o giro (ringC/ringS): a textura fica presa
                // à roda e gira junto com a geometria (que usa o anel girado). A
                // face externa usa o quadrado externo (sem espelho), a interna o
                // interno (espelhado em u).
                const float* fPlus = plusIsOuter ? o.wheelFaceOuter : o.wheelFaceInner;
                const float* fMinus = plusIsOuter ? o.wheelFaceInner : o.wheelFaceOuter;
                emitCap(cap, p + half, o1, o0, nOut, fPlus,                  // tampa +eixo
                        ringC[k + 1], ringS[k + 1], ringC[k], ringS[k], !plusIsOuter);
                emitCap(cap, p - half, i0, i1, nIn, fMinus,                  // tampa -eixo
                        ringC[k], ringS[k], ringC[k + 1], ringS[k + 1], plusIsOuter);
            } else {
                emitTriangle(out, p + half, o1, o0, ax);       // tampas pretas
                emitTriangle(out, p - half, i0, i1, scale(ax, -1.0f));
            }
        }
    }
    o.wheelFlatVerts = int32_t(out - o.wheelStaging.data());
    o.wheelCapVerts  = int32_t(cap - o.wheelCapStaging.data());

    glBindBuffer(GL_ARRAY_BUFFER, o.wheelVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(size_t(o.wheelFlatVerts) * sizeof(TerrainVertex)),
                    o.wheelStaging.data());
    if (o.wheelCapVerts > 0) {
        glBindBuffer(GL_ARRAY_BUFFER, o.wheelCapVbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(size_t(o.wheelCapVerts) * sizeof(GpuVertex)),
                        o.wheelCapStaging.data());
    }
}

} // namespace

void freeWheels(Object& o) {
    if (o.wheelVbo) glDeleteBuffers(1, &o.wheelVbo);
    if (o.wheelVao) glDeleteVertexArrays(1, &o.wheelVao);
    if (o.wheelCapVbo) glDeleteBuffers(1, &o.wheelCapVbo);
    if (o.wheelCapVao) glDeleteVertexArrays(1, &o.wheelCapVao);
    o.wheelVbo = 0;
    o.wheelVao = 0;
    o.wheelCapVbo = 0;
    o.wheelCapVao = 0;
}

// Antes dos passes do frame (junto com prepareObjects). Com velocidade, adianta
// o giro de cada roda neste frame: ângulo = distância andada / raio (cada roda
// pelo seu diâmetro; positivo = para a frente), e remonta.
void prepareWheels(RzContext* ctx, Object& o) {
    if (o.wheelStaging.empty() || !o.positioned) return;
    if (o.wheelSpeed != 0.0f) {
        const float dist = o.wheelSpeed * ctx->cellSize;       // tiles/frame -> mundo
        for (int32_t i = 0; i < 4; ++i) {
            o.wheelRoll[i] -= dist / (0.5f * o.wheelDiameter[i]);   // -: topo vai para a frente
            o.wheelRoll[i] = fmodf(o.wheelRoll[i], 2.0f * kPi);
        }
        o.wheelsDirty = true;
    }
    if (!o.wheelsDirty) return;
    buildWheels(o);
    o.wheelsDirty = false;
}

// Passe de sombra: o programa de profundidade já está ligado. A posição está no
// atributo 0 nos dois VBOs (pneu e faces), então os dois projetam sombra.
void drawWheelsDepth(const Object& o) {
    if (o.wheelStaging.empty() || !o.positioned) return;
    glBindVertexArray(o.wheelVao);
    glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelFlatVerts));
    if (o.wheelCapVerts > 0) {
        glBindVertexArray(o.wheelCapVao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelCapVerts));
    }
}

// Passe principal, depois dos objetos: o pneu (lateral), programa do terreno
// sem textura. As faces vão em drawWheelFaces. Sem culling (o depth resolve).
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
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelFlatVerts));
    }
}

// As faces (discos) das rodas: programa dos objetos, com a textura do objeto,
// depois de drawWheels. Sem culling, como o pneu.
void drawWheelFaces(RzContext* ctx, const Mat4& viewProj) {
    bool bound = false;
    for (const Object& o : ctx->objects) {
        if (!o.alive || !o.positioned || o.wheelCapVerts <= 0 || !objectInRange(ctx, o)) continue;
        const ObjectProgram& prog = ctx->objectProgram;
        if (!bound) {
            glUseProgram(prog.program);
            glUniformMatrix4fv(prog.viewProj, 1, GL_TRUE, viewProj.e);
            bindShadowMaps(ctx, prog.shadow);           // deixa a unidade 0 ativa
            bindFog(ctx, prog.fog);
            glDisable(GL_CULL_FACE);
            bound = true;
        }
        glBindTexture(GL_TEXTURE_2D, o.texture ? o.texture : ctx->fallbackTex);
        glBindVertexArray(o.wheelCapVao);
        glDrawArrays(GL_TRIANGLES, 0, GLsizei(o.wheelCapVerts));
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
    o.wheelSpeed = 0.0f;
    o.wheelOuterResolved = false;      // redecide interna/externa para as novas rodas
    for (int32_t i = 0; i < 4; ++i) {
        o.wheelVertex[i]   = wheels[i].hubVertex;
        o.wheelFront[i]    = wheels[i].front ? 1 : 0;
        o.wheelDiameter[i] = wheels[i].diameter * cs;
        o.wheelRoll[i]     = 0.0f;
    }
    if (!o.wheelVao) {                                          // carga: aloca uma vez
        o.wheelStaging.resize(size_t(4) * kWheelVertices);     // pneu + (faces pretas, se sem textura)
        glGenVertexArrays(1, &o.wheelVao);
        glGenBuffers(1, &o.wheelVbo);
        glBindVertexArray(o.wheelVao);
        glBindBuffer(GL_ARRAY_BUFFER, o.wheelVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.wheelStaging.size() * sizeof(TerrainVertex)),
                     nullptr, GL_DYNAMIC_DRAW);
        setTerrainVertexLayout();

        o.wheelCapStaging.resize(size_t(4) * kWheelCapVerts);  // faces texturizadas
        glGenVertexArrays(1, &o.wheelCapVao);
        glGenBuffers(1, &o.wheelCapVbo);
        glBindVertexArray(o.wheelCapVao);
        glBindBuffer(GL_ARRAY_BUFFER, o.wheelCapVbo);
        glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.wheelCapStaging.size() * sizeof(GpuVertex)),
                     nullptr, GL_DYNAMIC_DRAW);
        setObjectVertexLayout();
        glBindVertexArray(0);
    }
    o.wheelsDirty = true;
    return recordError(ctx, "rzSetObjectWheels", glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectWheelFaces(RzContext* ctx, int32_t id,
                                                      const RzWheelFace* outer, const RzWheelFace* inner) {
    if (!validId(ctx, id) || !outer || !inner) return recordError(ctx, "rzSetObjectWheelFaces", RZ_ERR_INVALID_ARG);
    Object& o = ctx->objects[id];
    if (o.wheelStaging.empty()) return recordError(ctx, "rzSetObjectWheelFaces", RZ_ERR_INVALID_ARG);   // sem rzSetObjectWheels
    const float in[8] = { outer->u0, outer->v0, outer->u1, outer->v1,
                          inner->u0, inner->v0, inner->u1, inner->v1 };
    for (float uv : in) if (!std::isfinite(uv)) return recordError(ctx, "rzSetObjectWheelFaces", RZ_ERR_INVALID_ARG);
    for (int32_t i = 0; i < 4; ++i) {
        o.wheelFaceOuter[i] = in[i];
        o.wheelFaceInner[i] = in[i + 4];
    }
    o.wheelFacesSet = true;
    o.wheelsDirty = true;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectWheelSpin(RzContext* ctx, int32_t id, float speed) {
    if (!validId(ctx, id) || !std::isfinite(speed)) return recordError(ctx, "rzSetObjectWheelSpin", RZ_ERR_INVALID_ARG);
    Object& o = ctx->objects[id];
    if (o.wheelStaging.empty()) return recordError(ctx, "rzSetObjectWheelSpin", RZ_ERR_INVALID_ARG);   // sem rzSetObjectWheels
    o.wheelSpeed = speed;
    return RZ_OK;
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
