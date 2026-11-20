#pragma once

#include "common.h"

#include <string>
#include <vector>

namespace sm {

struct Settings {
    int version = 1;
    std::wstring language = L"zh-CN";
    std::wstring theme = L"auto";
    std::wstring logoStyle = L"apple";  // "apple" | "windows"

    double barHeight = 28.0;
    int monitor = 0;
    bool reserveWorkArea = true;
    bool autoHide = false;
    bool hideInFullscreen = true;
    bool showSeconds = false;
    bool use24Hour = true;
    bool showDate = true;
    bool showAppName = true;
    bool showAppleMenu = true;
    bool showWifi = true;
    bool showBattery = true;
    bool showBatteryPercent = false;
    bool showVolume = false;
    bool showControlCenter = true;
    bool showSearch = true;
    bool showNotifications = false;
    // Gap between right-side items (status block / search / clock / tray icons).
    int rightIconSpacing = 12;
    // Per-icon overrides for the tray mirror. Record IDs are decimal and stored
    // as semicolon-separated strings in the JSON contract.
    std::wstring trayShow;
    std::wstring trayHide;
    std::vector<unsigned long long> trayShowIds;
    std::vector<unsigned long long> trayHideIds;
    double fontScale = 1.0;

    std::wstring glassPreset = L"liquid";
    double transparency = 0.94;
    double blur = 0.52;
    double refraction = 18.0;
    double edge = 18.0;
    double dispersion = 0.30;
    double highlight = 0.80;
    double shadow = 0.35;
    std::wstring tint = L"#FFFFFF";
    double noise = 0.015;
    double saturation = 1.15;

    bool launchAtLogin = false;
    bool menuAnimations = true;
};

Settings DefaultSettings();
std::wstring SettingsPath();
std::wstring SettingsDirectory();

// Reads the JSON settings file over the supplied defaults. Missing or invalid
// keys keep the default. Returns false if the file could not be parsed at all.
bool LoadSettings(Settings& settings);

// Creates the settings file with defaults if it does not exist. Returns true if
// a file was written.
bool EnsureSettingsFile();

// Monotonic-ish stamp for change detection (last write time + size).
uint64_t SettingsStamp();

// RGB from "#RRGGBB"; falls back to white.
void ParseHexColor(const std::wstring& hex, float& r, float& g, float& b);

// Values uploaded to the glass shader for a surface at `scale` DPI scale.
struct MaterialParams {
    float tintMix = 0.06f;
    float blurMix = 1.0f;
    float blurLod = 3.3f;
    float refract = 16.0f;
    float edge = 12.0f;
    float dispersion = 0.30f;
    float highlight = 0.80f;
    float shadow = 0.35f;
    float saturation = 1.15f;
    float noise = 0.015f;
    float tintR = 1.0f, tintG = 1.0f, tintB = 1.0f;
};
MaterialParams ComputeMaterial(const Settings& s, double scale, double shapeHeightDip, bool popup);

bool IsDarkAppTheme();

}  // namespace sm
