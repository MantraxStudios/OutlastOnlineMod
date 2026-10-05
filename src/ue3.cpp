#include "ue3.h"
#include "log.h"
#include <windows.h>
#include <cstring>

namespace ue {

Engine G;

namespace off {
int SuperField = -1, Children = -1, ElementSize = -1, PropOffset = -1, BoolBitMask = -1, StructRef = -1,
    ObjPropClass = -1;
}

// Lectura protegida (los punteros candidatos durante el escaneo pueden ser basura).
bool SafeRead(const void* p, void* out, size_t n) {
    __try {
        memcpy(out, p, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T> static bool Rd(const void* p, T& out) { return SafeRead(p, &out, sizeof(T)); }

static bool ReadNameEntry(uint8_t* entry, int strOff, char* out, size_t cap) {
    uint8_t raw[256];
    if (!SafeRead(entry + strOff, raw, sizeof(raw))) return false;
    bool wide = raw[1] == 0 && raw[2] != 0;
    size_t i = 0;
    for (; i + 1 < cap; ++i) {
        size_t k = wide ? i * 2 : i;
        if (k >= sizeof(raw)) break;
        char c = (char)raw[k];
        if (!c) break;
        out[i] = c;
    }
    out[i] = 0;
    return true;
}

// ---------------------------------------------------------------------------
// Localización de GNames / GObjects
// ---------------------------------------------------------------------------
bool Engine::FindGlobals() {
    base = (uintptr_t)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t dataBeg = 0, dataEnd = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (!memcmp(sec[i].Name, ".data", 6)) {
            dataBeg = base + sec[i].VirtualAddress;
            dataEnd = dataBeg + sec[i].Misc.VirtualSize;
        }
    }
    if (!dataBeg) return false;

    for (uintptr_t a = dataBeg; a + 16 <= dataEnd && (!names_ || !objects_); a += 8) {
        auto arr = (TArray<uint8_t*>*)a;
        if (arr->Num < 1000 || arr->Max < arr->Num || arr->Max > 20000000 || !arr->Data) continue;
        uint8_t* e[2];
        if (!SafeRead(arr->Data, e, sizeof(e))) continue;

        if (!names_ && e[0] && e[1]) {
            for (int so = 0x08; so <= 0x20; so += 4) {
                char n0[16], n1[16];
                if (ReadNameEntry(e[0], so, n0, 16) && ReadNameEntry(e[1], so, n1, 16) && !strcmp(n0, "None") &&
                    !strcmp(n1, "ByteProperty")) {
                    names_ = arr;
                    nameStrOff_ = so;
                    LOG("GNames en RVA 0x%llX (Num=%d, strOff=0x%X)", (unsigned long long)(a - base), arr->Num, so);
                    break;
                }
            }
            if (names_ == arr) continue;
        }
        if (!objects_) {
            int hits = 0;
            for (int i = 0; i < 64; ++i) {
                uint8_t* o;
                int idx;
                if (!Rd(arr->Data + i, o) || !o) continue;
                if (Rd(o + off::Index, idx) && idx == i) ++hits;
            }
            if (hits >= 48) {
                objects_ = (TArray<UObject*>*)arr;
                LOG("GObjects en RVA 0x%llX (Num=%d)", (unsigned long long)(a - base), arr->Num);
            }
        }
    }
    return names_ && objects_;
}

// ---------------------------------------------------------------------------
// Descubrimiento de offsets de UStruct/UProperty
// ---------------------------------------------------------------------------
bool Engine::DiscoverOffsets() {
    UClass* clsObject = FindClass("Core.Object");
    UClass* clsActor = FindClass("Engine.Actor");
    if (!clsObject || !clsActor) {
        LOG("No se encontraron Core.Object / Engine.Actor");
        return false;
    }
    const int lo = off::FieldNext, hi = off::FieldNext + 0x60, P = off::Ptr;
    for (int o = lo; o <= hi; o += P)
        if (At<UObject*>(clsActor, o) == clsObject) { off::SuperField = o; break; }
    for (int o = lo; o <= hi; o += P) {
        if (o == off::SuperField) continue;
        UObject* c = At<UObject*>(clsActor, o);
        if (c && IsValid(c) && At<UObject*>(c, off::Outer) == clsActor) { off::Children = o; break; }
    }
    LOG("UStruct: SuperField=0x%X Children=0x%X", off::SuperField, off::Children);
    if (off::SuperField < 0 || off::Children < 0) return false;

    auto child = [&](UStruct* st, const char* n) -> UProperty* {
        int ni = NameIndex(n);
        for (UObject* c = At<UObject*>(st, off::Children); c; c = At<UObject*>(c, off::FieldNext))
            if (At<FName>(c, off::Name).Index == ni) return c;
        return nullptr;
    };
    UProperty* pName = child(clsObject, "Name");
    UProperty* pOuter = child(clsObject, "Outer");
    UProperty* pClass = child(clsObject, "Class");
    UProperty* pIdx = child(clsObject, "ObjectInternalInteger");
    if (!pName || !pOuter || !pClass || !pIdx) {
        LOG("No se encontraron propiedades base de Object");
        return false;
    }
    for (int o = off::FieldNext; o <= off::FieldNext + 0x50; o += 4) {
        if (off::PropOffset < 0 && At<int>(pName, o) == off::Name && At<int>(pOuter, o) == off::Outer &&
            At<int>(pClass, o) == off::Class && At<int>(pIdx, o) == off::Index)
            off::PropOffset = o;
        if (off::ElementSize < 0 && At<int>(pName, o) == 8 && At<int>(pOuter, o) == off::Ptr && At<int>(pIdx, o) == 4)
            off::ElementSize = o;
    }
    LOG("UProperty: Offset=0x%X ElementSize=0x%X", off::PropOffset, off::ElementSize);
    if (off::PropOffset < 0 || off::ElementSize < 0) return false;

    UProperty* pLoc = FindProperty(clsActor, "Location");
    UProperty* pOwner = FindProperty(clsActor, "Owner");
    UProperty* bHidden = FindProperty(clsActor, "bHidden");
    UProperty* bStatic = FindProperty(clsActor, "bStatic");
    UObject* vecStruct = FindObject("ScriptStruct", "Core.Object.Vector");
    for (int o = off::PropOffset + 4; o <= off::PropOffset + 0x48; o += 4) {
        if (off::StructRef < 0 && pLoc && (o % off::Ptr) == 0 && At<UObject*>(pLoc, o) == vecStruct) off::StructRef = o;
        if (off::ObjPropClass < 0 && pOwner && (o % off::Ptr) == 0 && At<UObject*>(pOwner, o) == clsActor)
            off::ObjPropClass = o;
        if (off::BoolBitMask < 0 && bHidden && bStatic) {
            uint32_t a = At<uint32_t>(bHidden, o), b = At<uint32_t>(bStatic, o);
            if (a && b && a != b && !(a & (a - 1)) && !(b & (b - 1))) off::BoolBitMask = o;
        }
    }
    LOG("StructRef=0x%X ObjPropClass=0x%X BoolBitMask=0x%X", off::StructRef, off::ObjPropClass, off::BoolBitMask);
    return off::BoolBitMask >= 0;
}

bool Engine::Init() {
    if (ready_) return true;
    if (!names_ || !objects_) {
        if (!FindGlobals()) return false;
    }
    if (!DiscoverOffsets()) return false;
    ready_ = true;
    return true;
}

// ---------------------------------------------------------------------------
std::string Engine::NameStr(int idx) const {
    if (!names_ || idx < 0 || idx >= names_->Num) return "<?>";
    uint8_t* e = names_->Data[idx];
    if (!e) return "<null>";
    char buf[128];
    if (!ReadNameEntry(e, nameStrOff_, buf, sizeof(buf))) return "<bad>";
    return buf;
}

std::string Engine::NameStr(FName n) const {
    std::string s = NameStr(n.Index);
    if (n.Number > 0) s += "_" + std::to_string(n.Number - 1);
    return s;
}

int Engine::NameIndex(const char* s) {
    auto it = nameCache_.find(s);
    if (it != nameCache_.end() && it->second >= 0) return it->second;
    int found = -1;
    char buf[128];
    for (int i = 0; i < names_->Num; ++i) {
        uint8_t* e = names_->Data[i];
        if (!e || !ReadNameEntry(e, nameStrOff_, buf, sizeof(buf))) continue;
        if (!_stricmp(buf, s)) { found = i; break; }
    }
    if (found >= 0) nameCache_[s] = found;  // los no encontrados se reintentan (pueden cargarse más tarde)
    return found;
}

int Engine::ObjCount() const { return objects_ ? objects_->Num : 0; }
UObject* Engine::Obj(int i) const { return (objects_ && i >= 0 && i < objects_->Num) ? objects_->Data[i] : nullptr; }

std::string Engine::GetName(UObject* o) const { return o ? NameStr(At<FName>(o, off::Name)) : "None"; }

std::string Engine::GetPath(UObject* o) const {
    std::string s = GetName(o);
    for (UObject* p = At<UObject*>(o, off::Outer); p; p = At<UObject*>(p, off::Outer)) s = GetName(p) + "." + s;
    return s;
}

std::string Engine::ClassName(UObject* o) const { return o ? GetName(At<UObject*>(o, off::Class)) : "None"; }

bool Engine::IsValid(UObject* o) const {
    if (!o) return false;
    int idx;
    if (!Rd(o + off::Index, idx)) return false;
    if (Obj(idx) != o) return false;
    return (At<uint64_t>(o, off::ObjectFlags) & RF_PendingKill) == 0;
}

bool Engine::IsA(UObject* o, UClass* cls) const {
    if (!o || !cls) return false;
    for (UStruct* c = At<UObject*>(o, off::Class); c; c = At<UObject*>(c, off::SuperField))
        if (c == cls) return true;
    return false;
}

UObject* Engine::FindObject(const char* className, const char* path) {
    std::string key = std::string(className) + " " + path;
    auto it = objCache_.find(key);
    if (it != objCache_.end() && IsValid(it->second)) return it->second;

    const char* dot = strrchr(path, '.');
    int leaf = NameIndex(dot ? dot + 1 : path);
    if (leaf < 0) return nullptr;
    for (int i = 0, n = ObjCount(); i < n; ++i) {
        UObject* o = Obj(i);
        if (!o || At<FName>(o, off::Name).Index != leaf) continue;
        if (ClassName(o) != className) continue;
        if (_stricmp(GetPath(o).c_str(), path) != 0) continue;
        objCache_[key] = o;
        return o;
    }
    return nullptr;
}

UObject* Engine::FindInstance(UClass* cls, bool (*pred)(UObject*)) {
    for (int i = 0, n = ObjCount(); i < n; ++i) {
        UObject* o = Obj(i);
        if (!o || (At<uint64_t>(o, off::ObjectFlags) & (RF_ClassDefaultObject | RF_PendingKill))) continue;
        if (IsA(o, cls) && (!pred || pred(o))) return o;
    }
    return nullptr;
}

UProperty* Engine::FindProperty(UStruct* st, const char* name) {
    int ni = NameIndex(name);
    if (ni < 0) return nullptr;
    for (; st; st = At<UObject*>(st, off::SuperField)) {
        for (UObject* c = At<UObject*>(st, off::Children); c; c = At<UObject*>(c, off::FieldNext)) {
            if (At<FName>(c, off::Name).Index != ni) continue;
            std::string cn = ClassName(c);
            if (cn.size() > 8 && cn.compare(cn.size() - 8, 8, "Property") == 0) return c;
        }
    }
    return nullptr;
}

int Engine::PropOff(UStruct* st, const char* name) {
    char key[160];
    snprintf(key, sizeof(key), "%p.%s", (void*)st, name);
    auto it = offCache_.find(key);
    if (it != offCache_.end()) return it->second;
    UProperty* p = FindProperty(st, name);
    int o = p ? At<int>(p, off::PropOffset) : -1;
    if (!p) LOG("Propiedad no encontrada: %s.%s", GetName(st).c_str(), name);
    offCache_[key] = o;
    return o;
}

uint32_t Engine::BoolMask(UStruct* st, const char* name) {
    UProperty* p = FindProperty(st, name);
    return p ? At<uint32_t>(p, off::BoolBitMask) : 0;
}

UFunction* Engine::FindFunction(UStruct* cls, const char* name) {
    int ni = NameIndex(name);
    if (ni < 0) return nullptr;
    for (UStruct* st = cls; st; st = At<UObject*>(st, off::SuperField))
        for (UObject* c = At<UObject*>(st, off::Children); c; c = At<UObject*>(c, off::FieldNext))
            if (At<FName>(c, off::Name).Index == ni && ClassName(c) == "Function") return c;
    LOG("Funcion no encontrada: %s.%s", GetName(cls).c_str(), name);
    return nullptr;
}

void Engine::Call(UObject* self, UFunction* fn, void* parms) {
    if (!self || !fn || !processEvent) return;
    // ProcessEvent rechaza funciones con índice nativo != 0; lo anulamos durante la llamada.
    uint16_t saved = At<uint16_t>(fn, off::iNative);
    At<uint16_t>(fn, off::iNative) = 0;
    processEvent(self, fn, parms, nullptr);
    At<uint16_t>(fn, off::iNative) = saved;
}

// ---------------------------------------------------------------------------
Params::Params(UFunction* fn) : fn_(fn) {
    int sz = fn ? At<int>(fn, off::PropertiesSize) : 0;
    buf_.assign((size_t)sz + 64, 0);
}

Params& Params::SetBool(const char* name, bool v) {
    int o = G.PropOff(fn_, name);
    uint32_t m = G.BoolMask(fn_, name);
    if (o >= 0 && m) {
        uint32_t& w = *reinterpret_cast<uint32_t*>(buf_.data() + o);
        w = v ? (w | m) : (w & ~m);
    }
    return *this;
}

}  // namespace ue
