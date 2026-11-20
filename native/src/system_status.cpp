#include <winsock2.h>

#include "system_status.h"

#include "json.h"

#include <winevt.h>
#include <wlanapi.h>
#include <wbemcli.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>
#include <imm.h>
#include <iphlpapi.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <powrprof.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <map>

// MinGW's imm.h omits the WM_IME_CONTROL conversion-mode commands.
#ifndef IMC_GETCONVERSIONMODE
#define IMC_GETCONVERSIONMODE 0x0001
#endif
#ifndef IMC_SETCONVERSIONMODE
#define IMC_SETCONVERSIONMODE 0x0002
#endif

namespace sm {
namespace {

const GUID kClsidMMDeviceEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const PROPERTYKEY kPkeyFriendlyName = {
    {0xA45C254E, 0xDF1C, 0x4EFD, {0x80, 0x20, 0x67, 0xD1, 0x46, 0xA8, 0x50, 0xE0}}, 14};

std::wstring FileDescription(const std::wstring& path) {
    if (path.empty()) return {};
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!size) return {};
    std::vector<uint8_t> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
    struct Lang {
        WORD lang;
        WORD codepage;
    };
    Lang* lang = nullptr;
    UINT langSize = 0;
    if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&lang), &langSize) ||
        langSize < sizeof(Lang)) {
        return {};
    }
    wchar_t query[128];
    swprintf(query, std::size(query), L"\\StringFileInfo\\%04x%04x\\FileDescription", lang[0].lang, lang[0].codepage);
    wchar_t* value = nullptr;
    UINT valueSize = 0;
    if (VerQueryValueW(data.data(), query, reinterpret_cast<void**>(&value), &valueSize) && value && valueSize) {
        return std::wstring(value, wcsnlen(value, valueSize));
    }
    return {};
}

std::wstring ExeStem(const std::wstring& path) {
    if (path.empty()) return {};
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    const auto dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    return name;
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return false;
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL ok = AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(token);
    return ok;
}

std::wstring ReadRegString(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    return {};
}

}  // namespace

SystemMonitor::~SystemMonitor() {
    if (wlan_) WlanCloseHandle(wlan_, nullptr);
}

void SystemMonitor::Refresh(bool force) {
    const double now = NowMs();
    status_.now = [] {
        SYSTEMTIME t{};
        GetLocalTime(&t);
        return t;
    }();
    if (force || now - lastForegroundMs_ > 250) {
        RefreshForeground();
        lastForegroundMs_ = now;
    }
    if (force || now - lastBatteryMs_ > 2000) {
        RefreshBattery();
        lastBatteryMs_ = now;
    }
    if (force || now - lastWifiMs_ > 2500) {
        RefreshWifi();
        lastWifiMs_ = now;
    }
    if (force || now - lastEthernetMs_ > 1500) {
        RefreshEthernet();
        lastEthernetMs_ = now;
    }
    if (force || now - lastVolumeMs_ > 1000) {
        RefreshVolume();
        lastVolumeMs_ = now;
    }
    if (force || now - lastVolumeMs_ > 1000) RefreshRadioState();
    if (force || now - lastWindowsUpdateMs_ > 5000) {
        RefreshWindowsUpdate();
        lastWindowsUpdateMs_ = now;
    }
    if (force || now - lastInputMethodMs_ > 500) {
        RefreshInputMethod();
        lastInputMethodMs_ = now;
    }
    if (force || now - lastPrivacyMs_ > 1000) {
        RefreshPrivacy();
        lastPrivacyMs_ = now;
    }
    status_.network = status_.ethernetConnected
                          ? NetworkKind::Ethernet
                          : (status_.wifiConnected ? NetworkKind::Wifi : NetworkKind::None);
    status_.darkTheme = IsDarkAppTheme();
    status_.fullscreen = IsFullscreenAppForeground();
}

void SystemMonitor::RememberForeground(HWND hwnd) {
    if (!hwnd) return;
    const ForegroundApp fg = status_.app;
    if (fg.hwnd == hwnd && !fg.desktop) return;
    (void)hwnd;
    if (!status_.app.hwnd) return;
    lastExternal_ = status_.app;
}

void SystemMonitor::RefreshForeground() {
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool own = pid == GetCurrentProcessId();
    if (own && lastExternal_.hwnd) {
        status_.app = lastExternal_;
        return;
    }
    if (!fg) {
        if (!status_.app.hwnd) lastExternal_ = status_.app;
        return;
    }
    ForegroundApp app;
    app.hwnd = fg;
    app.pid = pid;
    if (IsDesktopWindowForeground(fg)) {
        app.desktop = true;
        app.name = L"Finder";
        app.path.clear();
    } else {
        wchar_t pathBuf[MAX_PATH * 2]{};
        DWORD pathSize = static_cast<DWORD>(std::size(pathBuf));
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (process) {
            if (QueryFullProcessImageNameW(process, 0, pathBuf, &pathSize)) app.path = pathBuf;
            CloseHandle(process);
        }
        app.name = FileDescription(app.path);
        if (app.name.empty()) app.name = ExeStem(app.path);
        if (app.name.empty()) app.name = L"SuperMenu";
        const std::wstring lower = Lower(app.path);
        if (lower.find(L"supermenu.settings.exe") != std::wstring::npos) {
            app.name = L"SuperMenu 设置";
        }
    }
    status_.app = app;
    if (!app.desktop && !app.path.empty()) lastExternal_ = app;
}

