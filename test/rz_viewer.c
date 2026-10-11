/* rz_viewer: aplicação SDL2 que mostra o terreno e os objetos (versão OpenGL).
 *
 * Escrita em C e linkada contra a biblioteca estática. O host (este programa)
 * cria a janela SDL2 e a passa para rzCreateWindow, que cria nela o contexto
 * OpenGL; a cada frame, rzRender desenha e o host troca os buffers.
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
 * Uso: rz_viewer [heightmap.raw] [atlas.pcx]   (em qualquer ordem)
 *   heightmap.raw : 256x256, 1 byte por ponto; sem ele, gera a ilha do rz_test
 *   atlas.pcx     : PCX de 8 bits, pelo menos 256x256 (só o canto 256x256 é
 *                   usado); sem ele, grava o atlas procedural num PCX temporário
 *                   e carrega de lá
 */

#include <SDL.h>
#include <math.h>
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

/* Entrada: teclas seguradas (lidas por scancode) e foco da janela. */
static const Uint8* g_keys;              /* SDL_GetKeyboardState: 1 = segurada */
static int g_hasFocus = 1;

static int keyDown(SDL_Scancode sc) {
    return g_keys && g_keys[sc];
}

static void placeVehicle(void) {
    rztdVehicle(&g_vehicle, g_heights, HEIGHT_SCALE, g_carX, g_carZ, g_carHeading);
}

static void driveCar(int active) {
    int forward = active && keyDown(SDL_SCANCODE_W);
    int back    = active && keyDown(SDL_SCANCODE_S);
    int left    = active && keyDown(SDL_SCANCODE_A);
    int right   = active && keyDown(SDL_SCANCODE_D);
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

/* Lê as teclas da câmera (seguradas) uma vez por frame. */
static void handleInput(void) {
    if (!g_hasFocus) return;
    if (keyDown(SDL_SCANCODE_UP))       g_followHeight += 0.03f;
    if (keyDown(SDL_SCANCODE_DOWN))     g_followHeight -= 0.03f;
    if (keyDown(SDL_SCANCODE_PAGEUP))   g_followDist /= ZOOM_SPEED;     /* aproxima */
    if (keyDown(SDL_SCANCODE_PAGEDOWN)) g_followDist *= ZOOM_SPEED;     /* afasta */
    if (g_followHeight < -0.5f) g_followHeight = -0.5f;
    if (g_followHeight > 20.0f) g_followHeight = 20.0f;
    if (g_followDist < 1.0f)    g_followDist = 1.0f;
    if (g_followDist > 40.0f)   g_followDist = 40.0f;
}

static void updateTitle(SDL_Window* win, int32_t renderUs, int32_t fps) {
    char title[256];
    if (g_follow) {
        snprintf(title, sizeof(title),
                 "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  seguindo: corda %.1f  altura %.1f",
                 fps, renderUs / 1000, (renderUs % 1000) / 10,
                 (double)g_followDist, (double)g_followHeight);
    } else {
        snprintf(title, sizeof(title),
                 "Renderizeitor  |  %d fps  |  render %d.%02d ms  |  visao geral",
                 fps, renderUs / 1000, (renderUs % 1000) / 10);
    }
    SDL_SetWindowTitle(win, title);
}

/* Diretório temporário para os PCX gerados (com a barra no fim). */
static void tempDirPath(char* out, size_t cap) {
    const char* t = getenv("TMPDIR");
    if (!t || !t[0]) t = "/tmp";
    snprintf(out, cap, "%s/", t);
}

static void fatal(SDL_Window* win, const char* msg) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "rz_viewer", msg, win);
    fprintf(stderr, "rz_viewer: %s\n", msg);
}

