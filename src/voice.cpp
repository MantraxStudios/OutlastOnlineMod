#include "voice.h"
#include "adpcm.h"
#include "log.h"
#include <cmath>
#include <cstring>
#include <cwctype>

#pragma comment(lib, "winmm.lib")

static double Now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

static adpcm::State g_enc;

bool VoiceChat::Start(SendFn send) {
    if (running_) return true;
    send_ = std::move(send);
    capEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    outEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    running_ = true;
    thread_ = std::thread(&VoiceChat::Run, this);
    return true;
}

void VoiceChat::Stop() {
    if (!running_) return;
    running_ = false;
    if (thread_.joinable()) thread_.join();
    CloseHandle(capEvent_);
    CloseHandle(outEvent_);
    capEvent_ = outEvent_ = nullptr;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& p : peers_) p = Peer{};
}

bool VoiceChat::SetMicEnabled(bool on) {
    wantMic_ = on;
    if (!running_) micOn_ = on;  // se abrirá al arrancar
    return true;
}

std::vector<std::wstring> VoiceChat::ListDevices() {
    std::vector<std::wstring> v;
    for (UINT i = 0, n = waveInGetNumDevs(); i < n; ++i) {
        WAVEINCAPSW caps{};
        if (waveInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) v.push_back(caps.szPname);
    }
    return v;
}

void VoiceChat::SetDevice(const std::wstring& name) {
    {
        std::lock_guard<std::mutex> lk(devMtx_);
        if (device_ == name) return;
        device_ = name;
    }
    reopen_ = true;  // el hilo de audio cierra y vuelve a abrir el micro
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool VoiceChat::OpenCapture() {
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 1, kRate, kRate * 2, 2, 16, 0};
    std::wstring want;
    {
        std::lock_guard<std::mutex> lk(devMtx_);
        want = Lower(device_);
    }
    // El dispositivo cuyo nombre contenga MicDevice, o el predeterminado de Windows.
    // (waveIn recorta los nombres a 31 caracteres: se compara en ambos sentidos)
    UINT dev = WAVE_MAPPER;
    auto list = ListDevices();
    for (UINT i = 0; i < list.size(); ++i) {
        std::wstring n = Lower(list[i]);
        bool match = !want.empty() && (n.find(want) != std::wstring::npos || want.find(n) != std::wstring::npos);
        LOG("  micro %u: %ls%s", i, list[i].c_str(), match && dev == WAVE_MAPPER ? "  <- elegido" : "");
        if (match && dev == WAVE_MAPPER) dev = i;
    }
    if (!want.empty() && dev == WAVE_MAPPER) LOG("  MicDevice '%ls' no encontrado, uso el predeterminado", want.c_str());
    MMRESULT r = waveInOpen(&waveIn_, dev, &fmt, (DWORD_PTR)capEvent_, 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        LOG("waveInOpen fallo: %u", r);
        waveIn_ = nullptr;
        return false;
    }
    for (int i = 0; i < kCapBufs; ++i) {
        capHdr_[i] = WAVEHDR{};
        capHdr_[i].lpData = (LPSTR)capData_[i];
        capHdr_[i].dwBufferLength = kFrame * 2;
        waveInPrepareHeader(waveIn_, &capHdr_[i], sizeof(WAVEHDR));
        waveInAddBuffer(waveIn_, &capHdr_[i], sizeof(WAVEHDR));
    }
    waveInStart(waveIn_);
    g_enc = adpcm::State{};
    LOG("Microfono abierto");
    return true;
}

void VoiceChat::CloseCapture() {
    if (!waveIn_) return;
    waveInReset(waveIn_);
    for (auto& h : capHdr_) waveInUnprepareHeader(waveIn_, &h, sizeof(WAVEHDR));
    waveInClose(waveIn_);
    waveIn_ = nullptr;
    LOG("Microfono cerrado");
}

void VoiceChat::ProcessCapture(WAVEHDR& h) {
    int n = (int)(h.dwBytesRecorded / 2);
    if (n < kFrame) return;
    auto s = (int16_t*)h.lpData;
    double acc = 0;
    for (int i = 0; i < kFrame; ++i) acc += double(s[i]) * s[i];
    float rms = (float)(std::sqrt(acc / kFrame) / 32768.0);
    float shown = std::fmin(1.f, rms * 6.f);
    micLevel_ = std::fmax(shown, micLevel_ * 0.85f);

    double now = Now();
    if (rms > threshold_) hangUntil_ = now + 0.35;  // mantener abierto un poco tras dejar de hablar
    bool tx = now < hangUntil_;
    transmitting_ = tx;
    if (!tx || !send_) return;

    proto::MsgVoice m{};
    m.seq = seq_++;
    m.predictor = g_enc.predictor;
    m.index = g_enc.index;
    adpcm::Encode(g_enc, s, kFrame, m.data);
    send_(m);
}

