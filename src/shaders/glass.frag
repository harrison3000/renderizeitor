#version 330 core
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
uniform vec3  uFogOrigin;   // olho (seguindo) ou centro do mapa (visão geral)
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
    float fog = uFogOn != 0 ? smoothstep(uFogStart, uFogEnd, length(vWorld - uFogOrigin)) : 0.0;
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
