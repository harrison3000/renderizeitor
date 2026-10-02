/* Dados de teste procedurais, compartilhados entre rz_test (C++) e rz_viewer (C).
 * Header-only, compila como C e como C++.
 *
 *   rztdGenerateHeightmap : ilha 256x256 (value noise + queda radial)
 *   rztdGenerateAtlas     : imagem paletizada 256x256 com 16x16 blocos de 16x16
 *   rztdWritePcx          : grava uma imagem paletizada como PCX de 8 bits (RLE),
 *                           para alimentar rzLoadTileAtlas
 *   rztdGenerateTileMap   : bloco de cada quad, escolhido pela altura
 */
#ifndef RZ_TESTDATA_H
#define RZ_TESTDATA_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define RZTD_ATLAS_SIZE 256
#define RZTD_HEIGHT_SCALE (16.0f / 255.0f)   /* byte 255 = 16 tiles, como no legado */

/* Tipos de bloco: cada um ocupa uma linha do atlas (16 variações) e usa uma
   rampa de 32 cores da paleta. */
enum {
    RZTD_WATER = 0,
    RZTD_SAND,
    RZTD_GRASS,
    RZTD_FOREST,
    RZTD_ROCK,
    RZTD_SNOW,
    RZTD_TYPE_COUNT
};

static uint32_t rztdHash(int32_t x, int32_t y, uint32_t seed) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static float rztdNoise(int ix, int iy, int oct) {
    return (float)(rztdHash(ix, iy, (uint32_t)oct + 7u) & 0xFFFFu) / 65535.0f;
}

/* ------------------------------------------------------------------------- */

static void rztdGenerateHeightmap(uint8_t* out) {
    static float tmp[256 * 256];
    float lo = 1e30f, hi = -1e30f;
    int x, y, i;

    for (y = 0; y < 256; ++y) {
        for (x = 0; x < 256; ++x) {
            float sum = 0.0f, amp = 1.0f, dx, dy, fall, val;
            int period, oct;
            for (period = 64, oct = 0; period >= 4; period >>= 1, ++oct) {
                int cx = x / period, cy = y / period;
                float fx = (float)(x % period) / (float)period;
                float fy = (float)(y % period) / (float)period;
                float a, b, c, d, top, bot;
                fx = fx * fx * (3.0f - 2.0f * fx);
                fy = fy * fy * (3.0f - 2.0f * fy);
                a = rztdNoise(cx, cy, oct);     b = rztdNoise(cx + 1, cy, oct);
                c = rztdNoise(cx, cy + 1, oct); d = rztdNoise(cx + 1, cy + 1, oct);
                top = a + (b - a) * fx;
                bot = c + (d - c) * fx;
                sum += (top + (bot - top) * fy) * amp;
                amp *= 0.5f;
            }
            dx = ((float)x - 127.5f) / 127.5f;
            dy = ((float)y - 127.5f) / 127.5f;
            fall = 1.0f - (dx * dx + dy * dy);
            if (fall < 0.0f) fall = 0.0f;
            val = sum * fall;
            tmp[y * 256 + x] = val;
            if (val < lo) lo = val;
            if (val > hi) hi = val;
        }
    }
    for (i = 0; i < 256 * 256; ++i) {
        float n = (tmp[i] - lo) / (hi - lo);
        n = n * n * 1.15f;
        if (n > 1.0f) n = 1.0f;
        out[i] = (uint8_t)(n * 255.0f + 0.5f);
    }
}

/* ------------------------------------------------------------------------- */

/* Paleta: rampa de 32 tons (escuro -> claro) por tipo, a partir do índice tipo*32. */
static void rztdBuildPalette(uint8_t* paletteRGB) {
    static const uint8_t dark[RZTD_TYPE_COUNT][3] = {
        {  10,  30,  80 }, { 150, 130,  85 }, {  30,  80,  20 },
        {  12,  45,  15 }, {  70,  60,  52 }, { 170, 175, 190 },
    };
    static const uint8_t light[RZTD_TYPE_COUNT][3] = {
        {  70, 130, 200 }, { 240, 225, 170 }, { 120, 190,  70 },
        {  60, 120,  45 }, { 175, 165, 150 }, { 255, 255, 255 },
    };
    int t, i, k;
    memset(paletteRGB, 0, 768);
    for (t = 0; t < RZTD_TYPE_COUNT; ++t) {
        for (i = 0; i < 32; ++i) {
            for (k = 0; k < 3; ++k) {
                int v = dark[t][k] + (light[t][k] - dark[t][k]) * i / 31;
                paletteRGB[(t * 32 + i) * 3 + k] = (uint8_t)v;
            }
        }
    }
}

