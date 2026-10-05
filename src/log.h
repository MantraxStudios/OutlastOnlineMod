#pragma once
#include <cstdio>
#include <cstdarg>
#include <windows.h>

// Logger muy simple: escribe en OutlastOnline.log junto a la DLL.
namespace logx {
void Init(const wchar_t* path);
void Write(const char* fmt, ...);
}

#define LOG(...) logx::Write(__VA_ARGS__)
