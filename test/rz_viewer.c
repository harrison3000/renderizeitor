/* rz_viewer: aplicação Win32 que mostra o terreno e os objetos (versão OpenGL).
 *
 * Escrita em C e linkada contra a DLL (import lib), como o código legado faria.
 * A DLL cria uma janela filha OpenGL dentro da janela deste programa
 * (rzCreateWindow) e desenha direto nela.
 *
 *   W / S                   acelera / freia e dá ré no carro
 *   A / D                   vira o carro para a esquerda / direita
 *   Seta cima / baixo       sobe / desce a câmera (altura acima do carro)
 *   PgUp / PgDn             aproxima / afasta a câmera (comprimento da corda)
 *   T                       liga / desliga as texturas
 *   C                       câmera segue o veículo / visão geral do terreno
 *   R                       volta a câmera aos valores iniciais
 *   Esc                     sai
 *
 * Uso: rz_viewer.exe [heightmap.raw] [atlas.pcx]   (em qualquer ordem)
 *   heightmap.raw : 256x256, 1 byte por ponto; sem ele, gera a ilha do rz_test
 *   atlas.pcx     : PCX de 8 bits, pelo menos 256x256 (só o canto 256x256 é
 *                   usado); sem ele, grava o atlas procedural em
 *                   %TEMP%\rz_atlas_teste.pcx e carrega de lá
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

#define ZOOM_SPEED      1.02f          /* fator por frame com a tecla segurada */

static RzContext* g_ctx;

/* Câmera de perseguição */
#define FOLLOW_DIST_DEFAULT   3.0f      /* na escala do carro (~0,85 tile) */
#define FOLLOW_HEIGHT_DEFAULT 1.0f
static float   g_followDist   = FOLLOW_DIST_DEFAULT;
static float   g_followHeight = FOLLOW_HEIGHT_DEFAULT;
static int32_t g_textures = 1;

/* Objetos: construções paradas e um cubo que este "host" gira a cada frame */
static int32_t  g_cubeId = -1;
static RztdMesh g_cube;
static float    g_cubePos[3];
static float    g_cubeAngle = 0.0f;

/* Veículo: primeiro objeto, dirigido com WASD e alvo da câmera.
   Sem física: velocidade com aceleração e atrito, e o carro só acompanha a
   altura e a inclinação do terreno. Unidades: tiles e frames (60 fps). */
static int32_t  g_vehicleId = -1;
static RztdMesh g_vehicle;
static RztdParticles g_particles;   /* sprites do carro (array compacto, como o legado) */
static int32_t  g_follow = 1;
static const uint8_t* g_heights;
static float    g_carX, g_carZ, g_carHeading, g_carSpeed, g_carSteer;
#define HEIGHT_SCALE RZTD_HEIGHT_SCALE   /* padrão de rzSetTerrainScale */

#define CAR_MAX_SPEED    0.06f            /* tiles/frame: ~3,6 tiles/s, ~60 km/h */
#define CAR_MAX_REVERSE  0.025f
#define CAR_ACCEL        0.0015f
#define CAR_BRAKE        0.003f
#define CAR_FRICTION     0.97f            /* sem acelerador, a velocidade decai */
#define CAR_TURN_RATE    0.045f           /* rad/frame na velocidade máxima */
#define CAR_STEER_MAX    0.45f            /* rad: esterçamento visual das rodas dianteiras */
#define CAR_STEER_RATE   0.06f            /* rad/frame para chegar lá */

static void placeVehicle(void) {
    rztdVehicle(&g_vehicle, g_heights, HEIGHT_SCALE, g_carX, g_carZ, g_carHeading);
}

static int keyDown(int vk);

