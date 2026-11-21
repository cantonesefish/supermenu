#include "app.h"

#include "atrium/shell/shell_taskbar.h"
#include "json.h"
#include "supermenu_version.h"
#include "tray_view.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <taskschd.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>

namespace sm {
namespace {

constexpr UINT kTrayCallback = WM_APP + 77;
constexpr UINT kTrayClosePopup = WM_APP + 90;
constexpr UINT kTrayKey = WM_APP + 92;
constexpr UINT kTimedMessage = WM_APP + 93;
constexpr UINT kSettingsMessage = WM_APP + 94;
// TrayModel's event message: wParam 0 = list changed, 1 = app response observed,
// 2 = system tray opened, 3 = needs the user, 4 = accepted/unconfirmed (no retry).
constexpr UINT kTrayModelEvent = WM_APP + 97;
constexpr UINT_PTR kTickTimer = 1;
constexpr int kTrayId = 1;

App* g_app = nullptr;
UINT g_taskbarCreated = 0;
UINT g_settingsChanged = 0;

bool IsTopMenu(MenuId id) {
    return id == MenuId::Apple || id == MenuId::AppMenu || id == MenuId::File || id == MenuId::Edit ||
           id == MenuId::View || id == MenuId::Window || id == MenuId::Help;
}

bool ContainsPoint(const RECT& r, POINT p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

void RunProcess(const wchar_t* exe, const wchar_t* args = nullptr) {
    ShellExecuteW(nullptr, L"open", exe, args, nullptr, SW_SHOWNORMAL);
}

std::wstring XmlEscape(std::wstring value) {
    auto replaceAll = [&](const wchar_t* from, const wchar_t* to) {
        for (size_t pos = 0; (pos = value.find(from, pos)) != std::wstring::npos;
             pos += wcslen(to)) {
            value.replace(pos, wcslen(from), to);
        }
    };
    replaceAll(L"&", L"&amp;");
    replaceAll(L"<", L"&lt;");
    replaceAll(L">", L"&gt;");
    replaceAll(L"\"", L"&quot;");
    return value;
}

std::wstring UserTaskId() {
    wchar_t user[256]{};
    wchar_t domain[256]{};
    DWORD userLength = static_cast<DWORD>(std::size(user));
    if (!GetUserNameW(user, &userLength)) return L"";
    if (!GetEnvironmentVariableW(L"USERDOMAIN", domain, static_cast<DWORD>(std::size(domain)))) {
        return user;
    }
    return std::wstring(domain) + L"\\" + user;
}

BSTR BstrAlloc(const std::wstring& text) {
    return SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
}

bool InstallLogonTask(const std::wstring& exe) {
    const std::wstring userId = UserTaskId();
    if (userId.empty()) return false;
    std::wstring xml =
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\n"
        L"<Task version=\"1.4\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\n"
        L" <RegistrationInfo><Author>SuperMenu</Author>"
        L"<Description>Start SuperMenu at user logon, earlier than Run-key startup apps.</Description>"
        L"</RegistrationInfo>\n"
        L" <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>" +
        XmlEscape(userId) + L"</UserId></LogonTrigger></Triggers>\n"
        L" <Principals><Principal id=\"Author\"><UserId>" +
        XmlEscape(userId) +
        L"</UserId><LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\n"
        L" <Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate>"
        L"<StartWhenAvailable>true</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled>"
        L"<Hidden>false</Hidden><RunOnlyIfIdle>false</RunOnlyIfIdle>"
        L"<WakeToRun>false</WakeToRun><ExecutionTimeLimit>PT0S</ExecutionTimeLimit>"
        L"<Priority>4</Priority><RestartOnFailure><Interval>PT1M</Interval>"
        L"<Count>2</Count></RestartOnFailure></Settings>\n"
        L" <Actions Context=\"Author\"><Exec><Command>" +
        XmlEscape(exe) + L"</Command></Exec></Actions>\n"
        L"</Task>";

    ComPtr<ITaskService> service;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(service.put()));
    if (FAILED(hr)) {
        LogHr(L"autostart: CoCreateInstance(CLSID_TaskScheduler)", hr);
        return false;
    }
    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) {
        LogHr(L"autostart: ITaskService::Connect", hr);
        return false;
    }
    ComPtr<ITaskFolder> root;
    BSTR rootPath = BstrAlloc(L"\\");
    hr = service->GetFolder(rootPath, root.put());
    SysFreeString(rootPath);
    if (FAILED(hr)) {
        LogHr(L"autostart: ITaskService::GetFolder", hr);
        return false;
    }
    ComPtr<ITaskDefinition> definition;
    hr = service->NewTask(0, definition.put());
    if (FAILED(hr)) {
        LogHr(L"autostart: ITaskService::NewTask", hr);
        return false;
    }
    BSTR xmlText = BstrAlloc(xml);
    hr = definition->put_XmlText(xmlText);
    SysFreeString(xmlText);
    if (FAILED(hr)) {
        LogHr(L"autostart: ITaskDefinition::put_XmlText", hr);
        return false;
    }
    ComPtr<IRegisteredTask> registered;
    BSTR taskName = BstrAlloc(L"SuperMenu");
    hr = root->RegisterTaskDefinition(taskName, definition.get(), TASK_CREATE_OR_UPDATE, empty, empty,
                                      TASK_LOGON_INTERACTIVE_TOKEN, empty, registered.put());
    SysFreeString(taskName);
    if (FAILED(hr)) {
        LogHr(L"autostart: RegisterTaskDefinition", hr);
        return false;
    }
    Log(L"autostart: logon task installed (%ls)", exe.c_str());
    return true;
}

void DeleteLogonTask() {
    ComPtr<ITaskService> service;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(service.put())))) {
        return;
    }
    VARIANT empty;
    VariantInit(&empty);
    if (FAILED(service->Connect(empty, empty, empty, empty))) return;
    ComPtr<ITaskFolder> root;
    BSTR rootPath = BstrAlloc(L"\\");
    if (FAILED(service->GetFolder(rootPath, root.put()))) {
        SysFreeString(rootPath);
        return;
    }
    SysFreeString(rootPath);
    BSTR taskName = BstrAlloc(L"SuperMenu");
    root->DeleteTask(taskName, 0);
    SysFreeString(taskName);
}

