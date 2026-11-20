#pragma once

// Atrium's tray-host code logs through this small namespace; SuperMenu forwards
// those lines to the same append-only log as the rest of the bar.
namespace atrium {

void Log(const wchar_t* format, ...);

}  // namespace atrium