int main(int argc, char** argv) {
    static uint8_t heightmap[256 * 256];
    SDL_Window* win = NULL;
    Uint64 freq, next, now, fpsStart;
    Sint64 frameTicks;
    int32_t err, running = 1, frames = 0, renderUsSum = 0;
    char pcxPath[1024], tempDir[512], wallPath[1024], carPath[1024];
    const char* rawPath = NULL;
    int i;

    /* Argumentos: .pcx é o atlas, o resto é o heightmap */
    pcxPath[0] = 0;
    for (i = 1; i < argc; ++i) {
        const char* a = argv[i];
        size_t len = strlen(a);
        if (len > 4 && SDL_strcasecmp(a + len - 4, ".pcx") == 0) {
            snprintf(pcxPath, sizeof(pcxPath), "%s", a);
        } else {
            rawPath = a;
        }
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "rz_viewer: SDL_Init falhou: %s\n", SDL_GetError());
        return 1;
    }

    if (rawPath) {
        if (!loadRaw(rawPath, heightmap)) {
            fatal(NULL, "Falha ao ler o heightmap (256x256 bytes).");
            SDL_Quit();
            return 1;
        }
    } else {
        rztdGenerateHeightmap(heightmap);
    }

    tempDirPath(tempDir, sizeof(tempDir));
    /* Texturas dos objetos: geradas e gravadas como PCX temporários */
    if (!rztdWriteObjectTextures(tempDir, wallPath, carPath, sizeof(wallPath))) {
        fatal(NULL, "Falha ao gravar as texturas de teste.");
        SDL_Quit();
        return 1;
    }
    if (!pcxPath[0]) {
        /* Sem atlas: grava o procedural num PCX temporário */
        static uint8_t atlas[RZTD_ATLAS_SIZE * RZTD_ATLAS_SIZE];
        static uint8_t palette[768];
        snprintf(pcxPath, sizeof(pcxPath), "%srz_atlas_teste.pcx", tempDir);
        rztdGenerateAtlas(atlas, palette);
        if (!rztdWritePcx(pcxPath, atlas, RZTD_ATLAS_SIZE, RZTD_ATLAS_SIZE, palette)) {
            fatal(NULL, "Falha ao gravar o atlas de teste.");
            SDL_Quit();
            return 1;
        }
    }

    /* Formato do framebuffer: escolhido na criação da janela */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    win = SDL_CreateWindow("Renderizeitor",
                           SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           FB_WIDTH, FB_HEIGHT, SDL_WINDOW_OPENGL);
    if (!win) {
        fprintf(stderr, "rz_viewer: SDL_CreateWindow falhou: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    g_keys = SDL_GetKeyboardState(NULL);

    /* Renderer: cria o contexto OpenGL na janela e desenha nela.
       Inicializa tudo sem olhar os retornos e checa uma vez no fim. */
    rzCreateWindow(win, &g_ctx);
    SDL_GL_SetSwapInterval(0);           /* sem v-sync: o laço abaixo controla o ritmo */
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
        int top = 0;
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
            char msg[1024 + 128];
            snprintf(msg, sizeof(msg), "%s falhou (erro %d)%s", where ? where : "Renderizeitor", (int)err,
                     err == RZ_ERR_GL ? ":\nOpenGL 3.3 indisponivel." : "");
            fatal(win, msg);
            if (g_ctx) rzDestroy(g_ctx);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return 1;
        }
    }

    /* Cubo: textura que não existe, de propósito, para mostrar o fallback.
       Fica depois da checagem e o erro esperado é descartado. */
    rzLoadObjectTexture(g_ctx, g_cubeId, "nao_existe.pcx");
    rzGetError(g_ctx, NULL);

    /* Laço com frame rate fixo: o carro e a câmera andam por frame, então o
       fps fixo mantém a velocidade constante. */
    freq = SDL_GetPerformanceFrequency();
    frameTicks = (Sint64)(freq / TARGET_FPS);
    next = SDL_GetPerformanceCounter();
    fpsStart = next;

    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_hasFocus = 1;
                else if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g_hasFocus = 0;
                break;
            case SDL_KEYDOWN:
                if (ev.key.repeat) break;              /* ignora auto-repeat */
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE:
                    running = 0;
                    break;
                case SDLK_c:
                    g_follow = !g_follow;
                    rzSetCameraTarget(g_ctx, g_follow ? g_vehicleId : -1, RZTD_VEHICLE_TARGET);
                    break;
                case SDLK_t:
                    g_textures = !g_textures;
                    rzSetTileMap(g_ctx, g_textures ? g_tileMap : NULL, 256, 256);
                    break;
                case SDLK_r:
                    g_followDist = FOLLOW_DIST_DEFAULT;
                    g_followHeight = FOLLOW_HEIGHT_DEFAULT;
                    break;
                default:
                    break;
                }
                break;
            default:
                break;
            }
        }
        if (!running) break;

        handleInput();

        /* O host move o carro e o cubo e avisa o renderer */
        driveCar(g_hasFocus);
        placeVehicle();
        rzUpdateObjectVertices(g_ctx, g_vehicleId, RZTD_VERTICES(&g_vehicle));
        rztdSteerVehicle(g_ctx, g_vehicleId, g_carSteer);
        rzSetObjectWheelSpin(g_ctx, g_vehicleId, g_carSpeed);  /* gira as rodas conforme a velocidade */
        rztdStepParticles(g_ctx, &g_particles, &g_vehicle);   /* fumaça e detritos */

        g_cubeAngle += 0.03f;
        rztdSpinningCube(&g_cube, g_cubePos[0], g_cubePos[1], g_cubePos[2], 0.6f, g_cubeAngle);
        rzUpdateObjectVertices(g_ctx, g_cubeId, RZTD_VERTICES(&g_cube));
        rzSetCameraFollow(g_ctx, g_followDist, g_followHeight, 0.08f);

        {
            Uint64 t0 = SDL_GetPerformanceCounter(), t1;
            rzRender(g_ctx);                 /* desenha no back buffer */
            SDL_GL_SwapWindow(win);          /* o host apresenta */
            t1 = SDL_GetPerformanceCounter();
            renderUsSum += (int32_t)((t1 - t0) * 1000000 / freq);
        }

        ++frames;
        now = SDL_GetPerformanceCounter();
        if (now - fpsStart >= freq / 2) {
            int32_t fps = (int32_t)((Uint64)frames * freq / (now - fpsStart));
            updateTitle(win, renderUsSum / frames, fps);
            frames = 0;
            renderUsSum = 0;
            fpsStart = now;
        }

        /* Espera o próximo tick; se atrasou demais, ressincroniza. */
        next += frameTicks;
        now = SDL_GetPerformanceCounter();
        if (now > next + (Uint64)frameTicks) next = now;
        while (now < next) {
            Sint64 remainMs = (Sint64)((next - now) * 1000 / freq);
            if (remainMs > 1) SDL_Delay((Uint32)(remainMs - 1));
            now = SDL_GetPerformanceCounter();
        }
    }

    if (g_ctx) rzDestroy(g_ctx);         /* apaga o contexto; a janela é nossa */
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
