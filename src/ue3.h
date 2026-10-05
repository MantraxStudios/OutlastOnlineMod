#pragma once
// Capa mínima de reflexión para Unreal Engine 3 (build de Outlast, x64).
// No depende de un SDK generado: todo se resuelve por nombre en tiempo de
// ejecución recorriendo GObjects/GNames, y los offsets de UProperty/UStruct
// se descubren y verifican al iniciar.
#include <cstdint>
#include <string>
#include <vector>
#include <unordered_map>

namespace ue {

using UObject = uint8_t;   // tratamos todos los objetos como memoria cruda
using UClass = uint8_t;
using UStruct = uint8_t;
using UFunction = uint8_t;
using UProperty = uint8_t;

struct FName { int32_t Index; int32_t Number; };
struct FString { const wchar_t* Data; int32_t Num; int32_t Max; };
struct FVector { float X, Y, Z; };
struct FRotator { int32_t Pitch, Yaw, Roll; };
struct FColor { uint8_t B, G, R, A; };
template <class T> struct TArray { T* Data; int32_t Num; int32_t Max; };

// Offsets fijos del UObject de UE3 (verificados por desensamblado de UObject::ProcessEvent
// en Binaries/Win64/OLGame.exe y Binaries/Win32/OLGame.exe).
namespace off {
#ifdef _WIN64
constexpr int ObjectFlags = 0x10;  // uint64
constexpr int Index = 0x38;
constexpr int Outer = 0x40;
constexpr int Name = 0x48;
constexpr int Class = 0x50;
constexpr int FieldNext = 0x60;
constexpr int PropertiesSize = 0x88;  // UStruct
constexpr int FunctionFlags = 0xD0;  // UFunction
constexpr int iNative = 0xD4;        // UFunction (uint16)
#else
constexpr int ObjectFlags = 0x08;  // uint64
constexpr int Index = 0x20;
constexpr int Outer = 0x28;
constexpr int Name = 0x2C;
constexpr int Class = 0x34;
constexpr int FieldNext = 0x3C;
constexpr int PropertiesSize = 0x50;  // UStruct
constexpr int FunctionFlags = 0x84;  // UFunction
constexpr int iNative = 0x88;        // UFunction (uint16)
#endif
constexpr int Ptr = (int)sizeof(void*);
// Descubiertos en Init():
extern int SuperField, Children, ElementSize, PropOffset, BoolBitMask, StructRef, ObjPropClass;
}

constexpr uint64_t RF_ClassDefaultObject = 0x200ull;
constexpr uint64_t RF_PendingKill = 0x2000000000000000ull;

template <class T> inline T& At(const void* base, int offset) {
    return *reinterpret_cast<T*>((uint8_t*)base + offset);
}

#ifdef _WIN64
using ProcessEventFn = void(__fastcall*)(UObject* self, UFunction* fn, void* parms, void* result);
#else
using ProcessEventFn = void(__thiscall*)(UObject* self, UFunction* fn, void* parms, void* result);
#endif

class Engine {
public:
    bool Init();                // localizar GNames/GObjects y descubrir offsets
    bool Ready() const { return ready_; }

    // --- nombres ---
    std::string NameStr(FName n) const;
    std::string NameStr(int idx) const;
    int NameIndex(const char* s);  // -1 si no existe (cacheado)

    // --- objetos ---
    int ObjCount() const;
    UObject* Obj(int i) const;
    std::string GetName(UObject* o) const;
    std::string GetPath(UObject* o) const;   // Outer.Outer.Name
    std::string ClassName(UObject* o) const;
    bool IsValid(UObject* o) const;          // sigue en GObjects y no está pendiente de borrar
    bool IsA(UObject* o, UClass* cls) const;
    UObject* FindObject(const char* className, const char* path);
    UClass* FindClass(const char* path) { return FindObject("Class", path); }
    // primera instancia (no CDO) cuya clase deriva de cls
    UObject* FindInstance(UClass* cls, bool (*pred)(UObject*) = nullptr);

    // --- reflexión ---
    UProperty* FindProperty(UStruct* st, const char* name);
    int PropOff(UStruct* st, const char* name);   // offset del campo o -1 (cacheado)
    uint32_t BoolMask(UStruct* st, const char* name);
    UFunction* FindFunction(UStruct* cls, const char* name);

    // --- llamadas ---
    ProcessEventFn processEvent = nullptr;  // trampolín al ProcessEvent original
    void Call(UObject* self, UFunction* fn, void* parms);

    uintptr_t base = 0;
    void* gWorldPtr = nullptr;  // &GWorld

private:
    bool ready_ = false;
    TArray<uint8_t*>* names_ = nullptr;
    TArray<UObject*>* objects_ = nullptr;
    int nameStrOff_ = 0x10;
    std::unordered_map<std::string, int> nameCache_;
    std::unordered_map<std::string, UObject*> objCache_;
    std::unordered_map<std::string, int> offCache_;
    bool FindGlobals();
    bool DiscoverOffsets();
};

extern Engine G;

// Construye el buffer de parámetros de un UFunction rellenando campos por nombre.
class Params {
public:
    explicit Params(UFunction* fn);
    template <class T> Params& Set(const char* name, const T& v) {
        int o = G.PropOff(fn_, name);
        if (o >= 0) *reinterpret_cast<T*>(buf_.data() + o) = v;
        return *this;
    }
    Params& SetBool(const char* name, bool v);
    template <class T> T Get(const char* name) {
        int o = G.PropOff(fn_, name);
        return o >= 0 ? *reinterpret_cast<T*>(buf_.data() + o) : T{};
    }
    void* Data() { return buf_.data(); }
    bool Ok() const { return fn_ != nullptr; }
private:
    UFunction* fn_;
    std::vector<uint8_t> buf_;
};

bool SafeRead(const void* p, void* out, size_t n);

}  // namespace ue
