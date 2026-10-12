#version 330 core
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
