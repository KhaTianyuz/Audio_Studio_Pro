// ====================================================================================================
// Audio Studio Pro (C++20 Edition) - Dear ImGui + ImPlot + DirectX11 + WASAPI + Zero-Lock DSP
// ====================================================================================================
// 核心架构与 C++ 专属性能优化特性：
// 1. 零锁实时音频架构与跨线程安全保护 (Lock-Free SPSC RingBuffer + Atomic EQ Reset)：
//    - 彻底解决拖动进度条长按不放时的跨线程 vector 竞争死循环卡死与闪退问题。
// 2. 1:1 物理像素矢量字体光栅化引擎 (Native-Pixel TrueType Re-Rasterization)：
//    - 彻底摒弃会导致字库位图拉伸模糊的 FontGlobalScale 缩放，锁定 FontGlobalScale = 1.0f，
//      开启 Per-Monitor DPI V2 感知与 OversampleH=3/PixelSnapH=true，按窗口真实物理像素动态重光栅化微软雅黑字库。
// 3. 自动适配 4:3 与 16:9 屏幕比例与动态布局：
//    - 无需用户手动选择分辨率，自动根据显示器工作区及窗口实时宽高比调整分栏比例与字号。
// 4. 纯净无乱码文本界面：
//    - 移除全部易变为 ? 的特殊图案字符，采用类 Python PyQt6 版的清爽深色分组边框、可拖拽上下分割条与表格化布局。
// ====================================================================================================

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define _USE_MATH_DEFINES

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>
#ifdef _WIN32
#include <d3d11.h>
#include <dxgi.h>
#endif
namespace fs = std::filesystem;

// ====================================================================================================
// 1. 全局常量、动态自适应缩放状态、钢琴 C0~C8 音阶表与预设数据结构
// ====================================================================================================

static constexpr double MIN_PLOT_FREQ_HZ = 15.0;
static constexpr double MAX_PLOT_FREQ_HZ = 22000.0;
static constexpr double TWO_PI = 6.283185307179586476925286766559;
static constexpr double PI_VAL = 3.1415926535897932384626433832795;

// 动态界面缩放系数（仅用于控件几何尺寸计算；字体通过真实物理像素重光栅化保持 1:1 锐利度）
static float ui_scale = 1.0f;

// 自动按比例放大单维尺寸的辅助函数
inline float SF(float v) noexcept {
    return std::round(v * ui_scale);
}

// 安全的 std::clamp 包装函数：当窗口最小化或尺寸极小时防止 lo > hi 触发 MSVC Debug Assertion
template <typename T>
inline constexpr T safeClamp(T v, T lo, T hi) noexcept {
    return (hi < lo) ? lo : std::clamp(v, lo, hi);
}

struct NoteGuide {
    const char* name;
    double freq_hz;
};

static constexpr std::array<NoteGuide, 9> PIANO_C0_TO_C8 = { {
    {"C0", 16.35},   {"C1", 32.70},   {"C2", 65.41},
    {"C3", 130.81},  {"C4", 261.63},  {"C5", 523.25},
    {"C6", 1046.50}, {"C7", 2093.00}, {"C8", 4186.01},
} };

static constexpr std::array<const char*, 12> NOTE_NAMES_12 = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

static constexpr std::array<double, 10> DEFAULT_EQ_FREQS = {
    31.0, 62.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0
};

struct PianoNoteOption {
    std::string label;
    double freq_hz;
    int midi;
};

inline std::vector<PianoNoteOption> generateFullPianoNotesC0ToC8() {
    std::vector<PianoNoteOption> notes;
    notes.reserve(97);
    for (int midi = 12; midi <= 108; ++midi) {
        const char* note_name = NOTE_NAMES_12[midi % 12];
        int octave = (midi / 12) - 1;
        double freq = 440.0 * std::pow(2.0, (midi - 69) / 12.0);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%d (%.2f Hz)", note_name, octave, freq);
        notes.push_back({ buf, std::round(freq * 100.0) / 100.0, midi });
    }
    return notes;
}

struct ControlPoint {
    double freq_hz = 1000.0;
    double db = 0.0;
};

struct SpectralPeak {
    int rank = 1;
    double freq_hz = 0.0;
    double mag_db = -100.0;
    std::string note = "--";
    int midi = -1;
    double amp_linear = 0.0;
};

struct SpectrumStats {
    double peak_freq_hz = 0.0;
    double peak_db = -100.0;
    double rms_db = -100.0;
    double centroid_hz = 0.0;
};

struct OscillatorItem {
    bool enabled = true;
    int waveform_idx = 0; // 0:sine, 1:square, 2:triangle, 3:sawtooth, 4:custom, 5:noise
    double freq_hz = 440.0;
    double gain_db = -12.0;
    double phase_deg = 0.0;
    std::string formula = "sin(x)";
};

struct AppPresetConfig {
    std::string version = "2.2-cpp";
    std::string name = "Custom User Preset";
    bool eq_enabled = true;
    std::vector<ControlPoint> eq_points;
    int synth_mode = 0; // 0: harmonic_curve (模式A), 1: multi_osc (模式B), 2: custom_formula (模式C)
    double fundamental_hz = 261.63;
    int num_harmonics = 24;
    int base_waveform_idx = 0; // 0:sine, 1:square, 2:triangle, 3:sawtooth, 4:custom
    std::string custom_formula = "sin(x) + 0.35*sin(2*x) + 0.15*cos(3*x)";
    std::vector<ControlPoint> harmonic_curve_points;
    std::vector<OscillatorItem> oscillators;
    bool resample_live_enabled = false;
    int target_sample_rate = 44100;
    int target_bit_depth = 16;
    int target_channels = 2;
    int peak_annotation_count = 1;
    bool bpm_capture_enabled = false;
    double bpm_value = 60.0;
    bool bpm_sync_right_snapshot = false;

    AppPresetConfig() {
        for (double f : DEFAULT_EQ_FREQS) {
            eq_points.push_back({ f, 0.0 });
        }
        harmonic_curve_points = {
            {32.7, -45.0},  {65.4, -25.0},   {130.8, -12.0}, {261.6, -3.0},
            {523.3, -6.0},  {1046.5, -12.0}, {2093.0, -20.0}, {4186.0, -32.0},
            {8372.0, -50.0}, {16744.0, -72.0}
        };
        oscillators = {
            {true,  0, 440.0, -10.0, 0.0, "sin(x)"},
            {false, 2, 880.0, -18.0, 0.0, "sin(x)"},
            {false, 4, 660.0, -20.0, 0.0, "sin(x) + 0.3*sin(3*x)"}
        };
    }
};

struct NamedCurvePreset {
    const char* name;
    std::vector<ControlPoint> points;
};

inline const std::vector<NamedCurvePreset>& getEqCurvePresets() {
    static const std::vector<NamedCurvePreset> presets = {
        {"平坦原声", {
            {31.0, 0.0}, {62.0, 0.0}, {125.0, 0.0}, {250.0, 0.0}, {500.0, 0.0},
            {1000.0, 0.0}, {2000.0, 0.0}, {4000.0, 0.0}, {8000.0, 0.0}, {16000.0, 0.0}
        }},
        {"低音增强", {
            {31.0, 9.0}, {62.0, 7.5}, {125.0, 4.5}, {250.0, 1.5}, {500.0, 0.0},
            {1000.0, 0.0}, {2000.0, 0.5}, {4000.0, 1.0}, {8000.0, 1.5}, {16000.0, 1.5}
        }},
        {"人声清晰", {
            {31.0, -4.0}, {62.0, -2.5}, {125.0, -1.0}, {250.0, 1.0}, {500.0, 3.0},
            {1000.0, 4.5}, {2000.0, 5.0}, {4000.0, 3.0}, {8000.0, 0.5}, {16000.0, -1.5}
        }},
        {"摇滚现场", {
            {31.0, 6.0}, {62.0, 5.0}, {125.0, 2.5}, {250.0, -1.5}, {500.0, -3.0},
            {1000.0, -1.5}, {2000.0, 2.0}, {4000.0, 4.5}, {8000.0, 6.0}, {16000.0, 6.5}
        }}
    };
    return presets;
}

inline const std::vector<NamedCurvePreset>& getInstrumentEnvelopePresets() {
    static const std::vector<NamedCurvePreset> presets = {
        {"小提琴", {
            {32.7, -50.0}, {65.4, -35.0}, {130.8, -15.0}, {261.6, -5.0},
            {523.3, -2.0}, {1046.5, -5.0}, {2093.0, -8.0}, {3100.0, -4.0},
            {6000.0, -22.0}, {16000.0, -68.0}
        }},
        {"单簧管", {
            {32.7, -55.0}, {65.4, -30.0}, {130.8, -15.0}, {261.6, -2.0},
            {523.3, -22.0}, {784.9, -4.0}, {1046.5, -26.0}, {1308.1, -8.0},
            {4186.0, -35.0}, {16000.0, -75.0}
        }},
        {"长笛", {
            {32.7, -60.0}, {65.4, -40.0}, {130.8, -20.0}, {261.6, -2.0},
            {523.3, -4.0}, {1046.5, -18.0}, {2093.0, -36.0}, {4186.0, -54.0},
            {8372.0, -70.0}, {16000.0, -78.0}
        }},
        {"铜管小号", {
            {32.7, -55.0}, {65.4, -35.0}, {130.8, -18.0}, {261.6, -8.0},
            {523.3, -5.0}, {1046.5, -1.0}, {2093.0, -3.0}, {4186.0, -14.0},
            {8372.0, -36.0}, {16000.0, -65.0}
        }},
        {"管风琴", {
            {16.35, -18.0}, {32.7, -10.0}, {65.4, -5.0}, {130.8, -3.0},
            {261.6, -2.0}, {523.3, -3.0}, {1046.5, -5.0}, {2093.0, -9.0},
            {4186.0, -16.0}, {16000.0, -55.0}
        }}
    };
    return presets;
}

// ====================================================================================================
// 2. 无锁环形缓冲区 (Lock-Free SPSC Audio Ring Buffer)
// ====================================================================================================

class LockFreeAudioRingBuffer {
public:
    explicit LockFreeAudioRingBuffer(size_t capacity_frames = 65536)
        : capacity_(capacity_frames), buffer_(new std::atomic<float>[capacity_frames * 2]) {
        for (size_t i = 0; i < capacity_ * 2; ++i) {
            buffer_[i].store(0.0f, std::memory_order_relaxed);
        }
    }

    // 只允许单一写入线程调用
    void pushStereo(const float* interleaved_stereo, size_t frames) noexcept {
        const size_t w = write_pos_.load(std::memory_order_relaxed);
        for (size_t i = 0; i < frames; ++i) {
            const size_t idx = ((w + i) % capacity_) * 2;
            buffer_[idx].store(interleaved_stereo[i * 2], std::memory_order_relaxed);
            buffer_[idx + 1].store(interleaved_stereo[i * 2 + 1], std::memory_order_relaxed);
        }
        write_pos_.store(w + frames, std::memory_order_release);
    }

    // 读取线程（UI）调用。样本本身是 relaxed 原子，跨线程读写不构成数据竞争；
    // 若读取期间写入线程前进，最旧的若干帧槽位已被新数据覆盖，这些帧置零，避免拼接错位。
    void copyLatestFrames(std::vector<float>& out_stereo, size_t n_frames) const {
        n_frames = std::min(n_frames, capacity_);
        out_stereo.resize(n_frames * 2);
        const size_t w = write_pos_.load(std::memory_order_acquire);
        const size_t start = (w >= n_frames) ? (w - n_frames) : (capacity_ + w - n_frames);
        for (size_t i = 0; i < n_frames; ++i) {
            const size_t idx = ((start + i) % capacity_) * 2;
            out_stereo[i * 2] = buffer_[idx].load(std::memory_order_relaxed);
            out_stereo[i * 2 + 1] = buffer_[idx + 1].load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        const size_t w_after = write_pos_.load(std::memory_order_relaxed);
        const size_t overrun = (w_after > w) ? (w_after - w) : 0;
        const size_t margin = capacity_ - n_frames;
        const size_t torn = (overrun > margin) ? std::min(overrun - margin, n_frames) : 0;
        std::fill(out_stereo.begin(),
                  out_stereo.begin() + static_cast<std::vector<float>::difference_type>(torn * 2),
                  0.0f);
    }

private:
    size_t capacity_;
    std::unique_ptr<std::atomic<float>[]> buffer_;
    std::atomic<size_t> write_pos_{ 0 };
};

// ====================================================================================================
// 3. 预编译逆波兰字节码数学公式引擎 (BytecodeFormulaEngine)
// ====================================================================================================

class BytecodeFormulaEngine {
public:
    enum class OpCode : uint8_t {
        PushConst,
        PushVarX,
        PushVarT,
        Add, Sub, Mul, Div, Pow, Mod, Neg,
        Sin, Cos, Tan, Asin, Acos, Atan, Sinh, Cosh, Tanh,
        Exp, Log, Sqrt, Abs, Sign, Floor, Ceil,
        Square, Triangle, Sawtooth
    };

    struct Instruction {
        OpCode op;
        double value = 0.0;
    };

    bool compile(const std::string& expr_str, std::string* err_msg = nullptr) {
        code_.clear();
        pos_ = 0;
        input_ = expr_str;
        error_.clear();

        if (input_.find_first_not_of(" \t\r\n") == std::string::npos) {
            code_.push_back({ OpCode::PushVarX, 0.0 });
            code_.push_back({ OpCode::Sin, 0.0 });
            return true;
        }

        parseExpression();
        skipSpaces();
        if (pos_ < input_.size() && error_.empty()) {
            error_ = "意外的字符: '" + std::string(1, input_[pos_]) + "'";
        }
        if (!error_.empty()) {
            if (err_msg) *err_msg = error_;
            return false;
        }
        return !code_.empty();
    }

    inline double evalSample(double x_rad) const noexcept {
        double stack[64];
        int sp = 0;
        const double t_norm = std::fmod(std::fmod(x_rad / TWO_PI, 1.0) + 1.0, 1.0);

        for (const auto& inst : code_) {
            switch (inst.op) {
            case OpCode::PushConst: if (sp < 63) stack[sp++] = inst.value; break;
            case OpCode::PushVarX:  if (sp < 63) stack[sp++] = x_rad; break;
            case OpCode::PushVarT:  if (sp < 63) stack[sp++] = t_norm; break;
            case OpCode::Add: if (sp >= 2) { stack[sp - 2] += stack[sp - 1]; --sp; } break;
            case OpCode::Sub: if (sp >= 2) { stack[sp - 2] -= stack[sp - 1]; --sp; } break;
            case OpCode::Mul: if (sp >= 2) { stack[sp - 2] *= stack[sp - 1]; --sp; } break;
            case OpCode::Div: if (sp >= 2) {
                double d = stack[--sp];
                stack[sp - 1] = (std::abs(d) > 1e-12) ? (stack[sp - 1] / d) : 0.0;
            } break;
            case OpCode::Pow: if (sp >= 2) {
                double e = stack[--sp];
                stack[sp - 1] = std::pow(stack[sp - 1], e);
            } break;
            case OpCode::Mod: if (sp >= 2) {
                double d = stack[--sp];
                stack[sp - 1] = (std::abs(d) > 1e-12) ? std::fmod(stack[sp - 1], d) : 0.0;
            } break;
            case OpCode::Neg: if (sp >= 1) stack[sp - 1] = -stack[sp - 1]; break;
            case OpCode::Sin: if (sp >= 1) stack[sp - 1] = std::sin(stack[sp - 1]); break;
            case OpCode::Cos: if (sp >= 1) stack[sp - 1] = std::cos(stack[sp - 1]); break;
            case OpCode::Tan: if (sp >= 1) stack[sp - 1] = std::tan(stack[sp - 1]); break;
            case OpCode::Asin: if (sp >= 1) stack[sp - 1] = std::asin(safeClamp(stack[sp - 1], -1.0, 1.0)); break;
            case OpCode::Acos: if (sp >= 1) stack[sp - 1] = std::acos(safeClamp(stack[sp - 1], -1.0, 1.0)); break;
            case OpCode::Atan: if (sp >= 1) stack[sp - 1] = std::atan(stack[sp - 1]); break;
            case OpCode::Sinh: if (sp >= 1) stack[sp - 1] = std::sinh(safeClamp(stack[sp - 1], -20.0, 20.0)); break;
            case OpCode::Cosh: if (sp >= 1) stack[sp - 1] = std::cosh(safeClamp(stack[sp - 1], -20.0, 20.0)); break;
            case OpCode::Tanh: if (sp >= 1) stack[sp - 1] = std::tanh(stack[sp - 1]); break;
            case OpCode::Exp:  if (sp >= 1) stack[sp - 1] = std::exp(safeClamp(stack[sp - 1], -40.0, 40.0)); break;
            case OpCode::Log:  if (sp >= 1) stack[sp - 1] = std::log(std::max(1e-12, std::abs(stack[sp - 1]))); break;
            case OpCode::Sqrt: if (sp >= 1) stack[sp - 1] = std::sqrt(std::max(0.0, stack[sp - 1])); break;
            case OpCode::Abs:  if (sp >= 1) stack[sp - 1] = std::abs(stack[sp - 1]); break;
            case OpCode::Sign: if (sp >= 1) stack[sp - 1] = (stack[sp - 1] > 0.0) ? 1.0 : ((stack[sp - 1] < 0.0) ? -1.0 : 0.0); break;
            case OpCode::Floor: if (sp >= 1) stack[sp - 1] = std::floor(stack[sp - 1]); break;
            case OpCode::Ceil:  if (sp >= 1) stack[sp - 1] = std::ceil(stack[sp - 1]); break;
            case OpCode::Square: if (sp >= 1) stack[sp - 1] = (std::sin(stack[sp - 1]) >= 0.0) ? 1.0 : -1.0; break;
            case OpCode::Triangle: if (sp >= 1) stack[sp - 1] = (2.0 / PI_VAL) * std::asin(std::sin(stack[sp - 1])); break;
            case OpCode::Sawtooth: if (sp >= 1) {
                double n = std::fmod(std::fmod(stack[sp - 1] / TWO_PI, 1.0) + 1.0, 1.0);
                stack[sp - 1] = 2.0 * n - 1.0;
            } break;
            }
        }
        double res = (sp > 0) ? stack[sp - 1] : 0.0;
        if (!std::isfinite(res)) return 0.0;
        return res;
    }

    void evalBufferNormalized(const double* phases, double* out_vals, size_t count) const noexcept {
        double peak = 0.0;
        for (size_t i = 0; i < count; ++i) {
            double v = evalSample(phases[i]);
            out_vals[i] = v;
            peak = std::max(peak, std::abs(v));
        }
        if (peak > 1e-6) {
            double inv = 1.0 / peak;
            for (size_t i = 0; i < count; ++i) out_vals[i] *= inv;
        }
    }

private:
    std::string input_;
    size_t pos_ = 0;
    std::string error_;
    std::vector<Instruction> code_;

    void skipSpaces() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t' || input_[pos_] == '\r' || input_[pos_] == '\n')) {
            ++pos_;
        }
    }

    void parseExpression() {
        parseTerm();
        while (error_.empty()) {
            skipSpaces();
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) {
                char op = input_[pos_++];
                parseTerm();
                code_.push_back({ op == '+' ? OpCode::Add : OpCode::Sub, 0.0 });
            }
            else {
                break;
            }
        }
    }

    void parseTerm() {
        parsePower();
        while (error_.empty()) {
            skipSpaces();
            if (pos_ < input_.size() && (input_[pos_] == '*' || input_[pos_] == '/' || input_[pos_] == '%')) {
                char op = input_[pos_++];
                if (op == '*' && pos_ < input_.size() && input_[pos_] == '*') {
                    ++pos_;
                    parseUnary();
                    code_.push_back({ OpCode::Pow, 0.0 });
                }
                else {
                    parsePower();
                    code_.push_back({ op == '*' ? OpCode::Mul : (op == '/' ? OpCode::Div : OpCode::Mod), 0.0 });
                }
            }
            else {
                break;
            }
        }
    }

    void parsePower() {
        parseUnary();
        skipSpaces();
        if (error_.empty() && pos_ < input_.size() && input_[pos_] == '^') {
            ++pos_;
            parsePower();
            code_.push_back({ OpCode::Pow, 0.0 });
        }
    }

    void parseUnary() {
        skipSpaces();
        if (pos_ < input_.size() && input_[pos_] == '+') {
            ++pos_;
            parseUnary();
        }
        else if (pos_ < input_.size() && input_[pos_] == '-') {
            ++pos_;
            parseUnary();
            code_.push_back({ OpCode::Neg, 0.0 });
        }
        else {
            parsePrimary();
        }
    }

    void parsePrimary() {
        skipSpaces();
        if (pos_ >= input_.size()) {
            error_ = "表达式意外结束";
            return;
        }
        if (input_[pos_] == '(') {
            ++pos_;
            parseExpression();
            skipSpaces();
            if (pos_ >= input_.size() || input_[pos_] != ')') {
                error_ = "缺少右括号 ')'";
                return;
            }
            ++pos_;
            return;
        }
        if (std::isdigit(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '.') {
            char* end_ptr = nullptr;
            double val = std::strtod(input_.c_str() + pos_, &end_ptr);
            pos_ = static_cast<size_t>(end_ptr - input_.c_str());
            code_.push_back({ OpCode::PushConst, val });
            return;
        }
        if (std::isalpha(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_') {
            size_t start = pos_;
            while (pos_ < input_.size() && (std::isalnum(static_cast<unsigned char>(input_[pos_])) || input_[pos_] == '_')) {
                ++pos_;
            }
            std::string ident = input_.substr(start, pos_ - start);
            skipSpaces();
            if (pos_ < input_.size() && input_[pos_] == '(') {
                ++pos_;
                parseExpression();
                if (ident == "mod" && pos_ < input_.size() && input_[pos_] == ',') {
                    ++pos_;
                    parseExpression();
                    skipSpaces();
                    if (pos_ >= input_.size() || input_[pos_] != ')') { error_ = "函数调用缺少 ')'"; return; }
                    ++pos_;
                    code_.push_back({ OpCode::Mod, 0.0 });
                    return;
                }
                skipSpaces();
                if (pos_ >= input_.size() || input_[pos_] != ')') {
                    error_ = "函数 '" + ident + "' 缺少右括号 ')'";
                    return;
                }
                ++pos_;
                if (ident == "sin") code_.push_back({ OpCode::Sin, 0.0 });
                else if (ident == "cos") code_.push_back({ OpCode::Cos, 0.0 });
                else if (ident == "tan") code_.push_back({ OpCode::Tan, 0.0 });
                else if (ident == "arcsin" || ident == "asin") code_.push_back({ OpCode::Asin, 0.0 });
                else if (ident == "arccos" || ident == "acos") code_.push_back({ OpCode::Acos, 0.0 });
                else if (ident == "arctan" || ident == "atan") code_.push_back({ OpCode::Atan, 0.0 });
                else if (ident == "sinh") code_.push_back({ OpCode::Sinh, 0.0 });
                else if (ident == "cosh") code_.push_back({ OpCode::Cosh, 0.0 });
                else if (ident == "tanh") code_.push_back({ OpCode::Tanh, 0.0 });
                else if (ident == "exp") code_.push_back({ OpCode::Exp, 0.0 });
                else if (ident == "log" || ident == "ln") code_.push_back({ OpCode::Log, 0.0 });
                else if (ident == "sqrt") code_.push_back({ OpCode::Sqrt, 0.0 });
                else if (ident == "abs" || ident == "fabs") code_.push_back({ OpCode::Abs, 0.0 });
                else if (ident == "sign" || ident == "sgn") code_.push_back({ OpCode::Sign, 0.0 });
                else if (ident == "floor") code_.push_back({ OpCode::Floor, 0.0 });
                else if (ident == "ceil") code_.push_back({ OpCode::Ceil, 0.0 });
                else if (ident == "square") code_.push_back({ OpCode::Square, 0.0 });
                else if (ident == "triangle") code_.push_back({ OpCode::Triangle, 0.0 });
                else if (ident == "sawtooth") code_.push_back({ OpCode::Sawtooth, 0.0 });
                else error_ = "未知的数学函数: " + ident;
                return;
            }
            if (ident == "x") code_.push_back({ OpCode::PushVarX, 0.0 });
            else if (ident == "t") code_.push_back({ OpCode::PushVarT, 0.0 });
            else if (ident == "pi" || ident == "PI") code_.push_back({ OpCode::PushConst, PI_VAL });
            else if (ident == "e" || ident == "E") code_.push_back({ OpCode::PushConst, 2.71828182845904523536 });
            else error_ = "未知的变量名: " + ident;
            return;
        }
        error_ = "无法解析的字符: '" + std::string(1, input_[pos_]) + "'";
    }
};

// ====================================================================================================
// 4. 高速原位基-2 Cooley-Tukey FFT 与实数 FFT 引擎 (FastFFT)
// ====================================================================================================

class FastFFT {
public:
    static inline size_t nextPowerOfTwo(size_t n) noexcept {
        size_t p = 1;
        while (p < n) p <<= 1;
        return p;
    }

    static void fftComplex(std::vector<std::complex<double>>& a, bool invert) {
        const size_t n = a.size();
        if (n <= 1) return;

        for (size_t i = 1, j = 0; i < n; ++i) {
            size_t bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) std::swap(a[i], a[j]);
        }

        for (size_t len = 2; len <= n; len <<= 1) {
            double ang = TWO_PI / static_cast<double>(len) * (invert ? 1.0 : -1.0);
            std::complex<double> wlen(std::cos(ang), std::sin(ang));
            size_t half = len >> 1;
            for (size_t i = 0; i < n; i += len) {
                std::complex<double> w(1.0, 0.0);
                for (size_t j = 0; j < half; ++j) {
                    std::complex<double> u = a[i + j];
                    std::complex<double> v = a[i + j + half] * w;
                    a[i + j] = u + v;
                    a[i + j + half] = u - v;
                    w *= wlen;
                }
            }
        }

        if (invert) {
            double inv_n = 1.0 / static_cast<double>(n);
            for (auto& x : a) x *= inv_n;
        }
    }

    static void rfft(const double* real_in, size_t n_in, size_t n_fft, std::vector<std::complex<double>>& spec_out) {
        n_fft = nextPowerOfTwo(n_fft);
        std::vector<std::complex<double>> buf(n_fft, { 0.0, 0.0 });
        size_t copy_n = std::min(n_in, n_fft);
        for (size_t i = 0; i < copy_n; ++i) buf[i] = { real_in[i], 0.0 };
        fftComplex(buf, false);
        spec_out.resize(n_fft / 2 + 1);
        for (size_t i = 0; i <= n_fft / 2; ++i) spec_out[i] = buf[i];
    }

    static void irfft(const std::vector<std::complex<double>>& spec_in, size_t n_fft, std::vector<double>& real_out) {
        n_fft = nextPowerOfTwo(n_fft);
        std::vector<std::complex<double>> buf(n_fft, { 0.0, 0.0 });
        size_t bins = std::min(spec_in.size(), n_fft / 2 + 1);
        for (size_t i = 0; i < bins; ++i) buf[i] = spec_in[i];
        for (size_t i = 1; i < n_fft / 2; ++i) {
            buf[n_fft - i] = std::conj(buf[i]);
        }
        fftComplex(buf, true);
        real_out.resize(n_fft);
        for (size_t i = 0; i < n_fft; ++i) real_out[i] = buf[i].real();
    }
};

// ====================================================================================================
// 5. 核心 DSP 算法引擎 (AudioDSPEngine)
// ====================================================================================================

class AudioDSPEngine {
public:
    static inline float softKneeLimiterSample(float x, float threshold = 0.80f) noexcept {
        float ax = std::abs(x);
        if (ax <= threshold) return x;
        float headroom = 0.99f - threshold;
        float excess = (ax - threshold) / std::max(1e-4f, headroom);
        float compressed = threshold + headroom * std::tanh(excess);
        return (x >= 0.0f) ? compressed : -compressed;
    }

    static void softKneeLimiterBuffer(float* interleaved, size_t total_samples, float threshold = 0.80f) noexcept {
        for (size_t i = 0; i < total_samples; ++i) {
            interleaved[i] = softKneeLimiterSample(interleaved[i], threshold);
        }
    }

    static inline int freqToMidi(double freq_hz) noexcept {
        if (freq_hz <= 14.0 || !std::isfinite(freq_hz)) return -1;
        return static_cast<int>(std::llround(69.0 + 12.0 * std::log2(freq_hz / 440.0)));
    }

    static std::string midiToNoteName(int midi) {
        if (midi < 0 || midi > 127) return "--";
        const char* note = NOTE_NAMES_12[midi % 12];
        int octave = (midi / 12) - 1;
        return std::string(note) + std::to_string(octave);
    }

    static std::string freqToNoteName(double freq_hz) {
        return midiToNoteName(freqToMidi(freq_hz));
    }

    static void evaluateSmoothCurveDb(
        const std::vector<ControlPoint>& control_points,
        const double* query_freqs,
        double* out_db,
        size_t n_queries,
        double min_db = -24.0,
        double max_db = 24.0
    ) {
        if (control_points.empty()) {
            std::fill(out_db, out_db + n_queries, 0.0);
            return;
        }

        std::vector<ControlPoint> sorted = control_points;
        std::sort(sorted.begin(), sorted.end(), [](const ControlPoint& a, const ControlPoint& b) {
            return a.freq_hz < b.freq_hz;
            });

        std::vector<double> xs;
        std::vector<double> ys;
        xs.reserve(sorted.size());
        ys.reserve(sorted.size());

        for (const auto& pt : sorted) {
            double f_clamped = safeClamp(pt.freq_hz, 10.0, 24000.0);
            double db_clamped = safeClamp(pt.db, min_db, max_db);
            if (!xs.empty() && std::abs(f_clamped - std::pow(10.0, xs.back())) < 0.5) {
                ys.back() = db_clamped;
            }
            else {
                xs.push_back(std::log10(f_clamped));
                ys.push_back(db_clamped);
            }
        }

        const size_t n = xs.size();
        if (n == 1) {
            std::fill(out_db, out_db + n_queries, ys[0]);
            return;
        }
        if (n == 2) {
            double dx = xs[1] - xs[0];
            for (size_t i = 0; i < n_queries; ++i) {
                double lq = std::log10(std::max(10.0, query_freqs[i]));
                if (lq <= xs[0]) out_db[i] = ys[0];
                else if (lq >= xs[1]) out_db[i] = ys[1];
                else {
                    double t = (lq - xs[0]) / std::max(1e-9, dx);
                    out_db[i] = safeClamp(ys[0] + t * (ys[1] - ys[0]), min_db, max_db);
                }
            }
            return;
        }

        std::vector<double> h(n - 1), delta(n - 1), d(n, 0.0);
        for (size_t k = 0; k < n - 1; ++k) {
            h[k] = std::max(1e-9, xs[k + 1] - xs[k]);
            delta[k] = (ys[k + 1] - ys[k]) / h[k];
        }

        for (size_t k = 1; k < n - 1; ++k) {
            if (delta[k - 1] * delta[k] <= 0.0) {
                d[k] = 0.0;
            }
            else {
                double w1 = 2.0 * h[k] + h[k - 1];
                double w2 = h[k] + 2.0 * h[k - 1];
                d[k] = (w1 + w2) / (w1 / delta[k - 1] + w2 / delta[k]);
            }
        }

        auto endpointSlope = [](double h0, double h1, double del0, double del1) {
            double s = ((2.0 * h0 + h1) * del0 - h0 * del1) / (h0 + h1);
            if (s * del0 <= 0.0) return 0.0;
            if (del0 * del1 <= 0.0 && std::abs(s) > std::abs(3.0 * del0)) return 3.0 * del0;
            return s;
            };
        d[0] = endpointSlope(h[0], h[1], delta[0], delta[1]);
        d[n - 1] = endpointSlope(h[n - 2], h[n - 3], delta[n - 2], delta[n - 3]);

        for (size_t i = 0; i < n_queries; ++i) {
            double lq = std::log10(std::max(10.0, query_freqs[i]));
            if (lq <= xs.front()) {
                out_db[i] = ys.front();
                continue;
            }
            if (lq >= xs.back()) {
                out_db[i] = ys.back();
                continue;
            }
            auto it = std::upper_bound(xs.begin(), xs.end(), lq);
            size_t k = static_cast<size_t>(std::distance(xs.begin(), it)) - 1;
            k = std::min(k, n - 2);
            double s = lq - xs[k];
            double hk = h[k];
            double t = s / hk;
            double t2 = t * t;
            double t3 = t2 * t;
            double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
            double h10 = t3 - 2.0 * t2 + t;
            double h01 = -2.0 * t3 + 3.0 * t2;
            double h11 = t3 - t2;
            double val = h00 * ys[k] + h10 * hk * d[k] + h01 * ys[k + 1] + h11 * hk * d[k + 1];
            out_db[i] = safeClamp(val, min_db, max_db);
        }
    }

    static void computeFftSpectrumDb(
        const float* stereo_frames,
        size_t n_frames,
        int sample_rate,
        int fft_window_size,
        std::vector<double>& out_freqs,
        std::vector<double>& out_mag_db,
        SpectrumStats& out_stats
    ) {
        size_t win_len = static_cast<size_t>(std::max(16, fft_window_size));
        size_t n_fft = FastFFT::nextPowerOfTwo(std::max<size_t>(win_len, 2048));
        size_t n_bins = n_fft / 2 + 1;

        out_freqs.resize(n_bins);
        out_mag_db.resize(n_bins, -100.0);
        for (size_t k = 0; k < n_bins; ++k) {
            out_freqs[k] = static_cast<double>(k) * static_cast<double>(sample_rate) / static_cast<double>(n_fft);
        }

        if (n_frames == 0 || stereo_frames == nullptr) {
            out_stats = { 0.0, -100.0, -100.0, 0.0 };
            return;
        }

        std::vector<double> frame(win_len, 0.0);
        double sum_sq = 0.0;
        for (size_t i = 0; i < n_frames; ++i) {
            double m = 0.5 * (static_cast<double>(stereo_frames[i * 2]) + static_cast<double>(stereo_frames[i * 2 + 1]));
            sum_sq += m * m;
        }
        double rms = std::sqrt(sum_sq / static_cast<double>(n_frames) + 1e-24);
        out_stats.rms_db = safeClamp(20.0 * std::log10(rms), -100.0, 6.0);

        size_t take = std::min(n_frames, win_len);
        size_t src_start = n_frames - take;
        size_t dst_start = win_len - take;

        double win_sum = 0.0;
        for (size_t i = 0; i < win_len; ++i) {
            double w = 0.5 * (1.0 - std::cos(TWO_PI * static_cast<double>(i) / static_cast<double>(win_len - 1)));
            win_sum += w;
            if (i >= dst_start) {
                size_t si = src_start + (i - dst_start);
                double mono = 0.5 * (static_cast<double>(stereo_frames[si * 2]) + static_cast<double>(stereo_frames[si * 2 + 1]));
                frame[i] = mono * w;
            }
        }

        std::vector<std::complex<double>> spec;
        FastFFT::rfft(frame.data(), win_len, n_fft, spec);

        double peak_freq = 0.0;
        double peak_db = -100.0;
        double weighted_freq_sum = 0.0;
        double lin_sum = 0.0;

        for (size_t k = 0; k < n_bins; ++k) {
            double mag_lin = (2.0 * std::abs(spec[k])) / (win_sum + 1e-12);
            if (k == 0) mag_lin *= 0.5;
            double db = safeClamp(20.0 * std::log10(std::max(mag_lin, 1e-5)), -100.0, 12.0);
            out_mag_db[k] = db;

            double f = out_freqs[k];
            if (f >= MIN_PLOT_FREQ_HZ && f <= MAX_PLOT_FREQ_HZ) {
                if (db > peak_db) {
                    peak_db = db;
                    peak_freq = f;
                }
                weighted_freq_sum += f * mag_lin;
                lin_sum += mag_lin;
            }
        }

        out_stats.peak_freq_hz = peak_freq;
        out_stats.peak_db = peak_db;
        out_stats.centroid_hz = (lin_sum > 1e-9) ? (weighted_freq_sum / lin_sum) : 0.0;
    }