/* Tom 0..31 de um texel do bloco (tipo, variação), padrão diferente por tipo. */
static int rztdTexelShade(int type, int variant, int x, int y) {
    uint32_t h = rztdHash(x + variant * 16, y, (uint32_t)type * 31u + 3u);
    int n = (int)(h & 7u);                   /* ruído fino 0..7 */
    int s;
    switch (type) {
    case RZTD_WATER: {                        /* faixas de onda */
        int wave = (y + ((x + variant) >> 2)) & 7;
        s = 10 + (wave == 0 ? 8 : 0) + (n >> 1);
        break;
    }
    case RZTD_SAND:                          /* granulado claro com pontos escuros */
        s = 18 + (n >> 1) - ((h & 0xF0u) == 0 ? 8 : 0);
        break;
    case RZTD_GRASS:                         /* tufos */
        s = 12 + n + (((h >> 8) & 7u) == 0 ? 8 : 0);
        break;
    case RZTD_FOREST:                        /* copas: manchas redondas */
    {
        int cx = (x & 7) - 3, cy = (y & 7) - 3;
        s = (cx * cx + cy * cy < 9 ? 16 : 6) + n;
        break;
    }
    case RZTD_ROCK:                          /* rachaduras escuras */
        s = 14 + n - ((((x * 3 + y * 5 + variant) % 11) == 0) ? 10 : 0);
        break;
    default:                                 /* neve */
        s = 22 + (n >> 1);
        break;
    }
    if (s < 0) s = 0;
    if (s > 31) s = 31;
    return s;
}

/* Atlas 256x256 paletizado: linha de blocos = tipo, coluna = variação.
   indices: 256*256 bytes. paletteRGB: 768 bytes. */
static void rztdGenerateAtlas(uint8_t* indices, uint8_t* paletteRGB) {
    int ty, tx, x, y;
    rztdBuildPalette(paletteRGB);
    memset(indices, 0, RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE);
    for (ty = 0; ty < RZTD_TYPE_COUNT; ++ty) {
        for (tx = 0; tx < 16; ++tx) {
            for (y = 0; y < 16; ++y) {
                for (x = 0; x < 16; ++x) {
                    int shade = rztdTexelShade(ty, tx, x, y);
                    indices[(ty * 16 + y) * RZTD_ATLAS_SIZE + tx * 16 + x] = (uint8_t)(ty * 32 + shade);
                }
            }
        }
    }
}

