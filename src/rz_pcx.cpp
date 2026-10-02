// Leitura do atlas em PCX (ZSoft): 8 bits por pixel, 1 plano, RLE e paleta
// VGA de 256 cores no fim do arquivo. Só a fase de carga usa este código.
//
// A imagem precisa ter pelo menos 256x256; de uma maior, só o canto superior
// esquerdo (256x256) é usado e o resto é ignorado.

#include <cstdio>

#include "rz_internal.h"

namespace rz {

namespace {

constexpr int32_t kPcxHeaderSize  = 128;
constexpr int32_t kPcxPaletteSize = 769;              // marcador 0x0C + 256 x RGB
constexpr long    kPcxMaxFileSize = 64L * 1024 * 1024;

uint32_t readU16(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
}

// Arquivo inteiro na memória (fase de carga).
int32_t readFile(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return RZ_ERR_FILE;
    int32_t err = RZ_OK;
    long size = -1;
    if (std::fseek(f, 0, SEEK_END) == 0) size = std::ftell(f);
    if (size < 0 || std::fseek(f, 0, SEEK_SET) != 0) {
        err = RZ_ERR_FILE;
    } else if (size > kPcxMaxFileSize) {
        err = RZ_ERR_SIZE;
    } else {
        out.resize(size_t(size));
        if (size > 0 && std::fread(out.data(), 1, size_t(size), f) != size_t(size)) err = RZ_ERR_FILE;
    }
    std::fclose(f);
    return err;
}

} // namespace

int32_t loadPcxAtlas(const char* path, uint8_t* indices, uint8_t* paletteRGB) {
    std::vector<uint8_t> file;
    const int32_t err = readFile(path, file);
    if (err != RZ_OK) return err;

    const int32_t size = int32_t(file.size());
    if (size < kPcxHeaderSize + kPcxPaletteSize) return RZ_ERR_FORMAT;
    const uint8_t* h = file.data();

    // Cabeçalho: 0x0A, versão, codificação (1 = RLE; 0 = sem compressão),
    // bits por pixel, janela xmin/ymin/xmax/ymax, planos, bytes por linha.
    const uint32_t encoding = h[2];
    if (h[0] != 0x0A || encoding > 1 || h[3] != 8 || h[65] != 1) return RZ_ERR_FORMAT;
    const uint32_t xMin = readU16(h + 4), yMin = readU16(h + 6);
    const uint32_t xMax = readU16(h + 8), yMax = readU16(h + 10);
    if (xMax < xMin || yMax < yMin) return RZ_ERR_FORMAT;
    const int32_t width        = int32_t(xMax - xMin + 1);
    const int32_t height       = int32_t(yMax - yMin + 1);
    const int32_t bytesPerLine = int32_t(readU16(h + 66));
    if (bytesPerLine < width) return RZ_ERR_FORMAT;
    if (width < kAtlasSize || height < kAtlasSize) return RZ_ERR_SIZE;

    // Paleta VGA: 0x0C seguido de 256 x (R, G, B), nos últimos 769 bytes
    const uint8_t* pal = h + size - kPcxPaletteSize;
    if (pal[0] != 0x0C) return RZ_ERR_FORMAT;
    std::memcpy(paletteRGB, pal + 1, 768);

    // Pixels: só as primeiras kAtlasSize linhas, e de cada uma só as primeiras
    // kAtlasSize colunas. Uma sequência RLE pode atravessar o fim da linha.
    const uint8_t* src = h + kPcxHeaderSize;
    const uint8_t* end = pal;
    uint32_t runValue = 0;
    int32_t  runLeft  = 0;
    for (int32_t y = 0; y < kAtlasSize; ++y) {
        uint8_t* row = indices + y * kAtlasSize;
        for (int32_t x = 0; x < bytesPerLine; ++x) {
            if (runLeft == 0) {
                if (src >= end) return RZ_ERR_FORMAT;          // dados truncados
                const uint32_t b = *src++;
                if (encoding == 1 && b >= 0xC0) {
                    if (src >= end) return RZ_ERR_FORMAT;
                    runLeft  = int32_t(b & 0x3F);
                    runValue = *src++;
                    if (runLeft == 0) { --x; continue; }       // sequência vazia: ignora
                } else {
                    runLeft  = 1;
                    runValue = b;
                }
            }
            if (x < kAtlasSize) row[x] = uint8_t(runValue);
            --runLeft;
        }
    }
    return RZ_OK;
}

} // namespace rz