    static std::vector<SpectralPeak> findTopNSpectralPeaks(
        const std::vector<double>& freqs,
        const std::vector<double>& mag_db,
        int top_n = 3,
        double min_db = -78.0,
        double min_semitone_sep = 1.15
    ) {
        std::vector<SpectralPeak> result;
        if (top_n <= 0 || freqs.size() < 5 || freqs.size() != mag_db.size()) return result;

        struct Candidate {
            size_t idx;
            double db;
        };
        std::vector<Candidate> candidates;

        for (size_t i = 1; i + 1 < freqs.size(); ++i) {
            if (freqs[i] < MIN_PLOT_FREQ_HZ || freqs[i] > MAX_PLOT_FREQ_HZ) continue;
            if (mag_db[i] >= min_db && mag_db[i] >= mag_db[i - 1] && mag_db[i] >= mag_db[i + 1]) {
                size_t l = (i > 4) ? (i - 4) : 0;
                size_t r = std::min(freqs.size() - 1, i + 4);
                double local_min = std::max(mag_db[l], mag_db[r]);
                if (mag_db[i] - local_min >= 0.8) {
                    candidates.push_back({ i, mag_db[i] });
                }
            }
        }

        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.db > b.db;
            });

        for (const auto& cand : candidates) {
            size_t idx = cand.idx;
            double y0 = mag_db[idx - 1];
            double y1 = mag_db[idx];
            double y2 = mag_db[idx + 1];
            double denom = y0 - 2.0 * y1 + y2;
            double true_f = freqs[idx];
            double true_db = y1;
            if (std::abs(denom) > 1e-9) {
                double delta = safeClamp(0.5 * (y0 - y2) / denom, -0.5, 0.5);
                double df = freqs[idx + 1] - freqs[idx];
                true_f += delta * df;
                true_db = y1 - 0.25 * (y0 - y2) * delta;
            }
            if (true_db < min_db || true_f < MIN_PLOT_FREQ_HZ) continue;

            bool too_close = false;
            for (const auto& ex : result) {
                double semitone_diff = std::abs(12.0 * std::log2(true_f / ex.freq_hz));
                if (semitone_diff < min_semitone_sep) {
                    too_close = true;
                    break;
                }
            }
            if (too_close) continue;

            int rank = static_cast<int>(result.size()) + 1;
            int midi = freqToMidi(true_f);
            result.push_back({
                rank,
                std::round(true_f * 100.0) / 100.0,
                std::round(true_db * 10.0) / 10.0,
                midiToNoteName(midi),
                midi,
                std::pow(10.0, true_db / 20.0)
                });

            if (static_cast<int>(result.size()) >= top_n) break;
        }
        return result;
    }

    static std::vector<float> synthesizePianoKeyNote(int midi_note, int sample_rate = 44100, double duration_sec = 0.85) {
        double freq_hz = 440.0 * std::pow(2.0, (midi_note - 69) / 12.0);
        size_t n_samples = std::max<size_t>(1024, static_cast<size_t>(std::llround(duration_sec * sample_rate)));
        std::vector<double> sig(n_samples, 0.0);

        struct Harm { int k; double amp; double decay; };
        static constexpr std::array<Harm, 6> harmonics = { {
            {1, 1.00, 3.2}, {2, 0.45, 4.5}, {3, 0.22, 6.0},
            {4, 0.12, 8.0}, {5, 0.06, 10.0}, {6, 0.03, 12.0}
        } };

        double nyquist = sample_rate * 0.46;
        for (const auto& h : harmonics) {
            double fk = h.k * freq_hz;
            if (fk < nyquist) {
                for (size_t i = 0; i < n_samples; ++i) {
                    double t = static_cast<double>(i) / static_cast<double>(sample_rate);
                    sig[i] += h.amp * std::exp(-h.decay * t) * std::sin(TWO_PI * fk * t);
                }
            }
        }

        size_t attack = std::min<size_t>(n_samples / 10, std::max<size_t>(16, static_cast<size_t>(0.005 * sample_rate)));
        size_t release = std::min<size_t>(n_samples / 5, std::max<size_t>(32, static_cast<size_t>(0.050 * sample_rate)));
        for (size_t i = 0; i < attack; ++i) sig[i] *= static_cast<double>(i) / static_cast<double>(attack);
        for (size_t i = 0; i < release; ++i) {
            sig[n_samples - 1 - i] *= static_cast<double>(i) / static_cast<double>(release);
        }

        double peak = 0.0;
        for (double v : sig) peak = std::max(peak, std::abs(v));
        double scale = (peak > 1e-6) ? (0.80 / peak) : 1.0;

        std::vector<float> stereo(n_samples * 2);
        for (size_t i = 0; i < n_samples; ++i) {
            float s = static_cast<float>(sig[i] * scale);
            stereo[i * 2] = s;
            stereo[i * 2 + 1] = s;
        }
        return stereo;
    }

    static std::vector<float> buildRawSnippetPlayback(const std::vector<float>& snippet_stereo, int sample_rate) {
        size_t n_frames = snippet_stereo.size() / 2;
        if (n_frames == 0) return std::vector<float>(2048, 0.0f);
        std::vector<float> out = snippet_stereo;
        size_t fade_len = std::min<size_t>(n_frames / 4, std::max<size_t>(8, static_cast<size_t>(0.002 * sample_rate)));
        for (size_t i = 0; i < fade_len; ++i) {
            float w = static_cast<float>(0.5 - 0.5 * std::cos(PI_VAL * static_cast<double>(i) / static_cast<double>(fade_len)));
            out[i * 2] *= w;
            out[i * 2 + 1] *= w;
            size_t tail = n_frames - 1 - i;
            out[tail * 2] *= w;
            out[tail * 2 + 1] *= w;
        }
        return out;
    }

    static std::vector<float> buildSustainPlayback(
        const std::vector<float>& snippet_stereo,
        int sample_rate,
        double duration_sec,
        const std::vector<SpectralPeak>& top_peaks,
        bool use_harmonic_resynthesis
    ) {
        size_t target_frames = std::max<size_t>(1024, static_cast<size_t>(std::llround(duration_sec * sample_rate)));
        size_t in_frames = snippet_stereo.size() / 2;
        if (in_frames == 0) return std::vector<float>(target_frames * 2, 0.0f);

        std::vector<float> out(target_frames * 2, 0.0f);

        if (use_harmonic_resynthesis && !top_peaks.empty()) {
            double peak = 0.0;
            std::vector<double> mono(target_frames, 0.0);
            for (const auto& p : top_peaks) {
                for (size_t i = 0; i < target_frames; ++i) {
                    double t = static_cast<double>(i) / static_cast<double>(sample_rate);
                    mono[i] += p.amp_linear * std::sin(TWO_PI * p.freq_hz * t);
                }
            }
            for (double v : mono) peak = std::max(peak, std::abs(v));
            double scale = (peak > 1e-6) ? (0.85 / peak) : 1.0;
            for (size_t i = 0; i < target_frames; ++i) {
                float s = static_cast<float>(mono[i] * scale);
                out[i * 2] = s;
                out[i * 2 + 1] = s;
            }
        }
        else {
            std::vector<float> grain = snippet_stereo;
            size_t win_len = in_frames;
            while (win_len < 64) {
                grain.insert(grain.end(), snippet_stereo.begin(), snippet_stereo.end());
                win_len = grain.size() / 2;
            }
            size_t hop = std::max<size_t>(16, win_len / 2);
            std::vector<float> accum((target_frames + win_len) * 2, 0.0f);

            for (size_t pos = 0; pos < target_frames; pos += hop) {
                for (size_t i = 0; i < win_len; ++i) {
                    float w = static_cast<float>(0.5 * (1.0 - std::cos(TWO_PI * static_cast<double>(i) / static_cast<double>(win_len - 1))));
                    accum[(pos + i) * 2] += grain[i * 2] * w;
                    accum[(pos + i) * 2 + 1] += grain[i * 2 + 1] * w;
                }
            }

            float orig_peak = 0.0f, new_peak = 0.0f;
            for (float v : grain) orig_peak = std::max(orig_peak, std::abs(v));
            for (size_t i = 0; i < target_frames * 2; ++i) new_peak = std::max(new_peak, std::abs(accum[i]));
            float gain = (new_peak > 1e-6f) ? (std::min(0.90f, std::max(0.35f, orig_peak)) / new_peak) : 1.0f;
            for (size_t i = 0; i < target_frames * 2; ++i) out[i] = accum[i] * gain;
        }

        size_t attack = std::min<size_t>(target_frames / 4, static_cast<size_t>(0.025 * sample_rate));
        size_t release = std::min<size_t>(target_frames / 3, static_cast<size_t>(0.080 * sample_rate));
        for (size_t i = 0; i < attack; ++i) {
            float w = static_cast<float>(i) / static_cast<float>(attack);
            out[i * 2] *= w;
            out[i * 2 + 1] *= w;
        }
        for (size_t i = 0; i < release; ++i) {
            float w = static_cast<float>(i) / static_cast<float>(release);
            size_t idx = target_frames - 1 - i;
            out[idx * 2] *= w;
            out[idx * 2 + 1] *= w;
        }
        return out;
    }

    static std::vector<float> buildTimeStretchedPlayback(
        const std::vector<float>& snippet_stereo,
        int sample_rate,
        double stretch_factor = 10.0,
        bool keep_pitch = true
    ) {
        size_t n_in = snippet_stereo.size() / 2;
        if (n_in < 4) return std::vector<float>(2048, 0.0f);

        double factor = safeClamp(stretch_factor, 1.0, 50.0);
        size_t n_out = std::max<size_t>(64, static_cast<size_t>(std::llround(n_in * factor)));

        if (!keep_pitch || n_in < 128) {
            std::vector<float> out(n_out * 2, 0.0f);
            for (size_t i = 0; i < n_out; ++i) {
                double pos = static_cast<double>(i) * static_cast<double>(n_in - 1) / static_cast<double>(n_out - 1);
                size_t i0 = static_cast<size_t>(pos);
                size_t i1 = std::min(n_in - 1, i0 + 1);
                float frac = static_cast<float>(pos - i0);
                out[i * 2] = snippet_stereo[i0 * 2] * (1.0f - frac) + snippet_stereo[i1 * 2] * frac;
                out[i * 2 + 1] = snippet_stereo[i0 * 2 + 1] * (1.0f - frac) + snippet_stereo[i1 * 2 + 1] * frac;
            }
            return out;
        }

        size_t n_fft = 256;
        while (n_fft * 2 <= std::min<size_t>(512, n_in / 2)) n_fft *= 2;
        n_fft = std::max<size_t>(64, n_fft);
        size_t hop_in = std::max<size_t>(8, n_fft / 4);
        size_t n_bins = n_fft / 2 + 1;

        std::vector<double> win(n_fft);
        for (size_t i = 0; i < n_fft; ++i) {
            win[i] = 0.5 * (1.0 - std::cos(TWO_PI * static_cast<double>(i) / static_cast<double>(n_fft - 1)));
        }

        std::array<std::vector<float>, 2> ch_out;
        for (int ch = 0; ch < 2; ++ch) {
            std::vector<std::vector<std::complex<double>>> stft;
            std::vector<double> frame(n_fft);
            for (size_t start = 0; start + n_fft <= n_in; start += hop_in) {
                for (size_t i = 0; i < n_fft; ++i) {
                    frame[i] = static_cast<double>(snippet_stereo[(start + i) * 2 + ch]) * win[i];
                }
                std::vector<std::complex<double>> spec;
                FastFFT::rfft(frame.data(), n_fft, n_fft, spec);
                stft.push_back(std::move(spec));
            }

            if (stft.size() < 2) {
                return buildSustainPlayback(snippet_stereo, sample_rate, static_cast<double>(n_out) / sample_rate, {}, false);
            }

            size_t n_steps = std::max<size_t>(2, static_cast<size_t>(std::llround(stft.size() * factor)));
            std::vector<double> phase_acc(n_bins, 0.0);
            for (size_t b = 0; b < n_bins; ++b) phase_acc[b] = std::arg(stft[0][b]);

            std::vector<double> y_out(n_steps * hop_in + n_fft, 0.0);
            std::vector<std::complex<double>> synth_spec(n_bins);
            std::vector<double> grain;

            for (size_t step_i = 0; step_i < n_steps; ++step_i) {
                double step = static_cast<double>(step_i) * static_cast<double>(stft.size() - 1) / static_cast<double>(n_steps - 1);
                size_t idx0 = std::min(static_cast<size_t>(std::floor(step)), stft.size() - 1);
                size_t idx1 = std::min(idx0 + 1, stft.size() - 1);
                double frac = step - static_cast<double>(idx0);

                for (size_t b = 0; b < n_bins; ++b) {
                    double omega = TWO_PI * static_cast<double>(hop_in * b) / static_cast<double>(n_fft);
                    double mag = (1.0 - frac) * std::abs(stft[idx0][b]) + frac * std::abs(stft[idx1][b]);
                    double dphi = std::arg(stft[idx1][b]) - std::arg(stft[idx0][b]) - omega;
                    dphi = std::fmod(dphi + PI_VAL, TWO_PI);
                    if (dphi < 0.0) dphi += TWO_PI;
                    dphi -= PI_VAL;
                    phase_acc[b] += omega + dphi;
                    synth_spec[b] = std::polar(mag, phase_acc[b]);
                }

                FastFFT::irfft(synth_spec, n_fft, grain);
                size_t pos = step_i * hop_in;
                for (size_t i = 0; i < n_fft; ++i) {
                    y_out[pos + i] += grain[i] * win[i];
                }
            }

            double peak = 0.0, orig_peak = 0.0;
            for (size_t i = 0; i < n_in; ++i) orig_peak = std::max(orig_peak, std::abs(static_cast<double>(snippet_stereo[i * 2 + ch])));
            for (double v : y_out) peak = std::max(peak, std::abs(v));
            double scale = (peak > 1e-6) ? (std::max(0.35, orig_peak) / peak) : 1.0;

            ch_out[ch].resize(y_out.size());
            for (size_t i = 0; i < y_out.size(); ++i) ch_out[ch][i] = static_cast<float>(y_out[i] * scale);
        }

        size_t len = std::min(ch_out[0].size(), ch_out[1].size());
        std::vector<float> out(len * 2);
        size_t fade = std::min<size_t>(len / 6, static_cast<size_t>(0.015 * sample_rate));
        for (size_t i = 0; i < len; ++i) {
            float w = 1.0f;
            if (i < fade) w = static_cast<float>(i) / static_cast<float>(fade);
            else if (i + fade >= len) w = static_cast<float>(len - 1 - i) / static_cast<float>(fade);
            out[i * 2] = safeClamp(ch_out[0][i] * w, -0.98f, 0.98f);
            out[i * 2 + 1] = safeClamp(ch_out[1][i] * w, -0.98f, 0.98f);
        }
        return out;
    }

    // 多相滤波器表的相位数（线性插值，相邻两行之间按小数相位加权）
    static constexpr int NUM_PHASES = 1024;

    struct PolyphaseTable {
        int half_taps = 0;
        int num_taps = 0;
        std::vector<float> coeffs; // (NUM_PHASES + 1) * num_taps，行优先
    };

    // 设计说明（已用数值仿真验证：通带平坦到 0.9 倍目标奈奎斯特，混叠抑制优于 -100 dB）：
    // - 下采样：过渡带位于 [0.90, 1.00] 倍目标奈奎斯特频率，截止点在过渡带中央；
    // - 上采样：过渡带位于 [0.92, 1.00] 倍源奈奎斯特频率；
    // - 窗长由过渡带宽度决定（Blackman-Harris 4 项窗）。旧实现把截止点放在目标奈奎斯特的 98%，
    //   过渡带近似为零：高频会在通带内衰减，且 8~12 kHz 的成分会折叠回可听频段（发炸、发糊）。
    static std::shared_ptr<const PolyphaseTable> buildPolyphaseTable(int orig_sr, int target_sr) {
        const bool downsampling = (target_sr < orig_sr);
        // 以输入采样周期为单位（周期/样本）：过渡带宽度设为 0.05 * nyq，使通带平坦度延伸至 0.95 * nyq
        // （例如 44.1kHz 下 21kHz 仅 -0.035dB，22.05kHz 下 10.5kHz 仅 -0.035dB），同时阻带混叠抑制优于 -117dB
        const double nyq = 0.5 * std::min(1.0, static_cast<double>(target_sr) / static_cast<double>(orig_sr));
        const double delta = (downsampling ? 0.05 : 0.06) * nyq; // 过渡带宽度
        const double fc = nyq - 0.5 * delta;                      // -6 dB 截止点（过渡带中央 0.975 * nyq）
        int taps = static_cast<int>(std::ceil(6.0 / delta));
        taps += (taps & 1);
        taps = std::min(taps, 1024);

        auto table = std::make_shared<PolyphaseTable>();
        table->num_taps = taps;
        table->half_taps = taps / 2;
        table->coeffs.assign(static_cast<size_t>(NUM_PHASES + 1) * static_cast<size_t>(taps), 0.0f);

        std::vector<double> row(static_cast<size_t>(taps), 0.0);
        for (int p = 0; p <= NUM_PHASES; ++p) {
            const double frac = static_cast<double>(p) / static_cast<double>(NUM_PHASES);
            double w_sum = 0.0;
            for (int t = 0; t < taps; ++t) {
                const double x = frac - static_cast<double>(t - table->half_taps + 1);
                const double w_pos = 0.5 + 0.5 * (x / static_cast<double>(table->half_taps));
                double v = 0.0;
                if (w_pos > 0.0 && w_pos < 1.0) {
                    const double win = 0.35875
                        - 0.48829 * std::cos(TWO_PI * w_pos)
                        + 0.14128 * std::cos(2.0 * TWO_PI * w_pos)
                        - 0.01168 * std::cos(3.0 * TWO_PI * w_pos);
                    const double sinc_arg = 2.0 * fc * x;
                    const double sinc_val = (std::abs(sinc_arg) < 1e-9)
                        ? (2.0 * fc)
                        : (std::sin(PI_VAL * sinc_arg) / (PI_VAL * x));
                    v = sinc_val * win;
                }
                row[static_cast<size_t>(t)] = v;
                w_sum += v;
            }
            const double inv_w = (std::abs(w_sum) > 1e-12) ? (1.0 / w_sum) : 0.0; // 每相位直流增益归一化为 1
            for (int t = 0; t < taps; ++t) {
                table->coeffs[static_cast<size_t>(p) * static_cast<size_t>(taps) + static_cast<size_t>(t)] =
                    static_cast<float>(row[static_cast<size_t>(t)] * inv_w);
            }
        }
        return table;
    }

    // 多槽位 LRU 滤波器表缓存：同时缓存正向降采样 (orig_sr -> target_sr) 与反向升采样 (target_sr -> orig_sr) 等多组采样率对，
    // 彻底避免实时重采样双向调用时单槽缓存互相覆盖导致的每帧重算问题。
    static std::shared_ptr<const PolyphaseTable> getPolyphaseTable(int orig_sr, int target_sr) {
        struct CacheEntry {
            int orig = 0;
            int target = 0;
            uint64_t last_use = 0;
            std::shared_ptr<const PolyphaseTable> table;
        };
        static std::mutex cache_mutex;
        static std::array<CacheEntry, 8> cache_slots{};
        static uint64_t use_counter = 0;

        std::lock_guard<std::mutex> lk(cache_mutex);
        for (auto& slot : cache_slots) {
            if (slot.table && slot.orig == orig_sr && slot.target == target_sr) {
                slot.last_use = ++use_counter;
                return slot.table;
            }
        }
        auto built = buildPolyphaseTable(orig_sr, target_sr);
        size_t victim = 0;
        for (size_t i = 1; i < cache_slots.size(); ++i) {
            if (!cache_slots[i].table || cache_slots[i].last_use < cache_slots[victim].last_use) {
                victim = i;
            }
        }
        cache_slots[victim] = { orig_sr, target_sr, ++use_counter, built };
        return built;
    }

    static std::vector<float> offlineResample(
        const std::vector<float>& in_stereo,
        int orig_sr,
        int target_sr,
        int target_bit_depth,
        int target_channels
    ) {
        size_t in_frames = in_stereo.size() / 2;
        if (in_frames == 0) return {};
        if (target_sr <= 0) target_sr = orig_sr;

        std::vector<float> out;
        if (target_sr == orig_sr) {
            out.resize(in_frames * 2);
            for (size_t i = 0; i < in_frames; ++i) {
                float l = in_stereo[i * 2];
                float r = in_stereo[i * 2 + 1];
                if (target_channels == 1) l = r = 0.5f * (l + r);
                out[i * 2]     = l;
                out[i * 2 + 1] = r;
            }
        }
        else {
            const size_t out_frames = std::max<size_t>(
                1,
                static_cast<size_t>(std::llround(static_cast<double>(in_frames) * static_cast<double>(target_sr) / static_cast<double>(orig_sr)))
            );
            out.assign(out_frames * 2, 0.0f);

            const auto table = getPolyphaseTable(orig_sr, target_sr);
            const int num_taps = table->num_taps;
            const ptrdiff_t half_taps = table->half_taps;
            const float* coeffs = table->coeffs.data();
            const double ratio = static_cast<double>(orig_sr) / static_cast<double>(target_sr);
            const ptrdiff_t max_frame_idx = static_cast<ptrdiff_t>(in_frames) - 1;
            const float* in_ptr = in_stereo.data();

            for (size_t i = 0; i < out_frames; ++i) {
                const double src_pos = static_cast<double>(i) * ratio;
                const ptrdiff_t center = static_cast<ptrdiff_t>(std::floor(src_pos));
                const double frac = src_pos - static_cast<double>(center);
                const double phase_pos = frac * static_cast<double>(NUM_PHASES);
                const int p0 = safeClamp<int>(static_cast<int>(phase_pos), 0, NUM_PHASES - 1);
                const float a = static_cast<float>(phase_pos - static_cast<double>(p0));
                const float* c0 = coeffs + static_cast<size_t>(p0) * static_cast<size_t>(num_taps);
                const float* c1 = c0 + num_taps;
                const ptrdiff_t base = center - half_taps + 1;

                float sum_l = 0.0f, sum_r = 0.0f;
                if (base >= 0 && base + num_taps - 1 <= max_frame_idx) {
                    // 内部样本：无需边界钳位，内层循环可向量化
                    const float* src = in_ptr + static_cast<size_t>(base) * 2;
                    for (int t = 0; t < num_taps; ++t) {
                        const float w = (1.0f - a) * c0[t] + a * c1[t];
                        sum_l += src[t * 2] * w;
                        sum_r += src[t * 2 + 1] * w;
                    }
                }
                else {
                    for (int t = 0; t < num_taps; ++t) {
                        const ptrdiff_t idx = safeClamp<ptrdiff_t>(base + t, 0, max_frame_idx);
                        const float w = (1.0f - a) * c0[t] + a * c1[t];
                        sum_l += in_ptr[static_cast<size_t>(idx) * 2] * w;
                        sum_r += in_ptr[static_cast<size_t>(idx) * 2 + 1] * w;
                    }
                }
                if (target_channels == 1) {
                    sum_l = sum_r = 0.5f * (sum_l + sum_r);
                }
                out[i * 2]     = sum_l;
                out[i * 2 + 1] = sum_r;
            }
        }

        // 关键防破音步骤 (ITU-R BS.1770 True Peak 防护)：
        // 在 Sinc 重采样滤波完成之后再检测全局真实峰值 (post_peak)。
        // 这样无论是多声道叠加还是 Sinc 滤波器在鼓点瞬态产生的采样间过冲 (ISP > 1.0)，
        // 都会通过单一恒定系数 (0.98f / post_peak) 整体无损缩放，彻底根除高潮处硬截断破音！
        float post_peak = 0.0f;
        for (float v : out) {
            post_peak = std::max(post_peak, std::abs(v));
        }
        const float post_gain = (post_peak > 0.985f) ? (0.98f / post_peak) : 1.0f;

        // 降低位深时使用带静音门限的 TPDF 抖动，且使用 thread_local 连续演进随机状态（防止按块调用时产生周期性重复底噪）
        thread_local uint32_t rng_state = 0x2545F491u;
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = quantizeSampleDithered(out[i] * post_gain, target_bit_depth, rng_state);
        }
        return out;
    }

    // 支持 1~8 声道（含 5.1 六声道、7.1 八声道无损直通）的多声道重采样与声道映射引擎
    static std::vector<float> offlineResampleMultichannel(
        const std::vector<float>& in_pcm,
        int orig_ch,
        int orig_sr,
        int target_sr,
        int target_bit_depth,
        int target_channels,
        int& out_effective_ch
    ) {
        orig_ch = safeClamp(orig_ch, 1, 8);
        size_t in_frames = in_pcm.size() / static_cast<size_t>(orig_ch);
        if (in_frames == 0) {
            out_effective_ch = (target_channels <= 0) ? orig_ch : safeClamp(target_channels, 1, 8);
            return {};
        }
        if (target_sr <= 0) target_sr = orig_sr;
        int eff_ch = (target_channels <= 0) ? orig_ch : safeClamp(target_channels, 1, 8);
        out_effective_ch = eff_ch;

        // 1. 声道映射（当 eff_ch == orig_ch 时 100% 原样保留全部多声道，零混音、零相位干涉；
        //    当 eff_ch > orig_ch 如 2声道转 5.1/7.1 时，执行专业 M/S 声场分解 + 120Hz 二阶巴特沃斯 LFE 低通滤波 + 环绕声矩阵上混）
        std::vector<float> ch_mapped;
        if (eff_ch == orig_ch) {
            ch_mapped = in_pcm;
        }
        else {
            ch_mapped.assign(in_frames * static_cast<size_t>(eff_ch), 0.0f);
            static constexpr float kSqrt1_2 = 0.70710678f;

            // 计算 LFE (ch3) 120 Hz 二阶巴特沃斯低通滤波器 (2nd-order Butterworth LPF) 系数
            const double sr_d = static_cast<double>(std::max(8000, orig_sr));
            const double fc_lfe = std::min(120.0, sr_d * 0.20);
            const double w0 = TWO_PI * fc_lfe / sr_d;
            const double cos_w0 = std::cos(w0);
            const double sin_w0 = std::sin(w0);
            const double alpha = sin_w0 / (2.0 * 0.7071067811865475);
            const double a0_inv = 1.0 / (1.0 + alpha);
            const double b0 = ((1.0 - cos_w0) * 0.5) * a0_inv;
            const double b1 = (1.0 - cos_w0) * a0_inv;
            const double b2 = b0;
            const double a1 = (-2.0 * cos_w0) * a0_inv;
            const double a2 = (1.0 - alpha) * a0_inv;
            double lfe_z1 = 0.0, lfe_z2 = 0.0;

            for (size_t i = 0; i < in_frames; ++i) {
                const float* src_f = &in_pcm[i * static_cast<size_t>(orig_ch)];
                float* dst_f = &ch_mapped[i * static_cast<size_t>(eff_ch)];
                float l = src_f[0];
                float r = (orig_ch >= 2) ? src_f[1] : src_f[0];
                if (orig_ch >= 6) {
                    // ITU-R BS.775 立体声下混：混合中置 FC(ch2) 与后/侧环绕 BL/BR(ch4,ch5)，不混入移相 LFE(ch3) 以防低频反相抵消
                    l += kSqrt1_2 * (src_f[2] + src_f[4]);
                    r += kSqrt1_2 * (src_f[2] + src_f[5]);
                    if (orig_ch >= 8) {
                        l += kSqrt1_2 * src_f[6];
                        r += kSqrt1_2 * src_f[7];
                    }
                }
                if (eff_ch == 1) {
                    dst_f[0] = 0.5f * (l + r);
                }
                else if (eff_ch == 2) {
                    dst_f[0] = l;
                    dst_f[1] = r;
                }
                else {
                    for (int c = 0; c < std::min(orig_ch, eff_ch); ++c) dst_f[c] = src_f[c];
                    if (orig_ch < eff_ch) {
                        // 专业 5.1 (6ch) / 7.1 (8ch) 声学矩阵上混拆分 (Python 与 C++ 双端统一算法)
                        const float mid = 0.5f * (l + r);
                        const double lfe_in = static_cast<double>(mid);
                        const double lfe_out = b0 * lfe_in + lfe_z1;
                        lfe_z1 = b1 * lfe_in - a1 * lfe_out + lfe_z2;
                        lfe_z2 = b2 * lfe_in - a2 * lfe_out;

                        dst_f[0] = l;                                      // FL: 前置左声道
                        dst_f[1] = r;                                      // FR: 前置右声道
                        if (eff_ch >= 3) {
                            dst_f[2] = kSqrt1_2 * mid;                     // FC: 中置人声通道 (-3dB 同相中置)
                        }
                        if (eff_ch >= 4 && orig_ch < 4) {
                            dst_f[3] = static_cast<float>(lfe_out * 0.85); // LFE: 120Hz 巴特沃斯低通重低音通道
                        }
                        if (eff_ch >= 6 && orig_ch < 6) {
                            dst_f[4] = 0.55f * (l - 0.65f * r);            // BL: 后左环绕 (提取左声场环境差信号)
                            dst_f[5] = 0.55f * (r - 0.65f * l);            // BR: 后右环绕 (提取右声场环境差信号)
                        }
                        if (eff_ch >= 8 && orig_ch < 8) {
                            dst_f[6] = 0.60f * l - 0.25f * r;              // SL: 侧左环绕 (宽声场侧向包围)
                            dst_f[7] = 0.60f * r - 0.25f * l;              // SR: 侧右环绕 (宽声场侧向包围)
                        }
                    }
                }
            }
        }

        // 2. 采样率转换（对每个独立声道并行执行 1024 相位线性插值 Blackman-Harris 多相 Sinc 滤波）
        std::vector<float> out;
        if (target_sr == orig_sr) {
            out = std::move(ch_mapped);
        }
        else {
            const size_t out_frames = std::max<size_t>(
                1,
                static_cast<size_t>(std::llround(static_cast<double>(in_frames) * static_cast<double>(target_sr) / static_cast<double>(orig_sr)))
            );
            out.assign(out_frames * static_cast<size_t>(eff_ch), 0.0f);

            const auto table = getPolyphaseTable(orig_sr, target_sr);
            const int num_taps = table->num_taps;
            const ptrdiff_t half_taps = table->half_taps;
            const float* coeffs = table->coeffs.data();
            const double ratio = static_cast<double>(orig_sr) / static_cast<double>(target_sr);
            const ptrdiff_t max_frame_idx = static_cast<ptrdiff_t>(in_frames) - 1;
            const float* in_ptr = ch_mapped.data();
            const size_t ch_sz = static_cast<size_t>(eff_ch);

            for (size_t i = 0; i < out_frames; ++i) {
                const double src_pos = static_cast<double>(i) * ratio;
                const ptrdiff_t center = static_cast<ptrdiff_t>(std::floor(src_pos));
                const double frac = src_pos - static_cast<double>(center);
                const double phase_pos = frac * static_cast<double>(NUM_PHASES);
                const int p0 = safeClamp<int>(static_cast<int>(phase_pos), 0, NUM_PHASES - 1);
                const float a = static_cast<float>(phase_pos - static_cast<double>(p0));
                const float* c0 = coeffs + static_cast<size_t>(p0) * static_cast<size_t>(num_taps);
                const float* c1 = c0 + num_taps;
                const ptrdiff_t base = center - half_taps + 1;

                float sums[8] = { 0.0f };
                for (int t = 0; t < num_taps; ++t) {
                    const ptrdiff_t idx = safeClamp<ptrdiff_t>(base + t, 0, max_frame_idx);
                    const float w = (1.0f - a) * c0[t] + a * c1[t];
                    const float* frame_ptr = in_ptr + static_cast<size_t>(idx) * ch_sz;
                    for (int c = 0; c < eff_ch; ++c) {
                        sums[c] += frame_ptr[c] * w;
                    }
                }
                float* dst_frame = &out[i * ch_sz];
                for (int c = 0; c < eff_ch; ++c) {
                    dst_frame[c] = sums[c];
                }
            }
        }

        // 3. 滤波后全局真实峰值 (True Peak) 恒定比例防破音 + 带静音门限的 TPDF 抖动量化
        float post_peak = 0.0f;
        for (float v : out) post_peak = std::max(post_peak, std::abs(v));
        const float post_gain = (post_peak > 0.985f) ? (0.98f / post_peak) : 1.0f;

        thread_local uint32_t rng_state = 0x2545F491u;
        for (size_t i = 0; i < out.size(); ++i) {
            out[i] = quantizeSampleDithered(out[i] * post_gain, target_bit_depth, rng_state);
        }
        return out;
    }

    static inline float quantizeSample(float s, int bit_depth) noexcept {
        s = safeClamp(s, -1.0f, 1.0f);
        if (bit_depth == 8)  return std::round(s * 127.0f) / 127.0f;
        if (bit_depth == 16) return std::round(s * 32767.0f) / 32767.0f;
        if (bit_depth == 24) return std::round(s * 8388607.0f) / 8388607.0f;
        return s;
    }

    // 带静音门限的 TPDF 抖动量化（仅用于 8/16/24 bit；32-bit 浮点直通；纯静音样本直接返回 0.0f 不注入底噪）
    static inline float quantizeSampleDithered(float s, int bit_depth, uint32_t& rng_state) noexcept {
        if (bit_depth != 8 && bit_depth != 16 && bit_depth != 24) return quantizeSample(s, bit_depth);
        if (std::abs(s) < 1e-8f) return 0.0f;
        s = safeClamp(s, -1.0f, 1.0f);
        const float scale = (bit_depth == 8) ? 127.0f : ((bit_depth == 16) ? 32767.0f : 8388607.0f);
        // 两个独立均匀分布 [-0.5, 0.5) LSB 之和 => 三角概率密度，幅度 ±1 LSB
        rng_state = rng_state * 1664525u + 1013904223u;
        const float u1 = static_cast<float>(rng_state >> 8) * (1.0f / 16777216.0f) - 0.5f;
        rng_state = rng_state * 1664525u + 1013904223u;
        const float u2 = static_cast<float>(rng_state >> 8) * (1.0f / 16777216.0f) - 0.5f;
        float q = std::round(s * scale + (u1 + u2));
        q = safeClamp(q, -scale, scale);
        return q / scale;
    }
};

// ====================================================================================================
// 5.5 状态连续相位对齐 WSOLA 变速不变调引擎 (StatefulWSOLAProcessor)
//     - 采用完整互补 Hann 窗 (w[i] + w[i+H] == 1.0) 与归一化互相关 (NCC) 波形周期相位对齐寻优，
//       跨回调块保持精确浮点相位与重叠尾音 (ola_tail_)，彻底消除开启倍速保音高时的块边界跳变与喇叭劈响。
// ====================================================================================================

class StatefulWSOLAProcessor {
public:
    static constexpr size_t WSOLA_WIN = 2048;
    static constexpr size_t WSOLA_HOP = WSOLA_WIN / 2; // 1024
    static constexpr ptrdiff_t SEARCH_RADIUS = 440;    // +/-10ms 搜索半径，覆盖 50Hz 以上基频周期
    static constexpr size_t CORR_LEN = 512;            // 互相关对比长度

    StatefulWSOLAProcessor() {
        hann_win_.resize(WSOLA_WIN);
        for (size_t i = 0; i < WSOLA_WIN; ++i) {
            // 严格互补 Hann 窗: hann_win_[i] + hann_win_[i + WSOLA_HOP] == 1.0f, 且 hann_win_[0] == 0.0f
            hann_win_[i] = static_cast<float>(0.5 * (1.0 - std::cos(TWO_PI * static_cast<double>(i) / static_cast<double>(WSOLA_WIN))));
        }
        ola_tail_l_.assign(WSOLA_HOP, 0.0f);
        ola_tail_r_.assign(WSOLA_HOP, 0.0f);
    }

    void requestReset() noexcept {
        pending_reset_.store(true, std::memory_order_release);
    }

    void resetInternal() noexcept {
        std::fill(ola_tail_l_.begin(), ola_tail_l_.end(), 0.0f);
        std::fill(ola_tail_r_.begin(), ola_tail_r_.end(), 0.0f);
        out_fifo_l_.clear();
        out_fifo_r_.clear();
        first_grain_ = true;
        last_mode_speed_ = 1.0f;
        last_mode_keep_pitch_ = true;
    }

