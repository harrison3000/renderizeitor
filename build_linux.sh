#!/bin/sh
# Renderizeitor (OpenGL) no Linux: só o modo offscreen (rzCreate), via EGL
# surfaceless. Precisa de libEGL e de um driver Mesa (llvmpipe serve).
set -e
FLAGS="-std=c++23 -O2 -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Wdouble-promotion -Iinclude"
SRCS="src/rz_api.cpp src/rz_math.cpp src/rz_terrain.cpp src/rz_camera.cpp src/rz_render.cpp src/rz_object.cpp src/rz_gl.cpp src/rz_platform_egl.cpp src/rz_pcx.cpp src/rz_texture.cpp src/rz_shadow.cpp src/rz_border.cpp src/rz_glass.cpp"
g++ $FLAGS -DRZ_STATIC test/rz_test.cpp $SRCS -ldl -o rz_test
echo "OK: ./rz_test -o . -n 64 -e 16"