void SetRunValue(const std::wstring& exe, bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    if (enabled) {
        const std::wstring value = L"\"" + exe + L"\"";
        RegSetValueExW(key, L"SuperMenu", 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                       static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, L"SuperMenu");
    }
    RegCloseKey(key);
}

bool ShellReady() {
    return atrium::ShellTaskbar() != nullptr && GetShellWindow() != nullptr;
}

void WaitForShellReady() {
    if (ShellReady()) return;
    Log(L"startup: Explorer shell not ready; waiting");
    int elapsed = 0;
    for (int i = 0; i < 150 && !ShellReady(); ++i) {
        Sleep(100);
        elapsed += 100;
    }
    Log(L"startup: shell waited %d ms (ready=%d)", elapsed, ShellReady() ? 1 : 0);
}

}  // namespace

App::~App() { Shutdown(); }

bool App::Initialize() {
    EnsureSettingsFile();
    LoadSettings(settings_);
    settingsStamp_ = SettingsStamp();
    g_app = this;
    g_settingsChanged = RegisterWindowMessageW(L"SuperMenu.SettingsChanged");
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    const auto monitors = EnumerateMonitors();
    if (monitors.empty()) {
        Log(L"app: no monitors");
        return false;
    }
    const MonitorInfo* monitor = FindMonitor(monitors, settings_.monitor);
    if (!gpu_.Initialize(monitor->handle)) {
        MessageBoxW(nullptr, L"SuperMenu could not create a Direct3D 11 device for this display.",
                    L"SuperMenu", MB_ICONERROR | MB_OK);
        return false;
    }
    if (!capture_.Initialize(gpu_)) {
        Log(L"app: capture initialization failed");
        return false;
    }
    // GPU/capture initialization runs while Explorer is still starting, so a
    // logon-task launch is ready to publish the bar the instant the shell exists.
    WaitForShellReady();
    bar_ = std::make_unique<BarWindow>();
    if (!bar_->Create(hinst_, &gpu_, &capture_, this, gpu_.monitor())) {
        MessageBoxW(nullptr, L"SuperMenu could not create its menu bar window.", L"SuperMenu",
                    MB_ICONERROR | MB_OK);
        return false;
    }
    bar_->UpdateBackdrop(true);
    sys_.Refresh(true);
    brightness_ = 70;
    brightnessSupported_ = GetDisplayBrightness(brightness_);
    Log(L"app: brightness=%d%% supported=%d", brightness_, brightnessSupported_ ? 1 : 0);
    if (!brightnessSupported_) Log(L"app: display brightness control is not available (DDC/WMI)");
    RefreshStatusAndRender();
    bar_->Render(true);
    bar_->Show(true);
    bar_->Render(true);
    lastBarPointerMs_ = NowMs();
    CreateTray();
    // One tray model: its worker reads Explorer's notification area back (UI
    // Automation + NotifyIconSettings + Shell_NotifyIconGetRect) while its host
    // receives the apps' Shell_NotifyIcon calls on a hidden Shell_TrayWnd and
    // forwards them to Explorer. Both sources are merged by identity; the model
    // posts kTrayModelEvent here when the list or a click's outcome changes.
    // SuperMenu's own icon was registered by CreateTray() above.
    tray_ = std::make_unique<atrium::TrayModel>(trayHwnd_, kTrayModelEvent);
    tray_->SetHosting(true);
    tray_->Refresh();
    lastTrayRefreshMs_ = NowMs();
    if (bar_) tray_->SetHostPlaces(bar_->trayAreaScreenRect());
    Log(L"tray model: hosting=%d yielded=%d", tray_->Hosting() ? 1 : 0, tray_->Yielded() ? 1 : 0);
    ApplyLaunchAtLogin();
    media_.Start();
    StartHardwareWorker();
    initialized_ = true;
    Log(L"app: initialized");
    return true;
}

void App::Shutdown() {
    if (!initialized_ && !bar_) {
        g_app = nullptr;
        return;
    }
    media_.Stop();
    StopHardwareWorker();
    RemoveHooks();
    popup_.CloseNow();
    tray_.reset();  // stops the model's worker and its tray host
    DestroyTray();
    if (bar_) {
        bar_->Destroy();
        bar_.reset();
    }
    capture_.Shutdown();
    gpu_.Release();
    initialized_ = false;
    g_app = nullptr;
    Log(L"app: shutdown");
}

int App::Run(HINSTANCE hinst, bool openSettingsOnStart) {
    hinst_ = hinst;
    if (!Initialize()) return 1;
    if (openSettingsOnStart) {
        // Give the bar one composition tick before a companion window takes the
        // foreground, so it appears over a finished frame rather than a blank one.
        PostMessageW(bar_ ? bar_->hwnd() : nullptr, WM_APP + 88, 0, 0);
    }
    tickTimerId_ = SetTimer(nullptr, 0, 15, nullptr);
    MSG msg{};
    while (running_) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got <= 0) break;
        if (msg.message == WM_TIMER && msg.hwnd == nullptr) {
            Tick();
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (tickTimerId_) KillTimer(nullptr, tickTimerId_);
    Shutdown();
    return 0;
}

MenuContext App::menuContext() const {
    MenuContext ctx;
    ctx.settings = settings_;
    ctx.status = sys_.status();
    ctx.appName = TargetName();
    return ctx;
}

std::wstring App::TargetName() const {
    const ForegroundApp& app = sys_.status().app;
    if (app.desktop) return settings_.language == L"en-US" ? L"Finder" : L"访达";
    return app.name.empty() ? (settings_.language == L"en-US" ? L"SuperMenu" : L"超级菜单栏") : app.name;
}

HWND App::TargetWindow() const {
    if (targetHwnd_ && IsWindow(targetHwnd_)) return targetHwnd_;
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid != GetCurrentProcessId()) return fg;
    return sys_.LastExternalApp().hwnd;
}

