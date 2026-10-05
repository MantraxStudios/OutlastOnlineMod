#include "net.h"
#include "log.h"
#include <ws2tcpip.h>
#include <cstring>

using namespace proto;

static constexpr double kTimeout = 6.0;

double NetSession::Now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) / double(freq.QuadPart);
}

static void CopyName(char* dst, size_t cap, const std::string& s) {
    strncpy_s(dst, cap, s.c_str(), _TRUNCATE);
}

static bool SameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static std::string AddrStr(const sockaddr_in& a) {
    char ip[64];
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

static SOCKET OpenSocket(uint16_t bindPort) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return s;
    // Evita que un ICMP "port unreachable" rompa recvfrom con WSAECONNRESET.
    BOOL f = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12) /*SIO_UDP_CONNRESET*/, &f, sizeof(f), nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(bindPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(s, (sockaddr*)&a, sizeof(a)) != 0) {
        LOG("bind(%u) fallo: %d", bindPort, WSAGetLastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

bool NetSession::StartHost(uint16_t port, const std::string& name) {
    Stop();
    sock_ = OpenSocket(port);
    if (sock_ == INVALID_SOCKET) return false;
    mode_ = Mode::Host;
    myId_ = 0;
    myName_ = name;
    running_ = true;
    thread_ = std::thread(&NetSession::Run, this);
    LOG("Host iniciado en puerto %u", port);
    PushEvent("Servidor abierto en el puerto " + std::to_string(port));
    return true;
}

bool NetSession::StartClient(const std::string& host, uint16_t port, const std::string& name) {
    Stop();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        PushEvent("No se pudo resolver " + host);
        return false;
    }
    hostAddr_ = *(sockaddr_in*)res->ai_addr;
    freeaddrinfo(res);
    sock_ = OpenSocket(0);
    if (sock_ == INVALID_SOCKET) return false;
    mode_ = Mode::Client;
    myId_ = 0xFF;
    myName_ = name;
    running_ = true;
    thread_ = std::thread(&NetSession::Run, this);
    LOG("Conectando a %s", AddrStr(hostAddr_).c_str());
    PushEvent("Conectando a " + AddrStr(hostAddr_) + "...");
    return true;
}

void NetSession::Stop() {
    if (running_) {
        MsgLeave bye{MakeHeader(Bye, myId_)};
        if (mode_ == Mode::Host) Broadcast(&bye, sizeof(bye), -1);
        else SendTo(&bye, sizeof(bye), hostAddr_);
        running_ = false;
    }
    if (thread_.joinable()) thread_.join();
    if (sock_ != INVALID_SOCKET) closesocket(sock_);
    sock_ = INVALID_SOCKET;
    if (mode_ != Mode::Off) PushEvent("Desconectado");
    mode_ = Mode::Off;
    myId_ = 0xFF;
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& p : players_) p = Remote{};
}

void NetSession::SendTo(const void* data, int len, const sockaddr_in& to) {
    if (sock_ != INVALID_SOCKET) sendto(sock_, (const char*)data, len, 0, (const sockaddr*)&to, sizeof(to));
}

void NetSession::Broadcast(const void* data, int len, int exceptId) {
    sockaddr_in targets[kMaxPlayers];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (int i = 1; i < kMaxPlayers; ++i)
            if (players_[i].active && i != exceptId) targets[n++] = players_[i].addr;
    }
    for (int i = 0; i < n; ++i) SendTo(data, len, targets[i]);
}

void NetSession::SendState(const MsgState& s) {
    if (!Connected()) return;
    MsgState m = s;
    m.h = MakeHeader(State, myId_);
    CopyName(m.name, sizeof(m.name), myName_);
    if (mode_ == Mode::Host) Broadcast(&m, sizeof(m), -1);
    else SendTo(&m, sizeof(m), hostAddr_);
}

void NetSession::SendChat(const std::string& text) {
    if (!Connected()) return;
    MsgChat m{MakeHeader(Chat, myId_)};
    CopyName(m.text, sizeof(m.text), text);
    if (mode_ == Mode::Host) Broadcast(&m, sizeof(m), -1);
    else SendTo(&m, sizeof(m), hostAddr_);
    PushEvent(myName_ + ": " + text);
}

void NetSession::SendVoice(const MsgVoice& v) {
    if (!Connected()) return;
    MsgVoice m = v;
    m.h = MakeHeader(Voice, myId_);
    if (mode_ == Mode::Host) Broadcast(&m, sizeof(m), -1);
    else SendTo(&m, sizeof(m), hostAddr_);
}

std::vector<NetSession::Remote> NetSession::Snapshot() {
    std::lock_guard<std::mutex> lk(mtx_);
    return std::vector<Remote>(players_, players_ + kMaxPlayers);
}

std::vector<std::string> NetSession::PopEvents() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> v(events_.begin(), events_.end());
    events_.clear();
    return v;
}

