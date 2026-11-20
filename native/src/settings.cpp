#include "settings.h"

#include "json.h"

#include <winreg.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace sm {
namespace {

std::wstring GetString(const json::Object& o, const char* key, const std::wstring& def) {
    auto it = o.find(key);
    if (it == o.end() || !it->second.IsString()) return def;
    return json::Utf8ToWide(it->second.string);
}

double Get(const json::Object& o, const char* key, double def, double lo, double hi) {
    auto it = o.find(key);
    if (it == o.end() || !it->second.IsNumber() || !std::isfinite(it->second.number)) return def;
    return std::clamp(it->second.number, lo, hi);
}

bool Get(const json::Object& o, const char* key, bool def) {
    auto it = o.find(key);
    if (it == o.end() || !it->second.IsBool()) return def;
    return it->second.boolean;
}

int Get(const json::Object& o, const char* key, int def, int lo, int hi) {
    auto it = o.find(key);
    if (it == o.end() || !it->second.IsNumber() || !std::isfinite(it->second.number)) return def;
    return std::clamp(static_cast<int>(std::lround(it->second.number)), lo, hi);
}

std::vector<unsigned long long> ParseTrayIdList(const std::wstring& text) {
    std::vector<unsigned long long> ids;
    size_t i = 0;
    while (i < text.size() && ids.size() < 4096) {
        while (i < text.size() && !iswdigit(text[i])) ++i;
        if (i >= text.size()) break;
        wchar_t* end = nullptr;
        const unsigned long long id = std::wcstoull(text.c_str() + i, &end, 10);
        if (end && end != text.c_str() + i) {
            if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            i = static_cast<size_t>(end - text.c_str());
        } else {
            ++i;
        }
    }
    return ids;
}

std::wstring FormatNum(double v, int decimals = 4) {
    wchar_t buf[64];
    swprintf(buf, std::size(buf), L"%.*f", decimals, v);
    return buf;
}

}  // namespace

Settings DefaultSettings() { return Settings{}; }

