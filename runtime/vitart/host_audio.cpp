// Saída de som por WASAPI (ver host_audio.h).
#include "host_audio.h"
#include "vitart.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "winmm.lib")

namespace hostaudio {
namespace {

struct Port {
    uint32_t freq = kRate;
    std::vector<float> ring; // estéreo intercalado, capacidade fixa
    size_t head = 0, size = 0; // em quadros
    bool playing = false;      // tocando: a placa consome; parada, espera encher kPrebuf antes de voltar
    double frac = 0.0;         // reamostragem
    float last[2] = {};
};

std::mutex g_mutex;
std::map<int, Port> g_ports;
int g_next = 1;
std::atomic<bool> g_started{false}, g_ok{false}, g_silenced{false};
Stats g_stats;
constexpr size_t kRingFrames = kRate / 2; // 500 ms de capacidade por porta
constexpr size_t kPrebuf = 1536;          // 32 ms antes de uma porta parada voltar a tocar

void push(Port &p, float l, float r) {
    const size_t cap = p.ring.size() / 2;
    if (p.size == cap) { // cheia: descarta o mais antigo (não deve acontecer com a cadência pela fila)
        p.head = (p.head + 1) % cap;
        p.size--;
    }
    const size_t at = (p.head + p.size) % cap;
    p.ring[2 * at] = l, p.ring[2 * at + 1] = r;
    p.size++;
}

void mixer() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (!mm) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *dev = nullptr;
    IAudioClient *ac = nullptr;
    IAudioRenderClient *rc = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto fail = [&](const char *what, HRESULT hr) {
        vt::logf("vitart: WASAPI: %s falhou (0x%08lx); sem saída de som", what, (unsigned long)hr);
        g_ok = false;
        g_started = true;
    };
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&en);
    if (FAILED(hr)) return fail("MMDeviceEnumerator", hr);
    if (FAILED(hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) return fail("dispositivo padrão", hr);
    if (FAILED(hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac))) return fail("IAudioClient", hr);
    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_PCM, wf.nChannels = 2, wf.nSamplesPerSec = kRate, wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4, wf.nAvgBytesPerSec = kRate * 4;
    // 20 ms de buffer, por evento; o Windows converte a taxa se a placa usar outra.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    if (FAILED(hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &wf, nullptr))) return fail("Initialize", hr);
    UINT32 buf_frames = 0;
    ac->GetBufferSize(&buf_frames);
    ac->SetEventHandle(ev);
    if (FAILED(hr = ac->GetService(__uuidof(IAudioRenderClient), (void **)&rc))) return fail("IAudioRenderClient", hr);
    REFERENCE_TIME lat = 0;
    ac->GetStreamLatency(&lat);
    g_stats.latency_ms = (uint32_t)(buf_frames * 1000ull / kRate);
    vt::logf("vitart: som por WASAPI: buffer de %u quadros (%u ms), latência do fluxo %lld ms", buf_frames,
         g_stats.latency_ms, (long long)(lat / 10000));
    static FILE *dump = getenv("VITART_AUDIO_DUMP") ? fopen(getenv("VITART_AUDIO_DUMP"), "wb") : nullptr;
    // Começa com o buffer em silêncio, depois só repõe o que a placa consumiu.
    BYTE *data = nullptr;
    if (SUCCEEDED(rc->GetBuffer(buf_frames, &data))) rc->ReleaseBuffer(buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    ac->Start();
    g_ok = true;
    g_started = true;
    std::vector<float> mix;
    std::vector<int16_t> out;
    for (;;) {
        WaitForSingleObject(ev, 100);
        UINT32 pad = 0;
        if (FAILED(ac->GetCurrentPadding(&pad))) continue;
        const UINT32 n = buf_frames - pad;
        if (!n) continue;
        mix.assign((size_t)n * 2, 0.f);
        {
            std::lock_guard<std::mutex> g(g_mutex);
            if (!g_silenced)
                for (auto &[id, p] : g_ports) {
                    if (!p.playing) {
                        if (p.size < kPrebuf) continue; // pré-enchimento: evita micro-cortes no começo de cada som
                        p.playing = true;
                    }
                    const size_t cap = p.ring.size() / 2;
                    const size_t take = std::min<size_t>(n, p.size);
                    for (size_t i = 0; i < take; i++) {
                        const size_t at = (p.head + i) % cap;
                        mix[2 * i] += p.ring[2 * at], mix[2 * i + 1] += p.ring[2 * at + 1];
                    }
                    p.head = (p.head + take) % cap;
                    p.size -= take;
                    if (take < n) g_stats.underruns++, p.playing = false; // ficou sem dados: volta após encher de novo
                }
            g_stats.mixed += n;
        }
        out.resize((size_t)n * 2);
        for (size_t i = 0; i < out.size(); i++) {
            float x = mix[i];
            const float ax = std::fabs(x);
            if (ax > 0.9f) x = std::copysign(0.9f + 0.1f * std::tanh((ax - 0.9f) / 0.1f), x); // soma de portas: saturação suave
            out[i] = (int16_t)std::clamp(x * 32767.f, -32768.f, 32767.f);
        }
        if (dump) fwrite(out.data(), 2, out.size(), dump);
        if (SUCCEEDED(rc->GetBuffer(n, &data))) {
            std::memcpy(data, out.data(), (size_t)n * 4);
            rc->ReleaseBuffer(n, 0);
        }
    }
}

} // namespace