void SystemMonitor::RefreshBattery() {
    SYSTEM_POWER_STATUS sps{};
    if (!GetSystemPowerStatus(&sps)) return;
    status_.batteryPresent = sps.BatteryFlag != 128 && sps.BatteryLifePercent != 255;
    status_.batteryPercent = sps.BatteryLifePercent == 255 ? -1 : int(sps.BatteryLifePercent);
    status_.batteryOnAc = sps.ACLineStatus == 1;
    status_.batteryCharging = status_.batteryOnAc && (sps.BatteryFlag & 8) != 0;
    status_.batterySecondsLeft = sps.BatteryLifeTime == DWORD(-1) ? -1 : int(sps.BatteryLifeTime);
}

void SystemMonitor::RefreshWifi() {
    if (!wlan_) {
        DWORD version = 2;
        if (WlanOpenHandle(2, nullptr, &version, &wlan_) != ERROR_SUCCESS) {
            status_.wifiSupported = false;
            return;
        }
    }
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(wlan_, nullptr, &list) != ERROR_SUCCESS) return;
    status_.wifiSupported = list->dwNumberOfItems > 0;
    status_.wifiConnected = false;
    status_.wifiSignal = 0;
    status_.wifiSsid.clear();
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
        const auto& item = list->InterfaceInfo[i];
        if (item.isState != wlan_interface_state_connected) continue;
        DWORD size = 0;
        void* data = nullptr;
        if (WlanQueryInterface(wlan_, &item.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size, &data,
                               nullptr) == ERROR_SUCCESS &&
            data) {
            auto* conn = static_cast<WLAN_CONNECTION_ATTRIBUTES*>(data);
            const auto& assoc = conn->wlanAssociationAttributes;
            status_.wifiConnected = true;
            status_.wifiSignal = int(assoc.wlanSignalQuality);
            const DWORD len = assoc.dot11Ssid.uSSIDLength;
            const int wide = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCCH>(assoc.dot11Ssid.ucSSID),
                                                 static_cast<int>(len), nullptr, 0);
            if (wide > 0) {
                std::wstring ssid(static_cast<size_t>(wide), L'\0');
                MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<LPCCH>(assoc.dot11Ssid.ucSSID),
                                    static_cast<int>(len), ssid.data(), wide);
                status_.wifiSsid = ssid;
            }
            WlanFreeMemory(data);
            break;
        }
    }
    WlanFreeMemory(list);
}

void SystemMonitor::RefreshEthernet() {
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buffer;
    ULONG result = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 3; ++attempt) {
        buffer.resize(size);
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr,
                                      reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        if (result != ERROR_BUFFER_OVERFLOW) break;
    }
    if (result != NO_ERROR) return;
    bool connected = false;
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter;
         adapter = adapter->Next) {
        // Physical wired adapters only. VPN/Tailscale adapters report as tunnel
        // or PPP and must not suppress the Ethernet/Wi-Fi distinction.
        if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD && adapter->IfType != IF_TYPE_FASTETHER &&
            adapter->IfType != IF_TYPE_GIGABITETHERNET)
            continue;
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (!adapter->FirstGatewayAddress) continue;
        connected = true;
        break;
    }
    status_.ethernetConnected = connected;
}

void SystemMonitor::RefreshVolume() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) return;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.put())))) {
        return;
    }
    float level = 0;
    BOOL mute = FALSE;
    if (SUCCEEDED(volume->GetMasterVolumeLevelScalar(&level))) status_.volume = int(std::lround(level * 100.0f));
    if (SUCCEEDED(volume->GetMute(&mute))) status_.volumeMuted = mute != FALSE;

    ComPtr<IPropertyStore> store;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, store.put()))) {
        PROPVARIANT var;
        PropVariantInit(&var);
        if (SUCCEEDED(store->GetValue(kPkeyFriendlyName, &var)) && var.vt == VT_LPWSTR && var.pwszVal) {
            status_.audioDevice = var.pwszVal;
        }
        PropVariantClear(&var);
    }
}

HWND SystemMonitor::InputMethodTarget() const {
    HWND foreground = GetForegroundWindow();
    if (foreground) {
        DWORD pid = 0;
        GetWindowThreadProcessId(foreground, &pid);
        if (pid != GetCurrentProcessId() && IsWindowVisible(foreground)) return foreground;
    }
    if (lastExternal_.hwnd && IsWindow(lastExternal_.hwnd)) return lastExternal_.hwnd;
    if (status_.app.hwnd && IsWindow(status_.app.hwnd)) return status_.app.hwnd;
    return foreground;
}