std::wstring SettingsDirectory() {
    std::wstring dir = LocalAppData() + L"\\SuperMenu";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring SettingsPath() { return SettingsDirectory() + L"\\settings.json"; }

bool LoadSettings(Settings& out) {
    out = DefaultSettings();
    HANDLE f = CreateFileW(SettingsPath().c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(f, &size) || size.QuadPart <= 0 || size.QuadPart > 1024 * 1024) {
        CloseHandle(f);
        return false;
    }
    std::string text(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(f, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(f);
    if (!ok) return false;
    text.resize(read);

    json::Object obj;
    if (!json::ParseObject(text, obj)) {
        Log(L"settings: malformed JSON, keeping defaults");
        return false;
    }

    Settings s;
    s.version = Get(obj, "version", 1, 1, 1);
    s.language = GetString(obj, "language", L"zh-CN");
    if (s.language != L"zh-CN" && s.language != L"en-US") s.language = L"zh-CN";
    s.theme = GetString(obj, "theme", L"auto");
    if (s.theme != L"auto" && s.theme != L"light" && s.theme != L"dark") s.theme = L"auto";
    s.logoStyle = GetString(obj, "logoStyle", L"apple");
    if (s.logoStyle != L"apple" && s.logoStyle != L"windows" && s.logoStyle != L"windows-mono") s.logoStyle = L"apple";

    s.barHeight = Get(obj, "barHeight", 28.0, 24.0, 44.0);
    s.monitor = Get(obj, "monitor", 0, 0, 15);
    s.reserveWorkArea = Get(obj, "reserveWorkArea", true);
    s.autoHide = Get(obj, "autoHide", false);
    s.hideInFullscreen = Get(obj, "hideInFullscreen", true);
    s.showSeconds = Get(obj, "showSeconds", false);
    s.use24Hour = Get(obj, "use24Hour", true);
    s.showDate = Get(obj, "showDate", true);
    s.showAppName = Get(obj, "showAppName", true);
    s.showAppleMenu = Get(obj, "showAppleMenu", true);
    s.showWifi = Get(obj, "showWifi", true);
    s.showBattery = Get(obj, "showBattery", true);
    s.showBatteryPercent = Get(obj, "showBatteryPercent", false);
    s.showVolume = Get(obj, "showVolume", false);
    s.showControlCenter = Get(obj, "showControlCenter", true);
    s.showSearch = Get(obj, "showSearch", true);
    s.showNotifications = Get(obj, "showNotifications", false);
    s.rightIconSpacing = Get(obj, "rightIconSpacing", 12, 2, 24);
    s.trayShow = GetString(obj, "trayShow", L"");
    s.trayHide = GetString(obj, "trayHide", L"");
    s.trayShowIds = ParseTrayIdList(s.trayShow);
    s.trayHideIds = ParseTrayIdList(s.trayHide);
    s.fontScale = Get(obj, "fontScale", 1.0, 0.85, 1.25);

    s.glassPreset = GetString(obj, "glassPreset", L"liquid");
    if (s.glassPreset != L"liquid" && s.glassPreset != L"frosted" && s.glassPreset != L"clear") s.glassPreset = L"liquid";
    s.transparency = Get(obj, "transparency", 0.94, 0.0, 1.0);
    s.blur = Get(obj, "blur", 0.52, 0.0, 1.0);
    s.refraction = Get(obj, "refraction", 18.0, 0.0, 40.0);
    s.edge = Get(obj, "edge", 18.0, 0.0, 32.0);
    s.dispersion = Get(obj, "dispersion", 0.30, 0.0, 0.95);
    s.highlight = Get(obj, "highlight", 0.80, 0.0, 1.3);
    s.shadow = Get(obj, "shadow", 0.35, 0.0, 1.0);
    s.tint = GetString(obj, "tint", L"#FFFFFF");
    s.noise = Get(obj, "noise", 0.015, 0.0, 0.1);
    s.saturation = Get(obj, "saturation", 1.15, 0.5, 2.0);

    s.launchAtLogin = Get(obj, "launchAtLogin", false);
    s.menuAnimations = Get(obj, "menuAnimations", true);

    out = s;
    return true;
}

bool EnsureSettingsFile() {
    if (GetFileAttributesW(SettingsPath().c_str()) != INVALID_FILE_ATTRIBUTES) return false;
    const Settings s = DefaultSettings();
    struct Key {
        const char* name;
        std::wstring str;
        double num;
        bool boolean;
        enum { Str, Num, Bool } type;
    };
    wchar_t buf[256];
    auto num = [&](double v) {
        swprintf(buf, std::size(buf), L"%.4f", v);
        return std::wstring(buf);
    };
    std::wstring text;
    text += L"{\n";
    auto line = [&](const char* k, const std::wstring& v, bool comma = true) {
        text += L"  \"";
        text += json::Utf8ToWide(k);
        text += L"\": ";
        text += v;
        text += comma ? L",\n" : L"\n";
    };
    line("version", L"1");
    line("language", json::Utf8ToWide(json::Quote("zh-CN")));
    line("theme", json::Utf8ToWide(json::Quote("auto")));
    line("logoStyle", json::Utf8ToWide(json::Quote("apple")));
    line("barHeight", num(28.0));
    line("monitor", L"0");
    line("reserveWorkArea", L"true");
    line("autoHide", L"false");
    line("hideInFullscreen", L"true");
    line("showSeconds", L"false");
    line("use24Hour", L"true");
    line("showDate", L"true");
    line("showAppName", L"true");
    line("showAppleMenu", L"true");
    line("showWifi", L"true");
    line("showBattery", L"true");
    line("showBatteryPercent", L"false");
    line("showVolume", L"false");
    line("showControlCenter", L"true");
    line("showSearch", L"true");
    line("showNotifications", L"false");
    line("rightIconSpacing", L"12");
    line("trayShow", json::Utf8ToWide(json::Quote("")));
    line("trayHide", json::Utf8ToWide(json::Quote("")));
    line("fontScale", num(1.0));
    line("glassPreset", json::Utf8ToWide(json::Quote("liquid")));
    line("transparency", num(0.94));
    line("blur", num(0.52));
    line("refraction", num(18.0));
    line("edge", num(18.0));
    line("dispersion", num(0.30));
    line("highlight", num(0.80));
    line("shadow", num(0.35));
    line("tint", json::Utf8ToWide(json::Quote("#FFFFFF")));
    line("noise", num(0.015));
    line("saturation", num(1.15));
    line("launchAtLogin", L"false");
    line("menuAnimations", L"true", false);
    text += L"}\n";

    const std::wstring path = SettingsPath();
    const std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const std::string utf8 = json::WideToUtf8(text);
    DWORD written = 0;
    WriteFile(f, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    CloseHandle(f);
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

uint64_t SettingsStamp() {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard, &d)) return 0;
    uint64_t stamp = (static_cast<uint64_t>(d.ftLastWriteTime.dwHighDateTime) << 32) | d.ftLastWriteTime.dwLowDateTime;
    stamp ^= (static_cast<uint64_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    return stamp;
}

void ParseHexColor(const std::wstring& hex, float& r, float& g, float& b) {
    unsigned v = 0xFFFFFF;
    if (hex.size() >= 7 && hex[0] == L'#') {
        wchar_t* end = nullptr;
        unsigned long parsed = wcstoul(hex.c_str() + 1, &end, 16);
        if (end && end != hex.c_str() + 1) v = static_cast<unsigned>(parsed) & 0xFFFFFFu;
    }
    r = float((v >> 16) & 0xFF) / 255.0f;
    g = float((v >> 8) & 0xFF) / 255.0f;
    b = float(v & 0xFF) / 255.0f;
}

MaterialParams ComputeMaterial(const Settings& s, double scale, double shapeHeightDip, bool popup) {
    MaterialParams m;
    // Presets are material presets, not only UI labels: they must visibly
    // change every glass surface (bar, menus and the containerless Control
    // Center) even when the individual sliders are left at their defaults.
    double transparency = s.transparency;
    double blur = s.blur;
    double refraction = s.refraction;
    double highlight = s.highlight;
    if (s.glassPreset == L"frosted") {
        blur = std::max(blur, 0.72);
        refraction *= 0.55;
        transparency = std::clamp(transparency - 0.10, 0.0, 1.0);
        highlight *= 0.85;
    } else if (s.glassPreset == L"clear") {
        blur *= 0.38;
        refraction *= 1.12;
        transparency = std::clamp(transparency + 0.06, 0.0, 1.0);
        highlight *= 0.92;
    }
    m.tintMix = float(std::clamp(1.0 - transparency, 0.0, 1.0));
    m.blurMix = float(std::clamp(blur * 4.0, 0.0, 1.0));
    m.blurLod = float(1.0 + 4.5 * blur);
    const double heightPx = std::max(12.0, shapeHeightDip * scale);
    if (popup) {
        m.refract = float(std::min(refraction, 22.0) * scale);
        m.edge = float(std::clamp(s.edge, 1.0, 26.0) * scale);
    } else {
        m.refract = float(std::min(refraction, 0.6 * shapeHeightDip) * scale);
        m.edge = float(std::clamp(s.edge, 1.0, 0.45 * shapeHeightDip) * scale);
    }
    (void)heightPx;
    m.dispersion = float(std::clamp(s.dispersion, 0.0, 0.95));
    m.highlight = float(std::clamp(highlight, 0.0, 1.3));
    m.shadow = float(std::clamp(s.shadow, 0.0, 1.0));
    m.saturation = float(std::clamp(s.saturation, 0.5, 2.0));
    m.noise = float(std::clamp(s.noise, 0.0, 0.1));
    ParseHexColor(s.tint, m.tintR, m.tintG, m.tintB);
    // Theme gives the glass its overall cast without overriding backdrop-based
    // ink selection: light adds a breath of white, dark a breath of ink-blue.
    if (s.theme == L"light") {
        m.tintR = m.tintR * 0.5f + 0.5f;
        m.tintG = m.tintG * 0.5f + 0.5f;
        m.tintB = m.tintB * 0.5f + 0.5f;
        m.tintMix = std::min(0.55f, m.tintMix + 0.05f);
    } else if (s.theme == L"dark") {
        const float r = 0.055f, g = 0.06f, b = 0.075f;
        m.tintR = m.tintR * 0.35f + r * 0.65f;
        m.tintG = m.tintG * 0.35f + g * 0.65f;
        m.tintB = m.tintB * 0.35f + b * 0.65f;
        m.tintMix = std::min(0.55f, m.tintMix + 0.12f);
    }
    return m;
}

bool IsDarkAppTheme() {
    DWORD light = 1, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) == ERROR_SUCCESS) {
        return light == 0;
    }
    return false;
}

}  // namespace sm
