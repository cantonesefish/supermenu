#include "bar_window.h"

#include "atrium/shell/shell_taskbar.h"

#include "fluent_glyphs.h"

#include <commctrl.h>

#include <algorithm>
#include <cmath>

namespace sm {
namespace {
constexpr wchar_t kBarClass[] = L"SuperMenuBarWindow";
constexpr UINT kAppBarCallback = WM_APP + 22;

bool IsOpenMenu(MenuId id, MenuId open) { return open != MenuId::None && id == open; }

std::wstring Ellipsize(Ui* ui, const std::wstring& text, float px, DWRITE_FONT_WEIGHT weight, float maxWidth) {
    if (ui->Measure(text, px, weight) <= maxWidth) return text;
    std::wstring out = text;
    while (!out.empty()) {
        out.resize(out.size() - 1);
        std::wstring probe = out + L"…";
        if (ui->Measure(probe, px, weight) <= maxWidth) return probe;
    }
    return L"…";
}

// Native Windows status glyphs (Segoe Fluent Icons / Segoe MDL2 Assets).
wchar_t BatteryGlyph(int percent, bool charging) {
    static const wchar_t kNormal[11] = {0xE850, 0xE851, 0xE852, 0xE853, 0xE854, 0xE855,
                                        0xE856, 0xE857, 0xE858, 0xE859, 0xE83F};
    static const wchar_t kCharging[11] = {0xE85A, 0xE85B, 0xE85C, 0xE85D, 0xE85E, 0xE85F,
                                          0xE860, 0xE861, 0xE862, 0xE83E, 0xEA93};
    const int level = std::clamp(static_cast<int>(std::lround(std::clamp(percent, 0, 100) / 10.0)), 0, 10);
    return charging ? kCharging[level] : kNormal[level];
}

wchar_t ChargingGlyphFrame(int frame) {
    static const wchar_t kChargingFrames[11] = {0xE85A, 0xE85B, 0xE85C, 0xE85D, 0xE85E, 0xE85F,
                                                0xE860, 0xE861, 0xE862, 0xE83E, 0xEA93};
    const int count = static_cast<int>(std::size(kChargingFrames));
    return kChargingFrames[((frame % count) + count) % count];
}

wchar_t VolumeGlyph(int volume, bool muted) {
    if (muted) return 0xE74F;  // Mute
    if (volume <= 0) return 0xE992;  // Volume0
    if (volume < 34) return 0xE993;  // Volume1
    if (volume < 67) return 0xE994;  // Volume2
    return 0xE995;                   // Volume3
}

wchar_t NetworkGlyph(NetworkKind kind) {
    switch (kind) {
        case NetworkKind::Ethernet: return 0xE839;  // Ethernet
        case NetworkKind::Wifi: return 0xE701;      // Wifi
        default: return 0xE968;                     // Network (no connection)
    }
}

}  // namespace

BarWindow::~BarWindow() {
    Destroy();
    delete icons_;
    delete ui_;
}

void BarWindow::Destroy() {
    RegisterAppBar(false);
    if (tooltip_) {
        DestroyWindow(tooltip_);
        tooltip_ = nullptr;
    }
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    surface_.reset();
}

bool BarWindow::Create(HINSTANCE hinst, Gpu* gpu, Capture* capture, BarHost* host, const MonitorInfo& monitor) {
    hinst_ = hinst;
    gpu_ = gpu;
    capture_ = capture;
    host_ = host;
    monitor_ = monitor;
    ui_ = new Ui(gpu);
    icons_ = new Icons(gpu, ui_);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kBarClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        LogHr(L"RegisterClassExW(bar)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }

    const Settings& s = host_->settings();
    const double scale = gpu_->scale();
    barHeight_ = static_cast<UINT>(std::max(16, Px(s.barHeight, scale)));
    shadowPad_ = Px(16.0, scale);
    const int w = static_cast<int>(monitor_.rect.right - monitor_.rect.left);
    const int h = static_cast<int>(barHeight_) + shadowPad_;
    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                            kBarClass, L"SuperMenu", WS_POPUP, monitor_.rect.left, monitor_.rect.top, w, h,
                            nullptr, nullptr, hinst, this);
    if (!hwnd_) {
        LogHr(L"CreateWindowExW(bar)", HRESULT_FROM_WIN32(GetLastError()));
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ExcludeWindowFromCapture(hwnd_);

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, 0, 0, 0, 0, hwnd_, nullptr, hinst_,
                               nullptr);
    if (tooltip_) {
        SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, static_cast<LPARAM>(Px(320, scale)));
        SendMessageW(tooltip_, TTM_SETDELAYTIME, TTDT_INITIAL, 260);
        SendMessageW(tooltip_, TTM_SETTIPBKCOLOR, RGB(34, 34, 38), 0);
        SendMessageW(tooltip_, TTM_SETTIPTEXTCOLOR, RGB(245, 245, 247), 0);
        TOOLINFOW info{};
        info.cbSize = sizeof(info);
        info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
        info.hwnd = hwnd_;
        info.uId = 1;
        info.lpszText = const_cast<LPWSTR>(L"");
        SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
    }

    RECT wr = windowScreenRect();
    surface_ = std::make_unique<GlassSurface>();
    if (!surface_->Initialize(gpu_, hwnd_, wr, Px(36.0, scale), Pxf(14.0, scale), L"bar")) {
        Log(L"bar: glass surface failed to initialize");
        return false;
    }
    PositionWindow();
    Layout(static_cast<float>(surface_->width()), static_cast<float>(barHeight_), scale);
    RegisterAppBar(s.reserveWorkArea && !s.autoHide);
    contentDirty_ = true;
    lastFrameMs_ = NowMs();
    Log(L"bar: created at %ld,%ld %dx%d (content %u + shadow %d)", monitor_.rect.left, monitor_.rect.top, w, h,
        barHeight_, shadowPad_);
    return true;
}

RECT BarWindow::windowScreenRect() const {
    RECT wr{};
    if (hwnd_) GetWindowRect(hwnd_, &wr);
    else {
        wr.left = monitor_.rect.left;
        wr.top = monitor_.rect.top;
        wr.right = monitor_.rect.right;
        wr.bottom = monitor_.rect.top + static_cast<LONG>(barHeight_) + shadowPad_;
    }
    return wr;
}

