// Rodas dos objetos: quatro pneus com ombros arredondados e, opcionalmente,
// faces texturizadas (rzSetObjectWheelFaces).
//
// rzSetObjectWheels: para cada roda, o vértice do objeto que é o centro do
// cubo (hub), se é dianteira ou traseira e o diâmetro (tiles). Precisam ser
// duas dianteiras e duas traseiras.
// rzUpdateObjectWheels: ângulo de esterçamento das duas dianteiras (radianos;
// positivo vira para a esquerda, anti-horário visto de cima). Traseiras retas.
//
// A malha de cada roda é montada uma vez, no referencial dela (x frente,
// y cima, z eixo, centro no cubo), e só é refeita quando as rodas ou as faces
// mudam. A cada frame a CPU só calcula uma matriz por roda (wheelModel:
// posição do cubo, esterçamento e giro), aplicada no vertex shader (uModel).
//
// Referencial do carro, refeito a cada frame a partir dos quatro cubos:
//   frente f = meio das dianteiras − meio das traseiras;
//   cima   u = perpendicular a f e à linha entre as rodas, apontando para +y;
//   direita r = f x u.
// Cada roda: diâmetro d e largura kWheelWidth x d, eixo = r (nas dianteiras,
// girado em torno de u pelo esterçamento), kWheelSegments lados.
//
// Desenho: o pneu com o programa do terreno no modo sem textura (cor flat
// iluminada, sombras e neblina de graça), as faces com o dos objetos; sem
// culling (cilindro fechado: o depth resolve). Projetam sombra (mapas 1 e 2,
// junto com o objeto). Um glDrawArrays por roda (a matriz muda).

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t kWheelVertices = kWheelSideVerts;   // VBO plano por roda (casca + tampas pretas)

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

Vec3 scale(Vec3 v, float s) { return { v.x * s, v.y * s, v.z * s }; }
Vec3 normalized(Vec3 v) {
    const float l2 = dot(v, v);
    return l2 > 0.0f ? scale(v, 1.0f / sqrtf(l2)) : Vec3{ 0.0f, 0.0f, 0.0f };
}

