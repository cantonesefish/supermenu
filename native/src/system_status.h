#pragma once

#include "common.h"
#include "settings.h"

#include <wlanapi.h>

#include <string>

namespace sm {

struct ForegroundApp {
    HWND hwnd = nullptr;
    DWORD pid = 0;
    std::wstring name;    // display name (FileDescription / exe stem / localization)
    std::wstring path;    // exe path when known
    bool desktop = false; // Explorer desktop is foreground
};

enum class NetworkKind { None, Ethernet, Wifi };

struct SystemStatus {
    ForegroundApp app;
    SYSTEMTIME now{};
    bool batteryPresent = false;
    int batteryPercent = -1;
    bool batteryCharging = false;
    bool batteryOnAc = false;
    int batterySecondsLeft = -1;
    bool windowsUpdateRestartRequired = false;
    bool inputMethodAvailable = false;
    bool inputMethodNative = false;
    std::wstring inputMethodLabel;  // mirrors the system input-indicator text
    bool privacyMicrophone = false;
    bool privacyCamera = false;
    bool wifiSupported = false;
    bool wifiConnected = false;
    int wifiSignal = 0;  // 0..100
    std::wstring wifiSsid;
    bool ethernetConnected = false;
    NetworkKind network = NetworkKind::None;
    bool wifiRadioOn = true;
    bool bluetoothRadioOn = true;
    bool focusOn = false;
    bool radioStateKnown = false;
    double radioCommandAtMs = 0.0;
    bool volumeMuted = false;
    int volume = 0;  // 0..100
    std::wstring audioDevice;
    bool darkTheme = false;
    bool fullscreen = false;
    // Timestamps used by the Fluent status-icon transitions (NowMs values).
    double networkChangedMs = 0.0;
    double volumeChangedMs = 0.0;
};

class SystemMonitor {
public:
    SystemMonitor() = default;
    ~SystemMonitor();

    void Refresh(bool force = false);
    void RefreshRadioStateNow() { RefreshRadioState(); }
    void RefreshInputMethodNow() { RefreshInputMethod(); }
    void OpenInputMethodMenu();
    const SystemStatus& status() const { return status_; }
    SystemStatus& statusMutable() { return status_; }

    // Keep the last external foreground app while our own no-activate windows
    // briefly receive focus.
    void RememberForeground(HWND hwnd);
    ForegroundApp LastExternalApp() const { return lastExternal_; }

private:
    void RefreshForeground();
    void RefreshBattery();
    void RefreshWifi();
    void RefreshEthernet();
    void RefreshVolume();
    void RefreshRadioState();
    void RefreshWindowsUpdate();
    void RefreshInputMethod();
    void RefreshPrivacy();
    HWND InputMethodTarget() const;
    SystemStatus status_{};
    ForegroundApp lastExternal_{};
    double lastForegroundMs_ = 0;
    double lastBatteryMs_ = 0;
    double lastWifiMs_ = 0;
    double lastEthernetMs_ = 0;
    double lastVolumeMs_ = 0;
    double lastWindowsUpdateMs_ = 0;
    double lastInputMethodMs_ = 0;
    double lastPrivacyMs_ = 0;
    HANDLE wlan_ = nullptr;
    bool foregroundInitialized_ = false;
};

std::wstring FormatClock(const SYSTEMTIME& t, const Settings& s);
std::wstring FormatDate(const SYSTEMTIME& t, const Settings& s);
std::wstring FormatDateWithYear(const SYSTEMTIME& t, const Settings& s);
bool IsFullscreenAppForeground();
bool IsDesktopWindowForeground(HWND hwnd);
void LaunchUri(const wchar_t* uri);
void LaunchSystemCalendar();
void SetRadioState(const wchar_t* kind, bool on);
void OpenSuperMenuSettings();
void OpenSuperMenuFolder();

// Generic foreground-window commands (the bar never activates itself).
void SendCtrlKey(WORD vk);
void SendWinKey(WORD vk);
void SendMediaKey(WORD vk);
void SendKey(WORD vk);
void MinimizeForeground();
void MaximizeOrRestoreForeground();
void CloseForeground();
void QuitForegroundApp(HWND target);
void HideForegroundApp(HWND target);
void HideOtherWindows(HWND target);
void ShowAllWindows(HWND target);
void FullScreenForeground();
void LockScreen();
void SleepComputer();
void RestartComputer();
void ShutDownComputer();
void LogOffComputer();
void ToggleWindowsTheme();
void SetMasterVolume(int percent);
void SetMasterMuted(bool muted);

// Brings an existing window of the program to the foreground, or launches the
// executable when it is not running. Used by mirrored system-tray icons (Windows
// exposes no public API to replay an icon's own tray callback).
bool ActivateOrLaunchExecutable(const std::wstring& exePath);

// DDC/WMI display brightness. Returns false on desktop monitors that do not
// expose brightness control through WMI.
bool GetDisplayBrightness(int& percent);
bool SetDisplayBrightness(int percent);

}  // namespace sm
