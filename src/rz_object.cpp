// Objetos: malhas de polígonos convexos em coordenadas absolutas do mundo.
//
// Carga (rzCreateObject): valida os índices, triangula cada polígono em leque,
// aloca os buffers na CPU e cria o vertex buffer na GPU.
// Update (rzUpdateObjectVertices): converte as posições 8.24 para float,
// recalcula a cor sombreada de cada polígono e reenvia o vertex buffer.
// Frame (drawObjects): um glDrawArrays por objeto visível.

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

// Normal de Newell de cada polígono e cor sombreada (pelos dois lados, enquanto
// o winding do legado não é conhecido).
void computePolygonColors(Object& o) {
    for (size_t p = 0; p < o.polygonStart.size(); ++p) {
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
        o.polygonColors[p] = shadeFlat(o.baseColor, normal, true);
    }
}

// Monta os vértices dos triângulos e reenvia o vertex buffer inteiro.
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

// Primeira passada sobre os índices: conta polígonos, vértices e triângulos,
// e valida o fechamento e o intervalo dos índices.
int32_t scanPolygons(const uint16_t* indices, int32_t indexCount, int32_t vertexCount,
                     int32_t* polygons, int32_t* kept, int32_t* triangles) {
    *polygons = 0; *kept = 0; *triangles = 0;
    int32_t pos = 0;
    while (pos < indexCount) {
        const uint16_t first = indices[pos];
        int32_t end = pos + 1;
        while (end < indexCount && indices[end] != first) ++end;
        if (end >= indexCount) return RZ_ERR_INVALID_ARG;       // polígono sem fechamento
        for (int32_t i = pos; i < end; ++i) {
            if (indices[i] >= vertexCount) return RZ_ERR_INVALID_ARG;
        }
        const int32_t n = end - pos;
        if (n >= 3) {
            ++*polygons;
            *kept += n;
            *triangles += n - 2;
        }
        pos = end + 1;
    }
    if (*polygons > 65535) return RZ_ERR_SIZE;
    return RZ_OK;
}

} // namespace

namespace rz {

// Culling pela ordem na tela (glFrontFace já define frente = anti-horário visual).
void drawObjects(RzContext* ctx, const Mat4& viewProj, bool /*flipped*/) {
    bool programBound = false;
    for (const Object& o : ctx->objects) {
        if (!o.alive || !o.visible) continue;
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

RZ_API RZ_ENTRY int32_t RZ_CALL rzCreateObject(RzContext* ctx,
                                               const uint32_t* vertices, int32_t vertexCount,
                                               const uint16_t* indices, int32_t indexCount,
                                               int32_t* outId) {
    if (!ctx || !vertices || !indices || !outId) return RZ_ERR_INVALID_ARG;
    *outId = -1;
    if (vertexCount < 3 || indexCount < 4) return RZ_ERR_INVALID_ARG;
    if (vertexCount > kMaxObjectVertices) return RZ_ERR_SIZE;

    int32_t polygons, kept, triangles;
    const int32_t err = scanPolygons(indices, indexCount, vertexCount, &polygons, &kept, &triangles);
    if (err != RZ_OK) return err;
    if (triangles == 0) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    // Slot livre, ou um novo no fim (fase de carga)
    int32_t id = 0;
    while (id < int32_t(ctx->objects.size()) && ctx->objects[id].alive) ++id;
    if (id == int32_t(ctx->objects.size())) ctx->objects.emplace_back();

    Object& o = ctx->objects[id];
    o.world.resize(size_t(vertexCount));
    o.polygonStart.reserve(size_t(polygons));
    o.polygonLength.reserve(size_t(polygons));
    o.indices.reserve(size_t(kept));
    o.polygonColors.resize(size_t(polygons));
    o.triangles.reserve(size_t(triangles));
    o.staging.resize(size_t(triangles) * 3);

    // Segunda passada: copia os índices sem os fechamentos e monta os leques
    int32_t pos = 0;
    while (pos < indexCount) {
        const uint16_t first = indices[pos];
        int32_t end = pos + 1;
        while (indices[end] != first) ++end;
        const int32_t n = end - pos;
        if (n >= 3) {
            const uint16_t polygon = uint16_t(o.polygonStart.size());
            o.polygonStart.push_back(int32_t(o.indices.size()));
            o.polygonLength.push_back(n);
            o.indices.insert(o.indices.end(), indices + pos, indices + end);
            for (int32_t i = 1; i + 1 < n; ++i) {
                o.triangles.push_back({ indices[pos], indices[pos + i], indices[pos + i + 1], polygon });
            }
        }
        pos = end + 1;
    }

    // Vertex buffer: posição (vec3) + cor (4 bytes normalizados)
    glGenVertexArrays(1, &o.vao);
    glGenBuffers(1, &o.vbo);
    glBindVertexArray(o.vao);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(triangles) * 3 * GLsizeiptr(sizeof(GpuVertex)),
                 nullptr, GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, color)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);

    o.alive     = true;
    o.visible   = true;
    o.cull      = RZ_CULL_NONE;
    o.baseColor = kDefaultObjectColor;
    loadPositions(ctx, o, vertices);
    computePolygonColors(o);
    uploadObject(o);

    if (glGetError() != GL_NO_ERROR) {
        freeObject(o);
        return RZ_ERR_GL;
    }
    *outId = id;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzUpdateObjectVertices(RzContext* ctx, int32_t id,
                                                       const uint32_t* vertices) {
    if (!validId(ctx, id) || !vertices) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    Object& o = ctx->objects[id];
    loadPositions(ctx, o, vertices);
    computePolygonColors(o);
    uploadObject(o);
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
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    Object& o = ctx->objects[id];
    o.baseColor = rgb & 0x00FFFFFFu;
    computePolygonColors(o);
    uploadObject(o);
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