void VoiceChat::OnPacket(int id, const proto::MsgVoice& m) {
    if (id < 0 || id >= proto::kMaxPlayers) return;
    Frame f;
    adpcm::State st{m.predictor, (uint8_t)(m.index > 88 ? 88 : m.index)};
    adpcm::Decode(st, m.data, kFrame, f.data());
    std::lock_guard<std::mutex> lk(mtx_);
    auto& q = peers_[id].queue;
    q.push_back(f);
    while (q.size() > 8) q.pop_front();  // limitar la latencia
    lastVoice_[id] = Now();
}

void VoiceChat::SetGain(int id, float l, float r) {
    if (id < 0 || id >= proto::kMaxPlayers) return;
    gainL_[id] = l;
    gainR_[id] = r;
}

bool VoiceChat::Speaking(int id) const {
    return id >= 0 && id < proto::kMaxPlayers && Now() - lastVoice_[id].load() < 0.3;
}

void VoiceChat::ClearPlayer(int id) {
    if (id < 0 || id >= proto::kMaxPlayers) return;
    std::lock_guard<std::mutex> lk(mtx_);
    peers_[id] = Peer{};
    gainL_[id] = gainR_[id] = 0;
}

void VoiceChat::FillOutput(WAVEHDR& h) {
    int32_t mix[kFrame * 2] = {};
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int id = 0; id < proto::kMaxPlayers; ++id) {
            Peer& p = peers_[id];
            if (!p.playing && p.queue.size() >= 3) p.playing = true;  // pequeño colchón anti-cortes
            if (!p.playing) continue;
            if (p.queue.empty()) { p.playing = false; continue; }
            Frame f = p.queue.front();
            p.queue.pop_front();
            float gl = gainL_[id], gr = gainR_[id];
            if (gl <= 0.001f && gr <= 0.001f) continue;
            for (int i = 0; i < kFrame; ++i) {
                mix[i * 2] += (int32_t)(f[i] * gl);
                mix[i * 2 + 1] += (int32_t)(f[i] * gr);
            }
        }
    }
    auto out = (int16_t*)h.lpData;
    for (int i = 0; i < kFrame * 2; ++i) out[i] = (int16_t)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
}

void VoiceChat::Run() {
    WAVEFORMATEX fmt{WAVE_FORMAT_PCM, 2, kRate, kRate * 4, 4, 16, 0};
    if (waveOutOpen(&waveOut_, WAVE_MAPPER, &fmt, (DWORD_PTR)outEvent_, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
        LOG("waveOutOpen fallo");
        waveOut_ = nullptr;
    } else {
        for (int i = 0; i < kOutBufs; ++i) {
            outHdr_[i] = WAVEHDR{};
            outHdr_[i].lpData = (LPSTR)outData_[i];
            outHdr_[i].dwBufferLength = kFrame * 4;
            waveOutPrepareHeader(waveOut_, &outHdr_[i], sizeof(WAVEHDR));
            FillOutput(outHdr_[i]);
            waveOutWrite(waveOut_, &outHdr_[i], sizeof(WAVEHDR));
        }
    }
    LOG("Chat de voz iniciado");

    while (running_) {
        if (reopen_.exchange(false) && waveIn_) CloseCapture();
        if (wantMic_ && !waveIn_) {
            if (OpenCapture()) micOn_ = true;
            else { wantMic_ = false; micOn_ = false; micError = true; }
        } else if (!wantMic_ && waveIn_) {
            CloseCapture();
        }
        if (!wantMic_) { micOn_ = false; transmitting_ = false; micLevel_ = 0; }

        HANDLE hs[2] = {outEvent_, capEvent_};
        WaitForMultipleObjects(2, hs, FALSE, 30);

        if (waveOut_)
            for (auto& h : outHdr_)
                if (h.dwFlags & WHDR_DONE) {
                    FillOutput(h);
                    waveOutWrite(waveOut_, &h, sizeof(WAVEHDR));
                }
        if (waveIn_)
            for (auto& h : capHdr_)
                if (h.dwFlags & WHDR_DONE) {
                    ProcessCapture(h);
                    h.dwFlags &= ~WHDR_DONE;
                    h.dwBytesRecorded = 0;
                    waveInAddBuffer(waveIn_, &h, sizeof(WAVEHDR));
                }
    }
    CloseCapture();
    if (waveOut_) {
        waveOutReset(waveOut_);
        for (auto& h : outHdr_) waveOutUnprepareHeader(waveOut_, &h, sizeof(WAVEHDR));
        waveOutClose(waveOut_);
        waveOut_ = nullptr;
    }
    micOn_ = wantMic_.load();
    transmitting_ = false;
    LOG("Chat de voz detenido");
}
