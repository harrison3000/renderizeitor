// Objetos: malhas de polígonos convexos em coordenadas absolutas do mundo.
//
// Carga: rzCreateObject cria o objeto vazio (vértices, VAO e VBO);
// rzAddObjectPolygon valida e copia um polígono, triangula em leque, cresce os
// buffers da CPU e realoca o VBO.
// Update (rzUpdateObjectVertices): converte as posições 8.24 para float e
// recalcula a cor sombreada de cada polígono.
// Frame (drawObjects): reenvia o VBO dos objetos alterados (sem alocar) e faz
// um glDrawArrays por objeto visível.

#include "rz_internal.h"

using namespace rz;

namespace {

constexpr int32_t  kMaxObjectVertices = 65536;              // índice uint16
constexpr uint32_t kDefaultObjectColor = 0x00B0A090u;       // provisória
constexpr float    kFixed824ToFloat = 1.0f / 16777216.0f;   // 2^-24

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

// Libera os recursos de GPU e esvazia o slot (fica livre para reuso).
void freeObject(Object& o) {
    if (o.vbo) glDeleteBuffers(1, &o.vbo);
    if (o.vao) glDeleteVertexArrays(1, &o.vao);
    o = Object{};
}

// 8.24 -> mundo (y para cima no renderer), conforme a convenção de eixos.
void loadPositions(const RzContext* ctx, Object& o, const uint32_t* vertices) {
    const float scale = ctx->cellSize * kFixed824ToFloat;
    const bool zUp = ctx->objectAxes == RZ_AXES_Z_UP;
    for (int32_t i = 0; i < o.vertexCount(); ++i) {
        const float a = float(vertices[i * 3 + 0]) * scale;
        const float b = float(vertices[i * 3 + 1]) * scale;
        const float c = float(vertices[i * 3 + 2]) * scale;
        o.world[i] = zUp ? Vec3{ a, c, b } : Vec3{ a, b, c };
    }
}

// Normal de Newell do polígono e cor sombreada (pelos dois lados, enquanto o
// winding do legado não é conhecido).
uint32_t polygonColor(const Object& o, size_t p) {
    const uint16_t* idx = o.indices.data() + o.polygonStart[p];
    const int32_t n = o.polygonLength[p];
    Vec3 normal = { 0.0f, 0.0f, 0.0f };
    for (int32_t i = 0; i < n; ++i) {
        const Vec3 cur  = o.world[idx[i]];
        const Vec3 next = o.world[idx[i + 1 == n ? 0 : i + 1]];
        normal.x += (cur.y - next.y) * (cur.z + next.z);
        normal.y += (cur.z - next.z) * (cur.x + next.x);
        normal.z += (cur.x - next.x) * (cur.y + next.y);
    }
    return shadeFlat(o.baseColor, normal, true);
}

void computePolygonColors(Object& o) {
    for (size_t p = 0; p < o.polygonStart.size(); ++p) o.polygonColors[p] = polygonColor(o, p);
}

// Monta os vértices dos triângulos e reenvia o vertex buffer inteiro (que já
// tem o tamanho certo: é realocado em rzAddObjectPolygon).
void uploadObject(Object& o) {
    GpuVertex* v = o.staging.data();
    for (const ObjectTriangle& tri : o.triangles) {
        const uint32_t color = o.polygonColors[tri.polygon];
        const uint16_t corners[3] = { tri.a, tri.b, tri.c };
        for (uint16_t index : corners) {
            const Vec3 p = o.world[index];
            *v++ = { p.x, p.y, p.z, color };
        }
    }
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(o.staging.size() * sizeof(GpuVertex)),
                    o.staging.data());
}

} // namespace

namespace rz {

// Culling pela ordem na tela (glFrontFace já define frente = anti-horário visual).
void drawObjects(RzContext* ctx, const Mat4& viewProj, bool /*flipped*/) {
    bool programBound = false;
    for (Object& o : ctx->objects) {
        if (!o.alive || !o.visible || !o.positioned || o.triangles.empty()) continue;
        if (!programBound) {
            glUseProgram(ctx->objectProgram.program);
            glUniformMatrix4fv(ctx->objectProgram.viewProj, 1, GL_TRUE, viewProj.e);
            programBound = true;
        }
        if (o.cull == RZ_CULL_NONE) {
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_CULL_FACE);
            glCullFace(o.cull == RZ_CULL_CW ? GL_BACK : GL_FRONT);
        }
        if (o.gpuDirty) {
            uploadObject(o);
            o.gpuDirty = false;
        }
        glBindVertexArray(o.vao);
        glDrawArrays(GL_TRIANGLES, 0, o.triangleCount() * 3);
    }
}

