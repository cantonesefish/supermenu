// Adapted from Atrium Desktop (MIT, https://github.com/wynter-yaxley/atrium-desktop),
// native/tests/tray_interaction.cpp: the physical right-click portion (shell/tray_pointer.h)
// only. Upstream also covers taskbar visibility in the same file, but that needs
// shell/taskbar_visibility.h, which is not part of this port. Standalone: every
// check passes -> exit code 0.
#include "shell/tray_pointer.h"

#include <array>
#include <cstdio>
#include <functional>

using namespace atrium::tray;

namespace {
int g_failures = 0;
int g_checks = 0;
void Check(bool ok, const char* message)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", message);
    }
}

// No calls to SendInput/SetCursorPos or real shell windows: the production
// interaction algorithm runs against a deterministic desktop and input queue.
struct Desktop {
    POINT cursor{30, 40}, target{1820, 1040}, clicked{};
    RECT screen{0, 0, 1920, 1080};
    bool interacting = false, targetAvailable = true, exactHit = true, sendWorks = true;
    int moves = 0, sends = 0;
    std::function<void(DWORD)> wait;
    std::function<void()> hit;
    std::function<void()> targetRead;
    bool Cursor(POINT& p)
    {
        p = cursor;
        return true;
    }
    bool Move(POINT p)
    {
        ++moves;
        cursor = p;
        return true;
    }
    void Wait(DWORD ms)
    {
        if (wait)
            wait(ms);
    }
    bool Interacting() { return interacting; }
    bool Target(POINT& p)
    {
        if (targetRead)
            targetRead();
        p = target;
        return targetAvailable;
    }
    bool TargetAt(POINT)
    {
        if (hit)
            hit();
        return exactHit;
    }
    bool Send(POINT p)
    {
        ++sends;
        std::array<INPUT, 3> input{};
        Check(RightClickInputs(p, screen, input), "a real click must have valid virtual desktop coordinates");
        // A pending physical move arrives just before the batch. The batch
        // must reposition first, then press/release, without interleaving.
        cursor = {200, 250};
        Check((input[0].mi.dwFlags & (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)) ==
                  (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK),
              "batch positions on virtual desktop");
        cursor.x = LONG(int64_t(input[0].mi.dx) * (screen.right - screen.left) / 65536) + screen.left;
        cursor.y = LONG(int64_t(input[0].mi.dy) * (screen.bottom - screen.top) / 65536) + screen.top;
        Check(input[1].mi.dwFlags == MOUSEEVENTF_RIGHTDOWN && input[2].mi.dwFlags == MOUSEEVENTF_RIGHTUP,
              "batch has balanced right button events after positioning");
        clicked = cursor;
        return sendWorks;
    }
};
} // namespace

int main()
{
    Desktop d;
    Check(RightClick(d) == PointerClick::Sent && d.sends == 1 && SamePoint(d.clicked, d.target),
          "stationary primary icon is clicked even if motion precedes the input batch");
    Check(SamePoint(d.cursor, {30, 40}), "stationary pointer restored after menu settling");

    d = {};
    d.wait = [&](DWORD ms) {
        if (ms == 60)
            d.cursor = {200, 250};
    };
    Check(RightClick(d) == PointerClick::Cancelled && d.sends == 0 && SamePoint(d.cursor, {200, 250}) && d.moves == 1,
          "user motion during delay cancels input without pulling cursor back");

    d = {};
    d.hit = [&] { d.cursor = {210, 260}; };
    Check(RightClick(d) == PointerClick::Cancelled && d.sends == 0 && SamePoint(d.cursor, {210, 260}), "user motion during a slow UIA hit test also cancels");
    d = {};
    d.exactHit = false;
    d.hit = [&] { d.cursor = {210, 260}; };
    Check(RightClick(d) == PointerClick::Cancelled && d.sends == 0, "motion during a failed hit test still cancels all fallback routes");
    d = {};
    d.targetRead = [&] { d.cursor = {230, 280}; };
    Check(RightClick(d) == PointerClick::Cancelled && d.moves == 0 && d.sends == 0, "motion during initial UIA bounds lookup is not overwritten");
    d = {};
    d.targetAvailable = false;
    d.targetRead = [&] { d.cursor = {230, 280}; };
    Check(RightClick(d) == PointerClick::Cancelled && d.sends == 0, "motion during failed bounds lookup cancels fallback as well");

    d = {};
    d.wait = [&](DWORD ms) {
        if (ms == 180)
            d.cursor = {220, 270};
    };
    Check(RightClick(d) == PointerClick::Sent && SamePoint(d.cursor, {220, 270}) && d.moves == 1,
          "motion after click is not overwritten by cursor restoration");

    d = {};
    d.interacting = true;
    Check(RightClick(d) == PointerClick::Cancelled && d.moves == 0 && d.sends == 0, "held mouse buttons or modifiers prevent a synthetic click");
    d = {};
    d.wait = [&](DWORD ms) {
        if (ms == 60)
            d.interacting = true;
    };
    Check(RightClick(d) == PointerClick::Cancelled && d.sends == 0 && d.moves == 1, "a button pressed during delay cancels without dragging cursor");

    d = {};
    d.exactHit = false;
    Check(RightClick(d) == PointerClick::Unavailable && d.sends == 0 && SamePoint(d.cursor, {30, 40}),
          "a taskbar sibling or covering window is not the selected button");
    d = {};
    d.wait = [&](DWORD ms) {
        if (ms == 60)
            ++d.target.x;
    };
    Check(RightClick(d) == PointerClick::Unavailable && d.sends == 0, "taskbar animation or icon reordering invalidates stale coordinates");
    d = {};
    d.targetAvailable = false;
    Check(RightClick(d) == PointerClick::Unavailable && d.moves == 0 && d.sends == 0, "unavailable or offscreen target never moves or clicks pointer");
    d = {};
    d.sendWorks = false;
    Check(RightClick(d) == PointerClick::Unavailable && SamePoint(d.cursor, {30, 40}), "blocked input restores stationary pointer and reports failure");

    d = {};
    d.target = {1790, 870};
    Check(RightClick(d) == PointerClick::Sent && SamePoint(d.clicked, d.target), "a verified overflow button above the primary taskbar is eligible");
    d = {};
    d.screen = {-2560, -1440, 1920, 1080};
    d.target = {-2200, -1000};
    Check(RightClick(d) == PointerClick::Sent && SamePoint(d.clicked, d.target), "negative monitor origins are positioned on the full virtual desktop");

    std::array<INPUT, 3> input{};
    Check(!RightClickInputs({1920, 20}, {0, 0, 1920, 1080}, input), "outside virtual desktop rejected");
    Check(!RightClickInputs({0, 0}, {0, 0, 0, 100}, input), "empty desktop rejected before division");
    Check(RightClickInputs({-1, -1}, {-1, -1, 0, 0}, input) && input[0].mi.dx == 32768 && input[0].mi.dy == 32768, "single pixel desktop uses its centre");
    for (POINT p : {POINT{-2560, -1440}, POINT{1919, 1079}, POINT{0, 0}}) {
        const RECT screen{-2560, -1440, 1920, 1080};
        Check(RightClickInputs(p, screen, input), "virtual desktop corners accepted");
        Check(LONG(int64_t(input[0].mi.dx) * 4480 / 65536) - 2560 == p.x && LONG(int64_t(input[0].mi.dy) * 2520 / 65536) - 1440 == p.y,
              "absolute coordinates round-trip to intended pixel");
    }

    if (!g_failures)
        std::printf("tray pointer: all interaction scenarios passed (%d checks)\n", g_checks);
    return g_failures ? 1 : 0;
}
