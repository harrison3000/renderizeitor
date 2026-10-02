// Implementação da interface C (versão OpenGL). As alocações e uploads para a
// GPU acontecem na criação e nas funções de carga; rzRender não aloca.

#include <cmath>

#include "rz_internal.h"

using namespace rz;

namespace {

bool isPositiveFinite(float v) {
    return v > 0.0f && v < 3.0e38f;
}

void setSize(RzContext* ctx, int32_t width, int32_t height) {
    constexpr float degToRad = kPi / 180.0f;
    const float focal = 1.0f / tanf(kFovYDegrees * 0.5f * degToRad);
    ctx->width  = width;
    ctx->height = height;
    ctx->focalY = focal;
    ctx->focalX = focal * float(height) / float(width);
}

// Estado padrão + plataforma + OpenGL. Comum aos dois modos.
int32_t createContext(RzContext** outCtx, Platform* platform, bool windowed,
                      int32_t width, int32_t height, void* pixels) {
    if (!platform) return RZ_ERR_GL;
    if (!loadGl(platformGetProc)) {
        platformDestroy(platform);
        return RZ_ERR_GL;
    }

    RzContext* ctx = new RzContext;     // padrões nos inicializadores dos membros
    ctx->platform   = platform;
    ctx->windowed   = windowed;
    ctx->hostPixels = static_cast<uint32_t*>(pixels);
    setSize(ctx, width, height);
    updateTerrainBounds(ctx);

    if (!createRenderer(ctx)) {
        rzDestroy(ctx);
        return RZ_ERR_GL;
    }
    *outCtx = ctx;
    return RZ_OK;
}

} // namespace

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzCreate(int32_t width, int32_t height,
                                         void* pixels, RzContext** outCtx) {
    if (!outCtx) return RZ_ERR_INVALID_ARG;
    *outCtx = nullptr;
    if (!pixels) return RZ_ERR_INVALID_ARG;
    if (width < 1 || height < 1) return RZ_ERR_INVALID_ARG;
    if (width > RZ_MAX_WIDTH || height > RZ_MAX_HEIGHT) return RZ_ERR_SIZE;
    return createContext(outCtx, platformCreateOffscreen(), false, width, height, pixels);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzCreateWindow(void* parentWindow, int32_t x, int32_t y,
                                               int32_t width, int32_t height, RzContext** outCtx) {
    if (!outCtx) return RZ_ERR_INVALID_ARG;
    *outCtx = nullptr;
    if (!parentWindow || width < 1 || height < 1) return RZ_ERR_INVALID_ARG;
    if (width > kMaxWindowSize || height > kMaxWindowSize) return RZ_ERR_SIZE;
    return createContext(outCtx, platformCreateChildWindow(parentWindow, x, y, width, height),
                         true, width, height, nullptr);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetViewport(RzContext* ctx, int32_t x, int32_t y,
                                              int32_t width, int32_t height) {
    if (!ctx || !ctx->windowed) return RZ_ERR_INVALID_ARG;
    if (width < 1 || height < 1) return RZ_ERR_INVALID_ARG;
    if (width > kMaxWindowSize || height > kMaxWindowSize) return RZ_ERR_SIZE;
    if (!platformMoveWindow(ctx->platform, x, y, width, height)) return RZ_ERR_GL;
    setSize(ctx, width, height);
    return RZ_OK;
}

RZ_API RZ_ENTRY void RZ_CALL rzDestroy(RzContext* ctx) {
    if (!ctx) return;
    if (ctx->platform && platformMakeCurrent(ctx->platform)) {
        destroyAllObjects(ctx);
        destroyRenderer(ctx);
    }
    platformDestroy(ctx->platform);
    // ctx->hostPixels pertence ao host: nunca é liberado aqui.
    delete ctx;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetHeightmap(RzContext* ctx, const uint8_t* data,
                                               int32_t width, int32_t height) {
    if (!ctx || !data) return RZ_ERR_INVALID_ARG;
    if (width != kGridSize || height != kGridSize) return RZ_ERR_SIZE;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    ctx->heights.assign(data, data + kVertexCount);
    updateTerrainBounds(ctx);
    return buildTerrainMesh(ctx) ? RZ_OK : RZ_ERR_GL;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetTerrainScale(RzContext* ctx,
                                                  float cellSize, float heightScale) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (!isPositiveFinite(cellSize) || !isPositiveFinite(heightScale)) return RZ_ERR_INVALID_ARG;
    ctx->cellSize    = cellSize;
    ctx->heightScale = heightScale;
    updateTerrainBounds(ctx);
    if (ctx->hasTerrain()) {          // posições e iluminação dependem da escala
        if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
        if (!buildTerrainMesh(ctx)) return RZ_ERR_GL;
    }
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetTileAtlas(RzContext* ctx, const uint8_t* indices,
                                               int32_t width, int32_t height,
                                               const uint8_t* paletteRGB) {
    if (!ctx || !indices || !paletteRGB) return RZ_ERR_INVALID_ARG;
    if (width < kTileSize || height < kTileSize || width > 4096 || height > 4096 ||
        (width % kTileSize) != 0 || (height % kTileSize) != 0) {
        return RZ_ERR_SIZE;
    }
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;

    // 256 blocos x (256 + 64 + 16 + 4 + 1) texels, por nível; temporário da carga
    constexpr int32_t kTexels = kMaxTiles * 341;
    std::vector<uint32_t> tiles(kTexels);
    buildAtlasLevels(tiles.data(), indices, width, height, paletteRGB);

    if (!ctx->atlasTex) glGenTextures(1, &ctx->atlasTex);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx->atlasTex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    constexpr int32_t kLevelOffset[kMipLevels] = { 0, 256, 320, 336, 340 };
    for (int32_t level = 0; level < kMipLevels; ++level) {
        const int32_t side = kTileSize >> level;
        glTexImage3D(GL_TEXTURE_2D_ARRAY, level, GL_RGBA8, side, side, kMaxTiles, 0,
                     GL_BGRA, GL_UNSIGNED_BYTE, tiles.data() + kMaxTiles * kLevelOffset[level]);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, kMipLevels - 1);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    applyTextureFilter(ctx);
    if (glGetError() != GL_NO_ERROR) return RZ_ERR_GL;
    ctx->hasAtlas = true;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetTileMap(RzContext* ctx, const uint8_t* data,
                                             int32_t width, int32_t height) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (!data) {
        ctx->hasTileMap = false;     // a malha mantém os blocos; só deixam de ser usados
        return RZ_OK;
    }
    if (width != kGridSize || height != kGridSize) return RZ_ERR_SIZE;

    ctx->tileMap.assign(data, data + kVertexCount);
    if (ctx->hasTerrain()) {          // o bloco vai no vértice
        if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
        if (!buildTerrainMesh(ctx)) return RZ_ERR_GL;
    }
    ctx->hasTileMap = true;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetTextureFilter(RzContext* ctx, int32_t filter) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (filter < RZ_FILTER_NEAREST || filter > RZ_FILTER_TRILINEAR) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    ctx->textureFilter = filter;
    applyTextureFilter(ctx);
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetCameraTarget(RzContext* ctx, int32_t id, int32_t vertex) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (id < 0) {
        ctx->cameraTargetObject = -1;
        return RZ_OK;
    }
    if (id >= int32_t(ctx->objects.size()) || !ctx->objects[id].alive ||
        vertex < 0 || vertex >= ctx->objects[id].vertexCount()) {
        return RZ_ERR_INVALID_ARG;
    }
    if (id != ctx->cameraTargetObject) ctx->followInitialized = false;   // reposiciona atrás do novo alvo
    ctx->cameraTargetObject = id;
    ctx->cameraTargetVertex = vertex;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzSetCameraFollow(RzContext* ctx, float distance, float height,
                                                  float stiffness) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (!(distance > 0.0f) || !(height == height) || !(stiffness > 0.0f)) return RZ_ERR_INVALID_ARG;
    if (stiffness > 1.0f) stiffness = 1.0f;
    ctx->followDistance  = distance;
    ctx->followHeight    = height;
    ctx->followStiffness = stiffness;
    return RZ_OK;
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzRender(RzContext* ctx) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    renderFrame(ctx);
    return glGetError() == GL_NO_ERROR ? RZ_OK : RZ_ERR_GL;
}

} // extern "C"
