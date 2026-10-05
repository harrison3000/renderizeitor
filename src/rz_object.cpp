// Objetos: malhas de polígonos convexos em coordenadas absolutas do mundo.
//
// Carga: rzCreateObject cria o objeto vazio (vértices, VAO e VBO);
// rzAddObjectPolygon / rzAddObjectTexturedPolygon validam e copiam um
// polígono (e seus UVs ou o índice de paleta), triangulam em leque, crescem os
// buffers da CPU e realocam o VBO. A textura do objeto fica em rz_texture.cpp.
//
// Todo polígono é texturizado. O de cor sólida (rzAddObjectPolygon) aponta os
// três cantos para o centro do bloquinho da sua cor na faixa de amostras da
// paleta, no pé da textura (kSwatch*, rz_texture.cpp): UV constante, derivada
// zero, sempre o nível 0 do mipmap. Como a posição do bloco depende do lado
// da textura, esses UVs são calculados no envio ao VBO.
// Update (rzUpdateObjectVertices): converte as posições 8.24 para float e
// recalcula a cor sombreada de cada polígono.
// Frame (drawObjects): reenvia o VBO dos objetos alterados (sem alocar) e faz
// um glDrawArrays por objeto visível.

#include <cmath>

#include "rz_internal.h"

using namespace rz;

