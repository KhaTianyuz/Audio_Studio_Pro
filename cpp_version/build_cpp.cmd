@echo off
title Audio Studio Pro (C++20) -- One-click Build Script
cd /d "%~dp0"
echo ============================================================================
echo   Audio Studio Pro (C++20 Native) -- Windows One-click Build Script
echo ============================================================================
echo.

where cmake >nul 2>nul
if %errorlevel% neq 0 (
    echo [INFO] CMake not detected. Please install Visual Studio ^(with Desktop development with C++^) or CMake.
    pause
    exit /b 1
)

echo [1/2] Configuring project, fetching dependencies (ImGui/ImPlot/miniaudio, first run needs network)...
cmake -B build -DCMAKE_BUILD_TYPE=Release
if %errorlevel% neq 0 (
    echo [ERROR] CMake configure failed. Check Visual Studio C++ tools and network connection.
    pause
    exit /b 1
)

echo.
echo [2/2] Building standalone AudioStudioPro.exe...
cmake --build build --config Release
if %errorlevel% neq 0 (
    echo [ERROR] Build failed. Check error messages above.
    pause
    exit /b 1
)

echo.
echo ============================================================================
echo Build succeeded. Output executable:
echo   %~dp0build\Release\AudioStudioPro.exe
echo ============================================================================

if exist "%~dp0build\Release" (
    explorer "%~dp0build\Release"
)
pause
