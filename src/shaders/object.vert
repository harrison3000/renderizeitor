#version 330 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aNormal;     // 10 bits por eixo
layout(location = 2) in vec2 aUV;
uniform mat4 uViewProj;
uniform mat4 uShadowMatrix[3];            // luz: cascatas 0, 1 e 2
uniform vec3  uLight;                     // direção para a luz (normalizada)
uniform float uAmbient;
flat out float vLight;
out vec2 vUV;
out vec3 vShadow0, vShadow1, vShadow2;
out vec3 vWorld;                          // para a neblina
void main() {
    vLight = uAmbient + (1.0 - uAmbient) * max(dot(normalize(aNormal.xyz), uLight), 0.0);
    vUV = aUV;
    vec4 world = vec4(aPosition, 1.0);
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    vWorld = aPosition;
    gl_Position = uViewProj * world;
}
