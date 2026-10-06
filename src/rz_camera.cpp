// Câmera: perseguição de um vértice-alvo "na corda". Sem alvo, visão geral
// fixa do terreno inteiro (fallback para testes e base de um futuro modo de
// câmera livre). Roda na CPU uma vez por frame e produz a view-projection.

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

// Altura do terreno (mundo) no ponto (x, z) do mundo, bilinear; 0 sem terreno.
// Com a continuação montada, vale também fora do mapa (câmera atrás do alvo
// perto da borda).
float groundHeight(const RzContext* ctx, float x, float z) {
    if (!ctx->hasTerrain()) return 0.0f;
    if (!ctx->extHeights.empty()) return extendedGroundHeight(ctx, x, z);
    const float invCell = 1.0f / ctx->cellSize;      // uma divisão por frame
    float gx = x * invCell, gz = z * invCell;
    if (gx < 0.0f) gx = 0.0f;
    if (gz < 0.0f) gz = 0.0f;
    if (gx > 254.999f) gx = 254.999f;
    if (gz > 254.999f) gz = 254.999f;
    const int32_t ix = int32_t(gx), iz = int32_t(gz);
    const float fx = gx - float(ix), fz = gz - float(iz);
    const uint8_t* h = ctx->heights.data() + iz * kGridSize + ix;
    const float top = float(h[0]) + (float(h[1]) - float(h[0])) * fx;
    const float bot = float(h[kGridSize]) + (float(h[kGridSize + 1]) - float(h[kGridSize])) * fx;
    return (top + (bot - top) * fz) * ctx->heightScale;
}

// Câmera "na corda": avança o estado da câmera de perseguição um frame.
//   - horizontal: se o alvo passa do comprimento da corda, a câmera é puxada
//     até a distância da corda (fica para trás, na direção de onde veio); se
//     chega perto demais, recua. Entre os dois, a corda está frouxa e a câmera
//     não se mexe. O movimento até a posição desejada é amortecido.
//   - vertical: busca ficar `followHeight` acima do alvo, também amortecida, e
//     nunca abaixo do chão.
void updateFollowCamera(RzContext* ctx, Vec3 target) {
    const float cs = ctx->cellSize;
    const float rope = ctx->followDistance * cs;
    const float minRope = 0.5f * rope;
    const float k = ctx->followStiffness;

    if (!ctx->followInitialized) {
        // Começa no lado +z do alvo (o mesmo da visão geral), já na posição
        // de repouso; quando o alvo andar, a corda leva a câmera para trás dele.
        ctx->followEye = { target.x, target.y + ctx->followHeight * cs, target.z + rope };
        ctx->followInitialized = true;
        ctx->followAppliedDistance = ctx->followDistance;
    }

    Vec3 eye = ctx->followEye;

    // Corda mudou de comprimento (zoom do host): a distância horizontal atual
    // muda na mesma proporção, na hora. Sem isso, a câmera só reagiria quando
    // o alvo saísse da faixa frouxa da corda.
    if (ctx->followAppliedDistance != ctx->followDistance) {
        const float scale = ctx->followDistance / ctx->followAppliedDistance;
        eye.x = target.x + (eye.x - target.x) * scale;
        eye.z = target.z + (eye.z - target.z) * scale;
        ctx->followAppliedDistance = ctx->followDistance;
    }

    const float dx = eye.x - target.x;
    const float dz = eye.z - target.z;
    const float dist = sqrtf(dx * dx + dz * dz);

    float wantX = eye.x, wantZ = eye.z;
    if (dist > rope || dist < minRope) {
        const float len = (dist > rope) ? rope : minRope;
        if (dist > 1e-4f) {
            const float scale = len / dist;
            wantX = target.x + dx * scale;
            wantZ = target.z + dz * scale;
        } else {
            wantX = target.x + len;                    // em cima do alvo: sai para +x
            wantZ = target.z;
        }
    }
    eye.x += (wantX - eye.x) * k;
    eye.z += (wantZ - eye.z) * k;

    // Altura: a desejada nunca fica a menos de kFollowClearance do chão. Subir
    // é amortecido mais rápido que descer, para não entrar no morro; o clamp
    // duro (kFollowMinClearance) é só uma rede de segurança.
    const float ground = groundHeight(ctx, eye.x, eye.z);
    float wantY = target.y + ctx->followHeight * cs;
    if (wantY < ground + kFollowClearance * cs) wantY = ground + kFollowClearance * cs;
    const float kY = (wantY > eye.y && k < kFollowClimb) ? kFollowClimb : k;
    eye.y += (wantY - eye.y) * kY;
    if (eye.y < ground + kFollowMinClearance * cs) eye.y = ground + kFollowMinClearance * cs;

    ctx->followEye = eye;
}

