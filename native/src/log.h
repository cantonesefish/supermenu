#pragma once

#include <windows.h>
#include <string>

namespace sm {

void Log(const wchar_t* fmt, ...);
void LogHr(const wchar_t* what, HRESULT hr);
void LogLine(const std::wstring& text);
void LogFlush();

}  // namespace sm
