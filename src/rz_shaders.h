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
uniform mat4 uShadowMatrix[3];            // luz: cascatas 0, 1 e 2

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

// Filtro fixo, só estado do sampler: nearest dentro do nível de mipmap e
// mistura linear entre níveis (GL_NEAREST_MIPMAP_LINEAR); textureGrad com as
// derivadas tiradas antes do desvio da neblina.
// Com textura, a cor é multiplicada por um sombreamento leve (a luz flat do
// triângulo atenuada por uShading) e, na sombra, por uShadowDim. Nas cores
// flat, que já vêm iluminadas, a luz do triângulo cai para uShadowLight.
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
uniform vec3  uCascade;                   // fim da cascata 0, fim da 1, faixa de transição (mundo)
uniform int   uCascadeOnlyFar;            // visão geral: só a cascata 2
uniform int   uTextured;
uniform float uShading;
uniform float uShadowLight;               // luz na sombra (kShadowLight)
uniform float uShadowDim;                 // chão texturizado na sombra: fator fixo

out vec4 fragColor;

// Sombra: 1 iluminado, 0 na sombra. Fora da caixa de um mapa, ele não sombreia.
// sampler2DShadow com GL_LINEAR: PCF 2x2 do hardware. textureLod: o mapa não
// tem mipmap e a leitura fica dentro de um if (sem derivadas).
float shadowLit(sampler2DShadow map, vec3 c) {
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    return textureLod(map, c, 0.0);
}

// Cascatas (rz_shadow.cpp): escolhe pela distância ao olho; numa faixa de
// uCascade.z antes de cada divisa, mistura as duas vizinhas. Visão geral: só a 2.
float shadowTerm() {
    if (uCascadeOnlyFar != 0) return shadowLit(uShadow2, vShadow2);
    float d = length(vWorld - uEye);
    float b = uCascade.z;
    if (d < uCascade.x - b) return shadowLit(uShadow0, vShadow0);
    if (d < uCascade.x)
        return mix(shadowLit(uShadow0, vShadow0), shadowLit(uShadow1, vShadow1), (d - (uCascade.x - b)) / b);
    if (d < uCascade.y - b) return shadowLit(uShadow1, vShadow1);
    if (d < uCascade.y)
        return mix(shadowLit(uShadow1, vShadow1), shadowLit(uShadow2, vShadow2), (d - (uCascade.y - b)) / b);
    return shadowLit(uShadow2, vShadow2);
}

// Fator de neblina (0 limpo, 1 só neblina)
float fogFactor() {
    if (uFogOn == 0) return 0.0;
    return smoothstep(uFogStart, uFogEnd, length(vWorld - uEye));
}


void main() {
    // Derivadas antes de qualquer desvio: a neblina sai cedo em parte dos pixels
    vec2 dUVx = dFdx(vUV), dUVy = dFdy(vUV);
    float fog = fogFactor();
    if (fog >= 1.0) {                        // só neblina: nem textura nem sombra
        fragColor = vec4(uFogColor, 0.0);
        return;
    }
    float lit   = shadowTerm();
    float light = mix(min(uShadowLight, vLight), vLight, lit);
    vec3 color;
    if (uTextured == 0) {
        color = vColor * (light / max(vLight, 0.001));
        fragColor = vec4(mix(color, uFogColor, fog), 0.0);
        return;
    }
    vec3 uvw = vec3(vUV, float(vLayer));
    vec4 tex = textureGrad(uAtlas, uvw, dUVx, dUVy);         // nearest no nível, linear entre níveis
    if (tex.a < 0.5) discard;                                // índice 255 do PCX: transparente
    vec3 texel = tex.rgb;
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
uniform mat4 uShadowMatrix[3];            // luz: cascatas 0, 1 e 2
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

// Mesmo filtro do chão (estado do sampler).
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
uniform vec3  uCascade;                   // fim da cascata 0, fim da 1, faixa de transição (mundo)
uniform int   uCascadeOnlyFar;            // visão geral: só a cascata 2
uniform float uShadowLight;               // luz na sombra (kShadowLight)

out vec4 fragColor;

// Sombra: 1 iluminado, 0 na sombra. Fora da caixa de um mapa, ele não sombreia.
// sampler2DShadow com GL_LINEAR: PCF 2x2 do hardware. textureLod: o mapa não
// tem mipmap e a leitura fica dentro de um if (sem derivadas).
float shadowLit(sampler2DShadow map, vec3 c) {
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    return textureLod(map, c, 0.0);
}

// Cascatas (rz_shadow.cpp): escolhe pela distância ao olho; numa faixa de
// uCascade.z antes de cada divisa, mistura as duas vizinhas. Visão geral: só a 2.
float shadowTerm() {
    if (uCascadeOnlyFar != 0) return shadowLit(uShadow2, vShadow2);
    float d = length(vWorld - uEye);
    float b = uCascade.z;
    if (d < uCascade.x - b) return shadowLit(uShadow0, vShadow0);
    if (d < uCascade.x)
        return mix(shadowLit(uShadow0, vShadow0), shadowLit(uShadow1, vShadow1), (d - (uCascade.x - b)) / b);
    if (d < uCascade.y - b) return shadowLit(uShadow1, vShadow1);
    if (d < uCascade.y)
        return mix(shadowLit(uShadow1, vShadow1), shadowLit(uShadow2, vShadow2), (d - (uCascade.y - b)) / b);
    return shadowLit(uShadow2, vShadow2);
}

// Fator de neblina (0 limpo, 1 só neblina)
float fogFactor() {
    if (uFogOn == 0) return 0.0;
    return smoothstep(uFogStart, uFogEnd, length(vWorld - uEye));
}


void main() {
    // Derivadas antes de qualquer desvio: a neblina sai cedo em parte dos pixels
    vec2 dUVx = dFdx(vUV), dUVy = dFdy(vUV);
    float fog = fogFactor();
    if (fog >= 1.0) {
        fragColor = vec4(uFogColor, 0.0);
        return;
    }
    vec4 tex = textureGrad(uTexture, vUV, dUVx, dUVy);       // nearest no nível, linear entre níveis
    if (tex.a < 0.5) discard;                                // índice 255 do PCX: transparente
    vec3 texel = tex.rgb;
    float lit = shadowTerm();
    vec3 color = texel * mix(min(uShadowLight, vLight), vLight, lit);
    fragColor = vec4(mix(color, uFogColor, fog), 0.0);
}
)GLSL";