// Triângulo do pneu com uma normal por vértice (sombreamento liso, não flat).
void emitSmooth(TerrainVertex*& out, Vec3 a, Vec3 b, Vec3 c, Vec3 na, Vec3 nb, Vec3 nc) {
    *out++ = { a.x, a.y, a.z, kWheelColor, 0, 0, 0, 0, packNormal(na) };
    *out++ = { b.x, b.y, b.z, kWheelColor, 0, 0, 0, 0, packNormal(nb) };
    *out++ = { c.x, c.y, c.z, kWheelColor, 0, 0, 0, 0, packNormal(nc) };
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

// Triângulo texturizado qualquer (a parede lateral é um anel, não um leque): as
// UV de cada canto vêm do ponto (cx, cy) no círculo do quadrado (|cx|,|cy| <= 1).
void emitTexTri(GpuVertex*& out, Vec3 pa, Vec3 pb, Vec3 pc, uint32_t normal, const float face[4],
                float ax, float ay, float bx, float by, float cx, float cy, bool mirror) {
    *out++ = capCorner(pa, normal, face, ax, ay, mirror);
    *out++ = capCorner(pb, normal, face, bx, by, mirror);
    *out++ = capCorner(pc, normal, face, cx, cy, mirror);
}

// Referencial do carro a partir dos quatro cubos (ver o topo do arquivo):
// fwd/axis [0] traseira (reta), [1] dianteira (esterçada). Na primeira vez,
// fixa o sentido do "cima" e qual tampa de cada roda é a externa.
void wheelFrame(Object& o, Vec3* up, Vec3 fwd[2], Vec3 axis[2]) {
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
    // O "cima" das rodas: fixa o sentido na 1a vez (carro em pé, u aponta
    // para +y) e depois acompanha o corpo continuamente, SEM reforçar o +y a
    // cada frame. Assim emborcar não inverte o eixo (nem, com ele, a identidade
    // das tampas): u gira junto com o carro em vez de dar um salto.
    Vec3 u = normalized(cross(axle, f));
    if (!o.wheelOuterResolved) o.wheelUpSign = (u.y >= 0.0f) ? 1.0f : -1.0f;
    u = scale(u, o.wheelUpSign);
    if (dot(u, u) == 0.0f) u = { 0.0f, 1.0f, 0.0f };

    const float s = sinf(o.wheelSteer), c = cosf(o.wheelSteer);
    fwd[0] = f;
    fwd[1] = scale(f, c) + scale(cross(u, f), s);
    axis[0] = normalized(cross(fwd[0], u));
    axis[1] = normalized(cross(fwd[1], u));
    *up = u;

    // Decide uma vez (e fixa) qual tampa de cada roda é a externa, pelo eixo
    // RETO (axis[0], o lado direito do carro), não pelo esterçado: assim o
    // esterçamento, por maior que seja, não troca mais interna por externa.
    if (!o.wheelOuterResolved) {
        for (int32_t i = 0; i < 4; ++i)
            o.wheelPlusOuter[i] = dot(hub[i] - carMid, axis[0]) > 0.0f ? 1 : 0;
        o.wheelOuterResolved = true;
        o.wheelsDirty = true;            // a malha depende de qual tampa é a externa
    }
}

// Monta a malha das quatro rodas, cada uma no próprio referencial (x frente,
// y cima, z eixo, centro no cubo, sem giro), e envia os VBOs. Só na carga e
// quando rodas/faces mudam: posição, esterçamento e giro vêm de wheelModel.
// O pneu (a lateral) vai para o VBO plano; as duas tampas vão para o VBO das
// faces com a textura do objeto quando há faces (rzSetObjectWheelFaces), ou
// para o VBO plano, pretas, quando não há.
void buildWheels(Object& o) {
    // anel unitário (fechado: ring[kWheelSegments] = ring[0])
    float ringC[kWheelSegments + 1], ringS[kWheelSegments + 1];
    for (int32_t k = 0; k <= kWheelSegments; ++k) {
        const float a = 2.0f * kPi * float(k % kWheelSegments) / float(kWheelSegments);
        ringC[k] = cosf(a);
        ringS[k] = sinf(a);
    }
    const Vec3 fw = { 1.0f, 0.0f, 0.0f }, u = { 0.0f, 1.0f, 0.0f }, ax = { 0.0f, 0.0f, 1.0f };
    const Vec3 p = { 0.0f, 0.0f, 0.0f };
    const uint32_t nOut = packNormal(ax), nIn = packNormal(scale(ax, -1.0f));

    const bool textured = o.wheelFacesSet;
    TerrainVertex* const flat0 = o.wheelStaging.data();
    GpuVertex*     const cap0  = o.wheelCapStaging.data();
    TerrainVertex* out = flat0;
    GpuVertex*     cap = cap0;
    for (int32_t i = 0; i < 4; ++i) {
        TerrainVertex* const outStart = out;
        GpuVertex*     const capStart = cap;
        const bool plusIsOuter = o.wheelPlusOuter[i] != 0;   // tampa +eixo é a externa?

        // Medidas do pneu (mundo), de fora para dentro no raio: banda de rodagem
        // (reta, raio R) -> ombro arredondado -> parede lateral texturizada (anel
        // de `sideW` de largura) -> recuo da jante (miolo afundado `recess`). A
        // casca (rodagem + ombro + parede do recuo) é de cor fixa; a parede
        // lateral e a jante recuada são texturizadas.
        const float d      = o.wheelDiameter[i];
        const float R      = 0.5f * d;                       // raio da rodagem
        const float w      = 0.5f * kWheelWidth * d;         // meia largura
        const float sh     = 0.12f * R;                      // ombro pequeno: só arredonda a beira
        const float wt     = w - sh;                         // meia largura da rodagem reta
        const float rimR   = R - sh;                         // raio na beira (fim do ombro)
        const float sideW  = 0.35f * R;                      // largura da parede lateral texturizada
        const float sideIn = rimR - sideW;                   // raio onde a parede vira recuo da jante
        const float recess = 0.30f * w;                      // profundidade do recuo da jante
        const float frIn   = sideIn / rimR;                  // fração do raio da face (UV) na beira interna

        // Perfil da casca de cor fixa (a = axial, r = raio, n = normal no plano
        // radial/axial): ombro (-), rodagem reta, ombro (+). Termina em ±w (beira);
        // a face texturizada fecha dali para dentro.
        constexpr int32_t kMaxSamples = 2 * kWheelShoulderSteps + 4;
        float pa[kMaxSamples], pr[kMaxSamples], pnr[kMaxSamples], pna[kMaxSamples];
        int32_t ns = 0;
        auto push = [&](float a, float r, float nr, float na) {
            pa[ns] = a; pr[ns] = r; pnr[ns] = nr; pna[ns] = na; ++ns;
        };
        for (int32_t s = 0; s <= kWheelShoulderSteps; ++s) { // ombro (-): θ de π/2 (beira) a 0
            const float th = 0.5f * kPi * float(kWheelShoulderSteps - s) / float(kWheelShoulderSteps);
            push(-(wt + sh * sinf(th)), rimR + sh * cosf(th), cosf(th), -sinf(th));
        }
        push(wt, R, 1.0f, 0.0f);                             // rodagem reta (outra beira)
        for (int32_t s = 1; s <= kWheelShoulderSteps; ++s) { // ombro (+): θ de 0 a π/2 (beira)
            const float th = 0.5f * kPi * float(s) / float(kWheelShoulderSteps);
            push(wt + sh * sinf(th), rimR + sh * cosf(th), cosf(th), sinf(th));
        }

        // A face externa usa o quadrado externo (sem espelho), a interna o
        // interno (espelhado em u). A UV fica presa à malha, então o desenho da
        // face gira junto com a roda (o giro está em wheelModel).
        const float* fPlus  = plusIsOuter ? o.wheelFaceOuter : o.wheelFaceInner;   // tampa +eixo
        const float* fMinus = plusIsOuter ? o.wheelFaceInner : o.wheelFaceOuter;   // tampa -eixo
        const bool mPlus = !plusIsOuter, mMinus = plusIsOuter;

        for (int32_t k = 0; k < kWheelSegments; ++k) {
            const Vec3 rad0 = scale(fw, ringC[k]) + scale(u, ringS[k]);           // direção radial (unitária)
            const Vec3 rad1 = scale(fw, ringC[k + 1]) + scale(u, ringS[k + 1]);
            // Casca de cor fixa: uma banda por par de pontos do perfil, com normais
            // por vértice (ombros arredondados com luz lisa).
            for (int32_t j = 0; j + 1 < ns; ++j) {
                const Vec3 axA = scale(ax, pa[j]),     axB = scale(ax, pa[j + 1]);
                const Vec3 A0 = p + axA + scale(rad0, pr[j]),     A1 = p + axA + scale(rad1, pr[j]);
                const Vec3 B0 = p + axB + scale(rad0, pr[j + 1]), B1 = p + axB + scale(rad1, pr[j + 1]);
                const Vec3 nA0 = scale(rad0, pnr[j]) + scale(ax, pna[j]);
                const Vec3 nA1 = scale(rad1, pnr[j]) + scale(ax, pna[j]);
                const Vec3 nB0 = scale(rad0, pnr[j + 1]) + scale(ax, pna[j + 1]);
                const Vec3 nB1 = scale(rad1, pnr[j + 1]) + scale(ax, pna[j + 1]);
                emitSmooth(out, A0, B0, B1, nA0, nB0, nB1);
                emitSmooth(out, A0, B1, A1, nA0, nB1, nA1);
            }
            if (textured) {
                // As duas faces: parede lateral (anel texturizado, no plano ±w) e
                // jante recuada (leque texturizado, afundado em ±(w-recess)); entre
                // elas, a parede do recuo, de cor fixa (raio sideIn).
                for (int32_t side = 0; side < 2; ++side) {
                    const float sgn = side == 0 ? 1.0f : -1.0f;            // + eixo, depois - eixo
                    const uint32_t nf = side == 0 ? nOut : nIn;
                    const float* face = side == 0 ? fPlus : fMinus;
                    const bool mir = side == 0 ? mPlus : mMinus;
                    const Vec3 cFront = p + scale(ax, sgn * w);            // plano da parede lateral
                    const Vec3 cBack  = p + scale(ax, sgn * (w - recess)); // plano da jante recuada
                    // parede lateral: anel rimR -> sideIn, no plano da beira
                    const Vec3 lo0 = cFront + scale(rad0, rimR),  lo1 = cFront + scale(rad1, rimR);
                    const Vec3 li0 = cFront + scale(rad0, sideIn), li1 = cFront + scale(rad1, sideIn);
                    emitTexTri(cap, lo0, lo1, li1, nf, face,
                               ringC[k], ringS[k], ringC[k + 1], ringS[k + 1],
                               frIn * ringC[k + 1], frIn * ringS[k + 1], mir);
                    emitTexTri(cap, lo0, li1, li0, nf, face,
                               ringC[k], ringS[k], frIn * ringC[k + 1], frIn * ringS[k + 1],
                               frIn * ringC[k], frIn * ringS[k], mir);
                    // parede do recuo (cor fixa): cilindro em sideIn, da beira ao fundo
                    const Vec3 bi0 = cBack + scale(rad0, sideIn), bi1 = cBack + scale(rad1, sideIn);
                    const Vec3 wn0 = scale(rad0, -1.0f), wn1 = scale(rad1, -1.0f);   // normal p/ dentro
                    emitSmooth(out, li0, bi0, bi1, wn0, wn0, wn1);
                    emitSmooth(out, li0, bi1, li1, wn0, wn1, wn1);
                    // jante recuada: leque texturizado do centro à beira interna
                    emitCap(cap, cBack, bi0, bi1, nf, face,
                            frIn * ringC[k], frIn * ringS[k], frIn * ringC[k + 1], frIn * ringS[k + 1], mir);
                }
            } else {
                // Sem textura: tampas pretas planas, raio rimR, no plano da beira.
                const Vec3 cP = p + scale(ax, w), cM = p - scale(ax, w);
                const Vec3 oP0 = cP + scale(rad0, rimR), oP1 = cP + scale(rad1, rimR);
                const Vec3 iM0 = cM + scale(rad0, rimR), iM1 = cM + scale(rad1, rimR);
                emitSmooth(out, cP, oP1, oP0, ax, ax, ax);
                const Vec3 axn = scale(ax, -1.0f);
                emitSmooth(out, cM, iM0, iM1, axn, axn, axn);
            }
        }
        o.wheelFlatFirst[i] = int32_t(outStart - flat0);
        o.wheelFlatCount[i] = int32_t(out - outStart);
        o.wheelCapFirst[i]  = int32_t(capStart - cap0);
        o.wheelCapCount[i]  = int32_t(cap - capStart);
    }
    o.wheelFlatVerts = int32_t(out - flat0);
    o.wheelCapVerts  = int32_t(cap - cap0);

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
// pelo seu diâmetro; positivo = para a frente). Refaz a malha local só se
// rodas/faces mudaram; todo frame, só as quatro matrizes (wheelModel).
void prepareWheels(RzContext* ctx, Object& o) {
    if (o.wheelStaging.empty() || !o.positioned) return;
    if (o.wheelSpeed != 0.0f) {
        const float dist = o.wheelSpeed * ctx->cellSize;       // tiles/frame -> mundo
        for (int32_t i = 0; i < 4; ++i) {
            o.wheelRoll[i] -= dist / (0.5f * o.wheelDiameter[i]);   // -: topo vai para a frente
            o.wheelRoll[i] = fmodf(o.wheelRoll[i], 2.0f * kPi);
        }
    }
    Vec3 u, fwd[2], axis[2];
    wheelFrame(o, &u, fwd, axis);
    if (o.wheelsDirty) {
        buildWheels(o);
        o.wheelsDirty = false;
    }
    // Colunas da matriz: x (frente) e y (cima) girados pelo giro da roda em
    // volta do eixo z; z = eixo; translação = cubo
    for (int32_t i = 0; i < 4; ++i) {
        const int32_t which = o.wheelFront[i];
        const Vec3 f = fwd[which], ax = axis[which], hub = o.world[o.wheelVertex[i]];
        const float cr = cosf(o.wheelRoll[i]), sr = sinf(o.wheelRoll[i]);
        const Vec3 x = scale(f, cr) + scale(u, sr);
        const Vec3 y = scale(f, -sr) + scale(u, cr);
        Mat4& m = o.wheelModel[i];
        m = Mat4::identity();
        m[0, 0] = x.x; m[0, 1] = y.x; m[0, 2] = ax.x; m[0, 3] = hub.x;
        m[1, 0] = x.y; m[1, 1] = y.y; m[1, 2] = ax.y; m[1, 3] = hub.y;
        m[2, 0] = x.z; m[2, 1] = y.z; m[2, 2] = ax.z; m[2, 3] = hub.z;
    }
}

// Passe de sombra: o programa de profundidade já está ligado. A posição está no
// atributo 0 nos dois VBOs (pneu e faces), então os dois projetam sombra.
void drawWheelsDepth(const RzContext* ctx, const Object& o) {
    if (o.wheelStaging.empty() || !o.positioned) return;
    const GLint model = ctx->depthProgram.model;
    for (int32_t i = 0; i < 4; ++i) {
        glUniformMatrix4fv(model, 1, GL_TRUE, o.wheelModel[i].e);
        glBindVertexArray(o.wheelVao);
        glDrawArrays(GL_TRIANGLES, o.wheelFlatFirst[i], o.wheelFlatCount[i]);
        if (o.wheelCapCount[i] > 0) {
            glBindVertexArray(o.wheelCapVao);
            glDrawArrays(GL_TRIANGLES, o.wheelCapFirst[i], o.wheelCapCount[i]);
        }
    }
    glUniformMatrix4fv(model, 1, GL_TRUE, Mat4::identity().e);
}

// Passe principal, depois dos objetos: o pneu (lateral), programa do terreno
// sem textura. As faces vão em drawWheelFaces. Sem culling (o depth resolve).
void drawWheels(RzContext* ctx, const Mat4& viewProj) {
    bool bound = false;
    const TerrainProgram& t = ctx->terrainProgram;
    for (const Object& o : ctx->objects) {
        if (!o.alive || !o.positioned || o.wheelStaging.empty() || !objectInRange(ctx, o)) continue;
        if (!bound) {
            glUseProgram(t.program);
            glUniformMatrix4fv(t.viewProj, 1, GL_TRUE, viewProj.e);
            bindShadowMaps(ctx, t.shadow);
            bindFog(ctx, t.fog);
            glUniform1i(t.textured, 0);
            glUniform1i(t.smooth, 1);            // pneu: luz por pixel (normais lisas)
            glDisable(GL_CULL_FACE);
            bound = true;
        }
        glBindVertexArray(o.wheelVao);
        for (int32_t i = 0; i < 4; ++i) {
            glUniformMatrix4fv(t.model, 1, GL_TRUE, o.wheelModel[i].e);
            glDrawArrays(GL_TRIANGLES, o.wheelFlatFirst[i], o.wheelFlatCount[i]);
        }
    }
    if (bound) {                                 // restaura o terreno: flat, sem modelo
        glUniform1i(t.smooth, 0);
        glUniformMatrix4fv(t.model, 1, GL_TRUE, Mat4::identity().e);
    }
}

// As faces (discos) das rodas: programa dos objetos, com a textura do objeto,
// depois de drawWheels. Sem culling, como o pneu.
void drawWheelFaces(RzContext* ctx, const Mat4& viewProj) {
    bool bound = false;
    const ObjectProgram& prog = ctx->objectProgram;
    for (const Object& o : ctx->objects) {
        if (!o.alive || !o.positioned || o.wheelCapVerts <= 0 || !objectInRange(ctx, o)) continue;
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
        for (int32_t i = 0; i < 4; ++i) {
            glUniformMatrix4fv(prog.model, 1, GL_TRUE, o.wheelModel[i].e);
            glDrawArrays(GL_TRIANGLES, o.wheelCapFirst[i], o.wheelCapCount[i]);
        }
    }
    if (bound) glUniformMatrix4fv(prog.model, 1, GL_TRUE, Mat4::identity().e);
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
    o.wheelSteer = steer;                // só muda a matriz (prepareWheels)
    return RZ_OK;
}

} // extern "C"
