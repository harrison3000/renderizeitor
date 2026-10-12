#version 330 core
flat in vec3  vColor;                     // cor base, sem luz
flat in int   vLayer;
flat in float vLight;
in float vLightSmooth;
in vec2 vUV;
in vec3 vShadow0, vShadow1, vShadow2;
in vec3 vWorld;

// Neblina: smoothstep da distância a uFogOrigin entre uFogStart e uFogEnd, na
// cor uFogColor (a de fundo).
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;
uniform vec3  uFogOrigin;   // olho (seguindo) ou centro do mapa (visão geral)

uniform sampler2DArray  uAtlas;
uniform sampler2DShadow uShadow0, uShadow1, uShadow2;
uniform vec3  uCascade;                   // fim da cascata 0, fim da 1, faixa de transição (mundo)
uniform int   uCascadeOnlyFar;            // visão geral: só a cascata 2
uniform int   uTextured;
uniform int   uSmooth;                    // 1: luz por pixel (vLightSmooth), p/ as rodas
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
    return smoothstep(uFogStart, uFogEnd, length(vWorld - uFogOrigin));
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
    float base  = (uSmooth != 0) ? vLightSmooth : vLight;   // por pixel nas rodas, flat no resto
    float light = mix(min(uShadowLight, base), base, lit);
    vec3 color;
    if (uTextured == 0) {
        color = vColor * light;                             // vColor já é a cor base, sem luz
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