void destroyAllObjects(RzContext* ctx) {
    for (Object& o : ctx->objects) {
        if (o.alive) freeObject(o);
    }
    ctx->objects.clear();
}

} // namespace rz

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectAxes(RzContext* ctx, int32_t axes) {
    if (!ctx || (axes != RZ_AXES_Y_UP && axes != RZ_AXES_Z_UP)) return RZ_ERR_INVALID_ARG;
    ctx->objectAxes = axes;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzCreateObject(RzContext* ctx, int32_t vertexCount, int32_t* outId) {
    if (!ctx || !outId) return RZ_ERR_INVALID_ARG;
    *outId = -1;
    if (vertexCount < 1) return RZ_ERR_INVALID_ARG;
    if (vertexCount > kMaxObjectVertices) return RZ_ERR_SIZE;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    // Slot livre, ou um novo no fim (fase de carga)
    int32_t id = 0;
    while (id < int32_t(ctx->objects.size()) && ctx->objects[id].alive) ++id;
    if (id == int32_t(ctx->objects.size())) ctx->objects.emplace_back();

    Object& o = ctx->objects[id];
    o.world.assign(size_t(vertexCount), Vec3{ 0.0f, 0.0f, 0.0f });

    // Vertex buffer (vazio até o primeiro polígono): posição (vec3) + cor (4 bytes normalizados)
    glGenVertexArrays(1, &o.vao);
    glGenBuffers(1, &o.vbo);
    glBindVertexArray(o.vao);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, color)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    o.alive      = true;
    o.visible    = true;
    o.positioned = false;
    o.gpuDirty   = false;
    o.cull       = RZ_CULL_NONE;
    o.baseColor  = kDefaultObjectColor;

    if (glGetError() != GL_NO_ERROR) {
        freeObject(o);
        return RZ_ERR_GL;
    }
    *outId = id;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzAddObjectPolygon(RzContext* ctx, int32_t id,
                                                   const uint16_t* indices, int32_t count) {
    if (!validId(ctx, id) || !indices || count < 0) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];

    int32_t n = count;
    if (n > 1 && indices[n - 1] == indices[0]) --n;          // fechamento do legado
    for (int32_t i = 0; i < n; ++i) {
        if (indices[i] >= o.vertexCount()) return RZ_ERR_INVALID_ARG;
    }
    if (n < 3) return RZ_OK;                                  // degenerado: ignorado
    if (o.polygonStart.size() >= 65535) return RZ_ERR_SIZE;  // polígono do triângulo é uint16
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    // Cópia dos índices e leque (fase de carga: os vetores crescem aqui)
    const uint16_t polygon = uint16_t(o.polygonStart.size());
    o.polygonStart.push_back(int32_t(o.indices.size()));
    o.polygonLength.push_back(n);
    o.indices.insert(o.indices.end(), indices, indices + n);
    for (int32_t i = 1; i + 1 < n; ++i) {
        o.triangles.push_back({ indices[0], indices[i], indices[i + 1], polygon });
    }
    o.polygonColors.push_back(polygonColor(o, polygon));
    o.staging.resize(o.triangles.size() * 3);

    // VBO com o novo tamanho; o conteúdo vai no próximo frame
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.staging.size() * sizeof(GpuVertex)),
                 nullptr, GL_DYNAMIC_DRAW);
    o.gpuDirty = true;
    return glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzUpdateObjectVertices(RzContext* ctx, int32_t id,
                                                       const uint32_t* vertices) {
    if (!validId(ctx, id) || !vertices) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];
    loadPositions(ctx, o, vertices);
    computePolygonColors(o);
    o.positioned = true;
    o.gpuDirty   = true;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzDestroyObject(RzContext* ctx, int32_t id) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    platformMakeCurrent(ctx->platform);
    freeObject(ctx->objects[id]);
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectColor(RzContext* ctx, int32_t id, uint32_t rgb) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];
    o.baseColor = rgb & 0x00FFFFFFu;
    computePolygonColors(o);
    o.gpuDirty = true;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectVisible(RzContext* ctx, int32_t id, int32_t visible) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    ctx->objects[id].visible = visible != 0;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetObjectCulling(RzContext* ctx, int32_t id, int32_t cull) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    if (cull < RZ_CULL_NONE || cull > RZ_CULL_CCW) return RZ_ERR_INVALID_ARG;
    ctx->objects[id].cull = cull;
    return RZ_OK;
}

} // extern "C"
