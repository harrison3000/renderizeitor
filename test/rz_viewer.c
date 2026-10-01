/* rz_viewer: aplicação Win32 que mostra o terreno e os objetos (versão OpenGL).
 *
 * Escrita em C e linkada contra a DLL (import lib), como o código legado faria.
 * A DLL cria uma janela filha OpenGL dentro da janela deste programa
 * (rzCreateWindow) e desenha direto nela.
 *
 *   Seta cima / baixo       sobe / desce a câmera (pitch da órbita; seguindo: altura)
 *   Seta direita / esquerda acelera / freia a rotação (passando de zero, inverte)
 *   PgUp / PgDn             aproxima / afasta a câmera (seguindo: comprimento da corda)
 *   T                       liga / desliga as texturas
 *   F                       filtro: nearest -> mipmap -> mip+dither -> mip linear -> trilinear
 *   O                       mostra / esconde os objetos
 *   C                       câmera segue o veículo / volta ao centro do terreno
 *   Espaço                  para / retoma a rotação
 *   R                       volta aos valores iniciais
 *   Esc                     sai
 *
 * Uso: rz_viewer.exe [heightmap.raw]   (256x256, 1 byte por ponto)
 * Sem argumento, gera a mesma ilha procedural do rz_test.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "renderizeitor.h"
#include "rz_testdata.h"

#define FB_WIDTH   1280
#define FB_HEIGHT  720
#define TARGET_FPS 60

#define PITCH_DEFAULT   35.0f
#define PITCH_SPEED     0.75f          /* graus por frame com a tecla segurada */
#define STEP_DEFAULT    (1 << 22)      /* 1024 frames por volta */
#define STEP_ACCEL      (1 << 17)      /* variação do passo por frame */
#define STEP_MAX        (1 << 25)      /* 128 frames por volta */
#define ZOOM_SPEED      1.02f          /* fator por frame com a tecla segurada */

static RzContext* g_ctx;

static float   g_pitch  = PITCH_DEFAULT;
static int32_t g_step   = STEP_DEFAULT;
static int32_t g_paused = 0;
static float   g_zoom   = 1.0f;

/* Câmera de perseguição */
#define FOLLOW_DIST_DEFAULT   12.0f
#define FOLLOW_HEIGHT_DEFAULT 3.0f
static float   g_followDist   = FOLLOW_DIST_DEFAULT;
static float   g_followHeight = FOLLOW_HEIGHT_DEFAULT;
static int32_t g_textures = 1;
static int32_t g_filter = RZ_FILTER_MIP_DITHER;

/* Objetos: construções paradas e um cubo que este "host" gira a cada frame */
static int32_t  g_objectIds[RZTD_MAX_OBJECTS + 2];
static int32_t  g_objectCount = 0;
static int32_t  g_objectsVisible = 1;
static int32_t  g_cubeId = -1;
static RztdMesh g_cube;
static float    g_cubePos[3];
static float    g_cubeAngle = 0.0f;

/* Veículo: primeiro objeto, anda pela ilha e é o alvo da câmera */
static int32_t  g_vehicleId = -1;
static RztdMesh g_vehicle;
static int32_t  g_vehicleFrame = 0;
static int32_t  g_follow = 1;
static const uint8_t* g_heights;
#define HEIGHT_SCALE RZTD_HEIGHT_SCALE   /* padrão de rzSetTerrainScale */

static void placeVehicle(void) {
    float x, z, heading;
    rztdVehiclePath((float)g_vehicleFrame / 2048.0f, &x, &z, &heading);
    rztdVehicle(&g_vehicle, x, rztdGroundHeight(g_heights, HEIGHT_SCALE, x, z), z, heading);
}
static uint8_t g_tileMap[256 * 256];

static int loadRaw(const char* path, uint8_t* out) {
    FILE* f = fopen(path, "rb");
    size_t n;
    if (!f) return 0;
    n = fread(out, 1, 256 * 256, f);
    fclose(f);
    return n == 256 * 256;
}

