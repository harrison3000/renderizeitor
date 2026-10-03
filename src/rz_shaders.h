// Shaders GLSL 3.30 core.
#pragma once

namespace rz {

// Terreno: malha montada na CPU na carga (rz_terrain.cpp), um vértice por
// canto de triângulo, com a cor flat do triângulo e (u, v, bloco, luz).
constexpr const char* kTerrainVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;      // B, G, R, 0 normalizados
layout(location = 2) in vec4 aUvLayer;    // u, v em {0, 1}; bloco 0..255; luz 0..255
uniform mat4 uViewProj;
uniform mat4 uShadowMatrix[3];            // luz: terreno, objetos próximos, alvo

flat out vec3  vColor;
flat out int   vLayer;
flat out float vLight;                    // luz flat do triângulo, 0..1
out vec2 vUV;
out vec3 vShadow0, vShadow1, vShadow2;
out vec3 vWorld;                          // para a neblina

void main() {
    vColor = aColor.bgr;
    vUV    = aUvLayer.xy;
    vLayer = int(aUvLayer.z);
    vLight = aUvLayer.w / 255.0;
    vec4 world = vec4(aPosition, 1.0);
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    vWorld = aPosition;
    gl_Position = uViewProj * world;
}
)GLSL";

// Filtros (RZ_FILTER_*): 0 nearest, 1 mipmap, 2 mipmap com dither, 3 mipmap
// linear entre níveis, 4 trilinear. 0, 1, 3 e 4 são só estado do sampler; o
// dither (2) escolhe o nível no shader: lod = log2 da maior derivada de uv em
// texels, e o nível = floor(lod + limiar de Bayer 4x4) — o mesmo do software.
// Com textura, a cor é multiplicada por um sombreamento leve (a luz flat do
// triângulo atenuada por uShading) e, na sombra, por uShadowDim. Nas cores
// flat, que já vêm iluminadas, a luz do triângulo cai para o ambiente.
// A saída tem alfa 0: no modo offscreen ele vira o byte reservado do RGBQUAD.
constexpr const char* kTerrainFragmentShader = R"GLSL(#version 330 core
flat in vec3  vColor;
flat in int   vLayer;
flat in float vLight;
in vec2 vUV;
in vec3 vShadow0, vShadow1, vShadow2;
in vec3 vWorld;

// Neblina: smoothstep da distância ao olho entre uFogStart e uFogEnd, na cor
// uFogColor (a de fundo). Desligada na visão geral.
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;

uniform sampler2DArray  uAtlas;
uniform sampler2DShadow uShadow0, uShadow1, uShadow2;
uniform int   uShadowTargetOn;            // mapa 2 (objeto seguido) em uso
uniform int   uTextured;
uniform int   uFilter;
uniform float uShading;
uniform float uAmbient;
uniform float uShadowDim;                 // chão texturizado na sombra: fator fixo

out vec4 fragColor;

// Sombra: 1 iluminado, 0 na sombra. Fora da caixa de um mapa, ele não sombreia.
// sampler2DShadow com GL_LINEAR: PCF 2x2 do hardware. textureLod: o mapa não
// tem mipmap e a leitura fica dentro de um if (sem derivadas).
float shadowLit(sampler2DShadow map, vec3 c) {
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    return textureLod(map, c, 0.0);
}

// Os três mapas (rz_shadow.cpp): iluminado = mínimo
float shadowTerm() {
    float lit = min(shadowLit(uShadow0, vShadow0), shadowLit(uShadow1, vShadow1));
    if (uShadowTargetOn != 0) lit = min(lit, shadowLit(uShadow2, vShadow2));
    return lit;
}

// Fator de neblina (0 limpo, 1 só neblina)
float fogFactor() {
    if (uFogOn == 0) return 0.0;
    return smoothstep(uFogStart, uFogEnd, length(vWorld - uEye));
}

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    // Derivadas antes de qualquer desvio: a neblina sai cedo em parte dos pixels
    vec2 dUVx = dFdx(vUV), dUVy = dFdy(vUV);
    float fog = fogFactor();
    if (fog >= 1.0) {                        // só neblina: nem textura nem sombra
        fragColor = vec4(uFogColor, 0.0);
        return;
    }
    float lit   = shadowTerm();
    float light = mix(min(uAmbient, vLight), vLight, lit);
    vec3 color;
    if (uTextured == 0) {
        color = vColor * (light / max(vLight, 0.001));
        fragColor = vec4(mix(color, uFogColor, fog), 0.0);
        return;
    }
    vec3 uvw = vec3(vUV, float(vLayer));
    vec3 texel;
    if (uFilter == 2) {
        vec2 dx = dUVx * 16.0;
        vec2 dy = dUVy * 16.0;
        float rho2 = max(dot(dx, dx), dot(dy, dy));
        float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, 4.0);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        float level = min(floor(lod + threshold), 4.0);
        texel = textureLod(uAtlas, uvw, level).rgb;
    } else {
        texel = textureGrad(uAtlas, uvw, dUVx, dUVy).rgb;
    }
    color = texel * mix(1.0, vLight, uShading) * mix(uShadowDim, 1.0, lit);
    fragColor = vec4(mix(color, uFogColor, fog), 0.0);
}
)GLSL";

