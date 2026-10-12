#version 330 core
layout(location = 0) in vec4 aCenterRadius;
layout(location = 1) in vec4 aCornerFrame;  // canto x, canto y (-1/+1), quadro, tipo
layout(location = 2) in vec4 aColor;        // B, G, R, 0 normalizados
layout(location = 3) in float aOpacity;     // fumaça: multiplicador do ruído
uniform mat4 uViewProj;
uniform vec3 uRight, uUp;
out vec3 vUV;
flat out vec3 vColor;
flat out float vOpacity;
flat out int vType;
flat out vec3 vCenter;
out vec3 vWorld;
void main() {
    vec3 p = aCenterRadius.xyz + (uRight * aCornerFrame.x + uUp * aCornerFrame.y) * aCenterRadius.w;
    vUV = vec3(aCornerFrame.xy * 0.5 + 0.5, aCornerFrame.z);
    vColor = aColor.bgr;
    vOpacity = aOpacity;
    vType = int(aCornerFrame.w);
    vCenter = aCenterRadius.xyz;
    vWorld = p;
    gl_Position = uViewProj * vec4(p, 1.0);
}
