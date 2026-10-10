# Renderizeitor (OpenGL 3.3) - build no Linux (GCC).
#
#   make            -> biblioteca estática + rz_test + rz_viewer
#   make lib        -> só a biblioteca estática
#   make test       -> só o executável de teste (offscreen, via EGL)
#   make viewer     -> aplicação SDL2 (rz_viewer, janela OpenGL)
#   make check      -> roda o teste offscreen
#
# Precisa de libEGL + driver Mesa (llvmpipe serve), libGL e SDL2.

CXX ?= g++
CC  ?= gcc
AR  ?= ar

CXXFLAGS = -std=c++23 -O2 -ffp-contract=off \
           -fno-exceptions -fno-rtti \
           -Wall -Wextra -Wdouble-promotion \
           -Iinclude

SDL_CFLAGS = $(shell pkg-config --cflags sdl2)
SDL_LIBS   = $(shell pkg-config --libs sdl2)

SRCS = src/rz_api.cpp src/rz_math.cpp src/rz_terrain.cpp src/rz_camera.cpp \
       src/rz_render.cpp src/rz_object.cpp src/rz_gl.cpp src/rz_platform_egl.cpp src/rz_pcx.cpp src/rz_texture.cpp src/rz_shadow.cpp src/rz_border.cpp src/rz_glass.cpp src/rz_wheels.cpp src/rz_sprites.cpp
HDRS = include/renderizeitor.h src/rz_internal.h src/rz_gl.h src/rz_platform.h src/rz_shaders.h

OBJ_STATIC = $(patsubst src/%.cpp,build/static/%.o,$(SRCS))

.PHONY: all lib test viewer check clean

all: lib test viewer

lib: librenderizeitor_static.a
test: rz_test
viewer: rz_viewer

build/static/%.o: src/%.cpp $(HDRS)
	@mkdir -p build/static
	$(CXX) $(CXXFLAGS) -c $< -o $@

librenderizeitor_static.a: $(OBJ_STATIC)
	$(AR) rcs $@ $^

rz_test: test/rz_test.cpp test/rz_testdata.h librenderizeitor_static.a $(HDRS)
	$(CXX) $(CXXFLAGS) test/rz_test.cpp librenderizeitor_static.a -ldl -o $@

VIEWER_CFLAGS = -std=c11 -O2 -Wall -Wextra -Iinclude

rz_viewer: test/rz_viewer.c test/rz_testdata.h librenderizeitor_static.a include/renderizeitor.h
	$(CC) $(VIEWER_CFLAGS) $(SDL_CFLAGS) test/rz_viewer.c librenderizeitor_static.a $(SDL_LIBS) -lstdc++ -ldl -lm -o $@

check: test
	./rz_test -r reference_hashes.txt

clean:
	rm -rf build librenderizeitor_static.a rz_test rz_viewer
