// Texturas dos objetos: uma textura 2D quadrada por objeto, lida de um PCX de
// 8 bits, e uma textura fallback para os objetos sem textura própria ou cuja
// carga falhou: um xadrez gerado aqui, ou um PCX (rzLoadFallbackTexture).
//
// Tamanho: a largura da imagem dita o lado W, a maior potência de 2 que cabe
// nela (mínimo 256, máximo kMaxObjectTexture e o limite do driver). Na
// horizontal, o que sobra à direita é descartado; na vertical, a imagem é
// cortada em W linhas ou completada embaixo repetindo a última linha. Assim
// v = 1 corresponde a W pixels, a mesma escala de u.
//
// Gambiarra das cores sólidas: as últimas kSwatchRows linhas (que deveriam ser
// padding) recebem 256 bloquinhos kSwatchBlock x kSwatchBlock, um por cor da
// paleta do PCX; os polígonos de rzAddObjectPolygon amostram o centro do
// bloco da sua cor. O que a imagem tiver nessas linhas é sobrescrito. O xadrez
// fallback gerado aqui não tem paleta: a faixa continua xadrez.
//
// Mipmaps gerados na CPU (média 2x2), como no atlas do chão; filtro fixo, o
// mesmo do chão (nearest no nível, linear entre níveis).

#include <cstring>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t  kMinObjectTexture = 256;
constexpr int32_t  kMaxObjectTexture = 4096;
constexpr int32_t  kFallbackSize     = 256;
constexpr int32_t  kFallbackCell     = 32;
constexpr uint32_t kFallbackColorA   = 0x00C040C0u;   // magenta: "sem textura"
constexpr uint32_t kFallbackColorB   = 0x00302830u;

int32_t floorPow2(int32_t v) {
    int32_t p = 1;
    while (p * 2 <= v) p *= 2;
    return p;
}

bool validId(const RzContext* ctx, int32_t id) {
    return ctx && id >= 0 && id < int32_t(ctx->objects.size()) && ctx->objects[id].alive;
}

void releaseTexture(Object& o) {
    if (o.texture) glDeleteTextures(1, &o.texture);
    o.texture = 0;
    o.textureSize = 0;
}

// Bloco da cor i: coluna i % (side / bloco), linha de blocos i / (side / bloco)
void swatchOrigin(int32_t i, int32_t side, int32_t* x, int32_t* y) {
    const int32_t perRow = side / kSwatchBlock;
    *x = (i % perRow) * kSwatchBlock;
    *y = side - kSwatchRows + (i / perRow) * kSwatchBlock;
}

} // namespace

// Centro do bloco da cor (texel kSwatchBlock/2 dentro dele), em UV.
void swatchUV(int32_t paletteIndex, int32_t side, float* u, float* v) {
    int32_t x, y;
    swatchOrigin(paletteIndex, side, &x, &y);
    const float half = float(kSwatchBlock) * 0.5f;
    *u = (float(x) + half) / float(side);
    *v = (float(y) + half) / float(side);
}

int32_t mipLevels(int32_t side) {
    int32_t levels = 1;
    while ((side >> (levels - 1)) > 1) ++levels;
    return levels;
}

// Nível anisotrópico a usar: kAnisotropy, limitado ao máximo do driver; 0 se
// o driver não tem EXT/ARB_texture_filter_anisotropic (core só no GL 4.6).
// Contexto já corrente.
float detectAnisotropy() {
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    bool found = false;
    for (GLint i = 0; i < count && !found; ++i) {
        const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i)));
        if (!e) continue;
        found = std::strcmp(e, "GL_EXT_texture_filter_anisotropic") == 0 ||
                std::strcmp(e, "GL_ARB_texture_filter_anisotropic") == 0;
    }
    if (!found) return 0.0f;
    GLfloat maxAniso = 0.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    if (!(maxAniso >= 1.0f)) return 0.0f;
    return maxAniso < kAnisotropy ? maxAniso : kAnisotropy;
}

// Filtro fixo: ampliação nearest; redução nearest dentro do nível e mistura
// linear entre níveis (GL_NEAREST_MIPMAP_LINEAR), mais o anisotrópico quando o
// driver tem (anisotropy > 0; na prática os drivers filtram a redução)
void applyFilter2D(GLuint texture, int32_t side, float anisotropy) {
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
    if (anisotropy > 0.0f) glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY, anisotropy);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, mipLevels(side) - 1);
}