void App::Tick() {
    const double now = NowMs();
    if (restartAtMs_ > 0 && now >= restartAtMs_) {
        RestartForDisplayChange();
        return;
    }
    sys_.Refresh();
    const auto& st = sys_.status();
    if (!iconStateSeen_) {
        iconStateSeen_ = true;
        lastNetworkKind_ = st.network;
        lastVolume_ = st.volume;
        lastMuted_ = st.volumeMuted;
        lastFocusOn_ = st.focusOn;
    } else {
        if (st.network != lastNetworkKind_) {
            lastNetworkKind_ = st.network;
            sys_.statusMutable().networkChangedMs = now;
        }
        if (st.volume != lastVolume_ || st.volumeMuted != lastMuted_) {
            lastVolume_ = st.volume;
            lastMuted_ = st.volumeMuted;
            sys_.statusMutable().volumeChangedMs = now;
        }
        if (st.focusOn != lastFocusOn_) {
            lastFocusOn_ = st.focusOn;
            if (popup_.IsVisible()) popup_.Render();
        }
    }
    // Keep one extra static frame after each transition so the animated glyph
    // always settles back to its steady state.
    const bool iconAnimating = st.batteryCharging || now - st.networkChangedMs < 1000.0 ||
                               now - st.volumeChangedMs < 500.0;
    if (iconAnimating && bar_) bar_->Invalidate();
    RefreshTray(false);
    if (media_.Poll() && popup_.IsVisible() && popup_.kind() == PopupKind::ControlCenter) popup_.Render();
    if (popup_.IsVisible() && popup_.kind() == PopupKind::ControlCenter &&
        (now - st.networkChangedMs < 1000.0 || now - st.volumeChangedMs < 500.0 ||
         popup_.wifiToggleAnimating())) {
        popup_.Render();
    }
    capture_.TickRestart(now);
    const bool newFrame = capture_.Poll();
    if (newFrame) {
        if (bar_) bar_->UpdateBackdrop(true);
        if (popup_.IsVisible()) popup_.UpdateBackdrop();
    }
    RefreshStatusAndRender();
    if (popup_.IsVisible()) {
        popup_.Tick(now);
        if (newFrame && popup_.IsVisible()) popup_.Render();
    }
    if (!popup_.IsVisible() && activeKind_ != PopupKind::None) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        RemoveHooks();
        if (bar_) {
            bar_->Invalidate();
            RenderBar();
        }
    }
    EnsureHiddenFullscreenState();
    UpdateDesktopMenuState();
    EnsureAutoHide(now);
    HandleSettingsReload();
    if (bar_ && (bar_->animating() || bar_->contentDirty() || newFrame)) RenderBar();
}

void App::RenderBar() {
    if (!bar_ || !bar_->hwnd()) return;
    if (!IsWindowVisible(bar_->hwnd())) return;
    bar_->Render(false);
}

void App::RefreshStatusAndRender() {
    if (!bar_) return;
    const SYSTEMTIME& t = sys_.status().now;
    wchar_t buf[512];
    swprintf(buf, std::size(buf), L"%04u-%02u-%02u %02u:%02u:%02u|%ls|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%ls|%d|%d", t.wYear, t.wMonth, t.wDay,
             t.wHour, t.wMinute, t.wSecond, sys_.status().app.name.c_str(), sys_.status().batteryPercent,
             sys_.status().batteryCharging ? 1 : 0, sys_.status().batteryOnAc ? 1 : 0,
             sys_.status().wifiSignal, sys_.status().wifiConnected ? 1 : 0,
             sys_.status().volume, sys_.status().volumeMuted ? 1 : 0, sys_.status().darkTheme ? 1 : 0,
             sys_.status().fullscreen ? 1 : 0, static_cast<int>(sys_.status().network),
             sys_.status().ethernetConnected ? 1 : 0, sys_.status().app.desktop ? 1 : 0,
             sys_.status().windowsUpdateRestartRequired ? 1 : 0,
             sys_.status().inputMethodAvailable ? 1 : 0,
             sys_.status().inputMethodNative ? 1 : 0,
             sys_.status().inputMethodLabel.c_str(),
             sys_.status().privacyMicrophone ? 1 : 0,
             sys_.status().privacyCamera ? 1 : 0);
    if (statusKey_ != buf) {
        statusKey_ = buf;
        bar_->Invalidate();
        RenderBar();
        if (popup_.IsVisible() && !popup_.closing()) popup_.Render();
    }
}

void App::ShowMenu(MenuId id, RECT anchor) {
    if (bar_) bar_->HideTrayTooltipNow();
    const MenuContext ctx = menuContext();
    const MenuModel model = BuildMenu(id, ctx, settings_.reserveWorkArea);
    activeKind_ = PopupKind::Menu;
    activeMenu_ = id;
    if (!popup_.Open(PopupKind::Menu, id, model, anchor, &gpu_, &capture_, this, bar_->ui(), bar_->icons(),
                     gpu_.monitor().rect)) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        return;
    }
    InstallHooks();
    bar_->Invalidate();
    RenderBar();
}

void App::ShowPanel(PopupKind kind, MenuId id, RECT anchor) {
    if (bar_) bar_->HideTrayTooltipNow();
    activeKind_ = kind;
    activeMenu_ = id;
    if (kind == PopupKind::ControlCenter) popup_.SetBrightness(brightness_);
    if (!popup_.OpenPanel(kind, anchor, &gpu_, &capture_, this, bar_->ui(), bar_->icons(), gpu_.monitor().rect)) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        return;
    }
    InstallHooks();
    bar_->Invalidate();
    RenderBar();
}

void App::ClosePopup() {
    if (!popup_.IsVisible()) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        RemoveHooks();
        return;
    }
    popup_.BeginClose();
    activeKind_ = PopupKind::None;
    activeMenu_ = MenuId::None;
    RemoveHooks();
    if (bar_) {
        bar_->Invalidate();
        RenderBar();
        // The tray area is the sensible place again once the popup is gone.
        if (tray_) tray_->SetHostPlaces(bar_->trayAreaScreenRect());
    }
}

