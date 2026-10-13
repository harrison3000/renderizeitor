#version 330 core
// Passe da sombra dos objetos: a posição vem do texture buffer (como em
// object.vert); as linhas trazem a posição direta (vértice -1).
layout(location = 0) in vec3 aPosition;
layout(location = 3) in ivec3 aPolygon;   // vértice do objeto (-1: usa aPosition), polígono
uniform mat4 uLightViewProj;
uniform samplerBuffer uPositions;
void main() {
    vec3 position = aPolygon.x >= 0 ? texelFetch(uPositions, aPolygon.x).xyz : aPosition;
    gl_Position = uLightViewProj * vec4(position, 1.0);
}