// rgb: side x side em 0x00RRGGBB (linha 0 = v 0). Gera os mipmaps na CPU e sobe
// tudo; devolve 0 se o OpenGL falhar.
GLuint uploadSquareTexture(const uint32_t* rgb, int32_t side, float anisotropy) {
    const int32_t levels = mipLevels(side);
    std::vector<uint32_t> chain;                         // temporário da carga
    size_t total = 0;
    for (int32_t l = 0; l < levels; ++l) total += size_t(side >> l) * size_t(side >> l);
    chain.resize(total);
    std::memcpy(chain.data(), rgb, size_t(side) * size_t(side) * sizeof(uint32_t));

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    size_t offset = 0;
    for (int32_t l = 0; l < levels; ++l) {
        const int32_t s = side >> l;
        if (l > 0) {
            const int32_t prev = s * 2;
            downsample(chain.data() + offset - size_t(prev) * size_t(prev), chain.data() + offset, s);
        }
        fillTransparent(chain.data() + offset, s);       // cor dos buracos = vizinhos opacos
        glTexImage2D(GL_TEXTURE_2D, l, GL_RGBA8, s, s, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                     chain.data() + offset);
        offset += size_t(s) * size_t(s);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    applyFilter2D(texture, side, anisotropy);
    if (glGetError() != GL_NO_ERROR) {
        glDeleteTextures(1, &texture);
        return 0;
    }
    return texture;
}

// PCX -> textura quadrada (regras do topo do arquivo). Em caso de erro, nada é
// criado e *outTexture fica 0.
static int32_t loadSquareTextureFromPcx(const char* path, float anisotropy, GLuint* outTexture, int32_t* outSide) {
    *outTexture = 0;
    *outSide = 0;
    PcxImage img;
    const int32_t err = loadPcx(path, img);
    if (err != RZ_OK) return err;
    if (img.width < kMinObjectTexture) return RZ_ERR_SIZE;

    GLint driverMax = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &driverMax);
    int32_t limit = kMaxObjectTexture;
    if (driverMax > 0 && driverMax < limit) limit = driverMax;
    const int32_t side = floorPow2(img.width < limit ? img.width : limit);

    // Canto superior esquerdo, side x side; abaixo da imagem, repete a última linha
    uint32_t pal[256];
    for (int i = 0; i < 256; ++i) {
        pal[i] = (uint32_t(img.palette[i * 3 + 0]) << 16)
               | (uint32_t(img.palette[i * 3 + 1]) << 8)
               |  uint32_t(img.palette[i * 3 + 2])
               | (i == kTransparentIndex ? 0u : kOpaqueAlpha);   // 255: transparente
    }
    std::vector<uint32_t> rgb(size_t(side) * size_t(side));
    for (int32_t y = 0; y < side; ++y) {
        const int32_t srcY = y < img.height ? y : img.height - 1;
        const uint8_t* src = img.pixels.data() + size_t(srcY) * size_t(img.width);
        uint32_t* dst = rgb.data() + size_t(y) * size_t(side);
        for (int32_t x = 0; x < side; ++x) dst[x] = pal[src[x]];
    }

    // Faixa de amostras da paleta, no pé da textura
    for (int32_t i = 0; i < 256; ++i) {
        int32_t bx, by;
        swatchOrigin(i, side, &bx, &by);
        for (int32_t y = 0; y < kSwatchBlock; ++y) {
            for (int32_t x = 0; x < kSwatchBlock; ++x) {
                rgb[size_t(by + y) * size_t(side) + size_t(bx + x)] = pal[i];
            }
        }
    }

    *outTexture = uploadSquareTexture(rgb.data(), side, anisotropy);
    if (!*outTexture) return RZ_ERR_GL;
    *outSide = side;
    return RZ_OK;
}

// Xadrez magenta/escuro: fácil de ver que o objeto ficou sem textura.
// Substitui (e libera) a fallback atual.
bool createFallbackTexture(RzContext* ctx) {
    std::vector<uint32_t> rgb(size_t(kFallbackSize) * kFallbackSize);
    for (int32_t y = 0; y < kFallbackSize; ++y) {
        for (int32_t x = 0; x < kFallbackSize; ++x) {
            const bool a = ((x / kFallbackCell) + (y / kFallbackCell)) % 2 == 0;
            rgb[size_t(y) * kFallbackSize + x] = (a ? kFallbackColorA : kFallbackColorB) | kOpaqueAlpha;
        }
    }
    const GLuint texture = uploadSquareTexture(rgb.data(), kFallbackSize, ctx->anisotropy);
    if (!texture) return false;
    if (ctx->fallbackTex) glDeleteTextures(1, &ctx->fallbackTex);
    ctx->fallbackTex  = texture;
    ctx->fallbackSize = kFallbackSize;
    return true;
}

} // namespace rz

using namespace rz;

extern "C" {

RZ_API RZ_ENTRY int32_t RZ_CALL rzLoadObjectTexture(RzContext* ctx, int32_t id, const char* pcxPath) {
    if (!validId(ctx, id) || !pcxPath) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    Object& o = ctx->objects[id];
    releaseTexture(o);                       // em caso de erro: fallback
    return loadSquareTextureFromPcx(pcxPath, ctx->anisotropy, &o.texture, &o.textureSize);
}

RZ_API RZ_ENTRY int32_t RZ_CALL rzLoadFallbackTexture(RzContext* ctx, const char* pcxPath) {
    if (!ctx) return RZ_ERR_INVALID_ARG;
    if (!platformMakeCurrent(ctx->platform)) return RZ_ERR_GL;
    if (!pcxPath) return createFallbackTexture(ctx) ? RZ_OK : RZ_ERR_GL;   // volta ao xadrez

    GLuint texture;
    int32_t side;
    const int32_t err = loadSquareTextureFromPcx(pcxPath, ctx->anisotropy, &texture, &side);
    if (err != RZ_OK) return err;            // a fallback atual continua
    glDeleteTextures(1, &ctx->fallbackTex);
    ctx->fallbackTex  = texture;
    ctx->fallbackSize = side;
    return RZ_OK;
}

} // extern "C"