/* ------------------------------------------------------------------------- */
/* Janela                                                                     */
/* ------------------------------------------------------------------------- */

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            DestroyWindow(hwnd);
        } else if (wp == VK_SPACE && !(lp & (1 << 30))) {   /* ignora auto-repeat */
            g_paused = !g_paused;
        } else if (wp == 'C' && !(lp & (1 << 30))) {
            g_follow = !g_follow;
            rzSetCameraTarget(g_ctx, g_follow ? g_vehicleId : -1, RZTD_VEHICLE_TARGET);
        } else if (wp == 'O' && !(lp & (1 << 30))) {
            int32_t i;
            g_objectsVisible = !g_objectsVisible;
            for (i = 0; i < g_objectCount; ++i) rzSetObjectVisible(g_ctx, g_objectIds[i], g_objectsVisible);
        } else if (wp == 'T' && !(lp & (1 << 30))) {
            g_textures = !g_textures;
            rzSetTileMap(g_ctx, g_textures ? g_tileMap : NULL, 256, 256);
        } else if (wp == 'F' && !(lp & (1 << 30))) {
            g_filter = (g_filter + 1) % 5;
            rzSetTextureFilter(g_ctx, g_filter);
        } else if (wp == 'R') {
            g_pitch = PITCH_DEFAULT;
            g_step = STEP_DEFAULT;
            g_zoom = 1.0f;
            g_followDist = FOLLOW_DIST_DEFAULT;
            g_followHeight = FOLLOW_HEIGHT_DEFAULT;
            g_paused = 0;
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;                       /* a área toda é da janela filha OpenGL */
    case WM_DESTROY:
        if (g_ctx) {                    /* antes da janela filha ser destruída junto */
            rzDestroy(g_ctx);
            g_ctx = NULL;
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

static int keyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

/* Lê as setas (seguradas) uma vez por frame. */
static void handleInput(HWND hwnd) {
    if (GetForegroundWindow() != hwnd) return;

    if (g_follow) {
        if (keyDown(VK_UP))    g_followHeight += 0.1f;
        if (keyDown(VK_DOWN))  g_followHeight -= 0.1f;
        if (keyDown(VK_PRIOR)) g_followDist /= ZOOM_SPEED;
        if (keyDown(VK_NEXT))  g_followDist *= ZOOM_SPEED;
        if (g_followHeight < -2.0f) g_followHeight = -2.0f;
        if (g_followHeight > 40.0f) g_followHeight = 40.0f;
        if (g_followDist < 3.0f)    g_followDist = 3.0f;
        if (g_followDist > 80.0f)   g_followDist = 80.0f;
    } else {
        if (keyDown(VK_UP))   g_pitch += PITCH_SPEED;
        if (keyDown(VK_DOWN)) g_pitch -= PITCH_SPEED;
        if (g_pitch < 0.0f)  g_pitch = 0.0f;
        if (g_pitch > 89.0f) g_pitch = 89.0f;

        if (keyDown(VK_PRIOR)) g_zoom /= ZOOM_SPEED;     /* Page Up: aproxima */
        if (keyDown(VK_NEXT))  g_zoom *= ZOOM_SPEED;     /* Page Down: afasta */
        if (g_zoom < 0.02f) g_zoom = 0.02f;
        if (g_zoom > 4.0f)  g_zoom = 4.0f;
    }

    if (keyDown(VK_RIGHT)) g_step += STEP_ACCEL;
    if (keyDown(VK_LEFT))  g_step -= STEP_ACCEL;
    if (g_step >  STEP_MAX) g_step =  STEP_MAX;
    if (g_step < -STEP_MAX) g_step = -STEP_MAX;
}

static void updateTitle(HWND hwnd, int32_t renderUs, int32_t fps) {
    char title[256];
    /* Só inteiros: wsprintf não formata float. */
    int32_t pitch10 = (int32_t)(g_pitch * 10.0f);
    /* voltas por segundo × 100 = passo · fps · 100 / 2^32 */
    int64_t rps100 = ((int64_t)g_step * TARGET_FPS * 100) / 4294967296LL;
    int32_t absRps = (int32_t)(rps100 < 0 ? -rps100 : rps100);
    int32_t zoom100 = (int32_t)(g_zoom * 100.0f);
    int32_t rope10 = (int32_t)(g_followDist * 10.0f);
    int32_t height10 = (int32_t)(g_followHeight * 10.0f);
    static const char* const filterNames[5] = { "nearest", "mipmap", "mip+dither", "mip linear", "trilinear" };
    const char* filter = filterNames[g_filter];

    if (g_follow) {
        wsprintfA(title,
                  "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  seguindo: corda %d.%d  altura %s%d.%d  |  %s",
                  fps, renderUs / 1000, (renderUs % 1000) / 10,
                  rope10 / 10, rope10 % 10,
                  height10 < 0 ? "-" : "", (height10 < 0 ? -height10 : height10) / 10,
                  (height10 < 0 ? -height10 : height10) % 10, filter);
    } else {
        wsprintfA(title,
                  "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  pitch %d.%d  |  dist %d.%02d  |  rotacao %s%d.%02d voltas/s  |  %s%s",
                  fps, renderUs / 1000, (renderUs % 1000) / 10,
                  pitch10 / 10, pitch10 % 10,
                  zoom100 / 100, zoom100 % 100,
                  rps100 < 0 ? "-" : "", absRps / 100, absRps % 100,
                  filter, g_paused ? " (pausado)" : "");
    }
    SetWindowTextA(hwnd, title);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdLine, int show) {
    static uint8_t heightmap[256 * 256];
    WNDCLASSA wc;
    RECT rc;
    HWND hwnd;
    /* WS_CLIPCHILDREN: o GDI desta janela não pinta por cima da janela filha OpenGL */
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    LARGE_INTEGER freq, now, next, t0, t1, fpsStart;
    LONGLONG frameTicks;
    int32_t err, running = 1, frames = 0, renderUsSum = 0;
    char path[MAX_PATH];
    (void)prev;

    /* Heightmap: argumento opcional (aspas removidas) ou procedural */
    lstrcpynA(path, cmdLine ? cmdLine : "", MAX_PATH);
    {
        char* p = path;
        size_t len;
        while (*p == ' ' || *p == '"') ++p;
        len = strlen(p);
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '"')) p[--len] = 0;
        if (len > 0) {
            if (!loadRaw(p, heightmap)) {
                MessageBoxA(NULL, "Falha ao ler o heightmap (256x256 bytes).", "rz_viewer", MB_ICONERROR);
                return 1;
            }
        } else {
            rztdGenerateHeightmap(heightmap);
        }
    }

    /* Janela do "host" */
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "RenderizeitorViewer";
    RegisterClassA(&wc);

    rc.left = 0; rc.top = 0; rc.right = FB_WIDTH; rc.bottom = FB_HEIGHT;
    AdjustWindowRect(&rc, style, FALSE);
    hwnd = CreateWindowA(wc.lpszClassName, "Renderizeitor", style,
                         CW_USEDEFAULT, CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                         NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);

    /* Renderer: janela filha OpenGL ocupando toda a área cliente */
    err = rzCreateWindow(hwnd, 0, 0, FB_WIDTH, FB_HEIGHT, &g_ctx);
    if (err == RZ_OK) err = rzSetHeightmap(g_ctx, heightmap, 256, 256);
    if (err == RZ_OK) {
        /* Atlas e mapa de blocos procedurais (no lugar do PCX, por enquanto) */
        static uint8_t atlas[RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE];
        static uint8_t palette[768];
        rztdGenerateAtlas(atlas, palette);
        rztdGenerateTileMap(heightmap, g_tileMap);
        err = rzSetTileAtlas(g_ctx, atlas, RZTD_ATLAS_SIZE, RZTD_ATLAS_SIZE, palette);
        if (err == RZ_OK) err = rzSetTileMap(g_ctx, g_tileMap, 256, 256);
    }
    if (err == RZ_OK) {
        /* Primeiro objeto: o veículo */
        g_heights = heightmap;
        placeVehicle();
        err = rzCreateObject(g_ctx, g_vehicle.vertices, g_vehicle.vertexCount,
                             g_vehicle.indices, g_vehicle.indexCount, &g_vehicleId);
        if (err == RZ_OK) {
            rzSetObjectColor(g_ctx, g_vehicleId, 0x003070E0u);
            rzSetObjectCulling(g_ctx, g_vehicleId, RZ_CULL_CW);
            rzSetCameraTarget(g_ctx, g_vehicleId, RZTD_VEHICLE_TARGET);
            g_objectIds[g_objectCount++] = g_vehicleId;
        }
    }
    if (err == RZ_OK) {
        static RztdMesh buildings[RZTD_MAX_OBJECTS];
        static uint32_t colors[RZTD_MAX_OBJECTS];
        const float heightScale = RZTD_HEIGHT_SCALE;   /* padrão de rzSetTerrainScale */
        int count = rztdGenerateBuildings(heightmap, heightScale, buildings, colors, RZTD_MAX_OBJECTS);
        int i, top = 0;
        for (i = 0; i < count && err == RZ_OK; ++i) {
            int32_t id;
            err = rzCreateObject(g_ctx, buildings[i].vertices, buildings[i].vertexCount,
                                 buildings[i].indices, buildings[i].indexCount, &id);
            if (err == RZ_OK) {
                rzSetObjectColor(g_ctx, id, colors[i]);
                rzSetObjectCulling(g_ctx, id, RZ_CULL_CW);
                g_objectIds[g_objectCount++] = id;
            }
        }
        for (i = 0; i < 256 * 256; ++i) if (heightmap[i] > top) top = heightmap[i];
        g_cubePos[0] = 128.0f; g_cubePos[1] = (float)top * heightScale + 8.0f; g_cubePos[2] = 128.0f;
        rztdSpinningCube(&g_cube, g_cubePos[0], g_cubePos[1], g_cubePos[2], 4.0f, 0.0f);
        if (err == RZ_OK) err = rzCreateObject(g_ctx, g_cube.vertices, g_cube.vertexCount,
                                               g_cube.indices, g_cube.indexCount, &g_cubeId);
        if (err == RZ_OK) {
            rzSetObjectColor(g_ctx, g_cubeId, 0x00E04030u);
            rzSetObjectCulling(g_ctx, g_cubeId, RZ_CULL_CW);
            g_objectIds[g_objectCount++] = g_cubeId;
        }
    }
    if (err != RZ_OK) {
        MessageBoxA(hwnd, err == RZ_ERR_GL ? "Falha ao inicializar o OpenGL 3.3."
                                           : "Falha ao inicializar o Renderizeitor.",
                    "rz_viewer", MB_ICONERROR);
        DestroyWindow(hwnd);
        return 1;
    }


    /* Laço com frame rate fixo: a rotação é por frame, então o fps fixo
       mantém a velocidade constante. */
    timeBeginPeriod(1);
    QueryPerformanceFrequency(&freq);
    frameTicks = freq.QuadPart / TARGET_FPS;
    QueryPerformanceCounter(&next);
    fpsStart = next;

    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = 0; break; }
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
        if (!running) break;

        handleInput(hwnd);

        /* O host move o cubo e avisa o renderer */
        ++g_vehicleFrame;
        placeVehicle();
        rzUpdateObjectVertices(g_ctx, g_vehicleId, g_vehicle.vertices);

        g_cubeAngle += 0.03f;
        rztdSpinningCube(&g_cube, g_cubePos[0], g_cubePos[1], g_cubePos[2], 4.0f, g_cubeAngle);
        rzUpdateObjectVertices(g_ctx, g_cubeId, g_cube.vertices);
        rzSetCameraPitch(g_ctx, g_pitch);
        rzSetCameraDistance(g_ctx, g_zoom);
        rzSetCameraFollow(g_ctx, g_followDist, g_followHeight, 0.08f);
        rzSetRotationStep(g_ctx, g_paused ? 0 : g_step);

        QueryPerformanceCounter(&t0);
        rzRender(g_ctx);                /* desenha e apresenta (SwapBuffers) */
        QueryPerformanceCounter(&t1);
        renderUsSum += (int32_t)((t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart);

        ++frames;
        QueryPerformanceCounter(&now);
        if (now.QuadPart - fpsStart.QuadPart >= freq.QuadPart / 2) {
            int32_t fps = (int32_t)((LONGLONG)frames * freq.QuadPart / (now.QuadPart - fpsStart.QuadPart));
            updateTitle(hwnd, renderUsSum / frames, fps);
            frames = 0;
            renderUsSum = 0;
            fpsStart = now;
        }

        /* Espera o próximo tick; se atrasou demais, ressincroniza. */
        next.QuadPart += frameTicks;
        QueryPerformanceCounter(&now);
        if (now.QuadPart > next.QuadPart + frameTicks) next = now;
        while (now.QuadPart < next.QuadPart) {
            LONGLONG remainMs = (next.QuadPart - now.QuadPart) * 1000 / freq.QuadPart;
            if (remainMs > 1) Sleep((DWORD)(remainMs - 1));
            QueryPerformanceCounter(&now);
        }
    }

    timeEndPeriod(1);
    if (g_ctx) rzDestroy(g_ctx);
    return 0;
}
