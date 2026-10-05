#pragma once
#include <cstddef>

// Detour x64 mínimo: sobrescribe el inicio de `target` con un salto absoluto a
// `detour` y devuelve un trampolín que ejecuta los `stolen` bytes originales y
// continúa en target+stolen. `stolen` debe abarcar instrucciones completas sin
// direccionamiento relativo a RIP (se verifica con `expected`).
void* InstallDetour(void* target, void* detour, size_t stolen, const unsigned char* expected);
