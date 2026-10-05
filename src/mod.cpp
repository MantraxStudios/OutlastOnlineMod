// Outlast Online: lógica del mod.
// Todo lo que toca objetos del motor se ejecuta en el hilo del juego, dentro
// del hook de UObject::ProcessEvent (eventos PlayerTick y HUD.PostRender).
#include "mod.h"
#include "hook.h"
#include "log.h"
#include "net.h"
#include "voice.h"
#include "ue3.h"
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

using namespace ue;

namespace mod {

// ---------------------------------------------------------------------------
// Configuración (OutlastOnline.ini junto a la DLL)
// ---------------------------------------------------------------------------
struct Config {
    std::string name = "Jugador";
    std::string hostIp = "127.0.0.1";
    int port = 7777;
    int autoStart = 0;           // 0 = manual, 1 = host al arrancar, 2 = unirse al arrancar
    std::string avatarMode = "pawn";  // "pawn" (misma clase que el jugador, con animaciones) o "mesh"
    std::string avatarMesh;      // ruta opcional de SkeletalMesh para el modo "mesh"
    int sendRate = 30;
    bool overlay = true;
    bool debug = false;
    bool syncLevels = true;      // cargar el checkpoint del jugador más avanzado
    bool syncSaveToDisk = true;  // guardar en disco el checkpoint sincronizado (para poder continuar luego)
    int keyHost = VK_F8, keyJoin = VK_F9, keyLeave = VK_F10, keyTeleport = VK_F11, keyOverlay = VK_F7,
        keyChat = 'T', keyVoice = 'M';
    bool voice = true;
    float voiceRange = 25.f;      // metros a los que deja de oírse
    float voiceThreshold = 0.02f; // sensibilidad de detección de voz
    std::wstring micDevice;       // nombre (o parte) del micrófono, "" = predeterminado
};
static Config g_cfg;
static std::wstring g_dir;

static std::string IniStr(const wchar_t* ini, const wchar_t* key, const char* def) {
    wchar_t buf[256];
    std::wstring wdef(def, def + strlen(def));
    GetPrivateProfileStringW(L"Online", key, wdef.c_str(), buf, 256, ini);
    char out[512];
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, sizeof(out), nullptr, nullptr);
    return out;
}