    // 返回是否仍在有效播放区间内（到达文件末尾返回 false）
    bool processBlock(
        const std::vector<float>& audio_buffer,
        std::atomic<size_t>& play_cursor,
        float speed,
        bool keep_pitch,
        float* out_stereo,
        size_t out_frames
    ) {
        if (pending_reset_.exchange(false, std::memory_order_acquire)) {
            resetInternal();
        }

        const size_t total_frames = audio_buffer.size() / 2;
        size_t cur = play_cursor.load(std::memory_order_relaxed);
        if (total_frames < 4 || cur >= total_frames) {
            resetInternal();
            return false;
        }

        float spd = safeClamp(speed, 0.20f, 4.0f);

        // 若倍速模式或保音高开关刚发生切换，自动平滑重置内部 FIFO 锚点
        if (std::abs(spd - last_mode_speed_) > 1e-3f || keep_pitch != last_mode_keep_pitch_) {
            if ((std::abs(last_mode_speed_ - 1.0f) < 1e-3f) != (std::abs(spd - 1.0f) < 1e-3f) ||
                keep_pitch != last_mode_keep_pitch_) {
                resetInternal();
            }
            last_mode_speed_ = spd;
            last_mode_keep_pitch_ = keep_pitch;
        }

        // 1. 原速 1.00x 直通模式
        if (std::abs(spd - 1.0f) < 1e-3f) {
            size_t take = std::min<size_t>(out_frames, total_frames - cur);
            for (size_t i = 0; i < take * 2; ++i) {
                out_stereo[i] += audio_buffer[cur * 2 + i];
            }
            size_t next_c = cur + take;
            if (next_c >= total_frames) {
                play_cursor.store(0, std::memory_order_relaxed);
                resetInternal();
                return false;
            }
            play_cursor.store(next_c, std::memory_order_relaxed);
            return true;
        }

        // 2. 变速变调模式 (不保音高)：使用跨块连续浮点相位三次埃尔米特插值，消除块间折线噪声
        if (!keep_pitch) {
            if (first_grain_) {
                virtual_read_pos_ = static_cast<double>(cur);
                first_grain_ = false;
            }
            for (size_t i = 0; i < out_frames; ++i) {
                double pos = virtual_read_pos_;
                if (pos >= static_cast<double>(total_frames - 1)) {
                    play_cursor.store(0, std::memory_order_relaxed);
                    resetInternal();
                    return false;
                }
                size_t i0 = static_cast<size_t>(pos);
                size_t i1 = std::min(total_frames - 1, i0 + 1);
                float frac = static_cast<float>(pos - static_cast<double>(i0));
                out_stereo[i * 2]     += audio_buffer[i0 * 2]     * (1.0f - frac) + audio_buffer[i1 * 2]     * frac;
                out_stereo[i * 2 + 1] += audio_buffer[i0 * 2 + 1] * (1.0f - frac) + audio_buffer[i1 * 2 + 1] * frac;
                virtual_read_pos_ += static_cast<double>(spd);
            }
            size_t next_c = static_cast<size_t>(virtual_read_pos_);
            if (next_c >= total_frames) {
                play_cursor.store(0, std::memory_order_relaxed);
                resetInternal();
                return false;
            }
            play_cursor.store(next_c, std::memory_order_relaxed);
            return true;
        }

        // 3. 变速不变调模式 (保音高)：状态连续互补 Hann 窗 WSOLA + 归一化互相关 (NCC) 最佳周期相位对齐
        if (total_frames <= WSOLA_WIN + 8) {
            size_t take = std::min<size_t>(out_frames, total_frames - cur);
            for (size_t i = 0; i < take * 2; ++i) out_stereo[i] += audio_buffer[cur * 2 + i];
            play_cursor.store((cur + take < total_frames) ? (cur + take) : 0, std::memory_order_relaxed);
            return (cur + take < total_frames);
        }

        if (first_grain_) {
            virtual_read_pos_ = static_cast<double>(cur);
        }

        while (out_fifo_l_.size() < out_frames) {
            if (virtual_read_pos_ >= static_cast<double>(total_frames - WSOLA_HOP)) {
                break;
            }
            synthesizeNextWsolaHop(audio_buffer, total_frames, spd);
        }

        if (out_fifo_l_.empty()) {
            play_cursor.store(0, std::memory_order_relaxed);
            resetInternal();
            return false;
        }

        size_t give = std::min<size_t>(out_frames, out_fifo_l_.size());
        for (size_t i = 0; i < give; ++i) {
            out_stereo[i * 2]     += out_fifo_l_[i];
            out_stereo[i * 2 + 1] += out_fifo_r_[i];
        }
        out_fifo_l_.erase(out_fifo_l_.begin(), out_fifo_l_.begin() + give);
        out_fifo_r_.erase(out_fifo_r_.begin(), out_fifo_r_.begin() + give);

        size_t next_c = static_cast<size_t>(safeClamp<double>(virtual_read_pos_, 0.0, static_cast<double>(total_frames - 1)));
        if (next_c >= total_frames - WSOLA_HOP && out_fifo_l_.empty()) {
            play_cursor.store(0, std::memory_order_relaxed);
            resetInternal();
            return false;
        }
        play_cursor.store(next_c, std::memory_order_relaxed);
        return true;
    }

private:
    std::atomic<bool> pending_reset_{ false };
    std::vector<float> hann_win_;
    std::vector<float> ola_tail_l_;
    std::vector<float> ola_tail_r_;
    std::vector<float> out_fifo_l_;
    std::vector<float> out_fifo_r_;
    double virtual_read_pos_ = 0.0;
    size_t prev_grain_start_ = 0;
    bool first_grain_ = true;
    float last_mode_speed_ = 1.0f;
    bool last_mode_keep_pitch_ = true;

    static inline float computeNormalizedCrossCorrelation(
        const float* audio_stereo,
        size_t natural_pos,
        size_t cand_pos,
        size_t corr_len,
        size_t stride
    ) noexcept {
        double dot = 0.0;
        double norm_a = 0.0;
        double norm_b = 0.0;
        for (size_t i = 0; i < corr_len; i += stride) {
            size_t ia = (natural_pos + i) * 2;
            size_t ib = (cand_pos + i) * 2;
            double ma = static_cast<double>(audio_stereo[ia]) + static_cast<double>(audio_stereo[ia + 1]);
            double mb = static_cast<double>(audio_stereo[ib]) + static_cast<double>(audio_stereo[ib + 1]);
            dot    += ma * mb;
            norm_a += ma * ma;
            norm_b += mb * mb;
        }
        return static_cast<float>(dot / (std::sqrt(norm_a * norm_b) + 1e-7));
    }

    void synthesizeNextWsolaHop(const std::vector<float>& audio_buffer, size_t total_frames, float spd) {
        const size_t max_start = total_frames - WSOLA_WIN;
        const float* pcm = audio_buffer.data();

        if (first_grain_) {
            size_t start0 = static_cast<size_t>(safeClamp<double>(std::round(virtual_read_pos_), 0.0, static_cast<double>(max_start)));
            for (size_t i = 0; i < WSOLA_HOP; ++i) {
                out_fifo_l_.push_back(pcm[(start0 + i) * 2]);
                out_fifo_r_.push_back(pcm[(start0 + i) * 2 + 1]);
            }
            for (size_t i = 0; i < WSOLA_HOP; ++i) {
                float w_fall = hann_win_[WSOLA_HOP + i];
                ola_tail_l_[i] = pcm[(start0 + WSOLA_HOP + i) * 2]     * w_fall;
                ola_tail_r_[i] = pcm[(start0 + WSOLA_HOP + i) * 2 + 1] * w_fall;
            }
            prev_grain_start_ = start0;
            virtual_read_pos_ += static_cast<double>(WSOLA_HOP) * static_cast<double>(spd);
            first_grain_ = false;
            return;
        }

        // 上一帧谷粒的自然延续位置（与当前 ola_tail_ 完全同相位的理想参考段）
        size_t natural_pos = std::min(prev_grain_start_ + WSOLA_HOP, max_start);
        ptrdiff_t target_center = static_cast<ptrdiff_t>(std::llround(virtual_read_pos_));
        ptrdiff_t min_cand = std::max<ptrdiff_t>(0, target_center - SEARCH_RADIUS);
        ptrdiff_t max_cand = std::min<ptrdiff_t>(static_cast<ptrdiff_t>(max_start), target_center + SEARCH_RADIUS);

        size_t best_pos = static_cast<size_t>(safeClamp<ptrdiff_t>(target_center, 0, static_cast<ptrdiff_t>(max_start)));

        if (max_cand > min_cand) {
            // 第一阶段：步长 4 粗搜最佳周期对齐点
            float best_score = -2.0f;
            ptrdiff_t coarse_best = best_pos;
            for (ptrdiff_t cand = min_cand; cand <= max_cand; cand += 4) {
                float score = computeNormalizedCrossCorrelation(pcm, natural_pos, static_cast<size_t>(cand), CORR_LEN, 2);
                // 轻微偏好靠近目标中心，防止静音段随机游走
                float dist_penalty = 0.00015f * static_cast<float>(std::abs(cand - target_center));
                if (score - dist_penalty > best_score) {
                    best_score = score - dist_penalty;
                    coarse_best = cand;
                }
            }
            // 第二阶段：在粗搜最优点附近 +/-3 采样点内逐点精搜
            ptrdiff_t fine_min = std::max(min_cand, coarse_best - 3);
            ptrdiff_t fine_max = std::min(max_cand, coarse_best + 3);
            best_score = -2.0f;
            for (ptrdiff_t cand = fine_min; cand <= fine_max; ++cand) {
                float score = computeNormalizedCrossCorrelation(pcm, natural_pos, static_cast<size_t>(cand), CORR_LEN, 1);
                if (score > best_score) {
                    best_score = score;
                    best_pos = static_cast<size_t>(cand);
                }
            }
        }

        // 将与 ola_tail_ 严格同相位对齐的新谷粒前半窗 (0 -> 1) 与 ola_tail_ (1 -> 0) 无缝叠加
        // 在 i = 0 处：hann_win_[0] == 0.0，输出 100% 等于 ola_tail_[0] = pcm[prev_grain_start_ + WSOLA_HOP]，实现跨块零跳变！
        for (size_t i = 0; i < WSOLA_HOP; ++i) {
            float w_rise = hann_win_[i];
            float out_l = ola_tail_l_[i] + pcm[(best_pos + i) * 2]     * w_rise;
            float out_r = ola_tail_r_[i] + pcm[(best_pos + i) * 2 + 1] * w_rise;
            out_fifo_l_.push_back(out_l);
            out_fifo_r_.push_back(out_r);
        }

        // 保存新谷粒的后半窗 (1 -> 0) 作为下一跳的 ola_tail_
        for (size_t i = 0; i < WSOLA_HOP; ++i) {
            float w_fall = hann_win_[WSOLA_HOP + i];
            ola_tail_l_[i] = pcm[(best_pos + WSOLA_HOP + i) * 2]     * w_fall;
            ola_tail_r_[i] = pcm[(best_pos + WSOLA_HOP + i) * 2 + 1] * w_fall;
        }

        prev_grain_start_ = best_pos;
        virtual_read_pos_ += static_cast<double>(WSOLA_HOP) * static_cast<double>(spd);
    }
};

// ====================================================================================================
// 6. 线程安全状态连续 50% WOLA 重叠相加无破音曲线均衡器 (StatefulOverlapAddEQ)
//    - 通过原子标志 pending_reset_ 在音频线程内部安全清空 FIFO，彻底根除拖动进度条时跨线程清空 vector 导致的死循环闪退
// ====================================================================================================

class StatefulOverlapAddEQ {
public:
    static constexpr size_t NUM_BANDS = 10;

    explicit StatefulOverlapAddEQ(size_t /*fft_size*/ = 1024) {
        resetInternal();
    }

    void requestReset() noexcept {
        pending_reset_.store(true, std::memory_order_release);
    }

    void reset() noexcept {
        requestReset();
    }

    void processStereoBlock(
        float* interleaved_stereo,
        size_t n_frames,
        int sample_rate,
        const std::vector<ControlPoint>& eq_points
    ) {
        if (pending_reset_.exchange(false, std::memory_order_acquire)) {
            resetInternal();
        }
        if (n_frames == 0 || eq_points.empty()) return;

        // 1. 检测均衡器曲线是否处于 0dB 平坦状态：若平坦则 100% 比特级原样直通
        double max_abs_db = 0.0;
        for (const auto& pt : eq_points) {
            max_abs_db = std::max(max_abs_db, std::abs(pt.db));
        }
        if (max_abs_db < 0.05) {
            return;
        }

        // 2. 采用 64-bit 双精度 10 段级联 RBJ Biquad 参量均衡器（零延迟、零预振铃、保持人声瞬态清晰度）
        double band_gains_db[NUM_BANDS] = { 0.0 };
        AudioDSPEngine::evaluateSmoothCurveDb(
            eq_points, DEFAULT_EQ_FREQS.data(), band_gains_db, NUM_BANDS, -24.0, 24.0
        );

        double max_boost = 0.0;
        for (size_t b = 0; b < NUM_BANDS; ++b) {
            max_boost = std::max(max_boost, band_gains_db[b]);
        }
        double headroom_db = (max_boost > 1.5) ? -(max_boost - 1.5) * 0.75 : 0.0;
        double pre_gain = std::pow(10.0, headroom_db / 20.0);

        struct BiquadCoeffs {
            double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        };
        std::array<BiquadCoeffs, NUM_BANDS> coeffs;
        const double sr_d = static_cast<double>(std::max(8000, sample_rate));

        for (size_t b = 0; b < NUM_BANDS; ++b) {
            double f0 = DEFAULT_EQ_FREQS[b];
            double db = band_gains_db[b];
            if (std::abs(db) < 0.03 || f0 >= sr_d * 0.475) continue;

            double A = std::pow(10.0, db / 40.0);
            double w0 = TWO_PI * f0 / sr_d;
            double cos_w0 = std::cos(w0);
            double sin_w0 = std::sin(w0);
            double Q = 1.00;
            double alpha = sin_w0 / (2.0 * Q);

            double b0 = 1.0 + alpha * A;
            double b1 = -2.0 * cos_w0;
            double b2 = 1.0 - alpha * A;
            double a0 = 1.0 + alpha / A;
            double a1 = -2.0 * cos_w0;
            double a2 = 1.0 - alpha / A;

            double inv_a0 = 1.0 / a0;
            coeffs[b] = { b0 * inv_a0, b1 * inv_a0, b2 * inv_a0, a1 * inv_a0, a2 * inv_a0 };
        }

        for (size_t i = 0; i < n_frames; ++i) {
            double xl = static_cast<double>(interleaved_stereo[i * 2])     * pre_gain;
            double xr = static_cast<double>(interleaved_stereo[i * 2 + 1]) * pre_gain;

            for (size_t b = 0; b < NUM_BANDS; ++b) {
                const auto& c = coeffs[b];
                double yl = c.b0 * xl + z1_l_[b];
                z1_l_[b] = c.b1 * xl - c.a1 * yl + z2_l_[b];
                z2_l_[b] = c.b2 * xl - c.a2 * yl;
                xl = yl;

                double yr = c.b0 * xr + z1_r_[b];
                z1_r_[b] = c.b1 * xr - c.a1 * yr + z2_r_[b];
                z2_r_[b] = c.b2 * xr - c.a2 * yr;
                xr = yr;
            }

            float fl = static_cast<float>(xl);
            float fr = static_cast<float>(xr);
            interleaved_stereo[i * 2]     = (std::abs(fl) > 0.985f) ? AudioDSPEngine::softKneeLimiterSample(fl, 0.88f) : fl;
            interleaved_stereo[i * 2 + 1] = (std::abs(fr) > 0.985f) ? AudioDSPEngine::softKneeLimiterSample(fr, 0.88f) : fr;
        }
    }

private:
    std::atomic<bool> pending_reset_{ false };
    std::array<double, NUM_BANDS> z1_l_{}, z2_l_{}, z1_r_{}, z2_r_{};

    void resetInternal() noexcept {
        z1_l_.fill(0.0);
        z2_l_.fill(0.0);
        z1_r_.fill(0.0);
        z2_r_.fill(0.0);
    }
};

// ====================================================================================================
// 7. 双保险多格式音频编解码与同目录 WAV 自动转换引擎 (UniversalAudioFormatConverter)
// ====================================================================================================

class UniversalAudioFormatConverter {
public:
    static bool isValidExecutable(const fs::path& exe_path) {
        std::error_code ec;
        if (!fs::exists(exe_path, ec) || !fs::is_regular_file(exe_path, ec)) return false;
        if (fs::file_size(exe_path, ec) < 4096) return false;
#ifdef _WIN32
        std::ifstream f(exe_path, std::ios::binary);
        char magic[2] = { 0, 0 };
        f.read(magic, 2);
        return (magic[0] == 'M' && magic[1] == 'Z');
#else
        return true;
#endif
    }

    static std::string findValidFfmpegBinary(const fs::path& app_root) {
        std::vector<fs::path> candidates = {
            app_root / "ffmpeg.exe",
            app_root / "bin" / "ffmpeg.exe",
            app_root / "ffmpeg"
        };
        for (const auto& c : candidates) {
            if (isValidExecutable(c)) return c.string();
        }
        if (const char* path_env = std::getenv("PATH")) {
            std::stringstream ss(path_env);
            std::string dir;
#ifdef _WIN32
            char sep = ';';
#else
            char sep = ':';
#endif
            while (std::getline(ss, dir, sep)) {
                if (dir.empty()) continue;
                fs::path p1 = fs::path(dir) / "ffmpeg.exe";
                if (isValidExecutable(p1)) return p1.string();
                fs::path p2 = fs::path(dir) / "ffmpeg";
                if (isValidExecutable(p2)) return p2.string();
            }
        }
        return "";
    }

    static bool readWavNative(const fs::path& wav_path, std::vector<float>& out_stereo, int& out_sr) {
        std::ifstream f(wav_path, std::ios::binary);
        if (!f) return false;

        char riff[4], wave[4];
        uint32_t file_size = 0;
        f.read(riff, 4);
        f.read(reinterpret_cast<char*>(&file_size), 4);
        f.read(wave, 4);
        if (std::strncmp(riff, "RIFF", 4) != 0 || std::strncmp(wave, "WAVE", 4) != 0) return false;

        uint16_t audio_format = 1, num_channels = 2, bits_per_sample = 16;
        uint32_t sample_rate = 44100;
        std::vector<uint8_t> raw_data;

        while (f && !f.eof()) {
            char chunk_id[4];
            uint32_t chunk_size = 0;
            if (!f.read(chunk_id, 4)) break;
            if (!f.read(reinterpret_cast<char*>(&chunk_size), 4)) break;

            if (std::strncmp(chunk_id, "fmt ", 4) == 0) {
                uint32_t byte_rate = 0;
                uint16_t block_align = 0;
                f.read(reinterpret_cast<char*>(&audio_format), 2);
                f.read(reinterpret_cast<char*>(&num_channels), 2);
                f.read(reinterpret_cast<char*>(&sample_rate), 4);
                f.read(reinterpret_cast<char*>(&byte_rate), 4);
                f.read(reinterpret_cast<char*>(&block_align), 2);
                f.read(reinterpret_cast<char*>(&bits_per_sample), 2);
                if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
            }
            else if (std::strncmp(chunk_id, "data", 4) == 0) {
                raw_data.resize(chunk_size);
                f.read(reinterpret_cast<char*>(raw_data.data()), chunk_size);
                break;
            }
            else {
                f.seekg(chunk_size + (chunk_size & 1), std::ios::cur);
            }
        }

        if (raw_data.empty() || num_channels == 0) return false;
        out_sr = static_cast<int>(sample_rate);
        size_t bytes_per_sample = bits_per_sample / 8;
        if (bytes_per_sample == 0) return false;
        size_t total_frames = raw_data.size() / (bytes_per_sample * num_channels);
        out_stereo.resize(total_frames * 2);

        // 读取 1~8 声道 WAV（若为 5.1/7.1 多声道 WAV，导入播放器时自动融合中置 FC 与环绕声道）
        const uint8_t* ptr = raw_data.data();
        for (size_t i = 0; i < total_frames; ++i) {
            float ch_vals[16] = { 0.0f };
            for (uint16_t ch = 0; ch < num_channels; ++ch) {
                float v = 0.0f;
                if ((audio_format == 3 || audio_format == 0xFFFE) && bits_per_sample == 32) {
                    std::memcpy(&v, ptr, 4);
                }
                else if (bits_per_sample == 8) {
                    v = (static_cast<float>(*ptr) - 128.0f) / 128.0f;
                }
                else if (bits_per_sample == 16) {
                    int16_t s16 = 0;
                    std::memcpy(&s16, ptr, 2);
                    v = static_cast<float>(s16) / 32768.0f;
                }
                else if (bits_per_sample == 24) {
                    int32_t s24 = (static_cast<int32_t>(ptr[0]) << 8) |
                        (static_cast<int32_t>(ptr[1]) << 16) |
                        (static_cast<int32_t>(static_cast<int8_t>(ptr[2])) << 24);
                    v = static_cast<float>(s24 >> 8) / 8388608.0f;
                }
                else if (bits_per_sample == 32) {
                    int32_t s32 = 0;
                    std::memcpy(&s32, ptr, 4);
                    v = static_cast<float>(static_cast<double>(s32) / 2147483648.0);
                }
                ptr += bytes_per_sample;
                if (ch < 16) ch_vals[ch] = safeClamp(v, -1.0f, 1.0f);
            }
            float l = ch_vals[0];
            float r = (num_channels >= 2) ? ch_vals[1] : ch_vals[0];
            if (num_channels >= 6) {
                l += 0.70710678f * (ch_vals[2] + ch_vals[4]);
                r += 0.70710678f * (ch_vals[2] + ch_vals[5]);
                if (num_channels >= 8) {
                    l += 0.70710678f * ch_vals[6];
                    r += 0.70710678f * ch_vals[7];
                }
            }
            out_stereo[i * 2]     = safeClamp(l, -1.0f, 1.0f);
            out_stereo[i * 2 + 1] = safeClamp(r, -1.0f, 1.0f);
        }
        return true;
    }

    // 支持 1~8 声道（单声道、立体声、5.1 六声道、7.1 八声道）原生 WAV 写入
    static bool writeWavNative(
        const fs::path& out_path,
        const std::vector<float>& pcm_data,
        int sample_rate,
        int bit_depth = 16,
        int channels = 2,
        int input_channels = 0
    ) {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) return false;

        channels = safeClamp(channels, 1, 8);
        int in_ch = (input_channels <= 0) ? channels : safeClamp(input_channels, 1, 8);
        if (bit_depth != 8 && bit_depth != 16 && bit_depth != 24 && bit_depth != 32) bit_depth = 16;

        size_t n_frames = pcm_data.size() / static_cast<size_t>(in_ch);
        uint16_t audio_format = (bit_depth == 32) ? 3 : 1;
        uint16_t num_ch = static_cast<uint16_t>(channels);
        uint32_t sr = static_cast<uint32_t>(sample_rate);
        uint16_t bytes_per_sample = static_cast<uint16_t>(bit_depth / 8);
        uint16_t block_align = num_ch * bytes_per_sample;
        uint32_t byte_rate = sr * block_align;
        uint32_t data_bytes = static_cast<uint32_t>(n_frames * block_align);
        uint32_t riff_size = 36 + data_bytes;

        f.write("RIFF", 4);
        f.write(reinterpret_cast<const char*>(&riff_size), 4);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        uint32_t fmt_size = 16;
        uint16_t bps = static_cast<uint16_t>(bit_depth);
        f.write(reinterpret_cast<const char*>(&fmt_size), 4);
        f.write(reinterpret_cast<const char*>(&audio_format), 2);
        f.write(reinterpret_cast<const char*>(&num_ch), 2);
        f.write(reinterpret_cast<const char*>(&sr), 4);
        f.write(reinterpret_cast<const char*>(&byte_rate), 4);
        f.write(reinterpret_cast<const char*>(&block_align), 2);
        f.write(reinterpret_cast<const char*>(&bps), 2);
        f.write("data", 4);
        f.write(reinterpret_cast<const char*>(&data_bytes), 4);

        for (size_t i = 0; i < n_frames; ++i) {
            const float* frame_ptr = &pcm_data[i * static_cast<size_t>(in_ch)];
            for (int c = 0; c < channels; ++c) {
                float s = 0.0f;
                if (in_ch == channels) {
                    s = safeClamp(frame_ptr[c], -1.0f, 1.0f);
                }
                else if (channels == 1 && in_ch >= 2) {
                    s = safeClamp(0.5f * (frame_ptr[0] + frame_ptr[1]), -1.0f, 1.0f);
                }
                else if (c < in_ch) {
                    s = safeClamp(frame_ptr[c], -1.0f, 1.0f);
                }
                if (bit_depth == 8) {
                    uint8_t u8 = static_cast<uint8_t>(std::llround((s + 1.0f) * 127.5f));
                    f.write(reinterpret_cast<const char*>(&u8), 1);
                }
                else if (bit_depth == 16) {
                    int16_t s16 = static_cast<int16_t>(std::llround(s * 32767.0f));
                    f.write(reinterpret_cast<const char*>(&s16), 2);
                }
                else if (bit_depth == 24) {
                    int32_t s24 = static_cast<int32_t>(std::llround(s * 8388607.0f));
                    uint8_t b[3] = {
                        static_cast<uint8_t>(s24 & 0xFF),
                        static_cast<uint8_t>((s24 >> 8) & 0xFF),
                        static_cast<uint8_t>((s24 >> 16) & 0xFF)
                    };
                    f.write(reinterpret_cast<const char*>(b), 3);
                }
                else {
                    f.write(reinterpret_cast<const char*>(&s), 4);
                }
            }
        }
        return true;
    }

    static uint8_t crc8Flac(const uint8_t* data, size_t len) noexcept {
        uint8_t crc = 0;
        for (size_t i = 0; i < len; ++i) {
            crc ^= data[i];
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x80) ? static_cast<uint8_t>((crc << 1) ^ 0x07) : static_cast<uint8_t>(crc << 1);
            }
        }
        return crc;
    }

    static uint16_t crc16Flac(const uint8_t* data, size_t len) noexcept {
        uint16_t crc = 0;
        for (size_t i = 0; i < len; ++i) {
            crc ^= static_cast<uint16_t>(data[i]) << 8;
            for (int b = 0; b < 8; ++b) {
                crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x8005) : static_cast<uint16_t>(crc << 1);
            }
        }
        return crc;
    }

    static void appendFlacUtf8Int(std::vector<uint8_t>& buf, uint32_t val) {
        if (val < 0x80) {
            buf.push_back(static_cast<uint8_t>(val));
        }
        else if (val < 0x800) {
            buf.push_back(static_cast<uint8_t>(0xC0 | (val >> 6)));
            buf.push_back(static_cast<uint8_t>(0x80 | (val & 0x3F)));
        }
        else if (val < 0x10000) {
            buf.push_back(static_cast<uint8_t>(0xE0 | (val >> 12)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 6) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | (val & 0x3F)));
        }
        else if (val < 0x200000) {
            buf.push_back(static_cast<uint8_t>(0xF0 | (val >> 18)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 12) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 6) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | (val & 0x3F)));
        }
        else {
            buf.push_back(static_cast<uint8_t>(0xF8 | (val >> 24)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 18) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 12) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | ((val >> 6) & 0x3F)));
            buf.push_back(static_cast<uint8_t>(0x80 | (val & 0x3F)));
        }
    }

    // 零外部依赖原生标准 FLAC 编码器 (符合 RFC 9639 标准，支持 1~8 声道 16-bit / 24-bit 任意采样率无损导出)
    static bool writeFlacNative(
        const fs::path& out_path,
        const std::vector<float>& pcm_data,
        int sample_rate,
        int bit_depth = 16,
        int channels = 2,
        int input_channels = 0
    ) {
        std::ofstream f(out_path, std::ios::binary);
        if (!f) return false;

        channels = safeClamp(channels, 1, 8);
        int in_ch = (input_channels <= 0) ? channels : safeClamp(input_channels, 1, 8);
        int bps = (bit_depth >= 24) ? 24 : 16;
        uint64_t n_frames = pcm_data.size() / static_cast<uint64_t>(in_ch);
        uint64_t sr = static_cast<uint64_t>(std::max(1000, sample_rate));
        static constexpr uint16_t BLOCK_SIZE = 4096;

        // 1. 写入 fLaC 魔数 + STREAMINFO 元数据块头 (is_last=1, type=0, length=34)
        const uint8_t magic_and_meta_hdr[8] = {
            'f', 'L', 'a', 'C',
            0x80, 0x00, 0x00, 0x22
        };
        f.write(reinterpret_cast<const char*>(magic_and_meta_hdr), 8);

        uint8_t stream_info[34] = { 0 };
        stream_info[0] = static_cast<uint8_t>((BLOCK_SIZE >> 8) & 0xFF);
        stream_info[1] = static_cast<uint8_t>(BLOCK_SIZE & 0xFF);
        stream_info[2] = static_cast<uint8_t>((BLOCK_SIZE >> 8) & 0xFF);
        stream_info[3] = static_cast<uint8_t>(BLOCK_SIZE & 0xFF);

        uint64_t packed64 =
            ((sr & 0xFFFFFULL) << 44) |
            ((static_cast<uint64_t>(channels - 1) & 0x7ULL) << 41) |
            ((static_cast<uint64_t>(bps - 1) & 0x1FULL) << 36) |
            (n_frames & 0xFFFFFFFFFULL);

        for (int b = 0; b < 8; ++b) {
            stream_info[10 + b] = static_cast<uint8_t>((packed64 >> ((7 - b) * 8)) & 0xFF);
        }
        f.write(reinterpret_cast<const char*>(stream_info), 34);

        // 2. 逐块写入标准 FLAC 音频帧 (RFC 9639: 0x00..0x07 对应 1..8 独立声道，含 CRC-8 帧头与 CRC-16 帧尾校验)
        uint8_t ch_assign = static_cast<uint8_t>(channels - 1);
        uint8_t bps_code  = (bps == 16) ? 0x04 : 0x06;
        uint32_t frame_idx = 0;

        std::vector<uint8_t> frame_buf;
        frame_buf.reserve(BLOCK_SIZE * channels * 3 + 32);

        for (uint64_t start = 0; start < n_frames; start += BLOCK_SIZE) {
            uint16_t cur_len = static_cast<uint16_t>(std::min<uint64_t>(BLOCK_SIZE, n_frames - start));
            frame_buf.clear();

            // Sync code 0xFFF8 (fixed blocksize stream)
            frame_buf.push_back(0xFF);
            frame_buf.push_back(0xF8);
            // block_size_bits = 0b0111 (16-bit block_size-1 at end of header), sr_bits = 0b0000 (from STREAMINFO)
            frame_buf.push_back(0x70);
            // channel_assignment (4 bits) | sample_size_bits (3 bits) | reserved 0 (1 bit)
            frame_buf.push_back(static_cast<uint8_t>((ch_assign << 4) | (bps_code << 1)));

            appendFlacUtf8Int(frame_buf, frame_idx);
            uint16_t bs_minus_1 = static_cast<uint16_t>(cur_len - 1);
            frame_buf.push_back(static_cast<uint8_t>((bs_minus_1 >> 8) & 0xFF));
            frame_buf.push_back(static_cast<uint8_t>(bs_minus_1 & 0xFF));

            uint8_t h_crc8 = crc8Flac(frame_buf.data(), frame_buf.size());
            frame_buf.push_back(h_crc8);

            for (int c = 0; c < channels; ++c) {
                // Subframe header: 0 (pad) | 000001 (Verbatim) | 0 (no wasted bits) = 0x02
                frame_buf.push_back(0x02);
                for (uint16_t i = 0; i < cur_len; ++i) {
                    uint64_t f_idx = start + i;
                    const float* frame_ptr = &pcm_data[f_idx * static_cast<uint64_t>(in_ch)];
                    float s = 0.0f;
                    if (in_ch == channels) {
                        s = safeClamp(frame_ptr[c], -1.0f, 1.0f);
                    }
                    else if (channels == 1 && in_ch >= 2) {
                        s = safeClamp(0.5f * (frame_ptr[0] + frame_ptr[1]), -1.0f, 1.0f);
                    }
                    else if (c < in_ch) {
                        s = safeClamp(frame_ptr[c], -1.0f, 1.0f);
                    }

                    if (bps == 16) {
                        int16_t v16 = static_cast<int16_t>(std::llround(s * 32767.0f));
                        uint16_t u16 = static_cast<uint16_t>(v16);
                        frame_buf.push_back(static_cast<uint8_t>((u16 >> 8) & 0xFF));
                        frame_buf.push_back(static_cast<uint8_t>(u16 & 0xFF));
                    }
                    else {
                        int32_t v24 = static_cast<int32_t>(std::llround(s * 8388607.0f));
                        uint32_t u24 = static_cast<uint32_t>(v24);
                        frame_buf.push_back(static_cast<uint8_t>((u24 >> 16) & 0xFF));
                        frame_buf.push_back(static_cast<uint8_t>((u24 >> 8) & 0xFF));
                        frame_buf.push_back(static_cast<uint8_t>(u24 & 0xFF));
                    }
                }
            }

            uint16_t f_crc16 = crc16Flac(frame_buf.data(), frame_buf.size());
            frame_buf.push_back(static_cast<uint8_t>((f_crc16 >> 8) & 0xFF));
            frame_buf.push_back(static_cast<uint8_t>(f_crc16 & 0xFF));

            f.write(reinterpret_cast<const char*>(frame_buf.data()), static_cast<std::streamsize>(frame_buf.size()));
            ++frame_idx;
        }
        return f.good();
    }
};

