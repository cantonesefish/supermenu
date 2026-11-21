#pragma once

#include "bar_window.h"
#include "capture.h"
#include "gpu.h"
#include "media_monitor.h"
#include "menu.h"
#include "popup_window.h"
#include "settings.h"
#include "system_status.h"
#include "atrium/shell/tray_model.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace sm {

class App : public BarHost, public PopupHost {
public:
    App() = default;
    App(const App&) = delete;
    App& operator=(const App&) = delete;
    ~App();

    int Run(HINSTANCE hinst, bool openSettingsOnStart = false);
    bool Initialize();
    void Shutdown();

    // BarHost
    const Settings& settings() const override { return settings_; }
    const SystemStatus& status() const override { return sys_.status(); }
    MenuContext menuContext() const override;
    void OnBarItem(MenuId id, RECT anchorScreen) override;
    void OnBarContextMenu(POINT screenPoint) override;
    void OnBarHover(MenuId id) override;
    void OnBarEmptyClick() override;
    MenuId openMenu() const override;
    bool popupVisible() const override;
    bool controlCenterOpen() const override {
        return popup_.IsVisible() && !popup_.closing() && popup_.kind() == PopupKind::ControlCenter;
    }
    bool trayOverflowOpen() const override;
    const std::vector<atrium::TrayIcon>& trayIcons() const override { return trayIconsCache_; }
    RECT trayAreaScreenRect() const override;
    const MediaInfo& mediaInfo() const override { return media_.info(); }
    bool focusOn() const override { return sys_.status().focusOn; }
    void OnTrayIconActivated(const atrium::TrayIcon& icon) override;
    void OnTrayIconContext(const atrium::TrayIcon& icon) override;
    void OnTrayIconHover(const atrium::TrayIcon* icon) override;
    void OnTrayOverflow(RECT anchorScreen) override;
    void OnDisplayChanged() override;
    void OnOpenSettings() override;
    void OnQuitRequested() override;

    // PopupHost
    void OnPopupCommand(Command command) override;
    void OnPopupVolume(int percent) override;
    void OnPopupBrightness(int percent) override;
    void OnPopupMute() override;
    void OnPopupCloseRequested() override;

    static LRESULT CALLBACK TrayWndProc(HWND, UINT, WPARAM, LPARAM);

private:
    void Tick();
    void RenderBar();
    void StartHardwareWorker();
    void StopHardwareWorker();
    void QueueVolume(int percent);
    void QueueBrightness(int percent);
    void QueueMute(bool muted);
    void HardwareLoop();
    void RefreshTray(bool force);
    void OnTrayModelEvent(WPARAM event);
    void WriteTrayLiveFile();
    void RestartForDisplayChange();
    void ShowMenu(MenuId id, RECT anchor);
    void ShowPanel(PopupKind kind, MenuId id, RECT anchor);
    void ClosePopup();
    void InstallHooks();
    void RemoveHooks();
    void ExecuteCommand(Command command);
    void ApplySettings(bool initial);
    void RefreshStatusAndRender();
    void CreateTray();
    void DestroyTray();
    void ShowTrayMenu(POINT pt);
    void ApplyLaunchAtLogin();
    void HandleSettingsReload();
    void EnsureHiddenFullscreenState();
    void UpdateDesktopMenuState();
    void EnsureAutoHide(double now);
    void OnTrayMessage(WPARAM wParam, LPARAM lParam);
    void RestartSelf();
    void ShowAbout();
    std::wstring TargetName() const;
    HWND TargetWindow() const;

    static LRESULT CALLBACK HookMouse(int code, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HookKeyboard(int code, WPARAM wParam, LPARAM lParam);

    HINSTANCE hinst_ = nullptr;
    Gpu gpu_;
    Capture capture_;
    SystemMonitor sys_;
    MediaMonitor media_;
    Settings settings_;
    // One model owns both sources of the notification area: its worker reads
    // Explorer's tray back and its host receives the apps' Shell_NotifyIcon calls.
    std::unique_ptr<atrium::TrayModel> tray_;
    // UI-thread copy of TrayModel::Icons(): the bar and the popup draw and click
    // only this snapshot, so a model refresh never invalidates what is on screen.
    std::vector<atrium::TrayIcon> trayIconsCache_;
    // Left-click continuation: the last left-clicked icon and when, so a second
    // click within the double-click time becomes the shell's second click.
    std::optional<atrium::TrayIcon> lastTrayClick_;
    ULONGLONG lastTrayClickTick_ = 0;
    std::unique_ptr<BarWindow> bar_;
    PopupWindow popup_;
    HWND trayHwnd_ = nullptr;
    bool trayAdded_ = false;
    bool running_ = true;
    bool initialized_ = false;
    bool hiddenForFullscreen_ = false;
    bool desktopFocused_ = false;
    MenuId activeMenu_ = MenuId::None;
    PopupKind activeKind_ = PopupKind::None;
    HWND targetHwnd_ = nullptr;
    uint64_t settingsStamp_ = 0;
    double lastSettingsCheckMs_ = 0;
    double lastTrayRefreshMs_ = 0;
    double restartAtMs_ = 0;
    bool restarting_ = false;
    NetworkKind lastNetworkKind_ = NetworkKind::None;
    int lastVolume_ = -1;
    bool lastMuted_ = false;
    bool lastFocusOn_ = false;
    bool iconStateSeen_ = false;

    // Sliders update the UI immediately; hardware writes happen on a worker so a
    // slow DDC/CI or audio-endpoint call never blocks the drag.
    std::thread hardwareThread_;
    std::mutex hardwareMutex_;
    std::condition_variable hardwareCv_;
    bool hardwareStop_ = false;
    int pendingVolume_ = -1;
    int pendingBrightness_ = -1;
    int pendingMute_ = -1;  // -1 none, 0 unmute, 1 mute
    std::wstring statusKey_;
    HHOOK mouseHook_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    double popupClosedCheckMs_ = 0;
    int brightness_ = 70;
    bool brightnessSupported_ = false;
    bool autoHidden_ = false;
    double lastBarPointerMs_ = 0;
    UINT_PTR tickTimerId_ = 0;
};

}  // namespace sm