// La configuración se comparte entre Win32 y Win64: Binaries\OutlastOnline.ini
static std::wstring IniPath() {
    std::wstring parent = g_dir.substr(0, g_dir.find_last_of(L"\\/", g_dir.size() - 2) + 1);
    std::wstring shared = parent + L"OutlastOnline.ini";
    std::wstring old = g_dir + L"OutlastOnline.ini";  // ubicación de versiones anteriores
    if (GetFileAttributesW(shared.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(old.c_str()) != INVALID_FILE_ATTRIBUTES)
        CopyFileW(old.c_str(), shared.c_str(), TRUE);
    return shared;
}

static void LoadConfig() {
    std::wstring ini = IniPath();
    if (GetFileAttributesW(ini.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // Crear un ini por defecto para que el usuario lo edite.
        char user[64] = "Jugador";
        DWORD n = sizeof(user);
        GetUserNameA(user, &n);
        FILE* f = nullptr;
        _wfopen_s(&f, ini.c_str(), L"w");
        if (f) {
            fprintf(f,
                    "[Online]\n"
                    "; Nombre visible para los demas jugadores\n"
                    "Name=%s\n"
                    "; IP del host (tambien editable en el juego con F9)\n"
                    "HostIP=127.0.0.1\n"
                    "; Puerto UDP (el host debe abrirlo / redirigirlo en el router)\n"
                    "Port=7777\n"
                    "; 0 = manual (F8/F9), 1 = hospedar al arrancar, 2 = unirse al arrancar\n"
                    "AutoStart=0\n"
                    "; pawn = el otro jugador usa el mismo modelo/animaciones que tu personaje\n"
                    "; mesh = solo el modelo (sin animaciones), mas seguro si 'pawn' da problemas\n"
                    "AvatarMode=pawn\n"
                    "; (modo mesh) ruta de un SkeletalMesh concreto, vacio = el del jugador\n"
                    "AvatarMesh=\n"
                    "; Paquetes de estado por segundo\n"
                    "SendRate=30\n"
                    "; 1 = cuando alguien llega a un checkpoint mas avanzado, los demas lo cargan\n"
                    "SyncLevels=1\n"
                    "; 1 = el checkpoint sincronizado se guarda en tu partida (Continuar)\n"
                    "SyncSaveToDisk=1\n"
                    "; Chat de voz por proximidad (tecla M = activar/desactivar microfono)\n"
                    "VoiceChat=1\n"
                    "; Distancia maxima a la que se oye a otro jugador (metros)\n"
                    "VoiceRange=25\n"
                    "; Umbral de deteccion de voz (mas bajo = mas sensible; 20 = normal)\n"
                    "VoiceSensitivity=20\n"
                    "; Microfono (se elige mas comodo en el juego con F9). Vacio = predeterminado de Windows\n"
                    "MicDevice=\n"
                    "Overlay=1\n",
                    user);
            fclose(f);
        }
    }
    g_cfg.name = IniStr(ini.c_str(), L"Name", "Jugador");
    g_cfg.hostIp = IniStr(ini.c_str(), L"HostIP", "127.0.0.1");
    g_cfg.port = GetPrivateProfileIntW(L"Online", L"Port", 7777, ini.c_str());
    g_cfg.autoStart = GetPrivateProfileIntW(L"Online", L"AutoStart", 0, ini.c_str());
    g_cfg.avatarMode = IniStr(ini.c_str(), L"AvatarMode", "pawn");
    g_cfg.avatarMesh = IniStr(ini.c_str(), L"AvatarMesh", "");
    g_cfg.sendRate = GetPrivateProfileIntW(L"Online", L"SendRate", 30, ini.c_str());
    if (g_cfg.sendRate < 5) g_cfg.sendRate = 5;
    if (g_cfg.sendRate > 60) g_cfg.sendRate = 60;
    g_cfg.overlay = GetPrivateProfileIntW(L"Online", L"Overlay", 1, ini.c_str()) != 0;
    g_cfg.debug = GetPrivateProfileIntW(L"Online", L"Debug", 0, ini.c_str()) != 0;
    g_cfg.voice = GetPrivateProfileIntW(L"Online", L"VoiceChat", 1, ini.c_str()) != 0;
    g_cfg.voiceRange = (float)GetPrivateProfileIntW(L"Online", L"VoiceRange", 25, ini.c_str());
    g_cfg.voiceThreshold = GetPrivateProfileIntW(L"Online", L"VoiceSensitivity", 20, ini.c_str()) / 1000.f;
    {
        wchar_t mic[128];
        GetPrivateProfileStringW(L"Online", L"MicDevice", L"", mic, 128, ini.c_str());
        g_cfg.micDevice = mic;
    }
    g_cfg.syncLevels = GetPrivateProfileIntW(L"Online", L"SyncLevels", 1, ini.c_str()) != 0;
    g_cfg.syncSaveToDisk = GetPrivateProfileIntW(L"Online", L"SyncSaveToDisk", 1, ini.c_str()) != 0;
    LOG("Config: name=%s host=%s port=%d auto=%d avatar=%s", g_cfg.name.c_str(), g_cfg.hostIp.c_str(), g_cfg.port,
        g_cfg.autoStart, g_cfg.avatarMode.c_str());
}

// ---------------------------------------------------------------------------
// Estado global
// ---------------------------------------------------------------------------
static NetSession g_net;
static VoiceChat g_voice;
static bool g_engineReady = false, g_resolved = false;
static double g_lastInitTry = 0;
static int nPlayerTick = -1, nPostRender = -1;

// Clases y funciones resueltas por nombre
struct Refs {
    UClass *Actor, *Pawn, *Controller, *PlayerController, *HUD, *Canvas, *Font, *SkelComp, *PrimComp, *SkelMeshActor;
    UFunction *Spawn, *SetLocation, *SetRotation, *SetCollision, *SetPhysics, *Destroy, *SetHidden;
    UFunction *SetCompHidden, *DrawRect, *DrawText, *Project, *TextSize, *GetViewPoint, *SetSkeletalMesh, *SetTranslation, *SetCompRotation;
    UObject* font;
    int oLocation, oRotation, oVelocity, oPawn, oCtrlRotation, oCanvas, oMesh, oSkelMesh, oTranslation, oCompRotation;
    int oBounds, oDefaultTexture, oCurX, oCurY, oDrawColor, oFont, oSizeX, oSizeY, oSkelMeshActorComp, oCrouched;
    uint32_t mCrouched;
    // Checkpoints (OLGame)
    UClass *OLGameInfo, *OLPC, *OLPawnCls;
    // Animación
    UClass *AnimNodeSlot, *AnimNodeSequence;
    UFunction *PlayCustomAnim, *StopCustomAnim, *SetAnimPosition;
    int oSlotChildren = -1, oChildAnim = -1, childSize = 0, oCustomChildIndex = -1, oSlotPlaying = -1;
    uint32_t mSlotPlaying = 0;
    int oSeqName = -1, oSeqTime = -1, oSeqRate = -1, oSeqLooping = -1;
    uint32_t mSeqLooping = 0;
    int oCylinder = -1, oCollisionHeight = -1, oCrouchHeight = -1;
    UFunction* StartAtCheckpoint;
    int oCurCheckpoint, oWorldInfo, oGame, oTravelling;
    uint32_t mTravelling;
    std::vector<int> checkpointOrder;  // índices FName en orden de la historia
};
static Refs R{};

struct Avatar {
    UObject* actor = nullptr;
    FVector pos{};
    float yaw = 0, pitch = 0;
    bool placed = false;
};
static Avatar g_avatars[proto::kMaxPlayers];

struct OverlayLine { std::string text; double until; };
static std::deque<OverlayLine> g_lines;

// Entrada de teclado capturada desde el WndProc del juego
static std::mutex g_inputMtx;
static std::vector<int> g_keyQueue;
static bool g_chatOpen = false, g_skipChar = false;
static std::string g_chatBuf;
static HWND g_hwnd = nullptr;
static WNDPROC g_origWndProc = nullptr;

// Menú de conexión (F9): nombre, IP y puerto editables dentro del juego
enum MenuItem { MenuName, MenuIp, MenuPort, MenuMic, MenuJoin, MenuHost, MenuCount };
static constexpr int kActJoin = 0x10000, kActHost = 0x10001;
struct ConnectMenu {
    bool open = false;
    int sel = MenuIp;
    std::string name, ip, port;
    std::vector<std::wstring> mics;  // micrófonos disponibles
    int mic = -1;                    // -1 = predeterminado de Windows
};
static ConnectMenu g_menu;

static void SaveConfig() {
    std::wstring ini = IniPath();
    auto w = [](const std::string& s) {
        wchar_t buf[256];
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, buf, 256);
        return std::wstring(buf);
    };
    WritePrivateProfileStringW(L"Online", L"Name", w(g_cfg.name).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Online", L"HostIP", w(g_cfg.hostIp).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Online", L"Port", std::to_wstring(g_cfg.port).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"Online", L"MicDevice", g_cfg.micDevice.c_str(), ini.c_str());
}

static UObject* g_pc = nullptr;  // PlayerController local
static double g_lastTick = 0, g_lastSend = 0;
static uint32_t g_seq = 0;
static std::string g_mapName;

static void AddLine(const std::string& s) {
    g_lines.push_back({s, NetSession::Now() + 8.0});
    while (g_lines.size() > 7) g_lines.pop_front();
}

// ---------------------------------------------------------------------------
// Resolución de clases/funciones/offsets
// ---------------------------------------------------------------------------
static bool ResolveRefs() {
    R.Actor = G.FindClass("Engine.Actor");
    R.Pawn = G.FindClass("Engine.Pawn");
    R.Controller = G.FindClass("Engine.Controller");
    R.PlayerController = G.FindClass("Engine.PlayerController");
    R.HUD = G.FindClass("Engine.HUD");
    R.Canvas = G.FindClass("Engine.Canvas");
    R.Font = G.FindClass("Engine.Font");
    R.SkelComp = G.FindClass("Engine.SkeletalMeshComponent");
    R.PrimComp = G.FindClass("Engine.PrimitiveComponent");
    R.SkelMeshActor = G.FindClass("Engine.SkeletalMeshActorSpawnable");
    if (!R.Actor || !R.Pawn || !R.PlayerController || !R.HUD || !R.Canvas) {
        LOG("ResolveRefs: faltan clases basicas");
        return false;
    }
    R.Spawn = G.FindFunction(R.Actor, "Spawn");
    R.SetLocation = G.FindFunction(R.Actor, "SetLocation");
    R.SetRotation = G.FindFunction(R.Actor, "SetRotation");
    R.SetCollision = G.FindFunction(R.Actor, "SetCollision");
    R.SetPhysics = G.FindFunction(R.Actor, "SetPhysics");
    R.Destroy = G.FindFunction(R.Actor, "Destroy");
    R.SetHidden = G.FindFunction(R.Actor, "SetHidden");
    R.DrawText = G.FindFunction(R.Canvas, "DrawText");
    R.DrawRect = G.FindFunction(R.Canvas, "DrawRect");
    R.oDefaultTexture = G.PropOff(R.Canvas, "DefaultTexture");
    R.Project = G.FindFunction(R.Canvas, "Project");
    R.TextSize = G.FindFunction(R.Canvas, "TextSize");
    R.GetViewPoint = G.FindFunction(R.Controller, "GetPlayerViewPoint");
    if (R.SkelComp) R.SetSkeletalMesh = G.FindFunction(R.SkelComp, "SetSkeletalMesh");
    if (R.PrimComp) {
        R.SetTranslation = G.FindFunction(R.PrimComp, "SetTranslation");
        R.SetCompRotation = G.FindFunction(R.PrimComp, "SetRotation");
        R.SetCompHidden = G.FindFunction(R.PrimComp, "SetHidden");
    }

    R.oLocation = G.PropOff(R.Actor, "Location");
    R.oRotation = G.PropOff(R.Actor, "Rotation");
    R.oVelocity = G.PropOff(R.Actor, "Velocity");
    R.oPawn = G.PropOff(R.Controller, "Pawn");
    R.oCtrlRotation = R.oRotation;
    R.oCanvas = G.PropOff(R.HUD, "Canvas");
    R.oMesh = G.PropOff(R.Pawn, "Mesh");
    R.oCrouched = G.PropOff(R.Pawn, "bIsCrouched");
    R.mCrouched = G.BoolMask(R.Pawn, "bIsCrouched");
    R.oCurX = G.PropOff(R.Canvas, "CurX");
    R.oCurY = G.PropOff(R.Canvas, "CurY");
    R.oDrawColor = G.PropOff(R.Canvas, "DrawColor");
    R.oFont = G.PropOff(R.Canvas, "Font");
    R.oSizeX = G.PropOff(R.Canvas, "SizeX");
    R.oSizeY = G.PropOff(R.Canvas, "SizeY");
    if (R.SkelComp) R.oSkelMesh = G.PropOff(R.SkelComp, "SkeletalMesh");
    if (R.PrimComp) {
        R.oTranslation = G.PropOff(R.PrimComp, "Translation");
        R.oCompRotation = G.PropOff(R.PrimComp, "Rotation");
        R.oBounds = G.PropOff(R.PrimComp, "Bounds");
    }
    if (R.SkelMeshActor) R.oSkelMeshActorComp = G.PropOff(R.SkelMeshActor, "SkeletalMeshComponent");

    R.OLPawnCls = G.FindClass("OLGame.OLPawn");
    R.AnimNodeSlot = G.FindClass("Engine.AnimNodeSlot");
    R.AnimNodeSequence = G.FindClass("Engine.AnimNodeSequence");
    UClass* blendBase = G.FindClass("Engine.AnimNodeBlendBase");
    UObject* childStruct = G.FindObject("ScriptStruct", "Engine.AnimNodeBlendBase.AnimBlendChild");
    if (R.AnimNodeSlot && R.AnimNodeSequence && blendBase && childStruct) {
        R.PlayCustomAnim = G.FindFunction(R.AnimNodeSlot, "PlayCustomAnim");
        R.StopCustomAnim = G.FindFunction(R.AnimNodeSlot, "StopCustomAnim");
        R.SetAnimPosition = G.FindFunction(R.AnimNodeSequence, "SetPosition");
        R.oSlotChildren = G.PropOff(blendBase, "Children");
        R.oChildAnim = G.PropOff(childStruct, "Anim");
        R.childSize = At<int>(childStruct, off::PropertiesSize);
        R.oCustomChildIndex = G.PropOff(R.AnimNodeSlot, "CustomChildIndex");
        R.oSlotPlaying = G.PropOff(R.AnimNodeSlot, "bIsPlayingCustomAnim");
        R.mSlotPlaying = G.BoolMask(R.AnimNodeSlot, "bIsPlayingCustomAnim");
        R.oSeqName = G.PropOff(R.AnimNodeSequence, "AnimSeqName");
        R.oSeqTime = G.PropOff(R.AnimNodeSequence, "CurrentTime");
        R.oSeqRate = G.PropOff(R.AnimNodeSequence, "Rate");
        R.oSeqLooping = G.PropOff(R.AnimNodeSequence, "bLooping");
        R.mSeqLooping = G.BoolMask(R.AnimNodeSequence, "bLooping");
    }
    R.oCylinder = G.PropOff(R.Actor, "CollisionComponent");
    UClass* cyl = G.FindClass("Engine.CylinderComponent");
    R.oCollisionHeight = cyl ? G.PropOff(cyl, "CollisionHeight") : -1;
    R.oCrouchHeight = G.PropOff(R.Pawn, "CrouchHeight");
    LOG("Anim: slot children=0x%X childSize=%d anim=0x%X seqName=0x%X play=%p", R.oSlotChildren, R.childSize,
        R.oChildAnim, R.oSeqName, (void*)R.PlayCustomAnim);
    R.OLGameInfo = G.FindClass("OLGame.OLGame");
    R.OLPC = G.FindClass("OLGame.OLPlayerController");
    R.oWorldInfo = G.PropOff(R.Actor, "WorldInfo");
    UClass* wi = G.FindClass("Engine.WorldInfo");
    R.oGame = wi ? G.PropOff(wi, "Game") : -1;
    if (R.OLGameInfo) R.oCurCheckpoint = G.PropOff(R.OLGameInfo, "CurrentCheckpointName");
    if (R.OLPC) {
        R.StartAtCheckpoint = G.FindFunction(R.OLPC, "StartNewGameAtCheckpoint");
        R.oTravelling = G.PropOff(R.OLPC, "bTravellingToCheckpoint");
        R.mTravelling = G.BoolMask(R.OLPC, "bTravellingToCheckpoint");
    }
    UClass* listCls = G.FindClass("OLGame.OLCheckpointList");
    // El orden de la historia está en un objeto OLCheckpointList (cogemos el que tenga la lista más larga).
    int oList = listCls ? G.PropOff(listCls, "CheckpointList") : -1;
    if (oList >= 0) {
        UObject* bestList = nullptr;
        for (int i = 0, n = G.ObjCount(); i < n; ++i) {
            UObject* o = G.Obj(i);
            if (!o || !G.IsA(o, listCls)) continue;
            int num = At<TArray<FName>>(o, oList).Num;
            LOG("  OLCheckpointList %s: %d", G.GetPath(o).c_str(), num);
            if (!bestList || num > At<TArray<FName>>(bestList, oList).Num) bestList = o;
        }
        if (bestList) {
            auto& arr = At<TArray<FName>>(bestList, oList);
            for (int i = 0; i < arr.Num; ++i) R.checkpointOrder.push_back(arr.Data[i].Index);
            if (g_cfg.debug)
                for (int i = 0; i < arr.Num; ++i) LOG("  checkpoint[%d] %s", i, G.NameStr(arr.Data[i]).c_str());
        }
    }
    LOG("Checkpoints: lista=%d StartAtCheckpoint=%p CurrentCheckpointName=0x%X", (int)R.checkpointOrder.size(),
        (void*)R.StartAtCheckpoint, R.oCurCheckpoint);

    R.font = G.FindObject("Font", "EngineFonts.SmallFont");
    if (!R.font && R.Font) R.font = G.FindInstance(R.Font);
    LOG("Refs OK. Location=0x%X Pawn=0x%X Canvas=0x%X font=%s", R.oLocation, R.oPawn, R.oCanvas,
        R.font ? G.GetPath(R.font).c_str() : "none");
    return R.Spawn && R.SetLocation && R.oLocation >= 0 && R.oPawn >= 0;
}

// ---------------------------------------------------------------------------
// Utilidades de juego
// ---------------------------------------------------------------------------
static UObject* LocalPawn() {
    if (!G.IsValid(g_pc)) return nullptr;
    UObject* p = At<UObject*>(g_pc, R.oPawn);
    return G.IsValid(p) ? p : nullptr;
}

static std::string MapOf(UObject* o) {
    UObject* top = o;
    while (At<UObject*>(top, off::Outer)) top = At<UObject*>(top, off::Outer);
    return G.GetName(top);
}

static void ActorSetLocation(UObject* a, const FVector& v) {
    Params p(R.SetLocation);
    p.Set("NewLocation", v);
    G.Call(a, R.SetLocation, p.Data());
}

static void ActorSetRotation(UObject* a, const FRotator& r) {
    Params p(R.SetRotation);
    p.Set("NewRotation", r);
    G.Call(a, R.SetRotation, p.Data());
}

static void DestroyAvatar(Avatar& av) {
    if (av.actor && G.IsValid(av.actor) && R.Destroy) {
        Params p(R.Destroy);
        G.Call(av.actor, R.Destroy, p.Data());
    }
    av = Avatar{};
}

static UObject* SpawnActor(UClass* cls, const FVector& loc, const FRotator& rot) {
    if (!cls || !g_pc) return nullptr;
    Params p(R.Spawn);
    p.Set("SpawnClass", cls).Set("SpawnLocation", loc).Set("SpawnRotation", rot).SetBool("bNoCollisionFail", true);
    G.Call(g_pc, R.Spawn, p.Data());
    return p.Get<UObject*>("ReturnValue");
}

static void DumpObject(UObject* o, const char* tag);
static UObject* SpawnAvatarActor(const FVector& loc, const FRotator& rot) {
    UObject* pawn = LocalPawn();
    UObject* a = nullptr;
    bool pawnMode = g_cfg.avatarMode != "mesh";

    if (pawnMode && pawn) {
        UClass* cls = At<UClass*>(pawn, off::Class);
        a = SpawnActor(cls, loc, rot);
        if (a) {
            // Sin física ni colisión: lo movemos nosotros cada frame.
            if (R.SetPhysics) {
                Params p(R.SetPhysics);
                p.Set<uint8_t>("newPhysics", 0);
                G.Call(a, R.SetPhysics, p.Data());
            }
            // Modelo alternativo opcional (p.ej. 02_Waylon_Park.Mesh.Waylon_Park)
            UObject* altMesh = g_cfg.avatarMesh.empty() ? nullptr : G.FindObject("SkeletalMesh", g_cfg.avatarMesh.c_str());
            UObject* body0 = R.oMesh >= 0 ? At<UObject*>(a, R.oMesh) : nullptr;
            if (altMesh && body0 && R.SetSkeletalMesh) {
                Params m(R.SetSkeletalMesh);
                m.Set("NewMesh", altMesh);
                G.Call(body0, R.SetSkeletalMesh, m.Data());
                LOG("Avatar con modelo %s", g_cfg.avatarMesh.c_str());
            }
            // El cuerpo de Miles no tiene cabeza (vista en primera persona): la cabeza es un
            // StaticMeshComponent aparte (OLHero.HeadMesh) que sólo proyecta sombra. La mostramos.
            int oHead = G.PropOff(cls, "HeadMesh");
            UObject* head = oHead >= 0 ? At<UObject*>(a, oHead) : nullptr;
            if (head && R.SetCompHidden) {
                Params h(R.SetCompHidden);
                h.SetBool("NewHidden", false);
                G.Call(head, R.SetCompHidden, h.Data());
                // Usar la iluminación dinámica del cuerpo (si no, se ve con un tono azulado)
                UObject* body = R.oMesh >= 0 ? At<UObject*>(a, R.oMesh) : nullptr;
                int oLE = G.PropOff(R.PrimComp, "LightEnvironment");
                UFunction* setLE = G.FindFunction(R.PrimComp, "SetLightEnvironment");
                if (body && oLE >= 0 && setLE) {
                    LOG("Head LightEnvironment=%s, body=%s", G.GetPath(At<UObject*>(head, oLE)).c_str(),
                        G.GetPath(At<UObject*>(body, oLE)).c_str());
                    Params le(setLE);
                    le.Set("NewLightEnvironment", At<UObject*>(body, oLE));
                    G.Call(head, setLE, le.Data());
                }
            }
            LOG("Avatar (pawn %s) creado: %s", G.GetName(cls).c_str(), G.GetPath(a).c_str());
            if (g_cfg.debug) {
                for (int i = 0, n = G.ObjCount(); i < n; ++i) {
                    UObject* o = G.Obj(i);
                    if (!o) continue;
                    std::string cn = G.ClassName(o);
                    if (cn.find("Material") == std::string::npos && cn.find("Mesh") == std::string::npos &&
                        cn.find("Texture") == std::string::npos) continue;
                    std::string path = G.GetPath(o);
                    if (path.find("Miles") != std::string::npos || path.find("Waylon") != std::string::npos ||
                        path.find("02_Player") != std::string::npos)
                        LOG("  [asset] %s %s", cn.c_str(), path.c_str());
                }
                DumpObject(pawn, "pawn local");
                if (R.oMesh >= 0) DumpObject(At<UObject*>(pawn, R.oMesh), "mesh local");
                DumpObject(a, "avatar");
                if (R.oMesh >= 0) DumpObject(At<UObject*>(a, R.oMesh), "mesh avatar");
                for (const char* n : {"ShadowProxy", "HeadMesh", "CameraMesh", "CameraMeshShadowProxy"}) {
                    int o = G.PropOff(At<UClass*>(a, off::Class), n);
                    if (o >= 0) DumpObject(At<UObject*>(a, o), n);
                }
            }
        } else {
            LOG("No se pudo crear avatar pawn, probando modo mesh");
        }
    }
    if (!a && R.SkelMeshActor) {
        a = SpawnActor(R.SkelMeshActor, loc, rot);
        if (a && R.oSkelMeshActorComp >= 0 && R.SetSkeletalMesh) {
            UObject* comp = At<UObject*>(a, R.oSkelMeshActorComp);
            UObject* heroMesh = (pawn && R.oMesh >= 0) ? At<UObject*>(pawn, R.oMesh) : nullptr;
            UObject* mesh = nullptr;
            if (!g_cfg.avatarMesh.empty()) mesh = G.FindObject("SkeletalMesh", g_cfg.avatarMesh.c_str());
            if (!mesh && heroMesh && R.oSkelMesh >= 0) mesh = At<UObject*>(heroMesh, R.oSkelMesh);
            if (comp && mesh) {
                Params p(R.SetSkeletalMesh);
                p.Set("NewMesh", mesh);
                G.Call(comp, R.SetSkeletalMesh, p.Data());
                if (heroMesh && R.SetTranslation && R.oTranslation >= 0) {
                    Params t(R.SetTranslation);
                    t.Set("NewTranslation", At<FVector>(heroMesh, R.oTranslation));
                    G.Call(comp, R.SetTranslation, t.Data());
                }
                if (heroMesh && R.SetCompRotation && R.oCompRotation >= 0) {
                    Params t(R.SetCompRotation);
                    t.Set("NewRotation", At<FRotator>(heroMesh, R.oCompRotation));
                    G.Call(comp, R.SetCompRotation, t.Data());
                }
            }
            LOG("Avatar (mesh %s) creado: %s", mesh ? G.GetPath(mesh).c_str() : "?", G.GetPath(a).c_str());
        }
    }
    if (a && R.SetCollision) {
        Params p(R.SetCollision);
        p.SetBool("bNewColActors", false).SetBool("bNewBlockActors", false).SetBool("bNewIgnoreEncroachers", true);
        G.Call(a, R.SetCollision, p.Data());
    }
    return a;
}

// Depuración: vuelca al log todas las propiedades de un objeto.
static void DumpObject(UObject* o, const char* tag) {
    if (!o) return;
    LOG("==== %s: %s (%s)", tag, G.GetPath(o).c_str(), G.ClassName(o).c_str());
    for (UStruct* st = At<UObject*>(o, off::Class); st; st = At<UObject*>(st, off::SuperField)) {
        for (UObject* c = At<UObject*>(st, off::Children); c; c = At<UObject*>(c, off::FieldNext)) {
            std::string cn = G.ClassName(c);
            if (cn.size() < 8 || cn.compare(cn.size() - 8, 8, "Property") != 0) continue;
            int po = At<int>(c, off::PropOffset);
            std::string val;
            if (cn == "ObjectProperty" || cn == "ComponentProperty" || cn == "ClassProperty") {
                UObject* v = At<UObject*>(o, po);
                val = v ? G.GetPath(v) : "None";
            } else if (cn == "BoolProperty") {
                val = (At<uint32_t>(o, po) & At<uint32_t>(c, off::BoolBitMask)) ? "true" : "false";
            } else if (cn == "FloatProperty") {
                val = std::to_string(At<float>(o, po));
            } else if (cn == "IntProperty") {
                val = std::to_string(At<int>(o, po));
            } else if (cn == "NameProperty") {
                val = G.NameStr(At<FName>(o, po));
            } else if (cn == "ByteProperty") {
                val = std::to_string(At<uint8_t>(o, po));
            } else if (cn == "ArrayProperty") {
                auto& arr = At<TArray<UObject*>>(o, po);
                val = "[" + std::to_string(arr.Num) + "]";
                for (int k = 0; k < arr.Num && k < 6; ++k)
                    if (G.IsValid(arr.Data[k])) val += " " + G.GetPath(arr.Data[k]);
            }
            LOG("  %s.%s %s @0x%X = %s", G.GetName(st).c_str(), G.GetName(c).c_str(), cn.c_str(), po, val.c_str());
        }
    }
}

// Depuración: registra en el log qué propiedades de un objeto cambian (bytes, bools, ints, names, objetos)
static void WatchObject(UObject* o, const char* tag) {
    static std::map<std::string, std::string> last;
    if (!o) return;
    for (UStruct* st = At<UObject*>(o, off::Class); st; st = At<UObject*>(st, off::SuperField)) {
        for (UObject* c = At<UObject*>(st, off::Children); c; c = At<UObject*>(c, off::FieldNext)) {
            std::string cn = G.ClassName(c);
            int po = At<int>(c, off::PropOffset);
            std::string val;
            if (cn == "BoolProperty") val = (At<uint32_t>(o, po) & At<uint32_t>(c, off::BoolBitMask)) ? "true" : "false";
            else if (cn == "ByteProperty") val = std::to_string(At<uint8_t>(o, po));
            else if (cn == "IntProperty") val = std::to_string(At<int>(o, po));
            else if (cn == "NameProperty") val = G.NameStr(At<FName>(o, po));
            else if (cn == "ObjectProperty") { UObject* v = At<UObject*>(o, po); val = v ? G.GetName(v) : "None"; }
            else continue;
            std::string key = std::string(tag) + "." + G.GetName(st) + "." + G.GetName(c);
            auto it = last.find(key);
            if (it != last.end() && it->second != val)
                LOG("[watch] %s: %s -> %s", key.c_str(), it->second.c_str(), val.c_str());
            last[key] = val;
        }
    }
}

static float WrapDelta(float d) {
    while (d > 32768.f) d -= 65536.f;
    while (d < -32768.f) d += 65536.f;
    return d;
}

// ---------------------------------------------------------------------------
// Lógica por frame (PlayerTick del PlayerController local)
// ---------------------------------------------------------------------------
static void HandleKey(int vk) {
    if (vk == g_cfg.keyHost) {
        if (!g_net.StartHost((uint16_t)g_cfg.port, g_cfg.name)) AddLine("No se pudo abrir el puerto " + std::to_string(g_cfg.port));
    } else if (vk == kActJoin || vk == kActHost) {
        // Valores confirmados en el menú
        if (!g_menu.name.empty()) g_cfg.name = g_menu.name;
        if (!g_menu.ip.empty()) g_cfg.hostIp = g_menu.ip;
        int port = atoi(g_menu.port.c_str());
        if (port > 0 && port < 65536) g_cfg.port = port;
        SaveConfig();
        if (vk == kActJoin) {
            g_net.StartClient(g_cfg.hostIp, (uint16_t)g_cfg.port, g_cfg.name);
        } else if (!g_net.StartHost((uint16_t)g_cfg.port, g_cfg.name)) {
            AddLine("No se pudo abrir el puerto " + std::to_string(g_cfg.port));
        }
    } else if (vk == g_cfg.keyLeave) {
        g_net.Stop();
    } else if (vk == g_cfg.keyVoice && g_cfg.voice) {
        bool on = !g_voice.MicEnabled();
        g_voice.SetMicEnabled(on);
        AddLine(on ? "Microfono ACTIVADO (habla y te oiran los que esten cerca)" : "Microfono desactivado");
    } else if (vk == g_cfg.keyOverlay) {
        g_cfg.overlay = !g_cfg.overlay;
    } else if (vk == g_cfg.keyTeleport) {
        UObject* pawn = LocalPawn();
        auto snap = g_net.Snapshot();
        for (int i = 0; i < proto::kMaxPlayers; ++i) {
            auto& r = snap[i];
            if (!r.active || !r.hasState || i == g_net.MyId()) continue;
            if (g_mapName != r.state.map) continue;
            if (pawn) {
                FVector dst{r.state.pos[0] + 60.f, r.state.pos[1], r.state.pos[2] + 10.f};
                ActorSetLocation(pawn, dst);
                AddLine("Teletransportado junto a " + r.name);
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Sincronización de animaciones
// ---------------------------------------------------------------------------
// Variables del personaje (OLPawn/OLHero/Pawn) que mueven el árbol de animación.
static const char* kSyncByteNames[proto::kSyncBytes] = {
    "LocomotionMode", "SpecialMove", "CamcorderState", "CamcorderMode", "BodySetup", "LedgeClimbType",
    "SqueezeTransitionType", "ActiveLedgeTransitionType", "DoorOpeningType", "DoorPartialOpenType",
    "DoorClosingType", "LastValidCornerPeekPosition"};
static const char* kSyncBoolNames[] = {"bPlayingSpecialMoveAnim", "bCamcorderDesired", "bRightHandIKActive",
                                       "bJumping", "bIsCrouched", "bWantsToCrouch", "bLeaningLeftPushing",
                                       "bLeaningRightPushing", "bForcedCrouch", "bPickupCrouched"};
static constexpr int kSyncBools = sizeof(kSyncBoolNames) / sizeof(kSyncBoolNames[0]);
static const char* kSyncFloatNames[proto::kSyncFloats] = {"CurrentLean", "SpecialMoveBlendAlpha"};
static const char* kSlotNames[proto::kAnimSlots] = {"FullBodyAnimSlot", "RightArmAnimSlot", "LeftArmAnimSlot"};

struct SyncTable {
    UClass* cls = nullptr;
    int byteOff[proto::kSyncBytes];
    int boolOff[kSyncBools];
    uint32_t boolMask[kSyncBools];
    int floatOff[proto::kSyncFloats];
    int slotOff[proto::kAnimSlots];
};
static SyncTable g_sync;

static const SyncTable& Sync(UClass* cls) {
    if (g_sync.cls == cls) return g_sync;
    g_sync.cls = cls;
    for (int i = 0; i < proto::kSyncBytes; ++i) g_sync.byteOff[i] = G.PropOff(cls, kSyncByteNames[i]);
    for (int i = 0; i < kSyncBools; ++i) {
        g_sync.boolOff[i] = G.PropOff(cls, kSyncBoolNames[i]);
        g_sync.boolMask[i] = G.BoolMask(cls, kSyncBoolNames[i]);
    }
    for (int i = 0; i < proto::kSyncFloats; ++i) g_sync.floatOff[i] = G.PropOff(cls, kSyncFloatNames[i]);
    for (int i = 0; i < proto::kAnimSlots; ++i) g_sync.slotOff[i] = G.PropOff(cls, kSlotNames[i]);
    return g_sync;
}

// AnimNodeSequence que está reproduciendo un AnimNodeSlot como animación personalizada
static UObject* SlotSequence(UObject* slot) {
    if (!slot || R.oSlotChildren < 0 || R.oChildAnim < 0 || R.childSize <= 0 || R.oCustomChildIndex < 0) return nullptr;
    auto& children = At<TArray<uint8_t>>(slot, R.oSlotChildren);
    int idx = At<int>(slot, R.oCustomChildIndex);
    if (idx <= 0 || idx >= children.Num) return nullptr;
    UObject* anim = At<UObject*>(children.Data + idx * R.childSize, R.oChildAnim);
    return (anim && G.IsA(anim, R.AnimNodeSequence)) ? anim : nullptr;
}

static void ReadAnimState(UObject* pawn, proto::MsgState& s) {
    UClass* cls = At<UClass*>(pawn, off::Class);
    const SyncTable& t = Sync(cls);
    for (int i = 0; i < proto::kSyncBytes; ++i) s.syncBytes[i] = t.byteOff[i] >= 0 ? At<uint8_t>(pawn, t.byteOff[i]) : 0;
    s.syncBools = 0;
    for (int i = 0; i < kSyncBools; ++i)
        if (t.boolOff[i] >= 0 && (At<uint32_t>(pawn, t.boolOff[i]) & t.boolMask[i])) s.syncBools |= 1u << i;
    for (int i = 0; i < proto::kSyncFloats; ++i) s.syncFloats[i] = t.floatOff[i] >= 0 ? At<float>(pawn, t.floatOff[i]) : 0.f;

    static std::string lastAnim[proto::kAnimSlots];
    static float lastTime[proto::kAnimSlots];
    static bool lastPlaying[proto::kAnimSlots];
    static uint8_t serial[proto::kAnimSlots];
    for (int i = 0; i < proto::kAnimSlots; ++i) {
        auto& o = s.slots[i];
        UObject* slot = t.slotOff[i] >= 0 ? At<UObject*>(pawn, t.slotOff[i]) : nullptr;
        bool playing = slot && R.oSlotPlaying >= 0 && (At<uint32_t>(slot, R.oSlotPlaying) & R.mSlotPlaying);
        UObject* seq = playing ? SlotSequence(slot) : nullptr;
        if (!seq) playing = false;
        std::string name = seq ? G.NameStr(At<FName>(seq, R.oSeqName)) : "";
        float time = seq ? At<float>(seq, R.oSeqTime) : 0.f;
        // Nueva animación: cambia el nombre, empieza a reproducirse o vuelve atrás en el tiempo (reinicio)
        if (playing && (!lastPlaying[i] || name != lastAnim[i] || time + 0.05f < lastTime[i])) ++serial[i];
        lastPlaying[i] = playing;
        lastAnim[i] = name;
        lastTime[i] = time;
        o.playing = playing;
        o.serial = serial[i];
        strncpy_s(o.anim, name.c_str(), _TRUNCATE);
        o.time = time;
        o.rate = seq ? At<float>(seq, R.oSeqRate) : 1.f;
        o.looping = seq && (At<uint32_t>(seq, R.oSeqLooping) & R.mSeqLooping) ? 1 : 0;
    }
}

// FName a partir de texto ("nombre" o "nombre_N")
static FName MakeName(const std::string& s) {
    int idx = G.NameIndex(s.c_str());
    if (idx >= 0) return FName{idx, 0};
    auto us = s.find_last_of('_');
    if (us != std::string::npos && us + 1 < s.size() && isdigit((unsigned char)s[us + 1])) {
        idx = G.NameIndex(s.substr(0, us).c_str());
        if (idx >= 0) return FName{idx, atoi(s.c_str() + us + 1) + 1};
    }
    return FName{-1, 0};
}

struct AvatarAnim {
    uint8_t serial[proto::kAnimSlots] = {};
    bool playing[proto::kAnimSlots] = {};
};
static AvatarAnim g_avatarAnim[proto::kMaxPlayers];

static void ApplyAnimState(int id, UObject* a, const proto::MsgState& s, double age) {
    UClass* cls = At<UClass*>(a, off::Class);
    const SyncTable& t = Sync(cls);
    for (int i = 0; i < proto::kSyncBytes; ++i)
        if (t.byteOff[i] >= 0) At<uint8_t>(a, t.byteOff[i]) = s.syncBytes[i];
    for (int i = 0; i < kSyncBools; ++i)
        if (t.boolOff[i] >= 0) {
            uint32_t& w = At<uint32_t>(a, t.boolOff[i]);
            w = (s.syncBools & (1u << i)) ? (w | t.boolMask[i]) : (w & ~t.boolMask[i]);
        }
    for (int i = 0; i < proto::kSyncFloats; ++i)
        if (t.floatOff[i] >= 0) At<float>(a, t.floatOff[i]) = s.syncFloats[i];

    if (!R.PlayCustomAnim) return;
    AvatarAnim& st = g_avatarAnim[id];
    for (int i = 0; i < proto::kAnimSlots; ++i) {
        UObject* slot = t.slotOff[i] >= 0 ? At<UObject*>(a, t.slotOff[i]) : nullptr;
        if (!slot) continue;
        const auto& r = s.slots[i];
        if (r.playing) {
            float target = r.time + (float)age * r.rate;
            if (!st.playing[i] || st.serial[i] != r.serial) {
                FName n = MakeName(std::string(r.anim, strnlen(r.anim, sizeof(r.anim))));
                if (n.Index >= 0) {
                    Params p(R.PlayCustomAnim);
                    p.Set("AnimName", n).Set("Rate", r.rate > 0.f ? r.rate : 1.f).Set("BlendInTime", 0.15f)
                        .Set("BlendOutTime", 0.15f).SetBool("bLooping", r.looping != 0).SetBool("bOverride", true);
                    G.Call(slot, R.PlayCustomAnim, p.Data());
                    if (g_cfg.debug) LOG("Avatar %d slot %d: %s", id, i, r.anim);
                }
                st.serial[i] = r.serial;
                st.playing[i] = true;
            }
            // Corregir desfase de tiempo
            UObject* seq = SlotSequence(slot);
            if (seq && R.SetAnimPosition && std::fabs(At<float>(seq, R.oSeqTime) - target) > 0.25f) {
                Params p(R.SetAnimPosition);
                p.Set("NewTime", target).SetBool("bFireNotifies", false);
                G.Call(seq, R.SetAnimPosition, p.Data());
            }
        } else if (st.playing[i]) {
            Params p(R.StopCustomAnim);
            p.Set("BlendOutTime", 0.2f);
            G.Call(slot, R.StopCustomAnim, p.Data());
            st.playing[i] = false;
        }
    }
}

// Al agacharse, el motor baja la cápsula del jugador; el avatar no está "agachado" físicamente,
// así que compensamos la altura para que no se hunda en el suelo.
static float CrouchZOffset(UObject* a) {
    if (R.oCrouchHeight < 0 || R.oCylinder < 0 || R.oCollisionHeight < 0) return 0.f;
    UObject* cyl = At<UObject*>(a, R.oCylinder);
    if (!cyl) return 0.f;
    float d = At<float>(cyl, R.oCollisionHeight) - At<float>(a, R.oCrouchHeight);
    return d > 0.f && d < 100.f ? d : 0.f;
}

static void UpdateAvatars(double dt) {
    auto snap = g_net.Snapshot();
    double now = NetSession::Now();
    for (int i = 0; i < proto::kMaxPlayers; ++i) {
        Avatar& av = g_avatars[i];
        if (av.actor && !G.IsValid(av.actor)) av = Avatar{};  // destruido (cambio de nivel, etc.)
        auto& r = snap[i];
        bool want = r.active && r.hasState && i != g_net.MyId() && (r.state.flags & proto::FlagHasPawn) &&
                    g_mapName == r.state.map && LocalPawn() != nullptr;
        if (!want) {
            if (av.actor) DestroyAvatar(av);
            g_avatarAnim[i] = AvatarAnim{};
            continue;
        }
        // Extrapolación corta con la velocidad recibida
        float ext = (float)std::fmin(now - r.stateTime, 0.2);
        FVector target{r.state.pos[0] + r.state.vel[0] * ext, r.state.pos[1] + r.state.vel[1] * ext,
                       r.state.pos[2] + r.state.vel[2] * ext};
        if (!av.actor) {
            av.actor = SpawnAvatarActor(target, FRotator{0, r.state.yaw, 0});
            if (!av.actor) continue;
            av.pos = target;
            av.yaw = (float)r.state.yaw;
            av.placed = true;
        }
        float a = (float)std::fmin(1.0, dt * 12.0);
        float dx = target.X - av.pos.X, dy = target.Y - av.pos.Y, dz = target.Z - av.pos.Z;
        if (dx * dx + dy * dy + dz * dz > 600.f * 600.f) a = 1.f;  // salto grande: teletransporte
        av.pos.X += dx * a;
        av.pos.Y += dy * a;
        av.pos.Z += dz * a;
        av.yaw += WrapDelta((float)r.state.yaw - av.yaw) * a;
        FVector place = av.pos;
        if ((r.state.flags & proto::FlagCrouched) && G.IsA(av.actor, R.Pawn)) place.Z += CrouchZOffset(av.actor);
        ActorSetLocation(av.actor, place);
        ActorSetRotation(av.actor, FRotator{0, (int32_t)av.yaw & 0xFFFF, 0});
        if (R.oVelocity >= 0) At<FVector>(av.actor, R.oVelocity) = FVector{r.state.vel[0], r.state.vel[1], r.state.vel[2]};
        if (G.IsA(av.actor, R.Pawn)) ApplyAnimState(i, av.actor, r.state, now - r.stateTime);
    }
}

// ---------------------------------------------------------------------------
// Sincronización de niveles por checkpoint
// ---------------------------------------------------------------------------
static std::string LocalCheckpoint() {
    if (!G.IsValid(g_pc) || R.oWorldInfo < 0 || R.oGame < 0 || R.oCurCheckpoint < 0) return "";
    UObject* wi = At<UObject*>(g_pc, R.oWorldInfo);
    UObject* game = wi ? At<UObject*>(wi, R.oGame) : nullptr;
    if (!game || !G.IsA(game, R.OLGameInfo)) return "";
    std::string n = G.NameStr(At<FName>(game, R.oCurCheckpoint));
    return n == "None" ? "" : n;
}

// Posición del checkpoint en la historia (-1 si no se conoce)
static int CheckpointRank(const std::string& name) {
    if (name.empty()) return -1;
    int idx = G.NameIndex(name.c_str());
    for (size_t i = 0; i < R.checkpointOrder.size(); ++i)
        if (R.checkpointOrder[i] == idx) return (int)i;
    return -1;
}

static std::string g_checkpoint;   // checkpoint local actual
static std::string g_syncTarget;   // checkpoint al que nos estamos sincronizando
static double g_syncTargetSince = 0, g_lastSyncAttempt = -100;

static bool Travelling() {
    return R.oTravelling >= 0 && G.IsValid(g_pc) && (At<uint32_t>(g_pc, R.oTravelling) & R.mTravelling);
}

static void LoadCheckpoint(const std::string& cp) {
    if (!R.StartAtCheckpoint || !G.IsA(g_pc, R.OLPC)) return;
    std::wstring w(cp.begin(), cp.end());
    Params p(R.StartAtCheckpoint);
    p.Set("CheckpointStr", FString{w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1});
    p.SetBool("bSaveToDisk", g_cfg.syncSaveToDisk);
    LOG("Sincronizando: cargando checkpoint %s (local=%s)", cp.c_str(), g_checkpoint.c_str());
    G.Call(g_pc, R.StartAtCheckpoint, p.Data());
}

static void SyncLevels() {
    double now = NetSession::Now();
    std::string prev = g_checkpoint;
    g_checkpoint = LocalCheckpoint();
    if (g_checkpoint != prev) LOG("Checkpoint local: '%s' (rank %d)", g_checkpoint.c_str(), CheckpointRank(g_checkpoint));
    if (!g_cfg.syncLevels || !g_net.Connected() || !R.StartAtCheckpoint || Travelling()) {
        g_syncTarget.clear();
        return;
    }
    // El checkpoint más avanzado entre todos los jugadores.
    // Si no se conoce el orden (lista vacía), manda el host.
    auto snap = g_net.Snapshot();
    std::string best;
    std::string bestWho;
    int bestRank = CheckpointRank(g_checkpoint);
    for (int i = 0; i < proto::kMaxPlayers; ++i) {
        auto& r = snap[i];
        if (!r.active || !r.hasState || i == g_net.MyId()) continue;
        std::string cp(r.state.checkpoint, strnlen(r.state.checkpoint, sizeof(r.state.checkpoint)));
        if (cp.empty() || cp == g_checkpoint) continue;
        int rank = CheckpointRank(cp);
        bool ahead = rank >= 0 ? rank > bestRank : (R.checkpointOrder.empty() && i == 0);
        if (ahead) {
            best = cp;
            bestWho = r.name;
            bestRank = rank;
        }
    }
    if (best.empty()) {
        g_syncTarget.clear();
        return;
    }
    if (best != g_syncTarget) {  // nuevo objetivo: esperar a que sea estable
        g_syncTarget = best;
        g_syncTargetSince = now;
        AddLine(bestWho + " ha llegado a " + best + ". Sincronizando en 3 s...");
        return;
    }
    if (now - g_syncTargetSince < 3.0 || now - g_lastSyncAttempt < 20.0) return;
    g_lastSyncAttempt = now;
    AddLine("Cargando checkpoint " + best + " para seguir a " + bestWho);
    LoadCheckpoint(best);
}

// ---------------------------------------------------------------------------
// Chat de voz: arranque/parada y volumen por proximidad
// ---------------------------------------------------------------------------
static void UpdateVoice() {
    if (!g_cfg.voice) return;
    if (g_net.Connected() && !g_voice.Running()) {
        g_voice.SetThreshold(g_cfg.voiceThreshold);
        g_voice.SetDevice(g_cfg.micDevice);
        g_voice.Start([](const proto::MsgVoice& m) { g_net.SendVoice(m); });
    } else if (g_net.GetMode() == NetSession::Mode::Off && g_voice.Running()) {
        g_voice.Stop();
    }
    if (g_voice.micError.exchange(false)) AddLine("No se encontro ningun microfono (revisa Windows > Privacidad > Microfono)");

    UObject* pawn = LocalPawn();
    auto snap = g_net.Snapshot();
    float range = g_cfg.voiceRange * 52.5f;  // metros -> unidades de Unreal
    const float full = 150.f;                // volumen completo a menos de ~3 m
    FRotator rot = G.IsValid(g_pc) ? At<FRotator>(g_pc, R.oCtrlRotation) : FRotator{};
    const float k = 3.14159265f / 32768.f;
    float rx = -std::sin(rot.Yaw * k), ry = std::cos(rot.Yaw * k);  // vector "derecha"
    for (int i = 0; i < proto::kMaxPlayers; ++i) {
        auto& r = snap[i];
        float gl = 0, gr = 0;
        if (pawn && r.active && r.hasState && i != g_net.MyId() && g_mapName == r.state.map) {
            FVector me = At<FVector>(pawn, R.oLocation);
            float dx = r.state.pos[0] - me.X, dy = r.state.pos[1] - me.Y, dz = r.state.pos[2] - me.Z;
            float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            float vol = dist <= full ? 1.f : dist >= range ? 0.f : 1.f - (dist - full) / (range - full);
            vol *= vol;  // caída más natural
            float h = std::sqrt(dx * dx + dy * dy);
            float pan = h > 1.f ? (dx * rx + dy * ry) / h : 0.f;  // -1 izquierda, +1 derecha
            float a = (pan + 1.f) * 0.25f * 3.14159265f;
            gl = vol * std::cos(a) * 1.41421f;
            gr = vol * std::sin(a) * 1.41421f;
        }
        g_voice.SetGain(i, gl, gr);
    }
}

static void Tick() {
    double now = NetSession::Now();
    double dt = g_lastTick > 0 ? now - g_lastTick : 0.016;
    if (dt > 0.25) dt = 0.25;
    g_lastTick = now;

    std::vector<int> keys;
    {
        std::lock_guard<std::mutex> lk(g_inputMtx);
        keys.swap(g_keyQueue);
    }
    for (int k : keys) HandleKey(k);
    for (auto& e : g_net.PopEvents()) AddLine(e);

    g_mapName = MapOf(g_pc);
    static double lastSync = 0;
    if (now - lastSync > 0.5) {
        lastSync = now;
        SyncLevels();
    }
    UObject* pawn = LocalPawn();

    if (g_net.Connected() && now - g_lastSend >= 1.0 / g_cfg.sendRate) {
        g_lastSend = now;
        proto::MsgState s{};
        s.seq = ++g_seq;
        strncpy_s(s.map, g_mapName.c_str(), _TRUNCATE);
        strncpy_s(s.checkpoint, g_checkpoint.c_str(), _TRUNCATE);
        if (pawn) {
            FVector loc = At<FVector>(pawn, R.oLocation);
            FVector vel = R.oVelocity >= 0 ? At<FVector>(pawn, R.oVelocity) : FVector{};
            FRotator rot = At<FRotator>(g_pc, R.oCtrlRotation);
            s.pos[0] = loc.X; s.pos[1] = loc.Y; s.pos[2] = loc.Z;
            s.vel[0] = vel.X; s.vel[1] = vel.Y; s.vel[2] = vel.Z;
            s.pitch = rot.Pitch;
            s.yaw = rot.Yaw;
            s.flags |= proto::FlagHasPawn;
            if (R.oCrouched >= 0 && (At<uint32_t>(pawn, R.oCrouched) & R.mCrouched)) s.flags |= proto::FlagCrouched;
            if (vel.X * vel.X + vel.Y * vel.Y > 300.f * 300.f) s.flags |= proto::FlagRunning;
            ReadAnimState(pawn, s);
        }
        g_net.SendState(s);
    }
    UpdateAvatars(dt);
    UpdateVoice();
    if (g_cfg.debug && pawn) {
        static double lastWatch = 0;
        if (now - lastWatch > 0.25) {
            lastWatch = now;
            WatchObject(pawn, "hero");
            if (R.oMesh >= 0) {
                // Animación de cuerpo completo en curso (parkour, armario, cama...)
                UObject* mesh = At<UObject*>(pawn, R.oMesh);
                int oSlot = G.PropOff(R.OLPawnCls, "FullBodyAnimSlot");
                UObject* slot = oSlot >= 0 ? At<UObject*>(pawn, oSlot) : nullptr;
                if (slot) WatchObject(slot, "slot");
                (void)mesh;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Overlay (HUD.PostRender)
// ---------------------------------------------------------------------------
static void CanvasText(UObject* canvas, float x, float y, const std::string& text, FColor col) {
    if (!R.DrawText || R.oCurX < 0) return;
    wchar_t wbuf[512];
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, wbuf, 512);
    std::wstring w(wbuf);
    if (R.oFont >= 0 && R.font) At<UObject*>(canvas, R.oFont) = R.font;
    // sombra
    At<FColor>(canvas, R.oDrawColor) = FColor{0, 0, 0, col.A};
    for (int pass = 0; pass < 2; ++pass) {
        At<float>(canvas, R.oCurX) = x + (pass ? 0.f : 1.f);
        At<float>(canvas, R.oCurY) = y + (pass ? 0.f : 1.f);
        if (pass) At<FColor>(canvas, R.oDrawColor) = col;
        Params p(R.DrawText);
        p.Set("Text", FString{w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1});
        p.SetBool("CR", false).Set("XScale", 1.f).Set("YScale", 1.f);
        G.Call(canvas, R.DrawText, p.Data());
    }
}

static void DrawBox(UObject* canvas, float x, float y, float w, float h, FColor c) {
    if (!R.DrawRect || R.oDefaultTexture < 0) return;
    At<FColor>(canvas, R.oDrawColor) = c;
    At<float>(canvas, R.oCurX) = x;
    At<float>(canvas, R.oCurY) = y;
    Params p(R.DrawRect);
    p.Set("RectX", w).Set("RectY", h).Set("Tex", At<UObject*>(canvas, R.oDefaultTexture));
    G.Call(canvas, R.DrawRect, p.Data());
}

// Icono de micrófono abajo a la izquierda
static void DrawMicIcon(UObject* canvas) {
    if (!g_cfg.voice) return;
    float sy = R.oSizeY >= 0 ? (float)At<int>(canvas, R.oSizeY) : 600.f;
    float x = 16.f, y = sy - 66.f;
    bool on = g_voice.MicEnabled(), tx = on && g_voice.Transmitting();
    FColor col = !on ? FColor{150, 150, 150, 230} : tx ? FColor{90, 230, 90, 255} : FColor{240, 240, 240, 255};

    DrawBox(canvas, x, y, 150.f, 50.f, FColor{0, 0, 0, 140});
    // cápsula
    DrawBox(canvas, x + 15.f, y + 6.f, 10.f, 2.f, col);
    DrawBox(canvas, x + 14.f, y + 8.f, 12.f, 18.f, col);
    DrawBox(canvas, x + 15.f, y + 26.f, 10.f, 2.f, col);
    // soporte en U
    DrawBox(canvas, x + 9.f, y + 18.f, 2.f, 10.f, col);
    DrawBox(canvas, x + 29.f, y + 18.f, 2.f, 10.f, col);
    DrawBox(canvas, x + 10.f, y + 28.f, 20.f, 2.f, col);
    DrawBox(canvas, x + 12.f, y + 30.f, 16.f, 2.f, col);
    // pie
    DrawBox(canvas, x + 19.f, y + 32.f, 2.f, 6.f, col);
    DrawBox(canvas, x + 13.f, y + 38.f, 14.f, 3.f, col);
    if (!on) {  // tachado rojo
        for (int i = 0; i < 16; ++i) DrawBox(canvas, x + 5.f + i * 2.f, y + 5.f + i * 2.3f, 3.f, 3.f, FColor{40, 40, 230, 255});
    } else {    // nivel del micrófono
        DrawBox(canvas, x + 37.f, y + 6.f, 4.f, 35.f, FColor{60, 60, 60, 200});
        float lv = g_voice.MicLevel() * 35.f;
        DrawBox(canvas, x + 37.f, y + 41.f - lv, 4.f, lv, tx ? FColor{90, 230, 90, 255} : FColor{200, 200, 200, 255});
    }
    CanvasText(canvas, x + 50.f, y + 9.f, on ? (tx ? "HABLANDO" : "MIC ON") : "MIC OFF", col);
    CanvasText(canvas, x + 50.f, y + 27.f, on ? "[M] desactivar" : "[M] activar", FColor{180, 180, 180, 220});
}

static void DrawMenu(UObject* canvas) {
    float sx = R.oSizeX >= 0 ? (float)At<int>(canvas, R.oSizeX) : 800.f;
    float sy = R.oSizeY >= 0 ? (float)At<int>(canvas, R.oSizeY) : 600.f;
    const float w = 560.f, h = 225.f;
    float x = (sx - w) * 0.5f, y = (sy - h) * 0.5f;
    // Fondo semitransparente
    if (R.DrawRect && R.oDefaultTexture >= 0) {
        At<FColor>(canvas, R.oDrawColor) = FColor{0, 0, 0, 200};
        At<float>(canvas, R.oCurX) = x;
        At<float>(canvas, R.oCurY) = y;
        Params p(R.DrawRect);
        p.Set("RectX", w).Set("RectY", h).Set("Tex", At<UObject*>(canvas, R.oDefaultTexture));
        G.Call(canvas, R.DrawRect, p.Data());
    }
    const FColor title{120, 255, 140, 255}, normal{200, 200, 200, 255}, active{80, 220, 255, 255};
    bool blink = std::fmod(NetSession::Now(), 1.0) < 0.5;
    auto line = [&](int item, const std::string& text) {
        bool sel = g_menu.sel == item;
        CanvasText(canvas, x + 20.f, y + 40.f + item * 22.f, (sel ? "> " : "  ") + text, sel ? active : normal);
    };
    auto cursor = [&](int item) { return g_menu.sel == item && blink ? "_" : ""; };
    CanvasText(canvas, x + 20.f, y + 12.f, "OUTLAST ONLINE - Conexion", title);
    line(MenuName, "Nombre:       " + g_menu.name + cursor(MenuName));
    line(MenuIp, "IP servidor:  " + g_menu.ip + cursor(MenuIp));
    line(MenuPort, "Puerto:       " + g_menu.port + cursor(MenuPort));
    {
        std::wstring wn = g_menu.mic < 0 ? L"Predeterminado de Windows" : g_menu.mics[g_menu.mic];
        char nm[256];
        WideCharToMultiByte(CP_UTF8, 0, wn.c_str(), -1, nm, sizeof(nm), nullptr, nullptr);
        std::string meter;
        if (g_voice.MicEnabled()) {
            int bars = (int)(g_voice.MicLevel() * 10.f + 0.5f);
            meter = "  [" + std::string(bars, '|') + std::string(10 - bars, '.') + "]";
        }
        line(MenuMic, std::string("Microfono:   < ") + nm + " >" + meter);
    }
    line(MenuJoin, "[ Unirse ]");
    line(MenuHost, "[ Hospedar partida ]");
    CanvasText(canvas, x + 20.f, y + h - 26.f, "Flechas/Tab: mover   Izq/Der: cambiar micro   Enter: aceptar   Esc: cerrar", normal);
}

static void DrawOverlay(UObject* hud) {
    if (R.oCanvas < 0) return;
    UObject* canvas = At<UObject*>(hud, R.oCanvas);
    if (!canvas || !G.IsValid(canvas)) return;
    double now = NetSession::Now();
    while (!g_lines.empty() && g_lines.front().until < now) g_lines.pop_front();

    const FColor white{255, 255, 255, 255}, green{120, 255, 140, 255}, yellow{80, 220, 255, 255};
    float y = 12.f;
    if (g_cfg.overlay) {
        std::string status;
        auto snap = g_net.Snapshot();
        int others = 0;
        float rtt = 0;
        for (int i = 0; i < proto::kMaxPlayers; ++i)
            if (snap[i].active && i != g_net.MyId()) { ++others; rtt = std::fmax(rtt, snap[i].rttMs); }
        switch (g_net.GetMode()) {
        case NetSession::Mode::Off:
            status = "Outlast Online  |  F8 hospedar   F9 menu de conexion (IP / puerto)";
            break;
        case NetSession::Mode::Host:
            status = "Outlast Online  |  HOST :" + std::to_string(g_cfg.port) + "  |  " + std::to_string(others + 1) +
                     " jugadores";
            break;
        case NetSession::Mode::Client:
            status = g_net.Connected() ? "Outlast Online  |  conectado  |  ping " + std::to_string((int)rtt) + " ms  |  " +
                                             std::to_string(others + 1) + " jugadores"
                                       : "Outlast Online  |  conectando a " + g_cfg.hostIp + "...";
            break;
        }
        CanvasText(canvas, 12.f, y, status, green);
        y += 18.f;
        // Jugadores en otros mapas
        for (int i = 0; i < proto::kMaxPlayers; ++i) {
            auto& r = snap[i];
            if (!r.active || i == g_net.MyId() || !r.hasState) continue;
            if (g_mapName != r.state.map) {
                std::string cp(r.state.checkpoint, strnlen(r.state.checkpoint, sizeof(r.state.checkpoint)));
                CanvasText(canvas, 12.f, y, "  " + r.name + " esta en " + r.state.map + (cp.empty() ? "" : " (" + cp + ")"),
                           white);
                y += 16.f;
            }
        }
    }
    for (auto& l : g_lines) {
        CanvasText(canvas, 12.f, y, l.text, yellow);
        y += 16.f;
    }
    DrawMicIcon(canvas);
    if (g_menu.open) DrawMenu(canvas);
    if (g_chatOpen) {
        float sy = R.oSizeY >= 0 ? (float)At<int>(canvas, R.oSizeY) : 600.f;
        CanvasText(canvas, 12.f, sy * 0.75f, "Chat: " + g_chatBuf + "_", white);
    }

    // Nombres sobre los avatares
    if (!g_cfg.overlay || !R.Project || !R.GetViewPoint || !G.IsValid(g_pc)) return;
    Params vp(R.GetViewPoint);
    G.Call(g_pc, R.GetViewPoint, vp.Data());
    FVector camLoc = vp.Get<FVector>("out_Location");
    FRotator camRot = vp.Get<FRotator>("out_Rotation");
    const float k = 3.14159265f / 32768.f;
    FVector dir{std::cos(camRot.Pitch * k) * std::cos(camRot.Yaw * k), std::cos(camRot.Pitch * k) * std::sin(camRot.Yaw * k),
                std::sin(camRot.Pitch * k)};
    auto snap = g_net.Snapshot();
    for (int i = 0; i < proto::kMaxPlayers; ++i) {
        Avatar& av = g_avatars[i];
        if (!av.actor || !G.IsValid(av.actor)) continue;
        // Parte superior de la caja de colisión visual del modelo (PrimitiveComponent.Bounds)
        FVector head{av.pos.X, av.pos.Y, av.pos.Z + 90.f};
        UObject* body = (R.oMesh >= 0 && G.IsA(av.actor, R.Pawn)) ? At<UObject*>(av.actor, R.oMesh) : nullptr;
        if (body && R.oBounds >= 0) {
            FVector origin = At<FVector>(body, R.oBounds), ext = At<FVector>(body, R.oBounds + 12);
            if (ext.Z > 10.f && ext.Z < 300.f) head = FVector{origin.X, origin.Y, origin.Z + ext.Z + 12.f};
        }
        FVector d{head.X - camLoc.X, head.Y - camLoc.Y, head.Z - camLoc.Z};
        float dist = std::sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
        if (d.X * dir.X + d.Y * dir.Y + d.Z * dir.Z <= 0 || dist > 6000.f) continue;  // detrás de la cámara
        Params p(R.Project);
        p.Set("Location", head);
        G.Call(canvas, R.Project, p.Data());
        FVector s = p.Get<FVector>("ReturnValue");
        float maxX = R.oSizeX >= 0 ? (float)At<int>(canvas, R.oSizeX) : 4096.f;
        if (s.X < 0 || s.X > maxX) continue;  // fuera de pantalla
        std::string label = (g_voice.Speaking(i) ? "<)) " : "") + snap[i].name + "  (" +
                            std::to_string((int)(dist / 52.5f)) + " m)";
        CanvasText(canvas, s.X - label.size() * 3.5f, s.Y, label, white);
    }
}

// ---------------------------------------------------------------------------
// Hook de ProcessEvent
// ---------------------------------------------------------------------------
static void EnsureWindowHook();
static thread_local int t_depth = 0;
static DWORD g_mainThread = 0;

static void TryInit() {
    double now = NetSession::Now();
    if (now - g_lastInitTry < 1.0) return;
    g_lastInitTry = now;
    if (!G.Init()) return;
    nPlayerTick = G.NameIndex("PlayerTick");
    nPostRender = G.NameIndex("PostRender");
    g_engineReady = true;
    LOG("Motor listo: %d objetos, PlayerTick=%d PostRender=%d", G.ObjCount(), nPlayerTick, nPostRender);
    if (g_cfg.debug) {
        // Volcado de campos/funciones de OLGame relacionados con checkpoints y niveles
        for (int i = 0, n = G.ObjCount(); i < n; ++i) {
            UObject* o = G.Obj(i);
            if (!o) continue;
            std::string path = G.GetPath(o);
            if (path.rfind("OLGame.", 0) != 0 && path.rfind("Engine.", 0) != 0) continue;
            std::string nm = G.GetName(o), cn = G.ClassName(o);
            bool interesting = nm.find("heckpoint") != std::string::npos || nm.find("Respawn") != std::string::npos ||
                               (path.rfind("OLGame.", 0) == 0 && (nm.find("Level") != std::string::npos ||
                                                                 nm.find("Stream") != std::string::npos ||
                                                                 nm.find("Chapter") != std::string::npos ||
                                                                 nm.find("Save") != std::string::npos ||
                                                                 nm.find("Load") != std::string::npos));
            if (interesting && cn != "Package") LOG("  [ol] %s %s", cn.c_str(), path.c_str());
        }
    }
}

static void OnEvent(UObject* self, UFunction* fn, bool after) {
    if (!g_engineReady) {
        if (!after) TryInit();
        return;
    }
    int n = At<FName>(fn, off::Name).Index;
    if (n != nPlayerTick && n != nPostRender) return;
    if (!g_resolved) {
        g_resolved = ResolveRefs();
        if (!g_resolved) { g_engineReady = false; return; }
        if (g_cfg.autoStart == 1) g_net.StartHost((uint16_t)g_cfg.port, g_cfg.name);
        else if (g_cfg.autoStart == 2) g_net.StartClient(g_cfg.hostIp, (uint16_t)g_cfg.port, g_cfg.name);
        AddLine("Outlast Online cargado. F8 = hospedar, F9 = menu IP/puerto, F10 = salir, F11 = ir al companero, T = chat");
    }
    EnsureWindowHook();
    if (n == nPlayerTick && !after && G.IsA(self, R.PlayerController)) {
        g_pc = self;
        Tick();
    } else if (n == nPostRender && after && G.IsA(self, R.HUD)) {
        DrawOverlay(self);
    }
}

#ifdef _WIN64
static void __fastcall HookProcessEvent(UObject* self, UFunction* fn, void* parms, void* result) {
#else
// ProcessEvent es __thiscall (this en ECX, callee limpia la pila); __fastcall con EDX ficticio es equivalente.
static void __fastcall HookProcessEvent(UObject* self, void* /*edx*/, UFunction* fn, void* parms, void* result) {
#endif
    bool mine = t_depth == 0 && fn && GetCurrentThreadId() == g_mainThread;
    if (mine) {
        ++t_depth;
        __try { OnEvent(self, fn, false); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Excepcion en OnEvent(pre)"); }
        --t_depth;
    }
    G.processEvent(self, fn, parms, result);
    if (mine) {
        ++t_depth;
        __try { OnEvent(self, fn, true); } __except (EXCEPTION_EXECUTE_HANDLER) { LOG("Excepcion en OnEvent(post)"); }
        --t_depth;
    }
}

// ---------------------------------------------------------------------------
// Ventana: teclas y chat
// ---------------------------------------------------------------------------
// Cambia de micrófono en el menú y lo aplica/guarda al momento
static void CycleMic(int dir) {
    int n = (int)g_menu.mics.size();
    g_menu.mic += dir;
    if (g_menu.mic >= n) g_menu.mic = -1;
    if (g_menu.mic < -1) g_menu.mic = n - 1;
    g_cfg.micDevice = g_menu.mic < 0 ? L"" : g_menu.mics[g_menu.mic];
    g_voice.SetDevice(g_cfg.micDevice);
    std::wstring ini = IniPath();
    WritePrivateProfileStringW(L"Online", L"MicDevice", g_cfg.micDevice.c_str(), ini.c_str());
}

static bool MenuInput(UINT msg, WPARAM wp) {
    if (msg == WM_KEYDOWN) {
        switch (wp) {
        case VK_ESCAPE: g_menu.open = false; break;
        case VK_UP: g_menu.sel = (g_menu.sel + MenuCount - 1) % MenuCount; break;
        case VK_DOWN:
        case VK_TAB: g_menu.sel = (g_menu.sel + 1) % MenuCount; break;
        case VK_LEFT:
        case VK_RIGHT:
            if (g_menu.sel == MenuMic) CycleMic(wp == VK_LEFT ? -1 : 1);
            break;
        case VK_RETURN:
            if (g_menu.sel == MenuMic) {
                CycleMic(1);
            } else if (g_menu.sel == MenuJoin || g_menu.sel == MenuHost) {
                std::lock_guard<std::mutex> lk(g_inputMtx);
                g_keyQueue.push_back(g_menu.sel == MenuJoin ? kActJoin : kActHost);
                g_menu.open = false;
            } else {
                g_menu.sel++;
            }
            break;
        }
        return true;
    }
    if (msg == WM_CHAR) {
        std::string* field = g_menu.sel == MenuName ? &g_menu.name
                             : g_menu.sel == MenuIp ? &g_menu.ip
                             : g_menu.sel == MenuPort ? &g_menu.port
                                                      : nullptr;
        if (!field) return true;
        char c = (char)wp;
        if (wp == '\b') {
            if (!field->empty()) field->pop_back();
        } else if (wp >= 32 && wp < 127) {
            bool ok = g_menu.sel == MenuPort ? (c >= '0' && c <= '9' && field->size() < 5)
                    : g_menu.sel == MenuIp   ? ((isalnum((unsigned char)c) || c == '.' || c == '-') && field->size() < 63)
                                             : field->size() < proto::kNameLen - 1;
            if (ok) field->push_back(c);
        }
        return true;
    }
    return msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP;
}

static LRESULT CALLBACK WndProcHook(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_menu.open) {
        if (MenuInput(msg, wp)) return 0;  // el juego no recibe el teclado con el menú abierto
    } else if (g_chatOpen) {
        if (msg == WM_CHAR) {
            std::lock_guard<std::mutex> lk(g_inputMtx);
            if (wp == '\r') {
                if (!g_chatBuf.empty()) g_net.SendChat(g_chatBuf);
                g_chatBuf.clear();
                g_chatOpen = false;
            } else if (wp == 27) {
                g_chatBuf.clear();
                g_chatOpen = false;
            } else if (wp == '\b') {
                if (!g_chatBuf.empty()) g_chatBuf.pop_back();
            } else if (wp >= 32 && wp < 127 && g_chatBuf.size() < 100) {
                g_chatBuf.push_back((char)wp);
            }
            return 0;
        }
        if (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) {
            if (msg == WM_KEYDOWN && wp == VK_ESCAPE) { g_chatOpen = false; g_chatBuf.clear(); }
            return 0;  // el juego no ve las teclas mientras escribes
        }
    } else if (msg == WM_KEYDOWN && !(lp & (1 << 30))) {
        int vk = (int)wp;
        if (vk == g_cfg.keyJoin) {
            g_menu.open = true;
            g_menu.sel = MenuIp;
            g_menu.name = g_cfg.name;
            g_menu.ip = g_cfg.hostIp;
            g_menu.port = std::to_string(g_cfg.port);
            g_menu.mics = VoiceChat::ListDevices();
            g_menu.mic = -1;
            for (int i = 0; i < (int)g_menu.mics.size(); ++i)
                if (!g_cfg.micDevice.empty() && g_menu.mics[i] == g_cfg.micDevice) g_menu.mic = i;
            return 0;
        }
        if (vk == g_cfg.keyChat && g_net.Connected()) {
            g_chatOpen = true;
            g_skipChar = true;
            g_chatBuf.clear();
            return 0;
        }
        if (vk == g_cfg.keyVoice || vk == g_cfg.keyHost || vk == g_cfg.keyLeave || vk == g_cfg.keyTeleport ||
            vk == g_cfg.keyOverlay) {
            std::lock_guard<std::mutex> lk(g_inputMtx);
            g_keyQueue.push_back(vk);
        }
    }
    return CallWindowProcW(g_origWndProc, h, msg, wp, lp);
}

struct WinSearch { HWND best; LONG area; };

static BOOL CALLBACK FindGameWindow(HWND h, LPARAM lp) {
    auto ws = (WinSearch*)lp;
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r;
    GetClientRect(h, &r);
    LONG area = (r.right - r.left) * (r.bottom - r.top);
    if (area > ws->area) { ws->best = h; ws->area = area; }
    return TRUE;
}

// Se llama desde el hilo del juego: engancha el WndProc de la ventana principal
// (al arrancar existe primero la ventana del splash, por eso se revisa periódicamente).
static void EnsureWindowHook() {
    static double last = 0;
    double now = NetSession::Now();
    if (now - last < 2.0) return;
    last = now;
    WinSearch ws{nullptr, 0};
    EnumWindows(FindGameWindow, (LPARAM)&ws);
    if (!ws.best || ws.best == g_hwnd) return;
    if (g_hwnd && IsWindow(g_hwnd) && g_origWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);
    g_hwnd = ws.best;
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)WndProcHook);
    LOG("Ventana del juego enganchada: %p", g_hwnd);
}

// ---------------------------------------------------------------------------
void Startup(HMODULE self) {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    g_dir = path;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1);
    logx::Init((g_dir + L"OutlastOnline.log").c_str());
    LOG("Outlast Online v1.1 (%s) cargando desde %ls", sizeof(void*) == 8 ? "64 bits" : "32 bits", path);
    g_mainThread = GetCurrentThreadId();
    LoadConfig();
    g_net.onVoice = [](int id, const proto::MsgVoice& m) { g_voice.OnPacket(id, m); };

#ifdef _WIN64
    // UObject::ProcessEvent (Binaries/Win64/OLGame.exe, Steam). Prólogo de 18 bytes sin RIP-relativo.
    static const unsigned char kPrologue[] = {0x40, 0x55, 0x41, 0x55, 0x41, 0x56, 0x48, 0x81, 0xEC,
                                              0xB0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x6C, 0x24, 0x20};
    static const unsigned char kTail[] = {0x48, 0xC7, 0x45, 0x78, 0xFE, 0xFF, 0xFF, 0xFF};
    const size_t tailAt = 18;
    const uintptr_t kRva = 0x71E50;
    // test dword ptr [rdx+0xD0], 0x402  (Function->FunctionFlags & (FUNC_Native|FUNC_Defined))
    static const unsigned char kChk[] = {0xF7, 0x82, 0xD0, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x00};
#else
    // UObject::ProcessEvent (Binaries/Win32/OLGame.exe, Steam): push ebp; mov ebp,esp; push -1 (5 bytes)
    static const unsigned char kPrologue[] = {0x55, 0x8B, 0xEC, 0x6A, 0xFF};
    // tras "push imm32" (SEH): mov eax,fs:[0]; push eax; sub esp,0x54
    static const unsigned char kTail[] = {0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC, 0x54};
    const size_t tailAt = 10;
    const uintptr_t kRva = 0x65180;
    // test dword ptr [ebx+0x84], 0x402
    static const unsigned char kChk[] = {0xF7, 0x83, 0x84, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x00};
#endif
    const size_t kSteal = sizeof(kPrologue);
    uint8_t* base = (uint8_t*)GetModuleHandleW(nullptr);
    uint8_t* target = base + kRva;
    uint8_t probe[64];
    bool ok = SafeRead(target, probe, sizeof(probe)) && !memcmp(probe, kPrologue, kSteal) &&
              !memcmp(probe + tailAt, kTail, sizeof(kTail));
    if (!ok) {
        // Otra versión del ejecutable: buscar el patrón en .text
        auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        target = nullptr;
        for (int i = 0; i < nt->FileHeader.NumberOfSections && !target; ++i) {
            if (memcmp(sec[i].Name, ".text", 5)) continue;
            uint8_t* p = base + sec[i].VirtualAddress;
            uint8_t* e = p + sec[i].Misc.VirtualSize - 64;
            for (; p < e; ++p)
                if (!memcmp(p, kPrologue, kSteal) && !memcmp(p + tailAt, kTail, sizeof(kTail))) {
                    // ProcessEvent es el único que además comprueba FunctionFlags & 0x402
                    uint8_t* q = p;
                    for (; q < p + 0x60; ++q)
                        if (!memcmp(q, kChk, sizeof(kChk))) break;
                    if (q < p + 0x60) { target = p; break; }
                }
        }
    }
    if (!target) {
        LOG("ERROR: no se encontro UObject::ProcessEvent. Version del juego no soportada.");
        // Aviso en otro hilo (no se debe mostrar UI dentro de DllMain)
        CloseHandle(CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
            MessageBoxW(nullptr,
                        L"Outlast Online no es compatible con este ejecutable de Outlast.\n"
                        L"Usa la version oficial (Steam/GOG) actualizada. El juego se abrira sin el mod.",
                        L"Outlast Online", MB_OK | MB_ICONWARNING);
            return 0;
        }, nullptr, 0, nullptr));
        return;
    }
    G.processEvent = (ProcessEventFn)InstallDetour(target, (void*)&HookProcessEvent, kSteal, kPrologue);
    LOG("Hook ProcessEvent en %p -> %s", target, G.processEvent ? "OK" : "FALLO");
}

void Shutdown() {
    g_voice.Stop();
    g_net.Stop();
}

}  // namespace mod