void BarWindow::PositionWindow() {
    if (!hwnd_) return;
    const int w = static_cast<int>(monitor_.rect.right - monitor_.rect.left);
    const int h = static_cast<int>(barHeight_) + shadowPad_;
    SetWindowPos(hwnd_, HWND_TOPMOST, monitor_.rect.left, monitor_.rect.top, w, h, SWP_NOACTIVATE);
}

void BarWindow::RegisterAppBar(bool reserve) {
    if (!hwnd_) return;
    if (!reserve) {
        if (appBarRegistered_) {
            APPBARDATA abd{};
            abd.cbSize = sizeof(abd);
            abd.hWnd = hwnd_;
            atrium::AppBarMessage(ABM_REMOVE, abd);
            appBarRegistered_ = false;
            PositionWindow();
        }
        return;
    }
    if (appBarRegistered_) {
        // Update the reserved height if the bar height changed.
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = hwnd_;
        abd.uEdge = ABE_TOP;
        abd.rc.left = monitor_.rect.left;
        abd.rc.top = monitor_.rect.top;
        abd.rc.right = monitor_.rect.right;
        abd.rc.bottom = monitor_.rect.top + static_cast<LONG>(barHeight_);
        atrium::AppBarMessage(ABM_QUERYPOS, abd);
        abd.rc.bottom = abd.rc.top + static_cast<LONG>(barHeight_);
        atrium::AppBarMessage(ABM_SETPOS, abd);
        PositionWindow();
        return;
    }
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = hwnd_;
    abd.uCallbackMessage = kAppBarCallback;
    if (!atrium::AppBarMessage(ABM_NEW, abd)) {
        Log(L"bar: appbar registration failed");
        return;
    }
    appBarRegistered_ = true;
    abd.uEdge = ABE_TOP;
    abd.rc.left = monitor_.rect.left;
    abd.rc.top = monitor_.rect.top;
    abd.rc.right = monitor_.rect.right;
    abd.rc.bottom = monitor_.rect.top + static_cast<LONG>(barHeight_);
    atrium::AppBarMessage(ABM_QUERYPOS, abd);
    abd.rc.bottom = abd.rc.top + static_cast<LONG>(barHeight_);
    atrium::AppBarMessage(ABM_SETPOS, abd);
    PositionWindow();
    Log(L"bar: appbar reserved top %ld px", static_cast<LONG>(barHeight_));
}

void BarWindow::ApplySettings() {
    if (!hwnd_ || !surface_) return;
    const Settings& s = host_->settings();
    const double scale = gpu_->scale();
    const UINT newH = static_cast<UINT>(std::max(16, Px(s.barHeight, scale)));
    const int newShadow = Px(16.0, scale);
    if (newH != barHeight_ || newShadow != shadowPad_) {
        barHeight_ = newH;
        shadowPad_ = newShadow;
        surface_->ResizeBuffers(static_cast<int>(monitor_.rect.right - monitor_.rect.left),
                                static_cast<int>(barHeight_) + shadowPad_);
        RegisterAppBar(s.reserveWorkArea && !s.autoHide);
        PositionWindow();
    } else {
        RegisterAppBar(s.reserveWorkArea && !s.autoHide);
    }
    surface_->SetScreenRect(windowScreenRect());
    contentDirty_ = true;
    Log(L"bar: settings applied (height %u)", barHeight_);
}

