#include "hook.h"
#include "log.h"
#include <windows.h>
#include <cstring>
#include <cstdint>

#ifdef _WIN64
constexpr size_t kJumpSize = 14;
static void WriteJump(uint8_t* at, void* to) {
    // jmp qword ptr [rip+0] ; dq to
    at[0] = 0xFF;
    at[1] = 0x25;
    *(uint32_t*)(at + 2) = 0;
    *(uint64_t*)(at + 6) = (uint64_t)to;
}
#else
constexpr size_t kJumpSize = 5;
static void WriteJump(uint8_t* at, void* to) {
    // jmp rel32
    at[0] = 0xE9;
    *(int32_t*)(at + 1) = (int32_t)((uint8_t*)to - (at + 5));
}
#endif

void* InstallDetour(void* target, void* detour, size_t stolen, const unsigned char* expected) {
    auto t = (uint8_t*)target;
    if (stolen < kJumpSize) return nullptr;
    if (expected && memcmp(t, expected, stolen) != 0) {
        LOG("Detour: bytes inesperados en %p, no se instala", target);
        return nullptr;
    }
    auto tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, t, stolen);
    WriteJump(tramp + stolen, t + stolen);

    DWORD old;
    VirtualProtect(t, stolen, PAGE_EXECUTE_READWRITE, &old);
    WriteJump(t, detour);
    for (size_t i = kJumpSize; i < stolen; ++i) t[i] = 0x90;
    VirtualProtect(t, stolen, old, &old);
    FlushInstructionCache(GetCurrentProcess(), t, stolen);
    return tramp;
}
