// Recursos de GPU do contexto (programas, FBO) e o frame.

#include "rz_internal.h"
#include "rz_shaders.h"

namespace rz {

namespace {

GLuint compileShader(GLenum type, const char* source) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
#if defined(RZ_DEBUG_SHADERS)
        char log[2048];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        __builtin_printf("shader: %s\n", log);
#endif
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}


constexpr GLint kUnitAtlas  = 0;    // unidade de textura do atlas (e da textura do objeto)
                                    // (1..3: shadow maps, kUnitShadowFirst)
// Frente = anti-horário na tela (como no software). Com a imagem espelhada na
// vertical (offscreen), o anti-horário visual vira horário para o OpenGL.
GLenum frontFaceWinding(const RzContext* ctx) {
    return ctx->windowed ? GL_CCW : GL_CW;
}

} // namespace

// Compila e liga um programa (também usado por rz_border.cpp)
GLuint linkProgram(const char* vertexSource, const char* fragmentSource) {
    GLuint vs = compileShader(GL_VERTEX_SHADER, vertexSource);
    GLuint fs = compileShader(GL_FRAGMENT_SHADER, fragmentSource);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
#if defined(RZ_DEBUG_SHADERS)
        char log[2048];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        __builtin_printf("link: %s\n", log);
#endif
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void initFogUniforms(GLuint program, FogUniforms& u) {
    u.on    = glGetUniformLocation(program, "uFogOn");
    u.start = glGetUniformLocation(program, "uFogStart");
    u.end   = glGetUniformLocation(program, "uFogEnd");
    u.color = glGetUniformLocation(program, "uFogColor");
    u.eye   = glGetUniformLocation(program, "uEye");
}

// Neblina do frame: liga só seguindo um alvo; cor = fundo
void bindFog(const RzContext* ctx, const FogUniforms& u) {
    glUniform1i(u.on, ctx->fogOn ? 1 : 0);
    glUniform1f(u.start, ctx->fogStart * ctx->cellSize);
    glUniform1f(u.end, ctx->fogEnd * ctx->cellSize);
    glUniform3f(u.color, ctx->backgroundR, ctx->backgroundG, ctx->backgroundB);
    glUniform3f(u.eye, ctx->eyePos.x, ctx->eyePos.y, ctx->eyePos.z);
}

bool createRenderer(RzContext* ctx) {
    TerrainProgram& t = ctx->terrainProgram;
    t.program = linkProgram(kTerrainVertexShader, kTerrainFragmentShader);
    if (!t.program) return false;
    t.viewProj = glGetUniformLocation(t.program, "uViewProj");
    t.atlas    = glGetUniformLocation(t.program, "uAtlas");
    t.textured = glGetUniformLocation(t.program, "uTextured");
    t.filter   = glGetUniformLocation(t.program, "uFilter");
    t.shading  = glGetUniformLocation(t.program, "uShading");
    t.ambient   = glGetUniformLocation(t.program, "uShadowLight");
    const GLint shadowDim = glGetUniformLocation(t.program, "uShadowDim");
    glUseProgram(t.program);
    glUniform1i(t.atlas, kUnitAtlas);
    glUniform1f(t.shading, kTexturedShading);
    glUniform1f(t.ambient, kShadowLight);
    glUniform1f(shadowDim, kShadowTexturedDim);
    initShadowUniforms(t.program, t.shadow);
    initFogUniforms(t.program, t.fog);

    ObjectProgram& o = ctx->objectProgram;
    o.program = linkProgram(kObjectVertexShader, kObjectFragmentShader);
    if (!o.program) return false;
    o.viewProj = glGetUniformLocation(o.program, "uViewProj");
    o.texture  = glGetUniformLocation(o.program, "uTexture");
    o.filter   = glGetUniformLocation(o.program, "uFilter");
    o.texSize  = glGetUniformLocation(o.program, "uTexSize");
    o.maxLevel = glGetUniformLocation(o.program, "uMaxLevel");
    o.ambient   = glGetUniformLocation(o.program, "uShadowLight");
    glUseProgram(o.program);
    glUniform1i(o.texture, kUnitAtlas);
    glUniform1f(o.ambient, kShadowLight);
    initShadowUniforms(o.program, o.shadow);
    initFogUniforms(o.program, o.fog);
    if (!createFallbackTexture(ctx)) return false;

    DepthProgram& d = ctx->depthProgram;
    d.program = linkProgram(kDepthVertexShader, kDepthFragmentShader);
    if (!d.program) return false;
    d.lightViewProj = glGetUniformLocation(d.program, "uLightViewProj");
    if (!createShadowMaps(ctx)) return false;
    if (!createBorder(ctx)) return false;
    if (!createGlassProgram(ctx)) return false;

    // Malha do terreno: buffer de tamanho fixo, preenchido em buildTerrainMesh
    glGenVertexArrays(1, &ctx->terrainVao);
    glGenBuffers(1, &ctx->terrainVbo);
    glBindVertexArray(ctx->terrainVao);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->terrainVbo);
    glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(kTriangleCount) * 3 * GLsizeiptr(sizeof(TerrainVertex)),
                 nullptr, GL_STATIC_DRAW);
    const GLsizei stride = sizeof(TerrainVertex);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 4, GL_UNSIGNED_BYTE, GL_TRUE, stride,
                          reinterpret_cast<void*>(offsetof(TerrainVertex, color)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_FALSE, stride,
                          reinterpret_cast<void*>(offsetof(TerrainVertex, u)));
    glEnableVertexAttribArray(2);
    glBindVertexArray(0);

    if (!ctx->windowed) {
        glGenFramebuffers(1, &ctx->fbo);
        glGenRenderbuffers(1, &ctx->colorRb);
        glGenRenderbuffers(1, &ctx->depthRb);
    }
    return resizeTargets(ctx) && glGetError() == GL_NO_ERROR;
}

