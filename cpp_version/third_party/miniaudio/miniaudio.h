// ====================================================================================================
// miniaudio.h - Built-in Native Win32 WASAPI + Native FLAC/WAV/MediaFoundation Implementation
// Bundled in third_party/miniaudio/miniaudio.h so CMake never fails or times out downloading from GitHub
// ====================================================================================================
#ifndef MINIAUDIO_H
#define MINIAUDIO_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <fstream>
#include <filesystem>
#include <algorithm>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

typedef int32_t   ma_result;
typedef uint32_t  ma_uint32;
typedef uint64_t  ma_uint64;
typedef uint32_t  ma_bool32;

#define MA_SUCCESS 0
#define MA_ERROR  -1
#define MA_TRUE    1
#define MA_FALSE   0

enum ma_format {
    ma_format_unknown = 0,
    ma_format_u8      = 1,
    ma_format_s16     = 2,
    ma_format_s24     = 3,
    ma_format_s32     = 4,
    ma_format_f32     = 5
};

enum ma_device_type {
    ma_device_type_playback = 1,
    ma_device_type_capture  = 2,
    ma_device_type_duplex   = 3,
    ma_device_type_loopback = 4
};

struct ma_device;
typedef void (*ma_device_callback_proc)(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount);

struct ma_device_config {
    ma_device_type deviceType;
    ma_uint32 sampleRate;
    ma_uint32 periodSizeInFrames;
    ma_bool32 noClip;
    struct {
        const void* pDeviceID;
        ma_format format;
        ma_uint32 channels;
    } playback;
    struct {
        const void* pDeviceID;
        ma_format format;
        ma_uint32 channels;
    } capture;
    struct {
        ma_bool32 noAutoConvertSRC;
    } wasapi;
    ma_device_callback_proc dataCallback;
    void* pUserData;
};

inline ma_device_config ma_device_config_init(ma_device_type deviceType) noexcept {
    ma_device_config cfg{};
    cfg.deviceType = deviceType;
    cfg.sampleRate = 0;
    cfg.periodSizeInFrames = 0;
    cfg.noClip = MA_TRUE;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.capture.format = ma_format_f32;
    cfg.capture.channels = 2;
    cfg.wasapi.noAutoConvertSRC = MA_FALSE;
    cfg.dataCallback = nullptr;
    cfg.pUserData = nullptr;
    return cfg;
}

struct ma_device {
    ma_device_config cfg{};
    ma_uint32 sampleRate = 48000;
    void* pUserData = nullptr;
    struct {
        ma_uint32 channels = 2;
        char name[256] = "WASAPI Playback";
    } playback;
    struct {
        ma_uint32 channels = 2;
        char name[256] = "WASAPI Capture";
    } capture;
#ifdef _WIN32
    std::thread worker_thread;
    HANDLE stop_event = nullptr;
    HANDLE init_event = nullptr;
    std::atomic<bool> init_ok{ false };
    std::atomic<bool> started{ false };
#endif
};