void NetSession::PushEvent(const std::string& s) {
    LOG("[evento] %s", s.c_str());
    std::lock_guard<std::mutex> lk(mtx_);
    events_.push_back(s);
    if (events_.size() > 32) events_.pop_front();
}

int NetSession::FindClient(const sockaddr_in& a) {
    for (int i = 1; i < kMaxPlayers; ++i)
        if (players_[i].active && SameAddr(players_[i].addr, a)) return i;
    return -1;
}

void NetSession::Run() {
    double lastPing = 0, lastHello = 0;
    char buf[1500];
    while (running_) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(sock_, &rs);
        timeval tv{0, 50000};
        if (select(0, &rs, nullptr, nullptr, &tv) > 0) {
            sockaddr_in from{};
            int fl = sizeof(from);
            int n = recvfrom(sock_, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            if (n >= (int)sizeof(Header)) Handle(buf, n, from);
        }
        double now = Now();
        if (mode_ == Mode::Client && myId_ == 0xFF && now - lastHello > 1.0) {
            MsgHello h{MakeHeader(Hello, 0xFF)};
            CopyName(h.name, sizeof(h.name), myName_);
            SendTo(&h, sizeof(h), hostAddr_);
            lastHello = now;
        }
        if (now - lastPing > 1.0) {
            MsgPing p{MakeHeader(Ping, myId_), (uint64_t)(now * 1e6)};
            if (mode_ == Mode::Host) Broadcast(&p, sizeof(p), -1);
            else if (myId_ != 0xFF) SendTo(&p, sizeof(p), hostAddr_);
            lastPing = now;
        }
        // timeouts
        std::vector<std::pair<int, std::string>> gone;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (int i = 0; i < kMaxPlayers; ++i) {
                auto& p = players_[i];
                if (p.active && now - p.lastRecv > kTimeout) {
                    gone.push_back({i, p.name});
                    p = Remote{};
                }
            }
        }
        for (auto& g : gone) {
            PushEvent(g.second + " perdio la conexion");
            if (mode_ == Mode::Host) {
                MsgLeave l{MakeHeader(Leave, (uint8_t)g.first)};
                Broadcast(&l, sizeof(l), -1);
            } else if (g.first == 0) {
                myId_ = 0xFF;  // host caído: volver a intentar Hello
                PushEvent("Reintentando conexion con el host...");
            }
        }
    }
}

