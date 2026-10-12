// Shaders GLSL 3.30 core, um por arquivo em src/shaders/, incorporados com
// #embed (bytes + 0 no fim, para o glShaderSource).
#pragma once

namespace rz {

// Terreno: malha montada na CPU na carga (rz_terrain.cpp), um vértice por
// canto de triângulo, com a cor do triângulo (sem luz), (u, v, bloco) e a
// normal da face. Luz flat: ambiente + difusa pela normal, por triângulo
// (os três vértices têm a mesma normal; `flat` pega a do último).
inline constexpr unsigned char kTerrainVertexShader[] = {
#embed "shaders/terrain.vert"
, 0 };

// Filtro fixo, só estado do sampler: nearest dentro do nível de mipmap e
// mistura linear entre níveis (GL_NEAREST_MIPMAP_LINEAR); textureGrad com as
// derivadas tiradas antes do desvio da neblina.
// Com textura, a cor é multiplicada por um sombreamento leve (a luz flat do
// triângulo atenuada por uShading) e, na sombra, por uShadowDim. Nas cores
// flat, que já vêm iluminadas, a luz do triângulo cai para uShadowLight.
// A saída tem alfa 0: no modo offscreen ele vira o byte reservado do RGBQUAD.
inline constexpr unsigned char kTerrainFragmentShader[] = {
#embed "shaders/terrain.frag"
, 0 };

// Objetos: posição no mundo, normal de fora do polígono (a mesma em todos os
// triângulos dele, mesmo não plano) e UV. Luz flat como no terreno. Todo
// polígono é texturizado (os de cor sólida amostram um bloquinho da faixa de
// paleta).
inline constexpr unsigned char kObjectVertexShader[] = {
#embed "shaders/object.vert"
, 0 };

// Mesmo filtro do chão (estado do sampler).
inline constexpr unsigned char kObjectFragmentShader[] = {
#embed "shaders/object.frag"
, 0 };

// Vidro (rz_glass.cpp): saída rgb = brilho especular, a = cinza do filtro;
// blending GL_ONE, GL_SRC_ALPHA → destino = brilho + destino x cinza.
inline constexpr unsigned char kGlassVertexShader[] = {
#embed "shaders/glass.vert"
, 0 };

inline constexpr unsigned char kGlassFragmentShader[] = {
#embed "shaders/glass.frag"
, 0 };

// Sprites (rz_sprites.cpp): quad virado para a câmera, aberto no vertex
// shader a partir do centro, do raio e do canto (-1/+1); neblina.
//   Detrito (tipo 0): alfa da textura de ruído (array, uma camada por quadro);
//     cor já com luz.
//   Fumaça (tipo 1): disco branco, alfa = ruído de valor 3D (posição no disco
//     + centro no mundo, tempo) x opacidade, com a borda suavizada.
inline constexpr unsigned char kSpriteVertexShader[] = {
#embed "shaders/sprite.vert"
, 0 };

inline constexpr unsigned char kSpriteFragmentShader[] = {
#embed "shaders/sprite.frag"
, 0 };

// Parede de limite (rz_border.cpp): semitransparente, com X vermelhos de
// uXSize tiles; só perto do alvo (fade entre uFadeFar e uFadeNear, distância
// horizontal do alvo ao ponto da parede); com neblina.
inline constexpr unsigned char kWallVertexShader[] = {
#embed "shaders/wall.vert"
, 0 };

inline constexpr unsigned char kWallFragmentShader[] = {
#embed "shaders/wall.frag"
, 0 };

// Passe da sombra: só profundidade, do ponto de vista da luz. Serve para o
// terreno e para os objetos (posição no atributo 0 nos dois).
inline constexpr unsigned char kDepthVertexShader[] = {
#embed "shaders/depth.vert"
, 0 };

inline constexpr unsigned char kDepthFragmentShader[] = {
#embed "shaders/depth.frag"
, 0 };

} // namespace rz