static void driveCar(int active) {
    int forward = active && keyDown('W');
    int back    = active && keyDown('S');
    int left    = active && keyDown('A');
    int right   = active && keyDown('D');
    float turn, nx, nz;

    if (forward)    g_carSpeed += (g_carSpeed < 0.0f) ? CAR_BRAKE : CAR_ACCEL;
    else if (back)  g_carSpeed -= (g_carSpeed > 0.0f) ? CAR_BRAKE : CAR_ACCEL;
    else            g_carSpeed *= CAR_FRICTION;
    if (g_carSpeed >  CAR_MAX_SPEED)   g_carSpeed =  CAR_MAX_SPEED;
    if (g_carSpeed < -CAR_MAX_REVERSE) g_carSpeed = -CAR_MAX_REVERSE;
    if (g_carSpeed > -0.0005f && g_carSpeed < 0.0005f && !forward && !back) g_carSpeed = 0.0f;

    /* Vira proporcional à velocidade (parado não gira; de ré, inverte) */
    turn = CAR_TURN_RATE * (g_carSpeed / CAR_MAX_SPEED);
    if (left)  g_carHeading -= turn;
    if (right) g_carHeading += turn;

    /* Rodas dianteiras: vão até o máximo enquanto A/D estiver apertado, e voltam */
    {
        float want = left ? CAR_STEER_MAX : (right ? -CAR_STEER_MAX : 0.0f);
        if (g_carSteer < want) { g_carSteer += CAR_STEER_RATE; if (g_carSteer > want) g_carSteer = want; }
        if (g_carSteer > want) { g_carSteer -= CAR_STEER_RATE; if (g_carSteer < want) g_carSteer = want; }
    }

    nx = g_carX + cosf(g_carHeading) * g_carSpeed;
    nz = g_carZ + sinf(g_carHeading) * g_carSpeed;
    if (nx < 1.0f || nx > 254.0f || nz < 1.0f || nz > 254.0f) {   /* borda do mapa */
        g_carSpeed = 0.0f;
        return;
    }
    g_carX = nx;
    g_carZ = nz;
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
        } else if (wp == 'C' && !(lp & (1 << 30))) {     /* ignora auto-repeat */
            g_follow = !g_follow;
            rzSetCameraTarget(g_ctx, g_follow ? g_vehicleId : -1, RZTD_VEHICLE_TARGET);
        } else if (wp == 'T' && !(lp & (1 << 30))) {
            g_textures = !g_textures;
            rzSetTileMap(g_ctx, g_textures ? g_tileMap : NULL, 256, 256);
        } else if (wp == 'R') {
            g_followDist = FOLLOW_DIST_DEFAULT;
            g_followHeight = FOLLOW_HEIGHT_DEFAULT;
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

/* Lê as teclas da câmera (seguradas) uma vez por frame. */
static void handleInput(HWND hwnd) {
    if (GetForegroundWindow() != hwnd) return;
    if (keyDown(VK_UP))    g_followHeight += 0.03f;
    if (keyDown(VK_DOWN))  g_followHeight -= 0.03f;
    if (keyDown(VK_PRIOR)) g_followDist /= ZOOM_SPEED;     /* Page Up: aproxima */
    if (keyDown(VK_NEXT))  g_followDist *= ZOOM_SPEED;     /* Page Down: afasta */
    if (g_followHeight < -0.5f) g_followHeight = -0.5f;
    if (g_followHeight > 20.0f) g_followHeight = 20.0f;
    if (g_followDist < 1.0f)    g_followDist = 1.0f;
    if (g_followDist > 40.0f)   g_followDist = 40.0f;
}

static void updateTitle(HWND hwnd, int32_t renderUs, int32_t fps) {
    char title[256];
    /* Só inteiros: wsprintf não formata float. */
    int32_t rope10 = (int32_t)(g_followDist * 10.0f);
    int32_t height10 = (int32_t)(g_followHeight * 10.0f);
    int32_t absHeight10 = height10 < 0 ? -height10 : height10;

    if (g_follow) {
        wsprintfA(title,
                  "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  seguindo: corda %d.%d  altura %s%d.%d",
                  fps, renderUs / 1000, (renderUs % 1000) / 10,
                  rope10 / 10, rope10 % 10,
                  height10 < 0 ? "-" : "", absHeight10 / 10, absHeight10 % 10);
    } else {
        wsprintfA(title, "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  visao geral",
                  fps, renderUs / 1000, (renderUs % 1000) / 10);
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
    char pcxPath[MAX_PATH], tempDir[MAX_PATH], wallPath[MAX_PATH], carPath[MAX_PATH];
    const char* rawPath = NULL;
    DWORD tempLen;
    int i;
    (void)prev;
    (void)cmdLine;

    /* Argumentos (já separados pelo runtime): .pcx é o atlas, o resto é o heightmap */
    pcxPath[0] = 0;
    for (i = 1; i < __argc; ++i) {
        const char* a = __argv[i];
        size_t len = strlen(a);
        if (len > 4 && lstrcmpiA(a + len - 4, ".pcx") == 0) lstrcpynA(pcxPath, a, MAX_PATH);
        else rawPath = a;
    }
    if (rawPath) {
        if (!loadRaw(rawPath, heightmap)) {
            MessageBoxA(NULL, "Falha ao ler o heightmap (256x256 bytes).", "rz_viewer", MB_ICONERROR);
            return 1;
        }
    } else {
        rztdGenerateHeightmap(heightmap);
    }
    tempLen = GetTempPathA(MAX_PATH, tempDir);
    if (tempLen == 0 || tempLen + 24 >= MAX_PATH) lstrcpyA(tempDir, ".\\");
    /* Texturas dos objetos: geradas e gravadas como PCX temporários */
    if (!rztdWriteObjectTextures(tempDir, wallPath, carPath, MAX_PATH)) {
        MessageBoxA(NULL, "Falha ao gravar as texturas de teste.", "rz_viewer", MB_ICONERROR);
        return 1;
    }
    if (!pcxPath[0]) {
        /* Sem atlas: grava o procedural num PCX temporário */
        static uint8_t atlas[RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE];
        static uint8_t palette[768];
        lstrcpyA(pcxPath, tempDir);
        lstrcatA(pcxPath, "rz_atlas_teste.pcx");
        rztdGenerateAtlas(atlas, palette);
        if (!rztdWritePcx(pcxPath, atlas, RZTD_ATLAS_SIZE, RZTD_ATLAS_SIZE, palette)) {
            MessageBoxA(NULL, "Falha ao gravar o atlas de teste.", "rz_viewer", MB_ICONERROR);
            return 1;
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

    /* Renderer: janela filha OpenGL ocupando toda a área cliente. Inicializa
       tudo sem olhar os retornos e checa uma vez no fim (rzGetError). */
    rzCreateWindow(hwnd, 0, 0, FB_WIDTH, FB_HEIGHT, &g_ctx);
    rzSetHeightmap(g_ctx, heightmap, 256, 256);
    rzLoadTileAtlas(g_ctx, pcxPath);                 /* atlas do PCX */
    rztdGenerateTileMap(heightmap, g_tileMap);       /* mapa de blocos procedural */
    rzSetTileMap(g_ctx, g_tileMap, 256, 256);

    /* Primeiro objeto: o veículo, no início do percurso automático */
    g_heights = heightmap;
    rztdVehiclePath(0.0f, &g_carX, &g_carZ, &g_carHeading);
    g_carSpeed = 0.0f;
    placeVehicle();
    rztdCreateObject(g_ctx, &g_vehicle, RZTD_CAR_V, RZTD_CAR_ROOF, &g_vehicleId);
    rzLoadObjectTexture(g_ctx, g_vehicleId, carPath);
    rztdSetVehicleWheels(g_ctx, g_vehicleId);
    rztdAddVehicleAntenna(g_ctx, g_vehicleId);
    rzSetCameraTarget(g_ctx, g_vehicleId, RZTD_VEHICLE_TARGET);

    {
        static RztdMesh buildings[RZTD_MAX_OBJECTS];
        static uint32_t colors[RZTD_MAX_OBJECTS];
        const float heightScale = RZTD_HEIGHT_SCALE;   /* padrão de rzSetTerrainScale */
        int count = rztdGenerateBuildings(heightmap, heightScale, buildings, colors, RZTD_MAX_OBJECTS);
        int i, top = 0;
        for (i = 0; i < count; ++i) {
            int32_t id;
            rztdCreateObject(g_ctx, &buildings[i], RZTD_WALL_V, RZTD_WALL_ROOF, &id);
            rzLoadObjectTexture(g_ctx, id, wallPath);
        }
        for (i = 0; i < 256 * 256; ++i) if (heightmap[i] > top) top = heightmap[i];
        g_cubePos[0] = 128.0f; g_cubePos[1] = (float)top * heightScale + 2.0f; g_cubePos[2] = 128.0f;
        rztdSpinningCube(&g_cube, g_cubePos[0], g_cubePos[1], g_cubePos[2], 0.6f, 0.0f);
        rztdCreateObject(g_ctx, &g_cube, RZTD_WALL_V, RZTD_WALL_ROOF, &g_cubeId);
    }

    {
        const char* where = NULL;
        err = rzGetError(g_ctx, &where);    /* g_ctx NULL: erro do rzCreateWindow */
        if (err != RZ_OK) {
            char msg[MAX_PATH + 128];
            wsprintfA(msg, "%s falhou (erro %d)%s%s", where ? where : "Renderizeitor", (int)err,
                      err == RZ_ERR_GL ? ":\nOpenGL 3.3 indisponivel." : "",
                      (where && !lstrcmpA(where, "rzLoadTileAtlas")) ? ":\n" : "");
            if (where && !lstrcmpA(where, "rzLoadTileAtlas")) lstrcatA(msg, pcxPath);
            MessageBoxA(hwnd, msg, "rz_viewer", MB_ICONERROR);
            DestroyWindow(hwnd);
            return 1;
        }
    }

    /* Cubo: textura que não existe, de propósito, para mostrar o fallback.
       Fica depois da checagem e o erro esperado é descartado. */
    rzLoadObjectTexture(g_ctx, g_cubeId, "nao_existe.pcx");
    rzGetError(g_ctx, NULL);

    /* Laço com frame rate fixo: o carro e a câmera andam por frame, então o
       fps fixo mantém a velocidade constante. */
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

        /* O host move o carro e o cubo e avisa o renderer */
        driveCar(GetForegroundWindow() == hwnd);
        placeVehicle();
        rzUpdateObjectVertices(g_ctx, g_vehicleId, RZTD_VERTICES(&g_vehicle));
        rztdSteerVehicle(g_ctx, g_vehicleId, g_carSteer);
        rzSetObjectWheelSpin(g_ctx, g_vehicleId, g_carSpeed);  /* gira as rodas conforme a velocidade */
        rztdStepParticles(g_ctx, &g_particles, &g_vehicle);   /* fumaça e detritos */

        g_cubeAngle += 0.03f;
        rztdSpinningCube(&g_cube, g_cubePos[0], g_cubePos[1], g_cubePos[2], 0.6f, g_cubeAngle);
        rzUpdateObjectVertices(g_ctx, g_cubeId, RZTD_VERTICES(&g_cube));
        rzSetCameraFollow(g_ctx, g_followDist, g_followHeight, 0.08f);

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