// (Re)aloca o FBO no modo offscreen; no modo janela, só o viewport muda.
bool resizeTargets(RzContext* ctx) {
    if (ctx->windowed) return true;
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->colorRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, ctx->width, ctx->height);
    glBindRenderbuffer(GL_RENDERBUFFER, ctx->depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, ctx->width, ctx->height);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ctx->colorRb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ctx->depthRb);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void destroyRenderer(RzContext* ctx) {
    if (ctx->terrainProgram.program) glDeleteProgram(ctx->terrainProgram.program);
    if (ctx->objectProgram.program) glDeleteProgram(ctx->objectProgram.program);
    if (ctx->terrainVao) glDeleteVertexArrays(1, &ctx->terrainVao);
    if (ctx->terrainVbo) glDeleteBuffers(1, &ctx->terrainVbo);
    if (ctx->fbo) glDeleteFramebuffers(1, &ctx->fbo);
    if (ctx->colorRb) glDeleteRenderbuffers(1, &ctx->colorRb);
    if (ctx->depthRb) glDeleteRenderbuffers(1, &ctx->depthRb);
    if (ctx->atlasTex) glDeleteTextures(1, &ctx->atlasTex);
    if (ctx->fallbackTex) glDeleteTextures(1, &ctx->fallbackTex);
    if (ctx->depthProgram.program) glDeleteProgram(ctx->depthProgram.program);
    destroyShadowMaps(ctx);
    destroyBorder(ctx);
    if (ctx->glassProgram.program) glDeleteProgram(ctx->glassProgram.program);
}

// Estado do sampler do atlas e das texturas dos objetos para o filtro atual.
// Ampliação sempre nearest.
void applyTextureFilter(RzContext* ctx) {
    if (ctx->fallbackTex) applyFilter2D(ctx->fallbackTex, ctx->fallbackSize, ctx->textureFilter);
    for (const Object& o : ctx->objects) {
        if (o.alive && o.texture) applyFilter2D(o.texture, o.textureSize, ctx->textureFilter);
    }
    if (!ctx->atlasTex) return;
    GLenum minFilter = GL_NEAREST_MIPMAP_NEAREST;     // mipmap; também o do dither (textureLod)
    switch (ctx->textureFilter) {
        case RZ_FILTER_NEAREST:    minFilter = GL_NEAREST; break;
        case RZ_FILTER_MIP_LINEAR: minFilter = GL_NEAREST_MIPMAP_LINEAR; break;
        case RZ_FILTER_TRILINEAR:  minFilter = GL_LINEAR_MIPMAP_LINEAR; break;
        default: break;
    }
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx->atlasTex);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GLint(minFilter));
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
}

void renderFrame(RzContext* ctx) {
    const Mat4 viewProj = updateCamera(ctx);

    prepareObjects(ctx);
    renderShadowMaps(ctx);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->windowed ? 0 : ctx->fbo);
    glViewport(0, 0, ctx->width, ctx->height);
    glClearColor(ctx->backgroundR, ctx->backgroundG, ctx->backgroundB, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glFrontFace(frontFaceWinding(ctx));

    if (ctx->hasTerrain()) {
        const TerrainProgram& t = ctx->terrainProgram;
        const bool textured = ctx->hasAtlas && ctx->hasTileMap;
        glUseProgram(t.program);
        glUniformMatrix4fv(t.viewProj, 1, GL_TRUE, viewProj.e);
        bindShadowMaps(ctx, t.shadow);
        bindFog(ctx, t.fog);
        glUniform1i(t.textured, textured ? 1 : 0);
        glUniform1i(t.filter, ctx->textureFilter);
        glActiveTexture(GL_TEXTURE0 + GLenum(kUnitAtlas));
        glBindTexture(GL_TEXTURE_2D_ARRAY, textured ? ctx->atlasTex : 0);

        glEnable(GL_CULL_FACE);                   // terreno: só a face de cima
        glCullFace(GL_BACK);
        glBindVertexArray(ctx->terrainVao);
        glDrawArrays(GL_TRIANGLES, 0, kTriangleCount * 3);
        drawSkirt(ctx);                           // continuação além do mapa (com neblina)
    }

    drawObjects(ctx, viewProj);
    drawWheels(ctx, viewProj);                    // rodas: programa do terreno, sem textura
    drawGlass(ctx, viewProj);                     // vidro: filtro + brilho, depois do opaco
    drawWall(ctx, viewProj);                      // semitransparente: por último

    if (ctx->windowed) {
        platformSwapBuffers(ctx->platform);
    } else {
        // Cópia para o buffer do host: BGRA em bytes = RGBQUAD; a imagem já
        // foi desenhada espelhada, então a linha 0 lida é a de cima.
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(0, 0, ctx->width, ctx->height, GL_BGRA, GL_UNSIGNED_BYTE, ctx->hostPixels);
    }
}

} // namespace rz
