#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Windows 全功能音乐频谱分析、88键钢琴可视化、瞬态截取延音、重采样、内录与泛音合成软件 (Audio Studio Pro)
====================================================================================================
核心特性：
1. 启动时自动检测并补齐缺失依赖，同时自动在软件根目录生成/维护 requirements.txt 与 build_exe.bat。
2. 拖拽加载、全自动同目录 WAV 转换与倍速播放（支持 0.25x ~ 2.0x 及“保持原音高”开关）：
   - 支持直接拖入 .mp3 / .wav / .flac / .ogg / .m4a / .aac / .wma / .opus 等任意音频文件，非 WAV 自动转为同目录同名 .wav。
   - 支持 0.25x ~ 2.0x 倍速播放，右侧附带【保持原音高】复选框（随时切换“变速不变调”与“变速变调”）。
3. 实时频谱分析、暂停保留频谱图像、可选 0/1/2/3 个主音波峰标注、超低窗口点数 (32~8192) 补零平滑曲线：
   - 横坐标：频率 (15Hz~22kHz，完整覆盖并标注钢琴 C0~C8 音名)，纵坐标：分贝 (dBFS, -100dB~0dB)。
   - 支持【📌 暂停时保留频谱图像】开关：音乐暂停时自动定格保留当前频谱、主音标注与参数，不会归零消失。
   - 支持自由选择【主音标注数量：0个 / 1个 / 2个 / 3个】，在频谱波峰、时域分量、88键钢琴上同步显示对应数量的主音。
   - 支持勾选【横坐标对齐与同步缩放】，一键切换【当前选中音频文件】vs【电脑系统媒体播放 (WASAPI 内录)】。
4. 瞬态声音截取、延音/慢放分析与 88 键标准钢琴可视化键盘：
   - 一键截取当前阶段声音（支持前后各 10ms / 20ms / 40ms / 100ms 窗口）。
   - 提供三大回放按钮：【▶ 原样短促播放】、【🎹 拉长延音播放 (1.5秒)】、【🐢 慢速拉伸播放 (可调倍数 2x~20x)】。
   - 左侧视图支持下拉切换：
     * 【📈 时域声波振动图】：动态图例清晰标明绿色真实合成波形与所选数量 (0~3个) 的主音正弦分量波形。
     * 【🎹 88键标准钢琴键盘 (A0~C8)】：绘制完整 52 白键 + 36 黑键，按所选数量 (0~3个) 在对应琴键内部高亮填充并显示音名/频率/分贝标签；支持勾选【🔄 钢琴键跟随音乐实时跳动】，并支持【鼠标点击任意琴键直接弹奏发声】。
5. 无破音可拖拽曲线均衡器 (WOLA + Auto-Headroom + Soft Limiter Curve EQ)。
6. 三模式函数波发生器 (模式 A 泛音包络 / 模式 B 多振荡器混音 / 模式 C 纯数学公式 f(x) 发声) & 钢琴 C0~C8 全音阶选音。
7. 音频重采样（边听边采样 + 一键采样导出）、双源录音与一键打包免安装独立程序 (.exe / build_exe.bat)。
"""

from __future__ import annotations

import importlib.util
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import wave
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# ===========================================================================
# 0. 启动时自动生成 requirements.txt / build_exe.bat 并自动检测补齐缺失依赖
# ===========================================================================

REQUIRED_PACKAGES: List[Tuple[str, str]] = [
    ("numpy", "numpy>=1.24.0"),
    ("scipy", "scipy>=1.10.0"),
    ("PyQt6", "PyQt6>=6.5.0"),
    ("pyqtgraph", "pyqtgraph>=0.13.3"),
    ("sounddevice", "sounddevice>=0.4.6"),
    ("soundfile", "soundfile>=0.12.1"),
    ("imageio_ffmpeg", "imageio-ffmpeg>=0.4.9"),
]
if os.name == "nt":
    REQUIRED_PACKAGES.append(("pyaudiowpatch", "PyAudioWPatch>=0.2.12.6"))

BUILD_BAT_TEMPLATE = """@echo off
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
echo   %~dp0dist\\AudioStudioPro\\
echo 主程序入口：
echo   %~dp0dist\\AudioStudioPro\\AudioStudioPro.exe
echo.
echo 您可以将 dist\\AudioStudioPro 整个文件夹压缩发给其他电脑，对方无需安装 Python 即可直接双击运行！
echo ============================================================================
if exist "%~dp0dist\\AudioStudioPro" (
    explorer "%~dp0dist\\AudioStudioPro"
)
pause
"""


def get_app_root_dir() -> Path:
    """获取软件所在根目录（兼容普通 .py 运行与 PyInstaller 打包后的 .exe 运行）"""
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent



DISCLAIMER_DOCUMENT_TEXT = """【Audio Studio Pro 软件使用免责协议与风险告知书 (DISCLAIMER.md)】

重要提示：在使用本软件（Audio Studio Pro，含 Python 版本、C++ 版本及打包后的可执行程序）之前，请您务必仔细阅读以下全部免责条款。您勾选“我已阅读 DISCLAIMER.md 并遵守以下协议”或以任何方式运行、使用、修改、传播本软件，即视为已完全理解并无条件同意本协议全部内容：

第一条 【完全自担风险原则】
本软件系开源免费工具，仅供个人学习、声学实验与音乐欣赏交流使用。用户下载、安装、运行、操作本软件纯属个人自愿行为。使用本软件过程中或因使用本软件而产生的任何直接、间接、偶然、特殊或衍生的一切后果、损失与风险，均由用户本人独立承担，与本软件作者无任何关系，作者不承担任何法律或经济责任。

第二条 【听力安全与音响/耳机硬件损坏免责】
本软件具备实时均衡器增益调节（最高 +24dB）、自定义数学函数波发生器、多振荡器合成及极端低频/高频声波生成功能。若因用户将音量调得过高、大幅拉高增益、输入剧烈振荡公式或在佩戴耳机/连接外置喇叭时操作不当，导致喇叭劈响、振膜破损、功放/声卡/耳机硬件烧毁，或造成用户及他人耳鸣、听力下降等健康损害，一切后果均由用户自行承担，与作者无关。建议首次调试时先将电脑音量调低并摘下耳机。

第三条 【音频版权与系统内录合规免责】
本软件内置的“音频格式自动转换”、“音频重采样导出”以及“录制电脑系统声音”功能，仅限用于用户拥有合法权利的音频素材或个人学习研究。用户不得利用本软件盗录、破解、非法转换或传播受著作权法保护的商业音乐、流媒体内容或他人隐私语音。因用户违反知识产权法律法规或侵犯第三方权益所引发的一切民事纠纷、行政处罚或刑事责任，均由用户本人全权负责，与本软件作者无关。

第四条 【文件同名覆盖与数据安全免责】
为保障兼容性，当您向本软件导入非 WAV 格式的音乐文件（如 .mp3、.flac、.ogg、.m4a 等）时，软件会自动在原文件同一文件夹下转换并覆盖生成同名的 .wav 文件（例如导入 song.mp3 会自动覆盖生成同目录下的 song.wav）。请勿将重要原始 `.wav` 母带文件与待转换文件同名混放。因同名文件覆盖、误操作、磁盘读写异常或程序中断导致的任何文件丢失、数据损坏，作者概不负责。

第五条 【系统兼容性、驱动冲突与设备故障免责】
本软件不保证在所有 Windows 操作系统版本、声卡硬件、虚拟音频驱动、蓝牙音频设备或第三方安全软件环境下均能正常、无故障地运行。若因运行本软件引发音频卡顿、爆音、程序闪退、系统死机、蓝屏或与其他软件发生冲突，由此造成的任何工作中断或数据损失，均与作者无关。

第六条 【分析结果准确性与“按原样 (AS-IS)”无担保条款】
本软件按“原样 (AS-IS)”提供，不附带任何形式的明示或暗示担保（包括但不限于适销性、特定用途适用性及无缺陷担保）。软件内显示的实时频谱、第一/第二/第三主音音名、88 键钢琴琴键高亮及 BPM 节拍测算结果仅作直观参考，不保证 100% 音乐扒谱或声学测量精度。作者不对因依赖软件显示结果而产生的任何误差或失误负责，亦不负有提供技术支持、故障修复或版本升级的强制义务。

第七条 【第三方使用与二次分发免责】
本软件基于 GNU AGPL v3.0 协议开源。任何第三方个人或机构对本软件进行打包、修改、分发或商业/非商业使用时，其行为仅代表该第三方自身，不代表原作者立场。因第三方修改版、捆绑版或分发渠道引起的任何木马病毒、安全隐患或法律纠纷，与原作者无任何关联。

