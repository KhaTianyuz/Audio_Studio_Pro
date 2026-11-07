# 软件使用免责协议、开源许可证与第三方开源组件致谢 (DISCLAIMER & OPEN-SOURCE LICENSES)

> **重要法律提示**：在使用 **Audio Studio Pro（音乐频谱看音、88键钢琴跟谱与调音工具）**（包括但不限于本项目的 Python 版本、C++20 版本、源代码、编译脚本及打包后的 `.exe` 可执行程序）之前，请您务必仔细阅读并充分理解本《免责/开源协议》的全部内容。
> **当您首次打开软件勾选“我已阅读 DISCLAIMER.md 并遵守以下免责/开源协议”、或以任何方式下载、安装、运行、修改、复制、传播本软件，即视为您已完全阅读、理解并无条件同意本协议的全部内容。如果您不同意本协议中的任何一条内容，请立即停止使用并彻底删除本软件的所有文件。**

---

## 第一部分：软件使用免责协议与风险告知书 (Disclaimer of Warranty & Liability)

### 第一条：完全自担风险与作者免责总则
1. 本软件系个人开发的免费开源工具（基于 GNU AGPL v3.0 协议发布，官方开源仓库：`https://github.com/KhaTianyuz/Audio_Studio_Pro`），仅供个人日常音乐欣赏、乐理学习与声学兴趣体验使用。
2. 用户下载、安装、运行、操作或修改本软件纯属个人自愿行为。**无论是谁在何种场景下使用本软件，因使用或无法使用本软件所引发的一切直接、间接、偶然、特殊、附带或衍生后果、事故、纠纷、损失与风险，均由使用者本人 100% 独立承担，与本软件作者无任何关系，作者在任何情况下均不承担任何民事、行政或刑事责任，亦不承担任何经济赔偿义务。**

### 第二条：听力健康安全与音响/耳机/电脑硬件免责
1. 本软件包含**均衡器音量增益调节（最高可放大 +24dB）**、**自定义声音发生器**、**多振荡器叠加合成**以及**超低频/超高频声波发生**功能。
2. 若因用户将音量调得过大、大幅拉高特定频段音量、输入剧烈变化的公式，或在佩戴入耳式耳机、头戴式耳机、连接外置音箱/功放/声卡时操作不当，可能会产生刺耳啸叫、巨大爆音或超负荷低频振动。
3. **由此导致的任何个人或他人耳鸣、耳痛、听力下降、永久性听力损伤等身体健康损害，或导致的耳机振膜破裂、音响喇叭劈响烧毁、功放损坏、电脑声卡或主板硬件故障，一切后果与维修、医疗费用均由用户本人自行承担，与本软件作者无关。**
4. **安全使用建议**：在开启合成器发声或大幅调节均衡器线条前，请务必先摘下耳机并将电脑系统音量调至 20% 以下，确认声音柔和正常后再逐步调高音量。

### 第三条：音乐版权合规与“录制电脑声音”法律责任免责
1. 本软件提供的“导入音乐自动转换格式”、“重采样导出音频”以及“录制电脑系统正在播放的声音”功能，仅限用于用户本人拥有合法著作权、已获授权或处于公有领域的音频素材，以及个人非商业性的离线学习研究。
2. **严禁任何用户利用本软件盗录、翻录、破解、批量转换、公开传播或商业售卖受著作权法保护的付费音乐、流媒体音视频、影视原声或他人私密通话/语音。**
3. 用户在使用上述功能时必须严格遵守所在国家或地区的《著作权法》及相关法律法规。**因用户违法录制、转换、传播音频或侵犯第三方知识产权、隐私权所引起的一切法律纠纷、平台封禁、索赔或行政/刑事处罚，均由用户本人全权负责，与本软件作者无任何关联。**

### 第四条：同名文件自动覆盖与个人数据安全免责
1. 为方便普通用户导入各种格式的音乐，当您向本软件拖入或导入非 `.wav` 格式的音乐文件（例如 `song.mp3`、`music.flac` 等）时，**软件会自动在该音乐文件所在的同一个文件夹内，转换并直接覆盖生成同名的 `.wav` 文件（即自动覆盖 `song.wav`）**。
2. 请用户在使用前自行备份重要音频文件，切勿将珍贵的原始 `.wav` 录音或母带文件与待导入的其他格式文件放在同一目录下并使用相同文件名。
3. **因软件自动同名覆盖、用户误操作、导出路径选择不当、程序异常中断或磁盘读写故障导致的任何文件被覆盖、原文件丢失或数据损坏，责任完全由用户自行承担，作者概不负责。**

