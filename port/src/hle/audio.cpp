// libSceAudioOut on WASAPI.
//
// Every port queues the buffers the game outputs, converted to float frames in the Windows 7.1
// channel order. One render thread mixes the ports into a 48 kHz 7.1 float stream that Windows
// converts to the device format (AUTOCONVERTPCM: stereo speakers, headphones, surround).
// sceAudioOutOutput blocks while its port already holds enough audio, so the sound card's clock
// paces the game's audio threads (FMOD, Bink) like the console's. Without an audio device the
// ports keep their timing on a timer and the audio is dropped.
//
// Ports seen in CTR: FMOD main mix and BGM (float 8 channels) and Bink (s16 8 channels), all
// 256 samples at 48 kHz. The FMOD banks (.snd, FSB5) are MPEG: FMOD decodes them in software.
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <string>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "../config.h"
#include "../hle.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr u32 kRate = 48000;
constexpr int kChannels = 8;           // FL FR FC LFE BL BR SL SR (KSAUDIO_SPEAKER_7POINT1_SURROUND)
constexpr u32 kQueueFrames = 3 * 256;  // a port blocks while it holds more than this (16 ms)
constexpr u32 kRingFrames = 8192;

struct Port {
    bool open = false;
    int type = 0;
    u32 samples = 256;
    u32 freq = kRate;
    u32 format = 1;
    float volume[8] = {1, 1, 1, 1, 1, 1, 1, 1};
    std::vector<float> ring;  // kRingFrames frames of kChannels floats
    u64 write = 0, read = 0;  // frame counters
    s64 next_qpc = 0;         // timer pacing without a device
    float peak = 0;           // level meter for the log
    u64 underrun = 0, dropped = 0;  // frames the mixer lacked / the queue could not take
};

std::mutex g_mutex;
std::condition_variable g_consumed;
Port g_ports[32];
std::atomic<bool> g_device_ok{false};

s64 Qpc() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart;
}
s64 QpcFreq() {
    static s64 f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v.QuadPart;
    }();
    return f;
}

void WaitUntil(s64 target) {
    thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    s64 now = Qpc();
    if (target <= now) return;
    LARGE_INTEGER due;
    due.QuadPart = -(s64)((target - now) * 10000000 / QpcFreq());
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

// SCE_AUDIO_OUT_PARAM_FORMAT: 0 s16 mono, 1 s16 stereo, 2 s16 8ch, 3 float mono, 4 float stereo,
// 5 float 8ch, 6 s16 8ch std, 7 float 8ch std. The 8ch order is L R C LFE Ls Rs Lb Rb (sides
// first); the "std" order is the Windows one (backs first).
int FormatChannels(u32 f) { return f == 0 || f == 3 ? 1 : f == 1 || f == 4 ? 2 : 8; }
bool FormatFloat(u32 f) { return f == 3 || f == 4 || f == 5 || f == 7; }

void Convert(const Port& p, const void* buf, float* out) {
    const int ch = FormatChannels(p.format);
    const bool fl = FormatFloat(p.format);
    const bool std_order = p.format == 6 || p.format == 7;
    for (u32 i = 0; i < p.samples; i++) {
        float in[8] = {};
        for (int c = 0; c < ch; c++)
            in[c] = fl ? ((const float*)buf)[i * ch + c] : ((const s16*)buf)[i * ch + c] / 32768.0f;
        float* o = out + (size_t)i * kChannels;
        memset(o, 0, sizeof(float) * kChannels);
        if (ch == 1) {
            o[0] = o[1] = in[0];
        } else if (ch == 2) {
            o[0] = in[0], o[1] = in[1];
        } else {
            o[0] = in[0], o[1] = in[1], o[2] = in[2], o[3] = in[3];
            if (std_order) o[4] = in[4], o[5] = in[5], o[6] = in[6], o[7] = in[7];
            else o[4] = in[6], o[5] = in[7], o[6] = in[4], o[7] = in[5];
        }
    }
}

// debugging aid: CTR_AUDIO_DUMP=<seconds> writes the start of the mix (front left / right) to
// audio_dump.wav in the working directory
void DumpMix(const float* frames, u32 n) {
    static const u32 limit = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("CTR_AUDIO_DUMP", v, sizeof(v)) ? (u32)atoi(v) * kRate : 0u;
    }();
    static std::vector<s16> pcm;
    static bool written = false;
    if (!limit || written) return;
    for (u32 i = 0; i < n && pcm.size() < (size_t)limit * 2; i++)
        for (int c = 0; c < 2; c++) pcm.push_back((s16)std::clamp(frames[i * kChannels + c] * 32767.0f, -32768.0f, 32767.0f));
    if (pcm.size() < (size_t)limit * 2) return;
    written = true;
    if (FILE* f = fopen("audio_dump.wav", "wb")) {
        u32 bytes = (u32)pcm.size() * 2, v32;
        u16 v16;
        fwrite("RIFF", 1, 4, f), v32 = 36 + bytes, fwrite(&v32, 4, 1, f), fwrite("WAVEfmt ", 1, 8, f);
        v32 = 16, fwrite(&v32, 4, 1, f), v16 = 1, fwrite(&v16, 2, 1, f), v16 = 2, fwrite(&v16, 2, 1, f);
        v32 = kRate, fwrite(&v32, 4, 1, f), v32 = kRate * 4, fwrite(&v32, 4, 1, f), v16 = 4, fwrite(&v16, 2, 1, f);
        v16 = 16, fwrite(&v16, 2, 1, f), fwrite("data", 1, 4, f), fwrite(&bytes, 4, 1, f);
        fwrite(pcm.data(), 2, pcm.size(), f);
        fclose(f);
        LOG("audio: wrote audio_dump.wav (%u s)", limit / kRate);
    }
}

