#!/bin/sh
# Renderizeitor (OpenGL) no Linux.
#   rz_test    : modo offscreen (rzCreate) via EGL surfaceless (llvmpipe serve).
#   rz_viewer  : aplicação SDL2 com janela (rzCreateCurrent).
# Precisa de libEGL + driver Mesa, de libGL e de SDL2 (pkg-config sdl2).
set -e

FLAGS="-std=c++23 -O2 -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Wdouble-promotion -Iinclude"
SRCS="src/rz_api.cpp src/rz_math.cpp src/rz_terrain.cpp src/rz_camera.cpp src/rz_render.cpp src/rz_object.cpp src/rz_gl.cpp src/rz_platform_egl.cpp src/rz_pcx.cpp src/rz_texture.cpp src/rz_shadow.cpp src/rz_border.cpp src/rz_glass.cpp src/rz_wheels.cpp src/rz_sprites.cpp"

echo "[1/3] librenderizeitor_static.a"
rm -rf build/static && mkdir -p build/static
OBJS=""
for s in $SRCS; do
    o="build/static/$(basename "$s" .cpp).o"
    g++ $FLAGS -c "$s" -o "$o"
    OBJS="$OBJS $o"
done
ar rcs librenderizeitor_static.a $OBJS

echo "[2/3] rz_test (offscreen, EGL)"
g++ $FLAGS test/rz_test.cpp librenderizeitor_static.a -ldl -o rz_test

echo "[3/3] rz_viewer (SDL2)"
gcc -std=c11 -O2 -Wall -Wextra -Iinclude $(pkg-config --cflags sdl2) \
    test/rz_viewer.c librenderizeitor_static.a \
    $(pkg-config --libs sdl2) -lstdc++ -ldl -lm -o rz_viewer

echo "OK: ./rz_test -o . -n 64 -e 16   |   ./rz_viewer"
