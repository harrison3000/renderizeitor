// Câmera: perseguição de um vértice-alvo "na corda". Sem alvo, visão geral
// fixa do terreno inteiro (fallback para testes e base de um futuro modo de
// câmera livre). Roda na CPU uma vez por frame e produz a view-projection.

#include <cmath>

#include "rz_internal.h"

namespace rz {

namespace {

// Altura do terreno (mundo) no ponto (x, z) do mundo, bilinear; 0 sem terreno.
float groundHeight(const RzContext* ctx, float x, float z) {
    if (!ctx->hasTerrain()) return 0.0f;
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
        if (!ctx->followInitialized) {
            ctx->followLook = raw;
        } else {
            Vec3& look = ctx->followLook;
            look.x += (raw.x - look.x) * kFollowLookXZ;
            look.z += (raw.z - look.z) * kFollowLookXZ;
            look.y += (raw.y - look.y) * kFollowLookY;
        }
        at = ctx->followLook;
        updateFollowCamera(ctx, at);
        eye = ctx->followEye;
    } else {
        ctx->followInitialized = false;
        at = terrainCenter;
        overviewCamera(ctx, at, &eye);
    }
    // Neblina e caixa do mapa de sombra 1: seguindo um alvo, a caixa fica à
    // frente do olho (cobre a parte visível sem neblina); na visão geral, sem
    // neblina e com o terreno inteiro.
    ctx->eyePos = eye;
    ctx->fogOn  = following;
    ctx->shadowWholeTerrain = !following;
    if (following) {
        float fx = at.x - eye.x, fz = at.z - eye.z;
        const float len = sqrtf(fx * fx + fz * fz);
        if (len > 1e-4f) { fx /= len; fz /= len; } else { fx = 0.0f; fz = -1.0f; }
        const float ahead = kShadowNearAhead * ctx->cellSize;
        ctx->shadowFocus = { eye.x + fx * ahead, at.y, eye.z + fz * ahead };
    } else {
        ctx->shadowFocus = at;
    }

    const Mat4 view = lookAt(eye, at);
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
    // Com neblina, nada além de kFogEnd aparece: o far encosta nela (melhor precisão)
    const float fogFar = kFogEnd * ctx->cellSize * 1.02f;
    if (following && farPlane > fogFar) farPlane = fogFar;
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