// ------------------------------------------------------------------ WASAPI render thread
void RenderThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    static const GUID kFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
    const float master = std::clamp(ConfigInt("Volume", 100), 0, 100) / 100.0f;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::vector<float> mix;
    bool logged_failure = false;
    for (;;) {
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        WAVEFORMATEXTENSIBLE wf{};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = kChannels;
        wf.Format.nSamplesPerSec = kRate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = kChannels * 4;
        wf.Format.nAvgBytesPerSec = kRate * kChannels * 4;
        wf.Format.cbSize = sizeof(wf) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        wf.dwChannelMask = 0x63F;  // FL FR FC LFE BL BR SL SR
        wf.SubFormat = kFloat;
        UINT32 frames = 0;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
        if (SUCCEEDED(hr)) hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)client.GetAddressOf());
        if (SUCCEEDED(hr))
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                    400000 /* 40 ms */, 0, &wf.Format, nullptr);
        if (SUCCEEDED(hr)) hr = client->SetEventHandle(event);
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&frames);
        if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
        if (SUCCEEDED(hr)) hr = client->Start();
        if (FAILED(hr)) {
            if (!logged_failure) LOG("audio: no output device (0x%08lx), audio is muted", hr);
            logged_failure = true;
            g_device_ok = false;
            g_consumed.notify_all();
            Sleep(2000);  // the user may plug one in
            continue;
        }
        LOG("audio: WASAPI output, 48 kHz 7.1 float, %u frame buffer, volume %.0f%%", frames, master * 100);
        logged_failure = false;
        g_device_ok = true;
        for (;;) {
            WaitForSingleObject(event, 100);
            UINT32 padding = 0;
            if (FAILED(hr = client->GetCurrentPadding(&padding))) break;  // device gone: reopen
            UINT32 n = frames - padding;
            if (!n) continue;
            BYTE* data = nullptr;
            if (FAILED(hr = render->GetBuffer(n, &data))) break;
            mix.assign((size_t)n * kChannels, 0.0f);
            {
                std::lock_guard lk(g_mutex);
                for (Port& p : g_ports) {
                    if (!p.open || p.ring.empty()) continue;
                    u64 avail = p.write - p.read;
                    u32 take = (u32)std::min<u64>(avail, n);
                    if (avail && take < n) p.underrun += n - take;  // ran dry while playing
                    for (u32 i = 0; i < take; i++) {
                        const float* src = &p.ring[((p.read + i) % kRingFrames) * kChannels];
                        float* dst = &mix[(size_t)i * kChannels];
                        for (int c = 0; c < kChannels; c++) {
                            dst[c] += src[c] * p.volume[c];
                            p.peak = std::max(p.peak, std::fabs(src[c]));
                        }
                    }
                    p.read += take;
                }
            }
            g_consumed.notify_all();
            // level meter in the log: every 10 s during the first two minutes
            static ULONGLONG start = GetTickCount64(), next = start + 10000;
            if (GetTickCount64() >= next && next < start + 130000) {
                next += 10000;
                std::string lv;
                std::lock_guard lk(g_mutex);
                for (int h = 1; h < 32; h++)
                    if (g_ports[h].open) {
                        char b[96];
                        snprintf(b, sizeof(b), " port %d (type %d) %.2f, %llu/%llu frames short/dropped", h,
                                 g_ports[h].type, g_ports[h].peak, (unsigned long long)g_ports[h].underrun,
                                 (unsigned long long)g_ports[h].dropped);
                        lv += b;
                        g_ports[h].peak = 0;
                    }
                LOG("audio: peak levels%s", lv.c_str());
            }
            float* out = (float*)data;
            for (size_t i = 0; i < mix.size(); i++) out[i] = std::clamp(mix[i] * master, -1.0f, 1.0f);
            DumpMix(out, n);
            render->ReleaseBuffer(n, 0);
        }
        LOG("audio: output device lost (0x%08lx), reopening", hr);
        client->Stop();
        g_device_ok = false;
    }
}

void StartAudio() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(RenderThread).detach(); });
}

