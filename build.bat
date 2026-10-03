@echo off
rem Renderizeitor (OpenGL) - build direto com MinGW32 (g++ no PATH), sem make.
rem Gera renderizeitor.dll, librenderizeitor.dll.a, rz_test.exe e rz_viewer.exe.
setlocal

set FLAGS=-std=c++23 -O2 -msse2 -mfpmath=sse -ffp-contract=off -fno-exceptions -fno-rtti -Wall -Wextra -Wdouble-promotion -Iinclude
set SRCS=src\rz_api.cpp src\rz_math.cpp src\rz_terrain.cpp src\rz_camera.cpp src\rz_render.cpp src\rz_object.cpp src\rz_gl.cpp src\rz_platform_win32.cpp src\rz_pcx.cpp src\rz_texture.cpp src\rz_shadow.cpp src\rz_border.cpp src\rz_glass.cpp
set LINK=-static-libgcc -static-libstdc++ -static
set SYSLIBS=-lopengl32 -lgdi32

echo [1/3] renderizeitor.dll
g++ %FLAGS% -DRZ_BUILD_DLL -shared %SRCS% -o renderizeitor.dll %LINK% %SYSLIBS% -Wl,--out-implib,librenderizeitor.dll.a || goto :fail

echo [2/3] rz_test.exe
g++ %FLAGS% -DRZ_STATIC test\rz_test.cpp %SRCS% -o rz_test.exe %LINK% %SYSLIBS% || goto :fail

echo [3/3] rz_viewer.exe
gcc -std=c11 -O2 -msse2 -mfpmath=sse -Wall -Wextra -Iinclude test\rz_viewer.c -o rz_viewer.exe -L. -lrenderizeitor -lgdi32 -lwinmm -mwindows -static-libgcc || goto :fail

echo.
echo Dependencias da DLL (devem ser so DLLs do sistema: KERNEL32, USER32, GDI32, OPENGL32, msvcrt):
objdump -p renderizeitor.dll | findstr "DLL Name"
echo.
echo OK. Rode: rz_viewer.exe  ou  rz_test.exe -r reference_hashes.txt
exit /b 0

:fail
echo BUILD FALHOU
exit /b 1