void SystemMonitor::RefreshInputMethod() {
    HWND target = InputMethodTarget();
    if (!target) {
        status_.inputMethodAvailable = false;
        status_.inputMethodNative = false;
        status_.inputMethodLabel.clear();
        return;
    }
    const DWORD thread = GetWindowThreadProcessId(target, nullptr);
    HKL layout = GetKeyboardLayout(thread);
    const LANGID language = LOWORD(reinterpret_cast<DWORD_PTR>(layout));
    const WORD primary = PRIMARYLANGID(language);
    bool native = false;
    if (primary == LANG_CHINESE || primary == LANG_JAPANESE || primary == LANG_KOREAN) {
        native = true;  // CJK layouts default to their native input mode.
        if (HWND ime = ImmGetDefaultIMEWnd(target)) {
            DWORD_PTR mode = 0;
            if (SendMessageTimeoutW(ime, WM_IME_CONTROL, IMC_GETCONVERSIONMODE, 0,
                                    SMTO_ABORTIFHUNG, 40, &mode)) {
                native = (mode & IME_CMODE_NATIVE) != 0;
            }
        }
    }

    std::wstring label;
    if (primary == LANG_CHINESE) {
        label = native ? L"中" : L"英";
    } else if (primary == LANG_JAPANESE) {
        label = native ? L"あ" : L"A";
    } else if (primary == LANG_KOREAN) {
        label = native ? L"한" : L"A";
    } else if (primary == LANG_ENGLISH) {
        label = L"ENG";
    } else {
        wchar_t abbreviation[16]{};
        const LCID lcid = MAKELCID(language, SORT_DEFAULT);
        if (GetLocaleInfoW(lcid, LOCALE_SABBREVLANGNAME, abbreviation,
                           static_cast<int>(std::size(abbreviation)))) {
            label = abbreviation;
        } else {
            label = L"IME";
        }
    }
    const bool changed = !status_.inputMethodAvailable ||
                         status_.inputMethodNative != native ||
                         status_.inputMethodLabel != label;
    status_.inputMethodAvailable = true;
    status_.inputMethodNative = native;
    status_.inputMethodLabel = std::move(label);
    if (changed) {
        sm::Log(L"input: layout=%04x native=%d label=%ls", static_cast<unsigned>(language),
                native ? 1 : 0, status_.inputMethodLabel.c_str());
    }
}

namespace {

bool ReadQwordValue(HKEY key, const wchar_t* name, unsigned long long& value) {
    DWORD type = 0;
    DWORD size = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &size) ==
               ERROR_SUCCESS &&
           type == REG_QWORD && size == sizeof(value);
}

// Windows records capability use below ConsentStore; LastUsedTimeStop is zero
// while an app is actively using the camera/microphone.
bool CapabilityKeyInUse(HKEY key, int depth) {
    unsigned long long start = 0;
    unsigned long long stop = 0;
    if (ReadQwordValue(key, L"LastUsedTimeStart", start) &&
        ReadQwordValue(key, L"LastUsedTimeStop", stop) && start != 0 && stop == 0) {
        return true;
    }
    if (depth >= 5) return false;
    wchar_t name[256]{};
    DWORD index = 0;
    for (;;) {
        DWORD nameLength = static_cast<DWORD>(std::size(name));
        const LONG result = RegEnumKeyExW(key, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
        if (result != ERROR_SUCCESS) break;
        HKEY child = nullptr;
        if (RegOpenKeyExW(key, name, 0, KEY_READ, &child) == ERROR_SUCCESS) {
            const bool active = CapabilityKeyInUse(child, depth + 1);
            RegCloseKey(child);
            if (active) return true;
        }
        ++index;
    }
    return false;
}

bool CapabilityActive(const wchar_t* capability) {
    std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\";
    path += capability;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    const bool active = CapabilityKeyInUse(key, 0);
    RegCloseKey(key);
    return active;
}

std::wstring EventDataValue(const std::wstring& xml, const wchar_t* name) {
    std::wstring needle = L"Name=\"";
    needle += name;
    needle += L"\">";
    size_t open = xml.find(needle);
    if (open == std::wstring::npos) {
        needle = L"Name='";
        needle += name;
        needle += L"'>";
        open = xml.find(needle);
        if (open == std::wstring::npos) return {};
    }
    open += needle.size();
    const size_t close = xml.find(L'<', open);
    if (close == std::wstring::npos) return {};
    return xml.substr(open, close - open);
}

// Windows 11's taskbar privacy indicator is driven by the FrameServer stream
// lifecycle. ConsentStore is only an audit trail on some builds and may not be
// written in real time. The latest FSStream watchdog operation is 1 (start) or
// 4 (stop), which gives us the actual live camera state.
// ConsentStore can be an audit trail rather than a live state (the Windows
// camera and microphone indicators can stay absent on some builds). WASAPI is
// the authoritative live signal for microphone capture: an active capture
// session exists exactly while an app is recording.
bool AudioCaptureActive() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return false;
    }
    const ERole roles[] = {eConsole, eCommunications};
    for (ERole role : roles) {
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, role, device.put()))) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.put())))) {
            continue;
        }
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(sessions.put()))) continue;
        int count = 0;
        if (FAILED(sessions->GetCount(&count))) continue;
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, control.put()))) continue;
            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(control->GetState(&state)) && state == AudioSessionStateActive) return true;
        }
    }
    return false;
}