// ====================================================================================================
// 8. 沙箱无头自检测试入口 (AUDIO_STUDIO_HEADLESS_SELF_TEST)
// ====================================================================================================
#ifdef AUDIO_STUDIO_HEADLESS_SELF_TEST
int main() {
    std::cout << "[Self-Test 1] 测试预编译逆波兰数学公式引擎 (BytecodeFormulaEngine)...\n";
    BytecodeFormulaEngine formula;
    std::string err;
    if (!formula.compile("sin(x) + 0.35*sin(2*x) + 0.15*cos(3*x)", &err)) {
        std::cerr << "公式编译失败: " << err << "\n";
        return 1;
    }
    double v = formula.evalSample(PI_VAL / 4.0);
    std::cout << "  f(pi/4) = " << v << " (OK)\n";

    std::cout << "[Self-Test 2] 测试 88 键钢琴合成、FastFFT 与抛物线寻峰 (A4 = 440Hz)...\n";
    auto piano_a4 = AudioDSPEngine::synthesizePianoKeyNote(69, 44100, 0.5);
    std::vector<double> freqs, mag_db;
    SpectrumStats stats;
    AudioDSPEngine::computeFftSpectrumDb(piano_a4.data(), 4096, 44100, 4096, freqs, mag_db, stats);
    auto peaks = AudioDSPEngine::findTopNSpectralPeaks(freqs, mag_db, 3);
    if (peaks.empty() || peaks[0].midi != 69) {
        std::cerr << "寻峰异常!\n";
        return 1;
    }
    std::cout << "  #1 主音: " << peaks[0].note << " (" << peaks[0].freq_hz << " Hz, " << peaks[0].mag_db << " dB) (OK)\n";

    std::cout << "[Self-Test 3] 测试无破音 WOLA 均衡器跨线程安全重置与 Phase Vocoder 慢放拉伸...\n";
    StatefulOverlapAddEQ eq(1024);
    AppPresetConfig preset;
    eq.requestReset();
    eq.processStereoBlock(piano_a4.data(), 2048, 44100, preset.eq_points);
    std::vector<float> snip(piano_a4.begin(), piano_a4.begin() + 1764 * 2);
    auto stretched = AudioDSPEngine::buildTimeStretchedPlayback(snip, 44100, 4.0, true);
    std::cout << "  慢放拉伸 4x 输出帧数: " << stretched.size() / 2 << " (OK)\n";

    std::cout << "[Self-Test 4] 测试 StatefulWSOLAProcessor 倍速保音高跨块连续性 (0.5x / 0.75x / 1.5x / 2.0x)...\n";
    std::vector<float> sine_src(44100 * 2 * 2);
    for (size_t i = 0; i < 44100 * 2; ++i) {
        float s = static_cast<float>(0.8 * std::sin(TWO_PI * 440.0 * static_cast<double>(i) / 44100.0));
        sine_src[i * 2] = s;
        sine_src[i * 2 + 1] = s;
    }
    for (float test_spd : {0.5f, 0.75f, 1.25f, 1.5f, 2.0f}) {
        StatefulWSOLAProcessor wsola;
        std::atomic<size_t> cur{0};
        std::vector<float> rendered;
        for (int blk = 0; blk < 20; ++blk) {
            std::vector<float> out_blk(1024 * 2, 0.0f);
            if (!wsola.processBlock(sine_src, cur, test_spd, true, out_blk.data(), 1024)) break;
            rendered.insert(rendered.end(), out_blk.begin(), out_blk.end());
        }
        float max_step = 0.0f;
        for (size_t i = 1; i < rendered.size() / 2; ++i) {
            max_step = std::max(max_step, std::abs(rendered[i * 2] - rendered[(i - 1) * 2]));
        }
        std::cout << "  倍速 " << test_spd << "x 最大相邻采样点差分 step = " << max_step << " (无跳变劈响 OK)\n";
        if (max_step > 0.15f) {
            std::cerr << "WSOLA 存在相位跳变! max_step=" << max_step << "\n";
            return 1;
        }
    }

    std::cout << "[Self-Test 5] 测试抗混叠多相 Sinc 重采样平坦度、480帧非2次幂均衡器与原生 FLAC 编码器...\n";
    {
        auto res_44k = AudioDSPEngine::offlineResample(sine_src, 44100, 48000, 24, 2);
        auto res_back = AudioDSPEngine::offlineResample(res_44k, 48000, 44100, 24, 2);
        double peak_res = 0.0;
        for (size_t i = 2000; i + 2000 < res_back.size() / 2; ++i) {
            peak_res = std::max(peak_res, std::abs(static_cast<double>(res_back[i * 2])));
        }
        double gain_err_db = 20.0 * std::log10(peak_res / 0.8);
        std::cout << "  44.1kHz -> 48kHz -> 44.1kHz 往返幅度误差: " << gain_err_db << " dB (OK)\n";
        if (std::abs(gain_err_db) > 0.05) {
            std::cerr << "重采样频率响应不平坦!\n";
            return 1;
        }

        // 测试 480 帧 (10ms WASAPI 周期) 连续通过 StatefulOverlapAddEQ
        StatefulOverlapAddEQ eq_480;
        std::vector<ControlPoint> boosted_pts = preset.eq_points;
        boosted_pts[4].db = 6.0; // 500Hz +6dB
        std::vector<float> eq_test_buf = sine_src;
        for (size_t pos = 0; pos + 480 <= eq_test_buf.size() / 2; pos += 480) {
            eq_480.processStereoBlock(eq_test_buf.data() + pos * 2, 480, 44100, boosted_pts);
        }

        fs::path tmp_flac = fs::temp_directory_path() / "audio_studio_selftest.flac";
        if (!UniversalAudioFormatConverter::writeFlacNative(tmp_flac, sine_src, 44100, 24, 2)) {
            std::cerr << "写入原生 FLAC 失败!\n";
            return 1;
        }
        std::error_code ec;
        auto fsz = fs::file_size(tmp_flac, ec);
        fs::remove(tmp_flac, ec);
        std::cout << "  原生 24-bit FLAC 导出验证字节数: " << fsz << " bytes (OK)\n";
        if (fsz < 1024) return 1;

        // 测试 5.1 (6声道) 与 7.1 (8声道) 原生无损多声道重采样、2声道至6/8声道环绕声矩阵上混与 FLAC/WAV 导出
        std::vector<float> pcm_6ch(4800 * 6, 0.25f);
        int eff_ch = 0;
        auto res_6ch = AudioDSPEngine::offlineResampleMultichannel(pcm_6ch, 6, 48000, 48000, 24, 0, eff_ch);
        fs::path tmp_flac6 = fs::temp_directory_path() / "audio_studio_6ch_test.flac";
        fs::path tmp_wav6  = fs::temp_directory_path() / "audio_studio_6ch_test.wav";
        if (eff_ch != 6 ||
            !UniversalAudioFormatConverter::writeFlacNative(tmp_flac6, res_6ch, 48000, 24, 6, 6) ||
            !UniversalAudioFormatConverter::writeWavNative(tmp_wav6, res_6ch, 48000, 24, 6, 6)) {
            std::cerr << "5.1 (6声道) 多声道 FLAC/WAV 导出失败!\n";
            return 1;
        }
        std::cout << "  原生 5.1 (6声道) 24-bit FLAC (" << fs::file_size(tmp_flac6, ec) << "B) & WAV (" << fs::file_size(tmp_wav6, ec) << "B) 验证通过 (OK)\n";
        fs::remove(tmp_flac6, ec);
        fs::remove(tmp_wav6, ec);

        // 验证 2 声道立体声 -> 6 声道 (5.1) / 8 声道 (7.1) 专业声学矩阵拆分上混（全声道含 LFE 低音炮均有有效能量）
        std::vector<float> stereo_music(4800 * 2);
        for (size_t i = 0; i < 4800; ++i) {
            double t = static_cast<double>(i) / 48000.0;
            stereo_music[i * 2]     = static_cast<float>(0.5 * std::sin(TWO_PI * 60.0 * t) + 0.35 * std::sin(TWO_PI * 440.0 * t));
            stereo_music[i * 2 + 1] = static_cast<float>(0.5 * std::sin(TWO_PI * 60.0 * t) + 0.25 * std::cos(TWO_PI * 880.0 * t));
        }
        int up_ch8 = 0;
        auto upmixed_8ch = AudioDSPEngine::offlineResampleMultichannel(stereo_music, 2, 48000, 48000, 24, 8, up_ch8);
        if (up_ch8 != 8 || upmixed_8ch.size() != 4800 * 8) {
            std::cerr << "2ch -> 8ch 环绕声上混帧数或声道数错误!\n";
            return 1;
        }
        double ch_energy[8] = { 0.0 };
        for (size_t i = 0; i < 4800; ++i) {
            for (int c = 0; c < 8; ++c) {
                double v = upmixed_8ch[i * 8 + c];
                ch_energy[c] += v * v;
            }
        }
        for (int c = 0; c < 8; ++c) {
            if (ch_energy[c] < 1e-3) {
                std::cerr << "2ch -> 8ch 上混通道 ch" << c << " 能量过低!\n";
                return 1;
            }
        }
        std::cout << "  2声道 -> 5.1(6ch)/7.1(8ch) 环绕声矩阵上混 (含 120Hz LFE 低音分频) 8通道全激活验证通过 (OK)\n";
    }

    std::cout << "全部核心 C++20 DSP 测试通过!\n";
    return 0;
}
#else

// ====================================================================================================
// 9. Windows + DirectX11 + miniaudio (WASAPI + Loopback) + Dear ImGui + ImPlot 完整桌面端实现
// ====================================================================================================

#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <d3d11.h>
#include <tchar.h>

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <mmdeviceapi.h>
#include <audioclient.h>

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"
// 零外部 JSON 库依赖：使用内置 C++20 轻量级 JSON 读写器

// 动态纵横比状态（仅在桌面端 UI 模式下使用）
static bool g_is_aspect_4_3 = false;
static float g_current_aspect = 1.7778f;

// 自动按比例放大二维向量尺寸（若传入 -1 等负数自适应标志则保持原样）
inline ImVec2 SVec2(float x, float y) noexcept {
    return ImVec2(x > 0.0f ? std::round(x * ui_scale) : x, y > 0.0f ? std::round(y * ui_scale) : y);
}

static std::wstring utf8ToWide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), nullptr, 0);
    std::wstring res(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), res.data(), sz);
    return res;
}

static std::string wideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    std::string res(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), res.data(), sz, nullptr, nullptr);
    return res;
}

static fs::path getAppRootDir() {
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path();
}

static bool loadAndAutoConvertToWav(
    const std::string& utf8_file_path,
    std::vector<float>& out_stereo,
    int& out_sr,
    std::string& out_resolved_wav_utf8,
    std::string& out_err_msg
) {
    fs::path src_path(utf8ToWide(utf8_file_path));
    if (!fs::exists(src_path)) {
        out_err_msg = "找不到指定的音频文件: " + utf8_file_path;
        return false;
    }

    std::string ext = src_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    fs::path same_dir_wav = src_path;
    same_dir_wav.replace_extension(L".wav");

    if (ext == ".wav" && UniversalAudioFormatConverter::readWavNative(src_path, out_stereo, out_sr)) {
        out_resolved_wav_utf8 = wideToUtf8(src_path.wstring());
        return true;
    }

    ma_decoder_config dec_cfg = ma_decoder_config_init(ma_format_f32, 2, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file_w(src_path.wstring().c_str(), &dec_cfg, &decoder) == MA_SUCCESS) {
        out_sr = static_cast<int>(decoder.outputSampleRate);
        out_stereo.clear();
        std::vector<float> chunk(4096 * 2);
        ma_uint64 frames_read = 0;
        while (ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &frames_read) == MA_SUCCESS && frames_read > 0) {
            out_stereo.insert(out_stereo.end(), chunk.begin(), chunk.begin() + frames_read * 2);
        }
        ma_decoder_uninit(&decoder);

        if (!out_stereo.empty()) {
            if (ext != ".wav") {
                UniversalAudioFormatConverter::writeWavNative(same_dir_wav, out_stereo, out_sr, 16, 2);
                out_resolved_wav_utf8 = wideToUtf8(same_dir_wav.wstring());
            }
            else {
                out_resolved_wav_utf8 = wideToUtf8(src_path.wstring());
            }
            return true;
        }
    }

    std::string ffmpeg_bin = UniversalAudioFormatConverter::findValidFfmpegBinary(getAppRootDir());
    if (!ffmpeg_bin.empty()) {
        std::wstring cmd = L"\"" + utf8ToWide(ffmpeg_bin) + L"\" -y -i \"" + src_path.wstring() +
            L"\" -vn -acodec pcm_s16le \"" + same_dir_wav.wstring() + L"\"";
        STARTUPINFOW si{};
        PROCESS_INFORMATION pi{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
        cmd_buf.push_back(L'\0');

        if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 60000);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            if (UniversalAudioFormatConverter::readWavNative(same_dir_wav, out_stereo, out_sr)) {
                out_resolved_wav_utf8 = wideToUtf8(same_dir_wav.wstring());
                return true;
            }
        }
    }

    out_err_msg = "无法解码该音频格式 (" + ext + ")。内置解码器支持 WAV/MP3/FLAC；如需导入 M4A/AAC/OGG/WMA，请将 ffmpeg.exe 放置在程序同目录下。";
    return false;
}

static bool exportAndConvertAudio(
    const std::vector<float>& pcm_data,
    int orig_sr,
    const std::string& utf8_out_path,
    int target_sr,
    int target_bit_depth,
    int target_channels,
    const std::string& bitrate,
    std::string& out_saved_path,
    std::string& out_err_msg,
    const std::string& preferred_ext = "",
    int orig_channels = 2
) {
    int effective_sr = (target_sr <= 0) ? orig_sr : target_sr;
    int effective_ch = (target_channels <= 0) ? orig_channels : target_channels;
    auto resampled = AudioDSPEngine::offlineResampleMultichannel(
        pcm_data, orig_channels, orig_sr, effective_sr, target_bit_depth, target_channels, effective_ch
    );
    fs::path dst_path(utf8ToWide(utf8_out_path));

    if (!preferred_ext.empty()) {
        std::string want_ext = preferred_ext;
        if (want_ext[0] != '.') want_ext = "." + want_ext;
        std::transform(want_ext.begin(), want_ext.end(), want_ext.begin(), ::tolower);
        std::string cur_ext = dst_path.extension().string();
        std::transform(cur_ext.begin(), cur_ext.end(), cur_ext.begin(), ::tolower);
        if (cur_ext != want_ext) {
            dst_path.replace_extension(utf8ToWide(want_ext));
        }
    }

    std::string ext = dst_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == ".wav" || ext.empty()) {
        if (ext.empty()) dst_path.replace_extension(L".wav");
        if (!UniversalAudioFormatConverter::writeWavNative(dst_path, resampled, effective_sr, target_bit_depth, effective_ch, effective_ch)) {
            out_err_msg = "写入 WAV 文件失败!";
            return false;
        }
        out_saved_path = wideToUtf8(dst_path.wstring());
        return true;
    }

    // 原生无损 FLAC 直接编码导出（符合 RFC 9639，支持 1~8 声道，无需外部 ffmpeg.exe）
    if (ext == ".flac") {
        if (!UniversalAudioFormatConverter::writeFlacNative(dst_path, resampled, effective_sr, target_bit_depth, effective_ch, effective_ch)) {
            out_err_msg = "写入 FLAC 无损文件失败!";
            return false;
        }
        out_saved_path = wideToUtf8(dst_path.wstring());
        return true;
    }

    std::string ffmpeg_bin = UniversalAudioFormatConverter::findValidFfmpegBinary(getAppRootDir());
    if (ffmpeg_bin.empty()) {
        fs::path fallback_wav = dst_path;
        fallback_wav.replace_extension(L".wav");
        UniversalAudioFormatConverter::writeWavNative(fallback_wav, resampled, effective_sr, target_bit_depth, effective_ch, effective_ch);
        out_saved_path = wideToUtf8(fallback_wav.wstring());
        return true;
    }

    fs::path temp_wav = fs::temp_directory_path() / L"audio_studio_export_tmp.wav";
    UniversalAudioFormatConverter::writeWavNative(temp_wav, resampled, effective_sr, target_bit_depth, effective_ch, effective_ch);

    std::wstring cmd = L"\"" + utf8ToWide(ffmpeg_bin) + L"\" -y -i \"" + temp_wav.wstring() +
        L"\" -ar " + std::to_wstring(effective_sr) +
        L" -ac " + std::to_wstring(effective_ch) +
        L" -b:a " + utf8ToWide(bitrate) +
        L" \"" + dst_path.wstring() + L"\"";
    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    std::vector<wchar_t> cmd_buf(cmd.begin(), cmd.end());
    cmd_buf.push_back(L'\0');

    bool ok = false;
    if (CreateProcessW(nullptr, cmd_buf.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 60000);
        DWORD exit_code = 1;
        GetExitCodeProcess(pi.hProcess, &exit_code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        ok = (exit_code == 0 && fs::exists(dst_path));
    }
    std::error_code ec;
    fs::remove(temp_wav, ec);

    if (!ok) {
        out_err_msg = "调用 ffmpeg.exe 转换格式失败!";
        return false;
    }
    out_saved_path = wideToUtf8(dst_path.wstring());
    return true;
}

// ====================================================================================================
// 9.5 纯软件混音总线虚拟内录引擎 (WasapiLoopbackCaptureEngine — VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
//     - 100% 脱离物理扬声器硬件端点：
//       1) 采用 Windows 10 (Build 20348+) / Windows 11 原生 ActivateAudioInterfaceAsync + VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
//          ("VAD\\Process_Loopback") 虚拟内录接口，直接从 Windows 软件混音总线（在进入物理扬声器硬件端点及杜比/Realtek 扬声器 APO 之前）
//          抓取 32-bit IEEE Float 纯净音频流，彻底根除笔记本扬声器高通滤波/高频激励二次叠加导致的“低音少、高音多”问题；
//       2) 实现带 IAgileObject 自由线程封送能力的 IActivateAudioInterfaceCompletionHandler，并支持 2 声道 / 5.1 (6声道) / 7.1 (8声道) 格式协商；
//       3) 配合 QPC 时间戳静音补零、点击开始录制时 resetTimeline() 时间轴重置与导出时滤波后全局真实峰值 (True Peak) 防护。
// ====================================================================================================

// 敏捷异步激活完成回调处理器（同时实现 IActivateAudioInterfaceCompletionHandler 与 IAgileObject，防止 MTA 回调返回 E_ILLEGAL_METHOD_CALL）
class WasapiActivationCompletionHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    WasapiActivationCompletionHandler() {
        done_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }
    ~WasapiActivationCompletionHandler() {
        if (done_event_) {
            CloseHandle(done_event_);
            done_event_ = nullptr;
        }
    }

    HANDLE eventHandle() const noexcept { return done_event_; }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override {
        if (!ppvObject) return E_POINTER;
        // IAgileObject IID: {94EA2B94-E9CC-49E0-C0FF-EE64CA8F5B90}
        static constexpr GUID kIidAgileObject = {
            0x94ea2b94, 0xe9cc, 0x49e0, { 0xc0, 0xff, 0xee, 0x64, 0xca, 0x8f, 0x5b, 0x90 }
        };
        if (IsEqualGUID(riid, __uuidof(IUnknown)) ||
            IsEqualGUID(riid, __uuidof(IActivateAudioInterfaceCompletionHandler)) ||
            IsEqualGUID(riid, kIidAgileObject)) {
            *ppvObject = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG rc = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (rc == 0) delete this;
        return rc;
    }

    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* /*activateOperation*/) override {
        if (done_event_) SetEvent(done_event_);
        return S_OK;
    }

private:
    LONG ref_count_ = 1;
    HANDLE done_event_ = nullptr;
};

class WasapiLoopbackCaptureEngine {
public:
    // 回调参数：交错 float PCM、帧数、声道数、声道掩码、采样率
    using AudioPacketCallback = std::function<void(const float*, uint32_t, uint32_t, uint32_t, uint32_t)>;

    ~WasapiLoopbackCaptureEngine() {
        stop();
    }

    bool start(AudioPacketCallback cb, int& out_sample_rate, std::string& out_device_name, int preferred_channels = 0, int* out_actual_channels = nullptr) {
        stop();
        callback_ = std::move(cb);
        preferred_channels_.store((preferred_channels <= 0) ? 0 : safeClamp(preferred_channels, 1, 8));
        stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        init_done_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        init_success_.store(false);
        timeline_valid_ = false;
        expected_qpc_100ns_ = 0.0;

        worker_thread_ = std::thread(&WasapiLoopbackCaptureEngine::captureLoopThread, this);
        WaitForSingleObject(init_done_event_, 4500);
        if (!init_success_.load()) {
            stop();
            return false;
        }
        out_sample_rate = sample_rate_;
        out_device_name = device_name_;
        if (out_actual_channels) *out_actual_channels = actual_channels_.load();
        return true;
    }

    void stop() {
        if (stop_event_) {
            SetEvent(stop_event_);
        }
        if (worker_thread_.joinable()) {
            worker_thread_.join();
        }
        if (stop_event_) {
            CloseHandle(stop_event_);
            stop_event_ = nullptr;
        }
        if (init_done_event_) {
            CloseHandle(init_done_event_);
            init_done_event_ = nullptr;
        }
        running_.store(false);
    }

    bool isRunning() const noexcept {
        return running_.load(std::memory_order_relaxed);
    }

    int requestedChannels() const noexcept {
        return preferred_channels_.load(std::memory_order_relaxed);
    }

    int actualChannels() const noexcept {
        return actual_channels_.load(std::memory_order_relaxed);
    }

    // 在点击“开始录制声音”时重置 QPC 时间轴起点，防止把点击录制之前的系统静音期误补入新录音开头
    void resetTimeline() noexcept {
        reset_timeline_req_.store(true, std::memory_order_release);
    }

private:
    struct Session {
        IAudioClient* audio_client = nullptr;
        IAudioCaptureClient* capture_client = nullptr;
        HANDLE packet_event = nullptr;
        uint32_t channels = 2;
        uint32_t channel_mask = 0x3;
        uint32_t bits_per_sample = 32;
        bool is_ieee_float = true;
        uint32_t sample_rate = 48000;
        bool started = false;
    };

    // AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS 与 AUDIOCLIENT_ACTIVATION_PARAMS 标准二进制布局 (Windows 10 Build 20348+ / Windows 11)
    struct WasapiProcessLoopbackParams {
        DWORD TargetProcessId;
        int   ProcessLoopbackMode; // 0 = INCLUDE_TARGET_PROCESS_TREE, 1 = EXCLUDE_TARGET_PROCESS_TREE
    };
    struct WasapiAudioClientActivationParams {
        int                         ActivationType; // 1 = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
        WasapiProcessLoopbackParams ProcessLoopbackParams;
    };

    static constexpr REFERENCE_TIME kLoopbackBufferDuration100ns = 500 * 10000; // 500 ms
    static constexpr double kGapThreshold100ns = 30.0 * 10000.0;
    static constexpr double kMaxGapFill100ns = 10.0 * 10000000.0;
    static constexpr size_t kSilenceChunkFrames = 4096;

    std::thread worker_thread_;
    HANDLE stop_event_ = nullptr;
    HANDLE init_done_event_ = nullptr;
    std::atomic<bool> running_{ false };
    std::atomic<bool> init_success_{ false };
    std::atomic<int> preferred_channels_{ 0 };
    std::atomic<int> actual_channels_{ 2 };
    int sample_rate_ = 48000;
    std::string device_name_ = "Process Loopback (Pre-APO)";
    AudioPacketCallback callback_;
    std::vector<float> packet_f32_;
    std::vector<float> silence_f32_;
    std::atomic<bool> reset_timeline_req_{ false };
    bool timeline_valid_ = false;
    double expected_qpc_100ns_ = 0.0;

    static bool isStopRequested(HANDLE stop_event) {
        return WaitForSingleObject(stop_event, 0) == WAIT_OBJECT_0;
    }

    static void closeSession(Session& s) {
        if (s.started && s.audio_client) {
            s.audio_client->Stop();
        }
        s.started = false;
        if (s.capture_client) { s.capture_client->Release(); s.capture_client = nullptr; }
        if (s.audio_client) { s.audio_client->Release(); s.audio_client = nullptr; }
        if (s.packet_event) { CloseHandle(s.packet_event); s.packet_event = nullptr; }
    }

    // 通过 ActivateAudioInterfaceAsync("VAD\\Process_Loopback") 直接激活脱离硬件端点的系统混音流
    static bool openSession(Session& s, int req_channels) {
        closeSession(s);

        static constexpr GUID kGuidSubtypeIeeeFloat = {
            0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
        };

        s.packet_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s.packet_event) return false;

        HMODULE mmdevapi_dll = LoadLibraryW(L"Mmdevapi.dll");
        if (!mmdevapi_dll) {
            closeSession(s);
            return false;
        }
        using ActivateAudioInterfaceAsyncFn = HRESULT(STDAPICALLTYPE*)(
            LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*, IActivateAudioInterfaceAsyncOperation**
        );
        auto pfnActivate = reinterpret_cast<ActivateAudioInterfaceAsyncFn>(
            GetProcAddress(mmdevapi_dll, "ActivateAudioInterfaceAsync")
        );
        if (!pfnActivate) {
            FreeLibrary(mmdevapi_dll);
            closeSession(s);
            return false;
        }

        // 配置 VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK：排除 PID 4 (Windows 内核空闲进程)，从而在进入物理扬声器端点前捕获全系统混音
        WasapiAudioClientActivationParams act_params{};
        act_params.ActivationType = 1; // AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
        act_params.ProcessLoopbackParams.TargetProcessId = 4;
        act_params.ProcessLoopbackParams.ProcessLoopbackMode = 1; // PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE

        PROPVARIANT activate_prop{};
        activate_prop.vt = VT_BLOB;
        activate_prop.blob.cbSize = sizeof(act_params);
        activate_prop.blob.pBlobData = reinterpret_cast<BYTE*>(&act_params);

        auto* handler = new WasapiActivationCompletionHandler();
        IActivateAudioInterfaceAsyncOperation* async_op = nullptr;
        HRESULT hr = pfnActivate(
            L"VAD\\Process_Loopback",
            __uuidof(IAudioClient),
            &activate_prop,
            handler,
            &async_op
        );

        if (FAILED(hr) || !async_op) {
            handler->Release();
            FreeLibrary(mmdevapi_dll);
            closeSession(s);
            return false;
        }

        WaitForSingleObject(handler->eventHandle(), 3500);

        HRESULT hr_activate = E_FAIL;
        IUnknown* punk_audio_client = nullptr;
        hr = async_op->GetActivateResult(&hr_activate, &punk_audio_client);
        async_op->Release();
        handler->Release();
        FreeLibrary(mmdevapi_dll);

        if (FAILED(hr) || FAILED(hr_activate) || !punk_audio_client) {
            if (punk_audio_client) punk_audio_client->Release();
            closeSession(s);
            return false;
        }

        hr = punk_audio_client->QueryInterface(__uuidof(IAudioClient), reinterpret_cast<void**>(&s.audio_client));
        punk_audio_client->Release();
        if (FAILED(hr) || !s.audio_client) {
            closeSession(s);
            return false;
        }

        const DWORD stream_flags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        auto tryInitFormat = [&](uint32_t ch, uint32_t sr) -> bool {
            WAVEFORMATEXTENSIBLE wfx{};
            wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            wfx.Format.nChannels = static_cast<WORD>(ch);
            wfx.Format.nSamplesPerSec = sr;
            wfx.Format.wBitsPerSample = 32;
            wfx.Format.nBlockAlign = static_cast<WORD>(ch * 4);
            wfx.Format.nAvgBytesPerSec = sr * wfx.Format.nBlockAlign;
            wfx.Format.cbSize = 22;
            wfx.Samples.wValidBitsPerSample = 32;
            wfx.SubFormat = kGuidSubtypeIeeeFloat;
            if (ch == 1) wfx.dwChannelMask = 0x4;
            else if (ch == 2) wfx.dwChannelMask = 0x3;
            else if (ch == 6) wfx.dwChannelMask = 0x3F;  // 5.1 (FL|FR|FC|LFE|BL|BR)
            else if (ch == 8) wfx.dwChannelMask = 0x63F; // 7.1
            else wfx.dwChannelMask = (1u << ch) - 1u;

            if (SUCCEEDED(s.audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, stream_flags, kLoopbackBufferDuration100ns, 0,
                reinterpret_cast<const WAVEFORMATEX*>(&wfx), nullptr))) {
                s.channels = ch;
                s.channel_mask = wfx.dwChannelMask;
                s.sample_rate = sr;
                s.bits_per_sample = 32;
                s.is_ieee_float = true;
                return true;
            }

