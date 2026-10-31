@echo off
chcp 65001 >nul
title Audio Studio Pro - 一键打包免安装独立程序 (.exe)
cd /d "%~dp0"

echo ============================================================================
echo   Audio Studio Pro — Windows 免安装独立程序 (.exe) 自动打包脚本
echo ============================================================================
echo.

where python >nul 2>nul
if %errorlevel% neq 0 (
    echo [错误] 未检测到 Python 环境，请先安装 Python 3.9+ 并勾选 "Add Python to PATH"。
    pause
    exit /b 1
)

echo [1/3] 正在检查并安装运行依赖与 PyInstaller 打包工具...
python -m pip install --upgrade pip -i https://pypi.tuna.tsinghua.edu.cn/simple --trusted-host pypi.tuna.tsinghua.edu.cn
if exist requirements.txt (
    python -m pip install -r requirements.txt -i https://pypi.tuna.tsinghua.edu.cn/simple --trusted-host pypi.tuna.tsinghua.edu.cn
    if %errorlevel% neq 0 (
        python -m pip install -r requirements.txt
    )
) else (
    python -m pip install PyQt6 pyqtgraph numpy scipy sounddevice soundfile imageio-ffmpeg PyAudioWPatch pyinstaller -i https://pypi.tuna.tsinghua.edu.cn/simple --trusted-host pypi.tuna.tsinghua.edu.cn
)

echo.
echo [2/3] 正在调用 PyInstaller 打包独立程序 (包含全部音频解码库与依赖，耗时约 1~2 分钟)...
set ICON_ARG=
if exist "app_icon.ico" (
    set ICON_ARG=--icon "app_icon.ico" --add-data "app_icon.ico;."
)
python -m PyInstaller --noconfirm --clean --windowed --onedir --name "AudioStudioPro" %ICON_ARG% --collect-all soundfile --collect-all imageio_ffmpeg --collect-all pyqtgraph audio_studio_pro.py
if exist "app_icon.ico" (
    if exist "dist\AudioStudioPro" copy /y "app_icon.ico" "dist\AudioStudioPro\app_icon.ico" >nul
)
if exist "DISCLAIMER.md" (
    if exist "dist\AudioStudioPro" copy /y "DISCLAIMER.md" "dist\AudioStudioPro\DISCLAIMER.md" >nul
)

if %errorlevel% neq 0 (
    echo.
    echo [错误] 打包过程中出现异常，请检查上方控制台输出信息。
    pause
    exit /b 1
)

echo.
echo ============================================================================
echo [3/3] 打包成功！
echo 免安装独立程序目录已生成在：
echo   %~dp0dist\AudioStudioPro\
echo 主程序入口：
echo   %~dp0dist\AudioStudioPro\AudioStudioPro.exe
echo.
echo 您可以将 dist\AudioStudioPro 整个文件夹压缩发给其他电脑，对方无需安装 Python 即可直接双击运行！
echo ============================================================================
if exist "%~dp0dist\AudioStudioPro" (
    explorer "%~dp0dist\AudioStudioPro"
)
pause
