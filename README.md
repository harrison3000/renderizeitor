# Renderizeitor

OpenGL based renderer for the TeREp2 project

**Vibe coded using Claude**, kept in a separate repo so LLM and flesy code dont mix, if you dont want to use LLM generated things just dont use it 👍

Builds as a static library on Linux (OpenGL 3.3 core). The viewer uses SDL2 for the window and input; see `build_linux.sh` or the `Makefile`.

## Improvements

 * Higher resolution (haven't tested high-res textures yet, they should work for cars, for the ground its easy to implement)
 * Realtime shadows
 * World border visuals 