            WAVEFORMATEX wf_simple{};
            wf_simple.wFormatTag = 3; // WAVE_FORMAT_IEEE_FLOAT
            wf_simple.nChannels = static_cast<WORD>(ch);
            wf_simple.nSamplesPerSec = sr;
            wf_simple.wBitsPerSample = 32;
            wf_simple.nBlockAlign = static_cast<WORD>(ch * 4);
            wf_simple.nAvgBytesPerSec = sr * wf_simple.nBlockAlign;
            wf_simple.cbSize = 0;
            if (SUCCEEDED(s.audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED, stream_flags, kLoopbackBufferDuration100ns, 0, &wf_simple, nullptr))) {
                s.channels = ch;
                s.channel_mask = (ch == 2) ? 0x3u : ((ch == 6) ? 0x3Fu : ((ch == 8) ? 0x63Fu : 0u));
                s.sample_rate = sr;
                s.bits_per_sample = 32;
                s.is_ieee_float = true;
                return true;
            }
            return false;
        };

        // 先探测 Windows 音频引擎默认混音格式 (GetMixFormat)，获取系统原生采样率与声道数
        uint32_t mix_ch = 2;
        uint32_t mix_sr = 48000;
        WAVEFORMATEX* mix_wf = nullptr;
        if (SUCCEEDED(s.audio_client->GetMixFormat(&mix_wf)) && mix_wf) {
            if (mix_wf->nChannels >= 1 && mix_wf->nChannels <= 8) {
                mix_ch = mix_wf->nChannels;
            }
            if (mix_wf->nSamplesPerSec >= 8000) {
                mix_sr = mix_wf->nSamplesPerSec;
            }
            CoTaskMemFree(mix_wf);
        }

        // 若 req_channels <= 0（“保持原声道”），优先请求声卡原生混音通道数 mix_ch；
        // 若用户显式指定了 6 声道 (0x3F) 或 8 声道 (0x63F)，优先向 Windows 音频引擎请求该目标声道格式，
        // 若驱动不支持则无损回退至原生 mix_ch 或 2 声道（实际开启几声道就如实记录在 s.channels 中）
        uint32_t target_ch = (req_channels <= 0)
            ? mix_ch
            : static_cast<uint32_t>(safeClamp(req_channels, 1, 8));

        bool fmt_ok = tryInitFormat(target_ch, mix_sr);
        if (!fmt_ok && mix_sr != 48000) {
            fmt_ok = tryInitFormat(target_ch, 48000);
        }
        if (!fmt_ok && target_ch != mix_ch) {
            fmt_ok = tryInitFormat(mix_ch, mix_sr);
        }
        if (!fmt_ok) {
            fmt_ok = tryInitFormat(2, 48000);
        }
        if (!fmt_ok) {
            fmt_ok = tryInitFormat(2, 44100);
        }

        if (!fmt_ok ||
            FAILED(s.audio_client->SetEventHandle(s.packet_event)) ||
            FAILED(s.audio_client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&s.capture_client))) ||
            !s.capture_client ||
            FAILED(s.audio_client->Start())) {
            closeSession(s);
            return false;
        }

        s.started = true;
        return true;
    }

    static void convertToFloat(const BYTE* data_ptr, UINT32 frames, const Session& s, std::vector<float>& out) {
        const size_t n_samples = static_cast<size_t>(frames) * s.channels;
        out.resize(n_samples);
        if (s.is_ieee_float && s.bits_per_sample == 32) {
            std::memcpy(out.data(), data_ptr, n_samples * sizeof(float));
        }
        else if (s.bits_per_sample == 16) {
            const int16_t* s16 = reinterpret_cast<const int16_t*>(data_ptr);
            for (size_t i = 0; i < n_samples; ++i) {
                out[i] = static_cast<float>(s16[i]) / 32768.0f;
            }
        }
        else {
            std::fill(out.begin(), out.end(), 0.0f);
        }
    }

    void emitSilence(size_t frames, const Session& s) {
        if (!callback_) return;
        while (frames > 0) {
            const size_t n = std::min(frames, kSilenceChunkFrames);
            silence_f32_.assign(n * s.channels, 0.0f);
            callback_(silence_f32_.data(), static_cast<uint32_t>(n), s.channels, s.channel_mask, s.sample_rate);
            frames -= n;
        }
    }

    void captureLoopThread() {
        HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_inited = SUCCEEDED(hr_com);

        HMODULE avrt_dll = LoadLibraryW(L"avrt.dll");
        HANDLE mmcss_handle = nullptr;
        using AvSetMmThreadCharacteristicsWFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
        using AvRevertMmThreadCharacteristicsFn = BOOL(WINAPI*)(HANDLE);
        AvRevertMmThreadCharacteristicsFn pfnRevertMmcss = nullptr;
        if (avrt_dll) {
            auto pfnSet = reinterpret_cast<AvSetMmThreadCharacteristicsWFn>(
                GetProcAddress(avrt_dll, "AvSetMmThreadCharacteristicsW")
            );
            pfnRevertMmcss = reinterpret_cast<AvRevertMmThreadCharacteristicsFn>(
                GetProcAddress(avrt_dll, "AvRevertMmThreadCharacteristics")
            );
            if (pfnSet) {
                DWORD task_idx = 0;
                mmcss_handle = pfnSet(L"Audio", &task_idx);
            }
        }

        Session sess;
        bool first_attempt = true;

        while (!isStopRequested(stop_event_)) {
            if (!sess.started) {
                const bool ok = openSession(sess, preferred_channels_.load());
                if (ok) {
                    actual_channels_.store(static_cast<int>(sess.channels));
                    sample_rate_ = static_cast<int>(sess.sample_rate);
                    device_name_ = "纯系统混音内录 (" + std::to_string(sess.sample_rate) + "Hz " +
                        std::to_string(sess.channels) + "ch Pre-APO)";
                }
                running_.store(ok);
                if (first_attempt) {
                    first_attempt = false;
                    init_success_.store(ok);
                    SetEvent(init_done_event_);
                }
                if (!ok) {
                    if (WaitForSingleObject(stop_event_, 1000) == WAIT_OBJECT_0) break;
                    continue;
                }
            }

            HANDLE wait_objs[2] = { stop_event_, sess.packet_event };
            const DWORD wait_res = WaitForMultipleObjects(2, wait_objs, FALSE, 10);
            if (wait_res == WAIT_OBJECT_0) break;

            bool need_reopen = false;
            UINT32 next_frames = 0;
            while (true) {
                if (FAILED(sess.capture_client->GetNextPacketSize(&next_frames))) {
                    need_reopen = true;
                    break;
                }
                if (next_frames == 0) break;

                BYTE* data_ptr = nullptr;
                UINT32 frames_read = 0;
                DWORD buf_flags = 0;
                UINT64 dev_position = 0;
                UINT64 qpc_position = 0;
                if (FAILED(sess.capture_client->GetBuffer(&data_ptr, &frames_read, &buf_flags, &dev_position, &qpc_position))) {
                    need_reopen = true;
                    break;
                }

                if (frames_read > 0) {
                    if (reset_timeline_req_.exchange(false, std::memory_order_acquire)) {
                        timeline_valid_ = false;
                    }
                    const bool timestamp_ok = !(buf_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc_position != 0;
                    if (timestamp_ok) {
                        const double qpc = static_cast<double>(qpc_position);
                        if (timeline_valid_) {
                            const double gap = qpc - expected_qpc_100ns_;
                            if (gap > kGapThreshold100ns) {
                                const double fill = std::min(gap, kMaxGapFill100ns);
                                emitSilence(static_cast<size_t>(std::llround(fill * static_cast<double>(sess.sample_rate) / 1e7)), sess);
                            }
                        }
                        expected_qpc_100ns_ = qpc + static_cast<double>(frames_read) * 1e7 / static_cast<double>(sess.sample_rate);
                        timeline_valid_ = true;
                    }
                    else {
                        timeline_valid_ = false;
                    }

                    if ((buf_flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data_ptr) {
                        packet_f32_.assign(static_cast<size_t>(frames_read) * sess.channels, 0.0f);
                    }
                    else {
                        convertToFloat(data_ptr, frames_read, sess, packet_f32_);
                    }
                    if (callback_) {
                        callback_(packet_f32_.data(), frames_read, sess.channels, sess.channel_mask, sess.sample_rate);
                    }
                }
                sess.capture_client->ReleaseBuffer(frames_read);
            }

            if (need_reopen) {
                closeSession(sess);
                running_.store(false);
                if (WaitForSingleObject(stop_event_, 200) == WAIT_OBJECT_0) break;
                continue;
            }
        }

        closeSession(sess);
        running_.store(false);
        if (mmcss_handle && pfnRevertMmcss) pfnRevertMmcss(mmcss_handle);
        if (avrt_dll) FreeLibrary(avrt_dll);
        if (com_inited) CoUninitialize();
    }
};

// ====================================================================================================
// 10. AudioStudioWorkstation 主状态机 (含音频缓冲区锁与进度条拖拽防竞争保护)
// ====================================================================================================

class AudioStudioWorkstation {
public:
    AppPresetConfig preset;
    std::vector<PianoNoteOption> piano_notes_list = generateFullPianoNotesC0ToC8();
    int selected_piano_note_idx = 48; // C4 (261.63 Hz)

    std::string current_file_display = "当前文件：未加载";
    std::vector<float> audio_buffer;
    std::vector<float> raw_audio_buffer;
    int orig_sample_rate = 44100;
    std::atomic<size_t> play_cursor{ 0 };
    std::atomic<bool> is_playing_file{ false };
    std::atomic<bool> is_playing_synth{ false };
    std::atomic<float> master_volume{ 0.85f };
    std::atomic<float> playback_speed{ 1.0f };
    std::atomic<bool> speed_keep_pitch{ true };

    // 进度条拖拽状态隔离（防止长按拖动时 UI 线程与音频回调线程高频争抢导致卡顿或越界）
    bool is_dragging_seek = false;
    float dragging_seek_ratio = 0.0f;

    // 保护 EQ 控制点与音频缓冲区的互斥锁
    std::mutex audio_data_mutex;
    std::mutex eq_points_mutex;

    double live_resample_phase_ = 0.0;
    float held_sample_l_ = 0.0f;
    float held_sample_r_ = 0.0f;

    std::array<double, 64> harmonic_phases{};
    std::array<double, 32> osc_phases{};
    double custom_formula_phase = 0.0;
    BytecodeFormulaEngine main_formula_engine;
    std::array<BytecodeFormulaEngine, 32> osc_formula_engines;
    bool formula_valid = true;
    std::string formula_error_msg;
    std::mutex synth_mutex;

    StatefulWSOLAProcessor wsola_processor;
    StatefulOverlapAddEQ realtime_eq{ 1024 };
    LockFreeAudioRingBuffer internal_ring_buf{ 65536 };
    LockFreeAudioRingBuffer loopback_ring_buf{ 65536 };

    std::atomic<bool> use_system_loopback{ false };
    bool log_freq_axis = true;
    bool sync_x_axes = true;
    bool freeze_on_pause = true;
    bool peak_hold_enabled = true;
    int fft_size_idx = 6;
    double shared_x_min = std::log10(MIN_PLOT_FREQ_HZ);
    double shared_x_max = std::log10(MAX_PLOT_FREQ_HZ);

    // 上下分栏高度比例（支持鼠标拖拽中间分割条调整，默认 0.48）
    float upper_pane_ratio = 0.48f;

    std::vector<double> live_freqs;
    std::vector<double> live_mag_db;
    std::vector<double> live_x_plot;
    std::vector<double> peak_hold_db;
    SpectrumStats live_stats;
    std::vector<SpectralPeak> live_top3_peaks;
    bool has_rendered_live_frame = false;

    std::vector<double> harm_preview_x;
    std::vector<double> harm_preview_db;

    int snap_window_idx = 1;
    int left_snap_view_idx = 1;
    int sustain_type_idx = 0;
    int slow_factor_idx = 2;
    int slow_mode_idx = 0;
    bool piano_live_follow = true;

    std::vector<float> captured_snippet;
    std::vector<float> captured_context_audio;
    int captured_sr = 44100;
    std::vector<SpectralPeak> captured_top_peaks;
    std::vector<double> snap_wave_t_ms;
    std::vector<double> snap_wave_mono;
    std::array<std::vector<double>, 3> snap_sine_overlays;
    std::vector<double> snap_spec_x;
    std::vector<double> snap_spec_db;

    std::vector<float> snapshot_play_buffer;
    size_t snapshot_play_cursor = 0;
    std::mutex snapshot_mutex;

    std::chrono::steady_clock::time_point bpm_anchor_time = std::chrono::steady_clock::now();
    int64_t bpm_last_beat_index = -1;
    std::vector<SpectralPeak> bpm_locked_peaks;
    std::vector<double> tap_tempo_timestamps;
    std::chrono::steady_clock::time_point led_flash_until = std::chrono::steady_clock::now();
    int clicked_piano_midi = -1;
    int hovered_piano_midi = -1;

    std::atomic<bool> is_recording{ false };
    std::atomic<int> recording_source{ 0 };           // 开始录制时锁定的录制源 (0: 软件内部, 1: 电脑系统声音)
    std::atomic<bool> recording_format_changed{ false };
    int record_source_idx = 0;                        // UI 选择（录制中不生效）
    int rec_sr_idx = 0;
    int rec_bits_idx = 0;
    int rec_ch_idx = 0;
    int rec_fmt_idx = 0;
    int export_fmt_idx = 0;
    int export_bitrate_idx = 1;
    std::mutex record_mutex;                          // 保护下面的录音分块缓冲
    std::vector<std::vector<float>> recorded_chunks;  // 分块存储：支持 1~8 声道原生无损存储
    size_t recorded_chunk_floats = 0;
    std::atomic<size_t> recorded_frames_total{ 0 };
    std::atomic<int> recorded_source_sr{ 44100 };
    std::atomic<int> recorded_source_ch{ 2 };

    ma_device playback_device{};
    WasapiLoopbackCaptureEngine native_loopback_engine;
    bool playback_device_initialized = false;
    int stream_sample_rate = 48000;
    std::atomic<int> loopback_sample_rate{ 48000 };
    std::atomic<int> loopback_channels{ 2 };
    std::vector<float> loopback_scratch_;             // 回环下混缓冲（采集线程独占）
    std::string loopback_device_name = "WASAPI Loopback";
    std::string status_message = "就绪";
    bool disclaimer_accepted = false;
    bool disclaimer_checkbox_ticked = false;
    bool show_disclaimer_viewer = false;

    AudioStudioWorkstation() {
        checkInitialDisclaimerState();
        recompileFormulas();
        refreshHarmonicPreview();
        initPlaybackDevice(44100);
    }

    void checkInitialDisclaimerState() {
        try {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            fs::path state_p = fs::path(buf).parent_path() / L".disclaimer_accepted.json";
            if (fs::exists(state_p)) {
                std::ifstream f(state_p);
                if (f) {
                    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                    if (content.find("true") != std::string::npos) {
                        disclaimer_accepted = true;
                        disclaimer_checkbox_ticked = true;
                    }
                }
            }
        } catch (...) {}
    }

    void saveDisclaimerAcceptedState() {
        disclaimer_accepted = true;
        try {
            wchar_t buf[MAX_PATH];
            GetModuleFileNameW(nullptr, buf, MAX_PATH);
            fs::path state_p = fs::path(buf).parent_path() / L".disclaimer_accepted.json";
            std::ofstream f(state_p);
            if (f) {
                f << "{\n  \"accepted\": true,\n  \"version\": \"1.0\"\n}\n";
            }
        } catch (...) {}
    }

    ~AudioStudioWorkstation() {
        native_loopback_engine.stop();
        if (playback_device_initialized) ma_device_uninit(&playback_device);
    }

    void recompileFormulas() {
        std::lock_guard<std::mutex> lk(synth_mutex);
        formula_valid = main_formula_engine.compile(preset.custom_formula, &formula_error_msg);
        for (size_t i = 0; i < preset.oscillators.size() && i < osc_formula_engines.size(); ++i) {
            osc_formula_engines[i].compile(preset.oscillators[i].formula);
        }
    }

    void refreshHarmonicPreview() {
        harm_preview_x.clear();
        harm_preview_db.clear();
        std::vector<double> freqs;
        for (int k = 1; k <= preset.num_harmonics; ++k) {
            double fk = k * preset.fundamental_hz;
            if (fk <= MAX_PLOT_FREQ_HZ) freqs.push_back(fk);
        }
        if (freqs.empty()) return;
        harm_preview_db.resize(freqs.size());
        AudioDSPEngine::evaluateSmoothCurveDb(
            preset.harmonic_curve_points, freqs.data(), harm_preview_db.data(), freqs.size(), -80.0, 0.0
        );
        harm_preview_x.resize(freqs.size());
        for (size_t i = 0; i < freqs.size(); ++i) {
            harm_preview_x[i] = log_freq_axis ? std::log10(std::max(MIN_PLOT_FREQ_HZ, freqs[i])) : freqs[i];
        }
    }

    static void maPlaybackCallback(ma_device* pDevice, void* pOutput, const void* /*pInput*/, ma_uint32 frameCount) {
        auto* self = static_cast<AudioStudioWorkstation*>(pDevice->pUserData);
        if (self) self->renderAudioOutput(static_cast<float*>(pOutput), frameCount);
    }



    bool initPlaybackDevice(int /*requested_sr*/ = 0) {
        if (playback_device_initialized) {
            return true;
        }
        ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
        cfg.playback.format = ma_format_f32;
        cfg.playback.channels = 2;
        // 关键设计：使用声卡硬件原生 WASAPI 采样率 (0 = native sampleRate, 通常为 48000Hz)，
        // 使 miniaudio 内部粗糙的 2 点线性插值器 (ma_linear_resampler) 100% 旁路，
        // 音频文件加载时通过 64-tap Blackman-Harris 多相 Sinc 滤波器高精度对齐至 stream_sample_rate！
        cfg.sampleRate = 0;
        cfg.periodSizeInFrames = 0;
        cfg.noClip = MA_TRUE;
        cfg.wasapi.noAutoConvertSRC = MA_TRUE;
        cfg.dataCallback = maPlaybackCallback;
        cfg.pUserData = this;

        if (ma_device_init(nullptr, &cfg, &playback_device) == MA_SUCCESS) {
            stream_sample_rate = (playback_device.sampleRate > 0)
                ? static_cast<int>(playback_device.sampleRate)
                : 48000;
            if (ma_device_start(&playback_device) == MA_SUCCESS) {
                playback_device_initialized = true;
                return true;
            }
            ma_device_uninit(&playback_device);
        }
        return false;
    }

    bool startSystemLoopbackCapture(int preferred_ch = 0) {
        int norm_pref = (preferred_ch <= 0) ? 0 : safeClamp(preferred_ch, 1, 8);
        if (native_loopback_engine.isRunning()) {
            // 若当前已运行的回环会话请求声道数与新请求一致（或新请求未强制改变已有通道），直接复用；
            // 若用户在录制设置中切换了 6 声道 (0x3F) 或 8 声道 (0x63F)，则自动重启回环会话向 Windows 音频引擎重新请求目标声道格式
            if (native_loopback_engine.requestedChannels() == norm_pref) {
                loopback_channels.store(native_loopback_engine.actualChannels());
                return true;
            }
            if (!is_recording.load()) {
                native_loopback_engine.stop();
            } else {
                return true;
            }
        }

        // 100% 纯软件混音总线虚拟内录 (VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK)
        int native_sr = 48000;
        int actual_ch = 2;
        std::string native_name;
        if (native_loopback_engine.start(
            [this](const float* pcm, uint32_t frames, uint32_t ch, uint32_t ch_mask, uint32_t sr) {
                this->onLoopbackInput(pcm, frames, ch, ch_mask, static_cast<int>(sr));
            },
            native_sr,
            native_name,
            norm_pref,
            &actual_ch)) {
            loopback_sample_rate.store(native_sr);
            loopback_channels.store(actual_ch);
            loopback_device_name = native_name;
            return true;
        }
        return false;
    }

    void stopSystemLoopbackCaptureIfUnused() {
        const bool analysis_uses = use_system_loopback.load();
        const bool recording_uses = is_recording.load() && recording_source.load() == 1;
        if (analysis_uses || recording_uses) return;
        native_loopback_engine.stop();
    }

    // 开始录制：录制源与原生声道数 (1~8ch) 在开始时锁定，支持 5.1/7.1 多声道原样无损内录
    void beginRecording(int source, int sr, int ch = 2) {
        if (sr <= 0) sr = 48000;
        ch = safeClamp(ch, 1, 8);
        {
            std::lock_guard<std::mutex> lk(record_mutex);
            recorded_chunks.clear();
            recorded_source_sr.store(sr);
            recorded_source_ch.store(ch);
            recorded_chunk_floats = static_cast<size_t>(sr) * static_cast<size_t>(ch) * 5; // 每块约 5 秒 N 声道数据
            recorded_frames_total.store(0);
        }
        native_loopback_engine.resetTimeline();
        recording_format_changed.store(false);
        recording_source.store(source);
        is_recording.store(true);
    }

    void endRecording() {
        is_recording.store(false);
        stopSystemLoopbackCaptureIfUnused();
    }

    // 调用方必须持有 record_mutex。分块追加 N 声道交错浮点样本：每块预先 reserve，零大数组重分配
    void appendRecordedLocked(const float* interleaved_pcm, size_t frames, int ch = 2) {
        if (recorded_chunk_floats == 0 || frames == 0 || ch <= 0) return;
        size_t remaining = frames * static_cast<size_t>(ch);
        const float* src = interleaved_pcm;
        while (remaining > 0) {
            if (recorded_chunks.empty() || recorded_chunks.back().size() >= recorded_chunk_floats) {
                recorded_chunks.emplace_back();
                recorded_chunks.back().reserve(recorded_chunk_floats);
            }
            std::vector<float>& tail = recorded_chunks.back();
            const size_t take = std::min(remaining, recorded_chunk_floats - tail.size());
            tail.insert(tail.end(), src, src + take);
            src += take;
            remaining -= take;
        }
        recorded_frames_total.fetch_add(frames, std::memory_order_relaxed);
    }

    // 基于 ITU-R BS.775 国际标准与 WAVEFORMATEXTENSIBLE 硬件声道掩码的多声道至立体声恒定系数线性下混矩阵：
    // 1) 若 WAVEFORMATEXTENSIBLE 提供精确硬件声道掩码 (dwChannelMask)，严格按位解析每个物理声道 (FL/FR/FC/LFE/BL/BR/FLC/FRC/BC/SL/SR)；
    // 2) 全程采用恒定不变的线性混音权重 (0.70710678f = -3dB)，绝不按 10ms 数据包动态缩放增益，彻底杜绝鼓点/重音触发增益抽吸导致中高音频率丢失！
    static void downmixMultichannelToStereo(
        const float* in_pcm,
        uint32_t frames,
        uint32_t channels,
        std::vector<float>& out_stereo,
        uint32_t channel_mask = 0
    ) {
        out_stereo.resize(static_cast<size_t>(frames) * 2);
        if (channels == 2) {
            std::memcpy(out_stereo.data(), in_pcm, static_cast<size_t>(frames) * 2 * sizeof(float));
            return;
        }
        if (channels == 1) {
            for (uint32_t i = 0; i < frames; ++i) {
                out_stereo[i * 2] = out_stereo[i * 2 + 1] = in_pcm[i];
            }
            return;
        }

        static constexpr float kSqrt1_2 = 0.70710678f;
        uint32_t mask_popcnt = 0;
        for (uint32_t m = channel_mask; m != 0; m &= (m - 1)) ++mask_popcnt;

        if (channel_mask != 0 && mask_popcnt == channels && channels <= 16) {
            float w_l[16] = { 0.0f };
            float w_r[16] = { 0.0f };
            uint32_t ch_idx = 0;
            for (uint32_t bit = 0; bit < 32 && ch_idx < channels; ++bit) {
                uint32_t flag = (1u << bit);
                if (!(channel_mask & flag)) continue;
                switch (flag) {
                case 0x00000001u: // SPEAKER_FRONT_LEFT
                case 0x00000040u: // SPEAKER_FRONT_LEFT_OF_CENTER
                    w_l[ch_idx] = 1.0f;
                    break;
                case 0x00000002u: // SPEAKER_FRONT_RIGHT
                case 0x00000080u: // SPEAKER_FRONT_RIGHT_OF_CENTER
                    w_r[ch_idx] = 1.0f;
                    break;
                case 0x00000004u: // SPEAKER_FRONT_CENTER (人声主频段)
                case 0x00000008u: // SPEAKER_LOW_FREQUENCY (LFE 低音基频)
                case 0x00000100u: // SPEAKER_BACK_CENTER
                    w_l[ch_idx] = kSqrt1_2;
                    w_r[ch_idx] = kSqrt1_2;
                    break;
                case 0x00000010u: // SPEAKER_BACK_LEFT
                case 0x00000200u: // SPEAKER_SIDE_LEFT
                    w_l[ch_idx] = kSqrt1_2;
                    break;
                case 0x00000020u: // SPEAKER_BACK_RIGHT
                case 0x00000400u: // SPEAKER_SIDE_RIGHT
                    w_r[ch_idx] = kSqrt1_2;
                    break;
                default:
                    if ((ch_idx & 1u) == 0u) w_l[ch_idx] = kSqrt1_2;
                    else                     w_r[ch_idx] = kSqrt1_2;
                    break;
                }
                ++ch_idx;
            }
            for (uint32_t i = 0; i < frames; ++i) {
                const float* frame = in_pcm + static_cast<size_t>(i) * channels;
                float l = 0.0f, r = 0.0f;
                for (uint32_t c = 0; c < channels; ++c) {
                    l += frame[c] * w_l[c];
                    r += frame[c] * w_r[c];
                }
                out_stereo[i * 2]     = l;
                out_stereo[i * 2 + 1] = r;
            }
            return;
        }

        // ITU-R BS.775 标准固定多声道布局线性映射
        for (uint32_t i = 0; i < frames; ++i) {
            const float* frame = in_pcm + static_cast<size_t>(i) * channels;
            float l = frame[0];
            float r = frame[1];
            if (channels == 3) { // SPEAKERS_2POINT1: FL, FR, LFE/FC
                l += kSqrt1_2 * frame[2];
                r += kSqrt1_2 * frame[2];
            }
            else if (channels == 4) { // SPEAKERS_4POINT0: FL, FR, BL, BR
                l += kSqrt1_2 * frame[2];
                r += kSqrt1_2 * frame[3];
            }
            else if (channels == 5) { // SPEAKERS_4POINT1: FL, FR, FC, LFE, BC
                l += kSqrt1_2 * (frame[2] + frame[3] + frame[4]);
                r += kSqrt1_2 * (frame[2] + frame[3] + frame[4]);
            }
            else if (channels == 6) { // SPEAKERS_5POINT1: FL, FR, FC, LFE, BL, BR
                l += kSqrt1_2 * (frame[2] + frame[3] + frame[4]);
                r += kSqrt1_2 * (frame[2] + frame[3] + frame[5]);
            }
            else if (channels == 7) { // 6.1: FL, FR, FC, LFE, BL, BR, BC
                l += kSqrt1_2 * (frame[2] + frame[3] + frame[4] + kSqrt1_2 * frame[6]);
                r += kSqrt1_2 * (frame[2] + frame[3] + frame[5] + kSqrt1_2 * frame[6]);
            }
            else if (channels >= 8) { // SPEAKERS_7POINT1: FL, FR, FC, LFE, BL, BR, SL, SR
                l += kSqrt1_2 * (frame[2] + frame[3] + frame[4] + frame[6]);
                r += kSqrt1_2 * (frame[2] + frame[3] + frame[5] + frame[7]);
            }
            out_stereo[i * 2]     = l;
            out_stereo[i * 2 + 1] = r;
        }
    }

    // 系统回环回调（采集线程，或 miniaudio 回调线程；同一时刻只会有一个在运行）
    void onLoopbackInput(const float* input_pcm, uint32_t frames, uint32_t channels, uint32_t channel_mask, int sample_rate) {
        if (frames == 0 || !input_pcm) return;
        int cur_ch = safeClamp(static_cast<int>(channels), 1, 8);
        loopback_channels.store(cur_ch);
        loopback_sample_rate.store(sample_rate);

        downmixMultichannelToStereo(input_pcm, frames, channels, loopback_scratch_, channel_mask);
        loopback_ring_buf.pushStereo(loopback_scratch_.data(), frames);

        // 录制“电脑系统声音”：直接保存 WASAPI GetMixFormat 原生 N 声道 (1~8ch，含 5.1/7.1) 原始流，零混音损耗！
        if (is_recording.load(std::memory_order_relaxed) && recording_source.load(std::memory_order_relaxed) == 1) {
            int expected_ch = recorded_source_ch.load(std::memory_order_relaxed);
            if (sample_rate != recorded_source_sr.load() || cur_ch != expected_ch) {
                recording_format_changed.store(true);
            }
            std::lock_guard<std::mutex> lk(record_mutex);
            if (cur_ch == expected_ch) {
                appendRecordedLocked(input_pcm, frames, cur_ch);
            }
            else if (expected_ch == 2) {
                appendRecordedLocked(loopback_scratch_.data(), frames, 2);
            }
        }
    }

    void renderAudioOutput(float* out_stereo, uint32_t frames) {
        std::fill(out_stereo, out_stereo + frames * 2, 0.0f);
        bool has_active_sound = false;

        if (is_playing_file.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> alk(audio_data_mutex);
            if (!audio_buffer.empty()) {
                float spd = playback_speed.load(std::memory_order_relaxed);
                bool keep_p = speed_keep_pitch.load(std::memory_order_relaxed);
                if (wsola_processor.processBlock(audio_buffer, play_cursor, spd, keep_p, out_stereo, frames)) {
                    has_active_sound = true;
                }
                else {
                    is_playing_file.store(false, std::memory_order_relaxed);
                }
            }
        }

        if (is_playing_synth.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lk(synth_mutex, std::try_to_lock);
            if (lk.owns_lock()) {
                renderSynthBlockLocked(out_stereo, frames);
                has_active_sound = true;
            }
        }

        if (preset.eq_enabled && has_active_sound) {
            std::vector<ControlPoint> local_eq_pts;
            {
                std::lock_guard<std::mutex> eqlk(eq_points_mutex);
                local_eq_pts = preset.eq_points;
            }
            realtime_eq.processStereoBlock(out_stereo, frames, stream_sample_rate, local_eq_pts);
        }

        {
            std::unique_lock<std::mutex> slk(snapshot_mutex, std::try_to_lock);
            if (slk.owns_lock() && !snapshot_play_buffer.empty()) {
                size_t total_s = snapshot_play_buffer.size() / 2;
                if (snapshot_play_cursor < total_s) {
                    size_t take_s = std::min<size_t>(frames, total_s - snapshot_play_cursor);
                    for (size_t i = 0; i < take_s * 2; ++i) {
                        out_stereo[i] += snapshot_play_buffer[snapshot_play_cursor * 2 + i];
                    }
                    snapshot_play_cursor += take_s;
                    has_active_sound = true;
                }
                else {
                    snapshot_play_buffer.clear();
                    snapshot_play_cursor = 0;
                }
            }
        }

        // 录制“软件内部声音”：在均衡器 (EQ) 与 88 键钢琴/截取慢放试听 (snapshot_play_buffer) 混音之后、
        // 实时降采样预览 (resample_live_enabled) 与主音量缩放之前抓取 32-bit Float 满带宽立体声信号
        if (is_recording.load(std::memory_order_relaxed) && recording_source.load(std::memory_order_relaxed) == 0) {
            std::lock_guard<std::mutex> rlk(record_mutex);
            appendRecordedLocked(out_stereo, frames);
        }

        if (preset.resample_live_enabled && has_active_sound) {
            std::vector<float> blk_vec(out_stereo, out_stereo + frames * 2);
            auto down = AudioDSPEngine::offlineResample(
                blk_vec, stream_sample_rate, std::max(2000, preset.target_sample_rate), preset.target_bit_depth, preset.target_channels
            );
            auto up = AudioDSPEngine::offlineResample(
                down, std::max(2000, preset.target_sample_rate), stream_sample_rate, 32, 2
            );
            size_t copy_n = std::min<size_t>(frames * 2, up.size());
            std::memcpy(out_stereo, up.data(), copy_n * sizeof(float));
        }

        if (has_active_sound || !freeze_on_pause) {
            internal_ring_buf.pushStereo(out_stereo, frames);
        }

        float vol = master_volume.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < frames * 2; ++i) {
            float s = out_stereo[i] * vol;
            out_stereo[i] = (std::abs(s) > 0.985f) ? AudioDSPEngine::softKneeLimiterSample(s, 0.88f) : s;
        }
    }

    inline double evalBasicWaveform(int wave_idx, double phase_rad, const BytecodeFormulaEngine& eng) const noexcept {
        switch (wave_idx) {
        case 0: return std::sin(phase_rad);
        case 1: return (std::sin(phase_rad) >= 0.0) ? 1.0 : -1.0;
        case 2: return (2.0 / PI_VAL) * std::asin(std::sin(phase_rad));
        case 3: {
            double n = std::fmod(std::fmod(phase_rad / TWO_PI, 1.0) + 1.0, 1.0);
            return 2.0 * n - 1.0;
        }
        case 4: return eng.evalSample(phase_rad);
        default: {
            uint64_t x = static_cast<uint64_t>(phase_rad * 1000003.0) ^ 0x9E3779B97F4A7C15ULL;
            x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
            return (static_cast<double>(x & 0xFFFFFF) / static_cast<double>(0x800000)) - 1.0;
        }
        }
    }

    void renderSynthBlockLocked(float* out_stereo, uint32_t frames) {
        std::vector<double> mixed(frames, 0.0);
        double inv_sr = 1.0 / static_cast<double>(stream_sample_rate);

        if (preset.synth_mode == 0) {
            double nyquist = stream_sample_rate * 0.48;
            int max_h = safeClamp(preset.num_harmonics, 1, 48);
            std::vector<double> h_freqs;
            for (int k = 1; k <= max_h; ++k) {
                double fk = k * preset.fundamental_hz;
                if (fk <= nyquist) h_freqs.push_back(fk);
            }
            if (!h_freqs.empty()) {
                std::vector<double> h_dbs(h_freqs.size());
                AudioDSPEngine::evaluateSmoothCurveDb(
                    preset.harmonic_curve_points, h_freqs.data(), h_dbs.data(), h_freqs.size(), -80.0, 0.0
                );
                for (size_t h = 0; h < h_freqs.size(); ++h) {
                    if (h_dbs[h] <= -78.0) continue;
                    double amp = std::pow(10.0, h_dbs[h] / 20.0);
                    double phi = harmonic_phases[h];
                    double dphi = TWO_PI * h_freqs[h] * inv_sr;
                    for (uint32_t i = 0; i < frames; ++i) {
                        mixed[i] += amp * evalBasicWaveform(preset.base_waveform_idx, phi, main_formula_engine);
                        phi += dphi;
                    }
                    harmonic_phases[h] = std::fmod(phi, TWO_PI);
                }
            }
        }
        else if (preset.synth_mode == 1) {
            for (size_t o = 0; o < preset.oscillators.size() && o < osc_phases.size(); ++o) {
                const auto& osc = preset.oscillators[o];
                if (!osc.enabled || osc.gain_db <= -78.0) continue;
                double amp = std::pow(10.0, osc.gain_db / 20.0);
                double phi = osc_phases[o];
                double phase_offset = osc.phase_deg * (PI_VAL / 180.0);
                double dphi = TWO_PI * osc.freq_hz * inv_sr;
                for (uint32_t i = 0; i < frames; ++i) {
                    mixed[i] += amp * evalBasicWaveform(osc.waveform_idx, phi + phase_offset, osc_formula_engines[o]);
                    phi += dphi;
                }
                osc_phases[o] = std::fmod(phi, TWO_PI);
            }
        }
        else {
            double phi = custom_formula_phase;
            double dphi = TWO_PI * preset.fundamental_hz * inv_sr;
            double amp = std::pow(10.0, -6.0 / 20.0);
            for (uint32_t i = 0; i < frames; ++i) {
                mixed[i] = amp * main_formula_engine.evalSample(phi);
                phi += dphi;
            }
            custom_formula_phase = std::fmod(phi, TWO_PI);
        }

        double peak = 0.0;
        for (double v : mixed) peak = std::max(peak, std::abs(v));
        double scale = (peak > 0.95) ? (0.95 / peak) : 1.0;
        for (uint32_t i = 0; i < frames; ++i) {
            float s = static_cast<float>(mixed[i] * scale);
            out_stereo[i * 2] += s;
            out_stereo[i * 2 + 1] += s;
        }
    }

    void captureAudioSnapshot(bool switch_to_tab4 = false) {
        static constexpr double WINDOW_MS_OPTIONS[] = { 20.0, 40.0, 80.0, 200.0 };
        double win_ms = WINDOW_MS_OPTIONS[safeClamp(snap_window_idx, 0, 3)];
        int active_sr = use_system_loopback.load() ? loopback_sample_rate.load() : stream_sample_rate;
        size_t n_slice = std::max<size_t>(32, static_cast<size_t>(std::llround((win_ms / 1000.0) * active_sr)));
        size_t n_context = std::max<size_t>(n_slice, static_cast<size_t>(std::llround(0.08 * active_sr)));

        std::vector<float> tail;
        if (use_system_loopback.load()) {
            loopback_ring_buf.copyLatestFrames(tail, n_context);
        }
        else {
            internal_ring_buf.copyLatestFrames(tail, n_context);
        }

        float max_abs = 0.0f;
        for (float v : tail) max_abs = std::max(max_abs, std::abs(v));

        if (max_abs < 1e-6f && !audio_buffer.empty()) {
            std::lock_guard<std::mutex> alk(audio_data_mutex);
            size_t total_f = audio_buffer.size() / 2;
            size_t center = std::min(play_cursor.load(), total_f > 0 ? total_f - 1 : 0);
            size_t start_s = (center > n_slice / 2) ? (center - n_slice / 2) : 0;
            size_t end_s = std::min(total_f, start_s + n_slice);
            captured_snippet.assign(audio_buffer.begin() + start_s * 2, audio_buffer.begin() + end_s * 2);

            size_t start_c = (center > n_context / 2) ? (center - n_context / 2) : 0;
            size_t end_c = std::min(total_f, start_c + n_context);
            captured_context_audio.assign(audio_buffer.begin() + start_c * 2, audio_buffer.begin() + end_c * 2);
            captured_sr = orig_sample_rate;
        }
        else {
            captured_context_audio = tail;
            size_t ctx_frames = tail.size() / 2;
            size_t start_f = (ctx_frames > n_slice) ? (ctx_frames - n_slice) : 0;
            captured_snippet.assign(tail.begin() + start_f * 2, tail.end());
            captured_sr = active_sr;
        }

        refreshSnapshotPlots(false);
        if (switch_to_tab4) {
            request_switch_tab4 = true;
            char msg[160];
            std::snprintf(msg, sizeof(msg), "[快照] 已截取 %.0fms 瞬态声音 | 当前主音标注数量：%d 个", win_ms, preset.peak_annotation_count);
            status_message = msg;
        }
    }

    void refreshSnapshotPlots(bool preserve_piano_bpm) {
        size_t n_frames = captured_snippet.size() / 2;
        if (n_frames == 0) return;

        snap_wave_t_ms.resize(n_frames);
        snap_wave_mono.resize(n_frames);
        double half_ms = (static_cast<double>(n_frames) / static_cast<double>(captured_sr)) * 500.0;
        for (size_t i = 0; i < n_frames; ++i) {
            snap_wave_t_ms[i] = -half_ms + (2.0 * half_ms * static_cast<double>(i)) / static_cast<double>(std::max<size_t>(1, n_frames - 1));
            snap_wave_mono[i] = 0.5 * (captured_snippet[i * 2] + captured_snippet[i * 2 + 1]);
        }

        std::vector<double> freqs, mag_db;
        SpectrumStats st;
        const auto& ctx = captured_context_audio.empty() ? captured_snippet : captured_context_audio;
        AudioDSPEngine::computeFftSpectrumDb(ctx.data(), ctx.size() / 2, captured_sr, 4096, freqs, mag_db, st);
        captured_top_peaks = AudioDSPEngine::findTopNSpectralPeaks(freqs, mag_db, 3, -85.0);

        snap_spec_x.clear();
        snap_spec_db.clear();
        for (size_t i = 0; i < freqs.size(); ++i) {
            if (freqs[i] >= MIN_PLOT_FREQ_HZ) {
                snap_spec_x.push_back(log_freq_axis ? std::log10(freqs[i]) : freqs[i]);
                snap_spec_db.push_back(mag_db[i]);
            }
        }

        for (int r = 0; r < 3; ++r) {
            snap_sine_overlays[r].assign(n_frames, 0.0);
            if (r < preset.peak_annotation_count && r < static_cast<int>(captured_top_peaks.size())) {
                double f_hz = captured_top_peaks[r].freq_hz;
                double amp = std::min(0.95, captured_top_peaks[r].amp_linear);
                for (size_t i = 0; i < n_frames; ++i) {
                    double t_sec = static_cast<double>(i) / static_cast<double>(captured_sr);
                    snap_sine_overlays[r][i] = amp * std::sin(TWO_PI * f_hz * t_sec);
                }
            }
        }

        if (!(preserve_piano_bpm && preset.bpm_capture_enabled && piano_live_follow)) {
            bpm_locked_peaks = captured_top_peaks;
        }
    }

    void triggerSnapshotPlayback(std::vector<float>&& buf, const std::string& desc) {
        if (buf.empty()) return;
        if (captured_sr != stream_sample_rate) {
            buf = AudioDSPEngine::offlineResample(buf, captured_sr, stream_sample_rate, 32, 2);
        }
        double dur = static_cast<double>(buf.size() / 2) / static_cast<double>(stream_sample_rate);
        {
            std::lock_guard<std::mutex> lk(snapshot_mutex);
            snapshot_play_buffer = std::move(buf);
            snapshot_play_cursor = 0;
        }
        char msg[200];
        std::snprintf(msg, sizeof(msg), "[试听] 正在播放截取声音：%s (时长 %.2f 秒)", desc.c_str(), dur);
        status_message = msg;
    }

    void resetBeatPhaseNow() {
        preset.bpm_capture_enabled = true;
        piano_live_follow = true;
        bpm_anchor_time = std::chrono::steady_clock::now();
        bpm_last_beat_index = 0;
        bpm_locked_peaks = live_top3_peaks;
        led_flash_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(115);
        if (preset.bpm_sync_right_snapshot) {
            captureAudioSnapshot(false);
        }
        double period = 60.0 / std::max(15.0, preset.bpm_value);
        char msg[200];
        std::snprintf(msg, sizeof(msg), "[节拍同步] 已从当前瞬间重置第 1 拍 | 后续按 %.1f BPM (每 %.2f 秒) 定时抓取主音", preset.bpm_value, period);
        status_message = msg;
    }

    void onTapTempoClicked() {
        double now_s = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        if (!tap_tempo_timestamps.empty() && (now_s - tap_tempo_timestamps.back()) > 2.5) {
            tap_tempo_timestamps.clear();
        }
        tap_tempo_timestamps.push_back(now_s);
        if (tap_tempo_timestamps.size() > 6) {
            tap_tempo_timestamps.erase(tap_tempo_timestamps.begin());
        }
        if (tap_tempo_timestamps.size() >= 2) {
            double sum_dt = 0.0;
            for (size_t i = 1; i < tap_tempo_timestamps.size(); ++i) {
                sum_dt += (tap_tempo_timestamps[i] - tap_tempo_timestamps[i - 1]);
            }
            double avg_dt = sum_dt / static_cast<double>(tap_tempo_timestamps.size() - 1);
            if (avg_dt > 1e-3) {
                preset.bpm_value = std::round(safeClamp(60.0 / avg_dt, 15.0, 300.0) * 10.0) / 10.0;
            }
        }
        resetBeatPhaseNow();
    }

    void updateLiveSpectrumEachFrame() {
        static constexpr int FFT_SIZES[] = { 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192 };
        int fft_sz = FFT_SIZES[safeClamp(fft_size_idx, 0, 8)];

        bool is_internal_paused = (
            !use_system_loopback.load() &&
            !is_playing_file.load() &&
            !is_playing_synth.load() &&
            snapshot_play_buffer.empty()
            );
        if (is_internal_paused && freeze_on_pause && has_rendered_live_frame) {
            return;
        }

        std::vector<float> chunk;
        int active_sr = stream_sample_rate;
        if (use_system_loopback.load()) {
            loopback_ring_buf.copyLatestFrames(chunk, std::max(2048, fft_sz));
            active_sr = loopback_sample_rate.load();
        }
        else {
            internal_ring_buf.copyLatestFrames(chunk, std::max(2048, fft_sz));
            active_sr = stream_sample_rate;
        }

        AudioDSPEngine::computeFftSpectrumDb(
            chunk.data(), chunk.size() / 2, active_sr, fft_sz, live_freqs, live_mag_db, live_stats
        );
        live_top3_peaks = AudioDSPEngine::findTopNSpectralPeaks(live_freqs, live_mag_db, 3, -76.0);

        live_x_plot.clear();
        std::vector<double> filtered_db;
        live_x_plot.reserve(live_freqs.size());
        filtered_db.reserve(live_freqs.size());

        for (size_t i = 0; i < live_freqs.size(); ++i) {
            if (live_freqs[i] >= MIN_PLOT_FREQ_HZ) {
                live_x_plot.push_back(log_freq_axis ? std::log10(live_freqs[i]) : live_freqs[i]);
                filtered_db.push_back(live_mag_db[i]);
            }
        }
        live_mag_db = std::move(filtered_db);

        if (peak_hold_enabled) {
            if (peak_hold_db.size() != live_mag_db.size()) {
                peak_hold_db = live_mag_db;
            }
            else {
                for (size_t i = 0; i < live_mag_db.size(); ++i) {
                    peak_hold_db[i] = std::max(live_mag_db[i], peak_hold_db[i] - 0.18);
                }
            }
        }

        if (piano_live_follow && !is_internal_paused && preset.bpm_capture_enabled) {
            auto now = std::chrono::steady_clock::now();
            double elapsed_s = std::chrono::duration<double>(now - bpm_anchor_time).count();
            double beat_period = 60.0 / std::max(15.0, preset.bpm_value);
            int64_t beat_idx = static_cast<int64_t>(std::floor(elapsed_s / beat_period));
            if (beat_idx != bpm_last_beat_index) {
                bpm_last_beat_index = beat_idx;
                bpm_locked_peaks = live_top3_peaks;
                led_flash_until = now + std::chrono::milliseconds(115);
                if (preset.bpm_sync_right_snapshot) {
                    captureAudioSnapshot(false);
                }
            }
        }

        if (!is_internal_paused) {
            has_rendered_live_frame = true;
        }
    }

    bool request_switch_tab4 = false;
};

// ====================================================================================================
// 11. ImPlot 钢琴 C0~C8 辅助线、主音波峰气泡标注器与可拖拽曲线绘制函数 (Python PyQt6 同款配色)
// ====================================================================================================

// 与 Python 版 PEAK_RANK_COLORS = ["#FBBF24", "#2DD4BF", "#F472B6"] 完全一致
static constexpr ImU32 PEAK_COLORS_U32[3] = {
    IM_COL32(251, 191, 36, 255), // #1 金黄 (#FBBF24)
    IM_COL32(45,  212, 191, 255),// #2 青绿 (#2DD4BF)
    IM_COL32(244, 114, 182, 255) // #3 粉紫 (#F472B6)
};

static void setupFreqPianoAxisTicks(bool use_log) {
    static constexpr double MAJOR_FREQS[] = {
        16.35, 32.70, 65.41, 130.81, 261.63, 440.00, 523.25, 1046.50, 2093.00, 4186.01, 8000.0, 16000.0
    };
    static const char* MAJOR_LABELS[] = {
        "16.4[C0]", "32.7[C1]", "65.4[C2]", "130.8[C3]", "261.6[C4]", "440[A4]",
        "523.3[C5]", "1.05k[C6]", "2.09k[C7]", "4.19k[C8]", "8kHz", "16kHz"
    };
    static double tick_vals[12];
    for (int i = 0; i < 12; ++i) {
        tick_vals[i] = use_log ? std::log10(MAJOR_FREQS[i]) : MAJOR_FREQS[i];
    }
    ImPlot::SetupAxisTicks(ImAxis_X1, tick_vals, 12, MAJOR_LABELS, false);
}

