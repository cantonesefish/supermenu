#include "log.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>

namespace sm {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_tried = false;

void EnsureFile() {
    if (g_tried) return;
    g_tried = true;
    wchar_t* appdata = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &appdata)) && appdata) {
        path = appdata;
        CoTaskMemFree(appdata);
        path += L"\\SuperMenu";
        CreateDirectoryW(path.c_str(), nullptr);
        CreateDirectoryW((path + L"\\logs").c_str(), nullptr);
        path += L"\\logs\\supermenu.log";
    } else {
        wchar_t exe[MAX_PATH * 2]{};
        GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
        path = exe;
        path += L".log";
    }
    g_file = _wfopen(path.c_str(), L"a, ccs=UTF-8");
}

}  // namespace

void LogLine(const std::wstring& text) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t stamp[64]{};
    swprintf(stamp, std::size(stamp), L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds);
    std::lock_guard lock(g_mutex);
    EnsureFile();
    if (g_file) {
        fputws(stamp, g_file);
        fputws(text.c_str(), g_file);
        fputws(L"\n", g_file);
        fflush(g_file);
    }
    OutputDebugStringW(stamp);
    OutputDebugStringW(text.c_str());
    OutputDebugStringW(L"\n");
}

void Log(const wchar_t* fmt, ...) {
    wchar_t buf[2048]{};
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, std::size(buf) - 1, fmt, args);
    va_end(args);
    LogLine(buf);
}

void LogHr(const wchar_t* what, HRESULT hr) {
    wchar_t buf[512]{};
    swprintf(buf, std::size(buf), L"%ls failed: 0x%08X", what ? what : L"(null)", static_cast<unsigned>(hr));
    LogLine(buf);
}

void LogFlush() {
    std::lock_guard lock(g_mutex);
    if (g_file) fflush(g_file);
}

}  // namespace sm
