#version 330 core
layout(location = 0) in vec3 aPosition;   // linhas e faces das rodas: posição direta
layout(location = 1) in vec4 aNormal;     // idem, normal (10 bits por eixo)
layout(location = 2) in vec2 aUV;
layout(location = 3) in ivec3 aPolygon;   // objetos: vértice (-1: usa aPosition/aNormal),
                                          // início e tamanho do polígono em uIndices
uniform mat4 uViewProj;
uniform mat4 uModel;                      // identidade; rodas: posição/giro de cada roda
uniform int  uFromBuffer;                 // 1: objetos (posições em uPositions); 0: faces das rodas
uniform samplerBuffer  uPositions;        // posições no mundo dos vértices do objeto
uniform usamplerBuffer uIndices;          // índices dos polígonos (sem os fechamentos)
uniform mat4 uShadowMatrix[3];            // luz: cascatas 0, 1 e 2
uniform vec3  uLight;                     // direção para a luz (normalizada)
uniform float uAmbient;
flat out float vLight;
out vec2 vUV;
out vec3 vShadow0, vShadow1, vShadow2;
out vec3 vWorld;                          // para a neblina

vec3 vertexAt(int i) { return texelFetch(uPositions, i).xyz; }

// Normal de fora do polígono (Newell): uma só para o polígono inteiro, mesmo
// não plano. No sentido do legado (horário visto de fora), a de Newell aponta
// para dentro; a de fora é a oposta. Degenerada: para baixo (como packNormal).
vec3 polygonNormal(int first, int count) {
    vec3 n = vec3(0.0);
    vec3 cur = vertexAt(int(texelFetch(uIndices, first + count - 1).r));
    for (int k = 0; k < count; ++k) {
        vec3 next = vertexAt(int(texelFetch(uIndices, first + k).r));
        n += vec3((next.y - cur.y) * (next.z + cur.z),
                  (next.z - cur.z) * (next.x + cur.x),
                  (next.x - cur.x) * (next.y + cur.y));
        cur = next;
    }
    return dot(n, n) > 1.0e-20 ? n : vec3(0.0, -1.0, 0.0);
}

void main() {
    vec3 position = aPosition;
    vec3 normal = aNormal.xyz;
    if (uFromBuffer != 0 && aPolygon.x >= 0) {
        position = vertexAt(aPolygon.x);
        normal = polygonNormal(aPolygon.y, aPolygon.z);
    }
    vLight = uAmbient + (1.0 - uAmbient) * max(dot(normalize(mat3(uModel) * normal), uLight), 0.0);
    vUV = aUV;
    vec4 world = uModel * vec4(position, 1.0);
    vShadow0 = (uShadowMatrix[0] * world).xyz * 0.5 + 0.5;
    vShadow1 = (uShadowMatrix[1] * world).xyz * 0.5 + 0.5;
    vShadow2 = (uShadowMatrix[2] * world).xyz * 0.5 + 0.5;
    vWorld = world.xyz;
    gl_Position = uViewProj * world;
}
