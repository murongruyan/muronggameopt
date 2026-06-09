@echo off
setlocal EnableExtensions
chcp 65001 >nul

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "PROJECT_DIR=%%~fI"

set "BIN_DIR=%SCRIPT_DIR%"
set "APP_DIR="
for /d %%I in ("%PROJECT_DIR%\*") do (
    if exist "%%~fI\monitor.bpf.c" (
        if not defined APP_DIR set "APP_DIR=%%~fI"
    )
)
if not defined APP_DIR (
    for /d %%I in ("%PROJECT_DIR%\*") do (
        if exist "%%~fI\monitor.bpf.o" if exist "%%~fI\libbpf.so" (
            if not defined APP_DIR set "APP_DIR=%%~fI"
        )
    )
)

set "SRC_CPP=%BIN_DIR%\activity_diaodu.cpp"
set "SRC_CJSON=%BIN_DIR%\cJSON.c"
set "OUTPUT_BIN=%BIN_DIR%\activity_diaodu"
set "OBJ_CPP=%BIN_DIR%\activity_diaodu.o"
set "OBJ_CJSON=%BIN_DIR%\cJSON.o"
set "PREBUILT_BPF_OBJ=%APP_DIR%\monitor.bpf.o"
set "OUTPUT_BPF_OBJ=%BIN_DIR%\monitor.bpf.o"
set "LIBBPF_SO=%APP_DIR%\libbpf.so"
set "OUTPUT_CXX_SHARED=%BIN_DIR%\libc++_shared.so"

set "USER_NDK_ROOT=%NDK_ROOT%"
set "FOUND_NDK_ROOT="
if defined ANDROID_NDK_ROOT (
    if exist "%ANDROID_NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
        set "FOUND_NDK_ROOT=%ANDROID_NDK_ROOT%"
    )
)
if not defined FOUND_NDK_ROOT (
    if defined USER_NDK_ROOT (
        if exist "%USER_NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
            set "FOUND_NDK_ROOT=%USER_NDK_ROOT%"
        )
    )
)
if not defined FOUND_NDK_ROOT (
    if exist "C:\android-ndk-r27d-windows\huanjing\android-ndk-r30-beta1\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
        set "FOUND_NDK_ROOT=C:\android-ndk-r27d-windows\huanjing\android-ndk-r30-beta1"
    )
)
if not defined FOUND_NDK_ROOT (
    if exist "C:\android-ndk-r27d-windows\huanjing\android-ndk-r30\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
        set "FOUND_NDK_ROOT=C:\android-ndk-r27d-windows\huanjing\android-ndk-r30"
    )
)
if not defined FOUND_NDK_ROOT (
    if exist "C:\android-ndk-r27d-windows\android-ndk-r27d\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
        set "FOUND_NDK_ROOT=C:\android-ndk-r27d-windows\android-ndk-r27d"
    )
)
if not defined FOUND_NDK_ROOT (
    if exist "C:\android-ndk-r27d-windows\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe" (
        set "FOUND_NDK_ROOT=C:\android-ndk-r27d-windows"
    )
)
if not defined FOUND_NDK_ROOT (
    echo Build FAILED! Android NDK not found.
    echo Hint: set ANDROID_NDK_ROOT or NDK_ROOT first.
    goto :fail
)

set "NDK_ROOT=%FOUND_NDK_ROOT%"
set "CLANG=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe"
set "CLANGXX=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\bin\clang++.exe"
set "SYSROOT=%NDK_ROOT%\toolchains\llvm\prebuilt\windows-x86_64\sysroot"
set "CXX_SHARED_SO=%SYSROOT%\usr\lib\aarch64-linux-android\libc++_shared.so"

if not defined APP_DIR (
    echo Build FAILED! appopt source directory not found near %PROJECT_DIR%
    goto :fail
)
if not exist "%CLANG%" (
    echo Build FAILED! clang not found: %CLANG%
    goto :fail
)
if not exist "%CLANGXX%" (
    echo Build FAILED! clang++ not found: %CLANGXX%
    goto :fail
)
if not exist "%SRC_CPP%" (
    echo Build FAILED! source file not found: %SRC_CPP%
    goto :fail
)
if not exist "%SRC_CJSON%" (
    echo Build FAILED! source file not found: %SRC_CJSON%
    goto :fail
)
if not exist "%LIBBPF_SO%" (
    echo Build FAILED! libbpf.so not found: %LIBBPF_SO%
    goto :fail
)
if not exist "%CXX_SHARED_SO%" (
    echo Build FAILED! libc++_shared.so not found: %CXX_SHARED_SO%
    goto :fail
)
if not exist "%PREBUILT_BPF_OBJ%" (
    echo Build FAILED! monitor.bpf.o not found: %PREBUILT_BPF_OBJ%
    echo Hint: generate monitor.bpf.o first in the appopt source directory.
    goto :fail
)

del /q "%OBJ_CPP%" "%OBJ_CJSON%" "%OUTPUT_BIN%" "%OUTPUT_CXX_SHARED%" >nul 2>nul

copy /Y "%PREBUILT_BPF_OBJ%" "%OUTPUT_BPF_OBJ%" >nul
if errorlevel 1 (
    echo Build FAILED! unable to copy monitor.bpf.o
    goto :fail
)

echo [1/3] Compiling activity_diaodu.cpp
"%CLANGXX%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-I"%APP_DIR%" ^
-I"%APP_DIR%\bpf" ^
-std=c++17 -Wall -O3 -fPIE ^
-c "%SRC_CPP%" -o "%OBJ_CPP%"
if errorlevel 1 goto :fail

echo [2/3] Compiling cJSON.c
"%CLANG%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-Wall -O3 -fPIE ^
-c "%SRC_CJSON%" -o "%OBJ_CJSON%"
if errorlevel 1 goto :fail

echo [3/3] Linking activity_diaodu
"%CLANGXX%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-fPIE -pie ^
-L"%APP_DIR%" ^
-o "%OUTPUT_BIN%" "%OBJ_CPP%" "%OBJ_CJSON%" ^
-lbpf -lz -lc++_shared -lc -lm -ldl -latomic
if errorlevel 1 goto :fail

copy /Y "%CXX_SHARED_SO%" "%OUTPUT_CXX_SHARED%" >nul
if errorlevel 1 (
    echo Build FAILED! unable to copy libc++_shared.so
    goto :fail
)

del /q "%OBJ_CPP%" "%OBJ_CJSON%" >nul 2>nul

if exist "%OUTPUT_BIN%" (
    echo Build SUCCESS! Output file: %OUTPUT_BIN%
    exit /b 0
)

echo Build FAILED! Output file missing: %OUTPUT_BIN%
goto :fail

:fail
echo Build FAILED! Check errors above.
exit /b 1
