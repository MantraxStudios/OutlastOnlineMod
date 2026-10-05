#pragma once
#include "protocol.h"
#include <winsock2.h>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <deque>
#include <functional>

// Sesión de red P2P en estrella: un host y hasta 7 clientes.
// El host reenvía a cada cliente el estado de los demás.
class NetSession {
public:
    enum class Mode { Off, Host, Client };

    struct Remote {
        bool active = false;
        std::string name;
        proto::MsgState state{};   // último estado recibido
        bool hasState = false;
        double lastRecv = 0;       // segundos (reloj local)
        double stateTime = 0;
        float rttMs = 0;
        sockaddr_in addr{};        // sólo lo usa el host
    };

    ~NetSession() { Stop(); }

    bool StartHost(uint16_t port, const std::string& name);
    bool StartClient(const std::string& host, uint16_t port, const std::string& name);
    void Stop();

    Mode GetMode() const { return mode_; }
    bool Connected() const { return mode_ == Mode::Host || (mode_ == Mode::Client && myId_ != 0xFF); }
    uint8_t MyId() const { return myId_; }

    void SendState(const proto::MsgState& s);   // hilo de juego
    void SendChat(const std::string& text);
    void SendVoice(const proto::MsgVoice& v);
    // Voz recibida (hilo de red): id del jugador que habla
    std::function<void(int, const proto::MsgVoice&)> onVoice;

    // Copia segura del estado de los jugadores remotos (índice = playerId)
    std::vector<Remote> Snapshot();
    // Mensajes para la interfaz (unirse/salir/chat)
    std::vector<std::string> PopEvents();

    static double Now();

private:
    void Run();
    void Handle(const char* buf, int len, const sockaddr_in& from);
    void SendTo(const void* data, int len, const sockaddr_in& to);
    void Broadcast(const void* data, int len, int exceptId);
    void PushEvent(const std::string& s);
    int FindClient(const sockaddr_in& a);

    SOCKET sock_ = INVALID_SOCKET;
    std::thread thread_;
    std::atomic<bool> running_{false};
    Mode mode_ = Mode::Off;
    std::atomic<uint8_t> myId_{0xFF};
    std::string myName_;
    sockaddr_in hostAddr_{};

    std::mutex mtx_;
    Remote players_[proto::kMaxPlayers];
    std::deque<std::string> events_;
};
