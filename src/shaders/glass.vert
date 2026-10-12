#version 330 core
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