// Vidro (rz_glass.cpp): saída rgb = brilho especular, a = cinza do filtro;
// blending GL_ONE, GL_SRC_ALPHA → destino = brilho + destino x cinza.
constexpr const char* kGlassVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;     // de fora, por polígono
layout(location = 2) in float aTint;
uniform mat4 uViewProj;
uniform mat4 uShadowMatrix[3];
flat out vec3  vNormal;
flat out float vTint;
out vec3 vWorld;
out vec3 vShadow0, vShadow1, vShadow2;
void main() {
    vec4 world = vec4(aPosition, 1.0);
    vNormal = aNormal;
    vTint = aTint;
    vWorld = aPosition;
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    gl_Position = uViewProj * world;
}
)GLSL";

constexpr const char* kGlassFragmentShader = R"GLSL(#version 330 core
flat in vec3  vNormal;
flat in float vTint;
in vec3 vWorld;
in vec3 vShadow0, vShadow1, vShadow2;
uniform sampler2DShadow uShadow0, uShadow1, uShadow2;
uniform vec3  uCascade;
uniform int   uCascadeOnlyFar;
uniform vec3  uLight;                     // direção para a luz (normalizada)
uniform float uSpecular, uShininess;
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;
out vec4 fragColor;

float shadowLit(sampler2DShadow map, vec3 c) {
    if (c.x < 0.0 || c.x > 1.0 || c.y < 0.0 || c.y > 1.0 || c.z > 1.0) return 1.0;
    return textureLod(map, c, 0.0);
}

// Cascatas (rz_shadow.cpp): escolhe pela distância ao olho; numa faixa de
// uCascade.z antes de cada divisa, mistura as duas vizinhas. Visão geral: só a 2.
float shadowTerm() {
    if (uCascadeOnlyFar != 0) return shadowLit(uShadow2, vShadow2);
    float d = length(vWorld - uEye);
    float b = uCascade.z;
    if (d < uCascade.x - b) return shadowLit(uShadow0, vShadow0);
    if (d < uCascade.x)
        return mix(shadowLit(uShadow0, vShadow0), shadowLit(uShadow1, vShadow1), (d - (uCascade.x - b)) / b);
    if (d < uCascade.y - b) return shadowLit(uShadow1, vShadow1);
    if (d < uCascade.y)
        return mix(shadowLit(uShadow1, vShadow1), shadowLit(uShadow2, vShadow2), (d - (uCascade.y - b)) / b);
    return shadowLit(uShadow2, vShadow2);
}

void main() {
    float fog = uFogOn != 0 ? smoothstep(uFogStart, uFogEnd, length(vWorld - uEye)) : 0.0;
    if (fog >= 1.0) discard;
    vec3 n = normalize(vNormal);
    vec3 v = normalize(uEye - vWorld);
    float ndl = dot(n, uLight);
    float spec = 0.0;
    if (ndl > 0.0) {
        float lit = shadowTerm();
        vec3 h = normalize(uLight + v);
        spec = pow(max(dot(n, h), 0.0), uShininess) * uSpecular * lit;
    }
    spec *= 1.0 - fog;
    fragColor = vec4(vec3(spec), mix(vTint, 1.0, fog));
}
)GLSL";

