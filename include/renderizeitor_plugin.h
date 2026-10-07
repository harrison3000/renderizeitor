/* Renderizeitor como plugin: carga dinâmica da DLL em tempo de execução
 * (LoadLibrary + GetProcAddress), sem linkar com a import library.
 *
 * Uso:
 *     #include "renderizeitor_plugin.h"
 *
 *     RzPlugin rz;
 *     RzContext* ctx;
 *     if (rzPluginLoad(&rz, "renderizeitor.dll") != RZ_PLUGIN_OK) { ... }
 *     rz.rzCreateWindow(hwnd, 0, 0, 1280, 720, &ctx);
 *     rz.rzRender(ctx);
 *     ...
 *     rz.rzDestroy(ctx);
 *     rzPluginUnload(&rz);
 *
 * Independente de renderizeitor.h: traz as constantes e o tipo do contexto.
 * Cada membro tem o nome, a assinatura e a semântica da função de mesmo nome
 * em renderizeitor.h, que continua sendo a documentação da API. */
#pragma once

#include <stdint.h>
#include <windows.h>

#define RZ_CALL __cdecl

typedef struct RzContext RzContext;

/* Retornos */
#define RZ_OK               0
#define RZ_ERR_INVALID_ARG  1
#define RZ_ERR_SIZE         2
#define RZ_ERR_NO_MEMORY    3
#define RZ_ERR_GL           4
#define RZ_ERR_FILE         5
#define RZ_ERR_FORMAT       6

/* Limite do modo offscreen (rzCreate) */
#define RZ_MAX_WIDTH   1920
#define RZ_MAX_HEIGHT  1080

typedef struct RzVertex {
    float x, y, z;         /* tiles (z para cima) */
} RzVertex;

/* Canto de polígono texturizado: índice do vértice e UV. 12 bytes. */
typedef struct RzTexVertex {
    float    u, v;
    uint16_t index;
    uint16_t pad;
} RzTexVertex;

/* Uma roda (ver rzSetObjectWheels). 8 bytes. */
typedef struct RzWheel {
    float    diameter;     /* tiles */
    uint16_t hubVertex;    /* vértice do objeto no centro da roda */
    uint8_t  front;        /* != 0: dianteira */
    uint8_t  pad;
} RzWheel;

/* Um quadrado no atlas da textura do objeto, por dois cantos opostos em UV
   (0..1); o desenho de uma face da roda (ver rzSetObjectWheelFaces). 16 bytes. */
typedef struct RzWheelFace {
    float u0, v0;          /* um canto */
    float u1, v1;          /* o canto oposto */
} RzWheelFace;

/* Um sprite (ver rzSetSprites). 20 bytes. */
typedef struct RzSprite {
    float    x, y, z;      /* centro, em tiles (z para cima) */
    float    size;         /* diâmetro em tiles */
    uint8_t  color;        /* índice na paleta do jogo */
    uint8_t  pad[3];
} RzSprite;

/* Tamanhos conferidos em compilação (C89: array de tamanho negativo) */
typedef char RzAssertVertexSize[sizeof(RzVertex) == 12 ? 1 : -1];
typedef char RzAssertTexVertexSize[sizeof(RzTexVertex) == 12 ? 1 : -1];
typedef char RzAssertWheelSize[sizeof(RzWheel) == 8 ? 1 : -1];
typedef char RzAssertWheelFaceSize[sizeof(RzWheelFace) == 16 ? 1 : -1];
typedef char RzAssertSpriteSize[sizeof(RzSprite) == 20 ? 1 : -1];

