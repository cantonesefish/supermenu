#include "log.h"

#include "../log.h"

#include <cstdarg>
#include <cstdio>

namespace atrium {

void Log(const wchar_t* format, ...) {
    wchar_t buffer[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf(buffer, std::size(buffer) - 1, format, args);
    va_end(args);
    sm::Log(L"tray-host: %ls", buffer);
}

}  // namespace atrium
