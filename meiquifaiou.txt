# Renderizeitor (OpenGL 3.3) - build com MinGW32 (GCC 12+) no Windows.
#
#   make            -> DLL + import lib + rz_test.exe + rz_viewer.exe
#   make dll        -> só a DLL
#   make test       -> só o executável de teste (offscreen)
#   make viewer     -> aplicação Win32 (rz_viewer.exe, janela filha OpenGL)
#   make check      -> roda o teste e mostra as dependências da DLL
#
# Linux (offscreen via EGL, ex.: llvmpipe): ./build_linux.sh

CXX     ?= g++
CC      ?= gcc
AR      ?= ar
OBJDUMP ?= objdump

EXE ?= .exe

CXXFLAGS = -std=c++23 -O2 -msse2 -mfpmath=sse -ffp-contract=off \
           -fno-exceptions -fno-rtti \
           -Wall -Wextra -Wdouble-promotion \
           -Iinclude

STATIC_LINK = -static-libgcc -static-libstdc++ -static
SYSLIBS     = -lopengl32 -lgdi32

SRCS = src/rz_api.cpp src/rz_math.cpp src/rz_terrain.cpp src/rz_camera.cpp \
       src/rz_render.cpp src/rz_object.cpp src/rz_gl.cpp src/rz_platform_win32.cpp src/rz_pcx.cpp src/rz_texture.cpp src/rz_shadow.cpp src/rz_border.cpp src/rz_glass.cpp
HDRS = include/renderizeitor.h src/rz_internal.h src/rz_gl.h src/rz_platform.h src/rz_shaders.h

OBJ_STATIC = $(patsubst src/%.cpp,build/static/%.o,$(SRCS))
OBJ_DLL    = $(patsubst src/%.cpp,build/dll/%.o,$(SRCS))

.PHONY: all dll test viewer check clean

all: dll test viewer

dll: renderizeitor.dll
test: rz_test$(EXE)
viewer: rz_viewer$(EXE)

build/static/%.o: src/%.cpp $(HDRS)
	@mkdir -p build/static
	$(CXX) $(CXXFLAGS) -DRZ_STATIC -c $< -o $@

librenderizeitor_static.a: $(OBJ_STATIC)
	$(AR) rcs $@ $^

build/dll/%.o: src/%.cpp $(HDRS)
	@mkdir -p build/dll
	$(CXX) $(CXXFLAGS) -DRZ_BUILD_DLL -c $< -o $@

renderizeitor.dll: $(OBJ_DLL)
	$(CXX) -shared -o $@ $^ $(STATIC_LINK) $(SYSLIBS) -Wl,--out-implib,librenderizeitor.dll.a

rz_test$(EXE): test/rz_test.cpp test/rz_testdata.h librenderizeitor_static.a $(HDRS)
	$(CXX) $(CXXFLAGS) -DRZ_STATIC test/rz_test.cpp librenderizeitor_static.a -o $@ $(STATIC_LINK) $(SYSLIBS)

VIEWER_CFLAGS = -std=c11 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Iinclude

rz_viewer$(EXE): test/rz_viewer.c test/rz_testdata.h renderizeitor.dll include/renderizeitor.h
	$(CC) $(VIEWER_CFLAGS) test/rz_viewer.c -o $@ -L. -lrenderizeitor -lgdi32 -lwinmm -mwindows -static-libgcc

check: all
	./rz_test$(EXE) -r reference_hashes.txt
	$(OBJDUMP) -p renderizeitor.dll | grep "DLL Name"

clean:
	rm -rf build librenderizeitor_static.a renderizeitor.dll librenderizeitor.dll.a rz_test$(EXE) rz_viewer$(EXE)
