@echo off
REM Build the 32-bit hook DLL + loader with MinGW-w64 (i686).
REM Run from a shell where i686-w64-mingw32-gcc is on the PATH.
REM
REM Copyright (c) 2026 Bruce Armstrong. MIT License - see the LICENSE file.

setlocal
cd /d "%~dp0"

i686-w64-mingw32-gcc -O2 -shared -o pbsystreehook.dll pbsystreehook.c -lcomctl32 -luser32 -lgdi32 -Wl,--kill-at
if errorlevel 1 goto :fail

i686-w64-mingw32-gcc -O2 -o pbsystreehookload.exe pbsystreehookload.c -luser32
if errorlevel 1 goto :fail

echo.
echo Built pbsystreehook.dll and pbsystreehookload.exe
goto :eof

:fail
echo.
echo BUILD FAILED
exit /b 1