void App::OnBarItem(MenuId id, RECT anchorScreen) {
    // Remember the application the menu commands should target. Our own
    // no-activate windows never become foreground, so this is the app behind.
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    if (fg && pid != GetCurrentProcessId()) targetHwnd_ = fg;
    else if (sys_.LastExternalApp().hwnd) targetHwnd_ = sys_.LastExternalApp().hwnd;

    const bool visible = popup_.IsVisible() && !popup_.closing();
    if (visible && activeKind_ == PopupKind::Menu && activeMenu_ == id) {
        ClosePopup();
        return;
    }
    if (visible && activeKind_ != PopupKind::Menu && activeMenu_ == id) {
        ClosePopup();
        return;
    }
    if (IsTopMenu(id)) {
        ShowMenu(id, anchorScreen);
        return;
    }
    switch (id) {
        case MenuId::Search:
            ClosePopup();
            SendWinKey('S');
            break;
        case MenuId::Clock:
            ShowPanel(PopupKind::Calendar, id, anchorScreen);
            break;
        case MenuId::Wifi:
            ShowPanel(PopupKind::Wifi, id, anchorScreen);
            break;
        case MenuId::Battery:
            ShowPanel(PopupKind::Battery, id, anchorScreen);
            break;
        case MenuId::Volume:
            ShowPanel(PopupKind::Volume, id, anchorScreen);
            break;
        case MenuId::ControlCenter:
            ShowPanel(PopupKind::ControlCenter, id, anchorScreen);
            break;
        case MenuId::Notifications:
            ShowPanel(PopupKind::Notifications, id, anchorScreen);
            break;
        case MenuId::WindowsUpdate:
            LaunchUri(L"ms-settings:windowsupdate");
            break;
        case MenuId::InputMethod:
            sys_.OpenInputMethodMenu();
            break;
        case MenuId::Privacy:
            if (sys_.status().privacyCamera) LaunchUri(L"ms-settings:privacy-webcam");
            else LaunchUri(L"ms-settings:privacy-microphone");
            break;
        default:
            break;
    }
}

void App::OnBarContextMenu(POINT screenPoint) {
    if (!bar_) return;
    bar_->HideTrayTooltipNow();
    activeKind_ = PopupKind::Menu;
    activeMenu_ = MenuId::Context;
    const MenuModel model = BuildBarContextMenu(menuContext());
    const RECT barScreen = bar_->windowScreenRect();
    const int barBottom = barScreen.top + static_cast<int>(bar_->barHeightPx());
    const RECT anchor{screenPoint.x, barBottom, screenPoint.x + 1, barBottom + 1};
    if (!popup_.Open(PopupKind::Menu, MenuId::Context, model, anchor, &gpu_, &capture_, this, bar_->ui(),
                     bar_->icons(), gpu_.monitor().rect)) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        return;
    }
    InstallHooks();
    bar_->Invalidate();
    RenderBar();
}

void App::OnBarHover(MenuId id) {
    if (!popup_.IsVisible() || popup_.closing()) return;
    if (activeKind_ == PopupKind::Menu && IsTopMenu(id) && id != activeMenu_) {
        ShowMenu(id, bar_->plateScreenRect(id));
    }
}

void App::OnBarEmptyClick() { ClosePopup(); }

MenuId App::openMenu() const {
    return popup_.IsVisible() && !popup_.closing() && activeKind_ == PopupKind::Menu ? activeMenu_ : MenuId::None;
}

bool App::popupVisible() const { return popup_.IsVisible() && !popup_.closing(); }

bool App::trayOverflowOpen() const {
    return popup_.IsVisible() && !popup_.closing() && popup_.kind() == PopupKind::Tray;
}

void App::RefreshTray(bool force) {
    // The model refreshes asynchronously; a changed list arrives later as a
    // kTrayModelEvent 0 message, so there is no changed result to inspect here.
    const double now = NowMs();
    if (!force && now - lastTrayRefreshMs_ < 2000.0) return;
    lastTrayRefreshMs_ = now;
    if (tray_) tray_->Refresh();
}

void App::WriteTrayLiveFile() {
    // The settings app reads this small snapshot to show only icons that are
    // currently live, instead of every historical NotifyIconSettings record.
    std::string text;
    for (const auto& icon : trayIconsCache_) {
        // Settings edits registry records by their numeric id. Run-only hosted
        // icons have no record key and cannot be overridden there.
        if (!TrayIconRecordNumber(icon)) continue;
        for (wchar_t ch : icon.recordKey) text.push_back(static_cast<char>(ch));
        text.push_back('\n');
    }
    const std::wstring path = SettingsDirectory() + L"\\tray-live.txt";
    const std::wstring tmp = path + L".tmp";
    HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void App::OnTrayIconActivated(const atrium::TrayIcon& icon) {
    if (!tray_) return;
    // The popup (and the hover it forwarded) goes away before the app is told.
    ClosePopup();
    const std::wstring identity = TrayIconIdentity(icon);
    const ULONGLONG now = GetTickCount64();
    const bool continuation = lastTrayClick_ && !identity.empty() &&
                              TrayIconIdentity(*lastTrayClick_) == identity &&
                              now - lastTrayClickTick_ <= GetDoubleClickTime();
    if (continuation) {
        // The first click was already delivered: this is the shell's second click
        // of a double click, not a new single click.
        lastTrayClick_.reset();
        lastTrayClickTick_ = 0;
        tray_->DoubleClick(icon, true);
        Log(L"tray: double-click continuation on %ls", TrayIconLabel(icon).c_str());
        return;
    }
    lastTrayClick_ = icon;
    lastTrayClickTick_ = now;
    tray_->Click(icon, false);
    Log(L"tray: left click on %ls", TrayIconLabel(icon).c_str());
}

void App::OnTrayIconContext(const atrium::TrayIcon& icon) {
    if (!tray_) return;
    ClosePopup();
    lastTrayClick_.reset();
    lastTrayClickTick_ = 0;
    tray_->Click(icon, true);
    Log(L"tray: right click on %ls", TrayIconLabel(icon).c_str());
}

void App::OnTrayIconHover(const atrium::TrayIcon* icon) {
    if (!tray_) return;
    // Native hover: the hosted app gets the shell's arrival/popup/close messages.
    tray_->Hover(icon);
}

void App::OnTrayModelEvent(WPARAM event) {
    if (!tray_) return;
    switch (event) {
        case 0: {  // the icon list changed
            trayIconsCache_ = tray_->Icons();
            // Before the model's first read completes the list is empty: keep the
            // previous tray-live snapshot rather than publishing an empty one.
            if (tray_->Loaded()) WriteTrayLiveFile();
            if (bar_) {
                bar_->Invalidate();
                RenderBar();
                // Apps asking Shell_NotifyIconGetRect get the freshly laid-out area,
                // unless the hidden-icon panel is open: that one keeps its own anchor
                // (set by OnTrayOverflow) while it is on screen.
                if (!popup_.IsVisible() || popup_.kind() != PopupKind::Tray)
                    tray_->SetHostPlaces(bar_->trayAreaScreenRect());
            }
            if (popup_.IsVisible() && !popup_.closing()) {
                // The hidden-icon list may have changed under the open panel: repaint it
                // with the fresh icons instead of closing it (the host updates while it
                // is open). Each layout snapshots the icons it draws, so a click still
                // routes to the icon that was drawn.
                if (popup_.kind() == PopupKind::Tray) popup_.RefreshPanel();
                else popup_.Render();
            }
            break;
        }
        case 1:  // an app answered the forwarded click: it worked, do not retry
            Log(L"tray: click observed by the app");
            break;
        case 2:  // Explorer's own hidden-icons panel was opened instead
            Log(L"tray: the system tray opened");
            break;
        case 3:  // nothing could deliver the click: the user has to act
            Log(L"tray: needs user intervention; the Windows notification area must be used");
            if (bar_) {
                bar_->ShowTrayHint(settings_.language != L"en-US"
                                       ? L"请在 Windows 托盘中操作该图标"
                                       : L"Use the Windows notification area for this icon");
            }
            break;
        case 4:  // accepted but unconfirmed: never retried
            Log(L"tray: action accepted without confirmation");
            break;
        default:  // 5: an explicit system-tray opening succeeded; nothing to do
            break;
    }
}

RECT App::trayAreaScreenRect() const {
    if (bar_) return bar_->trayAreaScreenRect();
    RECT none{};
    return none;
}

void App::OnDisplayChanged() {
    if (restartAtMs_ == 0 && !restarting_) {
        restartAtMs_ = NowMs() + 1200.0;
        Log(L"app: display configuration changed; scheduling restart");
    }
}

void App::RestartForDisplayChange() {
    if (restarting_) return;
    restarting_ = true;
    restartAtMs_ = 0;
    wchar_t exe[MAX_PATH * 2]{};
    if (GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe))) == 0) {
        Log(L"app: restart aborted (GetModuleFileName failed)");
        restarting_ = false;
        return;
    }
    wchar_t commandLine[MAX_PATH * 3]{};
    swprintf(commandLine, std::size(commandLine), L"\"%ls\" --restart-after %lu", exe,
             static_cast<unsigned long>(GetCurrentProcessId()));
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe, commandLine, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"app: restart failed (error %lu)", GetLastError());
        restarting_ = false;
        return;
    }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (pi.hProcess) CloseHandle(pi.hProcess);
    Log(L"app: restarted after display change");
    PostQuitMessage(0);
}

