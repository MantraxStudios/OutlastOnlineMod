#pragma once
#include <windows.h>

namespace mod {
// Llamado desde DllMain (proceso recién cargado): lee config e instala el hook.
void Startup(HMODULE self);
void Shutdown();
}