bool start() {
    static std::once_flag once;
    std::call_once(once, [] {
        timeBeginPeriod(1); // esperas de 1 ms na cadência das portas (o padrão do Windows é 15,6 ms)
        if (getenv("VITART_MUTE")) {
            g_started = true;
            return;
        }
        std::thread(mixer).detach();
    });
    while (!g_started) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    return g_ok;
}

int open_port(uint32_t freq) {
    std::lock_guard<std::mutex> g(g_mutex);
    const int id = g_next++;
    Port &p = g_ports[id];
    p.freq = freq ? freq : kRate;
    p.ring.assign(kRingFrames * 2, 0.f);
    return id;
}

void close_port(int port) {
    std::lock_guard<std::mutex> g(g_mutex);
    g_ports.erase(port);
}

void write(int port, const int16_t *src, uint32_t frames, bool stereo, int vol_l, int vol_r) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_ports.find(port);
    if (it == g_ports.end()) return;
    Port &p = it->second;
    const float gl = vol_l / (32768.f * 32768.f), gr = vol_r / (32768.f * 32768.f);
    auto at = [&](uint32_t i, int c) { return (float)(stereo ? src[2 * i + c] : src[i]); };
    if (p.freq == kRate) {
        for (uint32_t i = 0; i < frames; i++) push(p, at(i, 0) * gl, at(i, 1) * gr);
    } else { // linear: as portas do jogo usam 48 kHz; isto só cobre o caso raro
        const double step = (double)p.freq / kRate;
        while (p.frac < frames) {
            const uint32_t i = (uint32_t)p.frac;
            const float t = (float)(p.frac - i);
            const float l0 = i ? at(i - 1, 0) : p.last[0], r0 = i ? at(i - 1, 1) : p.last[1];
            push(p, (l0 + (at(i, 0) - l0) * t) * gl, (r0 + (at(i, 1) - r0) * t) * gr);
            p.frac += step;
        }
        p.frac -= frames;
        p.last[0] = at(frames - 1, 0), p.last[1] = at(frames - 1, 1);
    }
}

uint32_t queued(int port) {
    std::lock_guard<std::mutex> g(g_mutex);
    auto it = g_ports.find(port);
    return it == g_ports.end() ? 0 : (uint32_t)it->second.size;
}

void silence() {
    std::lock_guard<std::mutex> g(g_mutex);
    g_silenced = true;
}

Stats stats() {
    std::lock_guard<std::mutex> g(g_mutex);
    return g_stats;
}

} // namespace hostaudio
