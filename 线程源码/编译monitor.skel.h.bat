@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

where wsl.exe >nul 2>nul
if errorlevel 1 (
    echo FAILED! wsl.exe not found. Install WSL first.
    exit /b 1
)

for /f "delims=" %%I in ('wsl.exe wslpath -a "%SCRIPT_DIR%"') do set "WSL_DIR=%%I"
if not defined WSL_DIR (
    echo FAILED! Unable to convert Windows path to WSL path.
    exit /b 1
)

echo Using WSL path: %WSL_DIR%
echo Building monitor.bpf.o and monitor.skel.h ...

wsl.exe bash -lc "set -e; cd \"%WSL_DIR%\"; clang -target bpf -g -O2 -D__TARGET_ARCH_arm64 -c monitor.bpf.c -o monitor.bpf.o; ./bpftool gen skeleton monitor.bpf.o > monitor.skel.h; ls -lh monitor.bpf.o monitor.skel.h"
if errorlevel 1 (
    echo FAILED! WSL build returned an error.
    exit /b 1
)

echo SUCCESS! Generated:
echo   %SCRIPT_DIR%\monitor.bpf.o
echo   %SCRIPT_DIR%\monitor.skel.h
exit /b 0
