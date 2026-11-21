// Live probe for the production tray model.
//
// Read-only by default: builds atrium::TrayModel, asks for a refresh, waits for
// the worker, prints the reconciled list (identity, source, promoted state,
// record key, artwork) and exits. It never clicks, opens Explorer's overflow or
// sends input unless `--click` is given explicitly.
//
// Usage (run from a console, no other SuperMenu instance required):
//   tray_probe.exe                     list the tray as SuperMenu sees it
//   tray_probe.exe --host              also run the Shell_NotifyIcon host
//   tray_probe.exe --click <exe-sub> left|right|double|middle [--host]
//   tray_probe.exe --watch 3000        keep pumping messages for N ms
//
// Exit codes: 0 = model loaded and (with --click) the target icon was found;
// 1 = model did not load / target not found.
#include "atrium/log.h"
#include "atrium/shell/tray_model.h"
#include "log.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr UINT kProbeEvent = WM_APP + 7;

struct Probe {
    HWND window = nullptr;
    std::vector<UINT> events;
    bool stop = false;
};

Probe g_probe;

std::vector<std::wstring> Arguments()
{
    std::vector<std::wstring> out;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!argv)
        return out;
    for (int i = 0; i < count; ++i)
        out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

std::wstring Lower(std::wstring text)
{
    CharLowerBuffW(text.data(), DWORD(text.size()));
    return text;
}

std::wstring FirstLine(const std::wstring& text)
{
    return text.substr(0, text.find(L'\n'));
}

void Pump(int milliseconds)
{
    const ULONGLONG until = GetTickCount64() + ULONGLONG(milliseconds);
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == kProbeEvent)
                g_probe.events.push_back(UINT(message.wParam));
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(20);
    } while (GetTickCount64() < until);
}

void PrintIcon(size_t index, const atrium::TrayIcon& icon)
{
    std::wprintf(L"[%2zu] promoted=%d order=%4zu index=%2d record=%-22ls identity=%-38ls source=%-12ls image=%dx%d%s%s\n", index,
                 icon.promoted, icon.order, icon.index, icon.recordKey.empty() ? L"-" : icon.recordKey.c_str(), icon.recordId.c_str(),
                 !icon.hostKey.empty() ? L"host" : icon.runtimeId.empty() ? L"record" : L"uia", icon.image ? icon.image->width : 0,
                 icon.image ? icon.image->height : 0, icon.appTooltip ? L" app-tooltip" : L"", icon.flashing ? L" flashing" : L"");
    std::wprintf(L"      tip='%ls'\n      exe='%ls' match=%ls shell=%lu\n", FirstLine(icon.tip).c_str(),
                 icon.exe.empty() ? L"-" : icon.exe.c_str(), icon.matchMethod.empty() ? L"-" : icon.matchMethod.c_str(),
                 icon.shell.pid);
}

const wchar_t* EventName(UINT code)
{
    switch (code) {
    case 0: return L"list-changed";
    case 1: return L"observed";
    case 2: return L"system-tray-opened";
    case 3: return L"needs-user";
    case 4: return L"accepted-unconfirmed";
    default: return L"other";
    }
}

} // namespace

int main()
{
    SetConsoleOutputCP(CP_UTF8);
    const auto args = Arguments();
    bool useHost = false, click = false;
    std::wstring target, action = L"left";
    int watchMs = 8000;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == L"--host")
            useHost = true;
        else if (args[i] == L"--click" && i + 1 < args.size()) {
            click = true;
            target = Lower(args[++i]);
            if (i + 1 < args.size() && args[i + 1].rfind(L"--", 0) != 0)
                action = Lower(args[++i]);
        } else if (args[i] == L"--watch" && i + 1 < args.size()) {
            watchMs = _wtoi(args[++i].c_str());
        }
    }

    sm::Log(L"tray-probe: start host=%d click=%d target='%ls' action='%ls'", useHost ? 1 : 0, click ? 1 : 0, target.c_str(), action.c_str());
    std::wprintf(L"== tray model probe (host=%d) ==\n", useHost ? 1 : 0);
    // A message-only window receives the model's change/click events.
    g_probe.window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!g_probe.window) {
        std::wprintf(L"could not create the probe window (error %lu)\n", GetLastError());
        return 1;
    }
    {
        atrium::TrayModel model(g_probe.window, kProbeEvent);
        if (useHost)
            model.SetHosting(true);
        model.Refresh();
        const ULONGLONG until = GetTickCount64() + ULONGLONG(watchMs);
        while (!model.Loaded() && GetTickCount64() < until)
            Pump(50);
        model.Refresh();
        Pump(300);

        const auto icons = model.Icons();
        std::wprintf(L"loaded=%d available=%d hosting=%d yielded=%d icons=%zu\n", model.Loaded() ? 1 : 0, model.Available() ? 1 : 0,
                     model.Hosting() ? 1 : 0, model.Yielded() ? 1 : 0, icons.size());
        for (size_t i = 0; i < icons.size(); ++i)
            PrintIcon(i, icons[i]);

        if (click) {
            size_t found = SIZE_MAX;
            for (size_t i = 0; i < icons.size() && found == SIZE_MAX; ++i)
                if (!icons[i].exe.empty() && Lower(icons[i].exe).find(target) != std::wstring::npos)
                    found = i;
            if (found == SIZE_MAX) {
                std::wprintf(L"target '%ls' not found; nothing clicked\n", target.c_str());
                sm::Log(L"tray-probe: target '%ls' not found", target.c_str());
                return 1;
            }
            const atrium::TrayIcon icon = icons[found];
            std::wprintf(L"clicking [%zu] '%ls' (%ls) %ls...\n", found, FirstLine(icon.tip).c_str(), icon.exe.c_str(), action.c_str());
            sm::Log(L"tray-probe: click %ls on '%ls' (%ls)", action.c_str(), FirstLine(icon.tip).c_str(), icon.exe.c_str());
            g_probe.events.clear();
            if (action == L"right")
                model.Click(icon, true);
            else if (action == L"double")
                model.DoubleClick(icon, false);
            else if (action == L"middle")
                model.MiddleClick(icon);
            else
                model.Click(icon, false);
            // The worker reports the outcome on the same message; give it time.
            Pump(2500);
            std::wprintf(L"events:");
            for (UINT code : g_probe.events)
                std::wprintf(L" %ls(%u)", EventName(code), code);
            std::wprintf(L"\n");
            sm::Log(L"tray-probe: click done; see docs/TRAY.md for the outcome lines above");
            return 0;
        }
        if (!model.Loaded()) {
            std::wprintf(L"model did not load in %d ms\n", watchMs);
            sm::Log(L"tray-probe: model did not load in %d ms", watchMs);
            return 1;
        }
        return icons.empty() ? 1 : 0;
    }
}