Vec3 lerp3(Vec3 a, Vec3 b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
}

// Posição de repouso atrás do alvo, na direção do voo (o alvo fica à frente).
Vec3 restEye(const RzContext* ctx, Vec3 target, Vec3 dir) {
    const float cs = ctx->cellSize;
    const float rope = ctx->followDistance * cs;
    return { target.x - dir.x * rope, target.y + ctx->followHeight * cs, target.z - dir.z * rope };
}

// Troca de alvo: monta a transição a partir do olho e do ponto olhado atuais
// (também no meio de outra transição).
void startRetarget(RzContext* ctx, Vec3 target) {
    const float cs = ctx->cellSize;
    ctx->retargetPending = false;
    ctx->retargetActive = true;
    ctx->retargetT = 0.0f;
    ctx->retargetEye0 = ctx->followEye;
    ctx->retargetLook0 = ctx->followLook;

    const float dx = target.x - ctx->followLook.x;
    const float dz = target.z - ctx->followLook.z;
    const float d = sqrtf(dx * dx + dz * dz);
    if (d <= kRetargetTurnDistance * cs) {
        ctx->retargetArc = false;
        ctx->retargetStep = 1.0f / kRetargetTurnFrames;
        return;
    }
    ctx->retargetArc = true;
    ctx->retargetDir = { dx / d, 0.0f, dz / d };
    float lift = d / kRetargetArcFull;
    if (lift > 1.0f) lift = 1.0f;
    ctx->retargetLift = lift * kRetargetArcHeight * cs;
    // smoothstep tem pico de velocidade 1,5x a média; o arco soma ~2x a subida
    const Vec3 move = restEye(ctx, target, ctx->retargetDir) - ctx->followEye;
    const float path = sqrtf(dot(move, move)) + 2.0f * ctx->retargetLift;
    float frames = 1.5f * path / (kRetargetMaxSpeed * cs);
    if (frames < kRetargetMinFrames) frames = kRetargetMinFrames;
    ctx->retargetStep = 1.0f / frames;
}

// Um frame da transição. O destino acompanha o alvo (ele pode estar andando).
void updateRetarget(RzContext* ctx, Vec3 target) {
    float t = ctx->retargetT + ctx->retargetStep;
    if (t > 1.0f) t = 1.0f;
    ctx->retargetT = t;
    const float e = t * t * (3.0f - 2.0f * t);                  // smoothstep

    if (!ctx->retargetArc) {
        ctx->followLook = lerp3(ctx->retargetLook0, target, e);
        updateFollowCamera(ctx, ctx->followLook);               // a corda traz a câmera
    } else {
        // O olhar vai na frente do olho (chega no alvo com 2/3 do voo): no alto
        // do arco a câmera olha adiante e para baixo, não para o próprio pé
        float tl = t * 1.5f;
        if (tl > 1.0f) tl = 1.0f;
        ctx->followLook = lerp3(ctx->retargetLook0, target, tl * tl * (3.0f - 2.0f * tl));
        const float cs = ctx->cellSize;
        Vec3 eye = lerp3(ctx->retargetEye0, restEye(ctx, target, ctx->retargetDir), e);
        eye.y += ctx->retargetLift * sinf(kPi * e);
        const float floorY = groundHeight(ctx, eye.x, eye.z) + kFollowClearance * cs;
        if (eye.y < floorY) eye.y = floorY;
        ctx->followEye = eye;
    }
    if (t >= 1.0f) ctx->retargetActive = false;                 // segue na corda normal
}

