#version 330 core
in vec3 vUV;
flat in vec3 vColor;
flat in float vOpacity;
flat in int vType;
flat in vec3 vCenter;
in vec3 vWorld;
uniform sampler2DArray uNoise;
uniform float uTime;        // segundos x velocidade do ruído da fumaça
uniform int   uFogOn;
uniform float uFogStart, uFogEnd;
uniform vec3  uFogColor;
uniform vec3  uEye;
uniform vec3  uFogOrigin;   // olho (seguindo) ou centro do mapa (visão geral)
out vec4 fragColor;
float hash3(vec3 p) {
    p = fract(p * 0.3183099 + 0.1) * 17.0;
    return fract(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float valueNoise(vec3 x) {          // 0..1, suave (interpolação cúbica)
    vec3 i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(hash3(i),                   hash3(i + vec3(1.0, 0.0, 0.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 0.0)), hash3(i + vec3(1.0, 1.0, 0.0)), f.x), f.y),
               mix(mix(hash3(i + vec3(0.0, 0.0, 1.0)), hash3(i + vec3(1.0, 0.0, 1.0)), f.x),
                   mix(hash3(i + vec3(0.0, 1.0, 1.0)), hash3(i + vec3(1.0, 1.0, 1.0)), f.x), f.y), f.z);
}
void main() {
    vec3 color;
    float a;
    if (vType == 1) {               // fumaça
        vec2 q = vUV.xy * 2.0 - 1.0;
        float d = length(q);
        if (d >= 1.0) discard;
        vec3 np = vec3(q * 1.6 + vCenter.xz * 0.7 + vec2(vCenter.y * 0.5), uTime);
        float n = 0.65 * valueNoise(np) + 0.35 * valueNoise(np * 2.03 + 17.0);
        a = n * vOpacity * (1.0 - smoothstep(0.5, 1.0, d));
        color = vec3(1.0);
    } else {                        // detrito
        a = texture(uNoise, vUV).r;
        color = vColor;
    }
    if (a <= 0.0) discard;
    float fog = uFogOn != 0 ? smoothstep(uFogStart, uFogEnd, length(vWorld - uFogOrigin)) : 0.0;
    if (fog >= 1.0) discard;
    fragColor = vec4(mix(color, uFogColor, fog), a);
}
