@echo off
setlocal EnableExtensions

rem 用法：build.bat [thread]
rem   build.bat          编译完整用户态版（含 FAS 与动态调频）
rem   build.bat thread   编译纯线程版：不编 FAS 4 个实现单元与动态调频，改用 *_thread 桩
set "THREAD_ONLY=0"
if /I "%~1"=="thread" set "THREAD_ONLY=1"

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
set "FAS_DIR=%BIN_DIR%\fas"
set "SRC_FAS_CONFIG=%FAS_DIR%\fas_config.cpp"
set "SRC_FAS_SAMPLING=%FAS_DIR%\fas_sampling.cpp"
set "SRC_FAS_TARGET=%FAS_DIR%\fas_target.cpp"
set "SRC_FAS_ENGINE=%FAS_DIR%\fas_engine.cpp"
set "OUTPUT_BIN=%BIN_DIR%\activity_diaodu"
set "OBJ_CPP=%BIN_DIR%\activity_diaodu.o"
set "OBJ_CJSON=%BIN_DIR%\cJSON.o"
set "PREBUILT_BPF_OBJ=%APP_DIR%\monitor.bpf.o"
set "OUTPUT_BPF_OBJ=%BIN_DIR%\monitor.bpf.o"
set "LIBBPF_SO=%APP_DIR%\libbpf.so"

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
if not exist "%PREBUILT_BPF_OBJ%" (
    echo Build FAILED! monitor.bpf.o not found: %PREBUILT_BPF_OBJ%
    echo Hint: generate monitor.bpf.o first in the appopt source directory.
    goto :fail
)

del /q "%OBJ_CPP%" "%OBJ_CJSON%" "%OUTPUT_BIN%" >nul 2>nul

copy /Y "%PREBUILT_BPF_OBJ%" "%OUTPUT_BPF_OBJ%" >nul
if errorlevel 1 (
    echo Build FAILED! unable to copy monitor.bpf.o
    goto :fail
)

set "THREAD_DEFINE="
if "%THREAD_ONLY%"=="1" set "THREAD_DEFINE=-DMURONG_THREAD_ONLY"

if "%THREAD_ONLY%"=="1" (echo [1/3] Compiling activity_diaodu.cpp [纯线程版]) else (echo [1/3] Compiling activity_diaodu.cpp)
"%CLANGXX%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-I"%APP_DIR%" ^
-I"%APP_DIR%\bpf" ^
-I"%BIN_DIR%" ^
-I"%FAS_DIR%" ^
%THREAD_DEFINE% ^
-std=c++17 -Wall -O3 -fPIE -Wno-unused-function ^
-c "%SRC_CPP%" -o "%OBJ_CPP%"
if errorlevel 1 goto :fail

echo [2/3] Compiling cJSON.c
"%CLANG%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-Wall -O3 -fPIE ^
-c "%SRC_CJSON%" -o "%OBJ_CJSON%"
if errorlevel 1 goto :fail

rem 两个版本的中间产物分开命名，避免复用上一次的 .o 造成「改了源码却编出旧行为」
set "STUB_OBJ=%FAS_DIR%\fas_thread_stub.o"

if "%THREAD_ONLY%"=="1" goto :thread_link

set "FAS_OBJS=%FAS_DIR%\fas_config.o %FAS_DIR%\fas_sampling.o %FAS_DIR%\fas_target.o %FAS_DIR%\fas_engine.o"

echo [3/8] Compiling fas_config.cpp
"%CLANGXX%" --target=aarch64-linux-android29 --sysroot="%SYSROOT%" -I"%APP_DIR%" -I"%BIN_DIR%" -I"%FAS_DIR%" -std=c++17 -Wall -O3 -fPIE -c "%SRC_FAS_CONFIG%" -o "%FAS_DIR%\fas_config.o"
if errorlevel 1 goto :fail

echo [4/8] Compiling fas_sampling.cpp
"%CLANGXX%" --target=aarch64-linux-android29 --sysroot="%SYSROOT%" -I"%APP_DIR%" -I"%BIN_DIR%" -I"%FAS_DIR%" -std=c++17 -Wall -O3 -fPIE -c "%SRC_FAS_SAMPLING%" -o "%FAS_DIR%\fas_sampling.o"
if errorlevel 1 goto :fail

echo [5/8] Compiling fas_target.cpp
"%CLANGXX%" --target=aarch64-linux-android29 --sysroot="%SYSROOT%" -I"%APP_DIR%" -I"%BIN_DIR%" -I"%FAS_DIR%" -std=c++17 -Wall -O3 -fPIE -c "%SRC_FAS_TARGET%" -o "%FAS_DIR%\fas_target.o"
if errorlevel 1 goto :fail

echo [6/8] Compiling fas_engine.cpp
"%CLANGXX%" --target=aarch64-linux-android29 --sysroot="%SYSROOT%" -I"%APP_DIR%" -I"%BIN_DIR%" -I"%FAS_DIR%" -std=c++17 -Wall -O3 -fPIE -c "%SRC_FAS_ENGINE%" -o "%FAS_DIR%\fas_engine.o"
if errorlevel 1 goto :fail

goto :do_link

:thread_link
rem 纯线程版：只编 FAS 空桩，不编 4 个实现单元，也不编 activity_dynamic_tuning.inc
set "FAS_OBJS=%STUB_OBJ%"
echo [3/4] Compiling fas_thread_stub.cpp (纯线程版桩)
"%CLANGXX%" --target=aarch64-linux-android29 --sysroot="%SYSROOT%" -I"%APP_DIR%" -I"%BIN_DIR%" -I"%FAS_DIR%" -DMURONG_THREAD_ONLY -std=c++17 -Wall -O3 -fPIE -c "%FAS_DIR%\fas_thread_stub.cpp" -o "%STUB_OBJ%"
if errorlevel 1 goto :fail

:do_link
echo [7/7] Linking activity_diaodu
"%CLANGXX%" ^
--target=aarch64-linux-android29 ^
--sysroot="%SYSROOT%" ^
-fPIE -pie ^
-static-libstdc++ ^
-L"%APP_DIR%" ^
-o "%OUTPUT_BIN%" "%OBJ_CPP%" "%OBJ_CJSON%" %FAS_OBJS% ^
-lbpf -lz -lc -lm -ldl -latomic
if errorlevel 1 goto :fail

del /q "%OBJ_CPP%" "%OBJ_CJSON%" >nul 2>nul
if defined FAS_OBJS del /q %FAS_OBJS% >nul 2>nul

if exist "%OUTPUT_BIN%" (
    echo Build SUCCESS! Output file: %OUTPUT_BIN%
    exit /b 0
)

echo Build FAILED! Output file missing: %OUTPUT_BIN%
goto :fail

:fail
echo Build FAILED! Check errors above.
exit /b 1