bool CameraFrameServerActive() {
    constexpr wchar_t kChannel[] = L"Microsoft-Windows-MF-FrameServer/Camera_FrameServer";
    EVT_HANDLE query = EvtQuery(nullptr, kChannel, L"*[System[(EventID=11)]]",
                                EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!query) return false;
    EVT_HANDLE events[8]{};
    DWORD returned = 0;
    const BOOL got = EvtNext(query, static_cast<DWORD>(std::size(events)), events, 0, 0, &returned);
    bool active = false;
    if (got && returned > 0) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD bytes = 0;
            DWORD properties = 0;
            if (!EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &bytes, &properties) &&
                GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
                continue;
            }
            std::wstring xml(bytes / sizeof(wchar_t) + 1, L'\0');
            if (!EvtRender(nullptr, events[i], EvtRenderEventXml, bytes,
                           xml.data(), &bytes, &properties)) {
                continue;
            }
            xml.resize(wcslen(xml.c_str()));
            if (EventDataValue(xml, L"TimerScope") != L"1") continue;
            const std::wstring operation = EventDataValue(xml, L"WatchdogOperation");
            active = operation == L"1";  // 1 = stream start, 4 = stream stop
            break;
        }
    }
    for (DWORD i = 0; i < returned; ++i) EvtClose(events[i]);
    EvtClose(query);
    return active;
}

}  // namespace

void SystemMonitor::RefreshPrivacy() {
    const bool microphone = AudioCaptureActive() || CapabilityActive(L"microphone");
    const bool camera = CameraFrameServerActive() || CapabilityActive(L"webcam");
    if (status_.privacyMicrophone != microphone || status_.privacyCamera != camera) {
        sm::Log(L"privacy: microphone=%d camera=%d", microphone ? 1 : 0, camera ? 1 : 0);
    }
    status_.privacyMicrophone = microphone;
    status_.privacyCamera = camera;
}

void SystemMonitor::OpenInputMethodMenu() {
    // Win+Space opens the same Windows input-method switcher as the taskbar
    // input indicator, so Japanese/Wubi/other installed layouts are selectable.
    sm::Log(L"input: opening system keyboard-layout switcher");
    SendWinKey(VK_SPACE);
}

void SystemMonitor::RefreshWindowsUpdate() {
    auto keyExists = [](const wchar_t* path) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
        RegCloseKey(key);
        return true;
    };
    status_.windowsUpdateRestartRequired =
        keyExists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
        keyExists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\PostRebootReporting") ||
        keyExists(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending");
}

