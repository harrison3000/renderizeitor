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
uniform float uShading;                   // força da luz sobre a textura

flat out vec3  vColor;
flat out int   vLayer;
flat out float vLight;
out vec2 vUV;

void main() {
    vColor = aColor.bgr;
    vUV    = aUvLayer.xy;
    vLayer = int(aUvLayer.z);
    vLight = mix(1.0, aUvLayer.w / 255.0, uShading);
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

// Filtros (RZ_FILTER_*): 0 nearest, 1 mipmap, 2 mipmap com dither, 3 mipmap
// linear entre níveis, 4 trilinear. 0, 1, 3 e 4 são só estado do sampler; o
// dither (2) escolhe o nível no shader: lod = log2 da maior derivada de uv em
// texels, e o nível = floor(lod + limiar de Bayer 4x4) — o mesmo do software.
// Com textura, a cor é multiplicada por um sombreamento leve (vLight), a mesma
// luz flat do triângulo atenuada por uShading.
// A saída tem alfa 0: no modo offscreen ele vira o byte reservado do RGBQUAD.
constexpr const char* kTerrainFragmentShader = R"GLSL(#version 330 core
flat in vec3  vColor;
flat in int   vLayer;
flat in float vLight;
in vec2 vUV;

uniform sampler2DArray uAtlas;
uniform int uTextured;
uniform int uFilter;

out vec4 fragColor;

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    if (uTextured == 0) {
        fragColor = vec4(vColor, 0.0);
        return;
    }
    vec3 uvw = vec3(vUV, float(vLayer));
    vec3 texel;
    if (uFilter == 2) {
        vec2 t  = vUV * 16.0;
        vec2 dx = dFdx(t);
        vec2 dy = dFdy(t);
        float rho2 = max(dot(dx, dx), dot(dy, dy));
        float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, 4.0);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        float level = min(floor(lod + threshold), 4.0);
        texel = textureLod(uAtlas, uvw, level).rgb;
    } else {
        texel = texture(uAtlas, uvw).rgb;
    }
    fragColor = vec4(texel * vLight, 0.0);
}
)GLSL";

// Objetos: posição no mundo, cor flat por vértice (0xAARRGGBB em memória =
// B, G, R, A, lido como vec4 normalizado) e UV. A = 1 marca polígono
// texturizado: aí RGB é a luz que multiplica a textura.
constexpr const char* kObjectVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
flat out vec4 vColor;
out vec2 vUV;
void main() {
    vColor = vec4(aColor.bgr, aColor.a);
    vUV = aUV;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

// Mesmos filtros do chão; no dither (2) o nível vem das derivadas de uv em
// texels (uTexSize) e do limiar de Bayer 4x4, limitado a uMaxLevel.
constexpr const char* kObjectFragmentShader = R"GLSL(#version 330 core
flat in vec4 vColor;
in vec2 vUV;

uniform sampler2D uTexture;
uniform int   uFilter;
uniform float uTexSize;
uniform float uMaxLevel;

out vec4 fragColor;

const float kBayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0,
                                   3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);

void main() {
    if (vColor.a < 0.5) {
        fragColor = vec4(vColor.rgb, 0.0);
        return;
    }
    vec3 texel;
    if (uFilter == 2) {
        vec2 t  = vUV * uTexSize;
        vec2 dx = dFdx(t);
        vec2 dy = dFdy(t);
        float rho2 = max(dot(dx, dx), dot(dy, dy));
        float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, uMaxLevel);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        float level = min(floor(lod + threshold), uMaxLevel);
        texel = textureLod(uTexture, vUV, level).rgb;
    } else {
        texel = texture(uTexture, vUV).rgb;
    }
    fragColor = vec4(texel * vColor.rgb, 0.0);
}
)GLSL";

} // namespace rz