### 第五条：系统兼容性、杀毒软件误报与运行故障免责
1. 本软件不保证在所有 Windows 操作系统版本、不同品牌的电脑主板、外置/内置声卡驱动、蓝牙耳机驱动或第三方桌面环境下均能正常、稳定、无误地运行。
2. 使用打包工具（如 PyInstaller 或 C++ 编译器）生成的 `.exe` 独立程序可能会被部分杀毒软件或系统安全中心误报拦截。用户应自行判断并决定是否信任或运行。
3. **若因运行本软件导致电脑卡顿、声音破音、声卡短暂无声、软件闪退、系统蓝屏死机、与其他正在运行的软件发生冲突，或因此造成未保存的工作内容丢失，作者不承担任何责任。**

### 第六条：音名识别准确性与“按原样 (AS-IS)”无担保条款
1. 本软件按照**“原样 (AS-IS)”**提供，作者不对本软件作出任何明示或暗示的保证，包括但不限于对软件的商业适用性、特定用途适用性、持续可用性、无故障性或结果准确性的保证。
2. 软件界面上显示的频谱图形、第一/第二/第三主音音名、88 键钢琴高亮位置以及 BPM 节拍数值，受歌曲混音复杂度、伴奏鼓点干扰及声学客观规律影响，**仅作为直观辅助参考，绝不保证 100% 绝对准确**。
3. **若用户将本软件用于音乐考试、正式演出、商业编曲制作、器材检测或教学演示，因识别误差、延迟或判断失误造成的任何考评失利、演出事故或经济损失，与本软件作者无关。**

### 第七条：无强制维护与技术支持义务条款
1. 作者开发并开源本软件纯属个人技术分享行为，未向用户收取任何费用。
2. **作者不负有任何为用户解答疑问、修复软件缺陷（Bug）、提供使用指导、开发新功能或保持软件后续更新的法律或道义义务。**

### 第八条：第三方修改版与二次分发免责
1. 本软件基于 GNU Affero General Public License v3.0 (AGPL-3.0) 开源。任何个人或机构在获取本项目源码后进行的修改、重新编译、打包分发或二次开发，其行为仅代表该第三方自身，不代表原作者立场。
2. **若用户从非官方渠道下载到被他人篡改、植入恶意代码、捆绑广告或收费售卖的版本，由此造成的一切财产损失、隐私泄露或电脑中毒，均与原作者无任何关系。**

### 第九条：未成年人使用特别提示
1. 若使用者为未成年人，必须在父母或法定监护人的全程指导与监督下阅读本协议并使用本软件（特别是涉及耳机音量调节与合成器发声功能时），监护人应承担全部监护与安全保障责任。

### 第十条：放弃追索权与协议可分割性
1. **一旦您勾选同意本协议或开始使用本软件，即表示您本人（及您的继承人、代理人或关联方）永久、无条件且不可撤销地放弃因本软件或与本软件相关的任何事项向本软件作者提起任何索赔、投诉、仲裁、诉讼或要求赔偿的权利。**
2. 如本协议中的任何条款因适用法律被认定为部分无效或不可执行，不影响其余条款的法律效力，其余条款仍具完全约束力。

---

## 第二部分：本项目开源协议与第三方开源组件致谢声明 (Open-Source Licenses & Third-Party Acknowledgments)

### 一、本项目主开源协议 (GNU AGPL v3.0)
- **项目名称**: Audio Studio Pro
- **开源仓库地址**: `https://github.com/KhaTianyuz/Audio_Studio_Pro`
- **主许可证**: **GNU Affero General Public License v3.0 (AGPL-3.0)**（完整英文法律文本见根目录 `LICENSE` 文件）。
- **核心自研代码范围**: 本软件主程序核心算法与业务逻辑（包括但不限于实时 FFT 超分辨抛物线寻峰、64-bit 级联 RBJ Biquad 参量均衡器、跨块相位连续 WSOLA 倍速引擎、5.1/7.1 环绕声矩阵上混与 120Hz 巴特沃斯 LFE 分频器、逆波兰字节码数学公式引擎、88 键标准钢琴可视化与 BPM 节拍跟谱器、原生 Win32 WASAPI 虚拟内录引擎、64-tap Blackman-Harris 多相 Sinc 重采样器以及符合 RFC 9639 标准的原生 FLAC/WAV 无损编码器）均为项目原创自研代码。
- **开源分发权利**: 您可以自由运行、复制、研究、修改本软件及发布构建后的 `.exe` 程序；若您修改了本软件源码、发布了衍生作品或通过网络部署服务，必须以相同的 **AGPL-3.0** 协议公开完整源代码，并完整保留本免责/开源协议声明。