void SystemMonitor::RefreshRadioState() {
    // While a user toggle is in flight the helper's state file is still the
    // previous value; keep the optimistic UI state until it has caught up.
    if (status_.radioCommandAtMs > 0 && NowMs() - status_.radioCommandAtMs < 1200.0) return;
    const std::wstring path = LocalAppData() + L"\\SuperMenu\\radio\\state.txt";
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[256]{};
    DWORD read = 0;
    const BOOL ok = ReadFile(file, text, sizeof(text) - 1, &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return;
    text[read] = '\0';
    bool wifi = status_.wifiRadioOn;
    bool bluetooth = status_.bluetoothRadioOn;
    bool focus = status_.focusOn;
    if (const char* value = strstr(text, "wifi=")) wifi = value[5] == '1';
    if (const char* value = strstr(text, "bluetooth=")) bluetooth = value[10] == '1';
    if (const char* value = strstr(text, "focus=")) focus = value[6] == '1';
    status_.wifiRadioOn = wifi;
    status_.bluetoothRadioOn = bluetooth;
    status_.focusOn = focus;
    status_.radioStateKnown = true;
}

std::wstring FormatClock(const SYSTEMTIME& t, const Settings& s) {
    wchar_t buf[64]{};
    if (s.use24Hour) {
        if (s.showSeconds) swprintf(buf, std::size(buf), L"%02u:%02u:%02u", t.wHour, t.wMinute, t.wSecond);
        else swprintf(buf, std::size(buf), L"%02u:%02u", t.wHour, t.wMinute);
    } else {
        const int hour = t.wHour % 12 == 0 ? 12 : t.wHour % 12;
        if (s.showSeconds) swprintf(buf, std::size(buf), L"%d:%02d:%02d %ls", hour, t.wMinute, t.wSecond,
                                    t.wHour < 12 ? L"AM" : L"PM");
        else swprintf(buf, std::size(buf), L"%d:%02d %ls", hour, t.wMinute, t.wHour < 12 ? L"AM" : L"PM");
    }
    return buf;
}

std::wstring FormatDate(const SYSTEMTIME& t, const Settings& s) {
    if (s.language == L"en-US") {
        static const wchar_t* weekdays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                          L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        wchar_t buf[64]{};
        swprintf(buf, std::size(buf), L"%ls %u, %ls", months[t.wMonth - 1], t.wDay, weekdays[t.wDayOfWeek]);
        return buf;
    }
    static const wchar_t* weekdays[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    wchar_t buf[64]{};
    swprintf(buf, std::size(buf), L"%u月%u日 %ls", t.wMonth, t.wDay, weekdays[t.wDayOfWeek]);
    return buf;
}

std::wstring FormatDateWithYear(const SYSTEMTIME& t, const Settings& s) {
    if (s.language == L"en-US") {
        static const wchar_t* weekdays[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
        static const wchar_t* months[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                          L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
        wchar_t buf[64]{};
        swprintf(buf, std::size(buf), L"%ls %u, %u, %ls", months[t.wMonth - 1], t.wDay, t.wYear,
                 weekdays[t.wDayOfWeek]);
        return buf;
    }
    static const wchar_t* weekdays[] = {L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六"};
    wchar_t buf[64]{};
    swprintf(buf, std::size(buf), L"%u年%u月%u日 %ls", t.wYear, t.wMonth, t.wDay, weekdays[t.wDayOfWeek]);
    return buf;
}

bool IsDesktopWindowForeground(HWND hwnd) {
    if (!hwnd) return false;
    wchar_t cls[128]{};
    GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
    return wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0 ||
           wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

bool IsFullscreenAppForeground() {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) {
        if (state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_PRESENTATION_MODE) return true;
    }
    HWND fg = GetForegroundWindow();
    if (!fg || IsDesktopWindowForeground(fg)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;
    // A maximized, captioned window is not fullscreen. This matters when the
    // appbar is disabled: a normal maximized app would otherwise hide the bar.
    if (IsZoomed(fg)) return false;
    const LONG style = GetWindowLongW(fg, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    if (style & WS_CHILD) return false;
    // Desktop widgets and other click-through overlays can cover the screen
    // without being fullscreen applications (Atrium's widget host is one).
    wchar_t cls[128]{};
    GetClassNameW(fg, cls, static_cast<int>(std::size(cls)));
    if (wcscmp(cls, L"AtriumSurface") == 0 || wcscmp(cls, L"Progman") == 0 ||
        wcscmp(cls, L"WorkerW") == 0 || wcscmp(cls, L"Shell_TrayWnd") == 0)
        return false;
    const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT)) != 0) return false;
    RECT wr{}, monitor{};
    if (!GetWindowRect(fg, &wr)) return false;
    HMONITOR mon = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return false;
    monitor = mi.rcMonitor;
    return wr.left <= monitor.left && wr.top <= monitor.top && wr.right >= monitor.right && wr.bottom >= monitor.bottom;
}

void LaunchUri(const wchar_t* uri) {
    ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

void LaunchSystemCalendar() {
    // Win+N opens the shell's taskbar notification center, whose right-hand
    // pane is the same system calendar surface as clicking the taskbar clock
    // (including when the taskbar is auto-hidden).
    INPUT inputs[4]{};
    for (auto& input : inputs) input.type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_LWIN;
    inputs[1].ki.wVk = 'N';
    inputs[2].ki.wVk = 'N';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_LWIN;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

void SetRadioState(const wchar_t* kind, bool on) {
    // The media helper already runs for the lifetime of the bar. Hand it the
    // radio command through a tiny file instead of spawning (and potentially
    // activating) a second process; it performs the WinRT radio call.
    const std::wstring dir = LocalAppData() + L"\\SuperMenu\\radio";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring path = dir + L"\\command.txt";
    const std::wstring temporary = path + L".tmp";
    std::string text = wcscmp(kind, L"wifi") == 0 ? "wifi"
                     : wcscmp(kind, L"focus") == 0 ? "focus"
                                                   : "bluetooth";
    text += on ? "=on\n" : "=off\n";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(file);
        MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

void OpenSuperMenuSettings() {
    // Prefer an already-open settings window. Launching a second copy would
    // create a duplicate tray/window and is especially annoying from the bar's
    // right-click gesture.
    struct SettingsWindowSearch {
        bool found = false;
    } search;
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<SettingsWindowSearch*>(param);
            if (!IsWindowVisible(hwnd)) return TRUE;
            wchar_t title[2]{};
            if (GetWindowTextW(hwnd, title, 2) == 0) return TRUE;
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (!process) return TRUE;
            wchar_t path[MAX_PATH * 2]{};
            DWORD length = static_cast<DWORD>(std::size(path));
            const bool isSettings = QueryFullProcessImageNameW(process, 0, path, &length) &&
                                    Lower(path).find(L"supermenu.settings.exe") != std::wstring::npos;
            CloseHandle(process);
            if (!isSettings) return TRUE;
            if (IsIconic(hwnd)) ShowWindowAsync(hwnd, SW_RESTORE);
            ShowWindowAsync(hwnd, SW_SHOWNORMAL);
            SetForegroundWindow(hwnd);
            state->found = true;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (search.found) return;

    const std::wstring exeDir = ExeDirectory();
    const wchar_t* candidates[] = {L"Settings\\SuperMenu.Settings.exe", L"SuperMenu.Settings.exe",
                                   L"..\\settings\\SuperMenu.Settings\\bin\\Release\\net10.0-windows\\SuperMenu.Settings.exe",
                                   L"..\\settings\\SuperMenu.Settings\\bin\\Release\\net10.0-windows\\win-x64\\SuperMenu.Settings.exe"};
    for (const wchar_t* rel : candidates) {
        const std::wstring path = exeDir + rel;
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, exeDir.c_str(), SW_SHOWNORMAL);
            return;
        }
    }
    // Development tree: the settings app may live under settings/.
    std::wstring root = exeDir;
    for (int i = 0; i < 4 && !root.empty(); ++i) {
        const std::wstring probe = root + L"settings\\SuperMenu.Settings\\bin\\Release\\net10.0-windows\\SuperMenu.Settings.exe";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            ShellExecuteW(nullptr, L"open", probe.c_str(), nullptr, root.c_str(), SW_SHOWNORMAL);
            return;
        }
        root = root.substr(0, root.find_last_of(L"\\/", root.size() - 2) + 1);
    }
    MessageBoxW(nullptr, L"SuperMenu.Settings.exe was not found next to the bar or in the development tree.",
                L"SuperMenu", MB_ICONINFORMATION | MB_OK);
}

void OpenSuperMenuFolder() {
    const std::wstring dir = SettingsDirectory();
    ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

namespace {
void InjectKey(WORD vk, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = vk;
    in.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(in));
}
void Chord(WORD modifier, WORD vk) {
    InjectKey(modifier, true);
    InjectKey(vk, true);
    InjectKey(vk, false);
    InjectKey(modifier, false);
}
void Chord2(WORD mod1, WORD mod2, WORD vk) {
    InjectKey(mod1, true);
    InjectKey(mod2, true);
    InjectKey(vk, true);
    InjectKey(vk, false);
    InjectKey(mod2, false);
    InjectKey(mod1, false);
}
}  // namespace

void SendCtrlKey(WORD vk) { Chord(VK_CONTROL, vk); }
void SendWinKey(WORD vk) { Chord(VK_LWIN, vk); }
void SendKey(WORD vk) {
    InjectKey(vk, true);
    InjectKey(vk, false);
}

void SendMediaKey(WORD vk) {
    // Hardware media keys are extended-key scancodes. Supplying both the
    // extended flag and the scancode makes the synthetic key follow the same
    // path as a real keyboard; otherwise apps that filter on scancode (Edge,
    // QQ Music, some players) can ignore the VK-only event.
    INPUT in[2]{};
    const WORD scan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = vk;
        in[i].ki.wScan = scan;
        in[i].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | (i == 1 ? KEYEVENTF_KEYUP : 0);
    }
    const UINT sent = SendInput(2, in, sizeof(INPUT));
    if (sent != 2) {
        Log(L"media: SendInput(%u) failed sent=%u error=%lu", static_cast<unsigned>(vk),
            static_cast<unsigned>(sent), GetLastError());
    }
}
void MinimizeForeground() {
    HWND fg = GetForegroundWindow();
    if (fg) ShowWindow(fg, SW_MINIMIZE);
}
void MaximizeOrRestoreForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    ShowWindow(fg, IsZoomed(fg) ? SW_RESTORE : SW_MAXIMIZE);
}
void CloseForeground() {
    HWND fg = GetForegroundWindow();
    if (fg) PostMessageW(fg, WM_CLOSE, 0, 0);
}
void QuitForegroundApp(HWND target) {
    if (!target) target = GetForegroundWindow();
    if (target) PostMessageW(target, WM_CLOSE, 0, 0);
}
void HideForegroundApp(HWND target) {
    if (!target) target = GetForegroundWindow();
    if (target) ShowWindow(target, SW_MINIMIZE);
}
void HideOtherWindows(HWND target) {
    if (!target) return;
    struct Ctx {
        DWORD pid;
        HWND keep;
    } ctx{};
    GetWindowThreadProcessId(target, &ctx.pid);
    ctx.keep = target;
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && hwnd != c->keep && IsWindowVisible(hwnd) && GetWindow(hwnd, GW_OWNER) == nullptr) {
                ShowWindow(hwnd, SW_MINIMIZE);
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}
void ShowAllWindows(HWND target) {
    if (!target) return;
    struct Ctx {
        DWORD pid;
    } ctx{};
    GetWindowThreadProcessId(target, &ctx.pid);
    EnumWindows(
        [](HWND hwnd, LPARAM lp) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(lp);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid == c->pid && IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
}
void FullScreenForeground() {
    HWND fg = GetForegroundWindow();
    if (!fg) return;
    SendWinKey(VK_UP);
    (void)fg;
}
void LockScreen() { LockWorkStation(); }
void SleepComputer() {
    EnablePrivilege(SE_SHUTDOWN_NAME);
    SetSuspendState(FALSE, FALSE, FALSE);
}
void RestartComputer() {
    if (EnablePrivilege(SE_SHUTDOWN_NAME)) ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER);
}
void ShutDownComputer() {
    if (EnablePrivilege(SE_SHUTDOWN_NAME)) ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER);
}
void LogOffComputer() {
    if (EnablePrivilege(SE_SHUTDOWN_NAME)) ExitWindowsEx(EWX_LOGOFF | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_OTHER);
}
void ToggleWindowsTheme() {
    const bool dark = IsDarkAppTheme();
    const DWORD value = dark ? 1 : 0;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                      KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"AppsUseLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegSetValueExW(key, L"SystemUsesLightTheme", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(key);
        SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"ImmersiveColorSet"),
                            SMTO_ABORTIFHUNG, 500, nullptr);
    }
}
namespace {

struct Bstr {
    BSTR p = nullptr;
    explicit Bstr(const wchar_t* s) : p(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(p); }
    operator BSTR() const { return p; }
};

bool WmiServices(ComPtr<IWbemServices>& services) {
    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWbemLocator),
                                reinterpret_cast<void**>(locator.put())))) {
        return false;
    }
    Bstr ns(L"ROOT\\WMI");
    if (FAILED(locator->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, services.put()))) return false;
    CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                      RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    return true;
}