// Sprites (rz_sprites.cpp): quad virado para a câmera, aberto no vertex
// shader a partir do centro, do raio e do canto (-1/+1); alfa da textura de
// ruído (array, uma camada por quadro); cor já com luz; neblina.
constexpr const char* kSpriteVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec4 aCenterRadius;
layout(location = 1) in vec4 aCornerFrame;  // canto x, canto y (-1/+1), quadro, 0
layout(location = 2) in vec4 aColor;        // B, G, R, 0 normalizados
uniform mat4 uViewProj;
uniform vec3 uRight, uUp;
out vec3 vUV;
flat out vec3 vColor;
out vec3 vWorld;
void main() {
    vec3 p = aCenterRadius.xyz + (uRight * aCornerFrame.x + uUp * aCornerFrame.y) * aCenterRadius.w;
    vUV = vec3(aCornerFrame.xy * 0.5 + 0.5, aCornerFrame.z);
    vColor = aColor.bgr;
    vWorld = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
)GLSL";

constexpr const char* kSpriteFragmentShader = R"GLSL(#version 330 core
in vec3 vUV;
flat in vec3 vColor;
in vec3 vWorld;
uniform sampler2DArray uNoise;
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;
out vec4 fragColor;
void main() {
    float a = texture(uNoise, vUV).r;
    if (a <= 0.0) discard;
    float fog = uFogOn != 0 ? smoothstep(uFogStart, uFogEnd, length(vWorld - uEye)) : 0.0;
    if (fog >= 1.0) discard;
    fragColor = vec4(mix(vColor, uFogColor, fog), a);
}
)GLSL";

// Parede de limite (rz_border.cpp): semitransparente, com X vermelhos de
// uXSize tiles; só perto do alvo (fade entre uFadeFar e uFadeNear, distância
// horizontal do alvo ao ponto da parede); com neblina.
constexpr const char* kWallVertexShader = R"GLSL(#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;          // tiles ao longo da parede / acima da base
uniform mat4 uViewProj;
out vec2 vUV;
out vec3 vWorld;
void main() {
    vUV = aUV;
    vWorld = aPosition;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
)GLSL";

constexpr const char* kWallFragmentShader = R"GLSL(#version 330 core
in vec2 vUV;
in vec3 vWorld;
uniform vec3  uTarget;
uniform float uFadeNear, uFadeFar;
uniform float uXSize;
uniform float uCellSize;
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;
out vec4 fragColor;
void main() {
    float near = 1.0 - smoothstep(uFadeNear, uFadeFar, length(vWorld.xz - uTarget.xz));
    if (near <= 0.0) discard;
    // Padrão preso ao mundo: ao longo da parede (u, tiles) e na altura do
    // mundo (não acompanha o relevo). Uma célula de uXSize tiles por círculo:
    // disco vermelho com borda branca e um X vazado no meio (mostra o fundo).
    // Raio R (fração da célula): diâmetro = metade da célula, então o espaço
    // entre dois círculos vizinhos é do tamanho de um círculo.
    const float R = 0.25;
    vec2  p  = vec2(vUV.x, vWorld.y / uCellSize) / uXSize;
    vec2  c  = fract(p) - 0.5;
    float r  = length(c);
    float aa = fwidth(p.x) + 0.002;                        // borda suave, ~1 pixel
    float disc  = 1.0 - smoothstep(R - aa, R + aa, r);
    float inner = 1.0 - smoothstep(R * 0.83 - aa, R * 0.83 + aa, r);
    float ring  = disc - inner;                            // borda branca
    float dx    = min(abs(c.x - c.y), abs(c.x + c.y)) * 0.70710678;
    float xMask = (1.0 - smoothstep(R * 0.13 - aa, R * 0.13 + aa, dx))
                * (1.0 - smoothstep(R * 0.6 - aa, R * 0.6 + aa, r));
    float red   = inner * (1.0 - xMask);
    vec3  color = vec3(0.62, 0.09, 0.07);                  // fundo: vermelho translúcido, escuro
    float alpha = 0.20;
    color = mix(color, vec3(0.92, 0.10, 0.07), red);  alpha = mix(alpha, 0.85, red);
    color = mix(color, vec3(1.0, 1.0, 1.0), ring);    alpha = mix(alpha, 0.90, ring);
    alpha *= near;
    if (uFogOn != 0) {
        float fog = smoothstep(uFogStart, uFogEnd, length(vWorld - uEye));
        color = mix(color, uFogColor, fog);
        alpha *= 1.0 - fog;
    }
    fragColor = vec4(color, alpha);
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