第八条 【放弃追索权与协议生效】
一旦您勾选同意并继续使用本软件，即表示您永久且不可撤销地放弃因使用本软件而向软件作者提起任何索赔、诉讼、仲裁或追偿的权利。如果您不同意本免责协议中的任何一条内容，请立即点击“不同意并退出”并彻底删除本软件全部文件。
"""


def get_disclaimer_state_path() -> Path:
    return get_app_root_dir() / ".disclaimer_accepted.json"


def ensure_disclaimer_file_on_disk() -> Path:
    disc_path = get_app_root_dir() / "DISCLAIMER.md"
    try:
        if not disc_path.exists():
            disc_path.write_text(DISCLAIMER_DOCUMENT_TEXT.strip() + "\n", encoding="utf-8")
    except OSError:
        pass
    return disc_path


def is_disclaimer_accepted() -> bool:
    state_file = get_disclaimer_state_path()
    if not state_file.exists():
        return False
    try:
        data = json.loads(state_file.read_text(encoding="utf-8"))
        return bool(data.get("accepted", False))
    except Exception:
        return False


def save_disclaimer_accepted() -> None:
    state_file = get_disclaimer_state_path()
    try:
        state_file.write_text(
            json.dumps({"accepted": True, "version": "1.0"}, indent=2, ensure_ascii=False),
            encoding="utf-8",
        )
    except OSError:
        pass


def ensure_requirements_file_and_dependencies() -> None:
    """
    1. 自动在软件根目录生成 requirements.txt 与 build_exe.bat（如果不存在）。
    2. 自动检查运行所需 Python 依赖库，如有缺失则自动调用当前 Python 的 pip 进行安装。
    """
    root_dir = get_app_root_dir()
    ensure_disclaimer_file_on_disk()
    req_file = root_dir / "requirements.txt"
    bat_file = root_dir / "build_exe.bat"
    req_lines = [pkg_spec for _, pkg_spec in REQUIRED_PACKAGES] + [
        "PyAudioWPatch>=0.2.12.6",
        "pyinstaller>=6.0.0",
    ]
    seen = set()
    unique_reqs = []
    for line in req_lines:
        if line not in seen:
            seen.add(line)
            unique_reqs.append(line)

    if not getattr(sys, "frozen", False):
        try:
            if not req_file.exists():
                req_file.write_text("\n".join(unique_reqs) + "\n", encoding="utf-8")
            if not bat_file.exists():
                bat_file.write_text(BUILD_BAT_TEMPLATE, encoding="utf-8")
        except OSError:
            pass

    if getattr(sys, "frozen", False) or os.environ.get("AUDIO_STUDIO_SKIP_AUTO_PIP") == "1":
        return

    missing_specs: List[str] = []
    for import_name, pip_spec in REQUIRED_PACKAGES:
        if importlib.util.find_spec(import_name) is None:
            missing_specs.append(pip_spec)

    if missing_specs:
        print(f"[依赖自检] 检测到缺失依赖库: {missing_specs}，正在通过 pip 自动为您安装...")
        for spec in missing_specs:
            try:
                subprocess.check_call(
                    [
                        sys.executable,
                        "-m",
                        "pip",
                        "install",
                        spec,
                        "-i",
                        "https://pypi.tuna.tsinghua.edu.cn/simple",
                        "--trusted-host",
                        "pypi.tuna.tsinghua.edu.cn",
                    ]
                )
            except Exception:
                try:
                    subprocess.check_call([sys.executable, "-m", "pip", "install", spec])
                except Exception as exc:
                    print(f"[依赖自检警告] 自动安装 {spec} 失败: {exc}")


ensure_requirements_file_and_dependencies()

import numpy as np
from scipy import interpolate, signal
from scipy.io import wavfile

try:
    import soundfile as sf
    HAS_SOUNDFILE = True
except Exception:
    sf = None
    HAS_SOUNDFILE = False

try:
    import imageio_ffmpeg
    HAS_IMAGEIO_FFMPEG = True
except Exception:
    imageio_ffmpeg = None
    HAS_IMAGEIO_FFMPEG = False

try:
    import sounddevice as sd
    HAS_SOUNDDEVICE = True
except Exception:
    sd = None
    HAS_SOUNDDEVICE = False

try:
    import pyaudiowpatch as pyaudio
    HAS_PYAUDIOWPATCH = True
except Exception:
    pyaudio = None
    HAS_PYAUDIOWPATCH = False

try:
    from PyQt6.QtCore import Qt, QTimer, pyqtSignal, QRectF
    from PyQt6.QtGui import (
        QColor,
        QDragEnterEvent,
        QDropEvent,
        QFont,
        QIcon,
        QLinearGradient,
        QPainter,
        QPen,
        QBrush,
    )
    from PyQt6.QtWidgets import (
        QApplication,
        QCheckBox,
        QComboBox,
        QDialog,
        QDoubleSpinBox,
        QFileDialog,
        QFormLayout,
        QGroupBox,
        QHBoxLayout,
        QHeaderView,
        QLabel,
        QLineEdit,
        QMainWindow,
        QGridLayout,
        QMessageBox,
        QProgressBar,
        QPushButton,
        QSizePolicy,
        QSlider,
        QSpinBox,
        QSplitter,
        QStackedWidget,
        QStatusBar,
        QTabWidget,
        QTableWidget,
        QTextEdit,
        QVBoxLayout,
        QWidget,
    )
    import pyqtgraph as pg
    HAS_QT = True
except Exception:
    HAS_QT = False


# ===========================================================================
# 1. 常量、钢琴 C0~C8 音名频率表与配置模型（支持 JSON 导入/导出）
# ===========================================================================

MIN_PLOT_FREQ_HZ = 15.0
MAX_PLOT_FREQ_HZ = 22000.0

PIANO_C0_TO_C8: List[Tuple[str, float]] = [
    ("C0", 16.35),
    ("C1", 32.70),
    ("C2", 65.41),
    ("C3", 130.81),
    ("C4", 261.63),
    ("C5", 523.25),
    ("C6", 1046.50),
    ("C7", 2093.00),
    ("C8", 4186.01),
]

NOTE_NAMES_12 = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def generate_full_piano_notes_c0_to_c8() -> List[Tuple[str, float]]:
    """返回从 C0 (MIDI 12, 16.35Hz) 到 C8 (MIDI 108, 4186.01Hz) 的全部音符及频率"""
    notes: List[Tuple[str, float]] = []
    for midi in range(12, 109):
        note_name = NOTE_NAMES_12[midi % 12]
        octave = (midi // 12) - 1
        freq = 440.0 * (2.0 ** ((midi - 69) / 12.0))
        notes.append((f"{note_name}{octave} ({freq:.2f} Hz)", round(freq, 2)))
    return notes


FULL_PIANO_NOTES_LIST = generate_full_piano_notes_c0_to_c8()

DEFAULT_EQ_FREQS = [31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]
FFT_WINDOW_SIZES = ["32", "64", "128", "256", "512", "1024", "2048", "4096", "8192"]
PLAYBACK_SPEED_OPTIONS = [
    ("0.25x 极慢", 0.25),
    ("0.50x 慢速", 0.50),
    ("0.75x 稍慢", 0.75),
    ("1.00x 原速", 1.00),
    ("1.25x 稍快", 1.25),
    ("1.50x 快速", 1.50),
    ("2.00x 倍速", 2.00),
]

PEAK_COUNT_OPTIONS = [
    ("0个 (不标注)", 0),
    ("1个 (仅第1主音)", 1),
    ("2个 (前2高主音)", 2),
    ("3个 (前3高主音)", 3),
]

# 波峰标注与钢琴高亮前 3 名的专属配色（#1 金黄、#2 青绿、#3 粉紫）
PEAK_RANK_COLORS = ["#FBBF24", "#2DD4BF", "#F472B6"]


@dataclass
class OscillatorItem:
    """单个自定义振荡器配置项"""
    enabled: bool = True
    waveform: str = "sine"  # sine, square, triangle, sawtooth, custom, noise
    freq_hz: float = 440.0
    gain_db: float = -12.0
    phase_deg: float = 0.0
    formula: str = "sin(x)"


@dataclass
class AppPresetConfig:
    """支持导入/导出的全局预设数据"""
    version: str = "1.5"
    name: str = "Custom User Preset"
    eq_enabled: bool = True
    eq_points: List[List[float]] = field(
        default_factory=lambda: [[f, 0.0] for f in DEFAULT_EQ_FREQS]
    )
    # synth_mode: "harmonic_curve" (模式A), "multi_osc" (模式B), "custom_formula" (模式C)
    synth_mode: str = "harmonic_curve"
    fundamental_hz: float = 261.63
    num_harmonics: int = 24
    base_waveform: str = "sine"
    custom_formula: str = "sin(x) + 0.35*sin(2*x) + 0.15*cos(3*x)"
    harmonic_curve_points: List[List[float]] = field(
        default_factory=lambda: [
            [32.7, -45.0],
            [65.4, -25.0],
            [130.8, -12.0],
            [261.6, -3.0],
            [523.3, -6.0],
            [1046.5, -12.0],
            [2093.0, -20.0],
            [4186.0, -32.0],
            [8372.0, -50.0],
            [16744.0, -72.0],
        ]
    )
    oscillators: List[Dict] = field(
        default_factory=lambda: [
            asdict(OscillatorItem(True, "sine", 440.0, -10.0, 0.0, "sin(x)")),
            asdict(OscillatorItem(False, "triangle", 880.0, -18.0, 0.0, "sin(x)")),
            asdict(OscillatorItem(False, "custom", 660.0, -20.0, 0.0, "sin(x) + 0.3*sin(3*x)")),
        ]
    )
    resample_live_enabled: bool = False
    target_sample_rate: int = 44100
    target_bit_depth: int = 16
    target_channels: int = 2
    peak_annotation_count: int = 1
    bpm_capture_enabled: bool = False
    bpm_value: float = 60.0
    bpm_sync_right_snapshot: bool = False

    def to_json(self) -> str:
        return json.dumps(asdict(self), indent=2, ensure_ascii=False)

    @classmethod
    def from_json(cls, text: str) -> "AppPresetConfig":
        data = json.loads(text)
        valid_keys = {k for k in cls.__dataclass_fields__.keys()}
        filtered = {k: v for k, v in data.items() if k in valid_keys}
        return cls(**filtered)


INSTRUMENT_ENVELOPE_PRESETS: Dict[str, List[List[float]]] = {
    "小提琴": [
        [32.7, -50.0], [65.4, -35.0], [130.8, -15.0], [261.6, -5.0],
        [523.3, -2.0], [1046.5, -5.0], [2093.0, -8.0], [3100.0, -4.0],
        [6000.0, -22.0], [16000.0, -68.0],
    ],
    "单簧管": [
        [32.7, -55.0], [65.4, -30.0], [130.8, -15.0], [261.6, -2.0],
        [523.3, -22.0], [784.9, -4.0], [1046.5, -26.0], [1308.1, -8.0],
        [4186.0, -35.0], [16000.0, -75.0],
    ],
    "长笛": [
        [32.7, -60.0], [65.4, -40.0], [130.8, -20.0], [261.6, -2.0],
        [523.3, -4.0], [1046.5, -18.0], [2093.0, -36.0], [4186.0, -54.0],
        [8372.0, -70.0], [16000.0, -78.0],
    ],
    "铜管小号": [
        [32.7, -55.0], [65.4, -35.0], [130.8, -18.0], [261.6, -8.0],
        [523.3, -5.0], [1046.5, -1.0], [2093.0, -3.0], [4186.0, -14.0],
        [8372.0, -36.0], [16000.0, -65.0],
    ],
    "管风琴": [
        [16.35, -18.0], [32.7, -10.0], [65.4, -5.0], [130.8, -3.0],
        [261.6, -2.0], [523.3, -3.0], [1046.5, -5.0], [2093.0, -9.0],
        [4186.0, -16.0], [16000.0, -55.0],
    ],
}

EQ_CURVE_PRESETS: Dict[str, List[List[float]]] = {
    "平坦原声": [[f, 0.0] for f in DEFAULT_EQ_FREQS],
    "低音增强": [
        [31.0, 9.0], [62.0, 7.5], [125.0, 4.5], [250.0, 1.5], [500.0, 0.0],
        [1000.0, 0.0], [2000.0, 0.5], [4000.0, 1.0], [8000.0, 1.5], [16000.0, 1.5],
    ],
    "人声清晰": [
        [31.0, -4.0], [62.0, -2.5], [125.0, -1.0], [250.0, 1.0], [500.0, 3.0],
        [1000.0, 4.5], [2000.0, 5.0], [4000.0, 3.0], [8000.0, 0.5], [16000.0, -1.5],
    ],
    "摇滚现场": [
        [31.0, 6.0], [62.0, 5.0], [125.0, 2.5], [250.0, -1.5], [500.0, -3.0],
        [1000.0, -1.5], [2000.0, 2.0], [4000.0, 4.5], [8000.0, 6.0], [16000.0, 6.5],
    ],
}


# ===========================================================================
# 2. 全自动多格式解码与同目录持久转换引擎（彻底消除 WinError 193）
# ===========================================================================

class UniversalAudioFormatConverter:
    """
    内置全自动音频格式转换与重采样引擎：
    导入非 WAV 音频（如 .mp3 / .flac / .ogg / .m4a / .aac / .wma）或特殊编码 WAV 时，
    自动将其转换为同目录下的标准 16-bit PCM .wav 文件（每次重新覆盖生成同名 .wav），然后加载播放。
    """

    @staticmethod
    def is_valid_executable(exe_path: Optional[str]) -> bool:
        if not exe_path or not os.path.isfile(exe_path):
            return False
        try:
            if os.path.getsize(exe_path) < 4096:
                return False
            if os.name == "nt":
                with open(exe_path, "rb") as f:
                    magic = f.read(2)
                return magic == b"MZ"
            return os.access(exe_path, os.X_OK)
        except OSError:
            return False

    @staticmethod
    def find_valid_ffmpeg_binary() -> Optional[str]:
        root_dir = get_app_root_dir()
        local_candidates = [
            root_dir / "ffmpeg.exe",
            root_dir / "bin" / "ffmpeg.exe",
            root_dir / "ffmpeg",
        ]
        for cand in local_candidates:
            if UniversalAudioFormatConverter.is_valid_executable(str(cand)):
                return str(cand)

        if HAS_IMAGEIO_FFMPEG:
            try:
                pkg_exe = imageio_ffmpeg.get_ffmpeg_exe()
                if UniversalAudioFormatConverter.is_valid_executable(pkg_exe):
                    return pkg_exe
            except Exception:
                pass

        sys_ffmpeg = shutil.which("ffmpeg.exe") or shutil.which("ffmpeg")
        if UniversalAudioFormatConverter.is_valid_executable(sys_ffmpeg):
            return sys_ffmpeg

        return None

    @staticmethod
    def load_and_auto_convert_to_wav(file_path: str) -> Tuple[np.ndarray, int, str]:
        if not os.path.isfile(file_path):
            raise FileNotFoundError(f"找不到音频文件: {file_path}")

        src_path = Path(file_path).resolve()
        ext = src_path.suffix.lower()
        same_dir_wav = src_path.with_suffix(".wav")

        if ext == ".wav":
            try:
                audio, sr = UniversalAudioFormatConverter._read_wav_native(str(src_path))
                return audio, sr, str(src_path)
            except Exception:
                audio, sr = UniversalAudioFormatConverter._decode_any_format_to_array(str(src_path))
                UniversalAudioFormatConverter._write_wav_native(str(same_dir_wav), audio, sr, 16)
                return audio, sr, str(same_dir_wav)

        audio, sr = UniversalAudioFormatConverter._decode_any_format_to_array(str(src_path))
        UniversalAudioFormatConverter._write_wav_native(str(same_dir_wav), audio, sr, 16)
        return audio, sr, str(same_dir_wav)

    @staticmethod
    def _decode_any_format_to_array(file_path: str) -> Tuple[np.ndarray, int]:
        errors: List[str] = []

        if HAS_SOUNDFILE:
            try:
                data, sr = sf.read(file_path, dtype="float32", always_2d=True)
                return UniversalAudioFormatConverter._normalize_stereo(data), int(sr)
            except Exception as exc:
                errors.append(f"soundfile 解码提示: {exc}")

        ffmpeg_bin = UniversalAudioFormatConverter.find_valid_ffmpeg_binary()
        if ffmpeg_bin:
            with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
                tmp_wav = tmp.name
            try:
                creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
                cmd = [
                    ffmpeg_bin,
                    "-y",
                    "-i",
                    file_path,
                    "-vn",
                    "-acodec",
                    "pcm_s16le",
                    tmp_wav,
                ]
                proc = subprocess.run(
                    cmd,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    check=False,
                    creationflags=creation_flags,
                )
                if proc.returncode == 0 and os.path.exists(tmp_wav) and os.path.getsize(tmp_wav) > 64:
                    return UniversalAudioFormatConverter._read_wav_native(tmp_wav)
                err_txt = proc.stderr.decode("utf-8", errors="ignore")[-300:]
                errors.append(f"内置转换器返回码 {proc.returncode}: {err_txt}")
            except Exception as exc:
                errors.append(f"内置转换器调用异常: {exc}")
            finally:
                if os.path.exists(tmp_wav):
                    try:
                        os.remove(tmp_wav)
                    except OSError:
                        pass

        try:
            return UniversalAudioFormatConverter._read_wav_native(file_path)
        except Exception as exc:
            errors.append(f"原生 WAV 读取提示: {exc}")

        detail = "\n".join(errors)
        raise RuntimeError(
            f"无法自动转换该音频文件 ({os.path.basename(file_path)})。\n"
            f"请确认已在终端运行: pip install soundfile imageio-ffmpeg\n"
            f"详细诊断信息:\n{detail}"
        )

    @staticmethod
    def _normalize_stereo(data: np.ndarray) -> np.ndarray:
        if data.dtype == np.int16:
            audio = data.astype(np.float32) / 32768.0
        elif data.dtype == np.int32:
            audio = data.astype(np.float32) / 2147483648.0
        elif data.dtype == np.uint8:
            audio = (data.astype(np.float32) - 128.0) / 128.0
        else:
            audio = data.astype(np.float32)
            max_val = float(np.max(np.abs(audio))) if audio.size > 0 else 0.0
            if max_val > 1.0:
                audio /= max_val

        if audio.ndim == 1:
            audio = np.column_stack([audio, audio])
        elif audio.shape[1] == 1:
            audio = np.column_stack([audio[:, 0], audio[:, 0]])
        elif audio.shape[1] > 2:
            audio = audio[:, :2]

        return np.clip(audio, -1.0, 1.0).astype(np.float32)

    @staticmethod
    def _read_wav_native(wav_path: str) -> Tuple[np.ndarray, int]:
        sr, data = wavfile.read(wav_path)
        return UniversalAudioFormatConverter._normalize_stereo(data), int(sr)

    @staticmethod
    def export_and_convert_audio(
        audio_data: np.ndarray,
        orig_sr: int,
        output_path: str,
        target_sr: int = 44100,
        target_bit_depth: int = 16,
        target_channels: int = 2,
        bitrate: str = "192k",
    ) -> str:
        processed = AudioDSPEngine.offline_resample(
            audio_data, orig_sr, target_sr, target_bit_depth, target_channels
        )

        ext = os.path.splitext(output_path)[1].lower()
        if ext == ".wav":
            UniversalAudioFormatConverter._write_wav_native(
                output_path, processed, target_sr, target_bit_depth
            )
            return output_path

        if HAS_SOUNDFILE and ext in (".flac", ".ogg", ".mp3"):
            try:
                subtype = None
                if ext == ".flac":
                    subtype = "PCM_24" if target_bit_depth >= 24 else "PCM_16"
                sf.write(output_path, processed, target_sr, subtype=subtype)
                return output_path
            except Exception:
                pass

        ffmpeg_bin = UniversalAudioFormatConverter.find_valid_ffmpeg_binary()
        if not ffmpeg_bin:
            fallback_wav = str(Path(output_path).with_suffix(".wav"))
            UniversalAudioFormatConverter._write_wav_native(
                fallback_wav, processed, target_sr, target_bit_depth
            )
            return fallback_wav

        with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
            tmp_wav = tmp.name

        try:
            UniversalAudioFormatConverter._write_wav_native(
                tmp_wav, processed, target_sr, target_bit_depth
            )
            creation_flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
            cmd = [
                ffmpeg_bin,
                "-y",
                "-i",
                tmp_wav,
                "-ar",
                str(target_sr),
                "-ac",
                str(target_channels),
                "-b:a",
                bitrate,
                output_path,
            ]
            proc = subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
                creationflags=creation_flags,
            )
            if proc.returncode != 0:
                err_msg = proc.stderr.decode("utf-8", errors="ignore")[-400:]
                raise RuntimeError(f"格式转换导出失败:\n{err_msg}")
            return output_path
        finally:
            if os.path.exists(tmp_wav):
                try:
                    os.remove(tmp_wav)
                except OSError:
                    pass

    @staticmethod
    def _write_wav_native(
        output_path: str, audio: np.ndarray, sr: int, bit_depth: int
    ) -> None:
        clipped = np.clip(audio, -1.0, 1.0)
        if bit_depth == 8:
            out_arr = np.round((clipped + 1.0) * 127.5).astype(np.uint8)
            wavfile.write(output_path, sr, out_arr)
        elif bit_depth == 16:
            out_arr = np.round(clipped * 32767.0).astype(np.int16)
            wavfile.write(output_path, sr, out_arr)
        elif bit_depth == 24:
            pcm32 = np.round(clipped * 8388607.0).astype(np.int32)
            if pcm32.ndim == 1:
                n_ch = 1
                flat = pcm32
            else:
                n_ch = pcm32.shape[1]
                flat = pcm32.reshape(-1)
            raw_bytes = flat.astype("<i4").tobytes()
            b_arr = np.frombuffer(raw_bytes, dtype=np.uint8).reshape(-1, 4)[:, :3].tobytes()
            with wave.open(output_path, "wb") as wf:
                wf.setnchannels(n_ch)
                wf.setsampwidth(3)
                wf.setframerate(sr)
                wf.writeframes(b_arr)
        else:
            wavfile.write(output_path, sr, clipped.astype(np.float32))


# ===========================================================================
# 3. 无破音重叠相加均衡器 (Stateful WOLA EQ) 与 DSP 寻峰/延音/慢放/倍速引擎
# ===========================================================================

class StatefulOverlapAddEQ:
    """
    解决“均衡器低音调高时喇叭有劈响/破音”的专用状态连续滤波器：
    采用双正弦窗 (Sine / Root-Hann Window, sin^2(x) + cos^2(x) = 1.0) 的 50% 重叠相加 (WOLA)
    + 自动动态余量补偿与软膝限幅器。
    """

    def __init__(self, block_size: int = 2048, channels: int = 2) -> None:
        self.block_size = block_size
        self.channels = channels
        self._prev_in = np.zeros((block_size, channels), dtype=np.float32)
        self._ola_tail = np.zeros((block_size // 2, channels), dtype=np.float32)

    def reset(self) -> None:
        self._prev_in.fill(0.0)
        self._ola_tail.fill(0.0)

    def process_block(
        self,
        audio_block: np.ndarray,
        sample_rate: int,
        eq_points: List[List[float]],
    ) -> np.ndarray:
        if audio_block.size == 0 or not eq_points:
            return audio_block

        work = audio_block.astype(np.float32)
        if work.ndim == 1:
            work = np.column_stack([work, work])

        n_frames, n_ch = work.shape
        if n_frames != self.block_size or n_ch != self.channels:
            self.block_size = n_frames
            self.channels = n_ch
            self._prev_in = np.zeros((n_frames, n_ch), dtype=np.float32)
            self._ola_tail = np.zeros((n_frames // 2, n_ch), dtype=np.float32)

        half = n_frames // 2
        freqs = np.fft.rfftfreq(n_frames, d=1.0 / float(sample_rate))
        gain_db = AudioDSPEngine.evaluate_smooth_curve_db(
            eq_points, freqs, min_db=-24.0, max_db=24.0
        )

        max_boost_db = float(np.max(gain_db))
        low_band_mask = freqs <= 280.0
        low_boost_db = float(np.max(gain_db[low_band_mask])) if np.any(low_band_mask) else 0.0
        headroom_db = 0.0
        if low_boost_db > 3.0:
            headroom_db -= (low_boost_db - 3.0) * 0.55
        elif max_boost_db > 4.0:
            headroom_db -= (max_boost_db - 4.0) * 0.40

        effective_db = gain_db + headroom_db
        gain_linear = (10.0 ** (effective_db / 20.0)).astype(np.float32)[:, None]

        idx = np.arange(n_frames, dtype=np.float32)
        sine_win = np.sin(np.pi * idx / float(n_frames))[:, None]

        frame1 = np.vstack([self._prev_in[half:], work[:half]]) * sine_win
        frame2 = work * sine_win

        y1 = (
            np.fft.irfft(np.fft.rfft(frame1, n=n_frames, axis=0) * gain_linear, n=n_frames, axis=0).astype(
                np.float32
            )
            * sine_win
        )
        y2 = (
            np.fft.irfft(np.fft.rfft(frame2, n=n_frames, axis=0) * gain_linear, n=n_frames, axis=0).astype(
                np.float32
            )
            * sine_win
        )

        out = np.zeros((n_frames, n_ch), dtype=np.float32)
        out[:half] = self._ola_tail + y1[:half]
        out[half:] = y1[half:] + y2[:half]

        self._ola_tail = y2[half:].copy()
        self._prev_in = work.copy()

        return AudioDSPEngine.soft_knee_limiter(out)


class AudioDSPEngine:
    """
    负责实时 FFT 频谱计算、0~3 个波峰寻峰与音名/MIDI 编号提取、实时倍速播放（变速不变调 & 变速变调）、
    88键钢琴点击合成、瞬态声音延音/慢放重构、任意曲线均衡器 (EQ) 滤波及泛音包络合成。
    """

    NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]

    @staticmethod
    def soft_knee_limiter(audio: np.ndarray, threshold: float = 0.80) -> np.ndarray:
        abs_x = np.abs(audio)
        over_mask = abs_x > threshold
        if not np.any(over_mask):
            return audio
        out = audio.copy()
        headroom = 0.99 - threshold
        excess = (abs_x[over_mask] - threshold) / max(1e-4, headroom)
        compressed = threshold + headroom * np.tanh(excess)
        out[over_mask] = np.sign(audio[over_mask]) * compressed
        return out.astype(np.float32)

    @staticmethod
    def freq_to_midi(freq_hz: float) -> int:
        if freq_hz <= 14.0 or not np.isfinite(freq_hz):
            return -1
        return int(round(69.0 + 12.0 * math.log2(freq_hz / 440.0)))

    @staticmethod
    def midi_to_note_name(midi: int) -> str:
        if midi < 0 or midi > 127:
            return "--"
        note = AudioDSPEngine.NOTE_NAMES[midi % 12]
        octave = (midi // 12) - 1
        return f"{note}{octave}"

    @staticmethod
    def freq_to_note_name(freq_hz: float) -> str:
        midi = AudioDSPEngine.freq_to_midi(freq_hz)
        return AudioDSPEngine.midi_to_note_name(midi)

    _DEFAULT_WSOLA_STATE: Dict[str, int] = {}

    @staticmethod
    def extract_variable_speed_block(
        audio_buffer: np.ndarray,
        play_cursor: int,
        out_frames: int,
        speed: float = 1.0,
        keep_pitch: bool = True,
        wsola_state: Optional[Dict[str, int]] = None,
    ) -> Tuple[np.ndarray, int]:
        """
        实时无破音 WSOLA (Waveform Similarity Overlap-Add) 倍速播放核心：
        1. 块间严格零跳变连续（C1 Continuity）：当前块的起始采样点 read_a 严格衔接上一块尾帧的结束点，
           当前块的结尾权重 w_b[-1]=1.0, w_a[-1]=0.0 且导数为 0，彻底消除每 2048 采样点交界处的爆音与劈响。
        2. 块内互相关同相锁定（Phase Correlation Alignment）：使用快速向量化互相关在目标时间窗附近寻找同相周期点，
           防止倍速交叉淡入淡出时发生反相抵消或梳状滤波失真。
        3. 绝对名义时间轴推进（Zero Tempo Drift）：next_cur 严格按 round(out_frames * speed) 步进，不发生长时累積漂移。
        """
        state = AudioDSPEngine._DEFAULT_WSOLA_STATE if wsola_state is None else wsola_state
        total_len = audio_buffer.shape[0]
        if total_len == 0 or play_cursor >= total_len:
            state.clear()
            return np.zeros((out_frames, 2), dtype=np.float32), 0

        spd = max(0.1, min(4.0, float(speed)))
        if abs(spd - 1.0) < 1e-3:
            take = min(out_frames, total_len - play_cursor)
            block = np.zeros((out_frames, 2), dtype=np.float32)
            block[:take] = audio_buffer[play_cursor : play_cursor + take]
            next_cur = play_cursor + take
            if next_cur >= total_len:
                next_cur = 0
                state.clear()
            else:
                state["read_head"] = next_cur
                state["expected_cursor"] = next_cur
                state["buf_len"] = total_len
            return block, next_cur

        nominal_advance = max(1, int(round(out_frames * spd)))
        next_cur = play_cursor + nominal_advance

        if not keep_pitch:
            idx_float = play_cursor + np.arange(out_frames, dtype=np.float64) * spd
            valid_mask = idx_float < (total_len - 1)
            idx_clamped = np.clip(idx_float, 0.0, max(0.0, float(total_len - 2)))
            idx0 = idx_clamped.astype(np.int64)
            frac = (idx_clamped - idx0).astype(np.float32)[:, None]
            out = (1.0 - frac) * audio_buffer[idx0] + frac * audio_buffer[idx0 + 1]
            out[~valid_mask] = 0.0
            if next_cur >= total_len:
                next_cur = 0
            state.clear()
            return out.astype(np.float32), next_cur

        # 若与上一音频块连续播放，则 Grain A 从上一块 Grain B 实际结束点 read_head 无缝接续，保证块间 0 阶跃
        if (
            state.get("expected_cursor") == play_cursor
            and state.get("buf_len") == total_len
            and 0 <= state.get("read_head", -1) < total_len
        ):
            read_a = int(state["read_head"])
        else:
            read_a = play_cursor

        grain_a = np.zeros((out_frames, 2), dtype=np.float32)
        take_a = min(out_frames, total_len - read_a)
        if take_a > 0:
            grain_a[:take_a] = audio_buffer[read_a : read_a + take_a]

        target_next_cur = min(total_len, next_cur)
        if target_next_cur >= total_len:
            state.clear()
            return grain_a, 0

        xfade_len = min(out_frames, target_next_cur)
        if xfade_len < 32:
            state["read_head"] = target_next_cur
            state["expected_cursor"] = target_next_cur
            state["buf_len"] = total_len
            return grain_a, target_next_cur

        # WSOLA 同相搜索：在 target_next_cur 附近搜索与 grain_a 同相位的最佳接续点 best_end_b
        search_radius = min(512, xfade_len // 3)
        min_end_b = max(xfade_len, target_next_cur - search_radius)
        max_end_b = min(total_len, target_next_cur + search_radius)

        best_end_b = target_next_cur
        if max_end_b > min_end_b and take_a >= out_frames:
            cmp_len = min(512, xfade_len // 2)
            rel_offset = (xfade_len - cmp_len) // 2
            a_start = out_frames - xfade_len + rel_offset
            ref_seg = grain_a[a_start : a_start + cmp_len, 0]
            if float(np.dot(ref_seg, ref_seg)) > 1e-8:
                col0 = audio_buffer[:, 0]
                s_start = min_end_b - xfade_len + rel_offset
                s_end = max_end_b - xfade_len + rel_offset + cmp_len
                search_slice = col0[s_start:s_end]
                if len(search_slice) >= len(ref_seg):
                    corrs = np.correlate(search_slice, ref_seg, mode="valid")
                    offsets = np.arange(len(corrs), dtype=np.float32) + (min_end_b - target_next_cur)
                    window_bias = 1.0 - 0.12 * (np.abs(offsets) / max(1.0, float(search_radius)))
                    weighted_corrs = np.where(corrs > 0, corrs * window_bias, corrs)
                    best_end_b = min_end_b + int(np.argmax(weighted_corrs))

        out = grain_a.copy()
        grain_b_tail = audio_buffer[best_end_b - xfade_len : best_end_b]

        theta = np.linspace(0.0, 0.5 * np.pi, xfade_len, dtype=np.float32)[:, None]
        w_a = np.cos(theta) ** 2
        w_b = np.sin(theta) ** 2
        out[out_frames - xfade_len :] = grain_a[out_frames - xfade_len :] * w_a + grain_b_tail * w_b

        if next_cur >= total_len:
            next_cur = 0
            state.clear()
        else:
            state["read_head"] = best_end_b
            state["expected_cursor"] = next_cur
            state["buf_len"] = total_len

        return out.astype(np.float32), next_cur

    @staticmethod
    def synthesize_piano_key_note(
        midi_note: int, sample_rate: int = 44100, duration_sec: float = 0.85
    ) -> np.ndarray:
        freq_hz = 440.0 * (2.0 ** ((midi_note - 69) / 12.0))
        n_samples = max(1024, int(round(duration_sec * sample_rate)))
        t = np.arange(n_samples, dtype=np.float64) / float(sample_rate)

        harmonics = [(1, 1.00, 3.2), (2, 0.45, 4.5), (3, 0.22, 6.0), (4, 0.12, 8.0), (5, 0.06, 10.0), (6, 0.03, 12.0)]
        sig = np.zeros(n_samples, dtype=np.float64)
        nyquist = sample_rate * 0.46
        for k, amp, decay_rate in harmonics:
            fk = k * freq_hz
            if fk < nyquist:
                env_k = np.exp(-decay_rate * t)
                sig += amp * env_k * np.sin(2.0 * np.pi * fk * t)

        attack = min(n_samples // 10, max(16, int(0.005 * sample_rate)))
        release = min(n_samples // 5, max(32, int(0.050 * sample_rate)))
        sig[:attack] *= np.linspace(0.0, 1.0, attack)
        sig[-release:] *= np.linspace(1.0, 0.0, release)

        peak = float(np.max(np.abs(sig)))
        if peak > 1e-6:
            sig *= 0.80 / peak
        return np.column_stack([sig, sig]).astype(np.float32)

    @staticmethod
    def find_top_n_spectral_peaks(
        freqs: np.ndarray,
        mag_db: np.ndarray,
        top_n: int = 3,
        min_db: float = -78.0,
        min_semitone_sep: float = 1.15,
    ) -> List[Dict[str, object]]:
        if top_n <= 0:
            return []
        valid_mask = (freqs >= MIN_PLOT_FREQ_HZ) & (freqs <= MAX_PLOT_FREQ_HZ)
        if not np.any(valid_mask):
            return []

        v_freqs = freqs[valid_mask]
        v_db = mag_db[valid_mask]
        if len(v_db) < 3:
            return []

        peak_indices, _ = signal.find_peaks(v_db, height=min_db, prominence=1.2)
        if len(peak_indices) == 0:
            max_i = int(np.argmax(v_db))
            if v_db[max_i] >= min_db:
                peak_indices = np.array([max_i], dtype=int)
            else:
                return []

        sorted_order = np.argsort(v_db[peak_indices])[::-1]
        sorted_indices = peak_indices[sorted_order]

        selected: List[Dict[str, object]] = []
        for idx in sorted_indices:
            if 0 < idx < len(v_db) - 1:
                y0, y1, y2 = float(v_db[idx - 1]), float(v_db[idx]), float(v_db[idx + 1])
                denom = y0 - 2.0 * y1 + y2
                if abs(denom) > 1e-9:
                    delta = float(np.clip(0.5 * (y0 - y2) / denom, -0.5, 0.5))
                    df = float(v_freqs[idx + 1] - v_freqs[idx])
                    true_f = float(v_freqs[idx]) + delta * df
                    true_db = y1 - 0.25 * (y0 - y2) * delta
                else:
                    true_f = float(v_freqs[idx])
                    true_db = y1
            else:
                true_f = float(v_freqs[idx])
                true_db = float(v_db[idx])

            if true_db < min_db or true_f < MIN_PLOT_FREQ_HZ:
                continue

            too_close = False
            for existing in selected:
                ex_f = float(existing["freq_hz"])
                semitone_diff = abs(12.0 * math.log2(true_f / ex_f))
                if semitone_diff < min_semitone_sep:
                    too_close = True
                    break
            if too_close:
                continue

            rank = len(selected) + 1
            midi_num = AudioDSPEngine.freq_to_midi(true_f)
            selected.append(
                {
                    "rank": rank,
                    "freq_hz": round(true_f, 2),
                    "mag_db": round(true_db, 1),
                    "note": AudioDSPEngine.midi_to_note_name(midi_num),
                    "midi": midi_num,
                    "amp_linear": float(10.0 ** (true_db / 20.0)),
                }
            )
            if len(selected) >= top_n:
                break

        return selected

    @staticmethod
    def build_raw_snippet_playback(snippet: np.ndarray, sample_rate: int) -> np.ndarray:
        if snippet.size == 0:
            return np.zeros((1024, 2), dtype=np.float32)
        work = snippet.astype(np.float32).copy()
        if work.ndim == 1:
            work = np.column_stack([work, work])

        n = work.shape[0]
        fade_len = min(n // 4, max(8, int(0.002 * sample_rate)))
        if fade_len > 1:
            fade_in = (0.5 - 0.5 * np.cos(np.linspace(0, np.pi, fade_len))).astype(np.float32)[:, None]
            fade_out = fade_in[::-1]
            work[:fade_len] *= fade_in
            work[-fade_len:] *= fade_out
        return np.clip(work, -0.98, 0.98)

    @staticmethod
    def build_sustain_playback(
        snippet: np.ndarray,
        sample_rate: int,
        duration_sec: float = 1.5,
        top_peaks: Optional[List[Dict[str, object]]] = None,
        use_harmonic_resynthesis: bool = False,
    ) -> np.ndarray:
        target_len = max(1024, int(round(duration_sec * sample_rate)))
        if snippet.size == 0:
            return np.zeros((target_len, 2), dtype=np.float32)

        work = snippet.astype(np.float32)
        if work.ndim == 1:
            work = np.column_stack([work, work])

        if use_harmonic_resynthesis and top_peaks:
            t = np.arange(target_len, dtype=np.float64) / float(sample_rate)
            sig = np.zeros(target_len, dtype=np.float64)
            for p in top_peaks:
                f_hz = float(p["freq_hz"])
                amp = float(p["amp_linear"])
                sig += amp * np.sin(2.0 * np.pi * f_hz * t)
            peak = float(np.max(np.abs(sig)))
            if peak > 1e-6:
                sig = sig * (0.85 / peak)
            out = np.column_stack([sig, sig]).astype(np.float32)
        else:
            win_len = work.shape[0]
            if win_len < 64:
                reps = int(math.ceil(64.0 / max(1, win_len)))
                work = np.tile(work, (reps, 1))
                win_len = work.shape[0]

            hop = max(16, win_len // 2)
            hanning = np.hanning(win_len).astype(np.float32)[:, None]
            windowed_grain = work * hanning

            out = np.zeros((target_len + win_len, 2), dtype=np.float32)
            pos = 0
            while pos < target_len:
                out[pos : pos + win_len] += windowed_grain
                pos += hop
            out = out[:target_len]

            peak = float(np.max(np.abs(out)))
            orig_peak = float(np.max(np.abs(work)))
            if peak > 1e-6:
                out *= min(0.90, max(0.35, orig_peak)) / peak

        attack = min(target_len // 4, int(0.025 * sample_rate))
        release = min(target_len // 3, int(0.080 * sample_rate))
        if attack > 1:
            out[:attack] *= np.linspace(0.0, 1.0, attack, dtype=np.float32)[:, None]
        if release > 1:
            out[-release:] *= np.linspace(1.0, 0.0, release, dtype=np.float32)[:, None]

        return np.clip(out, -0.98, 0.98).astype(np.float32)

    @staticmethod
    def build_time_stretched_playback(
        snippet: np.ndarray,
        sample_rate: int,
        stretch_factor: float = 10.0,
        keep_pitch: bool = True,
    ) -> np.ndarray:
        if snippet.size == 0:
            return np.zeros((1024, 2), dtype=np.float32)

        work = snippet.astype(np.float32)
        if work.ndim == 1:
            work = np.column_stack([work, work])

        factor = max(1.0, min(50.0, float(stretch_factor)))
        n_in = work.shape[0]
        n_out = max(64, int(round(n_in * factor)))

        if not keep_pitch or n_in < 128:
            x_old = np.linspace(0.0, 1.0, n_in)
            x_new = np.linspace(0.0, 1.0, n_out)
            out = np.zeros((n_out, 2), dtype=np.float32)
            for ch in range(2):
                out[:, ch] = np.interp(x_new, x_old, work[:, ch]).astype(np.float32)
        else:
            n_fft = min(512, max(64, 2 ** int(math.log2(n_in // 2))))
            hop_in = max(8, n_fft // 4)
            out_channels = []
            win = np.hanning(n_fft)

            for ch in range(2):
                sig = work[:, ch].astype(np.float64)
                frames = []
                for start in range(0, max(1, len(sig) - n_fft + 1), hop_in):
                    frames.append(np.fft.rfft(sig[start : start + n_fft] * win))
                if len(frames) < 2:
                    out_ch = AudioDSPEngine.build_sustain_playback(
                        work[:, ch], sample_rate, duration_sec=n_out / float(sample_rate)
                    )[:, 0]
                    out_channels.append(out_ch)
                    continue

                stft_mat = np.array(frames)
                n_stft_frames, n_bins = stft_mat.shape
                time_steps = np.linspace(0, n_stft_frames - 1, int(round(n_stft_frames * factor)))

                omega = 2.0 * np.pi * hop_in * np.arange(n_bins) / float(n_fft)
                phase_acc = np.angle(stft_mat[0]).copy()
                y_out = np.zeros(len(time_steps) * hop_in + n_fft, dtype=np.float64)

                for i, step in enumerate(time_steps):
                    idx0 = min(int(math.floor(step)), n_stft_frames - 1)
                    idx1 = min(idx0 + 1, n_stft_frames - 1)
                    frac = step - idx0

                    mag = (1.0 - frac) * np.abs(stft_mat[idx0]) + frac * np.abs(stft_mat[idx1])
                    dphi = np.angle(stft_mat[idx1]) - np.angle(stft_mat[idx0]) - omega
                    dphi = (dphi + np.pi) % (2.0 * np.pi) - np.pi
                    phase_acc += omega + dphi

                    synth_spec = mag * np.exp(1j * phase_acc)
                    grain = np.fft.irfft(synth_spec, n=n_fft) * win
                    pos = i * hop_in
                    y_out[pos : pos + n_fft] += grain

                peak = float(np.max(np.abs(y_out)))
                orig_peak = float(np.max(np.abs(sig)))
                if peak > 1e-6:
                    y_out *= max(0.35, orig_peak) / peak
                out_channels.append(y_out.astype(np.float32))

            min_len = min(len(out_channels[0]), len(out_channels[1]))
            out = np.column_stack([out_channels[0][:min_len], out_channels[1][:min_len]])

        fade = min(out.shape[0] // 6, int(0.015 * sample_rate))
        if fade > 1:
            out[:fade] *= np.linspace(0.0, 1.0, fade, dtype=np.float32)[:, None]
            out[-fade:] *= np.linspace(1.0, 0.0, fade, dtype=np.float32)[:, None]
        return np.clip(out, -0.98, 0.98).astype(np.float32)

    @staticmethod
    def compute_fft_spectrum_db(
        audio_chunk: np.ndarray, sample_rate: int, fft_size: int = 4096
    ) -> Tuple[np.ndarray, np.ndarray, Dict[str, float]]:
        win_len = max(16, int(fft_size))
        n_fft = max(win_len, 2048)

        if audio_chunk.size == 0:
            freqs = np.linspace(0, sample_rate / 2.0, n_fft // 2 + 1)
            mag_db = np.full_like(freqs, -100.0)
            return freqs, mag_db, {
                "peak_freq_hz": 0.0,
                "peak_db": -100.0,
                "rms_db": -100.0,
                "centroid_hz": 0.0,
            }

        if audio_chunk.ndim > 1:
            mono = np.mean(audio_chunk, axis=1)
        else:
            mono = audio_chunk

        if len(mono) >= win_len:
            frame = mono[-win_len:].astype(np.float64)
        else:
            frame = np.zeros(win_len, dtype=np.float64)
            frame[-len(mono):] = mono

        rms = float(np.sqrt(np.mean(mono.astype(np.float64) ** 2)) + 1e-12)
        rms_db = float(np.clip(20.0 * math.log10(rms), -100.0, 6.0))

        window = np.hanning(win_len)
        win_sum = np.sum(window)
        spectrum = np.fft.rfft(frame * window, n=n_fft)
        mag_linear = (2.0 * np.abs(spectrum)) / (win_sum + 1e-12)
        mag_linear[0] /= 2.0

        mag_db = 20.0 * np.log10(np.maximum(mag_linear, 1e-5))
        mag_db = np.clip(mag_db, -100.0, 12.0)
        freqs = np.fft.rfftfreq(n_fft, d=1.0 / float(sample_rate))

        valid_mask = freqs >= MIN_PLOT_FREQ_HZ
        if np.any(valid_mask):
            valid_freqs = freqs[valid_mask]
            valid_db = mag_db[valid_mask]
            valid_lin = mag_linear[valid_mask]
            max_idx = int(np.argmax(valid_db))
            peak_freq_hz = float(valid_freqs[max_idx])
            peak_db = float(valid_db[max_idx])
            lin_sum = float(np.sum(valid_lin))
            centroid_hz = (
                float(np.sum(valid_freqs * valid_lin) / lin_sum)
                if lin_sum > 1e-9
                else 0.0
            )
        else:
            peak_freq_hz = 0.0
            peak_db = -100.0
            centroid_hz = 0.0

        return freqs, mag_db, {
            "peak_freq_hz": peak_freq_hz,
            "peak_db": peak_db,
            "rms_db": rms_db,
            "centroid_hz": centroid_hz,
        }

    @staticmethod
    def evaluate_smooth_curve_db(
        control_points: List[List[float]],
        query_freqs: np.ndarray,
        min_db: float = -24.0,
        max_db: float = 24.0,
    ) -> np.ndarray:
        if not control_points:
            return np.zeros_like(query_freqs, dtype=np.float64)

        pts = sorted(control_points, key=lambda p: max(1.0, float(p[0])))
        unique_f: List[float] = []
        unique_db: List[float] = []
        for f, db in pts:
            f_clamped = max(10.0, min(24000.0, float(f)))
            if unique_f and abs(f_clamped - unique_f[-1]) < 0.5:
                unique_db[-1] = float(db)
            else:
                unique_f.append(f_clamped)
                unique_db.append(float(db))

        if len(unique_f) == 1:
            return np.full_like(
                query_freqs, np.clip(unique_db[0], min_db, max_db), dtype=np.float64
            )

        log_pts = np.log10(np.array(unique_f, dtype=np.float64))
        db_pts = np.clip(np.array(unique_db, dtype=np.float64), min_db, max_db)

        q_safe = np.maximum(np.asarray(query_freqs, dtype=np.float64), 10.0)
        log_q = np.log10(q_safe)

        if len(unique_f) >= 3:
            interpolator = interpolate.PchipInterpolator(
                log_pts, db_pts, extrapolate=True
            )
            interp_db = interpolator(log_q)
        else:
            interp_db = np.interp(log_q, log_pts, db_pts)

        interp_db = np.where(log_q <= log_pts[0], db_pts[0], interp_db)
        interp_db = np.where(log_q >= log_pts[-1], db_pts[-1], interp_db)
        return np.clip(interp_db, min_db, max_db)

    @staticmethod
    def apply_eq_curve_block(
        audio_block: np.ndarray,
        sample_rate: int,
        eq_points: List[List[float]],
    ) -> np.ndarray:
        if audio_block.size == 0 or not eq_points:
            return audio_block

        n_samples = audio_block.shape[0]
        freqs = np.fft.rfftfreq(n_samples, d=1.0 / float(sample_rate))
        gain_db = AudioDSPEngine.evaluate_smooth_curve_db(
            eq_points, freqs, min_db=-24.0, max_db=24.0
        )

        max_boost_db = float(np.max(gain_db))
        low_mask = freqs <= 280.0
        low_boost_db = float(np.max(gain_db[low_mask])) if np.any(low_mask) else 0.0
        headroom_db = 0.0
        if low_boost_db > 3.0:
            headroom_db -= (low_boost_db - 3.0) * 0.55
        elif max_boost_db > 4.0:
            headroom_db -= (max_boost_db - 4.0) * 0.40

        gain_linear = (10.0 ** ((gain_db + headroom_db) / 20.0)).astype(np.float32)

        if audio_block.ndim == 1:
            spec = np.fft.rfft(audio_block)
            filtered = np.fft.irfft(spec * gain_linear, n=n_samples).astype(np.float32)
            return AudioDSPEngine.soft_knee_limiter(filtered)

        out = np.empty_like(audio_block, dtype=np.float32)
        for ch in range(audio_block.shape[1]):
            spec = np.fft.rfft(audio_block[:, ch])
            out[:, ch] = np.fft.irfft(spec * gain_linear, n=n_samples)
        return AudioDSPEngine.soft_knee_limiter(out)

    @staticmethod
    def live_resample_effect_block(
        audio_block: np.ndarray,
        orig_sr: int,
        target_sr: int,
        target_bit_depth: int,
        target_channels: int,
    ) -> np.ndarray:
        if audio_block.size == 0:
            return audio_block

        work = audio_block.copy()
        n_frames = work.shape[0]

        if target_channels == 1 and work.ndim == 2 and work.shape[1] >= 2:
            mono = np.mean(work, axis=1, keepdims=True)
            work = np.repeat(mono, work.shape[1], axis=1)

        if target_sr != orig_sr and target_sr >= 2000 and n_frames >= 16:
            gcd = math.gcd(int(orig_sr), int(target_sr))
            up = int(target_sr) // gcd
            down = int(orig_sr) // gcd
            if up <= 64 and down <= 64:
                resampled = signal.resample_poly(work, up, down, axis=0)
                work = signal.resample_poly(resampled, down, up, axis=0)
            else:
                n_mid = max(4, int(round(n_frames * float(target_sr) / float(orig_sr))))
                resampled = signal.resample(work, n_mid, axis=0)
                work = signal.resample(resampled, n_frames, axis=0)

            if work.shape[0] > n_frames:
                work = work[:n_frames]
            elif work.shape[0] < n_frames:
                pad = np.zeros((n_frames - work.shape[0], work.shape[1]), dtype=np.float32)
                work = np.vstack([work, pad])

        work = np.clip(work, -1.0, 1.0)
        if target_bit_depth == 8:
            work = np.round(work * 127.0) / 127.0
        elif target_bit_depth == 16:
            work = np.round(work * 32767.0) / 32767.0
        elif target_bit_depth == 24:
            work = np.round(work * 8388607.0) / 8388607.0

        return work.astype(np.float32)

    @staticmethod
    def offline_resample(
        audio_data: np.ndarray,
        orig_sr: int,
        target_sr: int,
        target_bit_depth: int,
        target_channels: int,
    ) -> np.ndarray:
        work = audio_data.astype(np.float32)
        if work.ndim == 1:
            work = np.column_stack([work, work])

        if target_channels == 1:
            work = np.mean(work, axis=1)
        elif work.shape[1] == 1 and target_channels == 2:
            work = np.column_stack([work[:, 0], work[:, 0]])

        if target_sr != orig_sr:
            gcd = math.gcd(int(orig_sr), int(target_sr))
            up = int(target_sr) // gcd
            down = int(orig_sr) // gcd
            work = signal.resample_poly(work, up, down, axis=0)

        work = np.clip(work, -1.0, 1.0)
        if target_bit_depth == 8:
            work = np.round(work * 127.0) / 127.0
        elif target_bit_depth == 16:
            work = np.round(work * 32767.0) / 32767.0
        elif target_bit_depth == 24:
            work = np.round(work * 8388607.0) / 8388607.0

        return work.astype(np.float32)

    @staticmethod
    def eval_waveform_or_formula(
        phase_rad: np.ndarray,
        waveform: str = "sine",
        custom_formula: str = "sin(x)",
    ) -> np.ndarray:
        w = waveform.lower().strip()
        if w == "sine":
            return np.sin(phase_rad)
        elif w == "square":
            return np.where(np.sin(phase_rad) >= 0.0, 1.0, -1.0)
        elif w == "triangle":
            return (2.0 / np.pi) * np.arcsin(np.sin(phase_rad))
        elif w == "sawtooth":
            norm = (phase_rad / (2.0 * np.pi)) % 1.0
            return 2.0 * norm - 1.0
        elif w == "noise":
            rng = np.random.default_rng()
            return rng.uniform(-1.0, 1.0, size=phase_rad.shape)

        expr = (custom_formula or "sin(x)").strip()
        safe_env = {
            "__builtins__": {},
            "x": phase_rad,
            "t": (phase_rad / (2.0 * np.pi)) % 1.0,
            "pi": np.pi,
            "e": np.e,
            "sin": np.sin,
            "cos": np.cos,
            "tan": np.tan,
            "arcsin": np.arcsin,
            "arccos": np.arccos,
            "arctan": np.arctan,
            "sinh": np.sinh,
            "cosh": np.cosh,
            "tanh": np.tanh,
            "exp": np.exp,
            "log": np.log,
            "sqrt": np.sqrt,
            "abs": np.abs,
            "sign": np.sign,
            "mod": np.mod,
            "floor": np.floor,
            "ceil": np.ceil,
            "square": lambda p: np.where(np.sin(p) >= 0.0, 1.0, -1.0),
            "triangle": lambda p: (2.0 / np.pi) * np.arcsin(np.sin(p)),
            "sawtooth": lambda p: 2.0 * ((p / (2.0 * np.pi)) % 1.0) - 1.0,
        }
        result = eval(expr, safe_env, {})  # noqa: S307
        if np.isscalar(result):
            arr = np.full_like(phase_rad, float(result), dtype=np.float64)
        else:
            arr = np.asarray(result, dtype=np.float64)
        arr = np.nan_to_num(arr, nan=0.0, posinf=1.0, neginf=-1.0)
        peak = float(np.max(np.abs(arr)))
        if peak > 1e-6:
            arr = arr / peak
        return arr

    @staticmethod
    def synthesize_harmonic_curve_block(
        n_frames: int,
        sample_rate: int,
        fundamental_hz: float,
        num_harmonics: int,
        envelope_points: List[List[float]],
        phase_state: np.ndarray,
        base_waveform: str = "sine",
        custom_formula: str = "sin(x)",
    ) -> Tuple[np.ndarray, List[Tuple[float, float]]]:
        nyquist = sample_rate * 0.48
        k_indices = np.arange(1, max(1, int(num_harmonics)) + 1, dtype=np.float64)
        harmonic_freqs = k_indices * float(fundamental_hz)
        valid_mask = harmonic_freqs <= nyquist

        if not np.any(valid_mask):
            return np.zeros((n_frames, 2), dtype=np.float32), []

        active_freqs = harmonic_freqs[valid_mask]
        active_dbs = AudioDSPEngine.evaluate_smooth_curve_db(
            envelope_points, active_freqs, min_db=-80.0, max_db=0.0
        )
        active_amps = np.where(active_dbs <= -78.0, 0.0, 10.0 ** (active_dbs / 20.0))

        t_steps = np.arange(n_frames, dtype=np.float64) / float(sample_rate)
        mixed = np.zeros(n_frames, dtype=np.float64)

        for idx, (f_hz, amp) in enumerate(zip(active_freqs, active_amps)):
            if idx >= len(phase_state):
                break
            phi0 = phase_state[idx]
            phases = phi0 + 2.0 * np.pi * f_hz * t_steps
            if amp > 1e-5:
                if base_waveform == "sine":
                    mixed += amp * np.sin(phases)
                else:
                    mixed += amp * AudioDSPEngine.eval_waveform_or_formula(
                        phases, base_waveform, custom_formula
                    )
            phase_state[idx] = float(
                (phi0 + 2.0 * np.pi * f_hz * (n_frames / float(sample_rate))) % (2.0 * np.pi)
            )

        peak = float(np.max(np.abs(mixed)))
        if peak > 0.95:
            mixed = mixed * (0.95 / peak)

        stereo = np.column_stack([mixed, mixed]).astype(np.float32)
        harmonic_info = [
            (float(f), float(db)) for f, db in zip(active_freqs, active_dbs)
        ]
        return stereo, harmonic_info

    @staticmethod
    def synthesize_multi_osc_block(
        n_frames: int,
        sample_rate: int,
        oscillators: List[OscillatorItem],
        phase_state: np.ndarray,
    ) -> np.ndarray:
        t_steps = np.arange(n_frames, dtype=np.float64) / float(sample_rate)
        mixed = np.zeros(n_frames, dtype=np.float64)

        for idx, osc in enumerate(oscillators):
            if idx >= len(phase_state) or not osc.enabled:
                continue
            amp = 0.0 if osc.gain_db <= -78.0 else 10.0 ** (osc.gain_db / 20.0)
            phi0 = phase_state[idx] + math.radians(osc.phase_deg)
            phases = phi0 + 2.0 * np.pi * osc.freq_hz * t_steps
            try:
                wave_vals = AudioDSPEngine.eval_waveform_or_formula(
                    phases, osc.waveform, osc.formula
                )
                mixed += amp * wave_vals
            except Exception:
                pass
            phase_state[idx] = float(
                (phase_state[idx] + 2.0 * np.pi * osc.freq_hz * (n_frames / float(sample_rate)))
                % (2.0 * np.pi)
            )

        peak = float(np.max(np.abs(mixed)))
        if peak > 0.95:
            mixed = mixed * (0.95 / peak)

        return np.column_stack([mixed, mixed]).astype(np.float32)

    @staticmethod
    def synthesize_custom_formula_block(
        n_frames: int,
        sample_rate: int,
        fundamental_hz: float,
        custom_formula: str,
        phase_state: np.ndarray,
        gain_db: float = -6.0,
    ) -> np.ndarray:
        t_steps = np.arange(n_frames, dtype=np.float64) / float(sample_rate)
        phi0 = float(phase_state[0])
        phases = phi0 + 2.0 * np.pi * float(fundamental_hz) * t_steps
        try:
            wave_vals = AudioDSPEngine.eval_waveform_or_formula(
                phases, "custom", custom_formula
            )
        except Exception:
            wave_vals = np.sin(phases)

        phase_state[0] = float(
            (phi0 + 2.0 * np.pi * float(fundamental_hz) * (n_frames / float(sample_rate)))
            % (2.0 * np.pi)
        )
        amp = 10.0 ** (float(gain_db) / 20.0)
        sig = np.clip(wave_vals * amp, -0.95, 0.95).astype(np.float32)
        return np.column_stack([sig, sig])


# ===========================================================================
# 4. Windows WASAPI Loopback 系统内录采集线程
# ===========================================================================

class SystemLoopbackCapture:
    """捕获当前 Windows 电脑正在播放的媒体声音（WASAPI Loopback）"""

    def __init__(self) -> None:
        self.running = False
        self.thread: Optional[threading.Thread] = None
        self.sample_rate: int = 48000
        self.channels: int = 2
        self.device_name: str = "未启动"
        self._latest_chunk: np.ndarray = np.zeros((2048, 2), dtype=np.float32)
        self._lock = threading.Lock()
        self.on_audio_chunk_callback = None

    def start(self, callback=None) -> Tuple[bool, str]:
        if self.running:
            return True, f"正在监听: {self.device_name}"
        self.on_audio_chunk_callback = callback
        self.running = True

        if HAS_PYAUDIOWPATCH:
            self.thread = threading.Thread(target=self._run_pyaudiowpatch, daemon=True)
            self.thread.start()
            time.sleep(0.15)
            return True, f"WASAPI Loopback 已启动 ({self.device_name})"

        if HAS_SOUNDDEVICE:
            self.thread = threading.Thread(target=self._run_sounddevice_fallback, daemon=True)
            self.thread.start()
            time.sleep(0.15)
            return True, f"音频回环捕获已启动 ({self.device_name})"

        self.running = False
        return False, "未检测到 PyAudioWPatch 或 sounddevice 库"

    def stop(self) -> None:
        self.running = False
        if self.thread and self.thread.is_alive():
            self.thread.join(timeout=0.8)
        self.thread = None

    def get_latest_chunk(self) -> Tuple[np.ndarray, int, int]:
        with self._lock:
            return self._latest_chunk.copy(), self.sample_rate, self.channels

    def _run_pyaudiowpatch(self) -> None:
        p = pyaudio.PyAudio()
        stream = None
        try:
            loopback_dev = p.get_default_wasapi_loopback()
            self.sample_rate = int(loopback_dev["defaultSampleRate"])
            self.channels = max(1, min(2, int(loopback_dev["maxInputChannels"])))
            self.device_name = str(loopback_dev.get("name", "WASAPI Loopback"))
            chunk_frames = 2048

            stream = p.open(
                format=pyaudio.paFloat32,
                channels=self.channels,
                rate=self.sample_rate,
                frames_per_buffer=chunk_frames,
                input=True,
                input_device_index=loopback_dev["index"],
            )
            while self.running:
                raw = stream.read(chunk_frames, exception_on_overflow=False)
                arr = np.frombuffer(raw, dtype=np.float32)
                if self.channels > 1:
                    arr = arr.reshape(-1, self.channels)
                else:
                    arr = np.column_stack([arr, arr])
                with self._lock:
                    self._latest_chunk = arr
                if self.on_audio_chunk_callback:
                    self.on_audio_chunk_callback(arr, self.sample_rate)
        except Exception as exc:
            self.device_name = f"WASAPI 异常: {exc}"
        finally:
            if stream is not None:
                try:
                    stream.stop_stream()
                    stream.close()
                except Exception:
                    pass
            p.terminate()

    def _run_sounddevice_fallback(self) -> None:
        try:
            devices = sd.query_devices()
            target_idx = None
            for idx, dev in enumerate(devices):
                name_lower = str(dev.get("name", "")).lower()
                if dev.get("max_input_channels", 0) > 0 and (
                    "loopback" in name_lower
                    or "stereo mix" in name_lower
                    or "立体声混音" in name_lower
                    or "what u hear" in name_lower
                ):
                    target_idx = idx
                    self.device_name = dev["name"]
                    self.sample_rate = int(dev.get("default_samplerate", 48000))
                    self.channels = min(2, int(dev.get("max_input_channels", 2)))
                    break

            if target_idx is None:
                default_in = sd.query_devices(kind="input")
                self.device_name = str(default_in.get("name", "默认音频输入"))
                self.sample_rate = int(default_in.get("default_samplerate", 44100))
                self.channels = min(2, max(1, int(default_in.get("max_input_channels", 1))))

            def _cb(indata, frames, time_info, status):
                arr = indata.copy().astype(np.float32)
                if arr.ndim == 1 or arr.shape[1] == 1:
                    col = arr.reshape(-1)
                    arr = np.column_stack([col, col])
                with self._lock:
                    self._latest_chunk = arr
                if self.on_audio_chunk_callback:
                    self.on_audio_chunk_callback(arr, self.sample_rate)

            with sd.InputStream(
                device=target_idx,
                channels=self.channels,
                samplerate=self.sample_rate,
                blocksize=2048,
                dtype="float32",
                callback=_cb,
            ):
                while self.running:
                    time.sleep(0.05)
        except Exception as exc:
            self.device_name = f"输入流异常: {exc}"


# ===========================================================================
# 5. 一键打包免安装独立程序 (.exe) 工具
# ===========================================================================

def build_standalone_exe_package() -> Tuple[bool, str]:
    if importlib.util.find_spec("PyInstaller") is None:
        try:
            subprocess.check_call([sys.executable, "-m", "pip", "install", "pyinstaller>=6.0.0"])
        except Exception as exc:
            return False, f"安装 PyInstaller 失败: {exc}"

    script_path = Path(__file__).resolve()
    root_dir = script_path.parent
    icon_file = root_dir / "app_icon.ico"
    icon_args = ["--icon", str(icon_file), "--add-data", f"{icon_file}{os.pathsep}."] if icon_file.exists() else []
    cmd = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--windowed",
        "--onedir",
        "--name",
        "AudioStudioPro",
        *icon_args,
        "--collect-all",
        "soundfile",
        "--collect-all",
        "imageio_ffmpeg",
        "--collect-all",
        "pyqtgraph",
        str(script_path),
    ]
    proc = subprocess.run(
        cmd, cwd=str(root_dir), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True
    )
    if proc.returncode == 0:
        out_dir = root_dir / "dist" / "AudioStudioPro"
        return True, f"免安装独立程序已打包完成！\n输出目录: {out_dir}\n您可以直接将该文件夹压缩发给任何人双击 AudioStudioPro.exe 使用。"
    return False, f"打包过程出错:\n{proc.stdout[-600:]}"


# ===========================================================================
# 6. 88 键标准钢琴可视化控件 + 频率刻度与波峰 0~3 音名标注器 + 可拖拽曲线组件
# ===========================================================================

def build_freq_and_piano_axis_ticks(use_log: bool = True) -> List[List[Tuple[float, str]]]:
    major_items = [
        (16.35, "16.4Hz\n[C0]"),
        (32.70, "32.7Hz\n[C1]"),
        (65.41, "65.4Hz\n[C2]"),
        (130.81, "130.8Hz\n[C3]"),
        (261.63, "261.6Hz\n[C4]"),
        (440.00, "440Hz\n[A4]"),
        (523.25, "523.3Hz\n[C5]"),
        (1046.50, "1.05kHz\n[C6]"),
        (2093.00, "2.09kHz\n[C7]"),
        (4186.01, "4.19kHz\n[C8]"),
        (8000.0, "8kHz"),
        (16000.0, "16kHz"),
    ]
    ticks = []
    for f_hz, label in major_items:
        x_val = math.log10(f_hz) if use_log else f_hz
        ticks.append((x_val, label))
    return [ticks]


if HAS_QT:


    class DisclaimerAgreementDialog(QDialog):
        """首次启动软件时弹出的强制免责协议确认窗口（勾选同意后才允许进入软件）"""

        def __init__(self, parent=None, require_checkbox: bool = True) -> None:
            super().__init__(parent=parent)
            self.setWindowTitle("📜 软件使用免责协议与风险告知书 (DISCLAIMER.md)")
            self.resize(720, 540)
            self.setModal(True)
            self.setStyleSheet(
                """
                QDialog {
                    background-color: #0F172A;
                    color: #F1F5F9;
                    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
                }
                QTextEdit {
                    background-color: #141824;
                    color: #E2E8F0;
                    border: 1px solid #334155;
                    border-radius: 6px;
                    padding: 10px;
                    font-size: 13px;
                    line-height: 1.5;
                }
                QCheckBox {
                    color: #FBBF24;
                    font-size: 13px;
                    font-weight: bold;
                    spacing: 8px;
                }
                QPushButton {
                    border-radius: 6px;
                    padding: 7px 16px;
                    font-size: 13px;
                    font-weight: bold;
                }
                """
            )

            vbox = QVBoxLayout(self)
            vbox.setContentsMargins(16, 14, 16, 14)
            vbox.setSpacing(10)

            lbl_title = QLabel("⚠️ 欢迎使用 Audio Studio Pro — 请在开始使用前仔细阅读并同意以下免责协议：")
            lbl_title.setStyleSheet("color: #38BDF8; font-size: 14px; font-weight: bold;")
            vbox.addWidget(lbl_title)

            self.text_box = QTextEdit()
            self.text_box.setReadOnly(True)
            self.text_box.setPlainText(DISCLAIMER_DOCUMENT_TEXT.strip())
            vbox.addWidget(self.text_box, stretch=1)

            self.chk_agree = QCheckBox(
                "我已仔细阅读《DISCLAIMER.md 免责协议》，完全理解并同意遵守上述全部协议条款，自愿承担使用本软件的一切风险（使用本软件发生任何事情均与作者无关）"
            )
            self.chk_agree.setChecked(not require_checkbox)
            vbox.addWidget(self.chk_agree)

            btn_row = QHBoxLayout()
            btn_row.addStretch()

            if require_checkbox:
                self.btn_exit = QPushButton("❌ 不同意并退出程序")
                self.btn_exit.setStyleSheet("background-color: #991B1B; color: #FFFFFF; border: 1px solid #F87171;")
                self.btn_exit.clicked.connect(self.reject)
                btn_row.addWidget(self.btn_exit)

                self.btn_accept = QPushButton("✅ 我同意并进入软件")
                self.btn_accept.setEnabled(False)
                self.btn_accept.setStyleSheet(
                    "QPushButton:enabled { background-color: #059669; color: #FFFFFF; border: 1px solid #34D399; } "
                    "QPushButton:disabled { background-color: #1E293B; color: #64748B; border: 1px solid #334155; }"
                )
                self.chk_agree.toggled.connect(self.btn_accept.setEnabled)
                self.btn_accept.clicked.connect(self._on_accept_clicked)
                btn_row.addWidget(self.btn_accept)
            else:
                self.chk_agree.setVisible(False)
                btn_close = QPushButton("关闭窗口")
                btn_close.setStyleSheet("background-color: #0284C7; color: #FFFFFF; border: 1px solid #38BDF8;")
                btn_close.clicked.connect(self.accept)
                btn_row.addWidget(btn_close)

            vbox.addLayout(btn_row)

        def _on_accept_clicked(self) -> None:
            if self.chk_agree.isChecked():
                save_disclaimer_accepted()
                self.accept()


    class BeatFlashSquareWidget(QWidget):
        """
        极轻量级 16x16 小方块节拍闪烁指示灯：
        - 不使用任何进度条、不使用第几拍文字、不调用高开销的 setStyleSheet。
        - 仅在每拍触发瞬间点亮 110ms 后自动熄灭，其余时间 0 重绘、0 CPU 占用。
        """

        def __init__(self, parent=None) -> None:
            super().__init__(parent=parent)
            self.setFixedSize(16, 16)
            self.setToolTip("节拍闪烁灯：每次抓拍瞬间闪烁一下")
            self._lit: bool = False
            self._off_timer = QTimer(self)
            self._off_timer.setSingleShot(True)
            self._off_timer.timeout.connect(self._turn_off)

        def flash(self, duration_ms: int = 110) -> None:
            if not self._lit:
                self._lit = True
                self.update()
            self._off_timer.start(duration_ms)

        def _turn_off(self) -> None:
            if self._lit:
                self._lit = False
                self.update()

        def reset_off(self) -> None:
            self._off_timer.stop()
            if self._lit:
                self._lit = False
                self.update()

        def paintEvent(self, event) -> None:
            painter = QPainter(self)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)
            if self._lit:
                painter.setBrush(QBrush(QColor("#10B981")))
                painter.setPen(QPen(QColor("#A7F3D0"), 1.5))
            else:
                painter.setBrush(QBrush(QColor("#1E293B")))
                painter.setPen(QPen(QColor("#475569"), 1.2))
            painter.drawRoundedRect(QRectF(1.5, 1.5, 13.0, 13.0), 3.0, 3.0)
            painter.end()


    class Piano88KeysWidget(QWidget):
        """
        88 键标准钢琴键盘可视化与交互发声组件（MIDI 21 = A0 到 MIDI 108 = C8，共 52 白键 + 36 黑键）：
        - 缓存 88 个琴键几何矩形，仅在窗口尺寸变化时重算，避免重绘分配开销。
        - 仅在主音发生变化或到达节拍抓拍点时才触发重绘，拍内保持期间 0 重绘、不卡顿。
        - 支持鼠标左键点击任意白键或黑键直接弹响该琴键的标准钢琴音色。
        """

        keyClicked = pyqtSignal(int, float, str)  # (midi_note, freq_hz, note_name)

        WHITE_SEMITONES = {0, 2, 4, 5, 7, 9, 11}

        def __init__(self, parent=None) -> None:
            super().__init__(parent=parent)
            self.setMinimumHeight(185)
            self.setMinimumWidth(320)
            self.setMouseTracking(True)

            self._active_peaks: List[Dict[str, object]] = []
            self._active_signature: Tuple = ()
            self._clicked_midi: Optional[int] = None
            self._hover_midi: Optional[int] = None

            self._white_midis: List[int] = []
            self._black_midis: List[int] = []
            self._midi_to_white_index: Dict[int, float] = {}

            w_idx = 0
            for midi in range(21, 109):
                semi = midi % 12
                if semi in self.WHITE_SEMITONES:
                    self._white_midis.append(midi)
                    self._midi_to_white_index[midi] = float(w_idx)
                    w_idx += 1
                else:
                    self._black_midis.append(midi)
                    self._midi_to_white_index[midi] = float(w_idx) - 0.5

            self._cached_size: Tuple[int, int] = (0, 0)
            self._cached_rects: Optional[Tuple[Dict[int, QRectF], Dict[int, QRectF], float, float]] = None

        def set_highlighted_peaks(self, peaks: List[Dict[str, object]], force: bool = False) -> None:
            trimmed = list(peaks[:3]) if peaks else []
            sig = tuple(
                (
                    int(p.get("midi", 0)),
                    str(p.get("note", "")),
                    round(float(p.get("freq_hz", 0.0)), 1),
                    round(float(p.get("mag_db", 0.0)), 1),
                )
                for p in trimmed
            )
            if not force and sig == self._active_signature:
                return
            self._active_signature = sig
            self._active_peaks = trimmed
            if self.isVisible():
                self.update()

        def showEvent(self, event) -> None:
            super().showEvent(event)
            self.update()

        def set_clicked_midi(self, midi: Optional[int]) -> None:
            if self._clicked_midi != midi:
                self._clicked_midi = midi
                self.update()

        def _compute_key_rects(self) -> Tuple[Dict[int, QRectF], Dict[int, QRectF], float, float]:
            cur_size = (self.width(), self.height())
            if self._cached_rects is not None and self._cached_size == cur_size:
                return self._cached_rects

            w = float(cur_size[0])
            h = float(cur_size[1])

            margin_x = 8.0
            top_header_h = 70.0
            kb_y = top_header_h
            kb_h = max(72.0, h - top_header_h - 8.0)
            kb_w = max(180.0, w - 2.0 * margin_x)

            n_whites = len(self._white_midis)  # 52
            white_w = kb_w / float(n_whites)
            black_w = white_w * 0.64
            black_h = kb_h * 0.62

            white_rects: Dict[int, QRectF] = {}
            for i, midi in enumerate(self._white_midis):
                x = margin_x + i * white_w
                white_rects[midi] = QRectF(x, kb_y, white_w, kb_h)

            black_rects: Dict[int, QRectF] = {}
            for midi in self._black_midis:
                center_idx = self._midi_to_white_index[midi]
                bx = margin_x + (center_idx + 0.5) * white_w - black_w * 0.5
                black_rects[midi] = QRectF(bx, kb_y, black_w, black_h)

            self._cached_size = cur_size
            self._cached_rects = (white_rects, black_rects, kb_y, kb_h)
            return self._cached_rects

        def _hit_test_midi(self, x: float, y: float) -> Optional[int]:
            white_rects, black_rects, _, _ = self._compute_key_rects()
            for midi, rect in black_rects.items():
                if rect.contains(x, y):
                    return midi
            for midi, rect in white_rects.items():
                if rect.contains(x, y):
                    return midi
            return None

        def mousePressEvent(self, event) -> None:
            if event.button() == Qt.MouseButton.LeftButton:
                pos = event.position()
                midi = self._hit_test_midi(pos.x(), pos.y())
                if midi is not None:
                    self._clicked_midi = midi
                    freq_hz = 440.0 * (2.0 ** ((midi - 69) / 12.0))
                    note_name = AudioDSPEngine.midi_to_note_name(midi)
                    self.keyClicked.emit(midi, freq_hz, note_name)
                    self.update()

        def mouseReleaseEvent(self, event) -> None:
            if self._clicked_midi is not None:
                self._clicked_midi = None
                self.update()

        def mouseMoveEvent(self, event) -> None:
            pos = event.position()
            midi = self._hit_test_midi(pos.x(), pos.y())
            if midi != self._hover_midi:
                self._hover_midi = midi
                self.update()

        def leaveEvent(self, event) -> None:
            if self._hover_midi is not None:
                self._hover_midi = None
                self.update()

        def paintEvent(self, event) -> None:
            painter = QPainter(self)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing, True)

            w = float(self.width())
            painter.fillRect(self.rect(), QColor("#111622"))

            white_rects, black_rects, kb_y, kb_h = self._compute_key_rects()

            font_legend = QFont("Microsoft YaHei UI", 8)
            font_legend.setBold(True)
            painter.setFont(font_legend)
            painter.setPen(QColor("#E2E8F0"))

            if w >= 680:
                title_txt = "88键钢琴 (A0~C8)"
            else:
                title_txt = "88键钢琴 (A0~C8)"
            painter.drawText(10, 17, title_txt)

            legend_items = [
                (PEAK_RANK_COLORS[0], "#1主音"),
                (PEAK_RANK_COLORS[1], "#2次音"),
                (PEAK_RANK_COLORS[2], "#3泛音"),
                ("#10B981", "点击试音"),
            ]
            item_step = 68.0 if w < 620 else 76.0
            total_leg_w = len(legend_items) * item_step
            lx = max(210.0, w - total_leg_w - 8.0)
            for col_hex, label_txt in legend_items:
                if lx + 55.0 <= w:
                    painter.setBrush(QBrush(QColor(col_hex)))
                    painter.setPen(QPen(QColor("#FFFFFF"), 1.0))
                    painter.drawRoundedRect(QRectF(lx, 6, 11, 11), 2.5, 2.5)
                    painter.setPen(QColor(col_hex))
                    painter.drawText(int(lx + 15), 16, label_txt)
                lx += item_step

            highlight_map: Dict[int, Tuple[int, Dict[str, object]]] = {}
            for i, p in enumerate(self._active_peaks[:3]):
                m = int(p.get("midi", AudioDSPEngine.freq_to_midi(float(p["freq_hz"]))))
                if 21 <= m <= 108 and m not in highlight_map:
                    highlight_map[m] = (i, p)

            # 1. 绘制 52 个白键
            font_key = QFont("Microsoft YaHei UI", 7)
            painter.setFont(font_key)
            for midi in self._white_midis:
                rect = white_rects[midi]
                if midi in highlight_map:
                    rank_idx, _ = highlight_map[midi]
                    base_col = QColor(PEAK_RANK_COLORS[rank_idx])
                    painter.setBrush(QBrush(base_col))
                    painter.setPen(QPen(QColor("#FFFFFF"), 1.5))
                elif midi == self._clicked_midi:
                    painter.setBrush(QBrush(QColor("#10B981")))
                    painter.setPen(QPen(QColor("#FFFFFF"), 1.4))
                elif midi == self._hover_midi:
                    painter.setBrush(QBrush(QColor("#CBD5E1")))
                    painter.setPen(QPen(QColor("#38BDF8"), 1.2))
                else:
                    painter.setBrush(QBrush(QColor("#F1F5F9")))
                    painter.setPen(QPen(QColor("#334155"), 1.0))

                painter.drawRoundedRect(rect, 2.0, 2.0)

                note_str = AudioDSPEngine.midi_to_note_name(midi)
                if (midi % 12 == 0) or midi == 21 or (midi in highlight_map):
                    painter.setPen(QColor("#0F172A") if midi not in highlight_map else QColor("#000000"))
                    lbl_rect = QRectF(rect.left() - 2.0, rect.bottom() - 18.0, rect.width() + 4.0, 16.0)
                    painter.drawText(lbl_rect, Qt.AlignmentFlag.AlignCenter, note_str)

            # 2. 绘制 36 个黑键
            for midi in self._black_midis:
                rect = black_rects[midi]
                if midi in highlight_map:
                    rank_idx, _ = highlight_map[midi]
                    base_col = QColor(PEAK_RANK_COLORS[rank_idx])
                    painter.setBrush(QBrush(base_col))
                    painter.setPen(QPen(QColor("#FFFFFF"), 1.5))
                elif midi == self._clicked_midi:
                    painter.setBrush(QBrush(QColor("#059669")))
                    painter.setPen(QPen(QColor("#FFFFFF"), 1.3))
                elif midi == self._hover_midi:
                    painter.setBrush(QBrush(QColor("#475569")))
                    painter.setPen(QPen(QColor("#38BDF8"), 1.2))
                else:
                    painter.setBrush(QBrush(QColor("#1E293B")))
                    painter.setPen(QPen(QColor("#020617"), 1.0))

                painter.drawRoundedRect(rect, 1.8, 1.8)

            # 3. 在被点亮的主音琴键正上方绘制指示气泡与连接线
            font_badge = QFont("Microsoft YaHei UI", 8)
            font_badge.setBold(True)
            painter.setFont(font_badge)

            for midi, (rank_idx, p) in highlight_map.items():
                rect = black_rects[midi] if midi in black_rects else white_rects[midi]
                cx = rect.center().x()
                col = QColor(PEAK_RANK_COLORS[rank_idx])

                note_str = str(p["note"])
                f_hz = float(p["freq_hz"])
                db_val = float(p["mag_db"])
                badge_text = f"#{rank_idx + 1} {note_str} ({f_hz:.1f}Hz, {db_val:.1f}dB)"

                badge_w = 142.0
                badge_h = 20.0
                badge_y = 22.0 + rank_idx * 15.0
                badge_x = max(6.0, min(w - badge_w - 6.0, cx - badge_w * 0.5))

                painter.setPen(QPen(col, 1.5, Qt.PenStyle.DashLine))
                painter.drawLine(int(cx), int(badge_y + badge_h), int(cx), int(kb_y))

                badge_rect = QRectF(badge_x, badge_y, badge_w, badge_h)
                painter.setBrush(QBrush(QColor(15, 23, 42, 235)))
                painter.setPen(QPen(col, 1.4))
                painter.drawRoundedRect(badge_rect, 4.0, 4.0)

                painter.setPen(col)
                painter.drawText(badge_rect, Qt.AlignmentFlag.AlignCenter, badge_text)

            painter.end()

    def add_piano_c0_c8_guide_lines(plot_widget: pg.PlotWidget, use_log: bool = True) -> List:
        items = []
        for note_name, f_hz in PIANO_C0_TO_C8:
            x_pos = math.log10(f_hz) if use_log else f_hz
            line = pg.InfiniteLine(
                pos=x_pos,
                angle=90,
                pen=pg.mkPen(color=(148, 163, 184, 65), width=1, style=Qt.PenStyle.DotLine),
                label=f"{note_name}",
                labelOpts={
                    "position": 0.93,
                    "color": (251, 191, 36, 190),
                    "fill": (15, 23, 42, 160),
                },
            )
            plot_widget.addItem(line)
            items.append((line, f_hz))
        return items

    class TopPeaksAnnotator:
        """用于在频谱图的波峰顶端动态标注 0 / 1 / 2 / 3 个高音名的可视化辅助器"""

        def __init__(self, plot_widget: pg.PlotWidget, max_peaks: int = 3) -> None:
            self.plot_widget = plot_widget
            self.max_peaks = max_peaks
            self.scatter = pg.ScatterPlotItem(size=12, pen=pg.mkPen("#FFFFFF", width=1.5))
            self.plot_widget.addItem(self.scatter)

            self.text_items: List[pg.TextItem] = []
            for i in range(max_peaks):
                color_hex = PEAK_RANK_COLORS[i % len(PEAK_RANK_COLORS)]
                txt = pg.TextItem(
                    text="",
                    color=color_hex,
                    anchor=(0.5, 1.15),
                    fill=pg.mkBrush(15, 23, 42, 210),
                    border=pg.mkPen(color_hex, width=1.2),
                )
                font = QFont("Microsoft YaHei UI", 9)
                font.setBold(True)
                txt.setFont(font)
                txt.setVisible(False)
                self.plot_widget.addItem(txt)
                self.text_items.append(txt)

        def update_peaks(
            self,
            peaks: List[Dict[str, object]],
            use_log_x: bool = True,
            count_limit: int = 3,
        ) -> None:
            limit = max(0, min(self.max_peaks, int(count_limit)))
            active = peaks[:limit] if peaks and limit > 0 else []
            if not active:
                self.scatter.setData([], [])
                for txt in self.text_items:
                    txt.setVisible(False)
                return

            xs = []
            ys = []
            brushes = []
            for i, p in enumerate(active):
                f_hz = float(p["freq_hz"])
                db_val = float(p["mag_db"])
                note_str = str(p["note"])
                rank = int(p["rank"])
                x_pos = math.log10(max(MIN_PLOT_FREQ_HZ, f_hz)) if use_log_x else f_hz

                color_hex = PEAK_RANK_COLORS[i % len(PEAK_RANK_COLORS)]
                xs.append(x_pos)
                ys.append(db_val)
                brushes.append(pg.mkBrush(color_hex))

                txt_item = self.text_items[i]
                txt_item.setText(f"#{rank} {note_str}\n{f_hz:.1f}Hz ({db_val:.1f}dB)")
                txt_item.setPos(x_pos, min(2.0, db_val))
                txt_item.setVisible(True)

            for j in range(len(active), self.max_peaks):
                self.text_items[j].setVisible(False)

            self.scatter.setData(xs, ys, brush=brushes)

    class DraggableCurveWidget(pg.PlotWidget):
        """支持鼠标拖拽控制点、双击新增点、右键删点、空白处滚轮缩放横坐标的频率-分贝曲线组件"""

        curveChanged = pyqtSignal(list)

        def __init__(
            self,
            title: str,
            min_db: float = -24.0,
            max_db: float = 24.0,
            line_color: str = "#00E5FF",
            fill_color: Tuple[int, int, int, int] = (0, 229, 255, 45),
            parent=None,
        ) -> None:
            super().__init__(parent=parent)
            self.min_db = min_db
            self.max_db = max_db
            self.line_color = line_color
            self.fill_color = fill_color
            self.use_log_x: bool = True

            self._points: List[List[float]] = []
            self._dragging_idx: Optional[int] = None

            self.setBackground("#141824")
            self.showGrid(x=True, y=True, alpha=0.25)
            self.setTitle(title, color="#E2E8F0", size="10.5pt")
            self.setLabel("bottom", "频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]")
            self.setLabel("left", "分贝 (dB)")

            self.getAxis("left").setWidth(68)

            self.setXRange(math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ), padding=0.01)
            self.setYRange(self.min_db, self.max_db, padding=0.05)
            self.setMouseEnabled(x=True, y=False)
            self.getPlotItem().vb.setLimits(
                xMin=math.log10(12.0),
                xMax=math.log10(24000.0),
            )
            self.hideButtons()

            self.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=True))
            self._piano_lines = add_piano_c0_c8_guide_lines(self, use_log=True)

            if self.min_db < 0.0 <= self.max_db:
                zero_line = pg.InfiniteLine(
                    pos=0.0,
                    angle=0,
                    pen=pg.mkPen(color="#64748B", width=1, style=Qt.PenStyle.DashLine),
                )
                self.addItem(zero_line)

            self.smooth_curve = self.plot(
                [],
                [],
                pen=pg.mkPen(color=self.line_color, width=2.8),
                fillLevel=self.min_db,
                brush=pg.mkBrush(*self.fill_color),
            )

            self.harmonic_stems = pg.BarGraphItem(
                x=[], height=[], width=0.016, y0=self.min_db, brush=pg.mkBrush(255, 171, 0, 130)
            )
            self.addItem(self.harmonic_stems)

            self.scatter_item = pg.ScatterPlotItem(
                size=13,
                pen=pg.mkPen("#FFFFFF", width=1.8),
                brush=pg.mkBrush("#FF4081"),
                hoverable=True,
                hoverSize=16,
            )
            self.addItem(self.scatter_item)

            self.scene().installEventFilter(self)

        def _freq_to_x(self, f_hz: float) -> float:
            f_safe = max(MIN_PLOT_FREQ_HZ, float(f_hz))
            return math.log10(f_safe) if self.use_log_x else f_safe

        def _x_to_freq(self, x_val: float) -> float:
            if self.use_log_x:
                return float(10.0 ** np.clip(x_val, math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ)))
            return float(np.clip(x_val, MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ))

        def set_axis_log_mode(self, use_log: bool) -> None:
            self.use_log_x = use_log
            vb = self.getPlotItem().vb
            if use_log:
                vb.setLimits(xMin=math.log10(12.0), xMax=math.log10(24000.0))
                self.setXRange(math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ), padding=0.01)
            else:
                vb.setLimits(xMin=10.0, xMax=24000.0)
                self.setXRange(MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ, padding=0.01)

            self.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=use_log))
            for line, f_hz in self._piano_lines:
                line.setPos(math.log10(f_hz) if use_log else f_hz)
            self._refresh_visuals()

        def set_control_points(self, points: List[List[float]], emit_signal: bool = True) -> None:
            self._points = sorted(
                [
                    [
                        max(MIN_PLOT_FREQ_HZ, min(MAX_PLOT_FREQ_HZ, float(p[0]))),
                        max(self.min_db, min(self.max_db, float(p[1]))),
                    ]
                    for p in points
                ],
                key=lambda x: x[0],
            )
            self._refresh_visuals()
            if emit_signal:
                self.curveChanged.emit(self.get_control_points())

        def get_control_points(self) -> List[List[float]]:
            return [[round(p[0], 2), round(p[1], 2)] for p in self._points]

        def set_harmonic_indicators(self, harmonics: List[Tuple[float, float]]) -> None:
            if not harmonics:
                self.harmonic_stems.setOpts(x=[], height=[], y0=self.min_db)
                return
            xs = [self._freq_to_x(f) for f, _ in harmonics]
            hs = [max(0.0, db - self.min_db) for _, db in harmonics]
            bar_w = 0.015 if self.use_log_x else 80.0
            self.harmonic_stems.setOpts(x=xs, height=hs, y0=self.min_db, width=bar_w)

        def _refresh_visuals(self) -> None:
            if not self._points:
                self.smooth_curve.setData([], [])
                self.scatter_item.setData([], [])
                return

            if self.use_log_x:
                x_grid = np.linspace(math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ), 450)
                query_freqs = 10.0 ** x_grid
            else:
                x_grid = np.linspace(MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ, 450)
                query_freqs = x_grid

            interp_db = AudioDSPEngine.evaluate_smooth_curve_db(
                self._points, query_freqs, min_db=self.min_db, max_db=self.max_db
            )
            self.smooth_curve.setData(x_grid, interp_db)

            pt_x = [self._freq_to_x(p[0]) for p in self._points]
            pt_db_y = [p[1] for p in self._points]
            self.scatter_item.setData(pt_x, pt_db_y)

        def _find_nearest_point_index(self, scene_pos) -> Optional[int]:
            vb = self.getPlotItem().vb
            view_pos = vb.mapSceneToView(scene_pos)
            vx, vy = view_pos.x(), view_pos.y()

            vr = vb.viewRange()
            x_span = max(1e-6, vr[0][1] - vr[0][0])
            y_span = max(1.0, vr[1][1] - vr[1][0])

            best_idx = None
            best_dist = 999.0

            for i, (f_hz, db) in enumerate(self._points):
                px = self._freq_to_x(f_hz)
                dx = (px - vx) / x_span
                dy = (db - vy) / y_span
                dist = math.hypot(dx, dy)
                if dist < best_dist and dist < 0.045:
                    best_dist = dist
                    best_idx = i
            return best_idx

        def eventFilter(self, obj, event) -> bool:
            from PyQt6.QtCore import QEvent

            if obj is self.scene():
                et = event.type()
                if et == QEvent.Type.GraphicsSceneMousePress:
                    scene_pos = event.scenePos()
                    if self.getPlotItem().vb.sceneBoundingRect().contains(scene_pos):
                        if event.button() == Qt.MouseButton.LeftButton:
                            idx = self._find_nearest_point_index(scene_pos)
                            if idx is not None:
                                self._dragging_idx = idx
                                return True
                        elif event.button() == Qt.MouseButton.RightButton:
                            idx = self._find_nearest_point_index(scene_pos)
                            if idx is not None and len(self._points) > 2:
                                self._points.pop(idx)
                                self._refresh_visuals()
                                self.curveChanged.emit(self.get_control_points())
                                return True

                elif et == QEvent.Type.GraphicsSceneMouseDoubleClick:
                    scene_pos = event.scenePos()
                    if (
                        event.button() == Qt.MouseButton.LeftButton
                        and self.getPlotItem().vb.sceneBoundingRect().contains(scene_pos)
                    ):
                        view_pos = self.getPlotItem().vb.mapSceneToView(scene_pos)
                        new_freq = self._x_to_freq(view_pos.x())
                        new_db = float(np.clip(view_pos.y(), self.min_db, self.max_db))
                        self._points.append([new_freq, new_db])
                        self._points.sort(key=lambda p: p[0])
                        self._refresh_visuals()
                        self.curveChanged.emit(self.get_control_points())
                        return True

                elif et == QEvent.Type.GraphicsSceneMouseMove:
                    if self._dragging_idx is not None:
                        scene_pos = event.scenePos()
                        view_pos = self.getPlotItem().vb.mapSceneToView(scene_pos)
                        new_freq = self._x_to_freq(view_pos.x())
                        new_db = float(np.clip(view_pos.y(), self.min_db, self.max_db))

                        if self._dragging_idx > 0:
                            new_freq = max(new_freq, self._points[self._dragging_idx - 1][0] * 1.03)
                        if self._dragging_idx < len(self._points) - 1:
                            new_freq = min(new_freq, self._points[self._dragging_idx + 1][0] * 0.97)

                        self._points[self._dragging_idx] = [new_freq, new_db]
                        self._refresh_visuals()
                        self.curveChanged.emit(self.get_control_points())
                        return True

                elif et == QEvent.Type.GraphicsSceneMouseRelease:
                    if self._dragging_idx is not None:
                        self._dragging_idx = None
                        return True

            return super().eventFilter(obj, event)


# ===========================================================================
# 7. 主桌面应用程序窗口 (MainWindow)
# ===========================================================================

if HAS_QT:

    class AudioStudioMainWindow(QMainWindow):
        """Windows 音乐频谱分析、88键钢琴可视化(支持BPM按节拍抓主音)、瞬态截取延音、重采样、内录与泛音合成工作站主界面"""

        def __init__(self) -> None:
            super().__init__()
            self.setWindowTitle(
                "Audio Studio Pro"
            )
            self.setMinimumSize(920, 620)
            self.setAcceptDrops(True)

            self.preset = AppPresetConfig()

            self.current_file_path: Optional[str] = None
            self.audio_buffer: Optional[np.ndarray] = None
            self.orig_sample_rate: int = 44100
            self.play_cursor: int = 0
            self.is_playing_file: bool = False
            self.is_playing_synth: bool = False
            self.master_volume: float = 0.85

            self.playback_speed: float = 1.0
            self.speed_keep_pitch: bool = True

            self.analysis_source_mode: str = "file_or_synth"
            self.loopback_capture = SystemLoopbackCapture()

            self._latest_output_chunk = np.zeros((2048, 2), dtype=np.float32)
            self._has_rendered_live_frame: bool = False
            self._peak_hold_db: Optional[np.ndarray] = None
            self._latest_harmonics_info: List[Tuple[float, float]] = []
            self._latest_live_top3_peaks: List[Dict[str, object]] = []

            # BPM 节拍定时抓取状态（始终按用户设定的 BPM 绝对时间抓取）
            self._bpm_anchor_time: float = time.perf_counter()
            self._bpm_last_beat_index: int = -1
            self._bpm_locked_peaks: List[Dict[str, object]] = []
            self._tap_tempo_timestamps: List[float] = []

            self._rolling_history = np.zeros((24000, 2), dtype=np.float32)
            self._history_lock = threading.Lock()

            self.captured_snippet: Optional[np.ndarray] = None
            self.captured_context_audio: Optional[np.ndarray] = None
            self.captured_sr: int = 44100
            self.captured_top_peaks: List[Dict[str, object]] = []
            self.snapshot_play_buffer: Optional[np.ndarray] = None
            self.snapshot_play_cursor: int = 0
            self._snapshot_lock = threading.Lock()

            self._realtime_eq_processor = StatefulOverlapAddEQ(block_size=2048, channels=2)
            self._wsola_state: Dict[str, int] = {}

            self._harmonic_phases = np.zeros(64, dtype=np.float64)
            self._osc_phases = np.zeros(32, dtype=np.float64)
            self._custom_formula_phase = np.zeros(4, dtype=np.float64)

            self.is_recording: bool = False
            self.record_source_mode: str = "internal"
            self.recorded_chunks: List[np.ndarray] = []
            self.recorded_source_sr: int = 44100
            self.record_start_time: float = 0.0
            self._record_lock = threading.Lock()

            self.output_stream = None
            self.stream_sample_rate: int = 44100

            self._apply_dark_theme()
            self._build_ui()
            self._adapt_window_to_screen()
            self._apply_x_axis_sync(self.chk_sync_x_axes.isChecked())
            self._init_audio_output_stream()

            self.ui_timer = QTimer(self)
            self.ui_timer.setInterval(30)
            self.ui_timer.timeout.connect(self._on_ui_refresh_tick)
            self.ui_timer.start()

        def _adapt_window_to_screen(self) -> None:
            """自适应 16:9 与 4:3 显示器分辨率（含高 DPI 缩放），防止窗口过宽超出屏幕"""
            screen = QApplication.primaryScreen()
            if screen is not None:
                avail = screen.availableGeometry()
                target_w = min(1340, max(940, int(avail.width() * 0.94)))
                target_h = min(860, max(640, int(avail.height() * 0.92)))
                self.resize(target_w, target_h)
                x = avail.x() + max(0, (avail.width() - target_w) // 2)
                y = avail.y() + max(0, (avail.height() - target_h) // 2)
                self.move(x, y)
            else:
                self.resize(1220, 780)

        def _apply_dark_theme(self) -> None:
            self.setStyleSheet(
                """
                QMainWindow, QWidget {
                    background-color: #0F172A;
                    color: #F1F5F9;
                    font-family: "Microsoft YaHei UI", "Segoe UI", sans-serif;
                    font-size: 12px;
                }
                QGroupBox {
                    border: 1px solid #334155;
                    border-radius: 7px;
                    margin-top: 8px;
                    padding-top: 8px;
                    font-weight: bold;
                    color: #38BDF8;
                }
                QGroupBox::title {
                    subcontrol-origin: margin;
                    left: 10px;
                    padding: 0 5px;
                }
                QPushButton {
                    background-color: #1E293B;
                    border: 1px solid #475569;
                    border-radius: 5px;
                    padding: 4px 8px;
                    color: #F8FAFC;
                    font-weight: 600;
                }
                QPushButton:hover {
                    background-color: #334155;
                    border-color: #38BDF8;
                }
                QPushButton:checked {
                    background-color: #0284C7;
                    border-color: #38BDF8;
                    color: #FFFFFF;
                }
                QLineEdit, QComboBox, QSpinBox, QDoubleSpinBox {
                    background-color: #1E293B;
                    border: 1px solid #475569;
                    border-radius: 5px;
                    padding: 3px 6px;
                    color: #F8FAFC;
                }
                QProgressBar {
                    background-color: #1E293B;
                    border: 1px solid #475569;
                    border-radius: 4px;
                    text-align: center;
                    color: #FFFFFF;
                    font-weight: bold;
                    font-size: 11px;
                }
                QProgressBar::chunk {
                    background-color: #10B981;
                    border-radius: 3px;
                }
                QTabWidget::pane {
                    border: 1px solid #334155;
                    border-radius: 6px;
                    background-color: #0F172A;
                }
                QTabBar::tab {
                    background-color: #1E293B;
                    color: #94A3B8;
                    padding: 6px 12px;
                    border-top-left-radius: 6px;
                    border-top-right-radius: 6px;
                    margin-right: 3px;
                    font-weight: bold;
                }
                QTabBar::tab:selected {
                    background-color: #0284C7;
                    color: #FFFFFF;
                }
                QTableWidget {
                    background-color: #141824;
                    gridline-color: #334155;
                    border: 1px solid #334155;
                }
                QHeaderView::section {
                    background-color: #1E293B;
                    color: #38BDF8;
                    padding: 4px;
                    border: 1px solid #334155;
                }
                """
            )

        def _build_ui(self) -> None:
            central = QWidget()
            self.setCentralWidget(central)
            main_layout = QVBoxLayout(central)
            main_layout.setContentsMargins(8, 6, 8, 6)
            main_layout.setSpacing(6)

            top_bar = self._build_top_toolbar()
            main_layout.addWidget(top_bar)

            splitter = QSplitter(Qt.Orientation.Vertical)
            splitter.addWidget(self._build_spectrum_section())
            self.bottom_tabs = self._build_bottom_tabs()
            splitter.addWidget(self.bottom_tabs)
            splitter.setStretchFactor(0, 5)
            splitter.setStretchFactor(1, 5)
            main_layout.addWidget(splitter)

            self.status_bar = QStatusBar()
            self.setStatusBar(self.status_bar)
            self.status_bar.showMessage(
                "就绪"
            )

        def _build_top_toolbar(self) -> QGroupBox:
            box = QGroupBox("播放与控制")
            layout = QVBoxLayout(box)
            layout.setContentsMargins(8, 6, 8, 6)
            layout.setSpacing(5)

            # 第 1 行：核心播放、倍速、截取与进度条（紧凑排布，适配 4:3 与 16:9）
            row1 = QHBoxLayout()
            row1.setSpacing(6)

            self.btn_open_file = QPushButton("📂 导入音乐")
            self.btn_open_file.setToolTip("选择或直接拖入音乐文件（非 WAV 格式自动转换为同目录同名 .wav）")
            self.btn_open_file.clicked.connect(self.on_open_audio_file)
            row1.addWidget(self.btn_open_file)

            self.btn_play_pause = QPushButton("▶ 播放")
            self.btn_play_pause.setCheckable(True)
            self.btn_play_pause.clicked.connect(self.on_toggle_play_file)
            row1.addWidget(self.btn_play_pause)

            self.btn_stop = QPushButton("⏹ 停止")
            self.btn_stop.clicked.connect(self.on_stop_playback)
            row1.addWidget(self.btn_stop)

            row1.addWidget(QLabel("倍速:"))
            self.combo_play_speed = QComboBox()
            for spd_label, spd_val in PLAYBACK_SPEED_OPTIONS:
                self.combo_play_speed.addItem(spd_label, spd_val)
            self.combo_play_speed.setCurrentIndex(3)  # 默认 1.00x
            self.combo_play_speed.currentIndexChanged.connect(
                lambda: setattr(self, "playback_speed", float(self.combo_play_speed.currentData()))
            )
            row1.addWidget(self.combo_play_speed)

            self.chk_speed_keep_pitch = QCheckBox("保音高")
            self.chk_speed_keep_pitch.setChecked(True)
            self.chk_speed_keep_pitch.setToolTip("勾选后倍速播放不改变歌曲原有音高（变速不变调）；取消勾选则为磁带式变速变调")
            self.chk_speed_keep_pitch.toggled.connect(lambda v: setattr(self, "speed_keep_pitch", v))
            row1.addWidget(self.chk_speed_keep_pitch)

            self.btn_capture_snapshot_top = QPushButton("📸 截取当前声音")
            self.btn_capture_snapshot_top.setToolTip("截取当前瞬间前后的声音片段并跳转至第 4 页进行波形、主音与钢琴可视化分析")
            self.btn_capture_snapshot_top.setStyleSheet(
                "QPushButton { background-color: #7C3AED; border-color: #A78BFA; color: #FFFFFF; padding: 4px 9px; } "
                "QPushButton:hover { background-color: #6D28D9; }"
            )
            self.btn_capture_snapshot_top.clicked.connect(self.on_capture_audio_snapshot)
            row1.addWidget(self.btn_capture_snapshot_top)

            self.slider_seek = QSlider(Qt.Orientation.Horizontal)
            self.slider_seek.setRange(0, 1000)
            self.slider_seek.setMinimumWidth(90)
            self.slider_seek.sliderMoved.connect(self.on_seek_slider_moved)
            row1.addWidget(self.slider_seek, stretch=2)

            self.lbl_time = QLabel("00:00 / 00:00")
            row1.addWidget(self.lbl_time)

            row1.addWidget(QLabel("音量:"))
            self.slider_vol = QSlider(Qt.Orientation.Horizontal)
            self.slider_vol.setRange(0, 100)
            self.slider_vol.setValue(85)
            self.slider_vol.setFixedWidth(65)
            self.slider_vol.valueChanged.connect(lambda v: setattr(self, "master_volume", v / 100.0))
            row1.addWidget(self.slider_vol)

            layout.addLayout(row1)

            # 第 2 行：当前文件状态（自动省略防撑宽） + 分析源切换 + 预设导入导出 + 一键打包
            row2 = QHBoxLayout()
            row2.setSpacing(6)

            self.lbl_current_file = QLabel(
                "当前文件：未加载"
            )
            self.lbl_current_file.setStyleSheet("color: #94A3B8;")
            self.lbl_current_file.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
            row2.addWidget(self.lbl_current_file, stretch=2)

            self.btn_switch_source = QPushButton("分析源：软件内部")
            self.btn_switch_source.setCheckable(True)
            self.btn_switch_source.setToolTip("在【本软件内部播放/合成器声音】与【Windows电脑系统媒体声音 (WASAPI Loopback)】之间切换")
            self.btn_switch_source.setStyleSheet(
                "QPushButton { background-color: #0F766E; border-color: #14B8A6; } "
                "QPushButton:checked { background-color: #B45309; border-color: #F59E0B; }"
            )
            self.btn_switch_source.clicked.connect(self.on_toggle_analysis_source)
            row2.addWidget(self.btn_switch_source)

            btn_import_json = QPushButton("📥 导入预设")
            btn_import_json.setToolTip("从 .json 文件导入均衡器、泛音包络与节拍配置")
            btn_import_json.clicked.connect(self.on_import_preset_json)
            row2.addWidget(btn_import_json)

            btn_export_json = QPushButton("📤 导出预设")
            btn_export_json.setToolTip("将当前均衡器、泛音包络与节拍配置保存为 .json 文件")
            btn_export_json.clicked.connect(self.on_export_preset_json)
            row2.addWidget(btn_export_json)

            btn_build_exe = QPushButton("📦 打包EXE")
            btn_build_exe.setToolTip("一键使用 PyInstaller 打包免安装独立程序 (.exe)，或双击根目录下的 build_exe.bat")
            btn_build_exe.clicked.connect(self.on_build_standalone_exe)
            row2.addWidget(btn_build_exe)

            btn_show_disclaimer = QPushButton("📜 免责协议")
            btn_show_disclaimer.setToolTip("查看《DISCLAIMER.md 软件使用免责协议与风险告知书》")
            btn_show_disclaimer.clicked.connect(self.on_show_disclaimer_dialog)
            row2.addWidget(btn_show_disclaimer)

            layout.addLayout(row2)
            return box

        def _build_spectrum_section(self) -> QWidget:
            container = QWidget()
            h_layout = QHBoxLayout(container)
            h_layout.setContentsMargins(0, 0, 0, 0)
            h_layout.setSpacing(6)

            spec_group = QGroupBox("实时频谱")
            spec_vbox = QVBoxLayout(spec_group)
            spec_vbox.setContentsMargins(8, 6, 8, 6)
            spec_vbox.setSpacing(4)

            # 拆分为上下两行控制栏，适配 4:3 与 16:9 屏幕
            ctrl_row1 = QHBoxLayout()
            ctrl_row1.setSpacing(8)
            self.chk_log_freq = QCheckBox("对数坐标")
            self.chk_log_freq.setChecked(True)
            self.chk_log_freq.toggled.connect(self._update_spectrum_axis_mode)
            ctrl_row1.addWidget(self.chk_log_freq)

            self.chk_sync_x_axes = QCheckBox("横轴同步缩放")
            self.chk_sync_x_axes.setChecked(True)
            self.chk_sync_x_axes.setStyleSheet("color: #38BDF8; font-weight: bold;")
            self.chk_sync_x_axes.toggled.connect(self._apply_x_axis_sync)
            ctrl_row1.addWidget(self.chk_sync_x_axes)

            self.chk_freeze_on_pause = QCheckBox("暂停保留频谱")
            self.chk_freeze_on_pause.setChecked(True)
            self.chk_freeze_on_pause.setStyleSheet("color: #10B981; font-weight: bold;")
            ctrl_row1.addWidget(self.chk_freeze_on_pause)

            lbl_pk = QLabel("主音标注:")
            lbl_pk.setStyleSheet("color: #FBBF24; font-weight: bold;")
            ctrl_row1.addWidget(lbl_pk)
            self.combo_peak_count = QComboBox()
            for label_txt, cnt_val in PEAK_COUNT_OPTIONS:
                self.combo_peak_count.addItem(label_txt, cnt_val)
            self.combo_peak_count.setCurrentIndex(1)
            self.combo_peak_count.setStyleSheet("color: #FBBF24; font-weight: bold;")
            self.combo_peak_count.currentIndexChanged.connect(self._on_peak_count_changed)
            ctrl_row1.addWidget(self.combo_peak_count)
            ctrl_row1.addStretch()
            spec_vbox.addLayout(ctrl_row1)

            ctrl_row2 = QHBoxLayout()
            ctrl_row2.setSpacing(8)
            btn_reset_zoom = QPushButton("重置缩放")
            btn_reset_zoom.clicked.connect(self._reset_x_axis_zoom)
            ctrl_row2.addWidget(btn_reset_zoom)

            self.chk_peak_hold = QCheckBox("峰值包络线")
            self.chk_peak_hold.setChecked(True)
            ctrl_row2.addWidget(self.chk_peak_hold)

            btn_reset_peak = QPushButton("清除峰值")
            btn_reset_peak.clicked.connect(lambda: setattr(self, "_peak_hold_db", None))
            ctrl_row2.addWidget(btn_reset_peak)

            ctrl_row2.addWidget(QLabel("FFT 窗口点数:"))
            self.combo_fft_size = QComboBox()
            self.combo_fft_size.addItems(FFT_WINDOW_SIZES)
            self.combo_fft_size.setCurrentText("2048")
            self.combo_fft_size.currentTextChanged.connect(lambda _: setattr(self, "_peak_hold_db", None))
            ctrl_row2.addWidget(self.combo_fft_size)
            ctrl_row2.addStretch()
            spec_vbox.addLayout(ctrl_row2)

            self.spectrum_plot = pg.PlotWidget()
            self.spectrum_plot.setBackground("#111622")
            self.spectrum_plot.showGrid(x=True, y=True, alpha=0.25)
            self.spectrum_plot.setLabel("bottom", "频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]")
            self.spectrum_plot.setLabel("left", "分贝 (dBFS)")
            self.spectrum_plot.getAxis("left").setWidth(60)
            self.spectrum_plot.setYRange(-100.0, 8.0)
            self.spectrum_plot.setMouseEnabled(x=True, y=False)
            self.spectrum_plot.getPlotItem().vb.setLimits(
                xMin=math.log10(12.0), xMax=math.log10(24000.0)
            )
            self.spectrum_plot.setXRange(math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ), padding=0.01)
            self.spectrum_plot.hideButtons()

            self.spectrum_plot.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=True))
            self._spec_piano_lines = add_piano_c0_c8_guide_lines(self.spectrum_plot, use_log=True)

            self.spec_main_curve = self.spectrum_plot.plot(
                [],
                [],
                pen=pg.mkPen("#38BDF8", width=2.0),
                fillLevel=-100.0,
                brush=pg.mkBrush(56, 189, 248, 55),
            )
            self.spec_peak_curve = self.spectrum_plot.plot(
                [],
                [],
                pen=pg.mkPen("#F43F5E", width=1.4, style=Qt.PenStyle.DotLine),
            )

            self.live_top3_annotator = TopPeaksAnnotator(self.spectrum_plot, max_peaks=3)

            spec_vbox.addWidget(self.spectrum_plot)
            h_layout.addWidget(spec_group, stretch=4)

            metrics_group = QGroupBox("音频参数与主音")
            metrics_group.setMinimumWidth(220)
            metrics_group.setMaximumWidth(295)
            metrics_vbox = QVBoxLayout(metrics_group)
            metrics_vbox.setContentsMargins(8, 6, 8, 6)
            form = QFormLayout()
            form.setVerticalSpacing(4)

            self.lbl_metric_source = QLabel("软件内部音频流")
            self.lbl_metric_source.setStyleSheet("color: #38BDF8; font-weight: bold;")
            self.lbl_metric_orig_sr = QLabel("-- Hz")
            self.lbl_metric_live_sr = QLabel("-- Hz")
            self.lbl_metric_channels = QLabel("--")
            self.lbl_metric_peak1 = QLabel("#1: --")
            self.lbl_metric_peak1.setStyleSheet("color: #FBBF24; font-size: 13px; font-weight: bold;")
            self.lbl_metric_peak2 = QLabel("#2: --")
            self.lbl_metric_peak2.setStyleSheet("color: #2DD4BF; font-weight: bold;")
            self.lbl_metric_peak3 = QLabel("#3: --")
            self.lbl_metric_peak3.setStyleSheet("color: #F472B6; font-weight: bold;")
            self.lbl_metric_rms_db = QLabel("-- dBFS")
            self.lbl_metric_centroid = QLabel("-- Hz")

            form.addRow("监控源:", self.lbl_metric_source)
            form.addRow("原采样率:", self.lbl_metric_orig_sr)
            form.addRow("有效采样:", self.lbl_metric_live_sr)
            form.addRow("声道/位深:", self.lbl_metric_channels)
            form.addRow("第1主音:", self.lbl_metric_peak1)
            form.addRow("第2主音:", self.lbl_metric_peak2)
            form.addRow("第3主音:", self.lbl_metric_peak3)
            form.addRow("平均响度:", self.lbl_metric_rms_db)
            form.addRow("频谱重心:", self.lbl_metric_centroid)
            metrics_vbox.addLayout(form)

            metrics_vbox.addWidget(QLabel("时域波形:"))
            self.scope_plot = pg.PlotWidget()
            self.scope_plot.setBackground("#111622")
            self.scope_plot.setYRange(-1.05, 1.05)
            self.scope_plot.hideAxis("bottom")
            self.scope_plot.setFixedHeight(95)
            self.scope_curve = self.scope_plot.plot([], [], pen=pg.mkPen("#10B981", width=1.6))
            metrics_vbox.addWidget(self.scope_plot)

            h_layout.addWidget(metrics_group, stretch=1)
            return container

        def _build_bottom_tabs(self) -> QTabWidget:
            tabs = QTabWidget()
            tabs.addTab(self._build_tab_eq(), "均衡器")
            tabs.addTab(self._build_tab_synth(), "声音合成器")
            tabs.addTab(self._build_tab_resample_and_record(), "格式转换与录音")
            tabs.addTab(self._build_tab_snapshot_analyzer(), "声音截取与钢琴")
            return tabs

        def _build_tab_eq(self) -> QWidget:
            page = QWidget()
            vbox = QVBoxLayout(page)
            vbox.setContentsMargins(6, 6, 6, 6)
            vbox.setSpacing(4)

            ctrl_layout = QHBoxLayout()
            self.chk_eq_enable = QCheckBox("启用均衡器")
            self.chk_eq_enable.setChecked(True)
            self.chk_eq_enable.toggled.connect(lambda v: setattr(self.preset, "eq_enabled", v))
            ctrl_layout.addWidget(self.chk_eq_enable)

            ctrl_layout.addWidget(QLabel("预设:"))
            self.combo_eq_preset = QComboBox()
            self.combo_eq_preset.addItems(list(EQ_CURVE_PRESETS.keys()))
            self.combo_eq_preset.currentTextChanged.connect(self.on_apply_eq_preset)
            ctrl_layout.addWidget(self.combo_eq_preset)

            btn_flat = QPushButton("重置 0dB 平坦曲线")
            btn_flat.clicked.connect(
                lambda: self.eq_curve_widget.set_control_points([[f, 0.0] for f in DEFAULT_EQ_FREQS])
            )
            ctrl_layout.addWidget(btn_flat)

            lbl_tip = QLabel("")
            lbl_tip.setStyleSheet("color: #94A3B8;")
            ctrl_layout.addWidget(lbl_tip)
            ctrl_layout.addStretch()
            vbox.addLayout(ctrl_layout)

            self.eq_curve_widget = DraggableCurveWidget(
                title="均衡器曲线",
                min_db=-24.0,
                max_db=24.0,
                line_color="#38BDF8",
                fill_color=(56, 189, 248, 45),
            )
            self.eq_curve_widget.set_control_points(self.preset.eq_points, emit_signal=False)
            self.eq_curve_widget.curveChanged.connect(self._on_eq_curve_updated)
            vbox.addWidget(self.eq_curve_widget)
            return page

        def _build_tab_synth(self) -> QWidget:
            page = QWidget()
            main_h = QHBoxLayout(page)
            main_h.setContentsMargins(6, 6, 6, 6)
            main_h.setSpacing(6)

            left_group = QGroupBox("泛音与函数合成")
            left_v = QVBoxLayout(left_group)
            left_v.setContentsMargins(6, 6, 6, 6)
            left_v.setSpacing(4)

            top_ctrl = QHBoxLayout()
            self.btn_play_synth = QPushButton("🔊 开启合成器发声")
            self.btn_play_synth.setCheckable(True)
            self.btn_play_synth.setStyleSheet(
                "QPushButton { background-color: #4F46E5; padding: 4px 10px; } "
                "QPushButton:checked { background-color: #DC2626; }"
            )
            self.btn_play_synth.clicked.connect(self.on_toggle_synth_playback)
            top_ctrl.addWidget(self.btn_play_synth)

            top_ctrl.addWidget(QLabel("模式:"))
            self.combo_synth_mode = QComboBox()
            self.combo_synth_mode.addItems([
                "模式 A：泛音包络",
                "模式 B：多波混音",
                "模式 C：数学公式",
            ])
            self.combo_synth_mode.currentIndexChanged.connect(self._on_synth_mode_changed)
            top_ctrl.addWidget(self.combo_synth_mode, stretch=2)

            top_ctrl.addWidget(QLabel("音色预设:"))
            self.combo_inst_preset = QComboBox()
            self.combo_inst_preset.addItems(list(INSTRUMENT_ENVELOPE_PRESETS.keys()))
            self.combo_inst_preset.currentTextChanged.connect(self.on_apply_instrument_preset)
            top_ctrl.addWidget(self.combo_inst_preset, stretch=1)
            left_v.addLayout(top_ctrl)

            param_row = QHBoxLayout()
            param_row.setSpacing(6)
            param_row.addWidget(QLabel("基频 f0(Hz):"))
            self.spin_f0 = QDoubleSpinBox()
            self.spin_f0.setRange(15.0, 8000.0)
            self.spin_f0.setDecimals(2)
            self.spin_f0.setValue(self.preset.fundamental_hz)
            self.spin_f0.setSingleStep(5.0)
            self.spin_f0.setMinimumWidth(90)
            self.spin_f0.valueChanged.connect(self._on_f0_changed)
            param_row.addWidget(self.spin_f0)

            param_row.addWidget(QLabel("钢琴音阶(C0~C8):"))
            self.combo_piano_note = QComboBox()
            self.combo_piano_note.setMinimumWidth(135)
            for label_txt, f_val in FULL_PIANO_NOTES_LIST:
                self.combo_piano_note.addItem(label_txt, f_val)
            self.combo_piano_note.setCurrentText("C4 (261.63 Hz)")
            self.combo_piano_note.currentIndexChanged.connect(
                lambda: self.spin_f0.setValue(float(self.combo_piano_note.currentData()))
            )
            param_row.addWidget(self.combo_piano_note)

            param_row.addWidget(QLabel("泛音阶数:"))
            self.spin_harmonics = QSpinBox()
            self.spin_harmonics.setRange(1, 48)
            self.spin_harmonics.setValue(self.preset.num_harmonics)
            self.spin_harmonics.valueChanged.connect(self._on_harmonics_count_changed)
            param_row.addWidget(self.spin_harmonics)

            param_row.addWidget(QLabel("模式A基波:"))
            self.combo_base_wave = QComboBox()
            self.combo_base_wave.addItems(["sine", "square", "triangle", "sawtooth", "custom"])
            self.combo_base_wave.currentTextChanged.connect(lambda w: setattr(self.preset, "base_waveform", w))
            param_row.addWidget(self.combo_base_wave)
            param_row.addStretch()
            left_v.addLayout(param_row)

            # 钢琴八度快捷选音紧凑按钮行
            piano_btn_row = QHBoxLayout()
            piano_btn_row.setSpacing(3)
            lbl_quick = QLabel("快捷选音:")
            lbl_quick.setStyleSheet("color: #FBBF24;")
            piano_btn_row.addWidget(lbl_quick)

            quick_notes = [
                ("C0", 16.35),
                ("C1", 32.70),
                ("C2", 65.41),
                ("C3", 130.81),
                ("A3(220)", 220.00),
                ("C4(261.6)", 261.63),
                ("A4(440)", 440.00),
                ("C5", 523.25),
                ("C6", 1046.50),
                ("C7", 2093.00),
                ("C8", 4186.01),
            ]
            for note_label, freq_val in quick_notes:
                b = QPushButton(note_label)
                b.setToolTip(f"点击将基频 f0 设为 {note_label} ({freq_val} Hz)")
                b.setStyleSheet("QPushButton { padding: 2px 5px; font-size: 11px; }")
                b.clicked.connect(lambda _, f=freq_val: self.spin_f0.setValue(f))
                piano_btn_row.addWidget(b)
            piano_btn_row.addStretch()
            left_v.addLayout(piano_btn_row)

            formula_row = QHBoxLayout()
            formula_row.setSpacing(5)
            formula_row.addWidget(QLabel("数学公式 f(x):"))
            self.edit_custom_formula = QLineEdit(self.preset.custom_formula)
            self.edit_custom_formula.setPlaceholderText("例如: sin(x) + 0.35*sin(2*x) + 0.15*cos(3*x)")
            self.edit_custom_formula.textChanged.connect(self._on_custom_formula_edited)
            formula_row.addWidget(self.edit_custom_formula, stretch=2)

            btn_use_mode_c = QPushButton("⚡ 用公式发声(模式C)")
            btn_use_mode_c.setStyleSheet("background-color: #0284C7; padding: 3px 8px;")
            btn_use_mode_c.clicked.connect(self._activate_mode_c_directly)
            formula_row.addWidget(btn_use_mode_c)

            self.lbl_formula_status = QLabel("✅ 有效")
            self.lbl_formula_status.setStyleSheet("color: #10B981;")
            formula_row.addWidget(self.lbl_formula_status)
            left_v.addLayout(formula_row)

            self.harmonic_curve_widget = DraggableCurveWidget(
                title="泛音包络曲线",
                min_db=-80.0,
                max_db=0.0,
                line_color="#A855F7",
                fill_color=(168, 85, 247, 45),
            )
            self.harmonic_curve_widget.set_control_points(
                self.preset.harmonic_curve_points, emit_signal=False
            )
            self.harmonic_curve_widget.curveChanged.connect(self._on_harmonic_curve_updated)
            self._refresh_static_harmonic_preview()
            left_v.addWidget(self.harmonic_curve_widget)

            main_h.addWidget(left_group, stretch=3)

            right_group = QGroupBox("多振荡器列表")
            right_v = QVBoxLayout(right_group)
            right_v.setContentsMargins(6, 6, 6, 6)

            self.table_osc = QTableWidget(0, 5)
            self.table_osc.setHorizontalHeaderLabels(
                ["启用", "波形", "频率(Hz)", "分贝(dB)", "公式(custom)"]
            )
            self.table_osc.horizontalHeader().setSectionResizeMode(QHeaderView.ResizeMode.Stretch)
            right_v.addWidget(self.table_osc)

            osc_btn_row = QHBoxLayout()
            btn_add_osc = QPushButton("➕ 添加振荡器")
            btn_add_osc.clicked.connect(self.on_add_oscillator_row)
            osc_btn_row.addWidget(btn_add_osc)

            btn_del_osc = QPushButton("➖ 删除选中")
            btn_del_osc.clicked.connect(self.on_remove_oscillator_row)
            osc_btn_row.addWidget(btn_del_osc)
            right_v.addLayout(osc_btn_row)

            self._populate_osc_table_from_preset()
            main_h.addWidget(right_group, stretch=2)
            return page

        def _build_tab_resample_and_record(self) -> QWidget:
            page = QWidget()
            h_layout = QHBoxLayout(page)
            h_layout.setContentsMargins(6, 6, 6, 6)
            h_layout.setSpacing(8)

            resample_group = QGroupBox("重采样与格式转换")
            res_layout = QFormLayout(resample_group)
            res_layout.setVerticalSpacing(8)

            self.chk_live_resample = QCheckBox("开启实时重采样")
            self.chk_live_resample.setStyleSheet("color: #38BDF8; font-weight: bold;")
            self.chk_live_resample.toggled.connect(lambda v: setattr(self.preset, "resample_live_enabled", v))
            res_layout.addRow(self.chk_live_resample)

            self.combo_target_sr = QComboBox()
            for sr_val in [8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000]:
                self.combo_target_sr.addItem(f"{sr_val} Hz", sr_val)
            self.combo_target_sr.setCurrentText("44100 Hz")
            self.combo_target_sr.currentIndexChanged.connect(
                lambda: setattr(self.preset, "target_sample_rate", int(self.combo_target_sr.currentData()))
            )
            res_layout.addRow("目标采样率:", self.combo_target_sr)

            self.combo_target_bits = QComboBox()
            for b_label, b_val in [
                ("8-bit", 8),
                ("16-bit", 16),
                ("24-bit", 24),
                ("32-bit Float", 32),
            ]:
                self.combo_target_bits.addItem(b_label, b_val)
            self.combo_target_bits.setCurrentIndex(1)
            self.combo_target_bits.currentIndexChanged.connect(
                lambda: setattr(self.preset, "target_bit_depth", int(self.combo_target_bits.currentData()))
            )
            res_layout.addRow("目标采样位深:", self.combo_target_bits)

            self.combo_target_ch = QComboBox()
            self.combo_target_ch.addItem("立体声", 2)
            self.combo_target_ch.addItem("单声道", 1)
            self.combo_target_ch.currentIndexChanged.connect(
                lambda: setattr(self.preset, "target_channels", int(self.combo_target_ch.currentData()))
            )
            res_layout.addRow("目标声道数:", self.combo_target_ch)

            self.combo_export_format = QComboBox()
            self.combo_export_format.addItems(["wav", "mp3", "flac", "ogg", "m4a", "aac"])
            res_layout.addRow("转换目标格式:", self.combo_export_format)

            self.combo_export_bitrate = QComboBox()
            self.combo_export_bitrate.addItems(["128k", "192k", "256k", "320k"])
            self.combo_export_bitrate.setCurrentText("192k")
            res_layout.addRow("有损编码比特率:", self.combo_export_bitrate)

            btn_one_click_resample = QPushButton("导出音频文件")
            btn_one_click_resample.setStyleSheet("background-color: #0284C7; padding: 8px; font-size: 13px;")
            btn_one_click_resample.clicked.connect(self.on_one_click_resample_export)
            res_layout.addRow(btn_one_click_resample)

            h_layout.addWidget(resample_group, stretch=1)

            rec_group = QGroupBox("声音录制")
            rec_layout = QFormLayout(rec_group)
            rec_layout.setVerticalSpacing(8)

            self.combo_rec_source = QComboBox()
            self.combo_rec_source.addItem("软件内部声音", "internal")
            self.combo_rec_source.addItem("电脑系统声音", "loopback")
            rec_layout.addRow("选择录制源:", self.combo_rec_source)

            self.combo_rec_sr = QComboBox()
            for sr_val in [16000, 22050, 44100, 48000, 96000]:
                self.combo_rec_sr.addItem(f"{sr_val} Hz", sr_val)
            self.combo_rec_sr.setCurrentText("44100 Hz")
            rec_layout.addRow("录制导出采样率:", self.combo_rec_sr)

            self.combo_rec_bits = QComboBox()
            for b_label, b_val in [("16-bit PCM", 16), ("24-bit PCM", 24), ("32-bit Float", 32)]:
                self.combo_rec_bits.addItem(b_label, b_val)
            rec_layout.addRow("录制导出位深:", self.combo_rec_bits)

            self.combo_rec_ch = QComboBox()
            self.combo_rec_ch.addItem("立体声 (Stereo)", 2)
            self.combo_rec_ch.addItem("单声道 (Mono)", 1)
            rec_layout.addRow("录制导出声道:", self.combo_rec_ch)

            self.combo_rec_format = QComboBox()
            self.combo_rec_format.addItems(["wav", "mp3", "flac", "ogg"])
            rec_layout.addRow("录制保存格式:", self.combo_rec_format)

            self.lbl_rec_status = QLabel("状态：待机中 (已录制 0.0 秒)")
            self.lbl_rec_status.setStyleSheet("color: #FBBF24; font-weight: bold;")
            rec_layout.addRow("当前录音状态:", self.lbl_rec_status)

            rec_btn_row = QHBoxLayout()
            self.btn_start_rec = QPushButton("⏺ 开始录制声音")
            self.btn_start_rec.setCheckable(True)
            self.btn_start_rec.setStyleSheet(
                "QPushButton { background-color: #DC2626; padding: 8px; } "
                "QPushButton:checked { background-color: #991B1B; border: 2px solid #F87171; }"
            )
            self.btn_start_rec.clicked.connect(self.on_toggle_recording)
            rec_btn_row.addWidget(self.btn_start_rec)

            self.btn_save_rec = QPushButton("💾 导出保存录音文件")
            self.btn_save_rec.setStyleSheet("background-color: #059669; padding: 8px;")
            self.btn_save_rec.clicked.connect(self.on_export_recorded_audio)
            rec_btn_row.addWidget(self.btn_save_rec)
            rec_layout.addRow(rec_btn_row)

            h_layout.addWidget(rec_group, stretch=1)
            return page

        # -------------------------------------------------------------------
        # Tab 4: 瞬态声音截取 & 88键标准钢琴键盘 (含按节拍 BPM 抓主音 + 对拍重置 + 连点测速)
        # -------------------------------------------------------------------
        def _build_tab_snapshot_analyzer(self) -> QWidget:
            page = QWidget()
            vbox = QVBoxLayout(page)
            vbox.setContentsMargins(6, 6, 6, 6)
            vbox.setSpacing(5)

            # 第 1 行：瞬态截取、左侧视图切换、主音数量、三种慢放/延音试听按钮（紧凑排布，适配 4:3 与 16:9）
            ctrl_row1 = QHBoxLayout()
            ctrl_row1.setSpacing(6)

            btn_snap_now = QPushButton("📸 立即截取声音")
            btn_snap_now.setStyleSheet(
                "QPushButton { background-color: #7C3AED; border-color: #A78BFA; color: #FFFFFF; padding: 4px 10px; }"
            )
            btn_snap_now.clicked.connect(self.on_capture_audio_snapshot)
            ctrl_row1.addWidget(btn_snap_now)

            ctrl_row1.addWidget(QLabel("窗口:"))
            self.combo_snap_ms = QComboBox()
            self.combo_snap_ms.addItem("20ms(±10ms)", 20.0)
            self.combo_snap_ms.addItem("40ms(±20ms)", 40.0)
            self.combo_snap_ms.addItem("80ms(±40ms)", 80.0)
            self.combo_snap_ms.addItem("200ms(±100ms)", 200.0)
            self.combo_snap_ms.setCurrentIndex(1)
            ctrl_row1.addWidget(self.combo_snap_ms)

            ctrl_row1.addWidget(QLabel("左侧视图:"))
            self.combo_left_snap_view = QComboBox()
            self.combo_left_snap_view.addItem("时域波形", 0)
            self.combo_left_snap_view.addItem("88键钢琴", 1)
            self.combo_left_snap_view.setStyleSheet("color: #FBBF24; font-weight: bold;")
            self.combo_left_snap_view.currentIndexChanged.connect(self._on_left_snap_view_changed)
            ctrl_row1.addWidget(self.combo_left_snap_view)

            ctrl_row1.addWidget(QLabel("主音数:"))
            self.combo_snap_peak_count = QComboBox()
            for label_txt, cnt_val in PEAK_COUNT_OPTIONS:
                self.combo_snap_peak_count.addItem(label_txt, cnt_val)
            self.combo_snap_peak_count.setCurrentIndex(1)
            self.combo_snap_peak_count.currentIndexChanged.connect(self._on_snap_peak_count_changed)
            ctrl_row1.addWidget(self.combo_snap_peak_count)

            btn_play_raw = QPushButton("▶ 原样短放")
            btn_play_raw.setToolTip("原样播放截取到的短促声音片段")
            btn_play_raw.setStyleSheet("background-color: #0F766E; padding: 4px 8px;")
            btn_play_raw.clicked.connect(self.on_play_snapshot_raw)
            ctrl_row1.addWidget(btn_play_raw)

            btn_play_sustain = QPushButton("🎹 延音(1.5s)")
            btn_play_sustain.setToolTip("将截取到的瞬态声音拉长延音播放 1.5 秒")
            btn_play_sustain.setStyleSheet("background-color: #0284C7; padding: 4px 8px;")
            btn_play_sustain.clicked.connect(self.on_play_snapshot_sustain)
            ctrl_row1.addWidget(btn_play_sustain)

            self.combo_sustain_type = QComboBox()
            self.combo_sustain_type.addItem("无缝原波", False)
            self.combo_sustain_type.addItem("纯谐波延音", True)
            ctrl_row1.addWidget(self.combo_sustain_type)

            btn_play_slow = QPushButton("🐢 慢放")
            btn_play_slow.setToolTip("对截取片段进行慢速拉伸播放")
            btn_play_slow.setStyleSheet("background-color: #D97706; padding: 4px 8px;")
            btn_play_slow.clicked.connect(self.on_play_snapshot_slow_stretch)
            ctrl_row1.addWidget(btn_play_slow)

            self.combo_slow_factor = QComboBox()
            for factor in [2, 5, 10, 15, 20]:
                self.combo_slow_factor.addItem(f"{factor}x", float(factor))
            self.combo_slow_factor.setCurrentIndex(2)
            ctrl_row1.addWidget(self.combo_slow_factor)

            self.combo_slow_mode = QComboBox()
            self.combo_slow_mode.addItem("保音高", True)
            self.combo_slow_mode.addItem("物理降频", False)
            ctrl_row1.addWidget(self.combo_slow_mode)

            ctrl_row1.addStretch()
            vbox.addLayout(ctrl_row1)

            # 第 2 行：钢琴实时跟随 & 按节拍 (BPM) 抓主音控制栏（含 BPM 设置、重置第一拍、连点测速、拍点指示灯、联动右侧刷新）
            ctrl_row2 = QHBoxLayout()
            ctrl_row2.setSpacing(6)

            self.chk_piano_live_follow = QCheckBox("🔄 钢琴跟随音乐")
            self.chk_piano_live_follow.setChecked(True)
            self.chk_piano_live_follow.setToolTip("勾选后钢琴键盘随播放的音乐更新高亮；取消勾选则仅显示手动截取时的定格主音")
            self.chk_piano_live_follow.setStyleSheet("color: #10B981; font-weight: bold;")
            self.chk_piano_live_follow.toggled.connect(self._on_piano_follow_toggled)
            ctrl_row2.addWidget(self.chk_piano_live_follow)

            self.chk_bpm_capture = QCheckBox("⏱️ 按节拍(BPM)抓主音")
            self.chk_bpm_capture.setChecked(self.preset.bpm_capture_enabled)
            self.chk_bpm_capture.setToolTip(
                "勾选后，钢琴主音不再每帧闪烁，而是严格按设定的 BPM 节拍周期抓取一次并保持到下一拍（如 60 BPM = 每秒抓 1 次）"
            )
            self.chk_bpm_capture.setStyleSheet("color: #FBBF24; font-weight: bold;")
            self.chk_bpm_capture.toggled.connect(self._on_bpm_capture_toggled)
            ctrl_row2.addWidget(self.chk_bpm_capture)

            ctrl_row2.addWidget(QLabel("BPM:"))
            self.spin_bpm = QDoubleSpinBox()
            self.spin_bpm.setRange(15.0, 300.0)
            self.spin_bpm.setDecimals(1)
            self.spin_bpm.setSingleStep(1.0)
            self.spin_bpm.setValue(self.preset.bpm_value)
            self.spin_bpm.setMinimumWidth(68)
            self.spin_bpm.setToolTip("自由设置每分钟节拍数 (BPM)，例如 60 BPM = 1.00秒抓一次，120 BPM = 0.50秒抓一次")
            self.spin_bpm.valueChanged.connect(self._on_bpm_value_changed)
            ctrl_row2.addWidget(self.spin_bpm)

            self.btn_beat_reset = QPushButton("重置第1拍")
            self.btn_beat_reset.setToolTip(
                "当节拍与歌曲重音对不上时点击此按钮：从按下的这一瞬间立即记为第 1 拍并抓取主音，后续严格按 BPM 周期往后抓拍"
            )
            self.btn_beat_reset.setStyleSheet(
                "QPushButton { background-color: #0284C7; border-color: #38BDF8; color: #FFFFFF; padding: 3px 9px; } "
                "QPushButton:hover { background-color: #0369A1; }"
            )
            self.btn_beat_reset.clicked.connect(self.on_reset_beat_phase)
            ctrl_row2.addWidget(self.btn_beat_reset)

            self.btn_tap_tempo = QPushButton("🥁 连点测BPM")
            self.btn_tap_tempo.setToolTip("跟着歌曲节奏连续点击 2~6 下，自动测算歌曲 BPM 并同步对齐第 1 拍")
            self.btn_tap_tempo.setStyleSheet(
                "QPushButton { background-color: #4F46E5; border-color: #818CF8; color: #FFFFFF; padding: 3px 8px; } "
                "QPushButton:hover { background-color: #4338CA; }"
            )
            self.btn_tap_tempo.clicked.connect(self.on_tap_tempo_clicked)
            ctrl_row2.addWidget(self.btn_tap_tempo)

            self.chk_bpm_sync_snapshot = QCheckBox("每拍同步刷新右侧")
            self.chk_bpm_sync_snapshot.setChecked(self.preset.bpm_sync_right_snapshot)
            self.chk_bpm_sync_snapshot.setToolTip(
                "勾选后，每一拍不仅更新左侧钢琴主音，还会自动同步刷新右侧的定格频谱图与时域声波图"
            )
            self.chk_bpm_sync_snapshot.setStyleSheet("color: #38BDF8;")
            self.chk_bpm_sync_snapshot.toggled.connect(
                lambda v: setattr(self.preset, "bpm_sync_right_snapshot", bool(v))
            )
            ctrl_row2.addWidget(self.chk_bpm_sync_snapshot)

            ctrl_row2.addWidget(QLabel("拍点灯:"))
            self.beat_led_square = BeatFlashSquareWidget()
            ctrl_row2.addWidget(self.beat_led_square)

            ctrl_row2.addStretch()
            vbox.addLayout(ctrl_row2)

            charts_layout = QHBoxLayout()
            charts_layout.setSpacing(6)

            self.left_snap_stack = QStackedWidget()

            self.snap_wave_plot = pg.PlotWidget()
            self.snap_wave_plot.setBackground("#111622")
            self.snap_wave_plot.showGrid(x=True, y=True, alpha=0.25)
            self.snap_wave_plot.setTitle(
                "截取时域波形",
                color="#E2E8F0",
                size="9.5pt",
            )
            self.snap_wave_plot.setLabel("bottom", "相对截取时刻 Time (ms)")
            self.snap_wave_plot.setLabel("left", "振幅 Amplitude")
            self.snap_wave_plot.setYRange(-1.05, 1.05)

            self.snap_wave_legend = self.snap_wave_plot.addLegend(
                offset=(-10, 10),
                brush=pg.mkBrush(15, 23, 42, 220),
                pen=pg.mkPen("#475569", width=1.0),
                labelTextColor="#F8FAFC",
            )

            center_line = pg.InfiniteLine(
                pos=0.0, angle=90, pen=pg.mkPen(color="#94A3B8", width=1.2, style=Qt.PenStyle.DashLine)
            )
            self.snap_wave_plot.addItem(center_line)

            self.snap_wave_main_curve = self.snap_wave_plot.plot(
                [], [], pen=pg.mkPen("#10B981", width=2.2), name="🟢 绿色实线: 截取的真实合成声波"
            )
            self.snap_sine_overlays = [
                self.snap_wave_plot.plot(
                    [],
                    [],
                    pen=pg.mkPen(PEAK_RANK_COLORS[i], width=1.5, style=Qt.PenStyle.DashLine),
                )
                for i in range(3)
            ]
            self.left_snap_stack.addWidget(self.snap_wave_plot)

            self.piano_88_widget = Piano88KeysWidget()
            self.piano_88_widget.keyClicked.connect(self._on_piano_key_clicked_play)
            self.left_snap_stack.addWidget(self.piano_88_widget)

            charts_layout.addWidget(self.left_snap_stack, stretch=1)

            self.snap_spec_plot = pg.PlotWidget()
            self.snap_spec_plot.setBackground("#111622")
            self.snap_spec_plot.showGrid(x=True, y=True, alpha=0.25)
            self.snap_spec_plot.setTitle(
                "截取频谱",
                color="#E2E8F0",
                size="9.5pt",
            )
            self.snap_spec_plot.setLabel("bottom", "频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]")
            self.snap_spec_plot.setLabel("left", "分贝 (dBFS)")
            self.snap_spec_plot.getAxis("left").setWidth(60)
            self.snap_spec_plot.setYRange(-100.0, 8.0)
            self.snap_spec_plot.setXRange(math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ), padding=0.01)
            self.snap_spec_plot.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=True))
            self._snap_piano_lines = add_piano_c0_c8_guide_lines(self.snap_spec_plot, use_log=True)

            self.snap_spec_curve = self.snap_spec_plot.plot(
                [],
                [],
                pen=pg.mkPen("#A855F7", width=2.0),
                fillLevel=-100.0,
                brush=pg.mkBrush(168, 85, 247, 50),
            )
            self.snap_top3_annotator = TopPeaksAnnotator(self.snap_spec_plot, max_peaks=3)

            charts_layout.addWidget(self.snap_spec_plot, stretch=1)
            vbox.addLayout(charts_layout)
            return page

        # -------------------------------------------------------------------
        # 主音数量 (0 / 1 / 2 / 3 个) & BPM 节拍定时抓取联动控制
        # -------------------------------------------------------------------
        def _get_active_piano_peaks(self) -> List[Dict[str, object]]:
            cnt = self.preset.peak_annotation_count
            if not self.chk_piano_live_follow.isChecked():
                return self.captured_top_peaks[:cnt]
            if self.chk_bpm_capture.isChecked():
                return self._bpm_locked_peaks[:cnt]
            return self._latest_live_top3_peaks[:cnt]

        def _on_peak_count_changed(self, idx: int) -> None:
            cnt = int(self.combo_peak_count.currentData())
            self.preset.peak_annotation_count = cnt
            if self.combo_snap_peak_count.currentIndex() != idx:
                self.combo_snap_peak_count.blockSignals(True)
                self.combo_snap_peak_count.setCurrentIndex(idx)
                self.combo_snap_peak_count.blockSignals(False)
            self._refresh_snapshot_plots(preserve_piano_bpm=True)
            self.piano_88_widget.set_highlighted_peaks(self._get_active_piano_peaks())

        def _on_snap_peak_count_changed(self, idx: int) -> None:
            cnt = int(self.combo_snap_peak_count.currentData())
            self.preset.peak_annotation_count = cnt
            if self.combo_peak_count.currentIndex() != idx:
                self.combo_peak_count.blockSignals(True)
                self.combo_peak_count.setCurrentIndex(idx)
                self.combo_peak_count.blockSignals(False)
            self._refresh_snapshot_plots(preserve_piano_bpm=True)
            self.piano_88_widget.set_highlighted_peaks(self._get_active_piano_peaks())

        def _on_left_snap_view_changed(self, idx: int) -> None:
            self.left_snap_stack.setCurrentIndex(idx)
            if idx == 1:
                self.piano_88_widget.set_highlighted_peaks(self._get_active_piano_peaks(), force=True)
            else:
                self._refresh_snapshot_plots(preserve_piano_bpm=True)

        def _on_piano_follow_toggled(self, checked: bool) -> None:
            self.piano_88_widget.set_highlighted_peaks(self._get_active_piano_peaks())

        def _on_bpm_capture_toggled(self, checked: bool) -> None:
            self.preset.bpm_capture_enabled = bool(checked)
            if checked:
                if not self.chk_piano_live_follow.isChecked():
                    self.chk_piano_live_follow.setChecked(True)
                self.on_reset_beat_phase()
            else:
                self.beat_led_square.reset_off()
                self.piano_88_widget.set_highlighted_peaks(self._get_active_piano_peaks(), force=True)

        def _on_bpm_value_changed(self, val: float) -> None:
            bpm = max(15.0, min(300.0, float(val)))
            self.preset.bpm_value = bpm
            self._bpm_anchor_time = time.perf_counter()
            self._bpm_last_beat_index = 0

        def on_reset_beat_phase(self) -> None:
            """用户点击【重置第1拍】：从按下的这一瞬间立即记为第 1 拍并抓取一次当前主音，后续按 BPM 周期抓取"""
            if not self.chk_bpm_capture.isChecked():
                self.chk_bpm_capture.blockSignals(True)
                self.chk_bpm_capture.setChecked(True)
                self.preset.bpm_capture_enabled = True
                self.chk_bpm_capture.blockSignals(False)
            if not self.chk_piano_live_follow.isChecked():
                self.chk_piano_live_follow.setChecked(True)

            self._bpm_anchor_time = time.perf_counter()
            self._bpm_last_beat_index = 0
            self._execute_beat_capture_now()

            bpm = float(self.spin_bpm.value())
            period = 60.0 / max(15.0, bpm)
            self.status_bar.showMessage(
                f"🎯 已从当前瞬间重置拍点起点！后续严格按 {bpm:.1f} BPM (每 {period:.2f} 秒) 定时抓取一次主音。"
            )

        def on_tap_tempo_clicked(self) -> None:
            """连续点按测速 (Tap Tempo)：根据用户连续点击间隔自动测算 BPM，并同步对齐拍点起点"""
            now = time.perf_counter()
            if self._tap_tempo_timestamps and (now - self._tap_tempo_timestamps[-1]) > 2.5:
                self._tap_tempo_timestamps.clear()

            self._tap_tempo_timestamps.append(now)
            if len(self._tap_tempo_timestamps) > 6:
                self._tap_tempo_timestamps = self._tap_tempo_timestamps[-6:]

            if len(self._tap_tempo_timestamps) >= 2:
                intervals = np.diff(self._tap_tempo_timestamps)
                avg_interval = float(np.mean(intervals))
                if avg_interval > 1e-3:
                    measured_bpm = float(np.clip(60.0 / avg_interval, 15.0, 300.0))
                    self.spin_bpm.blockSignals(True)
                    self.spin_bpm.setValue(round(measured_bpm, 1))
                    self.preset.bpm_value = round(measured_bpm, 1)
                    self.spin_bpm.blockSignals(False)

            self.on_reset_beat_phase()
            if len(self._tap_tempo_timestamps) == 1:
                self.status_bar.showMessage("🥁 连点测速：已记录第 1 下并对齐拍点，请跟着节奏继续点击以自动算出 BPM...")
            else:
                bpm = float(self.spin_bpm.value())
                self.status_bar.showMessage(
                    f"🥁 连点测速完成：当前节拍 {bpm:.1f} BPM ({60.0/bpm:.2f} 秒/拍)，已同步对齐拍点！"
                )

        def _execute_beat_capture_now(
            self,
            precomputed_freqs: Optional[np.ndarray] = None,
            precomputed_mag_db: Optional[np.ndarray] = None,
            precomputed_peaks: Optional[List[Dict[str, object]]] = None,
        ) -> None:
            """在拍点到达瞬间执行主音锁定抓取（仅小方块灯闪烁一下，拍内保持期间零重绘、无阻塞）"""
            peak_cnt = self.preset.peak_annotation_count
            peaks_source = precomputed_peaks if precomputed_peaks is not None else self._latest_live_top3_peaks
            self._bpm_locked_peaks = list(peaks_source)
            self.piano_88_widget.set_highlighted_peaks(self._bpm_locked_peaks[:peak_cnt], force=True)
            self.beat_led_square.flash(110)

            if self.chk_bpm_sync_snapshot.isChecked():
                self._capture_snapshot_data_only()
                self._refresh_snapshot_plots(
                    preserve_piano_bpm=True,
                    precomputed_freqs=precomputed_freqs,
                    precomputed_mag_db=precomputed_mag_db,
                    precomputed_peaks=self._bpm_locked_peaks,
                )

        def _on_piano_key_clicked_play(self, midi_note: int, freq_hz: float, note_name: str) -> None:
            piano_wave = AudioDSPEngine.synthesize_piano_key_note(
                midi_note=midi_note,
                sample_rate=self.stream_sample_rate,
                duration_sec=0.85,
            )
            with self._snapshot_lock:
                self.snapshot_play_buffer = piano_wave
                self.snapshot_play_cursor = 0
            self.status_bar.showMessage(
                f"🎹 钢琴试音：您点击了琴键 【{note_name}】 (MIDI 编号 {midi_note} | 标准频率 {freq_hz:.2f} Hz)"
            )

        # -------------------------------------------------------------------
        # 横坐标对齐与同步缩放控制
        # -------------------------------------------------------------------
        def _apply_x_axis_sync(self, enabled: bool) -> None:
            if enabled:
                self.eq_curve_widget.setXLink(self.spectrum_plot)
                self.harmonic_curve_widget.setXLink(self.spectrum_plot)
                self.snap_spec_plot.setXLink(self.spectrum_plot)
                vr = self.spectrum_plot.viewRange()[0]
                self.eq_curve_widget.setXRange(vr[0], vr[1], padding=0.0)
                self.harmonic_curve_widget.setXRange(vr[0], vr[1], padding=0.0)
                self.snap_spec_plot.setXRange(vr[0], vr[1], padding=0.0)
            else:
                self.eq_curve_widget.setXLink(None)
                self.harmonic_curve_widget.setXLink(None)
                self.snap_spec_plot.setXLink(None)

        def _reset_x_axis_zoom(self) -> None:
            use_log = self.chk_log_freq.isChecked()
            if use_log:
                xmin, xmax = math.log10(MIN_PLOT_FREQ_HZ), math.log10(MAX_PLOT_FREQ_HZ)
            else:
                xmin, xmax = MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ
            self.spectrum_plot.setXRange(xmin, xmax, padding=0.01)
            self.eq_curve_widget.setXRange(xmin, xmax, padding=0.01)
            self.harmonic_curve_widget.setXRange(xmin, xmax, padding=0.01)
            self.snap_spec_plot.setXRange(xmin, xmax, padding=0.01)

        def _update_spectrum_axis_mode(self, use_log: bool) -> None:
            vb = self.spectrum_plot.getPlotItem().vb
            if use_log:
                vb.setLimits(xMin=math.log10(12.0), xMax=math.log10(24000.0))
            else:
                vb.setLimits(xMin=10.0, xMax=24000.0)

            self.spectrum_plot.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=use_log))
            for line, f_hz in self._spec_piano_lines:
                line.setPos(math.log10(f_hz) if use_log else f_hz)

            self.snap_spec_plot.getAxis("bottom").setTicks(build_freq_and_piano_axis_ticks(use_log=use_log))
            for line, f_hz in self._snap_piano_lines:
                line.setPos(math.log10(f_hz) if use_log else f_hz)

            self.eq_curve_widget.set_axis_log_mode(use_log)
            self.harmonic_curve_widget.set_axis_log_mode(use_log)
            self._reset_x_axis_zoom()
            self._refresh_static_harmonic_preview()
            self._refresh_snapshot_plots(preserve_piano_bpm=True)

        # -------------------------------------------------------------------
        # 实时音频流回调引擎
        # -------------------------------------------------------------------
        def _append_to_rolling_history(self, chunk: np.ndarray) -> None:
            if chunk.size == 0:
                return
            n = chunk.shape[0]
            with self._history_lock:
                total = self._rolling_history.shape[0]
                if n >= total:
                    self._rolling_history[:] = chunk[-total:]
                else:
                    self._rolling_history[:-n] = self._rolling_history[n:]
                    self._rolling_history[-n:] = chunk

        def _init_audio_output_stream(self) -> None:
            if not HAS_SOUNDDEVICE:
                return
            try:
                if self.output_stream is not None:
                    self.output_stream.stop()
                    self.output_stream.close()
                    self.output_stream = None

                self.output_stream = sd.OutputStream(
                    samplerate=self.stream_sample_rate,
                    channels=2,
                    blocksize=2048,
                    dtype="float32",
                    callback=self._audio_output_callback,
                )
                self.output_stream.start()
            except Exception as exc:
                self.status_bar.showMessage(f"音频输出流初始化提示: {exc}")

        def _audio_output_callback(self, outdata: np.ndarray, frames: int, time_info, status) -> None:
            block = np.zeros((frames, 2), dtype=np.float32)
            has_active_internal_sound = False

            if self.is_playing_file and self.audio_buffer is not None:
                rem = self.audio_buffer.shape[0] - self.play_cursor
                if rem <= 0:
                    self.is_playing_file = False
                    self.play_cursor = 0
                else:
                    spd = float(self.playback_speed)
                    rendered, next_cur = AudioDSPEngine.extract_variable_speed_block(
                        self.audio_buffer,
                        play_cursor=self.play_cursor,
                        out_frames=frames,
                        speed=spd,
                        keep_pitch=self.speed_keep_pitch,
                        wsola_state=self._wsola_state,
                    )
                    block += rendered
                    if next_cur == 0 and self.play_cursor > 0:
                        self.is_playing_file = False
                        self.play_cursor = 0
                    else:
                        self.play_cursor = next_cur
                    has_active_internal_sound = True

            if self.is_playing_synth:
                if self.preset.synth_mode == "harmonic_curve":
                    synth_block, harm_info = AudioDSPEngine.synthesize_harmonic_curve_block(
                        n_frames=frames,
                        sample_rate=self.stream_sample_rate,
                        fundamental_hz=self.preset.fundamental_hz,
                        num_harmonics=self.preset.num_harmonics,
                        envelope_points=self.preset.harmonic_curve_points,
                        phase_state=self._harmonic_phases,
                        base_waveform=self.preset.base_waveform,
                        custom_formula=self.preset.custom_formula,
                    )
                    self._latest_harmonics_info = harm_info
                elif self.preset.synth_mode == "multi_osc":
                    osc_items = [OscillatorItem(**d) for d in self.preset.oscillators]
                    synth_block = AudioDSPEngine.synthesize_multi_osc_block(
                        n_frames=frames,
                        sample_rate=self.stream_sample_rate,
                        oscillators=osc_items,
                        phase_state=self._osc_phases,
                    )
                else:
                    synth_block = AudioDSPEngine.synthesize_custom_formula_block(
                        n_frames=frames,
                        sample_rate=self.stream_sample_rate,
                        fundamental_hz=self.preset.fundamental_hz,
                        custom_formula=self.preset.custom_formula,
                        phase_state=self._custom_formula_phase,
                    )
                block += synth_block
                has_active_internal_sound = True

            if self.preset.resample_live_enabled and has_active_internal_sound:
                block = AudioDSPEngine.live_resample_effect_block(
                    block,
                    orig_sr=self.stream_sample_rate,
                    target_sr=self.preset.target_sample_rate,
                    target_bit_depth=self.preset.target_bit_depth,
                    target_channels=self.preset.target_channels,
                )

            if self.preset.eq_enabled and has_active_internal_sound:
                block = self._realtime_eq_processor.process_block(
                    block, self.stream_sample_rate, self.preset.eq_points
                )

            with self._snapshot_lock:
                if self.snapshot_play_buffer is not None:
                    rem_snap = self.snapshot_play_buffer.shape[0] - self.snapshot_play_cursor
                    if rem_snap > 0:
                        take_s = min(frames, rem_snap)
                        block[:take_s] += self.snapshot_play_buffer[
                            self.snapshot_play_cursor : self.snapshot_play_cursor + take_s
                        ]
                        self.snapshot_play_cursor += take_s
                        has_active_internal_sound = True
                    else:
                        self.snapshot_play_buffer = None
                        self.snapshot_play_cursor = 0

            block = AudioDSPEngine.soft_knee_limiter(block * self.master_volume, threshold=0.85)
            outdata[:] = block

            if has_active_internal_sound or not self.chk_freeze_on_pause.isChecked():
                self._latest_output_chunk = block.copy()
            if has_active_internal_sound and self.analysis_source_mode != "system_loopback":
                self._append_to_rolling_history(block)

            if self.is_recording and self.record_source_mode == "internal":
                with self._record_lock:
                    self.recorded_chunks.append(block.copy())
                    self.recorded_source_sr = self.stream_sample_rate

        def _on_loopback_chunk_received(self, chunk: np.ndarray, sr: int) -> None:
            if self.analysis_source_mode == "system_loopback":
                self._append_to_rolling_history(chunk)
            if self.is_recording and self.record_source_mode == "loopback":
                with self._record_lock:
                    self.recorded_chunks.append(chunk.copy())
                    self.recorded_source_sr = sr

        def _on_ui_refresh_tick(self) -> None:
            if self.audio_buffer is not None and self.orig_sample_rate > 0:
                total_sec = self.audio_buffer.shape[0] / float(self.orig_sample_rate)
                cur_sec = self.play_cursor / float(self.orig_sample_rate)
                if not self.slider_seek.isSliderDown() and self.audio_buffer.shape[0] > 0:
                    ratio = int((self.play_cursor / float(self.audio_buffer.shape[0])) * 1000)
                    self.slider_seek.setValue(ratio)
                self.lbl_time.setText(f"{self._fmt_sec(cur_sec)} / {self._fmt_sec(total_sec)}")

                if not self.is_playing_file and self.btn_play_pause.isChecked():
                    self.btn_play_pause.setChecked(False)
                    self.btn_play_pause.setText("▶ 播放")

            if self.is_recording:
                elapsed = time.time() - self.record_start_time
                self.lbl_rec_status.setText(f"🔴 正在录制 ({self.record_source_mode})... 已录制 {elapsed:.1f} 秒")

            use_log = self.chk_log_freq.isChecked()
            peak_cnt = self.preset.peak_annotation_count

            is_internal_paused = (
                self.analysis_source_mode != "system_loopback"
                and not self.is_playing_file
                and not self.is_playing_synth
                and self.snapshot_play_buffer is None
            )
            if is_internal_paused and self.chk_freeze_on_pause.isChecked() and self._has_rendered_live_frame:
                self.live_top3_annotator.update_peaks(
                    self._latest_live_top3_peaks, use_log_x=use_log, count_limit=peak_cnt
                )
                return

            if self.analysis_source_mode == "system_loopback":
                chunk, active_sr, active_ch = self.loopback_capture.get_latest_chunk()
                src_text = f"系统媒体 ({self.loopback_capture.device_name[:18]})"
                orig_sr_text = f"{active_sr} Hz"
                live_sr_text = f"{active_sr} Hz"
                ch_text = f"{active_ch}声道 / 32-bit"
            else:
                chunk = self._latest_output_chunk
                active_sr = self.stream_sample_rate
                src_text = f"软件内部 ({self.playback_speed:.2f}x)"
                orig_sr_text = f"{self.orig_sample_rate} Hz"
                if self.preset.resample_live_enabled:
                    live_sr_text = f"{self.preset.target_sample_rate} Hz (重采样)"
                    ch_text = f"{self.preset.target_channels}声道 / {self.preset.target_bit_depth}-bit"
                else:
                    live_sr_text = f"{active_sr} Hz (直通)"
                    ch_text = "2声道 / 32-bit"

            fft_size = int(self.combo_fft_size.currentText())
            freqs, mag_db, stats = AudioDSPEngine.compute_fft_spectrum_db(
                chunk, sample_rate=active_sr, fft_size=fft_size
            )

            valid = freqs >= MIN_PLOT_FREQ_HZ
            f_valid = freqs[valid]
            db_plot = mag_db[valid]

            x_plot = np.log10(f_valid) if use_log else f_valid

            self.spec_main_curve.setData(x_plot, db_plot)

            if self.chk_peak_hold.isChecked():
                if self._peak_hold_db is None or len(self._peak_hold_db) != len(db_plot):
                    self._peak_hold_db = db_plot.copy()
                else:
                    self._peak_hold_db = np.maximum(db_plot, self._peak_hold_db - 0.18)
                self.spec_peak_curve.setData(x_plot, self._peak_hold_db)
            else:
                self.spec_peak_curve.setData([], [])

            top3_peaks = AudioDSPEngine.find_top_n_spectral_peaks(freqs, mag_db, top_n=3, min_db=-76.0)
            self._latest_live_top3_peaks = top3_peaks
            self.live_top3_annotator.update_peaks(
                top3_peaks, use_log_x=use_log, count_limit=peak_cnt
            )

            # 更新 88 键钢琴键盘：区分【按节拍 BPM 定时抓主音】与【实时连续跟随】
            if self.chk_piano_live_follow.isChecked() and not is_internal_paused:
                if self.chk_bpm_capture.isChecked():
                    now = time.perf_counter()
                    beat_period = 60.0 / max(15.0, float(self.spin_bpm.value()))
                    beat_idx = int((now - self._bpm_anchor_time) // beat_period)
                    if beat_idx != self._bpm_last_beat_index:
                        self._bpm_last_beat_index = beat_idx
                        self._execute_beat_capture_now(
                            precomputed_freqs=freqs,
                            precomputed_mag_db=mag_db,
                            precomputed_peaks=top3_peaks,
                        )
                else:
                    self.piano_88_widget.set_highlighted_peaks(top3_peaks[:peak_cnt])

            mono_wave = np.mean(chunk, axis=1) if chunk.ndim > 1 else chunk
            self.scope_curve.setData(mono_wave[:512])

            if self.is_playing_synth and self.preset.synth_mode == "harmonic_curve":
                self.harmonic_curve_widget.set_harmonic_indicators(self._latest_harmonics_info)

            self.lbl_metric_source.setText(src_text)
            self.lbl_metric_orig_sr.setText(orig_sr_text)
            self.lbl_metric_live_sr.setText(live_sr_text)
            self.lbl_metric_channels.setText(ch_text)

            peak_labels = [self.lbl_metric_peak1, self.lbl_metric_peak2, self.lbl_metric_peak3]
            for i in range(3):
                if i < peak_cnt and i < len(top3_peaks):
                    p = top3_peaks[i]
                    peak_labels[i].setText(f"#{i+1} {p['note']} ({p['freq_hz']:.1f}Hz, {p['mag_db']:.1f}dB)")
                else:
                    peak_labels[i].setText(f"#{i+1}: --" if i < peak_cnt else f"#{i+1}: (未启用)")

            self.lbl_metric_rms_db.setText(f"{stats['rms_db']:.1f} dBFS")
            self.lbl_metric_centroid.setText(f"{stats['centroid_hz']:.1f} Hz")

            if not is_internal_paused:
                self._has_rendered_live_frame = True

        @staticmethod
        def _fmt_sec(sec: float) -> str:
            m = int(sec) // 60
            s = int(sec) % 60
            return f"{m:02d}:{s:02d}"

        # -------------------------------------------------------------------
        # 一键截取当前阶段瞬态声音 + 按所选主音数量 (0~3) 更新图例与 88 键钢琴
        # -------------------------------------------------------------------
        def _capture_snapshot_data_only(self) -> float:
            active_sr = (
                self.loopback_capture.sample_rate
                if self.analysis_source_mode == "system_loopback"
                else self.stream_sample_rate
            )
            window_ms = float(self.combo_snap_ms.currentData())
            n_slice = max(32, int(round((window_ms / 1000.0) * active_sr)))
            n_context = max(n_slice, int(round(0.08 * active_sr)))

            with self._history_lock:
                tail_context = self._rolling_history[-n_context:].copy()

            if np.max(np.abs(tail_context)) < 1e-6 and self.audio_buffer is not None:
                center = self.play_cursor
                half_s = n_slice // 2
                start_s = max(0, center - half_s)
                end_s = min(self.audio_buffer.shape[0], start_s + n_slice)
                self.captured_snippet = self.audio_buffer[start_s:end_s].copy()

                half_c = n_context // 2
                start_c = max(0, center - half_c)
                end_c = min(self.audio_buffer.shape[0], start_c + n_context)
                self.captured_context_audio = self.audio_buffer[start_c:end_c].copy()
                self.captured_sr = self.orig_sample_rate
            else:
                self.captured_snippet = tail_context[-n_slice:].copy()
                self.captured_context_audio = tail_context
                self.captured_sr = active_sr
            return window_ms

        def on_capture_audio_snapshot(self) -> None:
            window_ms = self._capture_snapshot_data_only()
            self.bottom_tabs.setCurrentIndex(3)
            self._refresh_snapshot_plots(preserve_piano_bpm=False)
            self.status_bar.showMessage(
                f"📸 已截取 {window_ms:.0f}ms 瞬态声音！当前显示主音数量：{self.preset.peak_annotation_count} 个。"
            )

        def _refresh_snapshot_plots(
            self,
            preserve_piano_bpm: bool = False,
            precomputed_freqs: Optional[np.ndarray] = None,
            precomputed_mag_db: Optional[np.ndarray] = None,
            precomputed_peaks: Optional[List[Dict[str, object]]] = None,
        ) -> None:
            if self.captured_snippet is None or self.captured_snippet.size == 0:
                return

            sr = self.captured_sr
            snippet = self.captured_snippet
            mono_snip = np.mean(snippet, axis=1) if snippet.ndim > 1 else snippet
            n_samples = len(mono_snip)

            half_ms = (n_samples / float(sr)) * 500.0
            t_ms = np.linspace(-half_ms, half_ms, n_samples)
            self.snap_wave_main_curve.setData(t_ms, mono_snip)

            if precomputed_freqs is not None and precomputed_mag_db is not None and precomputed_peaks is not None:
                freqs = precomputed_freqs
                mag_db = precomputed_mag_db
                self.captured_top_peaks = list(precomputed_peaks)
            else:
                ctx = self.captured_context_audio if self.captured_context_audio is not None else snippet
                freqs, mag_db, _ = AudioDSPEngine.compute_fft_spectrum_db(ctx, sample_rate=sr, fft_size=4096)
                self.captured_top_peaks = AudioDSPEngine.find_top_n_spectral_peaks(
                    freqs, mag_db, top_n=3, min_db=-85.0
                )

            valid = freqs >= MIN_PLOT_FREQ_HZ
            f_valid = freqs[valid]
            db_valid = mag_db[valid]
            use_log = self.chk_log_freq.isChecked()
            x_plot = np.log10(f_valid) if use_log else f_valid

            peak_cnt = self.preset.peak_annotation_count
            self.snap_spec_curve.setData(x_plot, db_valid)
            self.snap_top3_annotator.update_peaks(
                self.captured_top_peaks, use_log_x=use_log, count_limit=peak_cnt
            )

            # 仅当时域波形视图可见（或非节拍后台刷新）时才重建图例与正弦分解曲线，彻底杜绝后台重排卡顿
            if not preserve_piano_bpm or self.left_snap_stack.currentIndex() == 0:
                t_sec = np.arange(n_samples, dtype=np.float64) / float(sr)
                self.snap_wave_legend.clear()
                self.snap_wave_legend.addItem(
                    self.snap_wave_main_curve, "🟢 绿色实线: 截取的真实合成声波"
                )
                rank_badges = ["🟡 金色虚线 (#1第一主音)", "🔵 青色虚线 (#2第二主音)", "🟣 粉紫虚线 (#3第三主音)"]

                for i in range(3):
                    if i < peak_cnt and i < len(self.captured_top_peaks):
                        p = self.captured_top_peaks[i]
                        f_hz = float(p["freq_hz"])
                        db_v = float(p["mag_db"])
                        note_s = str(p["note"])
                        amp = min(0.95, float(p["amp_linear"]))
                        sine_comp = amp * np.sin(2.0 * np.pi * f_hz * t_sec)
                        self.snap_sine_overlays[i].setData(t_ms, sine_comp)
                        self.snap_wave_legend.addItem(
                            self.snap_sine_overlays[i],
                            f"{rank_badges[i]}: {note_s} ({f_hz:.1f}Hz, {db_v:.1f}dB) 正弦波",
                        )
                    else:
                        self.snap_sine_overlays[i].setData([], [])

            if not (preserve_piano_bpm and self.chk_bpm_capture.isChecked() and self.chk_piano_live_follow.isChecked()):
                self._bpm_locked_peaks = list(self.captured_top_peaks)
                self.piano_88_widget.set_highlighted_peaks(self.captured_top_peaks[:peak_cnt])

        def _trigger_snapshot_audio_playback(self, audio_arr: np.ndarray, desc: str) -> None:
            if audio_arr.size == 0:
                return
            if self.captured_sr != self.stream_sample_rate:
                audio_arr = AudioDSPEngine.offline_resample(
                    audio_arr,
                    orig_sr=self.captured_sr,
                    target_sr=self.stream_sample_rate,
                    target_bit_depth=32,
                    target_channels=2,
                )
            with self._snapshot_lock:
                self.snapshot_play_buffer = audio_arr
                self.snapshot_play_cursor = 0
            dur_s = audio_arr.shape[0] / float(self.stream_sample_rate)
            self.status_bar.showMessage(f"🔊 正在试听截取声音：{desc} (时长 {dur_s:.2f} 秒)")

        def on_play_snapshot_raw(self) -> None:
            if self.captured_snippet is None:
                self.on_capture_audio_snapshot()
            if self.captured_snippet is not None:
                buf = AudioDSPEngine.build_raw_snippet_playback(
                    self.captured_snippet, self.captured_sr
                )
                self._trigger_snapshot_audio_playback(buf, "原样短促播放")

        def on_play_snapshot_sustain(self) -> None:
            if self.captured_snippet is None:
                self.on_capture_audio_snapshot()
            if self.captured_snippet is not None:
                use_harm = bool(self.combo_sustain_type.currentData())
                cnt = max(1, self.preset.peak_annotation_count)
                buf = AudioDSPEngine.build_sustain_playback(
                    self.captured_snippet,
                    self.captured_sr,
                    duration_sec=1.5,
                    top_peaks=self.captured_top_peaks[:cnt],
                    use_harmonic_resynthesis=use_harm,
                )
                mode_name = f"前{cnt}主音纯谐波延音" if use_harm else "无缝原波循环延音"
                self._trigger_snapshot_audio_playback(buf, f"拉长延音播放 1.5秒 ({mode_name})")

        def on_play_snapshot_slow_stretch(self) -> None:
            if self.captured_snippet is None:
                self.on_capture_audio_snapshot()
            if self.captured_snippet is not None:
                factor = float(self.combo_slow_factor.currentData())
                keep_pitch = bool(self.combo_slow_mode.currentData())
                buf = AudioDSPEngine.build_time_stretched_playback(
                    self.captured_snippet,
                    self.captured_sr,
                    stretch_factor=factor,
                    keep_pitch=keep_pitch,
                )
                mode_str = "保持原音高慢放" if keep_pitch else "物理降频慢放"
                self._trigger_snapshot_audio_playback(buf, f"{factor:.0f}x 倍慢速拉伸播放 ({mode_str})")

        # -------------------------------------------------------------------
        # 事件处理：文件拖拽、自动转换同目录 WAV 与加载播放
        # -------------------------------------------------------------------
        def dragEnterEvent(self, event: QDragEnterEvent) -> None:
            if event.mimeData().hasUrls():
                event.acceptProposedAction()

        def dropEvent(self, event: QDropEvent) -> None:
            urls = event.mimeData().urls()
            if not urls:
                return
            file_path = urls[0].toLocalFile()
            if file_path and os.path.isfile(file_path):
                self.load_audio_file(file_path)

        def on_open_audio_file(self) -> None:
            path, _ = QFileDialog.getOpenFileName(
                self,
                "选择要播放与分析的音乐文件（非 WAV 格式将自动转换并保存为同目录同名 .wav）",
                "",
                "音频文件 (*.wav *.mp3 *.flac *.ogg *.m4a *.aac *.wma *.opus);;所有文件 (*.*)",
            )
            if path:
                self.load_audio_file(path)

        def load_audio_file(self, file_path: str) -> None:
            try:
                orig_ext = os.path.splitext(file_path)[1].lower()
                audio, sr, resolved_wav_path = UniversalAudioFormatConverter.load_and_auto_convert_to_wav(
                    file_path
                )
                self.current_file_path = resolved_wav_path
                self.audio_buffer = audio
                self.orig_sample_rate = sr
                self.play_cursor = 0
                self._peak_hold_db = None
                self._has_rendered_live_frame = False
                self._realtime_eq_processor.reset()
                self._wsola_state.clear()

                # 加载新歌曲时自动重置节拍起点
                self._bpm_anchor_time = time.perf_counter()
                self._bpm_last_beat_index = -1

                if sr != self.stream_sample_rate:
                    self.stream_sample_rate = sr
                    self._init_audio_output_stream()

                duration = audio.shape[0] / float(sr)
                if orig_ext != ".wav":
                    info_msg = (
                        f"已转同名WAV: {os.path.basename(resolved_wav_path)} "
                        f"| {sr} Hz | {duration:.1f}s"
                    )
                else:
                    info_msg = (
                        f"当前文件: {os.path.basename(resolved_wav_path)} | {sr} Hz | {duration:.1f}s"
                    )

                self.lbl_current_file.setText(info_msg)
                self.lbl_current_file.setToolTip(resolved_wav_path)
                self.is_playing_file = True
                self.btn_play_pause.setChecked(True)
                self.btn_play_pause.setText("⏸ 暂停")
                self.status_bar.showMessage(f"加载成功: {resolved_wav_path}")
            except Exception as exc:
                QMessageBox.warning(self, "音频导入与自动转换提示", str(exc))

        def on_toggle_play_file(self, checked: bool) -> None:
            if self.audio_buffer is None:
                self.btn_play_pause.setChecked(False)
                QMessageBox.information(self, "提示", "请先拖入或点击导入一个音乐文件！")
                return
            self.is_playing_file = checked
            if checked:
                self._bpm_anchor_time = time.perf_counter()
                self._bpm_last_beat_index = -1
            self.btn_play_pause.setText("⏸ 暂停" if checked else "▶ 播放")

        def on_stop_playback(self) -> None:
            self.is_playing_file = False
            self.play_cursor = 0
            self._realtime_eq_processor.reset()
            self._wsola_state.clear()
            self.btn_play_pause.setChecked(False)
            self.btn_play_pause.setText("▶ 播放")
            self.slider_seek.setValue(0)

        def on_seek_slider_moved(self, val: int) -> None:
            if self.audio_buffer is not None:
                self.play_cursor = int((val / 1000.0) * self.audio_buffer.shape[0])
                self._realtime_eq_processor.reset()
                self._wsola_state.clear()

        def on_toggle_analysis_source(self, checked: bool) -> None:
            if checked:
                ok, msg = self.loopback_capture.start(callback=self._on_loopback_chunk_received)
                if not ok:
                    self.btn_switch_source.setChecked(False)
                    QMessageBox.warning(self, "系统媒体捕获启动失败", msg)
                    return
                self.analysis_source_mode = "system_loopback"
                self._peak_hold_db = None
                self.btn_switch_source.setText("分析源：系统声音")
                self.status_bar.showMessage(msg)
            else:
                self.analysis_source_mode = "file_or_synth"
                self._peak_hold_db = None
                self.btn_switch_source.setText("分析源：软件内部")
                self.status_bar.showMessage("已切换回分析软件内部选中的音乐文件与合成器声音。")

        # -------------------------------------------------------------------
        # 事件处理：EQ 曲线、泛音合成器与自定义公式 (模式 A / B / C)
        # -------------------------------------------------------------------
        def _on_eq_curve_updated(self, pts: List[List[float]]) -> None:
            self.preset.eq_points = pts

        def on_apply_eq_preset(self, name: str) -> None:
            if name in EQ_CURVE_PRESETS:
                pts = [list(p) for p in EQ_CURVE_PRESETS[name]]
                self.preset.eq_points = pts
                self.eq_curve_widget.set_control_points(pts, emit_signal=False)

        def _on_harmonic_curve_updated(self, pts: List[List[float]]) -> None:
            self.preset.harmonic_curve_points = pts
            self._refresh_static_harmonic_preview()

        def on_apply_instrument_preset(self, name: str) -> None:
            if name in INSTRUMENT_ENVELOPE_PRESETS:
                pts = [list(p) for p in INSTRUMENT_ENVELOPE_PRESETS[name]]
                self.preset.harmonic_curve_points = pts
                self.harmonic_curve_widget.set_control_points(pts, emit_signal=False)
                self._refresh_static_harmonic_preview()

        def _on_f0_changed(self, val: float) -> None:
            self.preset.fundamental_hz = float(val)
            self._refresh_static_harmonic_preview()

        def _on_harmonics_count_changed(self, val: int) -> None:
            self.preset.num_harmonics = int(val)
            self._refresh_static_harmonic_preview()

        def _refresh_static_harmonic_preview(self) -> None:
            k_indices = np.arange(1, self.preset.num_harmonics + 1, dtype=np.float64)
            freqs = k_indices * self.preset.fundamental_hz
            freqs = freqs[freqs <= MAX_PLOT_FREQ_HZ]
            dbs = AudioDSPEngine.evaluate_smooth_curve_db(
                self.preset.harmonic_curve_points, freqs, min_db=-80.0, max_db=0.0
            )
            self.harmonic_curve_widget.set_harmonic_indicators(list(zip(freqs.tolist(), dbs.tolist())))

        def _on_synth_mode_changed(self, idx: int) -> None:
            if idx == 0:
                self.preset.synth_mode = "harmonic_curve"
            elif idx == 1:
                self.preset.synth_mode = "multi_osc"
            else:
                self.preset.synth_mode = "custom_formula"

        def _activate_mode_c_directly(self) -> None:
            self.combo_synth_mode.setCurrentIndex(2)
            self.preset.synth_mode = "custom_formula"
            if not self.is_playing_synth:
                self.btn_play_synth.setChecked(True)
                self.on_toggle_synth_playback(True)
            self.status_bar.showMessage(
                f"已切换至【模式 C：纯自定义数学公式发声】| 当前公式: f(x) = {self.preset.custom_formula} | 基频: {self.preset.fundamental_hz:.2f} Hz"
            )

        def on_toggle_synth_playback(self, checked: bool) -> None:
            self._sync_osc_table_to_preset()
            self.is_playing_synth = checked
            self.btn_play_synth.setText("🔇 停止合成器" if checked else "🔊 开启合成器发声")

        def _on_custom_formula_edited(self, text: str) -> None:
            self.preset.custom_formula = text
            try:
                test_x = np.linspace(0, 2 * np.pi, 64)
                _ = AudioDSPEngine.eval_waveform_or_formula(test_x, "custom", text)
                self.lbl_formula_status.setText("✅ 有效")
                self.lbl_formula_status.setStyleSheet("color: #10B981;")
            except Exception as exc:
                self.lbl_formula_status.setText("❌ 语法错误")
                self.lbl_formula_status.setToolTip(str(exc))
                self.lbl_formula_status.setStyleSheet("color: #EF4444;")

        def _populate_osc_table_from_preset(self) -> None:
            self.table_osc.setRowCount(0)
            for osc_dict in self.preset.oscillators:
                osc = OscillatorItem(**osc_dict)
                self._insert_osc_row(osc)

        def _insert_osc_row(self, osc: OscillatorItem) -> None:
            row = self.table_osc.rowCount()
            self.table_osc.insertRow(row)

            chk = QCheckBox()
            chk.setChecked(osc.enabled)
            chk.toggled.connect(self._sync_osc_table_to_preset)
            self.table_osc.setCellWidget(row, 0, chk)

            combo = QComboBox()
            combo.addItems(["sine", "square", "triangle", "sawtooth", "custom", "noise"])
            combo.setCurrentText(osc.waveform)
            combo.currentTextChanged.connect(self._sync_osc_table_to_preset)
            self.table_osc.setCellWidget(row, 1, combo)

            sp_freq = QDoubleSpinBox()
            sp_freq.setRange(15.0, 20000.0)
            sp_freq.setValue(osc.freq_hz)
            sp_freq.valueChanged.connect(self._sync_osc_table_to_preset)
            self.table_osc.setCellWidget(row, 2, sp_freq)

            sp_db = QDoubleSpinBox()
            sp_db.setRange(-80.0, 0.0)
            sp_db.setValue(osc.gain_db)
            sp_db.valueChanged.connect(self._sync_osc_table_to_preset)
            self.table_osc.setCellWidget(row, 3, sp_db)

            edit_f = QLineEdit(osc.formula)
            edit_f.textChanged.connect(self._sync_osc_table_to_preset)
            self.table_osc.setCellWidget(row, 4, edit_f)

        def _sync_osc_table_to_preset(self) -> None:
            items = []
            for r in range(self.table_osc.rowCount()):
                chk = self.table_osc.cellWidget(r, 0)
                combo = self.table_osc.cellWidget(r, 1)
                sp_freq = self.table_osc.cellWidget(r, 2)
                sp_db = self.table_osc.cellWidget(r, 3)
                edit_f = self.table_osc.cellWidget(r, 4)
                if chk and combo and sp_freq and sp_db and edit_f:
                    items.append(
                        asdict(
                            OscillatorItem(
                                enabled=chk.isChecked(),
                                waveform=combo.currentText(),
                                freq_hz=float(sp_freq.value()),
                                gain_db=float(sp_db.value()),
                                formula=edit_f.text(),
                            )
                        )
                    )
            self.preset.oscillators = items

        def on_add_oscillator_row(self) -> None:
            self._insert_osc_row(OscillatorItem(True, "sine", 550.0, -15.0, 0.0, "sin(x)"))
            self._sync_osc_table_to_preset()

        def on_remove_oscillator_row(self) -> None:
            cur = self.table_osc.currentRow()
            if cur >= 0:
                self.table_osc.removeRow(cur)
                self._sync_osc_table_to_preset()

        # -------------------------------------------------------------------
        # 事件处理：一键重采样转换、双源录制、JSON 预设与一键打包 .exe
        # -------------------------------------------------------------------
        def on_one_click_resample_export(self) -> None:
            if self.audio_buffer is None:
                QMessageBox.information(self, "提示", "请先导入或拖入一个音乐文件再执行一键采样转换！")
                return

            ext = self.combo_export_format.currentText()
            default_name = f"resampled_{self.preset.target_sample_rate}Hz_{self.preset.target_bit_depth}bit.{ext}"
            out_path, _ = QFileDialog.getSaveFileName(
                self, "保存重采样并转换格式后的音频文件", default_name, f"音频文件 (*.{ext})"
            )
            if not out_path:
                return

            try:
                audio_to_save = self.audio_buffer.copy()
                if self.preset.eq_enabled:
                    audio_to_save = AudioDSPEngine.apply_eq_curve_block(
                        audio_to_save, self.orig_sample_rate, self.preset.eq_points
                    )

                saved_path = UniversalAudioFormatConverter.export_and_convert_audio(
                    audio_data=audio_to_save,
                    orig_sr=self.orig_sample_rate,
                    output_path=out_path,
                    target_sr=int(self.combo_target_sr.currentData()),
                    target_bit_depth=int(self.combo_target_bits.currentData()),
                    target_channels=int(self.combo_target_ch.currentData()),
                    bitrate=self.combo_export_bitrate.currentText(),
                )
                QMessageBox.information(
                    self,
                    "一键重采样导出成功",
                    f"已成功按设定参数重采样并保存至:\n{saved_path}",
                )
            except Exception as exc:
                QMessageBox.critical(self, "导出失败", str(exc))

        def on_toggle_recording(self, checked: bool) -> None:
            if checked:
                self.record_source_mode = self.combo_rec_source.currentData()
                if self.record_source_mode == "loopback" and not self.loopback_capture.running:
                    ok, msg = self.loopback_capture.start(callback=self._on_loopback_chunk_received)
                    if not ok:
                        self.btn_start_rec.setChecked(False)
                        QMessageBox.warning(self, "内录启动失败", msg)
                        return

                with self._record_lock:
                    self.recorded_chunks.clear()
                self.record_start_time = time.time()
                self.is_recording = True
                self.btn_start_rec.setText("⏹ 停止录制")
                self.status_bar.showMessage("已开始录制音频...")
            else:
                self.is_recording = False
                self.btn_start_rec.setText("⏺ 开始录制声音")
                elapsed = time.time() - self.record_start_time
                self.lbl_rec_status.setText(f"状态：录制完成 (共 {elapsed:.1f} 秒，可点击右侧按钮导出)")
                self.status_bar.showMessage("录制已停止，请点击“导出保存录音文件”进行保存。")

        def on_export_recorded_audio(self) -> None:
            with self._record_lock:
                if not self.recorded_chunks:
                    QMessageBox.information(self, "提示", "当前没有已录制的音频数据，请先点击“开始录制声音”。")
                    return
                full_rec = np.vstack(self.recorded_chunks)
                rec_sr = self.recorded_source_sr

            ext = self.combo_rec_format.currentText()
            out_path, _ = QFileDialog.getSaveFileName(
                self, "导出录制的声音文件", f"recorded_audio.{ext}", f"音频文件 (*.{ext})"
            )
            if not out_path:
                return

            try:
                saved_path = UniversalAudioFormatConverter.export_and_convert_audio(
                    audio_data=full_rec,
                    orig_sr=rec_sr,
                    output_path=out_path,
                    target_sr=int(self.combo_rec_sr.currentData()),
                    target_bit_depth=int(self.combo_rec_bits.currentData()),
                    target_channels=int(self.combo_rec_ch.currentData()),
                    bitrate="192k",
                )
                QMessageBox.information(self, "录音保存成功", f"录音已成功保存至:\n{saved_path}")
            except Exception as exc:
                QMessageBox.critical(self, "录音导出失败", str(exc))

        def on_export_preset_json(self) -> None:
            self._sync_osc_table_to_preset()
            path, _ = QFileDialog.getSaveFileName(
                self, "导出均衡器与泛音合成配置 (.json)", "audio_studio_preset.json", "JSON 配置文件 (*.json)"
            )
            if not path:
                return
            try:
                with open(path, "w", encoding="utf-8") as f:
                    f.write(self.preset.to_json())
                QMessageBox.information(self, "导出成功", f"配置已导出至:\n{path}")
            except Exception as exc:
                QMessageBox.critical(self, "导出失败", str(exc))

        def on_import_preset_json(self) -> None:
            path, _ = QFileDialog.getOpenFileName(
                self, "导入均衡器与泛音合成配置 (.json)", "", "JSON 配置文件 (*.json)"
            )
            if not path:
                return
            try:
                with open(path, "r", encoding="utf-8") as f:
                    loaded = AppPresetConfig.from_json(f.read())
                self.preset = loaded
                self.chk_eq_enable.setChecked(loaded.eq_enabled)
                self.eq_curve_widget.set_control_points(loaded.eq_points, emit_signal=False)
                self.harmonic_curve_widget.set_control_points(loaded.harmonic_curve_points, emit_signal=False)
                self.spin_f0.setValue(loaded.fundamental_hz)
                self.spin_harmonics.setValue(loaded.num_harmonics)
                self.combo_base_wave.setCurrentText(loaded.base_waveform)
                self.edit_custom_formula.setText(loaded.custom_formula)
                self.chk_live_resample.setChecked(loaded.resample_live_enabled)
                cnt = max(0, min(3, int(getattr(loaded, "peak_annotation_count", 1))))
                self.combo_peak_count.setCurrentIndex(cnt)
                self.spin_bpm.setValue(float(getattr(loaded, "bpm_value", 60.0)))
                self.chk_bpm_sync_snapshot.setChecked(bool(getattr(loaded, "bpm_sync_right_snapshot", False)))
                self.chk_bpm_capture.setChecked(bool(getattr(loaded, "bpm_capture_enabled", False)))
                if loaded.synth_mode == "harmonic_curve":
                    self.combo_synth_mode.setCurrentIndex(0)
                elif loaded.synth_mode == "multi_osc":
                    self.combo_synth_mode.setCurrentIndex(1)
                else:
                    self.combo_synth_mode.setCurrentIndex(2)
                self._populate_osc_table_from_preset()
                self._refresh_static_harmonic_preview()
                QMessageBox.information(self, "导入成功", f"已成功加载预设配置:\n{path}")
            except Exception as exc:
                QMessageBox.critical(self, "导入配置失败", str(exc))

        def on_build_standalone_exe(self) -> None:
            reply = QMessageBox.question(
                self,
                "一键打包免安装独立程序 (.exe)",
                "是否立即使用 PyInstaller 将当前程序及所有音频解码依赖打包为免安装独立程序 (dist/AudioStudioPro)？\n"
                "（您也可以直接双击软件根目录下的 build_exe.bat 进行打包）",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No,
            )
            if reply != QMessageBox.StandardButton.Yes:
                return
            self.status_bar.showMessage("正在后台打包免安装独立程序 (.exe)，请稍候...")
            QApplication.processEvents()
            ok, msg = build_standalone_exe_package()
            if ok:
                QMessageBox.information(self, "打包成功", msg)
            else:
                QMessageBox.warning(self, "打包提示", msg)
            self.status_bar.showMessage("就绪。")

        def on_show_disclaimer_dialog(self) -> None:
            dlg = DisclaimerAgreementDialog(self, require_checkbox=False)
            dlg.exec()

        def closeEvent(self, event) -> None:
            self.is_playing_file = False
            self.is_playing_synth = False
            self.loopback_capture.stop()
            if self.output_stream is not None:
                try:
                    self.output_stream.stop()
                    self.output_stream.close()
                except Exception:
                    pass
            super().closeEvent(event)



def get_app_window_icon():
    if not HAS_QT:
        return None
    root_dir = get_app_root_dir()
    for name in ("app_icon.ico", "app_icon.png", "../assets/app_icon.ico", "../assets/app_icon.png"):
        cand = (root_dir / name).resolve()
        if cand.exists():
            return QIcon(str(cand))
    return None


def main() -> None:
    if "--build-exe" in sys.argv:
        ok, msg = build_standalone_exe_package()
        print(msg)
        sys.exit(0 if ok else 1)

    if not HAS_QT:
        print(
            "错误：未检测到 PyQt6 或 pyqtgraph。\n"
            "请在 Windows 终端运行以下命令安装依赖后再启动：\n"
            "pip install -r requirements.txt"
        )
        sys.exit(1)

    if os.name == "nt":
        try:
            import ctypes
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID("AudioStudioPro.Workstation.1")
        except Exception:
            pass
    app = QApplication(sys.argv)
    app_icon = get_app_window_icon()
    if app_icon is not None:
        app.setWindowIcon(app_icon)
    pg.setConfigOptions(antialias=True)
    ensure_disclaimer_file_on_disk()
    if not is_disclaimer_accepted():
        dlg = DisclaimerAgreementDialog(require_checkbox=True)
        if dlg.exec() != QDialog.DialogCode.Accepted:
            sys.exit(0)
    win = AudioStudioMainWindow()
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()