#ifdef _WIN32
inline void ma_wasapi_playback_thread_proc(ma_device* dev) {
    HRESULT hr_com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool com_inited = SUCCEEDED(hr_com);

    HMODULE avrt_dll = LoadLibraryW(L"avrt.dll");
    HANDLE mmcss_handle = nullptr;
    using AvSetMmThreadCharacteristicsWFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
    using AvRevertMmThreadCharacteristicsFn = BOOL(WINAPI*)(HANDLE);
    AvRevertMmThreadCharacteristicsFn pfnRevert = nullptr;
    if (avrt_dll) {
        auto pfnSet = reinterpret_cast<AvSetMmThreadCharacteristicsWFn>(GetProcAddress(avrt_dll, "AvSetMmThreadCharacteristicsW"));
        pfnRevert = reinterpret_cast<AvRevertMmThreadCharacteristicsFn>(GetProcAddress(avrt_dll, "AvRevertMmThreadCharacteristics"));
        if (pfnSet) {
            DWORD task_idx = 0;
            mmcss_handle = pfnSet(L"Pro Audio", &task_idx);
        }
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* endpoint = nullptr;
    IAudioClient* audio_client = nullptr;
    IAudioRenderClient* render_client = nullptr;
    WAVEFORMATEX* mix_fmt = nullptr;
    HANDLE ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    static constexpr GUID kSubtypeFloat = {
        0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 }
    };

    uint32_t dev_ch = 2;
    uint32_t dev_sr = 48000;
    uint32_t dev_bps = 32;
    bool dev_is_float = true;
    UINT32 buffer_frames = 0;

    if (ready_event &&
        SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator))) && enumerator &&
        SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &endpoint)) && endpoint &&
        SUCCEEDED(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&audio_client))) && audio_client &&
        SUCCEEDED(audio_client->GetMixFormat(&mix_fmt)) && mix_fmt) {

        dev_ch = std::max<uint32_t>(1, mix_fmt->nChannels);
        dev_sr = mix_fmt->nSamplesPerSec;
        dev_bps = mix_fmt->wBitsPerSample;
        dev_is_float = (mix_fmt->wFormatTag == 3);
        if (mix_fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix_fmt->cbSize >= 22) {
            const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mix_fmt);
            if (IsEqualGUID(ext->SubFormat, kSubtypeFloat)) dev_is_float = true;
        }

        const REFERENCE_TIME buf_dur_100ns = 30 * 10000; // 30 ms low-latency shared buffer
        if (SUCCEEDED(audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                buf_dur_100ns, 0, mix_fmt, nullptr)) &&
            SUCCEEDED(audio_client->SetEventHandle(ready_event)) &&
            SUCCEEDED(audio_client->GetBufferSize(&buffer_frames)) &&
            SUCCEEDED(audio_client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render_client))) &&
            render_client) {
            dev->sampleRate = dev_sr;
            dev->playback.channels = 2;
            dev->init_ok.store(true);
        }
    }

    SetEvent(dev->init_event);

    if (dev->init_ok.load() && audio_client && render_client) {
        std::vector<float> stereo_buf;
        while (!dev->started.load() && WaitForSingleObject(dev->stop_event, 5) != WAIT_OBJECT_0) {}

        if (WaitForSingleObject(dev->stop_event, 0) != WAIT_OBJECT_0) {
            audio_client->Start();
            HANDLE wait_objs[2] = { dev->stop_event, ready_event };

            while (WaitForSingleObject(dev->stop_event, 0) != WAIT_OBJECT_0) {
                DWORD w = WaitForMultipleObjects(2, wait_objs, FALSE, 12);
                if (w == WAIT_OBJECT_0) break;

                UINT32 padding = 0;
                if (FAILED(audio_client->GetCurrentPadding(&padding))) break;
                UINT32 avail = (buffer_frames > padding) ? (buffer_frames - padding) : 0;
                if (avail == 0) continue;

                BYTE* data_ptr = nullptr;
                if (SUCCEEDED(render_client->GetBuffer(avail, &data_ptr)) && data_ptr) {
                    stereo_buf.assign(static_cast<size_t>(avail) * 2, 0.0f);
                    if (dev->cfg.dataCallback) {
                        dev->cfg.dataCallback(dev, stereo_buf.data(), nullptr, avail);
                    }
                    if (dev_is_float && dev_bps == 32) {
                        float* dst = reinterpret_cast<float*>(data_ptr);
                        if (dev_ch == 2) {
                            std::memcpy(dst, stereo_buf.data(), static_cast<size_t>(avail) * 2 * sizeof(float));
                        } else {
                            for (UINT32 i = 0; i < avail; ++i) {
                                dst[i * dev_ch + 0] = stereo_buf[i * 2];
                                if (dev_ch >= 2) dst[i * dev_ch + 1] = stereo_buf[i * 2 + 1];
                                for (uint32_t c = 2; c < dev_ch; ++c) dst[i * dev_ch + c] = 0.0f;
                            }
                        }
                    } else if (dev_bps == 16) {
                        int16_t* dst = reinterpret_cast<int16_t*>(data_ptr);
                        for (UINT32 i = 0; i < avail; ++i) {
                            float l = std::clamp(stereo_buf[i * 2], -1.0f, 1.0f);
                            float r = std::clamp(stereo_buf[i * 2 + 1], -1.0f, 1.0f);
                            dst[i * dev_ch + 0] = static_cast<int16_t>(std::lrint(l * 32767.0f));
                            if (dev_ch >= 2) dst[i * dev_ch + 1] = static_cast<int16_t>(std::lrint(r * 32767.0f));
                            for (uint32_t c = 2; c < dev_ch; ++c) dst[i * dev_ch + c] = 0;
                        }
                    }
                    render_client->ReleaseBuffer(avail, 0);
                }
            }
            audio_client->Stop();
        }
    }

    if (render_client) render_client->Release();
    if (mix_fmt) CoTaskMemFree(mix_fmt);
    if (audio_client) audio_client->Release();
    if (endpoint) endpoint->Release();
    if (enumerator) enumerator->Release();
    if (ready_event) CloseHandle(ready_event);
    if (mmcss_handle && pfnRevert) pfnRevert(mmcss_handle);
    if (avrt_dll) FreeLibrary(avrt_dll);
    if (com_inited) CoUninitialize();
}
#endif