// Objetos: posição no mundo, luz flat por vértice (cinza; B, G, R, 0 lido como
// vec4 normalizado) e UV. Todo polígono é texturizado (os de cor sólida
// amostram um bloquinho da faixa de paleta).
constexpr const char* kObjectVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
uniform mat4 uShadowMatrix[3];            // luz: terreno, objetos próximos, alvo
flat out float vLight;
out vec2 vUV;
out vec3 vShadow0, vShadow1, vShadow2;
out vec3 vWorld;                          // para a neblina
void main() {
    vLight = aColor.b;                     // cinza: os três canais são iguais
    vUV = aUV;
    vec4 world = vec4(aPosition, 1.0);
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    vWorld = aPosition;
    gl_Position = uViewProj * world;
}
)GLSL";

// Mesmos filtros do chão; no dither (2) o nível vem das derivadas de uv em
// texels (uTexSize) e do limiar de Bayer 4x4, limitado a uMaxLevel.
constexpr const char* kObjectFragmentShader = R"GLSL(#version 330 core
flat in float vLight;
in vec2 vUV;
in vec3 vShadow0, vShadow1, vShadow2;
in vec3 vWorld;

// Neblina: smoothstep da distância ao olho entre uFogStart e uFogEnd, na cor
// uFogColor (a de fundo). Desligada na visão geral.
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;

uniform sampler2D       uTexture;
uniform sampler2DShadow uShadow0, uShadow1, uShadow2;
uniform int   uShadowTargetOn;            // mapa 2 (objeto seguido) em uso
uniform int   uFilter;
uniform float uTexSize;
uniform float uMaxLevel;
uniform float uAmbient;

out vec4 fragColor;

// Sombra: 1 iluminado, 0 na sombra. Fora da caixa de um mapa, ele não sombreia.
// sampler2DShadow com GL_LINEAR: PCF 2x2 do hardware. textureLod: o mapa não
// tem mipmap e a leitura fica dentro de um if (sem derivadas).
float shadowLit(sampler2DShadow map, vec3 c) {
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    return textureLod(map, c, 0.0);
}

// Os três mapas (rz_shadow.cpp): iluminado = mínimo
float shadowTerm() {
    float lit = min(shadowLit(uShadow0, vShadow0), shadowLit(uShadow1, vShadow1));
    if (uShadowTargetOn != 0) lit = min(lit, shadowLit(uShadow2, vShadow2));
    return lit;
}

// Fator de neblina (0 limpo, 1 só neblina)
float fogFactor() {
    if (uFogOn == 0) return 0.0;
    return smoothstep(uFogStart, uFogEnd, length(vWorld - uEye));
}

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    // Derivadas antes de qualquer desvio: a neblina sai cedo em parte dos pixels
    vec2 dUVx = dFdx(vUV), dUVy = dFdy(vUV);
    float fog = fogFactor();
    if (fog >= 1.0) {
        fragColor = vec4(uFogColor, 0.0);
        return;
    }
    vec3 texel;
    if (uFilter == 2) {
        vec2 dx = dUVx * uTexSize;
        vec2 dy = dUVy * uTexSize;
        float rho2 = max(dot(dx, dx), dot(dy, dy));
        float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, uMaxLevel);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        float level = min(floor(lod + threshold), uMaxLevel);
        texel = textureLod(uTexture, vUV, level).rgb;
    } else {
        texel = textureGrad(uTexture, vUV, dUVx, dUVy).rgb;
    }
    float lit = shadowTerm();
    vec3 color = texel * mix(min(uAmbient, vLight), vLight, lit);
    fragColor = vec4(mix(color, uFogColor, fog), 0.0);
}
)GLSL";

// Passe da sombra: só profundidade, do ponto de vista da luz. Serve para o
// terreno e para os objetos (posição no atributo 0 nos dois).
constexpr const char* kDepthVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uLightViewProj;
void main() {
    gl_Position = uLightViewProj * vec4(aPosition, 1.0);
}
)GLSL";

constexpr const char* kDepthFragmentShader = R"GLSL(#version 330 core
void main() {
}
)GLSL";

} // namespace rz