void App::OnTrayOverflow(RECT anchorScreen) {
    if (trayOverflowOpen()) {
        ClosePopup();
        return;
    }
    // While the hidden-icon panel is open, apps asking where their icon is are
    // told the panel's place (the renderer's own cells come from the model).
    if (tray_) tray_->SetHostPlaces(anchorScreen);
    ShowPanel(PopupKind::Tray, MenuId::None, anchorScreen);
}

void App::OnOpenSettings() {
    ClosePopup();
    OpenSuperMenuSettings();
}

void App::OnQuitRequested() {
    running_ = false;
    PostQuitMessage(0);
}

void App::OnPopupCommand(Command command) {
    ExecuteCommand(command);
    // Connectivity/Focus tiles are toggles inside the Control Center: keep the
    // panel open so the state and the Wi-Fi strength animation stay visible.
    if (command != CmdToggleWifi && command != CmdToggleBluetooth && command != CmdToggleFocus) {
        ClosePopup();
    }
}

void App::StartHardwareWorker() {
    if (hardwareThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(hardwareMutex_);
        hardwareStop_ = false;
    }
    hardwareThread_ = std::thread(&App::HardwareLoop, this);
}

void App::StopHardwareWorker() {
    if (!hardwareThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(hardwareMutex_);
        hardwareStop_ = true;
    }
    hardwareCv_.notify_all();
    hardwareThread_.join();
}

void App::QueueVolume(int percent) {
    std::lock_guard<std::mutex> lock(hardwareMutex_);
    pendingVolume_ = std::clamp(percent, 0, 100);
    hardwareCv_.notify_one();
}

void App::QueueBrightness(int percent) {
    std::lock_guard<std::mutex> lock(hardwareMutex_);
    pendingBrightness_ = std::clamp(percent, 0, 100);
    hardwareCv_.notify_one();
}

void App::QueueMute(bool muted) {
    std::lock_guard<std::mutex> lock(hardwareMutex_);
    pendingMute_ = muted ? 1 : 0;
    hardwareCv_.notify_one();
}

void App::HardwareLoop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        int volume = -1;
        int brightness = -1;
        int mute = -1;
        {
            std::unique_lock<std::mutex> lock(hardwareMutex_);
            hardwareCv_.wait(lock, [this] {
                return hardwareStop_ || pendingVolume_ >= 0 || pendingBrightness_ >= 0 || pendingMute_ >= 0;
            });
            if (hardwareStop_) break;
            volume = pendingVolume_;
            brightness = pendingBrightness_;
            mute = pendingMute_;
            pendingVolume_ = -1;
            pendingBrightness_ = -1;
            pendingMute_ = -1;
        }
        if (volume >= 0) SetMasterVolume(volume);
        if (mute >= 0) SetMasterMuted(mute != 0);
        if (brightness >= 0) {
            const bool ok = SetDisplayBrightness(brightness);
            Log(L"brightness: set %d%% (%s)", brightness, ok ? L"ok" : L"failed");
        }
    }
    CoUninitialize();
}

void App::OnPopupVolume(int percent) {
    const int value = std::clamp(percent, 0, 100);
    sys_.statusMutable().volume = value;
    sys_.statusMutable().volumeMuted = false;
    QueueVolume(value);
    if (popup_.IsVisible()) popup_.Render();
    RefreshStatusAndRender();
}

void App::OnPopupBrightness(int percent) {
    brightness_ = std::clamp(percent, 0, 100);
    if (brightnessSupported_) QueueBrightness(brightness_);
    popup_.SetBrightness(brightness_);
    if (popup_.IsVisible()) popup_.Render();
}

void App::OnPopupMute() {
    const bool muted = !sys_.status().volumeMuted;
    sys_.statusMutable().volumeMuted = muted;
    QueueMute(muted);
    if (popup_.IsVisible()) popup_.Render();
    RefreshStatusAndRender();
}

void App::OnPopupCloseRequested() { ClosePopup(); }