bool QueryFirst(IWbemServices* services, const wchar_t* wql, ComPtr<IWbemClassObject>& object) {
    Bstr language(L"WQL");
    Bstr query(wql);
    ComPtr<IEnumWbemClassObject> enumerator;
    if (FAILED(services->ExecQuery(language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                   enumerator.put()))) {
        return false;
    }
    ULONG returned = 0;
    IWbemClassObject* raw = nullptr;
    if (enumerator->Next(WBEM_INFINITE, 1, &raw, &returned) != S_OK || returned == 0) return false;
    object.attach(raw);
    return true;
}

}  // namespace

namespace {

constexpr DWORD kVcpBrightness = 0x10;

bool DdcGetBrightness(int& percent) {
    HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!monitor) return false;
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || count == 0) return false;
    std::vector<PHYSICAL_MONITOR> monitors(count);
    if (!GetPhysicalMonitorsFromHMONITOR(monitor, count, monitors.data())) return false;
    bool found = false;
    for (const PHYSICAL_MONITOR& physical : monitors) {
        DWORD current = 0, maximum = 0;
        MC_VCP_CODE_TYPE code = MC_SET_PARAMETER;
        if (GetVCPFeatureAndVCPFeatureReply(physical.hPhysicalMonitor, kVcpBrightness, &code, &current, &maximum) &&
            maximum > 0) {
            percent = static_cast<int>(std::lround(static_cast<double>(current) * 100.0 / maximum));
            found = true;
            break;
        }
    }
    DestroyPhysicalMonitors(count, monitors.data());
    return found;
}

