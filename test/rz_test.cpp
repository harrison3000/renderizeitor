// rz_test: executável de teste do Renderizeitor (versão OpenGL).
// Linka o núcleo estaticamente (RZ_STATIC), usa a mesma API C da DLL e
// renderiza no modo offscreen (rzCreate com buffer de pixels).
// Os hashes só são comparáveis na mesma máquina/driver.
//
//   rz_test [-w largura] [-h altura] [-n frames] [-e salvar_a_cada]
//           [-o pasta_saida] [-m heightmap.raw] [-r referencia.txt]
//           [-t 0|1 texturas] [-f 0..4 filtro]
//           [-O 0|1 objetos] [-c 0|1 camera segue o veiculo (0: visão geral)]
//
// Filtro: 0 nearest, 1 mipmap, 2 mipmap com dither (padrão), 3 mipmap linear, 4 trilinear.
//
// Sem -m, gera um heightmap procedural determinístico (ilha).
// Texturas (padrão ligado) usam o atlas e o mapa de blocos procedurais de
// rz_testdata.h; o atlas também é gravado em atlas.ppm na pasta de saída.
// Com -r: se o arquivo existe, compara os hashes; senão, cria a referência.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <chrono>
#include <vector>

#include "renderizeitor.h"
#include "rz_testdata.h"

namespace {

// ---------------------------------------------------------------------------
// Utilidades
// ---------------------------------------------------------------------------

uint32_t fnv1a(const uint32_t* pixels, int32_t count) {
    uint32_t h = 2166136261u;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t p = pixels[i];
        for (int k = 0; k < 4; ++k) {
            h ^= p & 0xFFu;
            h *= 16777619u;
            p >>= 8;
        }
    }
    return h;
}

bool writePpm(const char* path, const uint32_t* pixels, int32_t w, int32_t h) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    std::vector<unsigned char> line(size_t(w) * 3);
    for (int32_t y = 0; y < h; ++y) {
        for (int32_t x = 0; x < w; ++x) {
            const uint32_t p = pixels[y * w + x];
            line[x * 3 + 0] = (p >> 16) & 0xFF;
            line[x * 3 + 1] = (p >> 8) & 0xFF;
            line[x * 3 + 2] = p & 0xFF;
        }
        std::fwrite(line.data(), 1, line.size(), f);
    }
    std::fclose(f);
    return true;
}

bool loadRaw(const char* path, uint8_t* out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    const size_t n = std::fread(out, 1, 256 * 256, f);
    std::fclose(f);
    return n == 256 * 256;
}

} // namespace

