// fakepeer: jugador simulado para probar Outlast Online con una sola copia del juego.
//   fakepeer join 127.0.0.1 7777   -> se une al juego que hospeda (F8)
//   fakepeer host 7777             -> hospeda; en el juego pulsa F9 con HostIP=127.0.0.1
// El jugador simulado camina en círculo alrededor del jugador real, en su mismo mapa.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include "protocol.h"
#include "adpcm.h"

using namespace proto;

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("uso: fakepeer join <ip> [puerto] | fakepeer host [puerto]\n");
        return 1;
    }
    bool host = !strcmp(argv[1], "host");
    const char* ip = host ? "0.0.0.0" : (argc > 2 ? argv[2] : "127.0.0.1");
    int port = atoi(host ? (argc > 2 ? argv[2] : "7777") : (argc > 3 ? argv[3] : "7777"));
    int seconds = 0;
    if (const char* s = getenv("FAKEPEER_SECONDS")) seconds = atoi(s);

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(host ? (u_short)port : 0);
    bind(s, (sockaddr*)&me, sizeof(me));
    BOOL f = FALSE;
    DWORD ret;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &f, sizeof(f), nullptr, 0, &ret, nullptr, nullptr);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons((u_short)port);
    inet_pton(AF_INET, ip, &peer.sin_addr);
    bool havePeer = !host;
    uint8_t myId = host ? 0 : 0xFF;

    MsgState anchor{};
    bool haveAnchor = false;
    double t0 = GetTickCount64() / 1000.0, lastHello = -10, lastSend = 0;
    uint32_t seq = 0;
    bool front = getenv("FAKEPEER_FRONT") != nullptr, frontSet = false;
    float fx = 0, fy = 0, fz = 0;
    int32_t fyaw = 0;
    printf("fakepeer %s en puerto %d\n", host ? "host" : "cliente", port);

    while (true) {
        double now = GetTickCount64() / 1000.0 - t0;
        if (seconds && now > seconds) break;
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(s, &rs);
        timeval tv{0, 10000};
        if (select(0, &rs, nullptr, nullptr, &tv) > 0) {
            char buf[1500];
            sockaddr_in from{};
            int fl = sizeof(from);
            int n = recvfrom(s, buf, sizeof(buf), 0, (sockaddr*)&from, &fl);
            auto h = (Header*)buf;
            if (n >= (int)sizeof(Header) && h->magic == kMagic) {
                switch (h->type) {
                case Hello:
                    if (host) {
                        peer = from;
                        havePeer = true;
                        MsgWelcome w{MakeHeader(Welcome, 0), 1};
                        strcpy_s(w.hostName, "Bot");
                        sendto(s, (char*)&w, sizeof(w), 0, (sockaddr*)&peer, sizeof(peer));
                        printf("cliente conectado: %s\n", ((MsgHello*)buf)->name);
                    }
                    break;
                case Welcome:
                    if (myId == 0xFF) {
                        myId = ((MsgWelcome*)buf)->yourId;
                        printf("aceptado por %s con id %d\n", ((MsgWelcome*)buf)->hostName, myId);
                    }
                    break;
                case State:
                    anchor = *(MsgState*)buf;
                    {
                        static char lastCp[64] = "?";
                        if (strcmp(lastCp, anchor.checkpoint) != 0) {
                            strcpy_s(lastCp, anchor.checkpoint);
                            printf("%s: mapa '%s' checkpoint '%s'\n", anchor.name, anchor.map, anchor.checkpoint);
                        }
                    }
                    haveAnchor = true;
                    break;
                case Ping: {
                    MsgPing p = *(MsgPing*)buf;
                    p.h = MakeHeader(Pong, myId);
                    sendto(s, (char*)&p, sizeof(p), 0, (sockaddr*)&from, sizeof(from));
                    break;
                }
                case Chat:
                    printf("chat: %s\n", ((MsgChat*)buf)->text);
                    break;
                case Voice: {
                    static int nv = 0;
                    if (++nv % 50 == 1) printf("voz recibida de %d: %d paquetes\n", h->playerId, nv);
                    fflush(stdout);
                    break;
                }
                }
            }
        }
        if (!host && myId == 0xFF && now - lastHello > 1.0) {
            MsgHello hm{MakeHeader(Hello, 0xFF)};
            strcpy_s(hm.name, "Bot");
            sendto(s, (char*)&hm, sizeof(hm), 0, (sockaddr*)&peer, sizeof(peer));
            lastHello = now;
        }
        if (havePeer && myId != 0xFF && haveAnchor && now - lastSend > 1.0 / 30) {
            lastSend = now;
            MsgState m{};
            m.h = MakeHeader(State, myId);
            m.seq = ++seq;
            double a = now * 0.6;
            const float r = 250.f;
            m.pos[0] = anchor.pos[0] + r * (float)cos(a);
            m.pos[1] = anchor.pos[1] + r * (float)sin(a);
            m.pos[2] = anchor.pos[2];
            m.vel[0] = -r * 0.6f * (float)sin(a);
            m.vel[1] = r * 0.6f * (float)cos(a);
            m.yaw = (int32_t)((a + 3.14159265 / 2) * 32768.0 / 3.14159265) & 0xFFFF;
            m.flags = FlagHasPawn | FlagRunning;
            if (front) {
                // Quieto delante del jugador real (fijado la primera vez), mirándole.
                if (!frontSet || getenv("FAKEPEER_FOLLOW")) {
                    double ay = anchor.yaw * 3.14159265 / 32768.0;
                    fx = anchor.pos[0] + 220.f * (float)cos(ay);
                    fy = anchor.pos[1] + 220.f * (float)sin(ay);
                    fz = anchor.pos[2];
                    fyaw = (anchor.yaw + 32768) & 0xFFFF;
                    frontSet = true;
                }
                m.pos[0] = fx; m.pos[1] = fy; m.pos[2] = fz;
                m.vel[0] = m.vel[1] = 0;
                m.yaw = fyaw;
                m.flags = FlagHasPawn;
            }
            strcpy_s(m.map, anchor.map);
            if (getenv("FAKEPEER_CAM")) {  // simula que el bot saca la videocámara (CamcorderState=1, BodySetup=2)
                m.syncBytes[2] = 1;
                m.syncBytes[4] = 2;
                m.syncBools |= 2;  // bCamcorderDesired
            }
            if (getenv("FAKEPEER_CROUCH")) {  // simula que el bot está agachado
                m.flags |= FlagCrouched;
                m.syncBools |= (1u << 4) | (1u << 5);  // bIsCrouched, bWantsToCrouch
            }
            if (getenv("FAKEPEER_MIRROR")) {  // copiar el estado de animación del jugador real
                memcpy(m.syncBytes, anchor.syncBytes, sizeof(m.syncBytes));
                m.syncBools = anchor.syncBools;
                memcpy(m.syncFloats, anchor.syncFloats, sizeof(m.syncFloats));
                memcpy(m.slots, anchor.slots, sizeof(m.slots));
                m.flags = anchor.flags;
                static uint8_t lastSerial = 0;
                if (anchor.slots[0].playing && anchor.slots[0].serial != lastSerial) {
                    lastSerial = anchor.slots[0].serial;
                    printf("anim: %s\n", anchor.slots[0].anim);
                    fflush(stdout);
                }
            }
            // Checkpoint: el del jugador real, o el indicado en FAKEPEER_CHECKPOINT (para probar la sincronización)
            const char* cpEnv = getenv("FAKEPEER_CHECKPOINT");
            strcpy_s(m.checkpoint, cpEnv && now > 5 ? cpEnv : anchor.checkpoint);
            strcpy_s(m.name, "Bot");
            sendto(s, (char*)&m, sizeof(m), 0, (sockaddr*)&peer, sizeof(peer));
            // FAKEPEER_VOICE=1: "habla" (pitido de 440 Hz, 1 s si / 1 s no) para probar la voz
            if (getenv("FAKEPEER_VOICE") && fmod(now, 2.0) < 1.0) {
                static adpcm::State enc;
                static double phase = 0;
                static uint16_t vseq = 0;
                // ~33 ms entre envíos aquí: mandamos 2 tramas de 20 ms para no quedarnos cortos
                for (int f = 0; f < 2; ++f) {
                    int16_t pcm[kVoiceFrame];
                    for (int i = 0; i < kVoiceFrame; ++i) {
                        pcm[i] = (int16_t)(8000 * sin(phase));
                        phase += 2 * 3.14159265 * 440.0 / kVoiceRate;
                    }
                    MsgVoice v{};
                    v.h = MakeHeader(Voice, myId);
                    v.seq = vseq++;
                    v.predictor = enc.predictor;
                    v.index = enc.index;
                    adpcm::Encode(enc, pcm, kVoiceFrame, v.data);
                    sendto(s, (char*)&v, sizeof(v), 0, (sockaddr*)&peer, sizeof(peer));
                }
            }
            static int every = 0;
            if (++every % 300 == 0) {
                MsgChat c{MakeHeader(Chat, myId)};
                strcpy_s(c.text, "hola desde el bot");
                sendto(s, (char*)&c, sizeof(c), 0, (sockaddr*)&peer, sizeof(peer));
            }
        }
    }
    MsgLeave bye{MakeHeader(Bye, myId)};
    if (havePeer) sendto(s, (char*)&bye, sizeof(bye), 0, (sockaddr*)&peer, sizeof(peer));
    closesocket(s);
    return 0;
}