bool DdcSetBrightness(int percent) {
    HMONITOR monitor = MonitorFromWindow(GetDesktopWindow(), MONITOR_DEFAULTTOPRIMARY);
    if (!monitor) return false;
    DWORD count = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) || count == 0) return false;
    std::vector<PHYSICAL_MONITOR> monitors(count);
    if (!GetPhysicalMonitorsFromHMONITOR(monitor, count, monitors.data())) return false;
    bool changed = false;
    for (const PHYSICAL_MONITOR& physical : monitors) {
        DWORD current = 0, maximum = 0;
        MC_VCP_CODE_TYPE code = MC_SET_PARAMETER;
        if (GetVCPFeatureAndVCPFeatureReply(physical.hPhysicalMonitor, kVcpBrightness, &code, &current, &maximum) &&
            maximum > 0) {
            const DWORD target = static_cast<DWORD>(
                std::lround(std::clamp(percent, 0, 100) * static_cast<double>(maximum) / 100.0));
            if (SetVCPFeature(physical.hPhysicalMonitor, kVcpBrightness, target)) changed = true;
            break;
        }
    }
    DestroyPhysicalMonitors(count, monitors.data());
    return changed;
}

}  // namespace

bool GetDisplayBrightness(int& percent) {
    if (DdcGetBrightness(percent)) return true;
    ComPtr<IWbemServices> services;
    if (!WmiServices(services)) return false;
    ComPtr<IWbemClassObject> object;
    if (!QueryFirst(services.get(), L"SELECT CurrentBrightness FROM WmiMonitorBrightness", object)) return false;
    VARIANT value;
    VariantInit(&value);
    if (FAILED(object->Get(L"CurrentBrightness", 0, &value, nullptr, nullptr)) || value.vt != VT_UI1) {
        VariantClear(&value);
        return false;
    }
    percent = value.bVal;
    VariantClear(&value);
    return true;
}