// ------------------------------------------------------------------ libSceAudioOut
SYSV int sceAudioOutInit() {
    StartAudio();
    return 0;
}
SYSV int sceAudioOutOpen(int user, int type, int index, u32 len, u32 freq, u32 param) {
    StartAudio();
    std::lock_guard lk(g_mutex);
    for (int h = 1; h < 32; h++) {
        if (!g_ports[h].open) {
            Port& p = g_ports[h];
            p = Port{};
            p.open = true, p.type = type, p.samples = len, p.freq = freq, p.format = param & 0xFF;
            p.ring.assign((size_t)kRingFrames * kChannels, 0.0f);
            LOG("sceAudioOutOpen(type %d, %u samples, %u Hz, format %u) -> %d", type, len, freq, param & 0xFF, h);
            if (freq != kRate) LOG("audio: port %d at %u Hz is played at %u Hz", h, freq, kRate);
            return h;
        }
    }
    return (int)0x80260005;  // PORT_FULL
}
SYSV int sceAudioOutClose(int h) {
    std::lock_guard lk(g_mutex);
    if (h > 0 && h < 32) g_ports[h].open = false, g_ports[h].ring.clear();
    return 0;
}

// Queues one buffer (nullptr: just wait for the queued audio) and blocks like the hardware.
int Output(int h, const void* buf) {
    if (h <= 0 || h >= 32 || !g_ports[h].open) return (int)0x80260004;  // INVALID_PORT
    std::unique_lock lk(g_mutex);
    Port& p = g_ports[h];
    if (!g_device_ok) {  // no device: keep the timing, drop the audio
        lk.unlock();
        s64 period = (s64)p.samples * QpcFreq() / std::max<u32>(p.freq, 1);
        s64 now = Qpc();
        if (p.next_qpc == 0 || p.next_qpc < now - 4 * period) p.next_qpc = now;
        p.next_qpc += period;
        WaitUntil(p.next_qpc - period);
        return (int)p.samples;
    }
    g_consumed.wait_for(lk, std::chrono::milliseconds(200), [&] {
        return !p.open || !g_device_ok || p.write - p.read + p.samples <= kQueueFrames;
    });
    if (!p.open) return (int)0x80260004;
    if (buf && p.write - p.read + p.samples > kRingFrames) p.dropped += p.samples;
    if (buf && p.write - p.read + p.samples <= kRingFrames) {
        thread_local std::vector<float> tmp;
        tmp.resize((size_t)p.samples * kChannels);
        Convert(p, buf, tmp.data());
        for (u32 i = 0; i < p.samples; i++)
            memcpy(&p.ring[((p.write + i) % kRingFrames) * kChannels], &tmp[(size_t)i * kChannels],
                   sizeof(float) * kChannels);
        p.write += p.samples;
    }
    return (int)p.samples;
}

SYSV int sceAudioOutOutput(int h, const void* buf) { return Output(h, buf); }
struct OutputParam {
    int handle;
    const void* ptr;
};
SYSV int sceAudioOutOutputs(OutputParam* params, u32 num) {
    for (u32 i = 0; i < num; i++) Output(params[i].handle, params[i].ptr);
    return 0;
}
// SCE_AUDIO_VOLUME_0DB = 32768; flag bit n = channel n (port channel order)
SYSV int sceAudioOutSetVolume(int h, u32 flag, const int* vol) {
    if (h <= 0 || h >= 32) return (int)0x80260004;
    std::lock_guard lk(g_mutex);
    Port& p = g_ports[h];
    static const int kToWindows[8] = {0, 1, 2, 3, 6, 7, 4, 5};  // 8ch port order -> mix order
    const bool std_order = p.format == 6 || p.format == 7;
    for (int ch = 0; ch < 8; ch++) {
        if (!(flag & (1u << ch))) continue;
        float v = std::clamp(vol[ch], 0, 32768) / 32768.0f;
        if (FormatChannels(p.format) <= 2) {
            if (ch == 0) p.volume[0] = v;
            if (ch == 1 || FormatChannels(p.format) == 1) p.volume[1] = v;
        } else {
            p.volume[std_order ? ch : kToWindows[ch]] = v;
        }
    }
    return 0;
}
struct PortState {
    u16 output;
    u8 channel;
    u8 reserved8_1[1];
    s16 volume;
    u16 rerouteCounter;
    u64 flag;
    u64 reserved64[2];
};
SYSV int sceAudioOutGetPortState(int h, PortState* s) {
    memset(s, 0, sizeof(*s));
    s->output = 1;  // primary
    s->channel = 2;
    s->volume = 127;
    return 0;
}
HLE("libSceAudioOut", sceAudioOutInit);
HLE("libSceAudioOut", sceAudioOutOpen);
HLE("libSceAudioOut", sceAudioOutClose);
HLE("libSceAudioOut", sceAudioOutOutput);
HLE("libSceAudioOut", sceAudioOutOutputs);
HLE("libSceAudioOut", sceAudioOutSetVolume);
HLE("libSceAudioOut", sceAudioOutGetPortState);

}  // namespace
