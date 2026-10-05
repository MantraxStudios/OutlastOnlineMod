#pragma once
// Protocolo UDP de Outlast Online. Compartido entre la DLL y la herramienta fakepeer.
#include <cstdint>

namespace proto {

constexpr uint32_t kMagic = 0x504D4C4F;  // "OLMP"
constexpr uint8_t kVersion = 4;  // v4: estado de animación
constexpr int kMaxPlayers = 8;
constexpr int kNameLen = 24;
constexpr int kMapLen = 48;
constexpr int kVoiceRate = 16000;  // Hz, mono
constexpr int kVoiceFrame = 320;   // muestras por paquete (20 ms)
constexpr int kSyncBytes = 12, kSyncFloats = 2, kAnimSlots = 3, kAnimNameLen = 40;

enum Type : uint8_t {
    Hello = 1,    // cliente -> host: quiero unirme
    Welcome = 2,  // host -> cliente: aceptado, este es tu id
    State = 3,    // posición/rotación de un jugador (el host reenvía los de los clientes)
    Chat = 4,     // texto / avisos del sistema
    Ping = 5,
    Pong = 6,
    Bye = 7,      // me desconecto
    Leave = 8,    // host -> clientes: el jugador `playerId` se ha ido
    Full = 9,     // host -> cliente: partida llena / versión incompatible
    Voice = 10,   // 20 ms de voz IMA-ADPCM (el host la reenvía)
};

enum StateFlags : uint8_t {
    FlagCrouched = 1 << 0,
    FlagRunning = 1 << 1,
    FlagHasPawn = 1 << 2,
};

#pragma pack(push, 1)
// Animación reproducida en un AnimNodeSlot (parkour, armarios, camas, puertas...)
struct AnimSlotState {
    char anim[kAnimNameLen];
    float time;
    float rate;
    uint8_t playing;
    uint8_t looping;
    uint8_t serial;  // cambia cada vez que empieza una animación nueva
    uint8_t reserved;
};

struct Header {
    uint32_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t playerId;  // emisor original (0 = host)
    uint8_t reserved;
};

struct MsgHello { Header h; char name[kNameLen]; };
struct MsgWelcome { Header h; uint8_t yourId; char hostName[kNameLen]; };
struct MsgState {
    Header h;
    uint32_t seq;
    float pos[3];
    float vel[3];
    int32_t pitch, yaw;
    uint8_t flags;
    char map[kMapLen];
    char name[kNameLen];
    char checkpoint[kMapLen];  // OLGame.CurrentCheckpointName ("" si no hay)
    // Estado del personaje que mueve el árbol de animación (cámara, agachado, parkour, inclinarse...)
    uint8_t syncBytes[kSyncBytes];
    uint32_t syncBools;
    float syncFloats[kSyncFloats];
    AnimSlotState slots[kAnimSlots];
};
struct MsgChat { Header h; char text[128]; };
struct MsgPing { Header h; uint64_t t; };
struct MsgLeave { Header h; };
struct MsgVoice {
    Header h;
    uint16_t seq;
    int16_t predictor;  // estado ADPCM al inicio del paquete
    uint8_t index;
    uint8_t reserved;
    uint8_t data[kVoiceFrame / 2];
};
#pragma pack(pop)

inline Header MakeHeader(Type t, uint8_t id) { return Header{kMagic, kVersion, (uint8_t)t, id, 0}; }

}  // namespace proto