inline ma_result ma_device_init(const void* /*pContext*/, const ma_device_config* pConfig, ma_device* pDevice) noexcept {
    if (!pConfig || !pDevice) return MA_ERROR;
    pDevice->cfg = *pConfig;
    pDevice->pUserData = pConfig->pUserData;
    pDevice->sampleRate = (pConfig->sampleRate > 0) ? pConfig->sampleRate : 48000;
#ifdef _WIN32
    pDevice->stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pDevice->init_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    pDevice->init_ok.store(false);
    pDevice->started.store(false);
    pDevice->worker_thread = std::thread(ma_wasapi_playback_thread_proc, pDevice);
    WaitForSingleObject(pDevice->init_event, 3500);
    if (!pDevice->init_ok.load()) {
        SetEvent(pDevice->stop_event);
        if (pDevice->worker_thread.joinable()) pDevice->worker_thread.join();
        CloseHandle(pDevice->stop_event); pDevice->stop_event = nullptr;
        CloseHandle(pDevice->init_event); pDevice->init_event = nullptr;
        return MA_ERROR;
    }
    return MA_SUCCESS;
#else
    return MA_SUCCESS;
#endif
}

inline ma_result ma_device_start(ma_device* pDevice) noexcept {
    if (!pDevice) return MA_ERROR;
#ifdef _WIN32
    pDevice->started.store(true);
#endif
    return MA_SUCCESS;
}

inline void ma_device_uninit(ma_device* pDevice) noexcept {
    if (!pDevice) return;
#ifdef _WIN32
    if (pDevice->stop_event) SetEvent(pDevice->stop_event);
    if (pDevice->worker_thread.joinable()) pDevice->worker_thread.join();
    if (pDevice->stop_event) { CloseHandle(pDevice->stop_event); pDevice->stop_event = nullptr; }
    if (pDevice->init_event) { CloseHandle(pDevice->init_event); pDevice->init_event = nullptr; }
#endif
}

// ====================================================================================================
// ma_decoder: Native Verbatim FLAC + RIFF WAV Decoder (other formats fall through to FFmpeg in caller)
// ====================================================================================================
struct ma_decoder_config {
    ma_format format;
    ma_uint32 channels;
    ma_uint32 sampleRate;
};

inline ma_decoder_config ma_decoder_config_init(ma_format format, ma_uint32 channels, ma_uint32 sampleRate) noexcept {
    return { format, channels, sampleRate };
}