bool SetDisplayBrightness(int percent) {
    if (DdcSetBrightness(percent)) return true;
    ComPtr<IWbemServices> services;
    if (!WmiServices(services)) return false;
    ComPtr<IWbemClassObject> object;
    if (!QueryFirst(services.get(), L"SELECT * FROM WmiMonitorBrightnessMethods", object)) return false;
    VARIANT path;
    VariantInit(&path);
    if (FAILED(object->Get(L"__RELPATH", 0, &path, nullptr, nullptr)) || path.vt != VT_BSTR) {
        VariantClear(&path);
        return false;
    }
    ComPtr<IWbemClassObject> inSig, inInst;
    if (FAILED(object->GetMethod(L"WmiSetBrightness", 0, inSig.put(), nullptr)) || !inSig) {
        VariantClear(&path);
        return false;
    }
    if (FAILED(inSig->SpawnInstance(0, inInst.put()))) {
        VariantClear(&path);
        return false;
    }
    VARIANT timeout;
    VariantInit(&timeout);
    timeout.vt = VT_UI4;
    timeout.ulVal = 0;
    inInst->Put(L"Timeout", 0, &timeout, 0);
    VARIANT level;
    VariantInit(&level);
    level.vt = VT_UI1;
    level.bVal = static_cast<BYTE>(std::clamp(percent, 0, 100));
    inInst->Put(L"Brightness", 0, &level, 0);
    ComPtr<IWbemClassObject> outParams;
    Bstr method(L"WmiSetBrightness");
    const HRESULT hr =
        services->ExecMethod(path.bstrVal, method, 0, nullptr, inInst.get(), outParams.put(), nullptr);
    VariantClear(&path);
    return SUCCEEDED(hr);
}

bool ActivateOrLaunchExecutable(const std::wstring& exePath) {
    if (exePath.empty()) return false;
    const std::wstring wanted = Lower(exePath);
    struct Ctx {
        const std::wstring* wanted;
        HWND window = nullptr;
        DWORD pid = 0;
    } ctx{&wanted};
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W entry{sizeof(entry)};
        for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
            if (!process) continue;
            wchar_t path[MAX_PATH * 2]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size) && Lower(path) == wanted) {
                ctx.pid = entry.th32ProcessID;
            }
            CloseHandle(process);
            if (ctx.pid) break;
        }
        CloseHandle(snapshot);
    }
    if (ctx.pid) {
        EnumWindows(
            [](HWND hwnd, LPARAM lp) -> BOOL {
                auto* c = reinterpret_cast<Ctx*>(lp);
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != c->pid || !IsWindowVisible(hwnd)) return TRUE;
                wchar_t cls[128]{};
                GetClassNameW(hwnd, cls, static_cast<int>(std::size(cls)));
                if (wcscmp(cls, L"Progman") == 0 || wcscmp(cls, L"WorkerW") == 0 ||
                    wcscmp(cls, L"Shell_TrayWnd") == 0)
                    return TRUE;
                c->window = hwnd;
                return FALSE;
            },
            reinterpret_cast<LPARAM>(&ctx));
        if (ctx.window) {
            if (IsIconic(ctx.window)) ShowWindowAsync(ctx.window, SW_RESTORE);
            AllowSetForegroundWindow(ctx.pid);
            SetForegroundWindow(ctx.window);
            return true;
        }
    }
    if (GetFileAttributesW(exePath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const std::wstring dir = exePath.substr(0, exePath.find_last_of(L"\\/") + 1);
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, dir.c_str(),
                                                  SW_SHOWNORMAL)) > 32;
}

void SetMasterVolume(int percent) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) return;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.put())))) {
        return;
    }
    volume->SetMasterVolumeLevelScalar(std::clamp(percent, 0, 100) / 100.0f, nullptr);
    if (volume->GetMute(nullptr) == S_OK) volume->SetMute(FALSE, nullptr);
}
void SetMasterMuted(bool muted) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(kClsidMMDeviceEnumerator, nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(enumerator.put())))) {
        return;
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put()))) return;
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(volume.put())))) {
        return;
    }
    volume->SetMute(muted ? TRUE : FALSE, nullptr);
}

}  // namespace sm
