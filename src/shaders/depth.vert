#version 330 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uLightViewProj;
void main() {
    gl_Position = uLightViewProj * vec4(aPosition, 1.0);
}