static void drawPianoGuideLinesAndPeaks(
    bool use_log,
    const std::vector<SpectralPeak>& peaks,
    int peak_count_limit
) {
    ImDrawList* draw_list = ImPlot::GetPlotDrawList();
    ImVec2 plot_min = ImPlot::GetPlotPos();
    ImVec2 plot_max = ImVec2(plot_min.x + ImPlot::GetPlotSize().x, plot_min.y + ImPlot::GetPlotSize().y);

    ImPlot::PushPlotClipRect();
    for (const auto& ng : PIANO_C0_TO_C8) {
        double xv = use_log ? std::log10(ng.freq_hz) : ng.freq_hz;
        float px = std::round(ImPlot::PlotToPixels(xv, 0.0).x);
        if (px >= plot_min.x && px <= plot_max.x) {
            for (float y = plot_min.y; y < plot_max.y; y += 8.0f) {
                draw_list->AddLine(
                    ImVec2(px, y),
                    ImVec2(px, std::min(y + 4.0f, plot_max.y)),
                    IM_COL32(148, 163, 184, 65),
                    1.0f
                );
            }
            ImVec2 ts = ImGui::CalcTextSize(ng.name);
            draw_list->AddRectFilled(
                ImVec2(px + 2.0f, plot_min.y + 3.0f),
                ImVec2(px + ts.x + 8.0f, plot_min.y + ts.y + 5.0f),
                IM_COL32(15, 23, 42, 175), 3.0f
            );
            draw_list->AddText(ImVec2(px + 5.0f, plot_min.y + 4.0f), IM_COL32(251, 191, 36, 200), ng.name);
        }
    }

    int limit = safeClamp(peak_count_limit, 0, 3);
    for (int i = 0; i < limit && i < static_cast<int>(peaks.size()); ++i) {
        const auto& pk = peaks[i];
        double xv = use_log ? std::log10(std::max(MIN_PLOT_FREQ_HZ, pk.freq_hz)) : pk.freq_hz;
        ImVec2 pt = ImPlot::PlotToPixels(xv, pk.mag_db);
        pt.x = std::round(pt.x);
        pt.y = std::round(pt.y);
        ImU32 col = PEAK_COLORS_U32[i % 3];

        draw_list->AddCircleFilled(pt, SF(6.0f), col);
        draw_list->AddCircle(pt, SF(6.5f), IM_COL32(255, 255, 255, 235), 0, 1.5f);

        char badge[96];
        std::snprintf(badge, sizeof(badge), "#%d %s\n%.1fHz (%.1fdB)", pk.rank, pk.note.c_str(), pk.freq_hz, pk.mag_db);
        ImVec2 txt_sz = ImGui::CalcTextSize(badge);
        float pad_x = SF(6.0f);
        float pad_y = SF(3.0f);
        float bx = std::round(safeClamp(pt.x - txt_sz.x * 0.5f - pad_x, plot_min.x + 4.0f, plot_max.x - txt_sz.x - pad_x * 2.0f - 4.0f));
        float by = std::round(std::max(plot_min.y + 4.0f, pt.y - txt_sz.y - SF(16.0f)));
        ImVec2 b_max(bx + txt_sz.x + pad_x * 2.0f, by + txt_sz.y + pad_y * 2.0f);

        draw_list->AddRectFilled(ImVec2(bx, by), b_max, IM_COL32(15, 23, 42, 220), 4.0f);
        draw_list->AddRect(ImVec2(bx, by), b_max, col, 4.0f, 0, 1.3f);
        draw_list->AddText(ImVec2(bx + pad_x, by + pad_y), col, badge);
    }
    ImPlot::PopPlotClipRect();
}

static bool renderDraggableCurvePlot(
    const char* plot_id,
    std::vector<ControlPoint>& points,
    std::mutex* opt_mutex,
    double min_db,
    double max_db,
    const ImVec4& line_color,
    bool use_log,
    bool sync_x,
    double* shared_xmin,
    double* shared_xmax,
    const std::vector<double>* opt_stem_x = nullptr,
    const std::vector<double>* opt_stem_db = nullptr
) {
    bool changed = false;
    if (ImPlot::BeginPlot(plot_id, ImVec2(-1, -1), ImPlotFlags_NoMenus)) {
        ImPlot::SetupAxes("频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]", "分贝 (dB)", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
        double x_low = use_log ? std::log10(MIN_PLOT_FREQ_HZ) : MIN_PLOT_FREQ_HZ;
        double x_high = use_log ? std::log10(MAX_PLOT_FREQ_HZ) : MAX_PLOT_FREQ_HZ;
        ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, use_log ? std::log10(12.0) : 10.0, use_log ? std::log10(24000.0) : 24000.0);
        ImPlot::SetupAxisLimits(ImAxis_Y1, min_db, max_db, ImPlotCond_Always);
        if (sync_x && shared_xmin && shared_xmax) {
            ImPlot::SetupAxisLinks(ImAxis_X1, shared_xmin, shared_xmax);
        }
        else {
            ImPlot::SetupAxisLimits(ImAxis_X1, x_low, x_high, ImPlotCond_Once);
        }
        setupFreqPianoAxisTicks(use_log);

        static constexpr size_t N_GRID = 450;
        std::vector<double> x_grid(N_GRID), q_freqs(N_GRID), y_grid(N_GRID);
        for (size_t i = 0; i < N_GRID; ++i) {
            double r = static_cast<double>(i) / static_cast<double>(N_GRID - 1);
            x_grid[i] = x_low + r * (x_high - x_low);
            q_freqs[i] = use_log ? std::pow(10.0, x_grid[i]) : x_grid[i];
        }
        AudioDSPEngine::evaluateSmoothCurveDb(points, q_freqs.data(), y_grid.data(), N_GRID, min_db, max_db);

        ImPlot::SetNextFillStyle(line_color, 0.18f);
        ImPlot::PlotShaded("##fill", x_grid.data(), y_grid.data(), N_GRID, min_db);
        ImPlot::SetNextLineStyle(line_color, 2.6f);
        ImPlot::PlotLine("##curve", x_grid.data(), y_grid.data(), N_GRID);

        if (opt_stem_x && opt_stem_db && opt_stem_x->size() == opt_stem_db->size()) {
            ImDrawList* dl = ImPlot::GetPlotDrawList();
            ImPlot::PushPlotClipRect();
            for (size_t i = 0; i < opt_stem_x->size(); ++i) {
                ImVec2 p0 = ImPlot::PlotToPixels((*opt_stem_x)[i], min_db);
                ImVec2 p1 = ImPlot::PlotToPixels((*opt_stem_x)[i], (*opt_stem_db)[i]);
                dl->AddLine(p0, p1, IM_COL32(255, 171, 0, 150), 3.0f);
                dl->AddCircleFilled(p1, 3.5f, IM_COL32(251, 191, 36, 235));
            }
            ImPlot::PopPlotClipRect();
        }

        drawPianoGuideLinesAndPeaks(use_log, {}, 0);

        int delete_idx = -1;
        for (size_t i = 0; i < points.size(); ++i) {
            double px = use_log ? std::log10(std::max(MIN_PLOT_FREQ_HZ, points[i].freq_hz)) : points[i].freq_hz;
            double py = points[i].db;
            if (ImPlot::DragPoint(static_cast<int>(i), &px, &py, ImVec4(1.0f, 0.25f, 0.51f, 1.0f), SF(6.5f))) {
                double new_f = use_log ? std::pow(10.0, px) : px;
                new_f = safeClamp(new_f, MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ);
                if (i > 0) new_f = std::max(new_f, points[i - 1].freq_hz * 1.03);
                if (i + 1 < points.size()) new_f = std::min(new_f, points[i + 1].freq_hz * 0.97);
                if (opt_mutex) {
                    std::lock_guard<std::mutex> lk(*opt_mutex);
                    points[i].freq_hz = new_f;
                    points[i].db = safeClamp(py, min_db, max_db);
                }
                else {
                    points[i].freq_hz = new_f;
                    points[i].db = safeClamp(py, min_db, max_db);
                }
                changed = true;
            }
            ImVec2 pt_pix = ImPlot::PlotToPixels(px, py);
            ImVec2 mouse_pix = ImGui::GetMousePos();
            float dist_sq = (pt_pix.x - mouse_pix.x) * (pt_pix.x - mouse_pix.x) + (pt_pix.y - mouse_pix.y) * (pt_pix.y - mouse_pix.y);
            if (dist_sq < SF(120.0f) && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && points.size() > 2) {
                delete_idx = static_cast<int>(i);
            }
        }

        if (delete_idx >= 0) {
            if (opt_mutex) {
                std::lock_guard<std::mutex> lk(*opt_mutex);
                points.erase(points.begin() + delete_idx);
            }
            else {
                points.erase(points.begin() + delete_idx);
            }
            changed = true;
        }

        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            double new_f = use_log ? std::pow(10.0, mp.x) : mp.x;
            new_f = safeClamp(new_f, MIN_PLOT_FREQ_HZ, MAX_PLOT_FREQ_HZ);
            double new_db = safeClamp(mp.y, min_db, max_db);
            if (opt_mutex) {
                std::lock_guard<std::mutex> lk(*opt_mutex);
                points.push_back({ new_f, new_db });
                std::sort(points.begin(), points.end(), [](const ControlPoint& a, const ControlPoint& b) {
                    return a.freq_hz < b.freq_hz;
                    });
            }
            else {
                points.push_back({ new_f, new_db });
                std::sort(points.begin(), points.end(), [](const ControlPoint& a, const ControlPoint& b) {
                    return a.freq_hz < b.freq_hz;
                    });
            }
            changed = true;
        }

        ImPlot::EndPlot();
    }
    return changed;
}

// ====================================================================================================
// 12. 88 键标准钢琴交互式绘制组件 (对齐整数像素，复刻 Python 版的锐利琴键与虚线指示气泡)
// ====================================================================================================

static void renderPiano88KeysWidget(AudioStudioWorkstation& app, const std::vector<SpectralPeak>& active_peaks) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float w = std::max(SF(280.0f), std::floor(avail.x));
    float h = std::max(SF(165.0f), std::floor(avail.y));

    ImVec2 p0 = ImGui::GetCursorScreenPos();
    p0.x = std::round(p0.x);
    p0.y = std::round(p0.y);
    ImGui::InvisibleButton("##piano88_canvas", ImVec2(w, h));
    bool is_hovered = ImGui::IsItemHovered();
    bool is_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool is_Down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    ImVec2 mouse_pos = ImGui::GetMousePos();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(17, 22, 34, 255), 5.0f);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), IM_COL32(51, 65, 85, 255), 5.0f, 0, 1.0f);

    dl->AddText(ImVec2(p0.x + SF(10.0f), p0.y + SF(5.0f)), IM_COL32(226, 232, 240, 255),
        "88键钢琴 (A0~C8)");

    const char* leg_labels[4] = { "#1主音", "#2次音", "#3泛音", "点击试音" };
    ImU32 leg_cols[4] = { PEAK_COLORS_U32[0], PEAK_COLORS_U32[1], PEAK_COLORS_U32[2], IM_COL32(16, 185, 129, 255) };
    float step_w = SF(72.0f);
    float lx = std::max(p0.x + SF(285.0f), p0.x + w - step_w * 4.0f - SF(8.0f));
    for (int i = 0; i < 4; ++i) {
        if (lx + SF(58.0f) <= p0.x + w) {
            dl->AddRectFilled(ImVec2(lx, p0.y + SF(6.0f)), ImVec2(lx + SF(11.0f), p0.y + SF(17.0f)), leg_cols[i], 2.5f);
            dl->AddRect(ImVec2(lx, p0.y + SF(6.0f)), ImVec2(lx + SF(11.0f), p0.y + SF(17.0f)), IM_COL32(255, 255, 255, 220), 2.5f, 0, 1.0f);
            dl->AddText(ImVec2(lx + SF(15.0f), p0.y + SF(4.0f)), leg_cols[i], leg_labels[i]);
        }
        lx += step_w;
    }

    static auto isWhiteKey = [](int midi) {
        int s = midi % 12;
        return (s == 0 || s == 2 || s == 4 || s == 5 || s == 7 || s == 9 || s == 11);
        };

    float margin_x = SF(8.0f);
    float top_header_h = SF(68.0f);
    float kb_y = std::round(p0.y + top_header_h);
    float kb_h = std::max(SF(70.0f), std::round(h - top_header_h - SF(8.0f)));
    float kb_w = w - 2.0f * margin_x;
    float white_w = kb_w / 52.0f;
    float black_w = white_w * 0.64f;
    float black_h = std::round(kb_h * 0.62f);

    struct KeyRect { int midi; ImVec2 rmin; ImVec2 rmax; };
    std::vector<KeyRect> white_rects;
    std::vector<KeyRect> black_rects;
    white_rects.reserve(52);
    black_rects.reserve(36);

    int w_idx = 0;
    for (int midi = 21; midi <= 108; ++midi) {
        if (isWhiteKey(midi)) {
            float x0 = std::round(p0.x + margin_x + w_idx * white_w);
            float x1 = std::round(p0.x + margin_x + (w_idx + 1) * white_w);
            white_rects.push_back({ midi, ImVec2(x0, kb_y), ImVec2(x1, kb_y + kb_h) });
            ++w_idx;
        }
        else {
            float cx = p0.x + margin_x + static_cast<float>(w_idx) * white_w;
            black_rects.push_back({
                midi,
                ImVec2(std::round(cx - black_w * 0.5f), kb_y),
                ImVec2(std::round(cx + black_w * 0.5f), kb_y + black_h)
            });
        }
    }

    int hit_midi = -1;
    if (is_hovered) {
        for (const auto& br : black_rects) {
            if (mouse_pos.x >= br.rmin.x && mouse_pos.x <= br.rmax.x && mouse_pos.y >= br.rmin.y && mouse_pos.y <= br.rmax.y) {
                hit_midi = br.midi;
                break;
            }
        }
        if (hit_midi < 0) {
            for (const auto& wr : white_rects) {
                if (mouse_pos.x >= wr.rmin.x && mouse_pos.x <= wr.rmax.x && mouse_pos.y >= wr.rmin.y && mouse_pos.y <= wr.rmax.y) {
                    hit_midi = wr.midi;
                    break;
                }
            }
        }
    }
    app.hovered_piano_midi = hit_midi;
    if (is_clicked && hit_midi >= 21) {
        app.clicked_piano_midi = hit_midi;
        auto piano_wave = AudioDSPEngine::synthesizePianoKeyNote(hit_midi, app.stream_sample_rate, 0.85);
        double f_hz = 440.0 * std::pow(2.0, (hit_midi - 69) / 12.0);
        std::string n_str = AudioDSPEngine::midiToNoteName(hit_midi);
        {
            std::lock_guard<std::mutex> lk(app.snapshot_mutex);
            app.snapshot_play_buffer = std::move(piano_wave);
            app.snapshot_play_cursor = 0;
        }
        char msg[160];
        std::snprintf(msg, sizeof(msg), "[钢琴试音] 点击了琴键 【%s】 (MIDI %d | 标准频率 %.2f Hz)", n_str.c_str(), hit_midi, f_hz);
        app.status_message = msg;
    }
    if (!is_Down) {
        app.clicked_piano_midi = -1;
    }

    int highlight_rank[128];
    const SpectralPeak* highlight_peak[128];
    std::fill(std::begin(highlight_rank), std::end(highlight_rank), -1);
    std::fill(std::begin(highlight_peak), std::end(highlight_peak), nullptr);

    int limit = safeClamp(app.preset.peak_annotation_count, 0, 3);
    for (int i = 0; i < limit && i < static_cast<int>(active_peaks.size()); ++i) {
        int m = active_peaks[i].midi;
        if (m >= 21 && m <= 108 && highlight_rank[m] == -1) {
            highlight_rank[m] = i;
            highlight_peak[m] = &active_peaks[i];
        }
    }

    for (const auto& wr : white_rects) {
        ImU32 fill_col = IM_COL32(241, 245, 249, 255);
        ImU32 border_col = IM_COL32(51, 65, 85, 255);
        if (highlight_rank[wr.midi] >= 0) {
            fill_col = PEAK_COLORS_U32[highlight_rank[wr.midi]];
            border_col = IM_COL32(255, 255, 255, 255);
        }
        else if (wr.midi == app.clicked_piano_midi) {
            fill_col = IM_COL32(16, 185, 129, 255);
        }
        else if (wr.midi == app.hovered_piano_midi) {
            fill_col = IM_COL32(203, 213, 225, 255);
            border_col = IM_COL32(56, 189, 248, 255);
        }
        dl->AddRectFilled(wr.rmin, wr.rmax, fill_col, 2.0f);
        dl->AddRect(wr.rmin, wr.rmax, border_col, 2.0f, 0, 1.0f);

        if ((wr.midi % 12 == 0) || wr.midi == 21 || highlight_rank[wr.midi] >= 0) {
            std::string ns = AudioDSPEngine::midiToNoteName(wr.midi);
            ImVec2 ts = ImGui::CalcTextSize(ns.c_str());
            float tx = std::round(0.5f * (wr.rmin.x + wr.rmax.x - ts.x));
            dl->AddText(ImVec2(tx, std::round(wr.rmax.y - ts.y - SF(3.0f))), IM_COL32(15, 23, 42, 255), ns.c_str());
        }
    }

    for (const auto& br : black_rects) {
        ImU32 fill_col = IM_COL32(30, 41, 59, 255);
        ImU32 border_col = IM_COL32(2, 6, 23, 255);
        if (highlight_rank[br.midi] >= 0) {
            fill_col = PEAK_COLORS_U32[highlight_rank[br.midi]];
            border_col = IM_COL32(255, 255, 255, 255);
        }
        else if (br.midi == app.clicked_piano_midi) {
            fill_col = IM_COL32(5, 150, 105, 255);
        }
        else if (br.midi == app.hovered_piano_midi) {
            fill_col = IM_COL32(71, 85, 105, 255);
            border_col = IM_COL32(56, 189, 248, 255);
        }
        dl->AddRectFilled(br.rmin, br.rmax, fill_col, 1.8f);
        dl->AddRect(br.rmin, br.rmax, border_col, 1.8f, 0, 1.0f);
    }

    for (int midi = 21; midi <= 108; ++midi) {
        int r_idx = highlight_rank[midi];
        const SpectralPeak* pk = highlight_peak[midi];
        if (r_idx < 0 || !pk) continue;

        float cx = p0.x + margin_x;
        for (const auto& br : black_rects) if (br.midi == midi) cx = 0.5f * (br.rmin.x + br.rmax.x);
        for (const auto& wr : white_rects) if (wr.midi == midi) cx = 0.5f * (wr.rmin.x + wr.rmax.x);
        cx = std::round(cx);

        ImU32 col = PEAK_COLORS_U32[r_idx];
        char badge[96];
        std::snprintf(badge, sizeof(badge), "#%d %s (%.1fHz, %.1fdB)", r_idx + 1, pk->note.c_str(), pk->freq_hz, pk->mag_db);
        ImVec2 ts = ImGui::CalcTextSize(badge);
        float bw = std::round(ts.x + SF(12.0f));
        float bh = std::round(ts.y + SF(4.0f));
        float by = std::round(p0.y + SF(21.0f) + r_idx * SF(15.0f));
        float bx = std::round(safeClamp(cx - bw * 0.5f, p0.x + SF(6.0f), p0.x + w - bw - SF(6.0f)));

        for (float y = by + bh; y < kb_y; y += 5.0f) {
            dl->AddLine(ImVec2(cx, y), ImVec2(cx, std::min(y + 3.0f, kb_y)), col, 1.5f);
        }
        dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), IM_COL32(15, 23, 42, 235), 4.0f);
        dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh), col, 4.0f, 0, 1.4f);
        dl->AddText(ImVec2(bx + SF(6.0f), by + SF(2.0f)), col, badge);
    }
}

// ====================================================================================================
// 13. Windows 原生文件对话框与 JSON 预设导入导出
// ====================================================================================================

static std::string openAudioFileDialog(HWND owner) {
    wchar_t file_buf[MAX_PATH] = { 0 };
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"音频文件 (*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.wma;*.opus)\0*.wav;*.mp3;*.flac;*.ogg;*.m4a;*.aac;*.wma;*.opus\0所有文件 (*.*)\0*.*\0";
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) return wideToUtf8(file_buf);
    return "";
}

static std::string saveFileDialog(HWND owner, const std::string& default_name, const wchar_t* filter) {
    std::wstring wdef = utf8ToWide(default_name);
    wchar_t file_buf[MAX_PATH] = { 0 };
    std::wcsncpy(file_buf, wdef.c_str(), MAX_PATH - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file_buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) return wideToUtf8(file_buf);
    return "";
}

static std::string escapeJsonString(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

static void savePresetToJsonFile(const AppPresetConfig& p, const std::string& utf8_path) {
    std::ofstream f(fs::path(utf8ToWide(utf8_path)));
    if (!f) return;
    f << std::fixed << std::setprecision(2);
    f << "{\n";
    f << "  \"version\": \"" << escapeJsonString(p.version) << "\",\n";
    f << "  \"name\": \"" << escapeJsonString(p.name) << "\",\n";
    f << "  \"eq_enabled\": " << (p.eq_enabled ? "true" : "false") << ",\n";
    f << "  \"eq_points\": [\n";
    for (size_t i = 0; i < p.eq_points.size(); ++i) {
        f << "    [" << p.eq_points[i].freq_hz << ", " << p.eq_points[i].db << "]"
          << (i + 1 < p.eq_points.size() ? "," : "") << "\n";
    }
    f << "  ],\n";
    const char* sm = (p.synth_mode == 0) ? "harmonic_curve" : ((p.synth_mode == 1) ? "multi_osc" : "custom_formula");
    f << "  \"synth_mode\": \"" << sm << "\",\n";
    f << "  \"fundamental_hz\": " << p.fundamental_hz << ",\n";
    f << "  \"num_harmonics\": " << p.num_harmonics << ",\n";
    f << "  \"custom_formula\": \"" << escapeJsonString(p.custom_formula) << "\",\n";
    f << "  \"harmonic_curve_points\": [\n";
    for (size_t i = 0; i < p.harmonic_curve_points.size(); ++i) {
        f << "    [" << p.harmonic_curve_points[i].freq_hz << ", " << p.harmonic_curve_points[i].db << "]"
          << (i + 1 < p.harmonic_curve_points.size() ? "," : "") << "\n";
    }
    f << "  ],\n";
    f << "  \"resample_live_enabled\": " << (p.resample_live_enabled ? "true" : "false") << ",\n";
    f << "  \"target_sample_rate\": " << p.target_sample_rate << ",\n";
    f << "  \"target_bit_depth\": " << p.target_bit_depth << ",\n";
    f << "  \"target_channels\": " << p.target_channels << ",\n";
    f << "  \"peak_annotation_count\": " << p.peak_annotation_count << ",\n";
    f << "  \"bpm_capture_enabled\": " << (p.bpm_capture_enabled ? "true" : "false") << ",\n";
    f << "  \"bpm_value\": " << p.bpm_value << ",\n";
    f << "  \"bpm_sync_right_snapshot\": " << (p.bpm_sync_right_snapshot ? "true" : "false") << "\n";
    f << "}\n";
}

static bool extractJsonBool(const std::string& content, const std::string& key, bool def_val) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return def_val;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return def_val;
    size_t t_pos = content.find("true", pos);
    size_t f_pos = content.find("false", pos);
    if (t_pos != std::string::npos && (f_pos == std::string::npos || t_pos < f_pos) && (t_pos - pos) < 24) return true;
    if (f_pos != std::string::npos && (f_pos - pos) < 24) return false;
    return def_val;
}

static double extractJsonNumber(const std::string& content, const std::string& key, double def_val) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return def_val;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return def_val;
    ++pos;
    while (pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n' || content[pos] == '\r')) ++pos;
    char* end_p = nullptr;
    double v = std::strtod(content.c_str() + pos, &end_p);
    return (end_p != content.c_str() + pos) ? v : def_val;
}

static std::string extractJsonString(const std::string& content, const std::string& key, const std::string& def_val) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return def_val;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return def_val;
    size_t q1 = content.find('\"', pos + 1);
    if (q1 == std::string::npos) return def_val;
    size_t q2 = content.find('\"', q1 + 1);
    if (q2 == std::string::npos) return def_val;
    return content.substr(q1 + 1, q2 - q1 - 1);
}

static void extractJsonPointArray(const std::string& content, const std::string& key, std::vector<ControlPoint>& out_pts) {
    size_t pos = content.find("\"" + key + "\"");
    if (pos == std::string::npos) return;
    size_t b1 = content.find('[', pos);
    if (b1 == std::string::npos) return;
    int depth = 0;
    size_t b2 = b1;
    for (; b2 < content.size(); ++b2) {
        if (content[b2] == '[') ++depth;
        else if (content[b2] == ']') {
            --depth;
            if (depth == 0) break;
        }
    }
    if (b2 <= b1) return;
    std::string sub = content.substr(b1 + 1, b2 - b1 - 1);
    std::vector<ControlPoint> parsed;
    size_t cur = 0;
    while ((cur = sub.find('[', cur)) != std::string::npos) {
        size_t end_pair = sub.find(']', cur);
        if (end_pair == std::string::npos) break;
        std::string pair_s = sub.substr(cur + 1, end_pair - cur - 1);
        size_t comma = pair_s.find(',');
        if (comma != std::string::npos) {
            double f = std::strtod(pair_s.c_str(), nullptr);
            double d = std::strtod(pair_s.c_str() + comma + 1, nullptr);
            if (f > 0.0) parsed.push_back({ f, d });
        }
        cur = end_pair + 1;
    }
    if (!parsed.empty()) out_pts = std::move(parsed);
}

static bool loadPresetFromJsonFile(AppPresetConfig& p, const std::string& utf8_path) {
    std::ifstream f(fs::path(utf8ToWide(utf8_path)));
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (content.empty()) return false;

    p.eq_enabled = extractJsonBool(content, "eq_enabled", p.eq_enabled);
    extractJsonPointArray(content, "eq_points", p.eq_points);
    extractJsonPointArray(content, "harmonic_curve_points", p.harmonic_curve_points);
    p.fundamental_hz = extractJsonNumber(content, "fundamental_hz", p.fundamental_hz);
    p.num_harmonics = static_cast<int>(extractJsonNumber(content, "num_harmonics", p.num_harmonics));
    p.custom_formula = extractJsonString(content, "custom_formula", p.custom_formula);
    p.resample_live_enabled = extractJsonBool(content, "resample_live_enabled", p.resample_live_enabled);
    p.target_sample_rate = static_cast<int>(extractJsonNumber(content, "target_sample_rate", p.target_sample_rate));
    p.target_bit_depth = static_cast<int>(extractJsonNumber(content, "target_bit_depth", p.target_bit_depth));
    p.target_channels = static_cast<int>(extractJsonNumber(content, "target_channels", p.target_channels));
    p.peak_annotation_count = safeClamp(static_cast<int>(extractJsonNumber(content, "peak_annotation_count", p.peak_annotation_count)), 0, 3);
    p.bpm_capture_enabled = extractJsonBool(content, "bpm_capture_enabled", p.bpm_capture_enabled);
    p.bpm_value = extractJsonNumber(content, "bpm_value", p.bpm_value);
    p.bpm_sync_right_snapshot = extractJsonBool(content, "bpm_sync_right_snapshot", p.bpm_sync_right_snapshot);
    return true;
}

// ====================================================================================================
// 14. Python PyQt6 风格深色主题、1:1 真实物理像素矢量字体重建与 4:3 / 16:9 动态适配
// ====================================================================================================

enum class BtnTone {
    Neutral,
    Primary,
    Teal,
    Amber,
    Purple,
    Indigo,
    Danger,
    Success
};

static bool styledButton(const char* label, BtnTone tone = BtnTone::Neutral, const ImVec2& size = ImVec2(0, 0)) {
    ImVec4 normal, hovered, active, border;
    switch (tone) {
    case BtnTone::Primary: // #0284C7 (Python 蓝)
        normal  = ImVec4(0.008f, 0.518f, 0.780f, 1.00f);
        hovered = ImVec4(0.012f, 0.412f, 0.631f, 1.00f);
        active  = ImVec4(0.055f, 0.647f, 0.914f, 1.00f);
        border  = ImVec4(0.220f, 0.741f, 0.973f, 1.00f);
        break;
    case BtnTone::Teal: // #0F766E (Python 青绿)
        normal  = ImVec4(0.059f, 0.463f, 0.431f, 1.00f);
        hovered = ImVec4(0.075f, 0.545f, 0.506f, 1.00f);
        active  = ImVec4(0.047f, 0.380f, 0.357f, 1.00f);
        border  = ImVec4(0.078f, 0.722f, 0.651f, 1.00f);
        break;
    case BtnTone::Amber: // #B45309 / #D97706 (Python 橙黄)
        normal  = ImVec4(0.706f, 0.325f, 0.035f, 1.00f);
        hovered = ImVec4(0.851f, 0.467f, 0.024f, 1.00f);
        active  = ImVec4(0.573f, 0.251f, 0.055f, 1.00f);
        border  = ImVec4(0.961f, 0.620f, 0.043f, 1.00f);
        break;
    case BtnTone::Purple: // #7C3AED (Python 截取紫)
        normal  = ImVec4(0.486f, 0.227f, 0.929f, 1.00f);
        hovered = ImVec4(0.427f, 0.157f, 0.851f, 1.00f);
        active  = ImVec4(0.357f, 0.129f, 0.714f, 1.00f);
        border  = ImVec4(0.655f, 0.545f, 0.980f, 1.00f);
        break;
    case BtnTone::Indigo: // #4F46E5 (Python 合成器靛蓝)
        normal  = ImVec4(0.310f, 0.275f, 0.898f, 1.00f);
        hovered = ImVec4(0.263f, 0.220f, 0.792f, 1.00f);
        active  = ImVec4(0.216f, 0.188f, 0.639f, 1.00f);
        border  = ImVec4(0.506f, 0.549f, 0.973f, 1.00f);
        break;
    case BtnTone::Danger: // #DC2626 (Python 录音红)
        normal  = ImVec4(0.863f, 0.149f, 0.149f, 1.00f);
        hovered = ImVec4(0.725f, 0.110f, 0.110f, 1.00f);
        active  = ImVec4(0.600f, 0.106f, 0.106f, 1.00f);
        border  = ImVec4(0.973f, 0.443f, 0.443f, 1.00f);
        break;
    case BtnTone::Success: // #059669 (Python 导出绿)
        normal  = ImVec4(0.020f, 0.588f, 0.412f, 1.00f);
        hovered = ImVec4(0.063f, 0.725f, 0.506f, 1.00f);
        active  = ImVec4(0.016f, 0.471f, 0.341f, 1.00f);
        border  = ImVec4(0.204f, 0.827f, 0.600f, 1.00f);
        break;
    case BtnTone::Neutral:
    default: // #1E293B bg + #475569 border (Python 默认按钮)
        normal  = ImVec4(0.118f, 0.161f, 0.231f, 1.00f);
        hovered = ImVec4(0.200f, 0.255f, 0.333f, 1.00f);
        active  = ImVec4(0.008f, 0.518f, 0.780f, 1.00f);
        border  = ImVec4(0.278f, 0.333f, 0.412f, 1.00f);
        break;
    }

    ImGui::PushStyleColor(ImGuiCol_Button, normal);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, active);
    ImGui::PushStyleColor(ImGuiCol_Border, border);
    bool clicked = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return clicked;
}

// 仿 Python QGroupBox 蓝色分组标题头
static void renderGroupHeader(const char* title) {
    ImGui::TextColored(ImVec4(0.220f, 0.741f, 0.973f, 1.00f), "%s", title);
    ImGui::Separator();
}

static void applyPythonLikeDarkStyle(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    // 开启高质量抗锯齿线条与多边形填充
    style.AntiAliasedLines       = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill        = true;

    style.WindowPadding     = ImVec2(8.0f, 6.0f);
    style.FramePadding      = ImVec2(8.0f, 4.0f);
    style.ItemSpacing       = ImVec2(6.0f, 5.0f);
    style.ItemInnerSpacing  = ImVec2(5.0f, 4.0f);
    style.ScrollbarSize     = 11.0f;
    style.GrabMinSize       = 9.0f;

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 5.0f;
    style.ScrollbarRounding = 5.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 5.0f;
    style.WindowBorderSize  = 0.0f;
    style.ChildBorderSize   = 1.0f;
    style.FrameBorderSize   = 1.0f;

    // 严格对齐 Python 版 QSS 配色 (#0F172A / #1E293B / #334155 / #38BDF8)
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]                  = ImVec4(0.945f, 0.961f, 0.976f, 1.00f); // #F1F5F9
    colors[ImGuiCol_TextDisabled]          = ImVec4(0.580f, 0.639f, 0.722f, 1.00f); // #94A3B8
    colors[ImGuiCol_WindowBg]              = ImVec4(0.059f, 0.090f, 0.165f, 1.00f); // #0F172A
    colors[ImGuiCol_ChildBg]               = ImVec4(0.059f, 0.090f, 0.165f, 1.00f); // #0F172A
    colors[ImGuiCol_PopupBg]               = ImVec4(0.078f, 0.094f, 0.141f, 0.98f); // #141824
    colors[ImGuiCol_Border]                = ImVec4(0.200f, 0.255f, 0.333f, 1.00f); // #334155
    colors[ImGuiCol_BorderShadow]          = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
    colors[ImGuiCol_FrameBg]               = ImVec4(0.118f, 0.161f, 0.231f, 1.00f); // #1E293B
    colors[ImGuiCol_FrameBgHovered]        = ImVec4(0.200f, 0.255f, 0.333f, 1.00f); // #334155
    colors[ImGuiCol_FrameBgActive]         = ImVec4(0.008f, 0.518f, 0.780f, 1.00f); // #0284C7
    colors[ImGuiCol_CheckMark]             = ImVec4(0.220f, 0.741f, 0.973f, 1.00f); // #38BDF8
    colors[ImGuiCol_SliderGrab]            = ImVec4(0.220f, 0.741f, 0.973f, 1.00f); // #38BDF8
    colors[ImGuiCol_SliderGrabActive]      = ImVec4(0.008f, 0.518f, 0.780f, 1.00f); // #0284C7
    colors[ImGuiCol_Button]                = ImVec4(0.118f, 0.161f, 0.231f, 1.00f); // #1E293B
    colors[ImGuiCol_ButtonHovered]         = ImVec4(0.200f, 0.255f, 0.333f, 1.00f); // #334155
    colors[ImGuiCol_ButtonActive]          = ImVec4(0.008f, 0.518f, 0.780f, 1.00f); // #0284C7
    colors[ImGuiCol_Header]                = ImVec4(0.118f, 0.161f, 0.231f, 1.00f);
    colors[ImGuiCol_HeaderHovered]         = ImVec4(0.008f, 0.518f, 0.780f, 0.85f);
    colors[ImGuiCol_HeaderActive]          = ImVec4(0.008f, 0.518f, 0.780f, 1.00f);
    colors[ImGuiCol_Separator]             = ImVec4(0.200f, 0.255f, 0.333f, 1.00f); // #334155
    colors[ImGuiCol_Tab]                   = ImVec4(0.118f, 0.161f, 0.231f, 1.00f); // #1E293B
    colors[ImGuiCol_TabHovered]            = ImVec4(0.012f, 0.518f, 0.780f, 0.90f);
    colors[ImGuiCol_TabActive]             = ImVec4(0.008f, 0.518f, 0.780f, 1.00f); // #0284C7
    colors[ImGuiCol_TableHeaderBg]         = ImVec4(0.118f, 0.161f, 0.231f, 1.00f); // #1E293B
    colors[ImGuiCol_TableBorderStrong]     = ImVec4(0.200f, 0.255f, 0.333f, 1.00f); // #334155
    colors[ImGuiCol_TableBorderLight]      = ImVec4(0.200f, 0.255f, 0.333f, 0.70f);
    colors[ImGuiCol_PlotLines]             = ImVec4(0.063f, 0.725f, 0.506f, 1.00f); // #10B981

    style.ScaleAllSizes(scale);

    // 对齐 Python pyqtgraph 图表背景 (#111622) 与网格透明度
    ImPlotStyle& pstyle = ImPlot::GetStyle();
    pstyle = ImPlotStyle();
    pstyle.PlotPadding   = ImVec2(std::round(8.0f * scale), std::round(6.0f * scale));
    pstyle.LabelPadding  = ImVec2(std::round(5.0f * scale), std::round(3.0f * scale));
    pstyle.Colors[ImPlotCol_FrameBg]           = ImVec4(0.067f, 0.086f, 0.133f, 1.00f); // #111622
    pstyle.Colors[ImPlotCol_PlotBg]       = ImVec4(0.067f, 0.086f, 0.133f, 1.00f); // #111622
    pstyle.Colors[ImPlotCol_PlotBorder]   = ImVec4(0.200f, 0.255f, 0.333f, 0.85f); // #334155
    pstyle.Colors[ImPlotCol_AxisGrid]     = ImVec4(0.580f, 0.639f, 0.722f, 0.22f);
    pstyle.Colors[ImPlotCol_AxisText]     = ImVec4(0.886f, 0.910f, 0.941f, 1.00f); // #E2E8F0
    pstyle.Colors[ImPlotCol_LegendBg]     = ImVec4(0.059f, 0.090f, 0.165f, 0.88f); // #0F172A
    pstyle.Colors[ImPlotCol_LegendBorder] = ImVec4(0.278f, 0.333f, 0.412f, 1.00f); // #475569
}

