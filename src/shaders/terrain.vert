#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColor;      // B, G, R, 0 normalizados
layout(location = 2) in vec4 aUvLayer;    // u, v em {0, 1}; bloco 0..255; -
layout(location = 3) in vec4 aNormal;     // normal da face (10 bits por eixo)
uniform mat4 uViewProj;
uniform mat4 uShadowMatrix[3];            // luz: cascatas 0, 1 e 2
uniform vec3  uLight;                     // direção para a luz (normalizada)
uniform float uAmbient;

flat out vec3  vColor;                    // cor base do triângulo, SEM luz
flat out int   vLayer;
flat out float vLight;                    // luz flat do triângulo, 0..1
out float vLightSmooth;                   // a mesma luz, interpolada (normais por vértice): rodas
out vec2 vUV;
out vec3 vShadow0, vShadow1, vShadow2;
out vec3 vWorld;                          // para a neblina

void main() {
    vLight = uAmbient + (1.0 - uAmbient) * max(dot(normalize(aNormal.xyz), uLight), 0.0);
    vLightSmooth = vLight;
    vColor = aColor.bgr;
    vUV    = aUvLayer.xy;
    vLayer = int(aUvLayer.z);
    vec4 world = vec4(aPosition, 1.0);
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    vWorld = aPosition;
    gl_Position = uViewProj * world;
}
