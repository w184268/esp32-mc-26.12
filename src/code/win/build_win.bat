REM SPDX-License-Identifier: GPL-3.0-only
REM Attribution and modification details are documented in ../../NOTICE.

@echo off
REM ============================================================
REM ESP32MC Windows Build Script
REM 
REM 零依赖! 首次运行自动下载便携编译器 (w64devkit ~90MB)
REM 之后直接编译运行, 无需安装任何东西
REM 
REM 用法: 双击此文件
REM ============================================================

setlocal enabledelayedexpansion

set EXE_NAME=esp32mc_win.exe
REM toolchain 放在项目 code/ 目录外面的 .toolchain 文件夹
set TOOLCHAIN_DIR=%~dp0..\..\toolchain
set W64DEVKIT_DIR=%TOOLCHAIN_DIR%\w64devkit
set GCC=%W64DEVKIT_DIR%\bin\g++.exe
set DOWNLOAD_URL=https://github.com/skeeto/w64devkit/releases/download/v2.8.0/w64devkit-x64-2.8.0.7z.exe

REM ============================================================
REM 检查编译器是否已存在
REM ============================================================

if exist "%GCC%" goto :compile

echo ============================================================
echo  首次运行: 下载便携编译器 (w64devkit, ~90MB)
echo  下载完成后会自动缓存, 以后不再下载
echo ============================================================
echo.

REM 创建 toolchain 目录
if not exist "%TOOLCHAIN_DIR%" mkdir "%TOOLCHAIN_DIR%"

set ARCHIVE=%TOOLCHAIN_DIR%\w64devkit.7z.exe

REM 设置代理 (如果需要翻墙下载 GitHub)
REM 修改下面的端口号为你的代理端口, 或注释掉这两行如果不需要代理
set HTTP_PROXY=http://127.0.0.1:7890
set HTTPS_PROXY=http://127.0.0.1:7890

REM 尝试用 curl 下载 (Windows 10+ 自带)
echo [DOWNLOAD] %DOWNLOAD_URL%
echo [PROXY] %HTTP_PROXY%
echo.
curl.exe -L --proxy "%HTTP_PROXY%" -o "%ARCHIVE%" "%DOWNLOAD_URL%"
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Download failed! Please check your internet connection.
    echo You can also manually download from:
    echo   %DOWNLOAD_URL%
    echo And extract to: %TOOLCHAIN_DIR%\
    pause
    exit /b 1
)

echo.
echo [EXTRACT] Extracting toolchain...

REM w64devkit 是自解压 7z (.7z.exe), 直接运行解压
"%ARCHIVE%" -o"%TOOLCHAIN_DIR%" -y
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Extraction failed!
    echo Please manually extract %ARCHIVE% to %TOOLCHAIN_DIR%\
    pause
    exit /b 1
)

REM 删除下载的压缩包节省空间
del /q "%ARCHIVE%" >nul 2>&1

if not exist "%GCC%" (
    echo [ERROR] Compiler not found after extraction!
    echo Expected: %GCC%
    echo Please check %TOOLCHAIN_DIR% directory.
    pause
    exit /b 1
)

echo [OK] Toolchain ready!
echo.

REM ============================================================
REM 编译
REM ============================================================

:compile
REM 确保 w64devkit 的 bin 在 PATH 最前面, 防止系统旧的 32 位工具被调用
set PATH=%W64DEVKIT_DIR%\bin;%PATH%

echo [BUILD] Compiling ESP32MC (Windows)...

REM 验证编译器是 64 位版本
"%GCC%" -dumpmachine 2>nul | findstr /i "x86_64" >nul
if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] Compiler is not 64-bit!
    echo Found: 
    "%GCC%" -dumpmachine
    echo.
    echo Please delete the toolchain folder and re-run this script:
    echo   rmdir /s /q "%TOOLCHAIN_DIR%"
    echo.
    echo Make sure to download the x64 version.
    pause
    exit /b 1
)

echo [COMPILER] %GCC%
echo.

pushd "%~dp0"

"%GCC%" -m64 -O2 -D_WIN32 -DWIN32_LEAN_AND_MEAN -I. -I.. ^
    win_main.cpp ^
    win_network_layer.cpp ^
    ..\mc_server.cpp ^
    ..\packet_codec.cpp ^
    ..\game_state.cpp ^
    ..\terrain.cpp ^
    ..\procedures.cpp ^
    ..\crafting.cpp ^
    ..\registries.cpp ^
    -lws2_32 ^
    -o %EXE_NAME%

if %ERRORLEVEL% NEQ 0 (
    echo.
    echo [ERROR] Build failed!
    popd
    pause
    exit /b 1
)

popd

REM ============================================================
REM 运行
REM ============================================================

echo.
echo [BUILD] Success!
echo ============================================================
echo.
"%~dp0%EXE_NAME%"
pause