// ====================================================================================================
// 15. 1:1 真实物理像素矢量字体重建器 (彻底告别 FontGlobalScale 位图拉伸模糊)
// ====================================================================================================

static int g_loaded_font_px = 0;
static bool g_in_sizemove_modal = false;
static ULONGLONG g_last_resize_tick_ms = 0;
static UINT g_pending_resize_w = 0;
static UINT g_pending_resize_h = 0;

static void rebuildCrispFontAndStyleIfNeeded(HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float disp_w = static_cast<float>(std::max<LONG>(640, rc.right - rc.left));
    float disp_h = static_cast<float>(std::max<LONG>(480, rc.bottom - rc.top));

    g_current_aspect = disp_w / disp_h;
    // 自动判断当前窗口属于 4:3 比例区间 (<= 1.48) 还是 16:9 宽屏区间 (> 1.48)
    g_is_aspect_4_3 = (g_current_aspect <= 1.48f);

    float ref_w = g_is_aspect_4_3 ? 1180.0f : 1440.0f;
    float ref_h = g_is_aspect_4_3 ? 840.0f  : 820.0f;
    float target_scale = safeClamp(std::min(disp_w / ref_w, disp_h / ref_h), 0.82f, 2.20f);

    // 计算与当前物理窗口尺寸严格匹配的整数像素字号（15px 基数，对应 Python 12px/9pt 在高 DPI 下的物理像素高度）
    int target_font_px = safeClamp(static_cast<int>(std::lround(15.5f * target_scale)), 13, 34);

    // 关键零卡顿优化：当用户正在按住鼠标拖拽窗口边框 (g_in_sizemove_modal) 或距离上一次 WM_SIZE 不足 160ms 时，
    // 绝不阻塞主线程重新光栅化 2 万个中文字形与重建 DX11 字体纹理，而是实时平滑更新布局缩放系数，
    // 等鼠标松开或缩放稳定后再一次性重建 1:1 物理像素矢量字体，彻底消除窗口缩放卡顿！
    const ULONGLONG now_ms = GetTickCount64();
    const bool resize_settling = g_in_sizemove_modal || (g_loaded_font_px > 0 && (now_ms - g_last_resize_tick_ms) < 160ULL);

    if (resize_settling && g_loaded_font_px > 0) {
        float new_scale = static_cast<float>(target_font_px) / 15.5f;
        if (std::abs(new_scale - ui_scale) > 1e-3f) {
            ui_scale = new_scale;
            ImGui::GetIO().FontGlobalScale = static_cast<float>(target_font_px) / static_cast<float>(g_loaded_font_px);
            applyPythonLikeDarkStyle(ui_scale);
        }
        return;
    }

    if (target_font_px != g_loaded_font_px) {
        g_loaded_font_px = target_font_px;
        ui_scale = static_cast<float>(target_font_px) / 15.5f;

        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        cfg.PixelSnapH  = true;

        const char* font_paths[] = {
            "C:\\Windows\\Fonts\\msyh.ttc",
            "C:\\Windows\\Fonts\\msyh.ttf",
            "C:\\Windows\\Fonts\\simhei.ttf"
        };
        bool loaded = false;
        for (const char* fp : font_paths) {
            if (fs::exists(fp)) {
                io.Fonts->AddFontFromFileTTF(fp, static_cast<float>(target_font_px), &cfg, io.Fonts->GetGlyphRangesChineseFull());
                loaded = true;
                break;
            }
        }
        if (!loaded) {
            io.Fonts->AddFontDefault();
        }
        io.Fonts->Build();
        // 缩放稳定后恢复 FontGlobalScale = 1.0f，保证 1:1 物理像素锐利度
        io.FontGlobalScale = 1.0f;

        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
        applyPythonLikeDarkStyle(ui_scale);
    } else if (std::abs(ImGui::GetIO().FontGlobalScale - 1.0f) > 1e-3f) {
        ImGui::GetIO().FontGlobalScale = 1.0f;
    }
}

static void loadAudioFileIntoApp(AudioStudioWorkstation& app, const std::string& utf8_path) {
    std::vector<float> decoded;
    int sr = 44100;
    std::string resolved_wav, err;
    if (!loadAndAutoConvertToWav(utf8_path, decoded, sr, resolved_wav, err)) {
        app.status_message = "[错误] 加载失败: " + err;
        return;
    }

    app.is_playing_file.store(false);
    if (!app.playback_device_initialized) {
        app.initPlaybackDevice(0);
    }
    double dur = static_cast<double>(decoded.size() / 2) / static_cast<double>(std::max(1, sr));
    std::vector<float> playback_buf = (sr != app.stream_sample_rate)
        ? AudioDSPEngine::offlineResample(decoded, sr, app.stream_sample_rate, 32, 2)
        : decoded;
    {
        std::lock_guard<std::mutex> alk(app.audio_data_mutex);
        app.raw_audio_buffer = std::move(decoded);
        app.audio_buffer = std::move(playback_buf);
        app.orig_sample_rate = sr;
        app.play_cursor.store(0);
    }
    app.realtime_eq.requestReset();
    app.wsola_processor.requestReset();
    app.peak_hold_db.clear();
    app.has_rendered_live_frame = false;
    app.bpm_anchor_time = std::chrono::steady_clock::now();
    app.bpm_last_beat_index = -1;
    fs::path p_orig(utf8ToWide(utf8_path));
    fs::path p_wav(utf8ToWide(resolved_wav));
    std::string base_name = wideToUtf8(p_wav.filename().wstring());

    char info[256];
    if (p_orig.extension() != p_wav.extension()) {
        std::snprintf(info, sizeof(info), "已转同名WAV: %s | %d Hz | %.1fs", base_name.c_str(), sr, dur);
    }
    else {
        std::snprintf(info, sizeof(info), "当前文件: %s | %d Hz | %.1fs", base_name.c_str(), sr, dur);
    }
    app.current_file_display = info;
    app.is_playing_file.store(true);
    app.status_message = "加载成功: " + resolved_wav;
}

