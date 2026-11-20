#include "app.h"
#include "log.h"

#include <objbase.h>
#include <shellapi.h>

#include <string>

namespace {

bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) return true;
    }
    return false;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool wantSettings = argv && HasArg(argc, argv, L"--settings");
    const bool wantQuit = argv && HasArg(argc, argv, L"--quit");
    DWORD restartAfterPid = 0;
    if (argv) {
        for (int i = 1; i + 1 < argc; ++i) {
            if (_wcsicmp(argv[i], L"--restart-after") == 0) {
                restartAfterPid = static_cast<DWORD>(wcstoul(argv[i + 1], nullptr, 10));
                break;
            }
        }
        LocalFree(argv);
    }
    if (restartAfterPid != 0) {
        // Give the old process time to unregister its AppBar and release the
        // singleton mutex before the replacement creates its own.
        HANDLE oldProcess = OpenProcess(SYNCHRONIZE, FALSE, restartAfterPid);
        if (oldProcess) {
            WaitForSingleObject(oldProcess, 15000);
            CloseHandle(oldProcess);
        }
    }

    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\SuperMenu.Bar.Singleton");
    const bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;

    if (alreadyRunning) {
        HWND existing = FindWindowW(L"SuperMenuBarWindow", nullptr);
        if (existing) {
            PostMessageW(existing, wantQuit ? WM_APP + 89 : WM_APP + 88, 0, 0);
        }
        if (mutex) CloseHandle(mutex);
        return 0;
    }
    if (wantQuit) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        sm::Log(L"main: CoInitializeEx failed 0x%08X", static_cast<unsigned>(hr));
    }
    using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
    auto setCtx = reinterpret_cast<SetCtxFn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (!setCtx || !setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) SetProcessDPIAware();
    sm::Log(L"main: SuperMenu 1.0.0 starting");

    sm::App app;
    int rc = app.Run(hinst, wantSettings);

    CoUninitialize();
    if (mutex) CloseHandle(mutex);
    return rc;
}