void NetSession::Handle(const char* buf, int len, const sockaddr_in& from) {
    auto h = (const Header*)buf;
    if (h->magic != kMagic) return;
    if (h->version != kVersion) {
        if (mode_ == Mode::Host && h->type == Hello) {
            MsgChat m{MakeHeader(Full, 0)};
            CopyName(m.text, sizeof(m.text), "Version incompatible");
            SendTo(&m, sizeof(m), from);
        }
        return;
    }
    double now = Now();

    if (mode_ == Mode::Host) {
        int id = FindClient(from);
        if (h->type == Hello && len >= (int)sizeof(MsgHello)) {
            auto m = (const MsgHello*)buf;
            std::string name(m->name, strnlen(m->name, kNameLen));
            bool isNew = false;
            if (id < 0) {
                std::lock_guard<std::mutex> lk(mtx_);
                for (int i = 1; i < kMaxPlayers; ++i)
                    if (!players_[i].active) {
                        id = i;
                        players_[i] = Remote{};
                        players_[i].active = true;
                        players_[i].name = name;
                        players_[i].addr = from;
                        players_[i].lastRecv = now;
                        isNew = true;
                        break;
                    }
            }
            if (id < 0) {
                MsgChat f{MakeHeader(Full, 0)};
                CopyName(f.text, sizeof(f.text), "Partida llena");
                SendTo(&f, sizeof(f), from);
                return;
            }
            MsgWelcome w{MakeHeader(Welcome, 0), (uint8_t)id};
            CopyName(w.hostName, sizeof(w.hostName), myName_);
            SendTo(&w, sizeof(w), from);
            if (isNew) {
                PushEvent(name + " se unio (" + AddrStr(from) + ")");
                MsgChat c{MakeHeader(Chat, 0)};
                CopyName(c.text, sizeof(c.text), "* " + name + " se unio a la partida");
                Broadcast(&c, sizeof(c), id);
            }
            return;
        }
        if (id < 0) return;  // desconocido
        {
            std::lock_guard<std::mutex> lk(mtx_);
            players_[id].lastRecv = now;
        }
        switch (h->type) {
        case State:
            if (len >= (int)sizeof(MsgState)) {
                MsgState m = *(const MsgState*)buf;
                m.h.playerId = (uint8_t)id;  // el host fija la identidad
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    players_[id].state = m;
                    players_[id].hasState = true;
                    players_[id].stateTime = now;
                }
                Broadcast(&m, sizeof(m), id);
            }
            break;
        case Chat:
            if (len >= (int)sizeof(MsgChat)) {
                MsgChat m = *(const MsgChat*)buf;
                m.h.playerId = (uint8_t)id;
                m.text[sizeof(m.text) - 1] = 0;
                std::string who;
                { std::lock_guard<std::mutex> lk(mtx_); who = players_[id].name; }
                PushEvent(who + ": " + m.text);
                Broadcast(&m, sizeof(m), id);
            }
            break;
        case Voice:
            if (len >= (int)sizeof(MsgVoice)) {
                MsgVoice m = *(const MsgVoice*)buf;
                m.h.playerId = (uint8_t)id;
                if (onVoice) onVoice(id, m);
                Broadcast(&m, sizeof(m), id);
            }
            break;
        case Ping:
            if (len >= (int)sizeof(MsgPing)) {
                MsgPing p = *(const MsgPing*)buf;
                p.h = MakeHeader(Pong, 0);
                SendTo(&p, sizeof(p), from);
            }
            break;
        case Pong:
            if (len >= (int)sizeof(MsgPing)) {
                auto p = (const MsgPing*)buf;
                std::lock_guard<std::mutex> lk(mtx_);
                players_[id].rttMs = float((now * 1e6 - (double)p->t) / 1000.0);
            }
            break;
        case Bye: {
            std::string who;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                who = players_[id].name;
                players_[id] = Remote{};
            }
            PushEvent(who + " salio");
            MsgLeave l{MakeHeader(Leave, (uint8_t)id)};
            Broadcast(&l, sizeof(l), -1);
            break;
        }
        }
        return;
    }

    // ---- cliente ----
    if (!SameAddr(from, hostAddr_)) return;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        players_[0].lastRecv = now;
        if (!players_[0].active) { players_[0].active = true; }
    }
    switch (h->type) {
    case Welcome:
        if (len >= (int)sizeof(MsgWelcome) && myId_ == 0xFF) {
            auto w = (const MsgWelcome*)buf;
            myId_ = w->yourId;
            std::string hn(w->hostName, strnlen(w->hostName, kNameLen));
            { std::lock_guard<std::mutex> lk(mtx_); players_[0].name = hn; }
            PushEvent("Conectado a la partida de " + hn + " (id " + std::to_string(w->yourId) + ")");
        }
        break;
    case Full:
        if (len >= (int)sizeof(MsgChat)) {
            auto m = (const MsgChat*)buf;
            PushEvent(std::string("Rechazado por el host: ") + std::string(m->text, strnlen(m->text, 128)));
        }
        break;
    case State:
        if (len >= (int)sizeof(MsgState) && h->playerId < kMaxPlayers && h->playerId != myId_) {
            auto m = (const MsgState*)buf;
            std::lock_guard<std::mutex> lk(mtx_);
            auto& p = players_[h->playerId];
            if (!p.active) {
                p = Remote{};
                p.active = true;
            }
            p.name.assign(m->name, strnlen(m->name, kNameLen));
            p.state = *m;
            p.hasState = true;
            p.stateTime = now;
            p.lastRecv = now;
        }
        break;
    case Chat:
        if (len >= (int)sizeof(MsgChat)) {
            auto m = (const MsgChat*)buf;
            std::string text(m->text, strnlen(m->text, 128));
            std::string who;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (h->playerId < kMaxPlayers) who = players_[h->playerId].name;
            }
            PushEvent(text.rfind("* ", 0) == 0 || who.empty() ? text : who + ": " + text);
        }
        break;
    case Voice:
        if (len >= (int)sizeof(MsgVoice) && h->playerId < kMaxPlayers && h->playerId != myId_ && onVoice)
            onVoice(h->playerId, *(const MsgVoice*)buf);
        break;
    case Ping:
        if (len >= (int)sizeof(MsgPing)) {
            MsgPing p = *(const MsgPing*)buf;
            p.h = MakeHeader(Pong, myId_);
            SendTo(&p, sizeof(p), hostAddr_);
        }
        break;
    case Pong:
        if (len >= (int)sizeof(MsgPing)) {
            auto p = (const MsgPing*)buf;
            std::lock_guard<std::mutex> lk(mtx_);
            players_[0].rttMs = float((now * 1e6 - (double)p->t) / 1000.0);
        }
        break;
    case Leave:
    case Bye: {
        int id = h->type == Bye ? 0 : h->playerId;
        if (id >= kMaxPlayers) break;
        std::string who;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            who = players_[id].name;
            players_[id] = Remote{};
        }
        PushEvent((who.empty() ? std::string("Un jugador") : who) + " salio");
        if (id == 0) {
            myId_ = 0xFF;
            PushEvent("El host cerro la partida");
        }
        break;
    }
    }
}