/* Bloco de cada quad pela média das 4 alturas; variação aleatória por quad. */
static void rztdPutU16(uint8_t* p, int v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

/* Grava indices (width*height, linha 0 em cima) + paleta (256 x RGB) como PCX
   ZSoft versão 5: 8 bits, 1 plano, RLE por linha e paleta VGA no fim.
   Devolve 1 se gravou tudo. */
static int rztdWritePcx(const char* path, const uint8_t* indices, int width, int height,
                        const uint8_t* paletteRGB) {
    uint8_t header[128];
    int bytesPerLine = (width + 1) & ~1;   /* o formato pede número par */
    int ok = 1, y;
    FILE* f = fopen(path, "wb");
    if (!f) return 0;

    memset(header, 0, sizeof(header));
    header[0] = 0x0A;               /* ZSoft */
    header[1] = 5;                  /* versão com paleta de 256 cores */
    header[2] = 1;                  /* RLE */
    header[3] = 8;                  /* bits por pixel */
    rztdPutU16(header + 8, width - 1);
    rztdPutU16(header + 10, height - 1);
    rztdPutU16(header + 12, 72);
    rztdPutU16(header + 14, 72);
    header[65] = 1;                 /* planos */
    rztdPutU16(header + 66, bytesPerLine);
    rztdPutU16(header + 68, 1);     /* paleta colorida */
    ok &= fwrite(header, 1, sizeof(header), f) == sizeof(header);

    for (y = 0; y < height && ok; ++y) {
        const uint8_t* row = indices + y * width;
        int x = 0;
        while (x < bytesPerLine) {
            uint8_t v = x < width ? row[x] : 0;
            uint8_t run[2];
            int n = 1;
            while (x + n < bytesPerLine && n < 63 && (x + n < width ? row[x + n] : 0) == v) ++n;
            if (n > 1 || v >= 0xC0) {
                run[0] = (uint8_t)(0xC0 | n);
                run[1] = v;
                ok &= fwrite(run, 1, 2, f) == 2;
            } else {
                ok &= fwrite(&v, 1, 1, f) == 1;
            }
            x += n;
        }
    }
    if (ok) {
        uint8_t marker = 0x0C;
        ok &= fwrite(&marker, 1, 1, f) == 1;
        ok &= fwrite(paletteRGB, 1, 768, f) == 768;
    }
    if (fclose(f) != 0) ok = 0;
    return ok;
}

static void rztdGenerateTileMap(const uint8_t* heights, uint8_t* tileMap) {
    int r, c;
    memset(tileMap, 0, 256 * 256);
    for (r = 0; r < 255; ++r) {
        for (c = 0; c < 255; ++c) {
            int avg = (heights[r * 256 + c] + heights[r * 256 + c + 1] +
                       heights[(r + 1) * 256 + c] + heights[(r + 1) * 256 + c + 1]) >> 2;
            uint32_t h = rztdHash(c, r, 99u);
            int type;
            if (avg <= 40)       type = RZTD_WATER;
            else if (avg <= 60)  type = RZTD_SAND;
            else if (avg <= 150) type = (rztdNoise(c >> 3, r >> 3, 5) > 0.6f) ? RZTD_FOREST : RZTD_GRASS;
            else if (avg <= 210) type = RZTD_ROCK;
            else                 type = RZTD_SNOW;
            tileMap[r * 256 + c] = (uint8_t)(type * 16 + (int)(h & 15u));
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Objetos de teste, no formato do legado: vértices 8.24 absolutos (z para    */
/* cima, 1.0 = 1 tile), polígonos convexos fechados repetindo o 1º índice.    */
/* As funções abaixo recebem (x, y, z) com y para cima e gravam (x, z, y).   */
/* Faces orientadas para fora (regra da mão direita), o que as deixa em        */
/* sentido anti-horário na tela quando vistas de fora: use RZ_CULL_CW.         */
/* ------------------------------------------------------------------------- */

#define RZTD_MESH_MAX_VERTS  64
#define RZTD_MESH_MAX_INDEX  256
#define RZTD_MAX_OBJECTS     200

typedef struct {
    uint32_t vertices[RZTD_MESH_MAX_VERTS * 3];
    uint16_t indices[RZTD_MESH_MAX_INDEX];
    float    pos[RZTD_MESH_MAX_VERTS][3];      /* cópia em float, para orientar as faces */
    int      vertexCount;
    int      indexCount;
} RztdMesh;

static uint32_t rztdFixed824(float v) {
    if (v < 0.0f) v = 0.0f;
    if (v > 255.99f) v = 255.99f;
    return (uint32_t)(v * 16777216.0f);
}

static int rztdVertex(RztdMesh* m, float x, float y, float z) {
    int i = m->vertexCount++;
    m->pos[i][0] = x; m->pos[i][1] = y; m->pos[i][2] = z;
    /* Formato do legado: (coluna, linha, altura) — z para cima */
    m->vertices[i * 3 + 0] = rztdFixed824(x);
    m->vertices[i * 3 + 1] = rztdFixed824(z);
    m->vertices[i * 3 + 2] = rztdFixed824(y);
    return i;
}

/* Polígono com os vértices ids[0..n-1]; a ordem é invertida se a normal de
   Newell não apontar para o mesmo lado de `out` (direção "para fora"). */
static void rztdPolygon(RztdMesh* m, const int* ids, int n, float ox, float oy, float oz) {
    float nx = 0.0f, ny = 0.0f, nz = 0.0f;
    int i;
    for (i = 0; i < n; ++i) {
        const float* a = m->pos[ids[i]];
        const float* b = m->pos[ids[(i + 1) % n]];
        nx += (a[1] - b[1]) * (a[2] + b[2]);
        ny += (a[2] - b[2]) * (a[0] + b[0]);
        nz += (a[0] - b[0]) * (a[1] + b[1]);
    }
    if (nx * ox + ny * oy + nz * oz >= 0.0f) {
        for (i = 0; i < n; ++i) m->indices[m->indexCount++] = (uint16_t)ids[i];
    } else {
        for (i = n - 1; i >= 0; --i) m->indices[m->indexCount++] = (uint16_t)ids[i];
    }
    m->indices[m->indexCount] = m->indices[m->indexCount - n];   /* fechamento */
    m->indexCount++;
}

/* Prisma de base regular com `sides` lados (4 = caixa), centro (cx, cz),
   do chão y0 até y1, com tampa plana ou telhado piramidal (roof > 0). */
static void rztdPrism(RztdMesh* m, float cx, float cz, float radius, int sides, float angle0,
                      float y0, float y1, float roof) {
    int base = m->vertexCount, i, top[16];
    for (i = 0; i < sides; ++i) {
        float a = angle0 + 6.2831853f * (float)i / (float)sides;
        float x = cx + radius * cosf(a), z = cz + radius * sinf(a);
        rztdVertex(m, x, y0, z);
        rztdVertex(m, x, y1, z);
    }
    for (i = 0; i < sides; ++i) {
        int j = (i + 1) % sides;
        int q[4] = { base + i * 2, base + j * 2, base + j * 2 + 1, base + i * 2 + 1 };
        float a = angle0 + 6.2831853f * ((float)i + 0.5f) / (float)sides;
        rztdPolygon(m, q, 4, cosf(a), 0.0f, sinf(a));
        top[i] = base + i * 2 + 1;
    }
    if (roof > 0.0f) {
        int apex = rztdVertex(m, cx, y1 + roof, cz);
        for (i = 0; i < sides; ++i) {
            int j = (i + 1) % sides;
            int t[3] = { top[i], top[j], apex };
            float a = angle0 + 6.2831853f * ((float)i + 0.5f) / (float)sides;
            rztdPolygon(m, t, 3, cosf(a), 1.0f, sinf(a));
        }
    } else {
        rztdPolygon(m, top, sides, 0.0f, 1.0f, 0.0f);
    }
}

/* Cubo girando em torno do eixo y (o host atualiza os vértices a cada frame). */
static void rztdSpinningCube(RztdMesh* m, float cx, float cy, float cz, float half, float angle) {
    static const int faces[6][4] = {
        { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 },
    };
    float ca = cosf(angle), sa = sinf(angle), cb = cosf(angle * 0.7f), sb = sinf(angle * 0.7f);
    int i;
    m->vertexCount = 0;
    m->indexCount = 0;
    for (i = 0; i < 8; ++i) {
        float x = (i & 4) ? half : -half, y = (i & 2) ? half : -half, z = (i & 1) ? half : -half;
        float y2 = y * cb - z * sb, z2 = y * sb + z * cb;            /* inclina em x */
        float x3 = x * ca + z2 * sa, z3 = -x * sa + z2 * ca;          /* gira em y */
        rztdVertex(m, cx + x3, cy + y2, cz + z3);
    }
    for (i = 0; i < 6; ++i) {
        float fx = 0.0f, fy = 0.0f, fz = 0.0f;
        int k;
        for (k = 0; k < 4; ++k) {
            const float* p = m->pos[faces[i][k]];
            fx += p[0] - cx; fy += p[1] - cy; fz += p[2] - cz;
        }
        rztdPolygon(m, faces[i], 4, fx, fy, fz);
    }
}

/* Escala: 1 unidade = 1 tile (célula). O carro tem ~0,85 de comprimento
   (~4 m, então um tile tem ~4,7 m); o resto segue a mesma proporção. */

/* Casas e torres espalhadas nas áreas de grama planas. heightScale = altura
   de uma unidade do heightmap em tiles. Devolve quantos objetos foram gerados. */
static int rztdGenerateBuildings(const uint8_t* heights, float heightScale,
                                 RztdMesh* meshes, uint32_t* colors, int maxObjects) {
    int count = 0, gx, gz;
    for (gz = 8; gz < 248 && count < maxObjects; gz += 6) {
        for (gx = 8; gx < 248 && count < maxObjects; gx += 6) {
            uint32_t h = rztdHash(gx, gz, 1234u);
            int cx = gx + (int)(h % 5u) - 2, cz = gz + (int)((h >> 3) % 5u) - 2;
            int lo = 255, hi = 0, x, z;
            for (z = cz - 2; z <= cz + 2; ++z) {
                for (x = cx - 2; x <= cx + 2; ++x) {
                    int v = heights[z * 256 + x];
                    if (v < lo) lo = v;
                    if (v > hi) hi = v;
                }
            }
            if (lo < 55 || hi > 165 || hi - lo > 20 || (h >> 20) % 3u == 0) continue;
            {
                RztdMesh* m = &meshes[count];
                float ground = (float)lo * heightScale;
                float slope = (float)(hi - lo) * heightScale;   /* paredes descem até o ponto mais baixo */
                m->vertexCount = 0;
                m->indexCount = 0;
                if ((h >> 12) % 5u == 0) {       /* torre octogonal (~6 m de largura, ~12 m) */
                    rztdPrism(m, (float)cx, (float)cz, 0.6f, 8, 0.0f, ground, ground + 2.6f + slope, 0.0f);
                    colors[count] = 0x00A8A8B0u;
                } else {                          /* casa (~8 m de lado), telhado de quatro águas */
                    rztdPrism(m, (float)cx, (float)cz, 1.1f, 4, 0.785398f, ground, ground + 0.65f + slope, 0.5f);
                    colors[count] = ((h >> 16) & 1u) ? 0x00C0A080u : 0x00D8C8A0u;
                }
                ++count;
            }
        }
    }
    return count;
}

/* ------------------------------------------------------------------------- */
/* Veículo (objeto que se move, com vértice-alvo)                              */
/* ------------------------------------------------------------------------- */

#define RZTD_VEHICLE_TARGET 16   /* vértice extra, fora dos polígonos: alvo da câmera */
#define RZTD_VEHICLE_LENGTH 0.85f
#define RZTD_VEHICLE_WIDTH  0.40f

/* Altura do terreno em tiles no ponto (x, z), interpolação bilinear. */
static float rztdGroundHeight(const uint8_t* heights, float heightScale, float x, float z) {
    int ix, iz;
    float fx, fz, h00, h10, h01, h11;
    if (x < 0.0f) x = 0.0f;
    if (z < 0.0f) z = 0.0f;
    if (x > 254.999f) x = 254.999f;
    if (z > 254.999f) z = 254.999f;
    ix = (int)x; iz = (int)z;
    fx = x - (float)ix; fz = z - (float)iz;
    h00 = heights[iz * 256 + ix];       h10 = heights[iz * 256 + ix + 1];
    h01 = heights[(iz + 1) * 256 + ix]; h11 = heights[(iz + 1) * 256 + ix + 1];
    return ((h00 + (h10 - h00) * fx) * (1.0f - fz) + (h01 + (h11 - h01) * fx) * fz) * heightScale;
}

/* Referencial do veículo apoiado no terreno: origem no chão, f = frente,
   u = cima, s = lado; inclinado conforme a altura do terreno nas 4 pontas. */
typedef struct {
    float o[3], f[3], u[3], s[3];
} RztdFrame;

static void rztdNormalize(float* v) {
    float inv = 1.0f / sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    v[0] *= inv; v[1] *= inv; v[2] *= inv;
}

static RztdFrame rztdVehicleFrame(const uint8_t* heights, float heightScale,
                                  float x, float z, float heading) {
    RztdFrame fr;
    float ch = cosf(heading), sh = sinf(heading);
    float hl = 0.5f * RZTD_VEHICLE_LENGTH, hw = 0.5f * RZTD_VEHICLE_WIDTH;
    float front = rztdGroundHeight(heights, heightScale, x + ch * hl, z + sh * hl);
    float back  = rztdGroundHeight(heights, heightScale, x - ch * hl, z - sh * hl);
    float right = rztdGroundHeight(heights, heightScale, x - sh * hw, z + ch * hw);
    float left  = rztdGroundHeight(heights, heightScale, x + sh * hw, z - ch * hw);
    fr.o[0] = x; fr.o[1] = 0.25f * (front + back + right + left); fr.o[2] = z;
    fr.f[0] = ch * 2.0f * hl;  fr.f[1] = front - back; fr.f[2] = sh * 2.0f * hl;
    fr.s[0] = -sh * 2.0f * hw; fr.s[1] = right - left; fr.s[2] = ch * 2.0f * hw;
    rztdNormalize(fr.f);
    rztdNormalize(fr.s);
    fr.u[0] = fr.s[1] * fr.f[2] - fr.s[2] * fr.f[1];      /* u = s x f */
    fr.u[1] = fr.s[2] * fr.f[0] - fr.s[0] * fr.f[2];
    fr.u[2] = fr.s[0] * fr.f[1] - fr.s[1] * fr.f[0];
    rztdNormalize(fr.u);
    return fr;
}

static int rztdFrameVertex(RztdMesh* m, const RztdFrame* fr, float lx, float ly, float lz) {
    return rztdVertex(m, fr->o[0] + fr->f[0] * lx + fr->u[0] * ly + fr->s[0] * lz,
                         fr->o[1] + fr->f[1] * lx + fr->u[1] * ly + fr->s[1] * lz,
                         fr->o[2] + fr->f[2] * lx + fr->u[2] * ly + fr->s[2] * lz);
}

/* Caixa (8 vértices) em coordenadas locais do veículo (x frente, y cima, z lado). */
static void rztdVehicleBox(RztdMesh* m, const RztdFrame* fr,
                           float x0, float x1, float y0, float y1, float z0, float z1, int skipBottom) {
    static const int faces[6][4] = {
        { 0, 1, 3, 2 }, { 4, 6, 7, 5 }, { 0, 4, 5, 1 }, { 2, 3, 7, 6 }, { 0, 2, 6, 4 }, { 1, 5, 7, 3 },
    };
    int base = m->vertexCount, i, k;
    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
    for (i = 0; i < 8; ++i) {
        rztdFrameVertex(m, fr, (i & 4) ? x1 : x0, (i & 2) ? y1 : y0, (i & 1) ? z1 : z0);
    }
    for (i = 0; i < 8; ++i) { cx += m->pos[base + i][0]; cy += m->pos[base + i][1]; cz += m->pos[base + i][2]; }
    cx *= 0.125f; cy *= 0.125f; cz *= 0.125f;
    for (i = 0; i < 6; ++i) {
        int ids[4];
        float fx = 0.0f, fy = 0.0f, fz = 0.0f, down;
        for (k = 0; k < 4; ++k) {
            ids[k] = base + faces[i][k];
            fx += m->pos[ids[k]][0] - cx; fy += m->pos[ids[k]][1] - cy; fz += m->pos[ids[k]][2] - cz;
        }
        down = fx * fr->u[0] + fy * fr->u[1] + fz * fr->u[2];   /* face de baixo: oposta a u */
        if (skipBottom && down < -0.001f) continue;
        rztdPolygon(m, ids, 4, fx, fy, fz);
    }
}

/* Veículo (~0,85 x 0,40 tile) em (x, z), virado para `heading` (radianos, no
   plano xz), apoiado e inclinado no terreno. Carroceria + cabine, e o vértice
   RZTD_VEHICLE_TARGET acima do centro do teto (alvo da câmera). */
static void rztdVehicle(RztdMesh* m, const uint8_t* heights, float heightScale,
                        float x, float z, float heading) {
    RztdFrame fr = rztdVehicleFrame(heights, heightScale, x, z, heading);
    float hl = 0.5f * RZTD_VEHICLE_LENGTH, hw = 0.5f * RZTD_VEHICLE_WIDTH;
    m->vertexCount = 0;
    m->indexCount = 0;
    rztdVehicleBox(m, &fr, -hl, hl, 0.03f, 0.20f, -hw, hw, 0);                  /* carroceria */
    rztdVehicleBox(m, &fr, -0.26f, 0.10f, 0.20f, 0.36f, -0.16f, 0.16f, 1);     /* cabine */
    rztdFrameVertex(m, &fr, 0.0f, 0.36f, 0.0f);                                 /* alvo */
}

/* Posição no percurso automático (volta em torno do centro da ilha) no
   instante t (em voltas, 1.0 = uma volta). Devolve x, z e o rumo. */
static void rztdVehiclePath(float t, float* x, float* z, float* heading) {
    float a = t * 6.2831853f;
    float r = 62.0f + 14.0f * sinf(a * 3.0f);            /* raio variável: contorna morros */
    float dr = 14.0f * 3.0f * cosf(a * 3.0f);             /* dr/da */
    float dx, dz;
    *x = 128.0f + r * cosf(a);
    *z = 128.0f + r * sinf(a);
    dx = dr * cosf(a) - r * sinf(a);
    dz = dr * sinf(a) + r * cosf(a);
    *heading = atan2f(dz, dx);
}

#endif
