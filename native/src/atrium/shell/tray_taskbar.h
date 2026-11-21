// Revealing the system taskbar for one physical click on a tray button.
//
// The read-back fallback (docs/TRAY.md) may have to send a real mouse click to a
// tray icon when the shell accepted a UIA request but showed nothing. The taskbar
// or its overflow may be auto-hidden at that moment; the button's element
// rectangle is still reported, so the taskbar is shown for the moment of the
// click and put back afterwards. An originally visible taskbar is never hidden.
#pragma once

#include <windows.h>

#include <chrono>
#include <thread>
#include <utility>
#include <vector>

namespace atrium::tray {

// Explorer's taskbar windows: the desktop shell's Shell_TrayWnd (never another
// program's tray host) and the secondary monitors' Shell_SecondaryTrayWnd.
inline std::vector<HWND> ShellTrayWindows()
{
    std::vector<HWND> out;
    DWORD shell = 0;
    if (HWND desktop = GetShellWindow())
        GetWindowThreadProcessId(desktop, &shell);
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"Shell_TrayWnd", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == shell) {
            out.push_back(w);
            break;
        }
    }
    w = nullptr;
    while ((w = FindWindowExW(nullptr, w, L"Shell_SecondaryTrayWnd", nullptr)) != nullptr) {
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        if (pid == shell)
            out.push_back(w);
    }
    return out;
}

// RAII: show the shell's hidden taskbars for the duration of one click and hide
// again only the ones this object showed.
class TaskbarReveal {
public:
    TaskbarReveal()
    {
        for (HWND window : ShellTrayWindows()) {
            const bool visible = IsWindowVisible(window) != FALSE;
            if (!visible) {
                ShowWindowAsync(window, SW_SHOWNA);
                shown_.push_back(window);
            }
        }
        if (shown_.empty()) {
            active_ = true; // the taskbar was already on screen: usable as it is
            return;
        }
        // The shell does not show synchronously; give it a moment. A hidden
        // taskbar that stays hidden makes the physical fallback fail closed.
        for (int i = 0; i < 20; ++i) {
            bool all = true;
            for (HWND window : shown_)
                all = all && IsWindowVisible(window) != FALSE;
            if (all) {
                active_ = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        active_ = false;
    }
    ~TaskbarReveal()
    {
        for (HWND window : shown_)
            if (IsWindow(window))
                ShowWindowAsync(window, SW_HIDE);
        shown_.clear();
    }
    TaskbarReveal(const TaskbarReveal&) = delete;
    TaskbarReveal& operator=(const TaskbarReveal&) = delete;

    bool active() const { return active_; }

private:
    std::vector<HWND> shown_;
    bool active_ = false;
};

} // namespace atrium::tray