### 二、C++20 原生版 (`cpp_version/`) 使用的第三方开源组件与致谢
本项目 C++20 原生版在构建过程中使用了以下优秀的第三方开源项目，特此向原作者致以诚挚感谢，并依据各开源协议要求保留版权与许可证声明：

#### 1. Dear ImGui (v1.90.8) — MIT License
- **用途**: C++20 版即时模式图形用户界面 (GUI) 渲染框架
- **作者与版权**: `Copyright (c) 2014-2024 Omar Cornut and ImGui contributors`
- **官方仓库**: `https://github.com/ocornut/imgui`

#### 2. ImPlot (v0.16) — MIT License
- **用途**: C++20 版高性能实时频谱与交互式曲线绘图库
- **作者与版权**: `Copyright (c) 2020-2023 Evan Pezent`
- **官方仓库**: `https://github.com/epezent/implot`

#### 3. miniaudio (v0.11.21 接口规范) — Public Domain (Unlicense) / MIT-0
- **用途**: 跨平台音频播放设备抽象接口（本项目内置了原生 Win32 WASAPI 轻量级实现）
- **原作者**: `David Reid (mackron)` (`https://github.com/mackron/miniaudio`)

**【MIT License 许可证声明全文（适用于 Dear ImGui、ImPlot 等 MIT 协议组件）】**：
```text
Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### 三、Python 版 (`python_version/`) 使用的第三方开源组件与致谢
本项目 Python 版依赖以下开源 Python 生态库，特此向各开源社区及维护者致谢并遵循其开源协议：

1. **PyQt6 (`>=6.5.0`)** — **GNU GPL v3.0**
   - **版权与来源**: `Copyright (c) Riverbank Computing Limited / The Qt Company` (`https://www.riverbankcomputing.com/software/pyqt/`)
   - **合规说明**: PyQt6 采用 GPL v3.0 授权，本项目整体采用与之兼容的 GNU AGPL v3.0 协议开放全部源代码。
2. **pyqtgraph (`>=0.13.3`)** — **MIT License**
   - **版权与来源**: `Copyright (c) Luke Campagnola and contributors` (`https://github.com/pyqtgraph/pyqtgraph`)
3. **NumPy (`>=1.24.0`)** — **BSD-3-Clause License**
   - **版权与来源**: `Copyright (c) 2005-2024, NumPy Developers` (`https://numpy.org/`)
4. **SciPy (`>=1.10.0`)** — **BSD-3-Clause License**
   - **版权与来源**: `Copyright (c) 2001-2024, SciPy Developers` (`https://scipy.org/`)
5. **PyAudioWPatch (`>=0.2.12.6`)** — **MIT License**
   - **版权与来源**: `Copyright (c) Hubert Pham (PyAudio) / s0d3s (PyAudioWPatch)` (`https://github.com/s0d3s/PyAudioWPatch`)
6. **sounddevice (`>=0.4.6`)** — **MIT License**
   - **版权与来源**: `Copyright (c) 2015-2024 Matthias Geier` (`https://python-sounddevice.readthedocs.io/`)
7. **soundfile (`>=0.12.1`)** — **BSD-3-Clause License**
   - **版权与来源**: `Copyright (c) 2013, Bastian Bechtold`（底层封装 `libsndfile`，采用 LGPL-2.1+ 协议）
8. **imageio-ffmpeg (`>=0.4.9`) & FFmpeg** — **BSD-2-Clause License (包装器) / LGPL v2.1+ 或 GPL v2+ (FFmpeg 外部命令行工具)**
   - **版权与来源**: `Copyright (c) Almar Klein and contributors` / `FFmpeg Developers` (`https://ffmpeg.org/`)。本程序仅通过独立子进程命令行调用 FFmpeg 进行非 WAV 音频格式辅助转码。
9. **PyInstaller (`>=6.0.0`)** — **GPLv2+ with Bootloader Exception**
   - **版权与来源**: `Copyright (c) PyInstaller Development Team` (`https://pyinstaller.org/`)

**【BSD-3-Clause License 许可证声明全文（适用于 NumPy、SciPy、soundfile 等 BSD 协议组件）】**：
```text
Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the copyright holder nor the names of its contributors
   may be used to endorse or promote products derived from this software
   without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES ARE DISCLAIMED.
```