namespace {

constexpr int32_t  kMaxObjectVertices = 65536;              // índice uint16
constexpr float    kFixed824ToFloat = 1.0f / 16777216.0f;   // 2^-24

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

// Libera os recursos de GPU e esvazia o slot (fica livre para reuso).
void freeObject(Object& o) {
    if (o.texture) glDeleteTextures(1, &o.texture);
    if (o.vbo) glDeleteBuffers(1, &o.vbo);
    if (o.vao) glDeleteVertexArrays(1, &o.vao);
    freeGlass(o);
    freeWheels(o);
    o = Object{};
}

// 8.24 -> mundo. O legado tem z para cima, (coluna, linha, altura); o renderer
// tem y para cima: (x, y, z) = (coluna, altura, linha).
void loadPositions(const RzContext* ctx, Object& o, const RzVertex* vertices) {
    const float scale = ctx->cellSize * kFixed824ToFloat;
    for (int32_t i = 0; i < o.vertexCount(); ++i) {
        const float col = float(vertices[i].x) * scale;
        const float row = float(vertices[i].y) * scale;
        const float up  = float(vertices[i].z) * scale;
        o.world[i] = Vec3{ col, up, row };
    }

    // Esfera envolvente (centro da caixa dos vértices, raio máximo): corte por
    // distância (neblina) e caixa do mapa de sombra do alvo
    Vec3 lo = o.world[0], hi = o.world[0];
    for (const Vec3& v : o.world) {
        lo = { fminf(lo.x, v.x), fminf(lo.y, v.y), fminf(lo.z, v.z) };
        hi = { fmaxf(hi.x, v.x), fmaxf(hi.y, v.y), fmaxf(hi.z, v.z) };
    }
    o.center = { (lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f };
    float r2 = 0.0f;
    for (const Vec3& v : o.world) {
        const Vec3 d = v - o.center;
        r2 = fmaxf(r2, dot(d, d));
    }
    o.radius = sqrtf(r2);
}

// Luz flat do polígono, de um lado só: só a luz, em cinza, que o shader
// multiplica pela textura. No sentido do legado (horário visto de fora), a
// normal de Newell aponta para dentro; a de fora é a oposta. Face de costas
// para a luz fica só com o ambiente.
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
    return shadeFlat(0x00FFFFFFu, Vec3{ -normal.x, -normal.y, -normal.z }, false);
}

void computePolygonColors(Object& o) {
    for (size_t p = 0; p < o.polygonStart.size(); ++p) o.polygonColors[p] = polygonColor(o, p);
}

// Linhas (rzAddObjectLine): prisma de seção quadrada (lado = grossura) em
// volta do segmento a-b, 4 lados + 2 tampas, cor sólida da paleta (UV do
// bloquinho) com a luz flat de cada face. A seção é orientada pelo "cima" do
// mundo (não gira junto com o objeto em volta da linha; é fina, não aparece).
// Faces no sentido do legado (horário visto de fora), para o culling.
void emitLines(const Object& o, int32_t side, GpuVertex*& v) {
    for (const ObjectLine& l : o.lines) {
        const Vec3 pa = o.world[l.a], pb = o.world[l.b];
        float su, sv;
        swatchUV(l.palette, side, &su, &sv);
        Vec3 d = pb - pa;
        const float len2 = dot(d, d);
        if (!(len2 > 0.0f)) {                                  // ponta com ponta: nada visível
            for (int32_t k = 0; k < kLineVertices; ++k) *v++ = { pa.x, pa.y, pa.z, 0, su, sv };
            continue;
        }
        const float invLen = 1.0f / sqrtf(len2);
        d = { d.x * invLen, d.y * invLen, d.z * invLen };
        const Vec3 ref = fabsf(d.y) < 0.9f ? Vec3{ 0.0f, 1.0f, 0.0f } : Vec3{ 1.0f, 0.0f, 0.0f };
        Vec3 u = cross(d, ref);
        const float invU = 1.0f / sqrtf(dot(u, u));
        u = { u.x * invU, u.y * invU, u.z * invU };
        const Vec3 w = cross(d, u);
        auto off = [&](float su_, float sw_) {
            return Vec3{ (u.x * su_ + w.x * sw_) * l.half, (u.y * su_ + w.y * sw_) * l.half, (u.z * su_ + w.z * sw_) * l.half };
        };
        const Vec3 o4[4] = { off(1, 1), off(-1, 1), off(-1, -1), off(1, -1) };
        Vec3 ca[4], cb[4];
        for (int32_t k = 0; k < 4; ++k) { ca[k] = pa + o4[k]; cb[k] = pb + o4[k]; }
        // quad p0..p3 com normal de fora n: ordem horária vista de fora
        auto face = [&](Vec3 p0, Vec3 p1, Vec3 p2, Vec3 p3, Vec3 n) {
            if (dot(cross(p1 - p0, p2 - p0), n) > 0.0f) { const Vec3 t = p1; p1 = p3; p3 = t; }
            const uint32_t color = shadeFlat(0x00FFFFFFu, n, false);
            const Vec3 q[6] = { p0, p1, p2, p0, p2, p3 };
            for (const Vec3& p : q) *v++ = { p.x, p.y, p.z, color, su, sv };
        };
        for (int32_t k = 0; k < 4; ++k) {
            const int32_t k1 = (k + 1) & 3;
            face(ca[k], ca[k1], cb[k1], cb[k], o4[k] + o4[k1]);      // lado (normal pelo meio da aresta)
        }
        face(ca[0], ca[1], ca[2], ca[3], Vec3{ -d.x, -d.y, -d.z });  // tampas
        face(cb[0], cb[1], cb[2], cb[3], d);
    }
}

// Tamanho do staging/VBO: polígonos + linhas (fase de carga: realoca o VBO;
// o conteúdo vai no próximo frame)
void resizeObjectBuffers(Object& o) {
    o.staging.resize(o.triangles.size() * 3 + o.lines.size() * size_t(kLineVertices));
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(o.staging.size() * sizeof(GpuVertex)),
                 nullptr, GL_DYNAMIC_DRAW);
    o.gpuDirty = true;
}

// Monta os vértices dos triângulos (e das linhas) e reenvia o vertex buffer
// inteiro (que já tem o tamanho certo: é realocado em resizeObjectBuffers).
// side: lado da textura em uso, para o UV das cores sólidas.
void uploadObject(Object& o, int32_t side) {
    GpuVertex* v = o.staging.data();
    for (const ObjectTriangle& tri : o.triangles) {
        const uint32_t color = o.polygonColors[tri.polygon];
        const int32_t  pal   = o.polygonPalette[tri.polygon];
        float su = 0.0f, sv = 0.0f;
        if (pal >= 0) swatchUV(pal, side, &su, &sv);
        const int32_t corners[3] = { tri.a, tri.b, tri.c };
        for (int32_t corner : corners) {
            const Vec3 p = o.world[o.indices[corner]];
            if (pal >= 0) *v++ = { p.x, p.y, p.z, color, su, sv };
            else          *v++ = { p.x, p.y, p.z, color, o.uvs[corner * 2], o.uvs[corner * 2 + 1] };
        }
    }
    emitLines(o, side, v);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, GLsizeiptr(o.staging.size() * sizeof(GpuVertex)),
                    o.staging.data());
}