float smooth01(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

// Avança a transição de/para a visão geral um frame; devolve o progresso
// suavizado e o cru (t) em *rawT.
float stepOverview(RzContext* ctx, float* rawT) {
    float t = ctx->overviewT + 1.0f / kOverviewFrames;
    if (t >= 1.0f) {
        t = 1.0f;
        ctx->overviewActive = false;
    }
    ctx->overviewT = t;
    *rawT = t;
    return smooth01(t);
}

// Projeção perspectiva no clip space do OpenGL (−w ≤ z ≤ w), câmera olhando
// para −z. flipY espelha a imagem na vertical (modo offscreen: o glReadPixels
// lê de baixo para cima e o buffer do host é top-down).
Mat4 perspective(float focalX, float focalY, float nearPlane, float farPlane, bool flipY) {
    const float invRange = 1.0f / (nearPlane - farPlane);
    Mat4 p{};
    p[0, 0] = focalX;
    p[1, 1] = flipY ? -focalY : focalY;
    p[2, 2] = (farPlane + nearPlane) * invRange;
    p[2, 3] = 2.0f * nearPlane * farPlane * invRange;
    p[3, 2] = -1.0f;
    return p;
}

// Visão geral: olha para o centro do terreno do lado +z, com inclinação
// fixa, na distância em que a esfera envolvente (raio R) cabe no FOV.
void overviewCamera(const RzContext* ctx, Vec3 center, Vec3* eye) {
    constexpr float degToRad = kPi / 180.0f;
    const float pitch = kOverviewPitchDegrees * degToRad;
    // focal = 1 / tan(meio FOV), no eixo de FOV menor: a esfera fica quase
    // tangente às bordas e os cantos da caixa (mais baixa que a esfera) cabem
    const float focal = ctx->focalX > ctx->focalY ? ctx->focalX : ctx->focalY;
    const float d = ctx->terrainRadius * focal;
    *eye = center + Vec3{ 0.0f, sinf(pitch) * d, cosf(pitch) * d };
}

} // namespace