void BarWindow::Show(bool show) {
    if (!hwnd_) return;
    ShowWindow(hwnd_, show ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (show) {
        PositionWindow();
        contentDirty_ = true;
    }
}

bool BarWindow::UpdateBackdrop(bool updateLuma) {
    if (!surface_ || !capture_) return false;
    return surface_->UpdateBackdrop(capture_->Texture(), updateLuma);
}

void BarWindow::UpdateInk(double dt) {
    if (!surface_ || !surface_->hasLuma()) return;
    auto targetFor = [&](float luma, bool& initialized, float& ink) {
        float t = ink;
        if (luma > 0.52f) t = 1.0f;
        else if (luma < 0.43f) t = 0.0f;
        if (!initialized) {
            ink = t;
            initialized = true;
        } else {
            ink = Approach(ink, t, 4.0f, static_cast<float>(dt));
        }
    };
    const float scale = static_cast<float>(gpu_->scale());
    float leftEnd = 1.0f;
    float rightStart = 1.0f;
    for (const auto& item : items_) {
        if (!item.status && item.plate.right > leftEnd) leftEnd = item.plate.right;
        if (item.status && item.plate.left < rightStart) rightStart = item.plate.left;
    }
    const MaterialParams m = ComputeMaterial(host_->settings(), gpu_->scale(), host_->settings().barHeight, false);
    auto effective = [&](float luma) {
        return luma * (1.0f - m.tintMix) + (m.tintR * 0.2126f + m.tintG * 0.7152f + m.tintB * 0.0722f) * m.tintMix;
    };
    const float left = effective(surface_->RegionLuma(0, 0, leftEnd, static_cast<float>(barHeight_)));
    const float right = effective(surface_->RegionLuma(rightStart, 0, static_cast<float>(surface_->width()),
                                                       static_cast<float>(barHeight_)));
    targetFor(left, inkLeftInit_, inkLeft_);
    targetFor(right, inkRightInit_, inkRight_);
    inkClock_ = inkRight_;
    (void)scale;
}

GlassParams BarWindow::MakeParams() const {
    const Settings& s = host_->settings();
    MaterialParams m = ComputeMaterial(s, gpu_->scale(), s.barHeight, false);
    GlassParams p{};
    p.radius = 0.0f;
    p.edge = m.edge;
    p.refract = m.refract;
    p.dispersion = m.dispersion;
    p.blurMix = m.blurMix;
    p.blurLod = std::min(m.blurLod, 3.5f);
    p.tintMix = m.tintMix;
    p.tintR = m.tintR;
    p.tintG = m.tintG;
    p.tintB = m.tintB;
    p.tintA = 1.0f;
    p.highlight = m.highlight;
    p.shadow = m.shadow;
    p.lightX = 0.0f;
    p.lightY = -1.0f;
    p.inset = Pxf(14.0, gpu_->scale());
    p.noise = m.noise;
    p.vibrancy = m.saturation;
    p.shapeL = -2.0f;
    p.shapeT = -2.0f;
    p.shapeR = static_cast<float>(monitor_.rect.right - monitor_.rect.left) + 2.0f;
    p.shapeB = static_cast<float>(barHeight_);
    p.fade = 0.0f;
    return p;
}

void BarWindow::Layout(float width, float barHeight, double scale) {
    // Layout is rebuilt every render frame. Keep the hover animation state so a
    // stationary pointer does not restart the wash from zero and flicker.
    std::map<MenuId, float> oldMenuHover;
    std::map<std::wstring, float> oldTrayHover;
    float oldOverflowHover = 0.0f;
    for (const auto& item : items_) {
        if (item.tray) oldTrayHover[item.trayId] = item.hover;
        else if (item.overflow) oldOverflowHover = item.hover;
        else oldMenuHover[item.id] = item.hover;
    }
    auto restoreHover = [&](BarItem& item) {
        if (item.tray) {
            if (auto it = oldTrayHover.find(item.trayId); it != oldTrayHover.end()) item.hover = it->second;
        } else if (item.overflow) {
            item.hover = oldOverflowHover;
        } else if (auto it = oldMenuHover.find(item.id); it != oldMenuHover.end()) {
            item.hover = it->second;
        }
    };
    items_.clear();
    topMenus_ = BuildTopMenus(host_->menuContext(), host_->settings().reserveWorkArea);
    const float fs = std::max(10.0f, static_cast<float>(13.0 * scale * host_->settings().fontScale));
    const float plateH = std::max(20.0f, barHeight - static_cast<float>(8.0 * scale));
    const float plateY = (barHeight - plateH) * 0.5f;
    const float plateR = std::min(6.0f * static_cast<float>(scale), plateH * 0.35f);
    const float padding = 9.0f * static_cast<float>(scale);
    const float gap = 1.0f * static_cast<float>(scale);
    float x = 10.0f * static_cast<float>(scale);

    // Left: logo and top-level menus. While the Explorer desktop owns focus,
    // keep the glass bar but hide the application menu buttons (File/Edit/...).
    const bool desktopFocused = host_->status().app.desktop;
    for (const auto& menu : topMenus_) {
        if (desktopFocused && menu.id != MenuId::Apple) continue;
        BarItem item;
        item.id = menu.id;
        item.weight = menu.id == MenuId::AppMenu ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM;
        item.textPx = fs;
        item.status = false;
        float w = 0;
        if (menu.id == MenuId::Apple) {
            item.apple = true;
            item.drawsIcon = true;
            item.iconSize = std::min(static_cast<float>(barHeight) * 0.56f, 16.0f * static_cast<float>(scale));
            w = std::max(static_cast<float>(barHeight) * 0.86f, 24.0f * static_cast<float>(scale));
        } else {
            item.label = menu.title.empty() ? L"SuperMenu" : menu.title;
            if (menu.id == MenuId::AppMenu) {
                const float maxW = 210.0f * static_cast<float>(scale);
                item.label = Ellipsize(ui_, item.label, fs, item.weight, maxW - padding * 2);
            }
            w = ui_->Measure(item.label, fs, item.weight) + padding * 2;
        }
        item.plate = D2D1::RectF(x, plateY, x + w, plateY + plateH);
        item.text = D2D1::RectF(item.plate.left + padding, 0, item.plate.right - padding, barHeight);
        items_.push_back(item);
        x += w + gap;
    }
    const float leftEnd = x;

    // Right: a single status block (network + battery + Control Center) followed
    // by search and clock. Clock is rightmost. The inter-item gap is configurable
    // and defaults to a roomier 12 DIP.
    struct Spec {
        MenuId id;
        float width;
        float iconSize;
        bool clock;
        bool cluster;
    };
    std::vector<Spec> specs;
    const Settings& s = host_->settings();
    // Give the clock a little breathing room from the screen edge and from the
    // monitor's rounded corner / notch area.
    const float rightMargin = 24.0f * static_cast<float>(scale);
    const float right = width - rightMargin;
    // Apple-style SF silhouettes are normalized to the same square, which keeps
    // Wi-Fi/Ethernet, battery, Control Center and search visually identical.
    const float icon = 16.0f * static_cast<float>(scale);
    const float hpad = 7.0f * static_cast<float>(scale);
    const float rightGap = std::max(2, s.rightIconSpacing) * static_cast<float>(scale);
    const float clusterGlyph = icon;
    const float clusterPad = 5.0f * static_cast<float>(scale);
    std::wstring clockText;
    if (s.showDate) clockText = FormatDate(host_->status().now, s) + L" " + FormatClock(host_->status().now, s);
    else clockText = FormatClock(host_->status().now, s);
    if (!clockText.empty()) {
        float w = ui_->Measure(clockText, fs, DWRITE_FONT_WEIGHT_MEDIUM) + 16.0f * static_cast<float>(scale);
        specs.push_back({MenuId::Clock, w, 0, true, false});
    }
    if (s.showSearch) specs.push_back({MenuId::Search, icon + hpad * 2, icon, false, false});

    int clusterParts = 0;
    float clusterWidth = clusterPad * 2.0f;
    if (s.showWifi) {
        ++clusterParts;
        clusterWidth += clusterGlyph;
    }
    if (s.showBattery && host_->status().batteryPresent) {
        ++clusterParts;
        clusterWidth += clusterGlyph * 1.28f;
        if (s.showBatteryPercent) {
            const std::wstring pct = std::to_wstring(std::max(0, host_->status().batteryPercent)) + L"%";
            clusterWidth += ui_->Measure(pct, fs * 0.82f, DWRITE_FONT_WEIGHT_MEDIUM) +
                            3.0f * static_cast<float>(scale);
        }
    }
    if (s.showControlCenter) {
        ++clusterParts;
        clusterWidth += clusterGlyph;
    }
    if (clusterParts > 0) {
        clusterWidth += static_cast<float>(std::max(0, clusterParts - 1)) * rightGap;
        specs.push_back({MenuId::ControlCenter, clusterWidth, icon, false, true});
    }
    if (host_->status().inputMethodAvailable) {
        const std::wstring& imeState = host_->status().inputMethodLabel;
        const float stateW = ui_->Measure(imeState, fs * 0.78f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        const float imeWidth = icon + 3.0f * static_cast<float>(scale) + stateW + hpad * 2.0f;
        specs.push_back({MenuId::InputMethod, imeWidth, icon, false, false});
    }
    if (s.showVolume) specs.push_back({MenuId::Volume, icon + hpad * 2, icon, false, false});
    if (s.showNotifications) specs.push_back({MenuId::Notifications, icon + hpad * 2, icon, false, false});
    // Placed after the status cluster in this right-to-left list, so it sits
    // between the Windows tray icons and the Control Center status block.
    if (host_->status().windowsUpdateRestartRequired) {
        specs.push_back({MenuId::WindowsUpdate, icon + hpad * 2, icon, false, false});
    }

    float rx = right;
    for (auto& spec : specs) {
        rx -= spec.width;
        BarItem item;
        item.id = spec.id;
        item.status = true;
        item.statusCluster = spec.cluster;
        item.iconSize = spec.iconSize;
        item.textPx = fs;
        item.weight = DWRITE_FONT_WEIGHT_MEDIUM;
        item.plate = D2D1::RectF(rx, plateY, rx + spec.width, plateY + plateH);
        item.text = D2D1::RectF(item.plate.left, 0, item.plate.right, barHeight);
        if (spec.clock) {
            item.label = clockText;
            // Keep the highlight plate symmetric around the right-aligned
            // date/time instead of leaving all padding on its left.
            item.text = D2D1::RectF(item.plate.left + 8.0f * static_cast<float>(scale), 0,
                                    item.plate.right - 8.0f * static_cast<float>(scale), barHeight);
            item.showBatteryPercent = false;
        }
        if (spec.cluster) {
            item.label = std::to_wstring(std::max(0, host_->status().batteryPercent)) + L"%";
            item.showBatteryPercent = s.showBatteryPercent;

            // Individual hover plates, computed with the same widths and gaps
            // as DrawContent so the wash always sits directly behind the icon.
            const auto& st = host_->status();
            const float hoverPad = std::min(6.0f * static_cast<float>(scale), rightGap * 0.5f);
            float px = item.plate.left + clusterPad;
            auto addPart = [&](float width) {
                const float left = std::max(item.plate.left, px - hoverPad);
                const float right = std::min(item.plate.right, px + width + hoverPad);
                item.hoverParts.push_back(
                    D2D1::RectF(left, item.plate.top, right, item.plate.bottom));
            };
            if (s.showWifi) {
                addPart(clusterGlyph);
                px += clusterGlyph + rightGap;
            }
            if (s.showBattery && st.batteryPresent) {
                const float batteryW = clusterGlyph * 1.28f;
                addPart(batteryW);
                px += batteryW;
                if (s.showBatteryPercent) {
                    const std::wstring pct = std::to_wstring(std::max(0, st.batteryPercent)) + L"%";
                    px += ui_->Measure(pct, fs * 0.82f, DWRITE_FONT_WEIGHT_MEDIUM) +
                          3.0f * static_cast<float>(scale);
                }
                px += rightGap;
            }
            if (s.showControlCenter) addPart(clusterGlyph);
        }
        items_.push_back(item);
        rx -= rightGap;
    }

    // System tray group: visible (promoted) tray icons sit just left of the
    // status icons; a native chevron opens the hidden-icon dropdown.
    const auto& tray = host_->trayIcons();
    std::vector<const TrayIcon*> promoted;
    int hiddenCount = 0;
    for (const auto& icon : tray) {
        if (IsTrayIconShown(icon, s)) promoted.push_back(&icon);
        else ++hiddenCount;
    }
    float cursor = rx;
    const float trayIconSize = 16.0f * static_cast<float>(scale);
    const float trayCell = trayIconSize + 8.0f * static_cast<float>(scale);
    const float trayGap = rightGap;
    for (auto it = promoted.rbegin(); it != promoted.rend(); ++it) {
        const TrayIcon& icon = **it;
        cursor -= trayCell;
        BarItem item;
        item.status = true;
        item.tray = true;
        item.trayId = icon.id;
        item.trayLabel = icon.label;
        item.trayImageId = icon.imageId;
        item.trayW = icon.width;
        item.trayH = icon.height;
        item.iconSize = trayIconSize;
        item.plate = D2D1::RectF(cursor, plateY, cursor + trayCell, plateY + plateH);
        item.text = D2D1::RectF(item.plate.left, 0, item.plate.right, barHeight);
        items_.push_back(item);
        cursor -= trayGap;
    }
    if (hiddenCount > 0) {
        // Windows places the "show hidden icons" chevron to the left of the
        // visible tray icons.
        const float overflowW = 22.0f * static_cast<float>(scale);
        cursor -= overflowW;
        BarItem item;
        item.status = true;
        item.overflow = true;
        item.iconSize = 15.0f * static_cast<float>(scale);
        item.plate = D2D1::RectF(cursor, plateY, cursor + overflowW, plateY + plateH);
        item.text = D2D1::RectF(item.plate.left, 0, item.plate.right, barHeight);
        items_.push_back(item);
        cursor -= trayGap;
    }

    // macOS-style privacy dots live inside the existing right margin, to the
    // right of the date. They do not consume layout width, so the date never
    // moves when the camera or microphone starts/stops being used.
    const auto& status = host_->status();
    const int privacyDots = (status.privacyMicrophone ? 1 : 0) + (status.privacyCamera ? 1 : 0);
    if (privacyDots > 0) {
        const float dot = 7.0f * static_cast<float>(scale);
        const float dotGap = 3.0f * static_cast<float>(scale);
        const float groupW = dot * privacyDots + dotGap * (privacyDots - 1);
        const float left = width - rightMargin + (rightMargin - groupW) * 0.5f;
        BarItem item;
        item.id = MenuId::Privacy;
        item.status = true;
        item.iconSize = dot;
        item.plate = D2D1::RectF(left, plateY, left + groupW, plateY + plateH);
        item.text = D2D1::RectF(item.plate.left, 0, item.plate.right, barHeight);
        items_.push_back(item);
    }

    for (auto& item : items_) restoreHover(item);

    // Keep the vectors ordered left to right for drawing.
    std::sort(items_.begin(), items_.end(), [](const BarItem& a, const BarItem& b) {
        if (a.status != b.status) return !a.status;
        return a.plate.left < b.plate.left;
    });
    (void)leftEnd;
    (void)plateR;
    (void)right;
}

void BarWindow::DrawContent(ID2D1DeviceContext2* d2d) {
    const float barH = static_cast<float>(barHeight_);
    const double scale = gpu_->scale();
    const MenuId open = host_->openMenu();

    const Ink leftInk{inkLeft_};
    const Ink rightInk{inkRight_};
    const Ink clockInk{inkClock_};

    auto fillPlateRect = [&](const D2D1_RECT_F& plate, const BarItem& item, const Ink& ink) {
        const float a = sm::EaseOutCubic(std::clamp(item.hover, 0.0f, 1.0f));
        if (a <= 0.002f) return;
        D2D1_COLOR_F c = ink.wash(IsOpenMenu(item.id, open) ? 0.04f : 0.0f);
        c.a *= a;
        ComPtr<ID2D1SolidColorBrush> brush(ui_->Brush(d2d, c));
        const float radius = std::min(6.0f * static_cast<float>(scale),
                                      (plate.bottom - plate.top) * 0.35f);
        d2d->FillRoundedRectangle(D2D1::RoundedRect(plate, radius, radius), brush.get());
    };

    for (const auto& item : items_) {
        const Ink& ink = item.id == MenuId::Clock ? clockInk : (item.status ? rightInk : leftInk);
        if (item.statusCluster) {
            // Network, battery and Control Center always share one continuous
            // emphasis plate, whether the Control Center is open or not.
            fillPlateRect(item.plate, item, ink);
        } else {
            fillPlateRect(item.plate, item, ink);
        }
        ComPtr<ID2D1SolidColorBrush> brush(ui_->Brush(d2d, ink.text()));
        if (item.apple) {
            const float w = item.plate.right - item.plate.left;
            const float size = item.iconSize;
            if (host_->settings().logoStyle == L"windows" || host_->settings().logoStyle == L"windows-mono") {
                const bool mono = host_->settings().logoStyle == L"windows-mono";
                ComPtr<ID2D1SolidColorBrush> windowsBrush(
                    ui_->Brush(d2d, mono ? ink.text() : D2D1::ColorF(0.0f, 0.47f, 0.83f, 1.0f)));
                const D2D1_RECT_F r{item.plate.left + (w - size) * 0.5f, (barH - size) * 0.5f,
                                    item.plate.left + (w + size) * 0.5f, (barH + size) * 0.5f};
                icons_->Windows(d2d, r, windowsBrush.get());
            } else {
                const float iw = size * 0.78f;
                const D2D1_RECT_F r{item.plate.left + (w - iw) * 0.5f, (barH - size) * 0.5f,
                                    item.plate.left + (w + iw) * 0.5f, (barH + size) * 0.5f};
                icons_->Apple(d2d, r, brush.get());
            }
        } else if (item.tray) {
            DrawTrayIcon(d2d, item, brush.get());
        } else if (item.statusCluster) {
            const Settings& s = host_->settings();
            const auto& status = host_->status();
            const float glyph = item.iconSize;
            const float clusterPad = 5.0f * static_cast<float>(scale);
            const float gap = std::max(2, s.rightIconSpacing) * static_cast<float>(scale);
            float x = item.plate.left + clusterPad;
            int remaining = 0;
            if (s.showWifi) ++remaining;
            if (s.showBattery && status.batteryPresent) ++remaining;
            if (s.showControlCenter) ++remaining;
            auto glyphRect = [&](float left, float size) {
                return D2D1::RectF(left, (barH - size) * 0.5f, left + size, (barH + size) * 0.5f);
            };
            auto advance = [&] {
                --remaining;
                if (remaining > 0) x += gap;
            };
            if (s.showWifi) {
                const Ink networkInk = status.network == NetworkKind::None ? Ink{ink.dark + 0.35f} : ink;
                ComPtr<ID2D1SolidColorBrush> networkBrush(
                    ui_->Brush(d2d, networkInk.text(status.network == NetworkKind::None ? 0.55f : 1.0f)));
                const double networkElapsed = NowMs() - status.networkChangedMs;
                wchar_t networkGlyph = NetworkGlyph(status.network);
                if (status.network == NetworkKind::Wifi && networkElapsed >= 0 && networkElapsed < 900.0) {
                    networkGlyph = (static_cast<int>(networkElapsed / 160.0) % 2) ? fluent::WifiAni2
                                                                                : fluent::WifiAni1;
                } else if (status.network == NetworkKind::Ethernet && networkElapsed >= 0 &&
                           networkElapsed < 900.0) {
                    networkGlyph = fluent::EthernetAni;
                }
                ui_->DrawGlyph(d2d, networkGlyph, glyph, glyphRect(x, glyph), networkBrush.get());
                x += glyph;
                advance();
            }
            if (s.showBattery && status.batteryPresent) {
                // AC power changes the glyph family even when the battery is
                // full and not actively charging: the charging glyph carries
                // the visible lightning/plug mark.
                wchar_t batteryGlyph = BatteryGlyph(status.batteryPercent, status.batteryOnAc);
                if (status.batteryCharging) {
                    batteryGlyph = ChargingGlyphFrame(static_cast<int>(NowMs() / 180.0));
                }
                const float batterySize = glyph * 1.28f;
                const D2D1_RECT_F batteryRect{x + (glyph - batterySize) * 0.5f,
                                              (barH - batterySize) * 0.5f,
                                              x + (glyph + batterySize) * 0.5f,
                                              (barH + batterySize) * 0.5f};
                ui_->DrawGlyph(d2d, batteryGlyph, batterySize, batteryRect, brush.get());
                x += batterySize;
                if (s.showBatteryPercent) {
                    const std::wstring pct = std::to_wstring(std::max(0, status.batteryPercent)) + L"%";
                    const float textPx = item.textPx * 0.82f;
                    const float textW = ui_->Measure(pct, textPx, DWRITE_FONT_WEIGHT_MEDIUM);
                    const D2D1_RECT_F tr{x + 3.0f * static_cast<float>(scale), 0,
                                         x + 3.0f * static_cast<float>(scale) + textW, barH};
                    ui_->DrawTextWithHalo(d2d, pct, textPx, DWRITE_FONT_WEIGHT_MEDIUM, tr, ink,
                                          DWRITE_TEXT_ALIGNMENT_LEADING);
                    x += textW + 3.0f * static_cast<float>(scale);
                }
                advance();
            }
            if (s.showControlCenter) {
                ui_->DrawGlyph(d2d, fluent::ControlCenter, glyph, glyphRect(x, glyph), brush.get());
                x += glyph;
                advance();
            }
        } else if (item.overflow) {
            const float glyph = item.iconSize * 1.25f;
            const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                (barH - glyph) * 0.5f,
                                item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                (barH + glyph) * 0.5f};
            ui_->DrawGlyph(d2d, 0xE70D, glyph, r, brush.get());  // ChevronDown
        } else if (item.status) {
            switch (item.id) {
                case MenuId::Clock:
                    ui_->DrawTextWithHalo(d2d, item.label, item.textPx, DWRITE_FONT_WEIGHT_MEDIUM, item.text,
                                          ink, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    break;
                case MenuId::ControlCenter: {
                    const float glyph = item.iconSize * 1.32f;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    ui_->DrawGlyph(d2d, 0xE9E9, glyph, r, brush.get());  // Equalizer: sliders, Control Center
                    break;
                }
                case MenuId::Battery: {
                    const float glyph = item.iconSize * 1.52f;
                    const D2D1_RECT_F r{item.plate.left + 5.0f * static_cast<float>(scale),
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + 5.0f * static_cast<float>(scale) + glyph,
                                        (barH + glyph) * 0.5f};
                    ui_->DrawGlyph(d2d, BatteryGlyph(host_->status().batteryPercent,
                                                      host_->status().batteryOnAc),
                                   glyph, r, brush.get());
                    if (item.showBatteryPercent) {
                        const D2D1_RECT_F tr{r.right, 0, item.plate.right - 4.0f * static_cast<float>(scale), barH};
                        ui_->DrawTextWithHalo(d2d, item.label, item.textPx * 0.88f, DWRITE_FONT_WEIGHT_MEDIUM, tr,
                                              ink, DWRITE_TEXT_ALIGNMENT_TRAILING);
                    }
                    break;
                }
                case MenuId::Wifi: {
                    const float glyph = item.iconSize * 1.32f;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    const auto& st = host_->status();
                    const Ink wifiInk = st.wifiConnected ? ink : Ink{ink.dark + 0.35f};
                    const double elapsed = NowMs() - st.networkChangedMs;
                    wchar_t wifiGlyph = fluent::Wifi;
                    if (elapsed >= 0 && elapsed < 900.0) {
                        wifiGlyph = (static_cast<int>(elapsed / 160.0) % 2) ? fluent::WifiAni2 : fluent::WifiAni1;
                    }
                    ComPtr<ID2D1SolidColorBrush> wifiBrush(ui_->Brush(d2d, wifiInk.text(0.70f)));
                    ui_->DrawGlyph(d2d, wifiGlyph, glyph, r, wifiBrush.get());
                    break;
                }
                case MenuId::Search: {
                    const float glyph = item.iconSize;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    ui_->DrawGlyph(d2d, fluent::Search, glyph, r, brush.get());
                    break;
                }
                case MenuId::Volume: {
                    const double elapsed = NowMs() - host_->status().volumeChangedMs;
                    const float pulse = elapsed >= 0 && elapsed < 450.0
                                            ? 1.0f + 0.12f * std::sin(static_cast<float>(elapsed) * 0.045f)
                                            : 1.0f;
                    const float glyph = item.iconSize * pulse;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    ui_->DrawGlyph(d2d,
                                   VolumeGlyph(host_->status().volume, host_->status().volumeMuted), glyph, r,
                                   brush.get());
                    break;
                }
                case MenuId::Notifications: {
                    const float glyph = item.iconSize;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    ui_->DrawGlyph(d2d, fluent::Notification, glyph, r, brush.get());
                    break;
                }
                case MenuId::WindowsUpdate: {
                    const float glyph = item.iconSize;
                    const D2D1_RECT_F r{item.plate.left + (item.plate.right - item.plate.left - glyph) * 0.5f,
                                        (barH - glyph) * 0.5f,
                                        item.plate.left + (item.plate.right - item.plate.left + glyph) * 0.5f,
                                        (barH + glyph) * 0.5f};
                    ComPtr<ID2D1SolidColorBrush> updateBrush(ui_->Brush(d2d, AccentColor()));
                    ui_->DrawGlyph(d2d, fluent::WindowsUpdate, glyph, r, updateBrush.get());
                    break;
                }
                case MenuId::Privacy: {
                    const auto& st = host_->status();
                    const float dot = 7.0f * static_cast<float>(scale);
                    const float dotGap = 3.0f * static_cast<float>(scale);
                    int count = (st.privacyMicrophone ? 1 : 0) + (st.privacyCamera ? 1 : 0);
                    float x = (item.plate.left + item.plate.right - (dot * count + dotGap * (count - 1))) * 0.5f;
                    auto drawDot = [&](const D2D1_COLOR_F& color) {
                        ComPtr<ID2D1SolidColorBrush> dotBrush(ui_->Brush(d2d, color));
                        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + dot * 0.5f, barH * 0.5f),
                                                       dot * 0.5f, dot * 0.5f),
                                         dotBrush.get());
                        x += dot + dotGap;
                    };
                    if (st.privacyMicrophone) drawDot(D2D1::ColorF(1.0f, 0.58f, 0.0f));  // Apple orange
                    if (st.privacyCamera) drawDot(D2D1::ColorF(0.20f, 0.78f, 0.35f));    // Apple green
                    break;
                }
                case MenuId::InputMethod: {
                    const auto& st = host_->status();
                    const float glyph = item.iconSize;
                    const float left = item.plate.left + 7.0f * static_cast<float>(scale);
                    const D2D1_RECT_F gr{left, (barH - glyph) * 0.5f, left + glyph,
                                         (barH + glyph) * 0.5f};
                    ComPtr<ID2D1SolidColorBrush> imeBrush(
                        ui_->Brush(d2d, st.inputMethodNative ? AccentColor() : ink.text(0.72f)));
                    ui_->DrawGlyph(d2d, fluent::KeyboardStandard, glyph, gr, imeBrush.get());
                    const D2D1_RECT_F tr{left + glyph + 3.0f * static_cast<float>(scale), 0,
                                         item.plate.right - 7.0f * static_cast<float>(scale), barH};
                    ui_->DrawTextWithHalo(d2d, st.inputMethodLabel, item.textPx * 0.78f,
                                          DWRITE_FONT_WEIGHT_SEMI_BOLD, tr, ink,
                                          DWRITE_TEXT_ALIGNMENT_LEADING);
                    break;
                }
                default:
                    ui_->DrawTextWithHalo(d2d, item.label, item.textPx, item.weight, item.text, ink,
                                          DWRITE_TEXT_ALIGNMENT_CENTER);
                    break;
            }
        } else {
            ui_->DrawTextWithHalo(d2d, item.label, item.textPx, item.weight,
                                  D2D1::RectF(item.plate.left, 0, item.plate.right, barH), ink,
                                  DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }
}

void BarWindow::Render(bool force) {
    if (!surface_ || !hwnd_) return;
    const double now = NowMs();
    double dt = lastFrameMs_ > 0 ? (now - lastFrameMs_) / 1000.0 : 1.0 / 60.0;
    dt = std::clamp(dt, 0.0, 0.1);
    lastFrameMs_ = now;

    Layout(static_cast<float>(surface_->width()), static_cast<float>(barHeight_), gpu_->scale());
    surface_->PrepareBackdrop();

    // Animate hover values.
    const MenuId open = host_->openMenu();
    animating_ = false;
    for (auto& item : items_) {
        bool hot = false;
        if (item.tray) hot = hoveredTrayId_ == item.trayId;
        else if (item.overflow) hot = hoveredOverflow_ || host_->trayOverflowOpen();
        else hot = hovered_ == item.id || IsOpenMenu(item.id, open) ||
                   (item.statusCluster && host_->controlCenterOpen());
        const float target = hot ? 1.0f : 0.0f;
        const float rate = hot ? 18.0f : 10.0f;
        item.hover = Approach(item.hover, target, rate, static_cast<float>(dt));
        if (std::abs(item.hover - target) > 0.002f) animating_ = true;
        else item.hover = target;
    }
    const float oldLeft = inkLeft_, oldRight = inkRight_;
    UpdateInk(dt);
    if (std::abs(oldLeft - inkLeft_) > 0.002f || std::abs(oldRight - inkRight_) > 0.002f) animating_ = true;

    contentDirty_ = false;
    firstFrame_ = false;

    const GlassParams p = MakeParams();
    surface_->SetVisualTransform(1.0f, 1.0f, 0, 0);
    surface_->Render(p, [this](ID2D1DeviceContext2* d2d) { DrawContent(d2d); });
}

void BarWindow::EnsureMouseTracking() {
    if (mouseTracking_) return;
    TRACKMOUSEEVENT tme{};
    tme.cbSize = sizeof(tme);
    tme.dwFlags = TME_LEAVE;
    tme.hwndTrack = hwnd_;
    if (TrackMouseEvent(&tme)) mouseTracking_ = true;
}

BarWindow::BarItem* BarWindow::FindItem(POINT client) {
    if (client.y < 0 || client.y >= static_cast<LONG>(barHeight_)) return nullptr;
    for (auto& item : items_) {
        if (client.x >= item.plate.left - 2 && client.x <= item.plate.right + 2) return &item;
    }
    return nullptr;
}

ID2D1Bitmap1* BarWindow::TrayBitmap(ID2D1DeviceContext2* d2d, const TrayIcon& icon) {
    if (icon.imageId == 0 || icon.pixels.empty() || icon.width <= 0 || icon.height <= 0) return nullptr;
    auto it = trayBitmaps_.find(icon.imageId);
    if (it != trayBitmaps_.end()) return it->second.get();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    HRESULT hr = d2d->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(icon.width), static_cast<UINT32>(icon.height)),
                                   icon.pixels.data(), static_cast<UINT32>(icon.width * 4), props, bitmap.put());
    if (FAILED(hr)) return nullptr;
    auto [ins, ok] = trayBitmaps_.emplace(icon.imageId, std::move(bitmap));
    return ins->second.get();
}

