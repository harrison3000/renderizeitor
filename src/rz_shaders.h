// Shaders GLSL 3.30 core.
#pragma once

namespace rz {

// Terreno: malha montada na CPU na carga (rz_terrain.cpp), um vértice por
// canto de triângulo, com a cor flat do triângulo e (u, v, bloco).
constexpr const char* kTerrainVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;      // B, G, R, 0 normalizados
layout(location = 2) in vec3 aUvLayer;    // u, v em {0, 1}; bloco 0..255
uniform mat4 uViewProj;

flat out vec3 vColor;
flat out int  vLayer;
out vec2 vUV;

void main() {
    vColor = aColor.bgr;
    vUV    = aUvLayer.xy;
    vLayer = int(aUvLayer.z);
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

// Filtros (RZ_FILTER_*): 0 nearest, 1 mipmap, 2 mipmap com dither, 3 mipmap
// linear entre níveis, 4 trilinear. 0, 1, 3 e 4 são só estado do sampler; o
// dither (2) escolhe o nível no shader: lod = log2 da maior derivada de uv em
// texels, e o nível = floor(lod + limiar de Bayer 4x4) — o mesmo do software.
// A saída tem alfa 0: no modo offscreen ele vira o byte reservado do RGBQUAD.
constexpr const char* kTerrainFragmentShader = R"GLSL(#version 330 core
flat in vec3 vColor;
flat in int  vLayer;
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
    if (uFilter == 2) {
        vec2 t  = vUV * 16.0;
        vec2 dx = dFdx(t);
        vec2 dy = dFdy(t);
        float rho2 = max(dot(dx, dx), dot(dy, dy));
        float lod = clamp(0.5 * log2(max(rho2, 1e-12)), 0.0, 4.0);
        ivec2 p = ivec2(gl_FragCoord.xy) & 3;
        float threshold = (kBayer[p.y * 4 + p.x] + 0.5) / 16.0;
        float level = min(floor(lod + threshold), 4.0);
        fragColor = vec4(textureLod(uAtlas, uvw, level).rgb, 0.0);
    } else {
        fragColor = vec4(texture(uAtlas, uvw).rgb, 0.0);
    }
}
)GLSL";

// Objetos: posição no mundo e cor flat por vértice (0x00RRGGBB em memória =
// B, G, R, 0, lido como vec4 normalizado).
constexpr const char* kObjectVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;
uniform mat4 uViewProj;
flat out vec3 vColor;
void main() {
    vColor = aColor.bgr;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

constexpr const char* kObjectFragmentShader = R"GLSL(#version 330 core
flat in vec3 vColor;
out vec4 fragColor;
void main() {
    fragColor = vec4(vColor, 0.0);
}
)GLSL";

} // namespace rz