// Avança a câmera um frame e devolve a matriz view-projection, já na
// convenção do OpenGL.
Mat4 updateCamera(RzContext* ctx) {
    const float cs = ctx->cellSize;
    const float hs = ctx->heightScale;
    const Vec3 terrainCenter = { 127.5f * cs, 127.5f * hs, 127.5f * cs };

    const int32_t target = ctx->cameraTargetObject;
    const bool following = target >= 0 && target < int32_t(ctx->objects.size()) &&
                           ctx->objects[target].alive && ctx->objects[target].positioned &&
                           ctx->cameraTargetVertex < ctx->objects[target].vertexCount();

    Vec3 eye, at;
    if (following) {
        // Perseguição: câmera na corda, olhando para o vértice-alvo suavizado
        // (o mesmo ponto puxa a corda, então a altura da câmera também não treme)
        const Vec3 raw = ctx->objects[target].world[ctx->cameraTargetVertex];
        ctx->targetPos = raw;
        if (!ctx->followInitialized) {
            ctx->retargetPending = ctx->retargetActive = false;
            if (ctx->camHasLast && !ctx->camLastFollowing) {
                // Saindo da visão geral (ou do meio da ida para ela): voa de
                // onde a câmera está até atrás do alvo
                ctx->overviewActive = true;
                ctx->overviewToward = false;
                ctx->overviewT = 0.0f;
                ctx->overviewBlend0 = ctx->overviewBlend;
                ctx->overviewEye0 = ctx->camLastEye;
                ctx->overviewLook0 = ctx->camLastLook;
                float dx = raw.x - ctx->camLastEye.x, dz = raw.z - ctx->camLastEye.z;
                const float d = sqrtf(dx * dx + dz * dz);
                if (d > 1e-4f) { dx /= d; dz /= d; } else { dx = 0.0f; dz = -1.0f; }
                ctx->overviewDir = { dx, 0.0f, dz };
                ctx->followInitialized = true;
                ctx->followAppliedDistance = ctx->followDistance;
            } else {
                ctx->overviewActive = false;
                ctx->overviewBlend = 0.0f;
            }
        }
        if (ctx->retargetPending) {
            ctx->overviewActive = false;               // outro alvo no meio: voo normal
            startRetarget(ctx, raw);
        }
        if (ctx->overviewActive) {
            float t;
            const float e = stepOverview(ctx, &t);
            Vec3 rest = restEye(ctx, raw, ctx->overviewDir);
            Vec3 eye = lerp3(ctx->overviewEye0, rest, e);
            // desce só na segunda metade: sobre o caminho, a câmera fica alta
            eye.y = ctx->overviewEye0.y + (rest.y - ctx->overviewEye0.y) * smooth01(2.0f * t - 1.0f);
            const float floorY = groundHeight(ctx, eye.x, eye.z) + kFollowClearance * cs;
            if (eye.y < floorY) eye.y = floorY;
            ctx->followEye = eye;
            ctx->followLook = lerp3(ctx->overviewLook0, raw, smooth01(1.5f * t));
            ctx->overviewBlend = ctx->overviewBlend0 * (1.0f - smooth01(2.0f * t - 1.0f));   // junto com a descida
        } else if (ctx->retargetActive) {
            updateRetarget(ctx, raw);
        } else {
            if (!ctx->followInitialized) {
                ctx->followLook = raw;
            } else {
                Vec3& look = ctx->followLook;
                look.x += (raw.x - look.x) * kFollowLookXZ;
                look.z += (raw.z - look.z) * kFollowLookXZ;
                look.y += (raw.y - look.y) * kFollowLookY;
            }
            updateFollowCamera(ctx, ctx->followLook);
        }
        if (!ctx->overviewActive && ctx->overviewBlend > 0.0f) {
            // transição interrompida por outro alvo: a neblina termina sozinha
            ctx->overviewBlend -= 1.0f / kOverviewFrames;
            if (ctx->overviewBlend < 0.0f) ctx->overviewBlend = 0.0f;
        }
        at = ctx->followLook;
        eye = ctx->followEye;
    } else {
        ctx->followInitialized = false;
        ctx->retargetPending = ctx->retargetActive = false;
        Vec3 ovEye;
        overviewCamera(ctx, terrainCenter, &ovEye);
        if (ctx->camHasLast && ctx->camLastFollowing) {
            // Indo para a visão geral (ou do meio da saída dela)
            ctx->overviewActive = true;
            ctx->overviewToward = true;
            ctx->overviewT = 0.0f;
            ctx->overviewBlend0 = ctx->overviewBlend;
            ctx->overviewEye0 = ctx->camLastEye;
            ctx->overviewLook0 = ctx->camLastLook;
        } else if (!ctx->camHasLast) {
            ctx->overviewActive = false;
        }
        if (ctx->overviewActive && ctx->overviewToward) {
            float t;
            const float e = stepOverview(ctx, &t);
            eye = lerp3(ctx->overviewEye0, ovEye, e);
            // sobe já na primeira metade, para não atravessar morro
            eye.y = ctx->overviewEye0.y + (ovEye.y - ctx->overviewEye0.y) * smooth01(2.0f * t);
            const float floorY = groundHeight(ctx, eye.x, eye.z) + kFollowClearance * cs;
            if (eye.y < floorY) eye.y = floorY;
            at = lerp3(ctx->overviewLook0, terrainCenter, e);
            ctx->overviewBlend = ctx->overviewBlend0 + (1.0f - ctx->overviewBlend0) * smooth01(2.0f * t);   // junto com a subida
        } else {
            ctx->overviewActive = false;
            eye = ovEye;
            at = terrainCenter;
            ctx->overviewBlend = 1.0f;
        }
    }
    ctx->camHasLast = true;
    ctx->camLastFollowing = following;
    ctx->camLastEye = eye;
    ctx->camLastLook = at;

    // Neblina: seguindo, em volta do olho (rzSetFog); na visão geral, em volta
    // do centro do mapa, só depois do terreno (esconde o fim da continuação).
    // Na transição, origem e distâncias são interpoladas. Sombras: cascatas
    // seguindo, mapa do terreno inteiro na visão geral (troca no meio).
    const float b = ctx->overviewBlend;
    const float ovNear = ctx->terrainRadius, ovFar = ctx->terrainRadius + kOverviewFogDepth * cs;
    ctx->eyePos = eye;
    ctx->fogOn  = following || ctx->overviewActive || ctx->hasTerrain();   // sem terreno, a visão geral não tem o que esconder
    ctx->fogOrigin = lerp3(eye, terrainCenter, b);
    ctx->fogNear = ctx->fogStart * cs + (ovNear - ctx->fogStart * cs) * b;
    ctx->fogFar  = ctx->fogEnd * cs + (ovFar - ctx->fogEnd * cs) * b;
    ctx->shadowWholeTerrain = b >= 0.5f;
    ctx->wallOn = following && b < 0.5f;

    const Mat4 view = lookAt(eye, at);
    ctx->camRight   = { view[0, 0], view[0, 1], view[0, 2] };
    ctx->camUp      = { view[1, 0], view[1, 1], view[1, 2] };
    ctx->camForward = { -view[2, 0], -view[2, 1], -view[2, 2] };
    const Vec3 toAt = at - eye;
    const float focusDist = sqrtf(dot(toAt, toAt));

    // near/far: o terreno inteiro (esfera de raio R) e o ponto observado ficam
    // dentro do frustum em profundidade.
    const Vec3 toTerrain = eye - terrainCenter;
    const float terrainDist = sqrtf(dot(toTerrain, toTerrain));
    const float radius = ctx->terrainRadius;
    float nearPlane = terrainDist - radius;
    if (nearPlane > 0.5f * focusDist) nearPlane = 0.5f * focusDist;
    if (nearPlane < 0.002f * radius) nearPlane = 0.002f * radius;   // ~0,36 tile: chão perto da câmera
    float farPlane = terrainDist + radius;
    // Com neblina, nada além do fim dela aparece: o far encosta nele (melhor precisão)
    const Vec3 toFog = ctx->fogOrigin - eye;
    const float fogFar = (sqrtf(dot(toFog, toFog)) + ctx->fogFar) * 1.02f;
    if (farPlane > fogFar) farPlane = fogFar;
    if (farPlane < nearPlane * 2.0f) farPlane = nearPlane * 2.0f;
    ctx->nearPlane = nearPlane;
    ctx->farPlane  = farPlane;

    return perspective(ctx->focalX, ctx->focalY, nearPlane, farPlane, !ctx->windowed) * view;
}

// Matriz de view olhando de `eye` para `at`, y para cima (sistema destro,
// câmera olhando para −z no espaço de view).
Mat4 lookAt(Vec3 eye, Vec3 at) {
    Vec3 f = at - eye;
    const float invF = 1.0f / sqrtf(dot(f, f));
    f = { f.x * invF, f.y * invF, f.z * invF };
    Vec3 r = cross(f, Vec3{ 0.0f, 1.0f, 0.0f });
    const float invR = 1.0f / sqrtf(dot(r, r));
    r = { r.x * invR, r.y * invR, r.z * invR };
    const Vec3 u = cross(r, f);

    Mat4 v = Mat4::identity();
    v[0, 0] =  r.x; v[0, 1] =  r.y; v[0, 2] =  r.z; v[0, 3] = -dot(r, eye);
    v[1, 0] =  u.x; v[1, 1] =  u.y; v[1, 2] =  u.z; v[1, 3] = -dot(u, eye);
    v[2, 0] = -f.x; v[2, 1] = -f.y; v[2, 2] = -f.z; v[2, 3] =  dot(f, eye);
    return v;
}

} // namespace rz