void App::ExecuteCommand(Command command) {
    const HWND target = TargetWindow();
    const bool zh = settings_.language != L"en-US";
    switch (command) {
        case CmdAboutPC: LaunchUri(L"ms-settings:about"); break;
        case CmdSystemSettings: LaunchUri(L"ms-settings:"); break;
        case CmdAppStore: LaunchUri(L"ms-windows-store:"); break;
        case CmdForceQuit: RunProcess(L"taskmgr.exe"); break;
        case CmdSleep: SleepComputer(); break;
        case CmdLock: LockScreen(); break;
        case CmdRestart:
            if (MessageBoxW(bar_->hwnd(), zh ? L"确定要重新启动计算机吗？" : L"Restart this computer?",
                            L"SuperMenu", MB_OKCANCEL | MB_ICONQUESTION) == IDOK) {
                RestartComputer();
            }
            break;
        case CmdShutDown:
            if (MessageBoxW(bar_->hwnd(), zh ? L"确定要关机吗？" : L"Shut down this computer?", L"SuperMenu",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK) {
                ShutDownComputer();
            }
            break;
        case CmdLogOut:
            if (MessageBoxW(bar_->hwnd(), zh ? L"确定要退出登录吗？" : L"Log out of Windows?", L"SuperMenu",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) == IDOK) {
                LogOffComputer();
            }
            break;
        case CmdSuperMenuSettings: OpenSuperMenuSettings(); break;
        case CmdSuperMenuFolder: OpenSuperMenuFolder(); break;
        case CmdHelp: {
            std::wstring readme = JoinPath(ExeDirectory(), L"README.md");
            if (GetFileAttributesW(readme.c_str()) == INVALID_FILE_ATTRIBUTES) {
                readme = JoinPath(ExeDirectory(), L"..\\README.md");
            }
            if (GetFileAttributesW(readme.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", readme.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            else MessageBoxW(bar_->hwnd(), zh ? L"SuperMenu：Windows 的 macOS 风格液态玻璃顶栏。\n\n右键顶栏或托盘图标可以打开设置。" : L"SuperMenu: a macOS-style liquid-glass menu bar for Windows.\n\nRight-click the bar or use the tray icon for settings.", L"SuperMenu Help", MB_ICONINFORMATION | MB_OK);
            break;
        }
        case CmdAboutSuperMenu: ShowAbout(); break;
        case CmdQuitSuperMenu: OnQuitRequested(); break;
        case CmdReloadBar:
            settingsStamp_ = 0;
            HandleSettingsReload();
            break;
        case CmdAboutApp: {
            const ForegroundApp& app = sys_.status().app;
            std::wstring text = app.name + L"\n" + app.path;
            MessageBoxW(bar_->hwnd(), text.c_str(), zh ? L"关于应用" : L"About App", MB_ICONINFORMATION | MB_OK);
            break;
        }
        case CmdAppSettings: LaunchUri(L"ms-settings:appsfeatures"); break;
        case CmdHideApp: HideForegroundApp(target); break;
        case CmdHideOthers: HideOtherWindows(target); break;
        case CmdShowAll: ShowAllWindows(target); break;
        case CmdQuitApp: QuitForegroundApp(target); break;
        case CmdCloseWindow:
            if (target) PostMessageW(target, WM_CLOSE, 0, 0);
            break;
        case CmdMinimizeWindow:
            if (target) ShowWindow(target, SW_MINIMIZE);
            break;
        case CmdZoomWindow:
            if (target) ShowWindow(target, IsZoomed(target) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case CmdWindowLeft: SendWinKey(VK_LEFT); break;
        case CmdWindowRight: SendWinKey(VK_RIGHT); break;
        case CmdFullScreen: SendKey(VK_F11); break;
        case CmdUndo: SendCtrlKey(static_cast<WORD>('Z')); break;
        case CmdRedo: SendCtrlKey(static_cast<WORD>('Y')); break;
        case CmdCut: SendCtrlKey(static_cast<WORD>('X')); break;
        case CmdCopy: SendCtrlKey(static_cast<WORD>('C')); break;
        case CmdPaste: SendCtrlKey(static_cast<WORD>('V')); break;
        case CmdSelectAll: SendCtrlKey(static_cast<WORD>('A')); break;
        case CmdWifiSettings: LaunchUri(L"ms-settings:network-wifi"); break;
        case CmdBluetoothSettings: LaunchUri(L"ms-settings:bluetooth"); break;
        case CmdToggleWifi: {
            // Base rapid clicks on the optimistic UI state, not the state file
            // which may still contain the previous value.
            const bool next = !sys_.status().wifiRadioOn;
            sys_.statusMutable().radioCommandAtMs = NowMs();
            SetRadioState(L"wifi", next);
            sys_.statusMutable().wifiRadioOn = next;
            sys_.statusMutable().radioStateKnown = true;
            popup_.BeginWifiToggleAnimation();
            if (popup_.IsVisible()) popup_.Render();
            break;
        }
        case CmdToggleBluetooth: {
            const bool next = !sys_.status().bluetoothRadioOn;
            sys_.statusMutable().radioCommandAtMs = NowMs();
            SetRadioState(L"bluetooth", next);
            sys_.statusMutable().bluetoothRadioOn = next;
            sys_.statusMutable().radioStateKnown = true;
            if (popup_.IsVisible()) popup_.Render();
            break;
        }
        case CmdToggleFocus: {
            const bool next = !sys_.status().focusOn;
            sys_.statusMutable().radioCommandAtMs = NowMs();
            SetRadioState(L"focus", next);
            sys_.statusMutable().focusOn = next;
            if (popup_.IsVisible()) popup_.Render();
            break;
        }
        case CmdOpenSystemCalendar: LaunchSystemCalendar(); break;
        case CmdBatterySettings: LaunchUri(L"ms-settings:batterysaver"); break;
        case CmdSoundSettings: LaunchUri(L"ms-settings:sound"); break;
        case CmdNetworkSettings: LaunchUri(L"ms-settings:network-status"); break;
        case CmdOpenNotificationSettings: LaunchUri(L"ms-settings:notifications"); break;
        case CmdOpenDisplaySettings: LaunchUri(L"ms-settings:display"); break;
        case CmdToggleTheme: ToggleWindowsTheme(); break;
        case CmdMediaPlayPause: SendMediaKey(VK_MEDIA_PLAY_PAUSE); break;
        case CmdMediaNext: SendMediaKey(VK_MEDIA_NEXT_TRACK); break;
        case CmdMediaPrev: SendMediaKey(VK_MEDIA_PREV_TRACK); break;
        case CmdCenterWindow: break;
        default: break;
    }
}

void App::ApplySettings(bool initial) {
    (void)initial;
    if (!bar_) return;
    bar_->ApplySettings();
    if (hiddenForFullscreen_) {
        // Don't leave a reserved strip while the bar is intentionally hidden.
        bar_->RegisterAppBar(false);
    }
    bar_->Invalidate();
    if (IsWindowVisible(bar_->hwnd())) bar_->Render(true);
    if (tray_ && bar_) tray_->SetHostPlaces(bar_->trayAreaScreenRect());
    sys_.Refresh(true);
    RefreshStatusAndRender();
    ApplyLaunchAtLogin();
}

void App::HandleSettingsReload() {
    const double now = NowMs();
    if (now - lastSettingsCheckMs_ < 700) return;
    lastSettingsCheckMs_ = now;
    const uint64_t stamp = SettingsStamp();
    if (stamp == settingsStamp_) return;
    Settings ns;
    if (!LoadSettings(ns)) {
        settingsStamp_ = stamp;
        return;
    }
    if (ns.monitor != settings_.monitor) {
        Log(L"settings: monitor change needs a restart (requested %d)", ns.monitor);
        const bool zh = ns.language != L"en-US";
        MessageBoxW(bar_ ? bar_->hwnd() : nullptr,
                    zh ? L"显示器设置需要重启 SuperMenu 才能生效。" : L"Monitor changes take effect after restarting SuperMenu.",
                    L"SuperMenu", MB_ICONINFORMATION | MB_OK);
        ns.monitor = settings_.monitor;
    }
    settings_ = ns;
    settingsStamp_ = stamp;
    Log(L"settings: loaded language=%ls theme=%ls autoHide=%d barHeight=%.1f", settings_.language.c_str(),
        settings_.theme.c_str(), settings_.autoHide ? 1 : 0, settings_.barHeight);
    ApplySettings(false);
    ClosePopup();
    bar_->Invalidate();
    bar_->Render(true);
    Log(L"settings: reloaded");
}

void App::ApplyLaunchAtLogin() {
    wchar_t exeBuffer[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exeBuffer, static_cast<DWORD>(std::size(exeBuffer)));
    const std::wstring exe = exeBuffer;
    if (settings_.launchAtLogin) {
        const bool taskInstalled = InstallLogonTask(exe);
        // A logon task starts at user logon, before Explorer's Run-key phase.
        // Keep the Run key only as a fallback when Task Scheduler rejects it.
        SetRunValue(exe, !taskInstalled);
        Log(L"autostart: launchAtLogin=1 task=%d runKeyFallback=%d", taskInstalled ? 1 : 0,
            taskInstalled ? 0 : 1);
    } else {
        DeleteLogonTask();
        SetRunValue(exe, false);
        Log(L"autostart: launchAtLogin=0");
    }
}

void App::EnsureHiddenFullscreenState() {
    if (!bar_) return;
    if (!settings_.hideInFullscreen) {
        if (hiddenForFullscreen_) {
            hiddenForFullscreen_ = false;
            bar_->RegisterAppBar(settings_.reserveWorkArea && !settings_.autoHide);
            bar_->Show(true);
            bar_->Render(true);
        }
        return;
    }
    const bool hide = sys_.status().fullscreen;
    if (hide && !hiddenForFullscreen_) {
        ClosePopup();
        hiddenForFullscreen_ = true;
        bar_->RegisterAppBar(false);
        bar_->Show(false);
    } else if (!hide && hiddenForFullscreen_) {
        hiddenForFullscreen_ = false;
        bar_->RegisterAppBar(settings_.reserveWorkArea && !settings_.autoHide);
        bar_->Show(true);
        bar_->Invalidate();
        bar_->Render(true);
    }
}

void App::UpdateDesktopMenuState() {
    if (!bar_) return;
    const bool focused = sys_.status().app.desktop;
    if (focused == desktopFocused_) return;
    desktopFocused_ = focused;
    if (focused) ClosePopup();
    bar_->Invalidate();
    RenderBar();
    Log(focused ? L"bar: desktop focused, hiding menu buttons" : L"bar: desktop focus left, showing menu buttons");
}

void App::EnsureAutoHide(double now) {
    if (!bar_ || hiddenForFullscreen_) return;
    if (!settings_.autoHide) {
        if (autoHidden_) {
            autoHidden_ = false;
            bar_->Show(true);
            bar_->Invalidate();
            bar_->Render(true);
        }
        return;
    }
    POINT pt{};
    GetCursorPos(&pt);
    const RECT wr = bar_->windowScreenRect();
    const bool overBar = pt.y >= wr.top && pt.y < wr.top + static_cast<LONG>(bar_->barHeightPx());
    if (!IsWindowVisible(bar_->hwnd())) {
        if (pt.y <= wr.top + 2) {
            autoHidden_ = false;
            lastBarPointerMs_ = now;
            bar_->Show(true);
            bar_->Invalidate();
            bar_->Render(true);
        }
        return;
    }
    if (overBar) {
        lastBarPointerMs_ = now;
        return;
    }
    if (now - lastBarPointerMs_ > 550.0) {
        ClosePopup();
        autoHidden_ = true;
        bar_->Show(false);
    }
}

void App::CreateTray() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hinst_;
    wc.lpszClassName = L"SuperMenuTrayHost";
    RegisterClassExW(&wc);
    trayHwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"SuperMenuTrayHost", L"SuperMenu", WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, hinst_, this);
    if (!trayHwnd_) return;
    SetWindowLongPtrW(trayHwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = trayHwnd_;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = kTrayCallback;
    nid.hIcon = LoadIconW(hinst_, MAKEINTRESOURCEW(101));
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, L"SuperMenu — liquid glass menu bar");
    if (Shell_NotifyIconW(NIM_ADD, &nid)) {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        trayAdded_ = true;
    }
}

void App::DestroyTray() {
    if (trayAdded_) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = trayHwnd_;
        nid.uID = kTrayId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        trayAdded_ = false;
    }
    if (trayHwnd_) {
        DestroyWindow(trayHwnd_);
        trayHwnd_ = nullptr;
    }
}

void App::ShowTrayMenu(POINT pt) {
    if (!bar_) return;
    // Use the same glass context menu as the bar, with no shortcut hints, so
    // every right-click surface in the app has the same compact menu.
    activeKind_ = PopupKind::Menu;
    activeMenu_ = MenuId::Context;
    const MenuModel model = BuildBarContextMenu(menuContext());
    const RECT anchor{pt.x, pt.y, pt.x + 1, pt.y + 1};
    if (!popup_.Open(PopupKind::Menu, MenuId::Context, model, anchor, &gpu_, &capture_, this, bar_->ui(),
                     bar_->icons(), gpu_.monitor().rect)) {
        activeKind_ = PopupKind::None;
        activeMenu_ = MenuId::None;
        return;
    }
    InstallHooks();
    bar_->Invalidate();
    RenderBar();
}

void App::OnTrayMessage(WPARAM, LPARAM lParam) {
    switch (LOWORD(lParam)) {
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU: {
            POINT pt{};
            GetCursorPos(&pt);
            ShowTrayMenu(pt);
            break;
        }
        case NIN_SELECT:
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: {
            POINT pt{};
            GetCursorPos(&pt);
            if (LOWORD(lParam) == NIN_SELECT) {
                OpenSuperMenuSettings();
            } else {
                ShowTrayMenu(pt);
            }
            break;
        }
        default: break;
    }
}

void App::ShowAbout() {
    const bool zh = settings_.language != L"en-US";
    // The version follows the build version (native/CMakeLists.txt project VERSION,
    // through the generated supermenu_version.h), never a hand-written string.
    wchar_t text[1024];
    if (zh) {
        swprintf_s(text, L"SuperMenu %ls\n\nWindows 的 macOS 风格顶部菜单栏。\n液态玻璃着色器参考 Atrium Desktop（MIT）。\n字体 HarmonyOS Sans SC（Huawei Device / Hanyi Fonts）。\n\n设计与 Apple macOS 无关联。",
                   SUPERMENU_VERSION_W);
    } else {
        swprintf_s(text, L"SuperMenu %ls\n\nA macOS-style top menu bar for Windows.\nLiquid glass shader adapted from Atrium Desktop (MIT).\nHarmonyOS Sans SC font by Huawei Device / Hanyi Fonts.\n\nNot affiliated with Apple macOS.",
                   SUPERMENU_VERSION_W);
    }
    MessageBoxW(bar_ ? bar_->hwnd() : nullptr, text, L"About SuperMenu", MB_ICONINFORMATION | MB_OK);
}

void App::RestartSelf() {
    wchar_t exe[MAX_PATH * 2]{};
    GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)));
    ShellExecuteW(nullptr, L"open", exe, nullptr, nullptr, SW_SHOWNORMAL);
    running_ = false;
    PostQuitMessage(0);
}