/* X(retorno, nome, parâmetros) */
#define RZ_PLUGIN_FUNCTIONS(X)                                                                     \
    X(int32_t, rzCreate,        (int32_t width, int32_t height, void* pixels, RzContext** outCtx))   \
    X(int32_t, rzCreateWindow,  (void* parentWindow, int32_t x, int32_t y,                           \
                               int32_t width, int32_t height, RzContext** outCtx))                 \
    X(int32_t, rzSetViewport,   (RzContext* ctx, int32_t x, int32_t y, int32_t width, int32_t height)) \
    X(void,    rzDestroy,       (RzContext* ctx))                                                    \
    X(int32_t, rzSetHeightmap,  (RzContext* ctx, const uint8_t* data, int32_t width, int32_t height)) \
    X(int32_t, rzSetTerrainScale, (RzContext* ctx, float cellSize, float heightScale))               \
    X(int32_t, rzLoadTileAtlas, (RzContext* ctx, const char* pcxPath))                               \
    X(int32_t, rzSetTileMap,    (RzContext* ctx, const uint8_t* data, int32_t width, int32_t height)) \
    X(int32_t, rzUpdateTerrain, (RzContext* ctx))                                                      \
    X(int32_t, rzSetBackgroundColor, (RzContext* ctx, uint8_t r, uint8_t g, uint8_t b))              \
    X(int32_t, rzSetFog,        (RzContext* ctx, float start, float end))                            \
    X(int32_t, rzCreateObject,  (RzContext* ctx, int32_t vertexCount, int32_t* outId))               \
    X(int32_t, rzAddObjectPolygon, (RzContext* ctx, int32_t id, const uint16_t* indices,             \
                               int32_t count, int32_t paletteIndex))                               \
    X(int32_t, rzAddObjectTexturedPolygon, (RzContext* ctx, int32_t id, const RzTexVertex* corners,  \
                               int32_t count))                                                     \
    X(int32_t, rzAddObjectTranslucentPolygon, (RzContext* ctx, int32_t id, const uint16_t* indices,  \
                               int32_t count, int32_t tone))                                       \
    X(int32_t, rzAddObjectLine, (RzContext* ctx, int32_t id, uint16_t a, uint16_t b,                 \
                               float thickness, int32_t paletteIndex))                               \
    X(int32_t, rzSetObjectWheels, (RzContext* ctx, int32_t id, const RzWheel* wheels))               \
    X(int32_t, rzSetObjectWheelFaces, (RzContext* ctx, int32_t id, const RzWheelFace* outer,         \
                               const RzWheelFace* inner))                                            \
    X(int32_t, rzSetObjectWheelSpin, (RzContext* ctx, int32_t id, float speed))                      \
    X(int32_t, rzUpdateObjectWheels, (RzContext* ctx, int32_t id, float steer))                      \
    X(int32_t, rzLoadObjectTexture, (RzContext* ctx, int32_t id, const char* pcxPath))               \
    X(int32_t, rzLoadFallbackTexture, (RzContext* ctx, const char* pcxPath))                         \
    X(int32_t, rzUpdateObjectVertices, (RzContext* ctx, int32_t id, const RzVertex* vertices))       \
    X(int32_t, rzDestroyObject, (RzContext* ctx, int32_t id))                                        \
    X(int32_t, rzSetSprites,    (RzContext* ctx, const RzSprite* sprites, int32_t count))            \
    X(int32_t, rzSetCameraTarget, (RzContext* ctx, int32_t id, int32_t vertex))                      \
    X(int32_t, rzSetCameraFollow, (RzContext* ctx, float distance, float height, float stiffness))   \
    X(int32_t, rzRender,        (RzContext* ctx))                                                    \
    X(int32_t, rzGetError,      (RzContext* ctx, const char** outFunction))

#define RZ_PLUGIN_MEMBER(ret, name, params) ret (RZ_CALL *name) params;

typedef struct RzPlugin {
    HMODULE module;
    RZ_PLUGIN_FUNCTIONS(RZ_PLUGIN_MEMBER)
} RzPlugin;

#undef RZ_PLUGIN_MEMBER

#define RZ_PLUGIN_OK           0
#define RZ_PLUGIN_NO_LIBRARY   1   /* LoadLibrary falhou (DLL ou dependência não encontrada) */
#define RZ_PLUGIN_NO_FUNCTION  2   /* DLL de outra versão: falta alguma exportação */

#if defined(__GNUC__)
#  define RZ_PLUGIN_INLINE static __inline__
#else
#  define RZ_PLUGIN_INLINE static __inline
#endif

/* Descarrega a DLL e zera a tabela. Aceita plugin já descarregado. */
RZ_PLUGIN_INLINE void rzPluginUnload(RzPlugin* p) {
    if (p->module) FreeLibrary(p->module);
    ZeroMemory(p, sizeof(*p));
}

/* Carrega a DLL e resolve todas as funções. Em caso de erro, nada fica carregado. */
RZ_PLUGIN_INLINE int rzPluginLoad(RzPlugin* p, const char* dllPath) {
    ZeroMemory(p, sizeof(*p));
    p->module = LoadLibraryA(dllPath);
    if (!p->module) return RZ_PLUGIN_NO_LIBRARY;

    /* via void(*)(void): conversão entre ponteiros de função sem aviso do GCC */
#define RZ_PLUGIN_RESOLVE(ret, name, params)                                                       \
    p->name = (ret (RZ_CALL *) params)(void (*)(void))GetProcAddress(p->module, #name);       \
    if (!p->name) { rzPluginUnload(p); return RZ_PLUGIN_NO_FUNCTION; }
    RZ_PLUGIN_FUNCTIONS(RZ_PLUGIN_RESOLVE)
#undef RZ_PLUGIN_RESOLVE

    return RZ_PLUGIN_OK;
}