void BarWindow::UpdateTrayTooltip(const BarItem* item) {
    if (!tooltip_) return;
    if (host_ && host_->popupVisible()) {
        HideTrayTooltip();
        return;
    }
    if (!item || (!item->tray && !item->overflow)) {
        HideTrayTooltip();
        return;
    }
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    std::wstring text;
    if (item->tray) {
        text = item->trayLabel;
    } else {
        text = zh ? L"隐藏的图标" : L"Hidden icons";
    }
    if (text.empty()) text = zh ? L"系统托盘" : L"System tray";
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    info.hwnd = hwnd_;
    info.uId = 1;
    info.lpszText = const_cast<LPWSTR>(text.c_str());
    SendMessageW(tooltip_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
    const RECT wr = windowScreenRect();
    const int x = wr.left + static_cast<int>((item->plate.left + item->plate.right) * 0.5f);
    const int y = wr.top + static_cast<int>(barHeight_) + 2;
    SendMessageW(tooltip_, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, TRUE, reinterpret_cast<LPARAM>(&info));
    ShowWindow(tooltip_, SW_SHOWNOACTIVATE);
    tooltipKey_ = item->tray ? item->trayId : L"#overflow";
}

void BarWindow::HideTrayTooltip() {
    if (!tooltip_ || tooltipKey_.empty()) return;
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.uFlags = TTF_TRACK | TTF_ABSOLUTE;
    info.hwnd = hwnd_;
    info.uId = 1;
    SendMessageW(tooltip_, TTM_TRACKACTIVATE, FALSE, reinterpret_cast<LPARAM>(&info));
    ShowWindow(tooltip_, SW_HIDE);
    tooltipKey_.clear();
}

void BarWindow::DrawTrayIcon(ID2D1DeviceContext2* d2d, const BarItem& item, ID2D1Brush* fallbackBrush) {
    const auto& tray = host_->trayIcons();
    const TrayIcon* icon = nullptr;
    for (const auto& candidate : tray) {
        if (candidate.id == item.trayId) {
            icon = &candidate;
            break;
        }
    }
    const float box = item.iconSize;
    const float x = (item.plate.left + item.plate.right - box) * 0.5f;
    const float y = (static_cast<float>(barHeight_) - box) * 0.5f;
    ID2D1Bitmap1* bitmap = icon ? TrayBitmap(d2d, *icon) : nullptr;
    if (bitmap) {
        d2d->DrawBitmap(bitmap, D2D1::RectF(x, y, x + box, y + box), 0.96f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        ui_->DrawGlyph(d2d, 0xE7C3, box * 1.2f, D2D1::RectF(x, y, x + box, y + box), fallbackBrush);
    }
}

RECT BarWindow::ToScreen(const D2D1_RECT_F& r) const {
    RECT wr = windowScreenRect();
    RECT out;
    out.left = wr.left + static_cast<LONG>(std::lround(r.left));
    out.top = wr.top + static_cast<LONG>(std::lround(r.top));
    out.right = wr.left + static_cast<LONG>(std::lround(r.right));
    out.bottom = wr.top + static_cast<LONG>(std::lround(r.bottom));
    return out;
}

RECT BarWindow::plateScreenRect(MenuId id) const {
    for (const auto& item : items_) {
        if (item.id == id) return ToScreen(item.plate);
    }
    RECT r{};
    r.left = r.right = monitor_.rect.left + 12;
    r.top = monitor_.rect.top;
    r.bottom = monitor_.rect.top + static_cast<LONG>(barHeight_);
    return r;
}

LRESULT CALLBACK BarWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<BarWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wParam, lParam);
    switch (msg) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT wr{};
            GetWindowRect(hwnd, &wr);
            if (pt.y >= wr.top + static_cast<LONG>(self->barHeight_)) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            self->EnsureMouseTracking();
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            BarItem* item = self->FindItem(pt);
            MenuId menu = MenuId::None;
            std::wstring trayId;
            bool overflow = false;
            int statusPart = -1;
            if (item) {
                if (item->tray) trayId = item->trayId;
                else if (item->overflow) overflow = true;
                else menu = item->id;
                if (item->statusCluster) {
                    for (size_t i = 0; i < item->hoverParts.size(); ++i) {
                        const auto& part = item->hoverParts[i];
                        if (static_cast<float>(pt.x) >= part.left && static_cast<float>(pt.x) < part.right &&
                            static_cast<float>(pt.y) >= part.top && static_cast<float>(pt.y) < part.bottom) {
                            statusPart = static_cast<int>(i);
                            break;
                        }
                    }
                }
            }
            if (statusPart != self->hoveredStatusPart_) {
                self->hoveredStatusPart_ = statusPart;
                self->contentDirty_ = true;
            }
            if (menu != self->hovered_ || trayId != self->hoveredTrayId_ || overflow != self->hoveredOverflow_) {
                self->hovered_ = menu;
                self->hoveredTrayId_ = trayId;
                self->hoveredOverflow_ = overflow;
                self->contentDirty_ = true;
                self->host_->OnBarHover(menu);
            }
            self->UpdateTrayTooltip(item);
            TRACKMOUSEEVENT tme{};
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            self->mouseTracking_ = true;
            return 0;
        }
        case WM_MOUSELEAVE: {
            self->mouseTracking_ = false;
            self->HideTrayTooltip();
            self->hoveredStatusPart_ = -1;
            if (self->hovered_ != MenuId::None || !self->hoveredTrayId_.empty() || self->hoveredOverflow_) {
                self->hovered_ = MenuId::None;
                self->hoveredTrayId_.clear();
                self->hoveredOverflow_ = false;
                self->contentDirty_ = true;
                self->host_->OnBarHover(MenuId::None);
            }
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            self->HideTrayTooltip();
            BarItem* item = self->FindItem(pt);
            if (item && item->tray) {
                self->host_->OnTrayIconActivated(item->trayId);
            } else if (item && item->overflow) {
                self->host_->OnTrayOverflow(self->ToScreen(item->plate));
            } else if (item) {
                RECT anchor = self->ToScreen(item->plate);
                self->host_->OnBarItem(item->id, anchor);
            } else {
                self->host_->OnBarEmptyClick();
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            POINT client{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            BarItem* item = self->FindItem(client);
            POINT pt = client;
            ClientToScreen(hwnd, &pt);

            if (item && item->tray) {
                self->host_->OnTrayIconContext(item->trayId);
            } else {
                self->host_->OnBarContextMenu(pt);
            }
            return 0;
        }
        case WM_APP + 88:
            self->host_->OnOpenSettings();
            return 0;
        case WM_APP + 89:
            self->host_->OnQuitRequested();
            return 0;
        case WM_CLOSE:
            self->host_->OnQuitRequested();
            return 0;
        case kAppBarCallback:
            return 0;
        case WM_DPICHANGED:
            self->host_->OnDisplayChanged();
            return 0;
        case WM_DISPLAYCHANGE:
            self->contentDirty_ = true;
            self->host_->OnDisplayChanged();
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

}  // namespace sm
