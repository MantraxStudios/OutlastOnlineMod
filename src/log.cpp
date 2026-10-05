#include "log.h"
#include <mutex>
#include <share.h>

namespace logx {
static FILE* g_file = nullptr;
static std::mutex g_mtx;

void Init(const wchar_t* path) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_file) g_file = _wfsopen(path, L"w", _SH_DENYNO);
}

void Write(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!g_file) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_file, fmt, ap);
    va_end(ap);
    fputc('\n', g_file);
    fflush(g_file);
}
}