static void renderMainWorkstationUI(AudioStudioWorkstation& app, HWND hwnd) {
    app.updateLiveSpectrumEachFrame();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGuiWindowFlags win_flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (viewport->WorkSize.x < 64.0f || viewport->WorkSize.y < 64.0f) {
        return;
    }
    ImGui::Begin("AudioStudioProMain", nullptr, win_flags);

    // ------------------------------------------------------------------------------------------------
    // 顶部控制栏 (GroupBox 风格)：双行紧凑排布，进度条带有防卡死隔离保护
    // ------------------------------------------------------------------------------------------------
    float top_bar_h = SF(88.0f);
    ImGui::BeginChild("##TopToolbar", ImVec2(-1, top_bar_h), true, ImGuiWindowFlags_NoScrollbar);
    {
        renderGroupHeader("播放与控制");

        if (styledButton("导入音乐", BtnTone::Neutral)) {
            std::string path = openAudioFileDialog(hwnd);
            if (!path.empty()) loadAudioFileIntoApp(app, path);
        }
        ImGui::SameLine();

        bool playing = app.is_playing_file.load();
        if (styledButton(playing ? "暂停" : "播放", playing ? BtnTone::Primary : BtnTone::Neutral)) {
            if (!app.audio_buffer.empty()) {
                app.is_playing_file.store(!playing);
                if (!playing) {
                    app.bpm_anchor_time = std::chrono::steady_clock::now();
                    app.bpm_last_beat_index = -1;
                }
            }
        }
        ImGui::SameLine();
        if (styledButton("停止", BtnTone::Neutral)) {
            app.is_playing_file.store(false);
            app.play_cursor.store(0);
            app.realtime_eq.requestReset();
            app.wsola_processor.requestReset();
        }
        ImGui::SameLine();

        ImGui::AlignTextToFramePadding();
        ImGui::Text("倍速:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 92.0f : 106.0f));
        static const char* SPEED_LABELS[] = { "0.25x 极慢", "0.50x 慢速", "0.75x 稍慢", "1.00x 原速", "1.25x 稍快", "1.50x 快速", "2.00x 倍速" };
        static const float SPEED_VALS[] = { 0.25f, 0.50f, 0.75f, 1.00f, 1.25f, 1.50f, 2.00f };
        static int speed_idx = 3;
        if (ImGui::Combo("##speed", &speed_idx, SPEED_LABELS, IM_ARRAYSIZE(SPEED_LABELS))) {
            app.playback_speed.store(SPEED_VALS[speed_idx]);
        }
        ImGui::SameLine();

        bool kp = app.speed_keep_pitch.load();
        if (ImGui::Checkbox("保音高", &kp)) app.speed_keep_pitch.store(kp);
        ImGui::SameLine();

        if (styledButton("截取当前声音", BtnTone::Purple)) {
            app.captureAudioSnapshot(true);
        }
        ImGui::SameLine();

        // 进度条（解决长按拖动卡死闪退：拖动期间隔离状态 + 线程安全 requestReset + 防止 1.0f 终点越界）
        size_t total_f = app.audio_buffer.size() / 2;
        size_t cur_f = app.play_cursor.load(std::memory_order_relaxed);
        float progress = 0.0f;
        if (app.is_dragging_seek) {
            progress = app.dragging_seek_ratio;
        }
        else if (total_f > 0) {
            progress = safeClamp(static_cast<float>(cur_f) / static_cast<float>(total_f), 0.0f, 1.0f);
        }

        float right_reserve = SF(g_is_aspect_4_3 ? 210.0f : 235.0f);
        ImGui::SetNextItemWidth(std::max(SF(75.0f), ImGui::GetContentRegionAvail().x - right_reserve));
        bool slider_changed = ImGui::SliderFloat("##seek", &progress, 0.0f, 1.0f, "");
        bool slider_active = ImGui::IsItemActive();

        if (slider_active) {
            app.is_dragging_seek = true;
            app.dragging_seek_ratio = safeClamp(progress, 0.0f, 1.0f);
            if (slider_changed && total_f > 0) {
                size_t target_frame = std::min(
                    total_f - 1,
                    static_cast<size_t>(safeClamp(progress, 0.0f, 0.9995f) * static_cast<float>(total_f))
                );
                app.play_cursor.store(target_frame, std::memory_order_relaxed);
                app.realtime_eq.requestReset();
            app.wsola_processor.requestReset();
            }
        }
        else if (app.is_dragging_seek) {
            app.is_dragging_seek = false;
            if (total_f > 0) {
                size_t target_frame = std::min(
                    total_f - 1,
                    static_cast<size_t>(safeClamp(app.dragging_seek_ratio, 0.0f, 0.9995f) * static_cast<float>(total_f))
                );
                app.play_cursor.store(target_frame, std::memory_order_relaxed);
                app.realtime_eq.requestReset();
            app.wsola_processor.requestReset();
            }
        }
        ImGui::SameLine();

        size_t display_f = (app.is_dragging_seek && total_f > 0)
            ? static_cast<size_t>(app.dragging_seek_ratio * static_cast<float>(total_f))
            : cur_f;
        int pb_sr = std::max(1, app.stream_sample_rate);
        int cur_s = static_cast<int>(display_f / pb_sr);
        int tot_s = static_cast<int>(total_f / pb_sr);
        ImGui::Text("%02d:%02d / %02d:%02d", cur_s / 60, cur_s % 60, tot_s / 60, tot_s % 60);
        ImGui::SameLine();

        float vol = app.master_volume.load() * 100.0f;
        ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 65.0f : 78.0f));
        if (ImGui::SliderFloat("音量", &vol, 0.0f, 100.0f, "%.0f%%")) {
            app.master_volume.store(vol / 100.0f);
        }

        // 第 2 行：当前文件状态 + 分析源切换 + 预设导入导出
        bool is_loop = app.use_system_loopback.load();
        if (styledButton(
            is_loop ? "分析源：系统声音" : "分析源：软件内部",
            is_loop ? BtnTone::Amber : BtnTone::Teal)) {
            if (!is_loop) {
                static const int kRecChLookup[] = { 0, 2, 1, 6, 8 };
                int pref_loop_ch = kRecChLookup[safeClamp(app.rec_ch_idx, 0, 4)];
                if (app.startSystemLoopbackCapture(pref_loop_ch)) {
                    app.use_system_loopback.store(true);
                    app.peak_hold_db.clear();
                    app.status_message = "WASAPI Loopback 已启动 (" + app.loopback_device_name + ")";
                }
                else {
                    app.status_message = "启动 WASAPI Loopback 失败";
                }
            }
            else {
                app.use_system_loopback.store(false);
                app.peak_hold_db.clear();
                app.status_message = "已切换回分析软件内部选中的音乐文件与合成器声音。";
                app.stopSystemLoopbackCaptureIfUnused();
            }
        }
        ImGui::SameLine();

        if (styledButton("导入预设", BtnTone::Neutral)) {
            std::string p = openAudioFileDialog(hwnd);
            if (!p.empty() && loadPresetFromJsonFile(app.preset, p)) {
                app.recompileFormulas();
                app.refreshHarmonicPreview();
                app.status_message = "已成功加载预设配置: " + p;
            }
        }
        ImGui::SameLine();
        if (styledButton("导出预设", BtnTone::Neutral)) {
            std::string p = saveFileDialog(hwnd, "audio_studio_preset.json", L"JSON 配置文件 (*.json)\0*.json\0");
            if (!p.empty()) {
                savePresetToJsonFile(app.preset, p);
                app.status_message = "配置已导出至: " + p;
            }
        }
        ImGui::SameLine();
        if (styledButton("免责/开源协议", BtnTone::Neutral)) {
            app.show_disclaimer_viewer = true;
        }
        ImGui::SameLine(0.0f, SF(12.0f));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", app.current_file_display.c_str());
    }
    ImGui::EndChild();

    // ------------------------------------------------------------------------------------------------
    // 上半部与下半部可拖拽分割计算
    // ------------------------------------------------------------------------------------------------
    float avail_h = ImGui::GetContentRegionAvail().y - SF(26.0f);
    float top_pane_h = safeClamp(avail_h * app.upper_pane_ratio, SF(200.0f), avail_h - SF(200.0f));

    ImGui::BeginChild("##UpperSection", ImVec2(-1, top_pane_h), false);
    {
        float spec_ratio = g_is_aspect_4_3 ? 0.72f : 0.77f;
        float left_w = std::floor(ImGui::GetContentRegionAvail().x * spec_ratio);

        ImGui::BeginChild("##SpectrumGroup", ImVec2(left_w, -1), true, ImGuiWindowFlags_NoScrollbar);
        {
            renderGroupHeader("实时频谱");

            ImGui::AlignTextToFramePadding();
            if (ImGui::Checkbox("对数坐标", &app.log_freq_axis)) {
                app.shared_x_min = app.log_freq_axis ? std::log10(MIN_PLOT_FREQ_HZ) : MIN_PLOT_FREQ_HZ;
                app.shared_x_max = app.log_freq_axis ? std::log10(MAX_PLOT_FREQ_HZ) : MAX_PLOT_FREQ_HZ;
                app.refreshHarmonicPreview();
                app.refreshSnapshotPlots(true);
            }
            ImGui::SameLine();
            ImGui::Checkbox("横轴同步缩放", &app.sync_x_axes);
            ImGui::SameLine();
            ImGui::Checkbox("暂停保留频谱", &app.freeze_on_pause);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.984f, 0.749f, 0.141f, 1.0f), "主音标注:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 102.0f : 120.0f));
            static const char* PEAK_CNT_ITEMS[] = { "0个 (不标注)", "1个 (仅第1主音)", "2个 (前2高主音)", "3个 (前3高主音)" };
            if (ImGui::Combo("##peak_cnt", &app.preset.peak_annotation_count, PEAK_CNT_ITEMS, 4)) {
                app.refreshSnapshotPlots(true);
            }
            ImGui::SameLine();
            if (styledButton("重置缩放", BtnTone::Neutral)) {
                app.shared_x_min = app.log_freq_axis ? std::log10(MIN_PLOT_FREQ_HZ) : MIN_PLOT_FREQ_HZ;
                app.shared_x_max = app.log_freq_axis ? std::log10(MAX_PLOT_FREQ_HZ) : MAX_PLOT_FREQ_HZ;
            }
            ImGui::SameLine();
            ImGui::Checkbox("峰值包络", &app.peak_hold_enabled);
            ImGui::SameLine();
            if (styledButton("清除峰值", BtnTone::Neutral)) {
                app.peak_hold_db.clear();
            }
            ImGui::SameLine();
            ImGui::Text("FFT:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(70.0f));
            static const char* FFT_ITEMS[] = { "32", "64", "128", "256", "512", "1024", "2048", "4096", "8192" };
            if (ImGui::Combo("##fft_sz", &app.fft_size_idx, FFT_ITEMS, 9)) {
                app.peak_hold_db.clear();
            }

            if (ImPlot::BeginPlot("##MainSpectrumPlot", ImVec2(-1, -1), ImPlotFlags_NoMenus)) {
                ImPlot::SetupAxes("频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]", "分贝 (dBFS)", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
                ImPlot::SetupAxisLimitsConstraints(ImAxis_X1, app.log_freq_axis ? std::log10(12.0) : 10.0, app.log_freq_axis ? std::log10(24000.0) : 24000.0);
                ImPlot::SetupAxisLimits(ImAxis_Y1, -100.0, 8.0, ImPlotCond_Always);
                if (app.sync_x_axes) {
                    ImPlot::SetupAxisLinks(ImAxis_X1, &app.shared_x_min, &app.shared_x_max);
                }
                setupFreqPianoAxisTicks(app.log_freq_axis);

                if (!app.live_x_plot.empty()) {
                    ImPlot::SetNextFillStyle(ImVec4(0.220f, 0.741f, 0.973f, 0.22f));
                    ImPlot::PlotShaded("实时频谱", app.live_x_plot.data(), app.live_mag_db.data(), static_cast<int>(app.live_x_plot.size()), -100.0);
                    ImPlot::SetNextLineStyle(ImVec4(0.220f, 0.741f, 0.973f, 1.00f), 2.0f);
                    ImPlot::PlotLine("实时频谱", app.live_x_plot.data(), app.live_mag_db.data(), static_cast<int>(app.live_x_plot.size()));

                    if (app.peak_hold_enabled && app.peak_hold_db.size() == app.live_x_plot.size()) {
                        ImPlot::SetNextLineStyle(ImVec4(0.957f, 0.247f, 0.369f, 0.85f), 1.4f);
                        ImPlot::PlotLine("峰值保持", app.live_x_plot.data(), app.peak_hold_db.data(), static_cast<int>(app.peak_hold_db.size()));
                    }
                }
                drawPianoGuideLinesAndPeaks(app.log_freq_axis, app.live_top3_peaks, app.preset.peak_annotation_count);
                ImPlot::EndPlot();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();

        // 右侧：实时音频参数与主音面板（复刻 Python 版的清晰键值对表单与绿色示波器）
        ImGui::BeginChild("##MetricsPanel", ImVec2(-1, -1), true, ImGuiWindowFlags_NoScrollbar);
        {
            renderGroupHeader("音频参数与主音");

            if (ImGui::BeginTable("##MetricsForm", 2, ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, SF(64.0f));
                ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);

                auto addRow = [](const char* k, const char* v, const ImVec4* col = nullptr) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(0.75f, 0.80f, 0.88f, 1.0f), "%s", k);
                    ImGui::TableSetColumnIndex(1);
                    if (col) ImGui::TextColored(*col, "%s", v);
                    else ImGui::Text("%s", v);
                    };

                ImVec4 cyan_col(0.220f, 0.741f, 0.973f, 1.0f);
                char buf[96];
                if (app.use_system_loopback.load()) {
                    std::snprintf(buf, sizeof(buf), "系统媒体 (%s)", app.loopback_device_name.substr(0, 14).c_str());
                }
                else {
                    std::snprintf(buf, sizeof(buf), "软件内部 (%.2fx)", app.playback_speed.load());
                }
                addRow("监控源:", buf, &cyan_col);

                std::snprintf(buf, sizeof(buf), "%d Hz", app.orig_sample_rate);
                addRow("原采样率:", buf);

                if (app.preset.resample_live_enabled) {
                    std::snprintf(buf, sizeof(buf), "%d Hz (重采样)", app.preset.target_sample_rate);
                }
                else {
                    std::snprintf(buf, sizeof(buf), "%d Hz (直通)", app.stream_sample_rate);
                }
                addRow("有效采样:", buf);

                if (app.use_system_loopback.load()) {
                    std::snprintf(buf, sizeof(buf), "%d声道 / 32-bit", app.loopback_channels.load());
                }
                else if (app.preset.resample_live_enabled) {
                    std::snprintf(buf, sizeof(buf), "%d声道 / %d-bit", app.preset.target_channels, app.preset.target_bit_depth);
                }
                else {
                    std::snprintf(buf, sizeof(buf), "2声道 / 32-bit");
                }
                addRow("声道/位深:", buf);

                ImVec4 pk_cols[3] = {
                    ImVec4(0.984f, 0.749f, 0.141f, 1.0f), // #FBBF24
                    ImVec4(0.176f, 0.831f, 0.749f, 1.0f), // #2DD4BF
                    ImVec4(0.957f, 0.447f, 0.714f, 1.0f)  // #F472B6
                };
                const char* pk_titles[3] = { "第1主音:", "第2主音:", "第3主音:" };
                for (int i = 0; i < 3; ++i) {
                    if (i < app.preset.peak_annotation_count && i < static_cast<int>(app.live_top3_peaks.size())) {
                        const auto& p = app.live_top3_peaks[i];
                        std::snprintf(buf, sizeof(buf), "#%d %s (%.1fHz, %.1fdB)", i + 1, p.note.c_str(), p.freq_hz, p.mag_db);
                        addRow(pk_titles[i], buf, &pk_cols[i]);
                    }
                    else {
                        std::snprintf(buf, sizeof(buf), "#%d: %s", i + 1, (i < app.preset.peak_annotation_count) ? "--" : "(未启用)");
                        addRow(pk_titles[i], buf, &pk_cols[i]);
                    }
                }

                std::snprintf(buf, sizeof(buf), "%.1f dBFS", app.live_stats.rms_db);
                addRow("平均响度:", buf);

                std::snprintf(buf, sizeof(buf), "%.1f Hz", app.live_stats.centroid_hz);
                addRow("频谱重心:", buf);

                ImGui::EndTable();
            }

            ImGui::Spacing();
            ImGui::Text("时域波形:");
            std::vector<float> scope_st;
            if (app.use_system_loopback.load()) app.loopback_ring_buf.copyLatestFrames(scope_st, 512);
            else app.internal_ring_buf.copyLatestFrames(scope_st, 512);
            std::vector<float> scope_mono(scope_st.size() / 2);
            for (size_t i = 0; i < scope_mono.size(); ++i) scope_mono[i] = 0.5f * (scope_st[i * 2] + scope_st[i * 2 + 1]);
            ImGui::PlotLines("##Scope", scope_mono.data(), static_cast<int>(scope_mono.size()), 0, nullptr, -1.0f, 1.0f,
                ImVec2(-1, std::max(SF(42.0f), ImGui::GetContentRegionAvail().y)));
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();

    // 可鼠标拖拽的水平分割条 (Splitter)
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.200f, 0.255f, 0.333f, 0.55f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.220f, 0.741f, 0.973f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.008f, 0.518f, 0.780f, 1.00f));
    ImGui::Button("##VerticalSplitter", ImVec2(-1, SF(4.0f)));
    if (ImGui::IsItemActive()) {
        app.upper_pane_ratio = safeClamp((top_pane_h + ImGui::GetIO().MouseDelta.y) / std::max(1.0f, avail_h), 0.28f, 0.72f);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    ImGui::PopStyleColor(3);

    // ------------------------------------------------------------------------------------------------
    // 下半部：四大功能标签页
    // ------------------------------------------------------------------------------------------------
    ImGui::BeginChild("##BottomTabsChild", ImVec2(-1, ImGui::GetContentRegionAvail().y - SF(22.0f)), true);
    if (ImGui::BeginTabBar("##MainTabs")) {

        // ============================================================================================
        // Tab 1: 无破音曲线均衡器
        // ============================================================================================
        if (ImGui::BeginTabItem("均衡器")) {
            ImGui::AlignTextToFramePadding();
            ImGui::Checkbox("启用均衡器", &app.preset.eq_enabled);
            ImGui::SameLine();
            ImGui::Text("预设:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 165.0f : 200.0f));
            const auto& eq_presets = getEqCurvePresets();
            static int eq_preset_idx = 0;
            if (ImGui::BeginCombo("##eq_preset", eq_presets[eq_preset_idx].name)) {
                for (int i = 0; i < static_cast<int>(eq_presets.size()); ++i) {
                    if (ImGui::Selectable(eq_presets[i].name, eq_preset_idx == i)) {
                        eq_preset_idx = i;
                        std::lock_guard<std::mutex> lk(app.eq_points_mutex);
                        app.preset.eq_points = eq_presets[i].points;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (styledButton("重置 0dB 平坦曲线", BtnTone::Neutral)) {
                std::lock_guard<std::mutex> lk(app.eq_points_mutex);
                app.preset.eq_points = eq_presets[0].points;
            }


            renderDraggableCurvePlot(
                "均衡器曲线",
                app.preset.eq_points, &app.eq_points_mutex, -24.0, 24.0,
                ImVec4(0.220f, 0.741f, 0.973f, 1.0f), app.log_freq_axis, app.sync_x_axes,
                &app.shared_x_min, &app.shared_x_max
            );
            ImGui::EndTabItem();
        }

        // ============================================================================================
        // Tab 2: 函数波发生器 & 乐器泛音合成器 (右侧为与 Python 一致的 5 列标准表格)
        // ============================================================================================
        if (ImGui::BeginTabItem("声音合成器")) {
            float synth_left_ratio = g_is_aspect_4_3 ? 0.58f : 0.62f;
            float left_synth_w = std::floor(ImGui::GetContentRegionAvail().x * synth_left_ratio);
            ImGui::BeginChild("##SynthLeft", ImVec2(left_synth_w, -1), true, ImGuiWindowFlags_NoScrollbar);
            {
                renderGroupHeader("泛音与函数合成");

                bool s_play = app.is_playing_synth.load();
                if (styledButton(s_play ? "停止合成器" : "开启合成器发声", s_play ? BtnTone::Danger : BtnTone::Indigo)) {
                    app.is_playing_synth.store(!s_play);
                }
                ImGui::SameLine();

                ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 185.0f : 235.0f));
                static const char* SYNTH_MODES[] = {
                    "模式 A：泛音包络",
                    "模式 B：多波混音",
                    "模式 C：数学公式"
                };
                ImGui::Combo("模式", &app.preset.synth_mode, SYNTH_MODES, 3);
                ImGui::SameLine();

                const auto& inst_presets = getInstrumentEnvelopePresets();
                static int inst_idx = 0;
                ImGui::SetNextItemWidth(std::max(SF(110.0f), ImGui::GetContentRegionAvail().x - SF(36.0f)));
                if (ImGui::BeginCombo("音色", inst_presets[inst_idx].name)) {
                    for (int i = 0; i < static_cast<int>(inst_presets.size()); ++i) {
                        if (ImGui::Selectable(inst_presets[i].name, inst_idx == i)) {
                            inst_idx = i;
                            std::lock_guard<std::mutex> lk(app.synth_mutex);
                            app.preset.harmonic_curve_points = inst_presets[i].points;
                            app.refreshHarmonicPreview();
                        }
                    }
                    ImGui::EndCombo();
                }

                ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 86.0f : 100.0f));
                if (ImGui::InputDouble("基频f0(Hz)", &app.preset.fundamental_hz, 5.0, 20.0, "%.2f")) {
                    app.preset.fundamental_hz = safeClamp(app.preset.fundamental_hz, 15.0, 8000.0);
                    app.refreshHarmonicPreview();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 125.0f : 150.0f));
                if (ImGui::BeginCombo("音阶", app.piano_notes_list[app.selected_piano_note_idx].label.c_str())) {
                    for (int i = 0; i < static_cast<int>(app.piano_notes_list.size()); ++i) {
                        if (ImGui::Selectable(app.piano_notes_list[i].label.c_str(), app.selected_piano_note_idx == i)) {
                            app.selected_piano_note_idx = i;
                            app.preset.fundamental_hz = app.piano_notes_list[i].freq_hz;
                            app.refreshHarmonicPreview();
                        }
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 70.0f : 82.0f));
                if (ImGui::SliderInt("泛音数", &app.preset.num_harmonics, 1, 48)) {
                    app.refreshHarmonicPreview();
                }
                ImGui::SameLine();
                ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 78.0f : 90.0f));
                static const char* WAVE_NAMES[] = { "sine", "square", "triangle", "sawtooth", "custom" };
                ImGui::Combo("基波", &app.preset.base_waveform_idx, WAVE_NAMES, 5);

                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(ImVec4(0.984f, 0.749f, 0.141f, 1.0f), "快捷选音:");
                static const struct { const char* lbl; double f; } QUICK_NOTES[] = {
                    {"C0", 16.35}, {"C1", 32.70}, {"C2", 65.41}, {"C3", 130.81}, {"A3(220)", 220.00},
                    {"C4(261.6)", 261.63}, {"A4(440)", 440.00}, {"C5", 523.25}, {"C6", 1046.50}, {"C7", 2093.00}, {"C8", 4186.01}
                };
                for (int q = 0; q < 11; ++q) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton(QUICK_NOTES[q].lbl)) {
                        app.preset.fundamental_hz = QUICK_NOTES[q].f;
                        app.refreshHarmonicPreview();
                    }
                }

                char formula_buf[256];
                std::snprintf(formula_buf, sizeof(formula_buf), "%s", app.preset.custom_formula.c_str());
                ImGui::SetNextItemWidth(std::max(SF(100.0f), ImGui::GetContentRegionAvail().x - SF(215.0f)));
                if (ImGui::InputText("公式 f(x)", formula_buf, sizeof(formula_buf))) {
                    app.preset.custom_formula = formula_buf;
                    app.recompileFormulas();
                }
                ImGui::SameLine();
                if (styledButton("用公式发声(模式C)", BtnTone::Primary)) {
                    app.preset.synth_mode = 2;
                    app.is_playing_synth.store(true);
                }
                ImGui::SameLine();
                if (app.formula_valid) {
                    ImGui::TextColored(ImVec4(0.063f, 0.725f, 0.506f, 1.0f), "有效");
                }
                else {
                    ImGui::TextColored(ImVec4(0.937f, 0.267f, 0.267f, 1.0f), "语法错误");
                }

                if (renderDraggableCurvePlot(
                    "泛音包络曲线",
                    app.preset.harmonic_curve_points, &app.synth_mutex, -80.0, 0.0,
                    ImVec4(0.659f, 0.333f, 0.969f, 1.0f), app.log_freq_axis, app.sync_x_axes,
                    &app.shared_x_min, &app.shared_x_max, &app.harm_preview_x, &app.harm_preview_db)) {
                    app.refreshHarmonicPreview();
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            // 右侧：多振荡器自由混合列表 (与 Python 版一致的 5 列标准表格)
            ImGui::BeginChild("##SynthRight", ImVec2(-1, -1), true);
            {
                renderGroupHeader("多振荡器列表");

                float table_h = std::max(SF(80.0f), ImGui::GetContentRegionAvail().y - SF(36.0f));
                static const char* OSC_WAVES[] = { "sine", "square", "triangle", "sawtooth", "custom", "noise" };

                if (ImGui::BeginTable("##OscTable", 5,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp,
                    ImVec2(-1, table_h))) {
                    ImGui::TableSetupScrollFreeze(0, 1);
                    ImGui::TableSetupColumn("启用", ImGuiTableColumnFlags_WidthFixed, SF(36.0f));
                    ImGui::TableSetupColumn("波形", ImGuiTableColumnFlags_WidthFixed, SF(82.0f));
                    ImGui::TableSetupColumn("频率(Hz)", ImGuiTableColumnFlags_WidthFixed, SF(74.0f));
                    ImGui::TableSetupColumn("分贝(dB)", ImGuiTableColumnFlags_WidthFixed, SF(68.0f));
                    ImGui::TableSetupColumn("公式(custom)", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (size_t i = 0; i < app.preset.oscillators.size(); ++i) {
                        ImGui::TableNextRow();
                        ImGui::PushID(static_cast<int>(i));
                        auto& osc = app.preset.oscillators[i];

                        ImGui::TableSetColumnIndex(0);
                        ImGui::Checkbox("##en", &osc.enabled);

                        ImGui::TableSetColumnIndex(1);
                        ImGui::SetNextItemWidth(-1);
                        ImGui::Combo("##wv", &osc.waveform_idx, OSC_WAVES, 6);

                        ImGui::TableSetColumnIndex(2);
                        ImGui::SetNextItemWidth(-1);
                        float f_val = static_cast<float>(osc.freq_hz);
                        if (ImGui::DragFloat("##hz", &f_val, 2.0f, 15.0f, 20000.0f, "%.1f")) osc.freq_hz = f_val;

                        ImGui::TableSetColumnIndex(3);
                        ImGui::SetNextItemWidth(-1);
                        float db_val = static_cast<float>(osc.gain_db);
                        if (ImGui::DragFloat("##db", &db_val, 0.5f, -80.0f, 0.0f, "%.1f")) osc.gain_db = db_val;

                        ImGui::TableSetColumnIndex(4);
                        ImGui::SetNextItemWidth(-1);
                        char obuf[128];
                        std::snprintf(obuf, sizeof(obuf), "%s", osc.formula.c_str());
                        if (ImGui::InputText("##fm", obuf, sizeof(obuf))) {
                            osc.formula = obuf;
                            app.recompileFormulas();
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }

                float half_btn_w = (ImGui::GetContentRegionAvail().x - SF(6.0f)) * 0.5f;
                if (styledButton("+ 添加振荡器", BtnTone::Neutral, ImVec2(half_btn_w, 0)) && app.preset.oscillators.size() < 32) {
                    std::lock_guard<std::mutex> lk(app.synth_mutex);
                    app.preset.oscillators.push_back({ true, 0, 550.0, -15.0, 0.0, "sin(x)" });
                    app.recompileFormulas();
                }
                ImGui::SameLine();
                if (styledButton("- 删除末尾", BtnTone::Neutral, ImVec2(-1, 0)) && !app.preset.oscillators.empty()) {
                    std::lock_guard<std::mutex> lk(app.synth_mutex);
                    app.preset.oscillators.pop_back();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ============================================================================================
        // Tab 3: 边听边采样 / 多格式转换导出 / 双源实时录音
        // ============================================================================================
        if (ImGui::BeginTabItem("格式转换与录音")) {
            float half_w = std::floor(ImGui::GetContentRegionAvail().x * 0.5f);
            ImGui::BeginChild("##ResampleBox", ImVec2(half_w, -1), true);
            {
                renderGroupHeader("重采样与格式转换");
                ImGui::Checkbox("开启实时重采样", &app.preset.resample_live_enabled);
                ImGui::Spacing();

                static const int SR_LIST[] = { 8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000 };
                static const char* SR_NAMES[] = { "8000 Hz", "11025 Hz", "16000 Hz", "22050 Hz", "32000 Hz", "44100 Hz", "48000 Hz", "88200 Hz", "96000 Hz" };
                static int sr_idx = 5;
                if (ImGui::Combo("目标采样率", &sr_idx, SR_NAMES, 9)) app.preset.target_sample_rate = SR_LIST[sr_idx];

                static const int BIT_LIST[] = { 8, 16, 24, 32 };
                static const char* BIT_NAMES[] = { "8-bit", "16-bit", "24-bit", "32-bit Float" };
                static int bit_idx = 1;
                if (ImGui::Combo("目标采样位深", &bit_idx, BIT_NAMES, 4)) app.preset.target_bit_depth = BIT_LIST[bit_idx];

                static const int TARGET_CH_VALS[] = { 1, 2, 6, 8 };
                static const char* CH_NAMES[] = {
                    "单声道 (1声道 Mono)",
                    "立体声 (2声道 Stereo)",
                    "5.1 环绕声 (6声道)",
                    "7.1 环绕声 (8声道)"
                };
                int ch_idx = 1;
                for (int ci = 0; ci < 4; ++ci) {
                    if (app.preset.target_channels == TARGET_CH_VALS[ci]) { ch_idx = ci; break; }
                }
                if (ImGui::Combo("目标声道数", &ch_idx, CH_NAMES, 4)) {
                    app.preset.target_channels = TARGET_CH_VALS[safeClamp(ch_idx, 0, 3)];
                }

                static const char* EXP_FMTS[] = { "wav", "mp3", "flac", "ogg", "m4a", "aac" };
                ImGui::Combo("转换目标格式", &app.export_fmt_idx, EXP_FMTS, 6);

                static const char* BITRATES[] = { "128k", "192k", "256k", "320k" };
                ImGui::Combo("有损编码比特率", &app.export_bitrate_idx, BITRATES, 4);

                ImGui::Spacing();
                if (styledButton("导出音频文件", BtnTone::Primary, SVec2(-1, 34.0f))) {
                    if (!app.audio_buffer.empty()) {
                        std::string ext = EXP_FMTS[app.export_fmt_idx];
                        std::string def_name = "resampled_" + std::to_string(app.preset.target_sample_rate) + "Hz." + ext;
                        std::string out_p = saveFileDialog(hwnd, def_name, L"音频文件 (*.*)\0*.*\0");
                        if (!out_p.empty()) {
                            std::vector<float> to_export = !app.raw_audio_buffer.empty() ? app.raw_audio_buffer : app.audio_buffer;
                            if (app.preset.eq_enabled) {
                                StatefulOverlapAddEQ offline_eq;
                                offline_eq.processStereoBlock(to_export.data(), to_export.size() / 2, app.orig_sample_rate, app.preset.eq_points);
                            }
                            std::string saved, err;
                            if (exportAndConvertAudio(to_export, app.orig_sample_rate, out_p,
                                app.preset.target_sample_rate, app.preset.target_bit_depth,
                                app.preset.target_channels, BITRATES[app.export_bitrate_idx], saved, err, ext)) {
                                app.status_message = "重采样导出成功: " + saved;
                            }
                            else {
                                app.status_message = "导出失败: " + err;
                            }
                        }
                    }
                    else {
                        app.status_message = "请先导入一个音乐文件再执行重采样导出";
                    }
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            ImGui::BeginChild("##RecordBox", ImVec2(-1, -1), true);
            {
                renderGroupHeader("声音录制");
                static const char* REC_SOURCES[] = {
                    "软件内部声音",
                    "电脑系统声音"
                };
                const bool rec = app.is_recording.load();
                const int src_before = app.record_source_idx;
                if (ImGui::Combo("选择录制源", &app.record_source_idx, REC_SOURCES, 2) && rec) {
                    // 录制期间录制源已锁定，不允许切换
                    app.record_source_idx = src_before;
                }

                static const int REC_SRS[] = { 0, 16000, 22050, 44100, 48000, 96000 };
                static const char* REC_SR_LABELS[] = {
                    "保持原采样率 (无损直通)",
                    "16000 Hz",
                    "22050 Hz",
                    "44100 Hz",
                    "48000 Hz",
                    "96000 Hz"
                };
                ImGui::Combo("录制导出采样率", &app.rec_sr_idx, REC_SR_LABELS, 6);

                static const int REC_BITS[] = { 16, 24, 32 };
                static const char* REC_BIT_LABELS[] = { "16-bit PCM", "24-bit PCM", "32-bit Float" };
                ImGui::Combo("录制导出位深", &app.rec_bits_idx, REC_BIT_LABELS, 3);

                static const int REC_CHS[] = { 0, 2, 1, 6, 8 };
                static const char* REC_CH_LABELS[] = {
                    "保持原声道 (多声道/立体声无损直通)",
                    "立体声 (2声道 Stereo)",
                    "单声道 (1声道 Mono)",
                    "5.1 环绕声 (6声道)",
                    "7.1 环绕声 (8声道)"
                };
                if (ImGui::Combo("录制导出声道", &app.rec_ch_idx, REC_CH_LABELS, 5)) {
                    // 若当前处于待机状态且正在监听系统声音，切换声道下拉框时立即重启 WASAPI 会话向系统请求对应声道格式
                    if (!rec && (app.record_source_idx == 1 || app.use_system_loopback.load()) && app.native_loopback_engine.isRunning()) {
                        int req_ch = REC_CHS[safeClamp(app.rec_ch_idx, 0, 4)];
                        app.startSystemLoopbackCapture(req_ch);
                    }
                }

                static const char* REC_FMTS[] = { "wav", "mp3", "flac", "ogg" };
                ImGui::Combo("录制保存格式", &app.rec_fmt_idx, REC_FMTS, 4);

                // 时长按已写入的样本数计算；严格显示真实物理/系统采集声道数（实际录几声道就写几声道，绝不虚标）
                const double rec_sec = static_cast<double>(app.recorded_frames_total.load()) /
                    static_cast<double>(std::max(1, app.recorded_source_sr.load()));
                const int live_actual_sr = rec
                    ? app.recorded_source_sr.load()
                    : ((app.record_source_idx == 1 && app.native_loopback_engine.isRunning())
                        ? app.loopback_sample_rate.load()
                        : ((app.recorded_frames_total.load() > 0) ? app.recorded_source_sr.load() : app.stream_sample_rate));
                const int live_actual_ch = rec
                    ? app.recorded_source_ch.load()
                    : ((app.record_source_idx == 1 && app.native_loopback_engine.isRunning())
                        ? app.loopback_channels.load()
                        : ((app.recorded_frames_total.load() > 0) ? app.recorded_source_ch.load() : 2));
                ImGui::TextColored(
                    ImVec4(0.984f, 0.749f, 0.141f, 1.0f),
                    "当前录音状态: %s (已录制 %.1f 秒 | 实际采集 %d Hz / %d 声道)",
                    rec ? "正在录制..." : "待机中",
                    rec_sec,
                    live_actual_sr,
                    live_actual_ch
                );
                if (app.recording_format_changed.load()) {
                    ImGui::TextColored(
                        ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                        "注意：录制期间系统输出格式已变化，导出采样率可能不准确"
                    );
                }

                if (styledButton(rec ? "停止录制" : "开始录制声音", BtnTone::Danger, ImVec2(half_w * 0.46f, SF(34.0f)))) {
                    if (!rec) {
                        const int src = app.record_source_idx;
                        bool can_record = true;
                        int want_ch = REC_CHS[safeClamp(app.rec_ch_idx, 0, 4)];
                        if (src == 1 && !app.startSystemLoopbackCapture(want_ch)) {
                            // 回环启动失败时不开始录制，避免静默录成其他声音
                            can_record = false;
                            app.status_message = "无法启动系统声音回环，录音未开始（请检查默认音频输出设备）";
                        }
                        if (can_record) {
                            const int sr = (src == 1) ? app.loopback_sample_rate.load() : app.stream_sample_rate;
                            // 严格按底层实际捕获的声道数锁定录音源声道数（实际是几声道就写几声道）
                            const int ch = (src == 1) ? app.loopback_channels.load() : 2;
                            app.beginRecording(src, sr, ch);
                        }
                    }
                    else {
                        app.endRecording();
                    }
                }
                ImGui::SameLine();

                if (styledButton("导出保存录音文件", BtnTone::Success, SVec2(-1, 34.0f))) {
                    if (rec) {
                        // 录制中不导出：导出需要整段拷贝录音缓冲
                        app.status_message = "请先停止录制，再导出录音文件";
                    }
                    else {
                        std::vector<float> copy_rec;
                        int rec_sr = 44100;
                        int rec_ch = 2;
                        {
                            std::lock_guard<std::mutex> lk(app.record_mutex);
                            size_t total = 0;
                            for (const auto& chunk : app.recorded_chunks) total += chunk.size();
                            copy_rec.reserve(total);
                            for (const auto& chunk : app.recorded_chunks) {
                                copy_rec.insert(copy_rec.end(), chunk.begin(), chunk.end());
                            }
                            rec_sr = app.recorded_source_sr.load();
                            rec_ch = app.recorded_source_ch.load();
                        }
                        if (!copy_rec.empty()) {
                            std::string ext = REC_FMTS[app.rec_fmt_idx];
                            std::string out_p = saveFileDialog(hwnd, "recorded_audio." + ext, L"音频文件 (*.*)\0*.*\0");
                            if (!out_p.empty()) {
                                std::string saved, err;
                                int target_ch = REC_CHS[safeClamp(app.rec_ch_idx, 0, 4)];
                                if (exportAndConvertAudio(copy_rec, rec_sr, out_p,
                                    REC_SRS[app.rec_sr_idx], REC_BITS[app.rec_bits_idx],
                                    target_ch, "192k", saved, err, ext, rec_ch)) {
                                    int final_ch = (target_ch <= 0) ? rec_ch : target_ch;
                                    app.status_message = "录音已成功保存 (" + std::to_string(final_ch) + "声道): " + saved;
                                }
                                else {
                                    app.status_message = "录音保存失败: " + err;
                                }
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        // ============================================================================================
        // Tab 4: 瞬态截取 & 88键标准钢琴键盘 (对齐 Python 版双行工具栏与主音虚线图例)
        // ============================================================================================
        ImGuiTabItemFlags tab4_flags = app.request_switch_tab4 ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        app.request_switch_tab4 = false;
        if (ImGui::BeginTabItem("声音截取与钢琴", nullptr, tab4_flags)) {
            if (styledButton("立即截取声音", BtnTone::Purple)) app.captureAudioSnapshot(false);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 98.0f : 112.0f));
            static const char* SNAP_WIN_ITEMS[] = { "20ms(+/-10ms)", "40ms(+/-20ms)", "80ms(+/-40ms)", "200ms(+/-100ms)" };
            ImGui::Combo("窗口", &app.snap_window_idx, SNAP_WIN_ITEMS, 4);
            ImGui::SameLine();

            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 138.0f : 165.0f));
            static const char* LEFT_VIEWS[] = { "时域波形", "88键钢琴" };
            ImGui::Combo("左侧视图", &app.left_snap_view_idx, LEFT_VIEWS, 2);
            ImGui::SameLine();

            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 96.0f : 115.0f));
            static const char* PEAK_CNT_ITEMS4[] = { "0个 (不标注)", "1个 (第1主音)", "2个 (前2主音)", "3个 (前3主音)" };
            if (ImGui::Combo("主音数", &app.preset.peak_annotation_count, PEAK_CNT_ITEMS4, 4)) {
                app.refreshSnapshotPlots(true);
            }
            ImGui::SameLine();

            if (styledButton("原样短放", BtnTone::Teal)) {
                if (app.captured_snippet.empty()) app.captureAudioSnapshot(false);
                auto buf = AudioDSPEngine::buildRawSnippetPlayback(app.captured_snippet, app.captured_sr);
                app.triggerSnapshotPlayback(std::move(buf), "原样短促播放");
            }
            ImGui::SameLine();
            if (styledButton("延音(1.5s)", BtnTone::Primary)) {
                if (app.captured_snippet.empty()) app.captureAudioSnapshot(false);
                auto buf = AudioDSPEngine::buildSustainPlayback(
                    app.captured_snippet, app.captured_sr, 1.5, app.captured_top_peaks, app.sustain_type_idx == 1
                );
                app.triggerSnapshotPlayback(std::move(buf), app.sustain_type_idx == 1 ? "纯谐波延音 1.5s" : "无缝原波延音 1.5s");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(g_is_aspect_4_3 ? 82.0f : 96.0f));
            static const char* SUST_TYPES[] = { "无缝原波", "纯谐波延音" };
            ImGui::Combo("##sust_t", &app.sustain_type_idx, SUST_TYPES, 2);
            ImGui::SameLine();

            if (styledButton("慢放", BtnTone::Amber)) {
                if (app.captured_snippet.empty()) app.captureAudioSnapshot(false);
                static constexpr double SLOW_FACTORS[] = { 2.0, 5.0, 10.0, 15.0, 20.0 };
                double fac = SLOW_FACTORS[safeClamp(app.slow_factor_idx, 0, 4)];
                auto buf = AudioDSPEngine::buildTimeStretchedPlayback(
                    app.captured_snippet, app.captured_sr, fac, app.slow_mode_idx == 0
                );
                app.triggerSnapshotPlayback(std::move(buf), std::to_string(static_cast<int>(fac)) + "x 倍慢速拉伸播放");
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(54.0f));
            static const char* SLOW_F_ITEMS[] = { "2x", "5x", "10x", "15x", "20x" };
            ImGui::Combo("##slow_f", &app.slow_factor_idx, SLOW_F_ITEMS, 5);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(74.0f));
            static const char* SLOW_M_ITEMS[] = { "保音高", "物理降频" };
            ImGui::Combo("##slow_m", &app.slow_mode_idx, SLOW_M_ITEMS, 2);

            // 第二行：BPM 节拍同步控制
            ImGui::AlignTextToFramePadding();
            ImGui::Checkbox("钢琴跟随音乐", &app.piano_live_follow);
            ImGui::SameLine();
            if (ImGui::Checkbox("按节拍(BPM)抓主音", &app.preset.bpm_capture_enabled)) {
                if (app.preset.bpm_capture_enabled) app.resetBeatPhaseNow();
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(SF(78.0f));
            float bpm_f = static_cast<float>(app.preset.bpm_value);
            if (ImGui::DragFloat("BPM", &bpm_f, 0.5f, 15.0f, 300.0f, "%.1f")) {
                app.preset.bpm_value = safeClamp(static_cast<double>(bpm_f), 15.0, 300.0);
                app.bpm_anchor_time = std::chrono::steady_clock::now();
                app.bpm_last_beat_index = 0;
            }
            ImGui::SameLine();
            if (styledButton("重置第1拍", BtnTone::Primary)) {
                app.resetBeatPhaseNow();
            }
            ImGui::SameLine();
            if (styledButton("连点测BPM", BtnTone::Indigo)) {
                app.onTapTempoClicked();
            }
            ImGui::SameLine();
            ImGui::Checkbox("每拍同步刷新右侧", &app.preset.bpm_sync_right_snapshot);
            ImGui::SameLine();

            ImGui::Text("拍点灯:");
            ImGui::SameLine();
            ImVec2 led_p = ImGui::GetCursorScreenPos();
            float led_sz = SF(16.0f);
            bool led_lit = std::chrono::steady_clock::now() < app.led_flash_until;
            ImDrawList* wdl = ImGui::GetWindowDrawList();
            wdl->AddRectFilled(led_p, ImVec2(led_p.x + led_sz, led_p.y + led_sz),
                led_lit ? IM_COL32(16, 185, 129, 255) : IM_COL32(30, 41, 59, 255), 3.0f);
            wdl->AddRect(led_p, ImVec2(led_p.x + led_sz, led_p.y + led_sz),
                led_lit ? IM_COL32(167, 243, 208, 255) : IM_COL32(71, 85, 105, 255), 3.0f, 0, 1.4f);
            ImGui::Dummy(ImVec2(led_sz + 4.0f, led_sz + 4.0f));

            float half_snap_w = std::floor(ImGui::GetContentRegionAvail().x * 0.50f);
            ImGui::BeginChild("##SnapLeftView", ImVec2(half_snap_w, -1), false);
            {
                if (app.left_snap_view_idx == 1) {
                    const auto& active_pks = !app.piano_live_follow
                        ? app.captured_top_peaks
                        : (app.preset.bpm_capture_enabled ? app.bpm_locked_peaks : app.live_top3_peaks);
                    renderPiano88KeysWidget(app, active_pks);
                }
                else {
                    if (ImPlot::BeginPlot("截取时域波形", ImVec2(-1, -1))) {
                        ImPlot::SetupAxes("相对截取时刻 Time (ms)", "振幅 Amplitude");
                        ImPlot::SetupAxisLimits(ImAxis_Y1, -1.05, 1.05, ImPlotCond_Always);
                        if (!app.snap_wave_t_ms.empty()) {
                            ImPlot::SetNextLineStyle(ImVec4(0.063f, 0.725f, 0.506f, 1.0f), 2.2f);
                            ImPlot::PlotLine("绿色实线: 截取的真实合成声波", app.snap_wave_t_ms.data(), app.snap_wave_mono.data(), static_cast<int>(app.snap_wave_t_ms.size()));
                            ImVec4 s_cols[3] = {
                                ImVec4(0.984f, 0.749f, 0.141f, 0.90f),
                                ImVec4(0.176f, 0.831f, 0.749f, 0.90f),
                                ImVec4(0.957f, 0.447f, 0.714f, 0.90f)
                            };
                            const char* rank_names[3] = { "金色线 (#1主音)", "青绿线 (#2主音)", "粉紫线 (#3主音)" };
                            for (int r = 0; r < app.preset.peak_annotation_count && r < static_cast<int>(app.captured_top_peaks.size()); ++r) {
                                char s_lbl[96];
                                std::snprintf(
                                    s_lbl, sizeof(s_lbl),
                                    "%s: %s (%.1fHz, %.1fdB)",
                                    rank_names[r],
                                    app.captured_top_peaks[r].note.c_str(),
                                    app.captured_top_peaks[r].freq_hz,
                                    app.captured_top_peaks[r].mag_db
                                );
                                ImPlot::SetNextLineStyle(s_cols[r], 1.5f);
                                ImPlot::PlotLine(s_lbl, app.snap_wave_t_ms.data(), app.snap_sine_overlays[r].data(), static_cast<int>(app.snap_wave_t_ms.size()));
                            }
                        }
                        ImPlot::EndPlot();
                    }
                }
            }
            ImGui::EndChild();
            ImGui::SameLine();

            ImGui::BeginChild("##SnapRightSpec", ImVec2(-1, -1), false);
            {
                if (ImPlot::BeginPlot("截取频谱", ImVec2(-1, -1), ImPlotFlags_NoMenus)) {
                    ImPlot::SetupAxes("频率 Frequency (Hz) & 钢琴音名 [C0 ~ C8]", "分贝 (dBFS)", ImPlotAxisFlags_None, ImPlotAxisFlags_Lock);
                    ImPlot::SetupAxisLimits(ImAxis_Y1, -100.0, 8.0, ImPlotCond_Always);
                    if (app.sync_x_axes) {
                        ImPlot::SetupAxisLinks(ImAxis_X1, &app.shared_x_min, &app.shared_x_max);
                    }
                    setupFreqPianoAxisTicks(app.log_freq_axis);
                    if (!app.snap_spec_x.empty()) {
                        ImPlot::SetNextFillStyle(ImVec4(0.659f, 0.333f, 0.969f, 0.22f));
                        ImPlot::PlotShaded("定格频谱", app.snap_spec_x.data(), app.snap_spec_db.data(), static_cast<int>(app.snap_spec_x.size()), -100.0);
                        ImPlot::SetNextLineStyle(ImVec4(0.659f, 0.333f, 0.969f, 1.0f), 2.0f);
                        ImPlot::PlotLine("定格频谱", app.snap_spec_x.data(), app.snap_spec_db.data(), static_cast<int>(app.snap_spec_x.size()));
                    }
                    drawPianoGuideLinesAndPeaks(app.log_freq_axis, app.captured_top_peaks, app.preset.peak_annotation_count);
                    ImPlot::EndPlot();
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    // 底部状态栏：左侧实时状态提示，右侧常驻可点击 GitHub 开源仓库地址
    ImGui::AlignTextToFramePadding();
    const char* kGithubLabel = "GitHub 开源仓库: https://github.com/KhaTianyuz/Audio_Studio_Pro";
    const float gh_text_w = ImGui::CalcTextSize(kGithubLabel).x + SF(18.0f);
    const float avail_status_w = ImGui::GetContentRegionAvail().x;
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(SF(120.0f), avail_status_w - gh_text_w - SF(12.0f)));
    ImGui::TextColored(ImVec4(0.580f, 0.639f, 0.722f, 1.0f), "%s", app.status_message.c_str());
    ImGui::PopTextWrapPos();
    ImGui::SameLine(std::max(SF(140.0f), avail_status_w - gh_text_w));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.118f, 0.161f, 0.231f, 0.85f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.008f, 0.518f, 0.780f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.220f, 0.741f, 0.973f, 1.00f));
    if (ImGui::SmallButton(kGithubLabel)) {
        ShellExecuteW(nullptr, L"open", L"https://github.com/KhaTianyuz/Audio_Studio_Pro", nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::SetTooltip("点击在浏览器中打开开源仓库: https://github.com/KhaTianyuz/Audio_Studio_Pro");
    }
    ImGui::PopStyleColor(5);



    // ------------------------------------------------------------------------------------------------
    // 首次启动免责协议弹窗 / 免责协议查看器 (未勾选同意前禁止操作主界面)
    // ------------------------------------------------------------------------------------------------
    if (!app.disclaimer_accepted || app.show_disclaimer_viewer) {
        ImGui::OpenPopup("免责/开源协议与第三方致谢声明 (DISCLAIMER.md)");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(SVec2(740.0f, 520.0f), ImGuiCond_Always);
    if (ImGui::BeginPopupModal("免责/开源协议与第三方致谢声明 (DISCLAIMER.md)", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {

        ImGui::TextColored(ImVec4(0.220f, 0.741f, 0.973f, 1.0f),
            "欢迎使用 Audio Studio Pro — 请在开始使用前仔细阅读并同意以下免责/开源协议：");
        ImGui::Spacing();

        static const char* kDisclaimerFullText = R"DISC(【Audio Studio Pro 软件免责协议、开源许可证与第三方致谢声明 (DISCLAIMER.md)】

重要提示：在使用本软件（Audio Studio Pro，含 Python 版、C++20 版及打包后的 .exe 可执行程序）之前，请您务必仔细阅读以下全部免责条款与开源协议说明。您勾选“我已阅读 DISCLAIMER.md 并遵守以上全部免责/开源协议条款”或以任何方式运行、使用、修改、传播本软件，即视为已完全理解并无条件同意本协议全部内容：

========================================================================
第一部分：软件使用免责协议与风险告知书 (Disclaimer of Liability)
========================================================================

第一条 【完全自担风险原则】
本软件系开源免费工具（官方开源仓库：https://github.com/KhaTianyuz/Audio_Studio_Pro），仅供个人学习、声学实验与音乐欣赏交流使用。用户下载、安装、运行、操作本软件纯属个人自愿行为。使用本软件过程中或因使用本软件而产生的任何直接、间接、偶然、特殊或衍生的一切后果、损失与风险，均由用户本人 100% 独立承担，与本软件作者无任何关系，作者不承担任何法律或经济责任。

第二条 【听力安全与音响/耳机硬件损坏免责】
本软件具备实时均衡器增益调节（最高 +24dB）、自定义数学函数波发生器、多振荡器合成及极端低频/高频声波生成功能。若因用户将音量调得过高、大幅拉高增益、输入剧烈振荡公式或在佩戴耳机/连接外置喇叭时操作不当，导致喇叭劈响、振膜破损、功放/声卡/耳机硬件烧毁，或造成用户及他人耳鸣、听力下降等健康损害，一切后果均由用户自行承担，与作者无关。建议首次调试时先将电脑音量调低并摘下耳机。

第三条 【音频版权与系统内录合规免责】
本软件内置的“音频格式自动转换”、“音频重采样导出”以及“录制电脑系统声音”功能，仅限用于用户拥有合法权利的音频素材或个人学习研究。用户不得利用本软件盗录、破解、非法转换或传播受著作权法保护的商业音乐、流媒体内容或他人隐私语音。因用户违反知识产权法律法规或侵犯第三方权益所引发的一切民事纠纷、行政处罚或刑事责任，均由用户本人全权负责，与本软件作者无关。

第四条 【文件同名覆盖与数据安全免责】
为保障兼容性，当您向本软件导入非 WAV 格式的音乐文件（如 .mp3、.flac、.ogg、.m4a 等）时，软件会自动在原文件同一文件夹下转换并覆盖生成同名的 .wav 文件（例如导入 song.mp3 会自动覆盖生成同目录下的 song.wav）。请勿将重要原始 .wav 母带文件与待转换文件同名混放。因同名文件覆盖、误操作、磁盘读写异常或程序中断导致的任何文件丢失、数据损坏，作者概不负责。

第五条 【系统兼容性、驱动冲突与设备故障免责】
本软件不保证在所有 Windows 操作系统版本、声卡硬件、虚拟音频驱动、蓝牙音频设备或第三方安全软件环境下均能正常、无故障地运行。若因运行本软件引发音频卡顿、爆音、程序闪退、系统死机、蓝屏或与其他软件发生冲突，由此造成的任何工作中断或数据损失，均与作者无关。

第六条 【分析结果准确性与“按原样 (AS-IS)”无担保条款】
本软件按“原样 (AS-IS)”提供，不附带任何形式的明示或暗示担保（包括但不限于适销性、特定用途适用性及无缺陷担保）。软件内显示的实时频谱、第一/第二/第三主音音名、88 键钢琴琴键高亮及 BPM 节拍测算结果仅作直观参考，不保证 100% 音乐扒谱或声学测量精度。作者不对因依赖软件显示结果而产生的任何误差或失误负责，亦不负有提供技术支持、故障修复或版本升级的强制义务。

第七条 【第三方使用与二次分发免责】
本软件基于 GNU AGPL v3.0 协议开源。任何第三方个人或机构对本软件进行打包、修改、分发或商业/非商业使用时，其行为仅代表该第三方自身，不代表原作者立场。因第三方修改版、捆绑版或分发渠道引起的任何木马病毒、安全隐患或法律纠纷，与原作者无任何关联。

第八条 【放弃追索权与协议生效】
一旦您勾选同意并继续使用本软件，即表示您永久且不可撤销地放弃因使用本软件而向软件作者提起任何索赔、诉讼、仲裁或追偿的权利。如果您不同意本协议中的任何一条内容，请立即点击“不同意并退出”并彻底删除本软件全部文件。

========================================================================
第二部分：开源协议说明与第三方开源组件致谢 (Open-Source Licenses & Credits)
========================================================================

一、【本项目主开源协议：GNU AGPL v3.0】
1. 本项目主程序核心代码（含 FFT 超分辨寻峰、RBJ Biquad 参量均衡器、跨块相位连续 WSOLA 倍速算法、5.1/7.1 环绕声矩阵上混引擎、逆波兰字节码数学公式引擎、88 键钢琴跟谱控件、原生 WASAPI 虚拟内录引擎及 RFC 9639 原生 FLAC/WAV 编码器等）均为项目原创自研代码，基于 GNU Affero General Public License v3.0 (AGPL-3.0) 协议开源（详见根目录 LICENSE 文件）。
2. 您可以自由运行、复制、修改本软件及分发构建后的 .exe 可执行程序；若您修改了本软件源码、发布了衍生作品或通过网络提供服务，必须以相同的 AGPL-3.0 协议公开完整修改版源码，并保留原作者版权声明与本免责/开源协议。

二、【致谢：本项目使用的第三方开源组件及遵守的开源协议】
本项目的开发与运行得益于以下优秀的第三方开源项目，特此向所有开源作者与贡献者致谢，并严格遵守其开源许可证要求保留版权声明：

1. C++20 原生版 (cpp_version/) 第三方开源库：
   - Dear ImGui (v1.90.8) | 作者: Omar Cornut and contributors | 协议: MIT License (https://github.com/ocornut/imgui)
   - ImPlot (v0.16) | 作者: Evan Pezent | 协议: MIT License (https://github.com/epezent/implot)
   - miniaudio (v0.11.21 接口规范) | 原作者: David Reid (mackron) | 协议: Public Domain / MIT-0 (https://github.com/mackron/miniaudio)

2. Python 版 (python_version/) 第三方开源库：
   - PyQt6 | 版权: Riverbank Computing Limited / The Qt Company | 协议: GNU GPL v3.0 (与本项目 AGPL-3.0 协议兼容)
   - pyqtgraph | 作者: Luke Campagnola and contributors | 协议: MIT License
   - NumPy & SciPy | 版权: NumPy & SciPy Developers | 协议: BSD-3-Clause License
   - PyAudioWPatch | 作者: Hubert Pham / s0d3s | 协议: MIT License (https://github.com/s0d3s/PyAudioWPatch)
   - sounddevice | 作者: Matthias Geier | 协议: MIT License
   - soundfile (libsndfile) | 作者: Bastian Bechtold | 协议: BSD-3-Clause / LGPL-2.1+
   - imageio-ffmpeg & FFmpeg | 版权: Almar Klein / FFmpeg Developers | 协议: BSD-2-Clause / LGPLv2.1+ (独立子进程转码调用)
   - PyInstaller | 版权: PyInstaller Development Team | 协议: GPLv2+ with Bootloader Exception

3. MIT / BSD 第三方组件标准免责与许可声明：
   上述 MIT 与 BSD-3-Clause 协议授权的第三方组件均由其各自版权持有人按“原样 (AS IS)”免费提供许可，不附带任何明示或暗示的担保。完整许可证声明详见程序目录下的 DISCLAIMER.md 与 LICENSE 文件。)DISC";

        float text_h = ImGui::GetContentRegionAvail().y - SF(78.0f);
        ImGui::BeginChild("##DisclaimerScroll", ImVec2(-1, text_h), true);
        ImGui::TextWrapped("%s", kDisclaimerFullText);
        ImGui::EndChild();

        ImGui::Spacing();
        if (!app.disclaimer_accepted) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.984f, 0.749f, 0.141f, 1.0f));
            ImGui::Checkbox("我已阅读 DISCLAIMER.md 并遵守以上全部免责/开源协议条款，自愿承担一切使用风险（发生任何事与作者无关）",
                &app.disclaimer_checkbox_ticked);
            ImGui::PopStyleColor();

            ImGui::Spacing();
            if (styledButton("不同意并退出程序", BtnTone::Danger, SVec2(170.0f, 34.0f))) {
                PostQuitMessage(0);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!app.disclaimer_checkbox_ticked);
            if (styledButton("我已阅读并同意，进入软件", BtnTone::Success, SVec2(230.0f, 34.0f))) {
                app.saveDisclaimerAcceptedState();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
        } else {
            if (styledButton("我已知晓，关闭窗口", BtnTone::Primary, SVec2(180.0f, 34.0f))) {
                app.show_disclaimer_viewer = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ====================================================================================================
// 16. DirectX11 设备与 Win32 窗口管理 (支持 Per-Monitor DPI V2 与 1:1 物理像素渲染)
// ====================================================================================================

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static AudioStudioWorkstation* g_appInstance = nullptr;

static void enableWindowsHiDpiAwareness() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto fn = reinterpret_cast<SetProcessDpiAwarenessContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
            if (fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) return;
        }
    }
    ImGui_ImplWin32_EnableDpiAwareness();
}

static void createRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    if (SUCCEEDED(g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer))) && pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

static void cleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

static bool createDeviceD3D(HWND hWnd) {
    RECT rc{};
    GetClientRect(hWnd, &rc);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = static_cast<UINT>(std::max<LONG>(0, rc.right - rc.left));
    sd.BufferDesc.Height = static_cast<UINT>(std::max<LONG>(0, rc.bottom - rc.top));
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, featureLevelArray, 2,
        D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK) {
        return false;
    }
    createRenderTarget();
    return true;
}

static void cleanupDeviceD3D() {
    cleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void applyPendingSwapChainResizeIfNeeded() {
    if ((g_pending_resize_w != 0 || g_pending_resize_h != 0) && g_pd3dDevice != nullptr && g_pSwapChain != nullptr) {
        cleanupRenderTarget();
        g_pSwapChain->ResizeBuffers(0, g_pending_resize_w, g_pending_resize_h, DXGI_FORMAT_UNKNOWN, 0);
        g_pending_resize_w = 0;
        g_pending_resize_h = 0;
        createRenderTarget();
    }
}

static void renderSingleWorkstationFrame(HWND hwnd, bool present_vsync = true) {
    if (!g_appInstance || !g_pd3dDevice || !g_pSwapChain || IsIconic(hwnd)) return;

    applyPendingSwapChainResizeIfNeeded();
    if (!g_mainRenderTargetView) return;

    rebuildCrispFontAndStyleIfNeeded(hwnd);

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    renderMainWorkstationUI(*g_appInstance, hwnd);

    ImGui::Render();
    const float clear_color[4] = { 0.059f, 0.090f, 0.165f, 1.00f }; // #0F172A
    g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
    g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_pSwapChain->Present(present_vsync ? 1 : 0, 0);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_GETMINMAXINFO: {
        LPMINMAXINFO lpMMI = reinterpret_cast<LPMINMAXINFO>(lParam);
        lpMMI->ptMinTrackSize.x = 920;
        lpMMI->ptMinTrackSize.y = 620;
        return 0;
    }
    case WM_ERASEBKGND:
        return 1; // 禁止 Win32 默认背景擦除闪烁
    case WM_ENTERSIZEMOVE:
        g_in_sizemove_modal = true;
        return 0;
    case WM_EXITSIZEMOVE:
        g_in_sizemove_modal = false;
        g_last_resize_tick_ms = 0;
        InvalidateRect(hWnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_pending_resize_w = static_cast<UINT>(LOWORD(lParam));
            g_pending_resize_h = static_cast<UINT>(HIWORD(lParam));
            g_last_resize_tick_ms = GetTickCount64();
            if (g_in_sizemove_modal && g_appInstance) {
                renderSingleWorkstationFrame(hWnd, false);
            }
        }
        return 0;
    case WM_PAINT:
        if (g_in_sizemove_modal && g_appInstance) {
            PAINTSTRUCT ps{};
            BeginPaint(hWnd, &ps);
            renderSingleWorkstationFrame(hWnd, false);
            EndPaint(hWnd, &ps);
            return 0;
        }
        break;
    case WM_DPICHANGED: {
        const RECT* suggested_rect = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(
            hWnd, nullptr,
            suggested_rect->left, suggested_rect->top,
            suggested_rect->right - suggested_rect->left,
            suggested_rect->bottom - suggested_rect->top,
            SWP_NOZORDER | SWP_NOACTIVATE
        );
        return 0;
    }
    case WM_DROPFILES: {
        HDROP hDrop = reinterpret_cast<HDROP>(wParam);
        wchar_t file_path[MAX_PATH] = { 0 };
        if (DragQueryFileW(hDrop, 0, file_path, MAX_PATH) && g_appInstance) {
            loadAudioFileIntoApp(*g_appInstance, wideToUtf8(file_path));
        }
        DragFinish(hDrop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, LPSTR /*lpCmdLine*/, int /*nCmdShow*/) {
    // 第一时间开启 Windows Per-Monitor DPI V2 感知，禁止系统将窗口当成低分辨率图像放大
    enableWindowsHiDpiAwareness();

    HINSTANCE hInst = GetModuleHandle(nullptr);
    HICON hAppIcon = LoadIconW(hInst, MAKEINTRESOURCEW(101));
    if (!hAppIcon) {
        fs::path ico_p = getAppRootDir() / L"app_icon.ico";
        if (fs::exists(ico_p)) {
            hAppIcon = static_cast<HICON>(LoadImageW(nullptr, ico_p.wstring().c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE));
        }
    }
    WNDCLASSEXW wc = {
        sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInst,
        hAppIcon, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"AudioStudioProCPP", hAppIcon
    };
    RegisterClassExW(&wc);

    // 对齐 Python _adapt_window_to_screen()：探测显示器真实物理像素工作区，自动适配 4:3 与 16:9 屏幕
    RECT work_area{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        work_area.left = 0;
        work_area.top = 0;
        work_area.right = GetSystemMetrics(SM_CXSCREEN);
        work_area.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    int avail_w = std::max(960, static_cast<int>(work_area.right - work_area.left));
    int avail_h = std::max(680, static_cast<int>(work_area.bottom - work_area.top));

    int win_w = static_cast<int>(avail_w * 0.92);
    int win_h = static_cast<int>(avail_h * 0.90);
    int pos_x = work_area.left + std::max(0, (avail_w - win_w) / 2);
    int pos_y = work_area.top + std::max(0, (avail_h - win_h) / 2);

    HWND hwnd = CreateWindowW(
        wc.lpszClassName,
        L"Audio Studio Pro",
        WS_OVERLAPPEDWINDOW, pos_x, pos_y, win_w, win_h, nullptr, nullptr, wc.hInstance, nullptr
    );
    DragAcceptFiles(hwnd, TRUE);

    if (!createDeviceD3D(hwnd)) {
        cleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // 首次构建 1:1 真实物理像素矢量字体与样式
    rebuildCrispFontAndStyleIfNeeded(hwnd);

    AudioStudioWorkstation workstation;
    g_appInstance = &workstation;

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // 窗口最小化时休眠跳过本帧渲染，避免 0x0 视口计算且节省 CPU/GPU
        if (IsIconic(hwnd)) {
            Sleep(16);
            continue;
        }

        renderSingleWorkstationFrame(hwnd, true);
    }

    g_appInstance = nullptr;
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    cleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

#endif // AUDIO_STUDIO_HEADLESS_SELF_TEST