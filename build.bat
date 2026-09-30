@echo off
setlocal
cd /d "%~dp0"
set MIN=..\_toolchain\mingw64\bin

if not exist "%MIN%\g++.exe" (
  echo MinGW toolchain not found at %MIN%
  exit /b 1
)

"%MIN%\windres.exe" force.rc -O coff -o force_res.o || goto :err
"%MIN%\g++.exe" -O2 -std=c++17 -municode -mwindows -static ^
  -finput-charset=UTF-8 -fwide-exec-charset=UTF-16LE ^
  main.cpp force_res.o -o ForceEjectUSB.exe ^
  -luser32 -lshell32 -lsetupapi -lcfgmgr32 || goto :err

echo Build OK: ForceEjectUSB.exe
exit /b 0

:err
echo Build FAILED
exit /b 1