// Corpo comum de rzAddObjectPolygon (indices, cor = paletteIndex) e
// rzAddObjectTexturedPolygon (corners com índice + UV, paletteIndex = -1).
// Exatamente um de indices/corners não é nulo.
int32_t addPolygon(RzContext* ctx, int32_t id, const uint16_t* indices, const RzTexVertex* corners,
                   int32_t count, int32_t paletteIndex) {
    if (!validId(ctx, id) || (!indices && !corners) || count < 0) return RZ_ERR_INVALID_ARG;
    if (!corners && (paletteIndex < 0 || paletteIndex > 255)) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];
    auto indexAt = [&](int32_t i) { return corners ? corners[i].index : indices[i]; };

    int32_t n = count;
    if (n > 1 && indexAt(n - 1) == indexAt(0)) --n;          // fechamento do legado
    for (int32_t i = 0; i < n; ++i) {
        if (indexAt(i) >= o.vertexCount()) return RZ_ERR_INVALID_ARG;
        if (corners && !(std::isfinite(corners[i].u) && std::isfinite(corners[i].v))) {
            return RZ_ERR_INVALID_ARG;
        }
    }
    if (n < 3) return RZ_OK;                                  // degenerado: ignorado
    if (o.polygonStart.size() >= 65535) return RZ_ERR_SIZE;  // polígono do triângulo é uint16
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    // Cópia dos índices e UVs, e leque (fase de carga: os vetores crescem aqui)
    const uint16_t polygon = uint16_t(o.polygonStart.size());
    const int32_t  first   = int32_t(o.indices.size());
    o.polygonStart.push_back(first);
    o.polygonLength.push_back(n);
    o.polygonPalette.push_back(int16_t(corners ? -1 : paletteIndex));
    for (int32_t i = 0; i < n; ++i) {
        o.indices.push_back(indexAt(i));
        o.uvs.push_back(corners ? corners[i].u : 0.0f);
        o.uvs.push_back(corners ? corners[i].v : 0.0f);
    }
    for (int32_t i = 1; i + 1 < n; ++i) {
        o.triangles.push_back({ first, first + i, first + i + 1, polygon });
    }
    o.polygonColors.push_back(polygonColor(o, polygon));
    resizeObjectBuffers(o);
    return glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL;
}

} // namespace