int main(int argc, char** argv) {
    int32_t width = 1280, height = 720, frames = 64, saveEvery = 16;
    const char* outDir = ".";
    const char* rawPath = nullptr;
    const char* refPath = nullptr;
    int textures = 1;
    int objects = 1;
    int follow = 1;
    int filter = RZ_FILTER_MIP_DITHER;

    for (int i = 1; i + 1 < argc; i += 2) {
        const char* opt = argv[i];
        const char* val = argv[i + 1];
        if      (!std::strcmp(opt, "-w")) width = std::atoi(val);
        else if (!std::strcmp(opt, "-h")) height = std::atoi(val);
        else if (!std::strcmp(opt, "-n")) frames = std::atoi(val);
        else if (!std::strcmp(opt, "-e")) saveEvery = std::atoi(val);
        else if (!std::strcmp(opt, "-o")) outDir = val;
        else if (!std::strcmp(opt, "-m")) rawPath = val;
        else if (!std::strcmp(opt, "-r")) refPath = val;
        else if (!std::strcmp(opt, "-t")) textures = std::atoi(val);
        else if (!std::strcmp(opt, "-O")) objects = std::atoi(val);
        else if (!std::strcmp(opt, "-c")) follow = std::atoi(val);
        else if (!std::strcmp(opt, "-f")) filter = std::atoi(val);
        else { std::fprintf(stderr, "opcao desconhecida: %s\n", opt); return 2; }
    }

    bool ok = true;

    char path[1024];
    static uint8_t heightmap[256 * 256];
    if (rawPath) {
        if (!loadRaw(rawPath, heightmap)) { std::fprintf(stderr, "falha ao ler %s\n", rawPath); return 1; }
    } else {
        rztdGenerateHeightmap(heightmap);
    }

    std::vector<uint32_t> pixels(static_cast<size_t>(width) * height);
    RzContext* ctx = nullptr;
    int32_t err = rzCreate(width, height, pixels.data(), &ctx);
    if (err != RZ_OK) { std::fprintf(stderr, "rzCreate falhou: %d\n", err); return 1; }
    err = rzSetHeightmap(ctx, heightmap, 256, 256);
    if (err != RZ_OK) { std::fprintf(stderr, "rzSetHeightmap falhou: %d\n", err); return 1; }
    if (rzSetTextureFilter(ctx, filter) != RZ_OK) { std::fprintf(stderr, "filtro invalido: %d\n", filter); return 1; }

    if (textures) {
        static uint8_t atlas[RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE];
        static uint8_t palette[768];
        static uint8_t tileMap[256 * 256];
        rztdGenerateAtlas(atlas, palette);
        rztdGenerateTileMap(heightmap, tileMap);
        err = rzSetTileAtlas(ctx, atlas, RZTD_ATLAS_SIZE, RZTD_ATLAS_SIZE, palette);
        if (err == RZ_OK) err = rzSetTileMap(ctx, tileMap, 256, 256);
        if (err != RZ_OK) { std::fprintf(stderr, "texturas falharam: %d\n", err); return 1; }

        // Atlas em RGB para conferência
        static uint32_t atlasRgb[RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE];
        for (int i = 0; i < RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE; ++i) {
            const uint8_t* p = palette + atlas[i] * 3;
            atlasRgb[i] = (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2];
        }
        std::snprintf(path, sizeof(path), "%s/atlas.ppm", outDir);
        writePpm(path, atlasRgb, RZTD_ATLAS_SIZE, RZTD_ATLAS_SIZE);
    }

    // Objetos: casas e torres paradas, e um cubo que o "host" gira a cada frame
    static RztdMesh buildings[RZTD_MAX_OBJECTS];
    static uint32_t buildingColors[RZTD_MAX_OBJECTS];
    static RztdMesh cube;
    int32_t cubeId = -1;
    float cubeX = 128.0f, cubeY = 0.0f, cubeZ = 128.0f;
    static RztdMesh vehicle;
    int32_t vehicleId = -1;
    const float heightScale = RZTD_HEIGHT_SCALE;   // padrão de rzSetTerrainScale
    auto placeVehicle = [&](int frame) {
        float x, z, heading;
        rztdVehiclePath(float(frame) / 8192.0f, &x, &z, &heading);   // ~0,05 tile por frame
        rztdVehicle(&vehicle, heightmap, heightScale, x, z, heading);
    };
    if (objects) {
        // Primeiro objeto: o veículo, que anda e é o alvo da câmera
        placeVehicle(0);
        err = rzCreateObject(ctx, vehicle.vertices, vehicle.vertexCount,
                             vehicle.indices, vehicle.indexCount, &vehicleId);
        if (err != RZ_OK) { std::fprintf(stderr, "rzCreateObject (veiculo) falhou: %d\n", err); return 1; }
        rzSetObjectColor(ctx, vehicleId, 0x003070E0u);
        rzSetObjectCulling(ctx, vehicleId, RZ_CULL_CW);
        if (follow) rzSetCameraTarget(ctx, vehicleId, RZTD_VEHICLE_TARGET);

        const int count = rztdGenerateBuildings(heightmap, heightScale, buildings, buildingColors,
                                                RZTD_MAX_OBJECTS);
        for (int i = 0; i < count; ++i) {
            int32_t id;
            err = rzCreateObject(ctx, buildings[i].vertices, buildings[i].vertexCount,
                                 buildings[i].indices, buildings[i].indexCount, &id);
            if (err != RZ_OK) { std::fprintf(stderr, "rzCreateObject falhou: %d\n", err); return 1; }
            rzSetObjectColor(ctx, id, buildingColors[i]);
            rzSetObjectCulling(ctx, id, RZ_CULL_CW);
        }
        int top = 0;
        for (int i = 0; i < 256 * 256; ++i) if (heightmap[i] > top) top = heightmap[i];
        cubeY = float(top) * heightScale + 2.0f;
        rztdSpinningCube(&cube, cubeX, cubeY, cubeZ, 0.6f, 0.0f);
        err = rzCreateObject(ctx, cube.vertices, cube.vertexCount, cube.indices, cube.indexCount, &cubeId);
        if (err != RZ_OK) { std::fprintf(stderr, "rzCreateObject (cubo) falhou: %d\n", err); return 1; }
        rzSetObjectColor(ctx, cubeId, 0x00E04030u);
        rzSetObjectCulling(ctx, cubeId, RZ_CULL_CW);
        std::printf("objetos: 1 veiculo + %d construcoes + 1 cubo\n", count);
    }

    std::vector<uint32_t> hashes(static_cast<size_t>(frames));
    double totalMs = 0.0;
    for (int f = 0; f < frames; ++f) {
        if (cubeId >= 0) {
            rztdSpinningCube(&cube, cubeX, cubeY, cubeZ, 0.6f, float(f) * 0.05f);
            rzUpdateObjectVertices(ctx, cubeId, cube.vertices);
        }
        if (vehicleId >= 0) {
            placeVehicle(f);
            rzUpdateObjectVertices(ctx, vehicleId, vehicle.vertices);
        }
        const auto t0 = std::chrono::steady_clock::now();
        rzRender(ctx);
        const auto t1 = std::chrono::steady_clock::now();
        totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();

        hashes[f] = fnv1a(pixels.data(), width * height);
        if (saveEvery > 0 && (f % saveEvery == 0 || f == frames - 1)) {
            std::snprintf(path, sizeof(path), "%s/frame_%04d.ppm", outDir, f);
            if (!writePpm(path, pixels.data(), width, height)) std::fprintf(stderr, "falha ao gravar %s\n", path);
        }
    }
    std::printf("%d frames %dx%d, media %.2f ms/frame\n", frames, width, height, totalMs / frames);

    if (refPath) {
        FILE* rf = std::fopen(refPath, "r");
        if (rf) {
            int mismatches = 0, compared = 0;
            int idx; unsigned int h;
            while (std::fscanf(rf, "%d %x", &idx, &h) == 2) {
                if (idx >= 0 && idx < frames) {
                    ++compared;
                    if (hashes[idx] != h) {
                        ++mismatches;
                        std::printf("frame %d: hash %08x, referencia %08x\n", idx, hashes[idx], h);
                    }
                }
            }
            std::fclose(rf);
            std::printf("regressao: %d frames comparados, %d diferentes -> %s\n",
                        compared, mismatches, mismatches ? "FALHOU" : "ok");
            if (mismatches) ok = false;
        } else {
            rf = std::fopen(refPath, "w");
            if (rf) {
                for (int f = 0; f < frames; ++f) std::fprintf(rf, "%d %08x\n", f, hashes[f]);
                std::fclose(rf);
                std::printf("referencia criada: %s\n", refPath);
            }
        }
    }

    rzDestroy(ctx);
    return ok ? 0 : 1;
}