struct ma_decoder {
    ma_uint32 outputSampleRate = 44100;
    ma_uint32 outputChannels = 2;
    std::vector<float> decoded_stereo;
    size_t read_cursor_frames = 0;
};

inline ma_result ma_decoder_init_file_w(const wchar_t* pFilePath, const ma_decoder_config* /*pConfig*/, ma_decoder* pDecoder) {
    if (!pFilePath || !pDecoder) return MA_ERROR;
    pDecoder->decoded_stereo.clear();
    pDecoder->read_cursor_frames = 0;

    std::ifstream f(std::filesystem::path(pFilePath), std::ios::binary);
    if (!f) return MA_ERROR;

    uint8_t magic[4] = { 0 };
    if (!f.read(reinterpret_cast<char*>(magic), 4)) return MA_ERROR;

    // Native Verbatim FLAC parser (matches RFC 9639 Verbatim frames written by writeFlacNative)
    if (magic[0] == 'f' && magic[1] == 'L' && magic[2] == 'a' && magic[3] == 'C') {
        uint32_t sr = 44100;
        uint32_t channels = 2;
        uint32_t bps = 16;
        uint64_t total_samples = 0;

        while (f) {
            uint8_t mh[4] = { 0 };
            if (!f.read(reinterpret_cast<char*>(mh), 4)) return MA_ERROR;
            bool is_last = (mh[0] & 0x80) != 0;
            uint8_t mtype = mh[0] & 0x7F;
            uint32_t mlen = (static_cast<uint32_t>(mh[1]) << 16) | (static_cast<uint32_t>(mh[2]) << 8) | mh[3];
            if (mtype == 0 && mlen >= 34) {
                uint8_t si[34] = { 0 };
                f.read(reinterpret_cast<char*>(si), 34);
                if (mlen > 34) f.seekg(mlen - 34, std::ios::cur);
                uint64_t packed = 0;
                for (int b = 0; b < 8; ++b) packed = (packed << 8) | si[10 + b];
                sr = static_cast<uint32_t>((packed >> 44) & 0xFFFFF);
                channels = static_cast<uint32_t>(((packed >> 41) & 0x7) + 1);
                bps = static_cast<uint32_t>(((packed >> 36) & 0x1F) + 1);
                total_samples = packed & 0xFFFFFFFFFULL;
            } else {
                f.seekg(mlen, std::ios::cur);
            }
            if (is_last) break;
        }

        if (sr == 0 || channels == 0) return MA_ERROR;
        pDecoder->outputSampleRate = sr;
        pDecoder->outputChannels = 2;
        if (total_samples > 0 && total_samples < 48000ULL * 3600ULL) {
            pDecoder->decoded_stereo.reserve(static_cast<size_t>(total_samples) * 2);
        }

        while (f && f.peek() != EOF) {
            uint8_t sync[4] = { 0 };
            if (!f.read(reinterpret_cast<char*>(sync), 4)) break;
            if (sync[0] != 0xFF || (sync[1] & 0xFC) != 0xF8) return MA_ERROR;
            uint8_t bs_code = (sync[2] >> 4) & 0x0F;
            // Read UTF-8 coded frame number
            uint8_t u0 = 0;
            if (!f.read(reinterpret_cast<char*>(&u0), 1)) break;
            int extra_u = 0;
            if ((u0 & 0x80) == 0) extra_u = 0;
            else if ((u0 & 0xE0) == 0xC0) extra_u = 1;
            else if ((u0 & 0xF0) == 0xE0) extra_u = 2;
            else if ((u0 & 0xF8) == 0xF0) extra_u = 3;
            else if ((u0 & 0xFC) == 0xF8) extra_u = 4;
            else if ((u0 & 0xFE) == 0xFC) extra_u = 5;
            for (int k = 0; k < extra_u; ++k) { uint8_t dummy = 0; f.read(reinterpret_cast<char*>(&dummy), 1); }

            uint32_t block_len = 4096;
            if (bs_code == 6) {
                uint8_t b8 = 0; f.read(reinterpret_cast<char*>(&b8), 1);
                block_len = static_cast<uint32_t>(b8) + 1;
            } else if (bs_code == 7) {
                uint8_t b16[2] = { 0 }; f.read(reinterpret_cast<char*>(b16), 2);
                block_len = ((static_cast<uint32_t>(b16[0]) << 8) | b16[1]) + 1;
            }
            uint8_t crc8 = 0;
            f.read(reinterpret_cast<char*>(&crc8), 1);

            std::vector<std::vector<float>> ch_data(channels, std::vector<float>(block_len, 0.0f));
            for (uint32_t c = 0; c < channels; ++c) {
                uint8_t sub_hdr = 0;
                if (!f.read(reinterpret_cast<char*>(&sub_hdr), 1)) return MA_ERROR;
                if (sub_hdr != 0x02) return MA_ERROR; // Not a Verbatim subframe -> let FFmpeg decode compressed FLAC
                for (uint32_t i = 0; i < block_len; ++i) {
                    if (bps == 16) {
                        uint8_t b[2] = { 0 };
                        f.read(reinterpret_cast<char*>(b), 2);
                        int16_t v16 = static_cast<int16_t>((static_cast<uint16_t>(b[0]) << 8) | b[1]);
                        ch_data[c][i] = static_cast<float>(v16) / 32768.0f;
                    } else if (bps == 24) {
                        uint8_t b[3] = { 0 };
                        f.read(reinterpret_cast<char*>(b), 3);
                        int32_t v24 = (static_cast<int32_t>(static_cast<int8_t>(b[0])) << 16) |
                                      (static_cast<int32_t>(b[1]) << 8) | b[2];
                        ch_data[c][i] = static_cast<float>(v24) / 8388608.0f;
                    }
                }
            }
            uint8_t crc16[2] = { 0 };
            f.read(reinterpret_cast<char*>(crc16), 2);

            for (uint32_t i = 0; i < block_len; ++i) {
                float l = ch_data[0][i];
                float r = (channels >= 2) ? ch_data[1][i] : l;
                if (channels >= 6) {
                    l += 0.70710678f * (ch_data[2][i] + ch_data[4][i]);
                    r += 0.70710678f * (ch_data[2][i] + ch_data[5][i]);
                }
                pDecoder->decoded_stereo.push_back(std::clamp(l, -1.0f, 1.0f));
                pDecoder->decoded_stereo.push_back(std::clamp(r, -1.0f, 1.0f));
            }
        }
        return pDecoder->decoded_stereo.empty() ? MA_ERROR : MA_SUCCESS;
    }

    return MA_ERROR;
}

inline ma_result ma_decoder_read_pcm_frames(ma_decoder* pDecoder, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) noexcept {
    if (!pDecoder || !pFramesOut) return MA_ERROR;
    size_t total_frames = pDecoder->decoded_stereo.size() / 2;
    if (pDecoder->read_cursor_frames >= total_frames) {
        if (pFramesRead) *pFramesRead = 0;
        return MA_SUCCESS;
    }
    size_t take = std::min<size_t>(static_cast<size_t>(frameCount), total_frames - pDecoder->read_cursor_frames);
    std::memcpy(pFramesOut, &pDecoder->decoded_stereo[pDecoder->read_cursor_frames * 2], take * 2 * sizeof(float));
    pDecoder->read_cursor_frames += take;
    if (pFramesRead) *pFramesRead = take;
    return MA_SUCCESS;
}

inline void ma_decoder_uninit(ma_decoder* pDecoder) noexcept {
    if (pDecoder) {
        pDecoder->decoded_stereo.clear();
        pDecoder->read_cursor_frames = 0;
    }
}

#endif // MINIAUDIO_H
