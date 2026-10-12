#version 330 core
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
uniform vec3  uFogOrigin;   // olho (seguindo) ou centro do mapa (visão geral)
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
        float fog = smoothstep(uFogStart, uFogEnd, length(vWorld - uFogOrigin));
        color = mix(color, uFogColor, fog);
        alpha *= 1.0 - fog;
    }
    fragColor = vec4(color, alpha);
}