void App::InstallHooks() {
    if (!mouseHook_) {
        mouseHook_ = SetWindowsHookExW(WH_MOUSE_LL, HookMouse, GetModuleHandleW(nullptr), 0);
    }
    if (!keyboardHook_) {
        keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, HookKeyboard, GetModuleHandleW(nullptr), 0);
    }
}

void App::RemoveHooks() {
    if (mouseHook_) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
}

LRESULT CALLBACK App::HookMouse(int code, WPARAM wParam, LPARAM lParam) {
    if (!g_app || code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
    if (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN) {
        const auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
        const bool inPopup = g_app->popup_.ContainsScreenPoint(info->pt);
        const RECT barRect = g_app->bar_ ? g_app->bar_->windowScreenRect() : RECT{};
        const bool inBar = g_app->bar_ && ContainsPoint(barRect, info->pt);
        if (!inPopup && !inBar) {
            PostMessageW(g_app->trayHwnd_, kTrayClosePopup, 0, 0);
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK App::HookKeyboard(int code, WPARAM wParam, LPARAM lParam) {
    if (!g_app || code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
        const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        if (g_app->popupVisible()) {
            if (info->vkCode == VK_ESCAPE) {
                PostMessageW(g_app->trayHwnd_, kTrayKey, VK_ESCAPE, 0);
                return 1;
            }
            if (g_app->activeKind_ == PopupKind::Menu &&
                (info->vkCode == VK_UP || info->vkCode == VK_DOWN || info->vkCode == VK_RETURN)) {
                PostMessageW(g_app->trayHwnd_, kTrayKey, info->vkCode, 0);
                return 1;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK App::TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    if (msg == kTrayCallback) {
        self->OnTrayMessage(wParam, lParam);
        return 0;
    }
    if (msg == kTrayClosePopup) {
        self->ClosePopup();
        return 0;
    }
    if (msg == kTrayKey) {
        if (self->popupVisible()) self->popup_.HandleKey(static_cast<int>(wParam));
        return 0;
    }
    if (msg == kTrayModelEvent) {
        self->OnTrayModelEvent(wParam);
        return 0;
    }
    if (g_settingsChanged && msg == g_settingsChanged) {
        self->settingsStamp_ = 0;
        self->lastSettingsCheckMs_ = 0;
        return 0;
    }
    if (g_taskbarCreated && msg == g_taskbarCreated) {
        if (self->trayHwnd_ && self->trayAdded_) {
            NOTIFYICONDATAW nid{};
            nid.cbSize = sizeof(nid);
            nid.hWnd = self->trayHwnd_;
            nid.uID = kTrayId;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = kTrayCallback;
            nid.hIcon = LoadIconW(self->hinst_, MAKEINTRESOURCEW(101));
            if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            wcscpy_s(nid.szTip, L"SuperMenu — liquid glass menu bar");
            Shell_NotifyIconW(NIM_ADD, &nid);
        }
        // When autostart beats Explorer, the first AppBar registration may have
        // failed. Re-register and refresh the tray snapshot once the shell exists.
        if (self->bar_) {
            self->bar_->RegisterAppBar(self->settings_.reserveWorkArea && !self->settings_.autoHide);
            self->bar_->Show(true);
            // Explorer restarted: the model re-reads the new shell's tray; its
            // host keeps forwarding and handles the TaskbarCreated broadcast.
            if (self->tray_) self->tray_->Refresh();
            self->lastTrayRefreshMs_ = NowMs();
            self->sys_.Refresh(true);
            self->RefreshStatusAndRender();
            Log(L"startup: shell became ready; bar/appbar refreshed");
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace sm
