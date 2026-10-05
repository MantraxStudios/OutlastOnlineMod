#pragma once
// Chat de voz por proximidad: captura del micrófono (waveIn), detección de voz,
// codificación ADPCM y reproducción estéreo mezclada (waveOut) con volumen por jugador.
#include "protocol.h"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <array>
#include <string>
#include <vector>

class VoiceChat {
public:
    using SendFn = std::function<void(const proto::MsgVoice&)>;

    ~VoiceChat() { Stop(); }
    bool Start(SendFn send);  // abre la salida de audio (la entrada se abre al activar el micro)
    void Stop();
    bool Running() const { return running_; }

    bool SetMicEnabled(bool on);  // false si no hay micrófono
    bool MicEnabled() const { return micOn_; }
    float MicLevel() const { return micLevel_; }       // 0..1
    bool Transmitting() const { return transmitting_; }
    void SetThreshold(float t) { threshold_ = t; }
    // Micrófono a usar: parte de su nombre ("" = predeterminado de Windows). Se aplica al momento.
    void SetDevice(const std::wstring& nameFragment);
    static std::vector<std::wstring> ListDevices();

    void OnPacket(int playerId, const proto::MsgVoice& m);  // hilo de red
    void SetGain(int playerId, float left, float right);   // hilo de juego
    bool Speaking(int playerId) const;
    void ClearPlayer(int playerId);

    std::atomic<bool> micError{false};  // no se pudo abrir el micrófono (el mod lo muestra y lo limpia)

private:
    static constexpr int kRate = proto::kVoiceRate;
    static constexpr int kFrame = proto::kVoiceFrame;  // muestras por paquete (20 ms)
    static constexpr int kCapBufs = 6, kOutBufs = 5;
    using Frame = std::array<int16_t, kFrame>;

    void Run();
    void ProcessCapture(WAVEHDR& h);
    void FillOutput(WAVEHDR& h);
    bool OpenCapture();
    void CloseCapture();

    SendFn send_;
    std::mutex devMtx_;
    std::wstring device_;
    std::atomic<bool> reopen_{false};
    std::thread thread_;
    std::atomic<bool> running_{false}, micOn_{false}, transmitting_{false}, wantMic_{false};
    std::atomic<float> micLevel_{0}, threshold_{0.02f};
    HANDLE capEvent_ = nullptr, outEvent_ = nullptr;
    HWAVEIN waveIn_ = nullptr;
    HWAVEOUT waveOut_ = nullptr;
    WAVEHDR capHdr_[kCapBufs]{}, outHdr_[kOutBufs]{};
    int16_t capData_[kCapBufs][kFrame]{};
    int16_t outData_[kOutBufs][kFrame * 2]{};
    uint16_t seq_ = 0;
    double hangUntil_ = 0;

    struct Peer {
        std::deque<Frame> queue;
        bool playing = false;
    };
    mutable std::mutex mtx_;
    Peer peers_[proto::kMaxPlayers];
    std::atomic<float> gainL_[proto::kMaxPlayers]{}, gainR_[proto::kMaxPlayers]{};
    std::atomic<double> lastVoice_[proto::kMaxPlayers]{};
};