namespace rz {

static bool drawable(const Object& o) {
    return o.alive && o.positioned && !o.staging.empty();
}

// Fora do alcance: a esfera inteira além da neblina (+ margem para sombras
// longas). Sem neblina (visão geral), tudo está no alcance.
bool objectInRange(const RzContext* ctx, const Object& o) {
    if (!ctx->fogOn) return true;
    const Vec3 d = o.center - ctx->eyePos;
    const float reach = (ctx->fogEnd + kFogCullMargin) * ctx->cellSize + o.radius;
    return dot(d, d) <= reach * reach;
}

// Reenvia os VBOs alterados, antes dos dois passes do frame (sem alocar).
// Os fora do alcance ficam para quando entrarem (gpuDirty continua).
void prepareObjects(RzContext* ctx) {
    for (Object& o : ctx->objects) {
        if (o.alive && objectInRange(ctx, o)) prepareWheels(o);
        if (!drawable(o) || !objectInRange(ctx, o)) continue;
        const int32_t side = o.texture ? o.textureSize : ctx->fallbackSize;
        if (o.gpuDirty || o.uploadedSide != side) {   // textura trocada: UVs das cores mudam
            uploadObject(o, side);
            o.gpuDirty = false;
            o.uploadedSide = side;
        }
    }
}

// Passe da sombra: só a geometria (o programa de profundidade já está ligado).
// onlyId >= 0: só esse objeto; skipId >= 0: todos menos ele.
// Os além da neblina ficam de fora; os que só saem da caixa do mapa são
// cortados pelo clipping.
void drawObjectsDepth(RzContext* ctx, int32_t onlyId, int32_t skipId) {
    for (int32_t id = 0; id < int32_t(ctx->objects.size()); ++id) {
        const Object& o = ctx->objects[id];
        if (!drawable(o) || (onlyId >= 0 && id != onlyId) || id == skipId) continue;
        if (!objectInRange(ctx, o)) continue;
        glBindVertexArray(o.vao);
        glDrawArrays(GL_TRIANGLES, 0, o.drawVertexCount());
        drawWheelsDepth(o);
    }
}

// Culling fixo, na convenção do legado: descarta as faces em sentido
// anti-horário na tela (glFrontFace define frente = anti-horário visual, então
// é GL_FRONT que sai).
void drawObjects(RzContext* ctx, const Mat4& viewProj) {
    bool programBound = false;
    for (Object& o : ctx->objects) {
        if (!drawable(o) || !objectInRange(ctx, o)) continue;
        const ObjectProgram& prog = ctx->objectProgram;
        if (!programBound) {
            glEnable(GL_CULL_FACE);
            glCullFace(GL_FRONT);
            glUseProgram(prog.program);
            glUniformMatrix4fv(prog.viewProj, 1, GL_TRUE, viewProj.e);
            bindShadowMaps(ctx, prog.shadow);           // deixa a unidade 0 ativa
            bindFog(ctx, prog.fog);
            programBound = true;
        }
        // Textura própria ou fallback
        const GLuint  texture = o.texture ? o.texture : ctx->fallbackTex;
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindVertexArray(o.vao);
        glDrawArrays(GL_TRIANGLES, 0, o.drawVertexCount());
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

    // Vertex buffer (vazio até o primeiro polígono): posição (vec3), cor (4 bytes
    // normalizados) e UV (vec2)
    glGenVertexArrays(1, &o.vao);
    glGenBuffers(1, &o.vbo);
    glBindVertexArray(o.vao);
    glBindBuffer(GL_ARRAY_BUFFER, o.vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GpuVertex), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, color)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(GpuVertex),
                          reinterpret_cast<void*>(offsetof(GpuVertex, u)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    o.alive      = true;
    o.positioned = false;
    o.gpuDirty   = false;

    if (glGetError() != GL_NO_ERROR) {
        freeObject(o);
        return RZ_ERR_GL;
    }
    *outId = id;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzAddObjectPolygon(RzContext* ctx, int32_t id,
                                                   const uint16_t* indices, int32_t count,
                                                   int32_t paletteIndex) {
    return addPolygon(ctx, id, indices, nullptr, count, paletteIndex);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzAddObjectTexturedPolygon(RzContext* ctx, int32_t id,
                                                           const RzTexVertex* corners, int32_t count) {
    if (!corners) return RZ_ERR_INVALID_ARG;
    return addPolygon(ctx, id, nullptr, corners, count, -1);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzAddObjectLine(RzContext* ctx, int32_t id, uint16_t a, uint16_t b,
                                                float thickness, int32_t paletteIndex) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];
    if (a >= o.vertexCount() || b >= o.vertexCount()) return RZ_ERR_INVALID_ARG;
    if (!(thickness > 0.0f && thickness < 1.0e6f)) return RZ_ERR_INVALID_ARG;   // pega NaN
    if (paletteIndex < 0 || paletteIndex > 255) return RZ_ERR_INVALID_ARG;
    if (a == b) return RZ_OK;                                  // degenerada: ignorada
    if (o.lines.size() >= 65535) return RZ_ERR_SIZE;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    o.lines.push_back({ a, b, int16_t(paletteIndex), 0.5f * thickness * ctx->cellSize });
    resizeObjectBuffers(o);
    return glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzUpdateObjectVertices(RzContext* ctx, int32_t id,
                                                       const RzVertex* vertices) {
    if (!validId(ctx, id) || !vertices) return RZ_ERR_INVALID_ARG;
    Object& o = ctx->objects[id];
    loadPositions(ctx, o, vertices);
    computePolygonColors(o);
    o.positioned = true;
    o.gpuDirty   = true;
    o.glassDirty = !o.glassStaging.empty();
    o.wheelsDirty = !o.wheelStaging.empty();
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzDestroyObject(RzContext* ctx, int32_t id) {
    if (!validId(ctx, id)) return RZ_ERR_INVALID_ARG;
    platformMakeCurrent(ctx->platform);
    freeObject(ctx->objects[id]);
    return RZ_OK;
}

} // extern "C"
