#include "popup_window.h"

#include "fluent_glyphs.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace sm {
namespace {
constexpr wchar_t kPopupClass[] = L"SuperMenuPopupWindow";
// Atrium's kPop spring: response 0.36 s, damping 0.62. The popup is a
// DComposition visual, so the spring is evaluated per frame around the
// anchor and settles after the same 0.6 s as Atrium's PopVisual.
constexpr double kOpenMs = 190.0;
constexpr double kSpringOpenMs = 600.0;
constexpr double kCloseMs = 130.0;
constexpr float kPopResponse = 0.36f;
constexpr float kPopDamping = 0.62f;

float PopScale(double seconds) {
    constexpr float kFrom = 0.92f;
    constexpr float kTarget = 1.0f;
    if (seconds <= 0.0) return kFrom;
    if (seconds >= kSpringOpenMs / 1000.0) return kTarget;
    const float w0 = 6.2831853f / kPopResponse;
    const float z = kPopDamping;
    const float a = z * w0;
    const float wd = w0 * std::sqrt(1.0f - z * z);
    const float dt = static_cast<float>(seconds);
    const float y0 = kFrom - kTarget;
    const float e = std::exp(-a * dt);
    const float y = e * (y0 * std::cos(wd * dt) + (a * y0) / wd * std::sin(wd * dt));
    return kTarget + y;
}

int DaysInMonth(int year, int month) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) return 29;
    if (month < 1 || month > 12) return 30;
    return days[month - 1];
}
int WeekdayOfFirst(int year, int month) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = 1;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return 0;
    WORD index = 0;
    FileTimeToSystemTime(&ft, &st);
    // get day of week through a SYSTEMTIME->FILETIME round trip is insufficient;
    // use the C runtime free algorithm instead.
    int y = year, m = month;
    if (m < 3) {
        m += 12;
        --y;
    }
    const int k = y % 100;
    const int j = y / 100;
    const int h = (1 + 13 * (m + 1) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;  // 0 Sat
    const int sunday0 = (h + 6) % 7;                                       // 0 Sun
    (void)index;
    return sunday0;
}
std::wstring MonthTitle(const Settings& s, int year, int month) {
    if (s.language == L"en-US") {
        static const wchar_t* names[] = {L"January", L"February", L"March", L"April", L"May", L"June", L"July",
                                         L"August", L"September", L"October", L"November", L"December"};
        wchar_t buf[64];
        swprintf(buf, std::size(buf), L"%ls %d", names[std::clamp(month, 1, 12) - 1], year);
        return buf;
    }
    wchar_t buf[64];
    swprintf(buf, std::size(buf), L"%d年%d月", year, month);
    return buf;
}
std::wstring PercentText(int v) { return std::to_wstring(std::clamp(v, 0, 100)) + L"%"; }

bool PointInRect(POINT p, const RECT& r) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

bool InsideRoundedRect(POINT p, const RECT& r, float radius) {
    if (!PointInRect(p, r)) return false;
    radius = std::min(radius, std::min(float(r.right - r.left), float(r.bottom - r.top)) * 0.5f);
    const float cx = p.x < r.left + radius ? float(r.left + radius) : p.x > r.right - radius ? float(r.right - radius) : float(p.x);
    const float cy = p.y < r.top + radius ? float(r.top + radius) : p.y > r.bottom - radius ? float(r.bottom - radius) : float(p.y);
    return (p.x - cx) * (p.x - cx) + (p.y - cy) * (p.y - cy) <= radius * radius;
}

}  // namespace

PopupWindow::~PopupWindow() { CloseNow(); }

bool PopupWindow::CreateWindowOnly(Gpu* gpu, Capture* capture, PopupHost* host, Ui* ui, Icons* icons,
                                   const RECT& monitorRect) {
    hinst_ = GetModuleHandleW(nullptr);
    gpu_ = gpu;
    capture_ = capture;
    host_ = host;
    ui_ = ui;
    icons_ = icons;
    monitorRect_ = monitorRect;
    scale_ = gpu->scale();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hinst_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kPopupClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
                            kPopupClass, L"SuperMenu", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hinst_, this);
    if (!hwnd_) return false;
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    ExcludeWindowFromCapture(hwnd_);
    return true;
}

void PopupWindow::ComputeContentSize() {
    const Settings& s = host_->settings();
    const float fs = static_cast<float>(13.0 * scale_ * s.fontScale);
    if (kind_ == PopupKind::Menu) {
        const float leftPad = Dip(11), rightPad = Dip(11), checkW = Dip(16);
        const float rowH = Dip(23), sepH = Dip(8);
        float labelW = 0;
        for (const auto& item : menu_.items) {
            if (item.separator) continue;
            labelW = std::max(labelW, ui_->Measure(item.label, fs, DWRITE_FONT_WEIGHT_MEDIUM));
        }
        if (labelW <= 0) labelW = Dip(160);
        // No shortcut column anywhere: menu width is label + check column only.
        contentW_ = static_cast<int>(std::ceil(leftPad + checkW + labelW + rightPad));
        contentW_ = std::max(contentW_, static_cast<int>(Dip(196)));
        contentW_ = std::min(contentW_, static_cast<int>(Dip(430)));
        float h = Dip(7);
        for (const auto& item : menu_.items) h += item.separator ? sepH : rowH;
        h += Dip(7);
        contentH_ = static_cast<int>(std::ceil(h));
    } else if (kind_ == PopupKind::ControlCenter) {
        contentW_ = static_cast<int>(Dip(374));
        contentH_ = static_cast<int>(Dip(404));
    } else if (kind_ == PopupKind::Calendar) {
        contentW_ = static_cast<int>(Dip(330));
        contentH_ = static_cast<int>(Dip(420));
    } else if (kind_ == PopupKind::Volume) {
        contentW_ = static_cast<int>(Dip(300));
        contentH_ = static_cast<int>(Dip(114));
    } else if (kind_ == PopupKind::Notifications) {
        contentW_ = static_cast<int>(Dip(336));
        contentH_ = static_cast<int>(Dip(204));
    } else if (kind_ == PopupKind::Tray) {
        const size_t count = HiddenTray().size();
        const int columns = 5;
        const int rows = std::max(1, static_cast<int>((count + columns - 1) / columns));
        contentW_ = static_cast<int>(Dip(340));
        contentH_ = static_cast<int>(Dip(32) + rows * Dip(56) + Dip(24));
    } else {
        contentW_ = static_cast<int>(Dip(300));
        contentH_ = static_cast<int>(Dip(168));
    }
}

bool PopupWindow::Open(PopupKind kind, MenuId menuId, const MenuModel& menu, RECT anchorScreen, Gpu* gpu,
                       Capture* capture, PopupHost* host, Ui* ui, Icons* icons, const RECT& monitorRect) {
    if (!host || !gpu || !capture) return false;
    if (!hwnd_ && !CreateWindowOnly(gpu, capture, host, ui, icons, monitorRect)) return false;
    kind_ = kind;
    menuId_ = menuId;
    menu_ = menu;
    anchor_ = anchorScreen;
    monitorRect_ = monitorRect;
    pendingInkInit_ = true;
    if (kind == PopupKind::Calendar) {
        SYSTEMTIME now{};
        GetLocalTime(&now);
        calYear_ = calSelectedYear_ = now.wYear;
        calMonth_ = calSelectedMonth_ = now.wMonth;
        calSelectedDay_ = now.wDay;
        clockSecond_ = -1;
    }
    host_ = host;
    gpu_ = gpu;
    capture_ = capture;
    ui_ = ui;
    icons_ = icons;
    scale_ = gpu->scale();
    inset_ = static_cast<int>(Dip(12));
    ComputeContentSize();
    BuildLayout();
    PositionAndResize(anchorScreen);
    const RECT wr = screenRect();
    if (!surface_) {
        surface_ = std::make_unique<GlassSurface>();
        if (!surface_->Initialize(gpu_, hwnd_, wr, static_cast<int>(Dip(36)), static_cast<float>(inset_), L"popup")) {
            surface_.reset();
            return false;
        }
    } else {
        surface_->SetScreenRect(wr);
        surface_->ResizeBuffers(wr.right - wr.left, wr.bottom - wr.top);
    }
    surface_->UpdateBackdrop(capture_->Texture(), true);
    backdropValid_ = true;
    hoverHit_ = -1;
    closing_ = false;
    visible_ = true;
    openStartMs_ = NowMs();
    animating_ = true;
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd_, HWND_TOPMOST, wr.left, wr.top, wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Render();
    return true;
}

bool PopupWindow::OpenPanel(PopupKind kind, RECT anchorScreen, Gpu* gpu, Capture* capture, PopupHost* host, Ui* ui,
                            Icons* icons, const RECT& monitorRect) {
    MenuModel empty;
    empty.id = MenuId::None;
    return Open(kind, MenuId::None, empty, anchorScreen, gpu, capture, host, ui, icons, monitorRect);
}

void PopupWindow::PositionAndResize(RECT anchorScreen) {
    const float gap = Dip(kind_ == PopupKind::Menu ? 3 : 4);
    int contentLeft = anchorScreen.left;
    if (kind_ == PopupKind::ControlCenter || kind_ == PopupKind::Calendar || kind_ == PopupKind::Notifications ||
        kind_ == PopupKind::Tray) {
        contentLeft = monitorRect_.right - static_cast<int>(Dip(10)) - contentW_;
    } else if (kind_ != PopupKind::Menu) {
        contentLeft = anchorScreen.right - contentW_;
    }
    int contentTop = anchorScreen.bottom + static_cast<int>(gap);
    const int margin = static_cast<int>(Dip(8));
    contentLeft = std::clamp(contentLeft, static_cast<int>(monitorRect_.left) + margin,
                             static_cast<int>(monitorRect_.right) - margin - contentW_);
    const int maxBottom = std::max(static_cast<int>(monitorRect_.top) + margin + contentH_,
                                   static_cast<int>(monitorRect_.bottom) - margin);
    if (contentTop + contentH_ > maxBottom) contentTop = maxBottom - contentH_;
    if (contentTop < static_cast<int>(monitorRect_.top) + margin) contentTop = monitorRect_.top + margin;

    windowRect_.left = contentLeft - inset_;
    windowRect_.top = contentTop - inset_;
    windowRect_.right = contentLeft + contentW_ + inset_;
    windowRect_.bottom = contentTop + contentH_ + inset_;
    width_ = windowRect_.right - windowRect_.left;
    height_ = windowRect_.bottom - windowRect_.top;
    SetWindowPos(hwnd_, HWND_TOPMOST, windowRect_.left, windowRect_.top, width_, height_,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}

RECT PopupWindow::screenRect() const {
    RECT wr{};
    if (hwnd_) GetWindowRect(hwnd_, &wr);
    else wr = windowRect_;
    return wr;
}

void PopupWindow::BuildLayout() {
    hits_.clear();
    ccCardRects_.clear();
    if (kind_ == PopupKind::Menu) {
        const float fs = static_cast<float>(13.0 * scale_ * host_->settings().fontScale);
        const int left = inset_ + static_cast<int>(Dip(11));
        const int right = inset_ + contentW_ - static_cast<int>(Dip(11));
        (void)fs;
        float y = static_cast<float>(inset_) + Dip(7);
        for (size_t i = 0; i < menu_.items.size(); ++i) {
            const auto& item = menu_.items[i];
            const float h = item.separator ? Dip(8) : Dip(23);
            if (!item.separator) {
                Hit hit;
                hit.type = Hit::Type::MenuRow;
                hit.rect = RECT{left - static_cast<int>(Dip(6)), static_cast<int>(std::lround(y)),
                                right + static_cast<int>(Dip(6)), static_cast<int>(std::lround(y + h))};
                hit.command = item.command;
                hit.value = static_cast<int>(i);
                hits_.push_back(hit);
            }
            y += h;
        }
    } else if (kind_ == PopupKind::ControlCenter) {
        // Six floating glass islands: 3 connectivity/Focus pills, the media
        // card and two full-width sliders. There is deliberately no outer panel.
        const int pad = static_cast<int>(Dip(14));
        const int gap = static_cast<int>(Dip(10));
        const int pillGap = static_cast<int>(Dip(8));
        const int pillH = static_cast<int>(Dip(58));
        const int innerW = contentW_ - pad * 2;
        const int colGap = static_cast<int>(Dip(10));
        const int leftW = static_cast<int>(Dip(146));
        const int rightW = innerW - colGap - leftW;  // square media card (190x190)
        const int x0 = inset_ + pad;
        const int topY = inset_ + pad;
        const int topH = 3 * pillH + 2 * pillGap;
        // The icon circle toggles; the rest of the tile opens its settings page.
        const Command topCmds[3] = {CmdWifiSettings, CmdBluetoothSettings, CmdOpenNotificationSettings};
        const Command topToggles[3] = {CmdToggleWifi, CmdToggleBluetooth, CmdToggleFocus};
        for (int i = 0; i < 3; ++i) {
            Hit hit;
            hit.type = Hit::Type::Button;
            hit.command = topCmds[i];
            const int yy = topY + i * (pillH + pillGap);
            hit.rect = RECT{x0, yy, x0 + leftW, yy + pillH};
            hits_.push_back(hit);
            ccCardRects_.push_back(hit.rect);

            // Appended after the card hit so a click on the circle wins; text
            // and padding still open the settings page.
            Hit iconHit;
            iconHit.type = Hit::Type::Button;
            iconHit.command = topToggles[i];
            iconHit.rect = RECT{x0 + static_cast<int>(Dip(5)),
                                yy + pillH / 2 - static_cast<int>(Dip(22)),
                                x0 + static_cast<int>(Dip(48)),
                                yy + pillH / 2 + static_cast<int>(Dip(22))};
            hits_.push_back(iconHit);
        }
        const int rightX = x0 + leftW + colGap;
        const int mediaY = topY;
        const RECT mediaRect{rightX, mediaY, rightX + rightW, mediaY + topH};
        ccCardRects_.push_back(mediaRect);
        const int btn = static_cast<int>(Dip(28));
        const int transportGap = static_cast<int>(Dip(36));
        const int transportCenter = rightX + rightW / 2;
        const int buttonCenterY = mediaY + static_cast<int>(Dip(158));
        for (int i = 0; i < 3; ++i) {
            Hit hit;
            hit.type = Hit::Type::Button;
            hit.command = i == 0 ? CmdMediaPrev : i == 1 ? CmdMediaPlayPause : CmdMediaNext;
            const int cx = transportCenter + (i - 1) * transportGap;
            hit.rect = RECT{cx - btn / 2, buttonCenterY - btn / 2, cx + btn / 2, buttonCenterY + btn / 2};
            hits_.push_back(hit);
        }
        const int sliderY = topY + topH + gap;
        const int bh = static_cast<int>(Dip(72));
        for (int i = 0; i < 2; ++i) {
            const int currentY = sliderY + i * (bh + gap);
            const RECT card{x0, currentY, x0 + innerW, currentY + bh};
            ccCardRects_.push_back(card);
            Hit hit;
            hit.type = i == 0 ? Hit::Type::BrightnessSlider : Hit::Type::VolumeSlider;
            const int trackY = currentY + static_cast<int>(Dip(50));
            // 0% sits at the right tangent of a track-radius circle centred on
            // the glyph: track.left + 2 + radius (centre) + radius (tangent).
            hit.rect = RECT{x0 + static_cast<int>(Dip(38)), trackY - static_cast<int>(Dip(14)),
                            x0 + innerW - static_cast<int>(Dip(12)), trackY + static_cast<int>(Dip(14))};
            hits_.push_back(hit);
        }
    } else if (kind_ == PopupKind::Calendar) {
        // The whole mini-calendar preview opens the system Calendar app.
        Hit hit;
        hit.type = Hit::Type::Button;
        hit.command = CmdOpenSystemCalendar;
        hit.rect = RECT{inset_ + static_cast<int>(Dip(14)), inset_ + static_cast<int>(Dip(108)),
                        inset_ + contentW_ - static_cast<int>(Dip(14)), inset_ + contentH_ - static_cast<int>(Dip(10))};
        hits_.push_back(hit);
    } else if (kind_ == PopupKind::Tray) {
        const int pad = static_cast<int>(Dip(14));
        const int header = static_cast<int>(Dip(34));
        const int cellW = (contentW_ - pad * 2) / 5;
        const int cellH = static_cast<int>(Dip(56));
        const auto hidden = HiddenTray();
        for (size_t i = 0; i < hidden.size(); ++i) {
            const int col = static_cast<int>(i % 5);
            const int row = static_cast<int>(i / 5);
            Hit hit;
            hit.type = Hit::Type::TrayButton;
            hit.trayId = hidden[i]->id;
            hit.rect = RECT{inset_ + pad + col * cellW, inset_ + header + row * cellH,
                            inset_ + pad + (col + 1) * cellW, inset_ + header + (row + 1) * cellH};
            hits_.push_back(hit);
        }
    } else if (kind_ == PopupKind::Volume) {
        const int x0 = inset_ + static_cast<int>(Dip(16));
        Hit hit;
        hit.type = Hit::Type::VolumeSlider;
        const int trackY = inset_ + static_cast<int>(Dip(75));
        hit.rect = RECT{x0 + static_cast<int>(Dip(34)), trackY - static_cast<int>(Dip(8)),
                        inset_ + contentW_ - static_cast<int>(Dip(18)), trackY + static_cast<int>(Dip(8))};
        hits_.push_back(hit);
        Hit mute;
        mute.type = Hit::Type::Mute;
        mute.rect = RECT{x0, trackY - static_cast<int>(Dip(18)), x0 + static_cast<int>(Dip(28)), trackY + static_cast<int>(Dip(18))};
        hits_.push_back(mute);
    } else {
        Hit hit;
        hit.type = Hit::Type::Button;
        hit.command = kind_ == PopupKind::Wifi ? CmdNetworkSettings
                     : kind_ == PopupKind::Battery ? CmdBatterySettings
                     : kind_ == PopupKind::Notifications ? CmdOpenNotificationSettings
                                                       : CmdSystemSettings;
        hit.rect = RECT{inset_ + static_cast<int>(Dip(16)), inset_ + contentH_ - static_cast<int>(Dip(46)),
                        inset_ + contentW_ - static_cast<int>(Dip(16)), inset_ + contentH_ - static_cast<int>(Dip(14))};
        hits_.push_back(hit);
    }
}

GlassParams PopupWindow::MakeParams() const {
    const Settings& s = host_->settings();
    const bool followBar = kind_ == PopupKind::ControlCenter;
    // The Control Center deliberately reuses the bar's material (including the
    // barHeight-dependent edge/refraction caps) so the user's glass settings
    // look identical across the bar and its popovers.
    MaterialParams m = followBar ? ComputeMaterial(s, scale_, s.barHeight, false)
                                 : ComputeMaterial(s, scale_, 30.0, true);
    GlassParams p{};
    p.radius = std::min(Dip(15), float(std::min(contentW_, contentH_)) * 0.25f);
    p.edge = followBar ? m.edge : std::min(m.edge, Dip(22));
    p.refract = followBar ? m.refract : std::min(m.refract, Dip(20));
    p.dispersion = m.dispersion;
    p.blurMix = m.blurMix;
    p.blurLod = std::min(m.blurLod, followBar ? 3.5f : 4.0f);
    p.tintMix = followBar ? m.tintMix : std::min(0.36f, m.tintMix + 0.04f);
    p.tintR = m.tintR;
    p.tintG = m.tintG;
    p.tintB = m.tintB;
    p.tintA = 1.0f;
    p.highlight = m.highlight;
    p.shadow = followBar ? m.shadow : std::max(0.28f, m.shadow);
    p.lightX = followBar ? 0.0f : -0.6f;
    p.lightY = followBar ? -1.0f : -0.8f;
    p.inset = followBar ? Pxf(14.0, scale_) : static_cast<float>(inset_);
    p.noise = m.noise;
    p.vibrancy = m.saturation;
    p.shapeL = static_cast<float>(inset_);
    p.shapeT = static_cast<float>(inset_);
    p.shapeR = static_cast<float>(width_ - inset_);
    p.shapeB = static_cast<float>(height_ - inset_);
    p.fade = 0;

    // Control Center is containerless: the glass pass is evaluated separately
    // for each floating island so the gaps between cards show the desktop
    // instead of a large panel. Six islands fit the shader's PartRect slots.
    if (kind_ == PopupKind::ControlCenter) {
        const int pad = static_cast<int>(Dip(14));
        const int gap = static_cast<int>(Dip(10));
        const int pillGap = static_cast<int>(Dip(8));
        const int pillH = static_cast<int>(Dip(58));
        const int innerW = contentW_ - pad * 2;
        const int colGap = static_cast<int>(Dip(10));
        const int leftW = static_cast<int>(Dip(146));
        const int rightW = innerW - colGap - leftW;
        const int x0 = inset_ + pad;
        const int topY = inset_ + pad;
        const int topH = 3 * pillH + 2 * pillGap;
        const int sliderY = topY + topH + gap;
        const int bh = static_cast<int>(Dip(72));
        int index = 0;
        auto addPart = [&](float left, float top, float right, float bottom, float radius) {
            if (index >= 6) return;
            p.parts[index][0] = left;
            p.parts[index][1] = top;
            p.parts[index][2] = right;
            p.parts[index][3] = bottom;
            p.partRadii[index][0] = radius;
            ++index;
        };
        for (int i = 0; i < 3; ++i) {
            const int yy = topY + i * (pillH + pillGap);
            addPart(float(x0), float(yy), float(x0 + leftW), float(yy + pillH), float(pillH) * 0.5f);
        }
        const int rightX = x0 + leftW + colGap;
        addPart(float(rightX), float(topY), float(rightX + rightW), float(topY + topH), Dip(26));
        for (int i = 0; i < 2; ++i) {
            const int yy = sliderY + i * (bh + gap);
            addPart(float(x0), float(yy), float(x0 + innerW), float(yy + bh), Dip(20));
        }
        p.partCount = static_cast<float>(index);
        p.partMix = 1.0f;
    }
    return p;
}

void PopupWindow::Render() {
    if (!surface_ || !hwnd_ || !visible_) return;
    const double now = NowMs();
    float scale = 1.0f;
    float opacity = 1.0f;
    if (closing_) {
        const float t = static_cast<float>(std::clamp((now - closeStartMs_) / kCloseMs, 0.0, 1.0));
        scale = 1.0f - 0.045f * sm::EaseOutCubic(t);
        opacity = 1.0f - t;
        animating_ = t < 1.0f;
    } else if (kind_ == PopupKind::ControlCenter || kind_ == PopupKind::Calendar) {
        // The Control Center and the date/time popup get Atrium's kPop spring
        // (0.92 → 1 with a small overshoot over 0.6 s) plus a 160 ms fade.
        // Other popups keep the shorter 190 ms scale-in.
        const bool animate = host_->settings().menuAnimations;
        const double seconds = animate ? std::max(0.0, now - openStartMs_) / 1000.0
                                       : kSpringOpenMs / 1000.0;
        scale = animate ? PopScale(seconds) : 1.0f;
        opacity = animate ? sm::EaseOutCubic(static_cast<float>(std::min(1.0, seconds / 0.16))) : 1.0f;
        animating_ = animate && seconds < kSpringOpenMs / 1000.0;
    } else {
        const bool animate = host_->settings().menuAnimations;
        const float t = animate ? static_cast<float>(std::clamp((now - openStartMs_) / kOpenMs, 0.0, 1.0)) : 1.0f;
        scale = 0.965f + 0.035f * sm::EaseOutCubic(t);
        opacity = sm::EaseOutCubic(std::min(1.0f, t * 1.6f));
        animating_ = animate && t < 1.0f;
    }
    if (closing_ && !animating_) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        return;
    }
    const float ax = static_cast<float>(anchor_.left + anchor_.right) * 0.5f - static_cast<float>(windowRect_.left);
    surface_->SetVisualTransform(scale, opacity, ax, static_cast<float>(inset_));
    BuildLayout();
    surface_->PrepareBackdrop();

    // Ink follows the actual backdrop under this popup.
    const MaterialParams m = ComputeMaterial(host_->settings(), scale_, 30.0, true);
    const float luma = surface_->hasLuma() ? surface_->averageLuma() : 0.5f;
    const float effective = luma * (1.0f - m.tintMix) + 0.92f * m.tintMix;
    float target = 0.5f;
    if (effective > 0.52f) target = 1.0f;
    else if (effective < 0.43f) target = 0.0f;
    if (pendingInkInit_) {
        popupInk_ = target;
        pendingInkInit_ = false;
    } else {
        popupInk_ = sm::Approach(popupInk_, target, 4.0f, 1.0f / 60.0f);
    }

    surface_->Render(MakeParams(), [this](ID2D1DeviceContext2* d2d) { DrawContent(d2d); });
}

void PopupWindow::Tick(double nowMs) {
    if (!visible_) return;
    if (closing_ && nowMs - closeStartMs_ >= kCloseMs) {
        ShowWindow(hwnd_, SW_HIDE);
        visible_ = false;
        animating_ = false;
        closing_ = false;
        return;
    }
    if (kind_ == PopupKind::Calendar && !closing_) {
        const int second = host_->status().now.wSecond;
        const bool tick = second != clockSecond_;
        if (tick) clockSecond_ = second;
        if (animating_ || tick) Render();
        return;
    }
    if (animating_) Render();
}

void PopupWindow::BeginClose() {
    if (!visible_ || closing_) return;
    closing_ = true;
    closeStartMs_ = NowMs();
    animating_ = true;
}

void PopupWindow::CloseNow() {
    if (hwnd_) {
        surface_.reset();
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    visible_ = false;
    closing_ = false;
    kind_ = PopupKind::None;
    menuId_ = MenuId::None;
    hits_.clear();
}

bool PopupWindow::UpdateBackdrop() {
    if (!surface_ || !capture_ || !visible_) return false;
    const bool ok = surface_->UpdateBackdrop(capture_->Texture(), true);
    if (ok) backdropValid_ = true;
    return ok;
}

bool PopupWindow::ContainsScreenPoint(POINT pt) const {
    if (!visible_ || !hwnd_) return false;
    const RECT wr = screenRect();
    if (kind_ == PopupKind::ControlCenter && !ccCardRects_.empty()) {
        for (const RECT& card : ccCardRects_) {
            const RECT screen{wr.left + card.left, wr.top + card.top, wr.left + card.right,
                              wr.top + card.bottom};
            if (PointInRect(pt, screen)) return true;
        }
        return false;
    }
    return PointInRect(pt, wr);
}

void PopupWindow::SetSliderFromPoint(POINT client, bool volume) {
    if (hits_.empty()) return;
    for (const auto& hit : hits_) {
        if ((volume && hit.type != Hit::Type::VolumeSlider) || (!volume && hit.type != Hit::Type::BrightnessSlider)) continue;
        const float t = sm::Clamp(float(client.x - hit.rect.left) /
                                      float(std::max(1, static_cast<int>(hit.rect.right - hit.rect.left))),
                                  0.0f, 1.0f);
        const int percent = static_cast<int>(std::lround(t * 100.0f));
        if (volume) host_->OnPopupVolume(percent);
        else host_->OnPopupBrightness(percent);
        break;
    }
}

PopupWindow::Hit PopupWindow::HitTest(POINT client) const {
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
        if (PointInRect(client, it->rect)) return *it;
    }
    return Hit{};
}

void PopupWindow::OnClick(POINT client) {
    const Hit hit = HitTest(client);
    switch (hit.type) {
        case Hit::Type::MenuRow:
            host_->OnPopupCommand(hit.command);
            break;
        case Hit::Type::Button:
            host_->OnPopupCommand(hit.command);
            break;
        case Hit::Type::VolumeSlider:
            dragging_ = Hit::Type::VolumeSlider;
            SetCapture(hwnd_);
            SetSliderFromPoint(client, true);
            break;
        case Hit::Type::BrightnessSlider:
            dragging_ = Hit::Type::BrightnessSlider;
            SetCapture(hwnd_);
            SetSliderFromPoint(client, false);
            break;
        case Hit::Type::Mute:
            host_->OnPopupMute();
            break;
        case Hit::Type::CalendarPrev:
            --calMonth_;
            if (calMonth_ < 1) {
                calMonth_ = 12;
                --calYear_;
            }
            BuildLayout();
            Render();
            break;
        case Hit::Type::CalendarNext:
            ++calMonth_;
            if (calMonth_ > 12) {
                calMonth_ = 1;
                ++calYear_;
            }
            BuildLayout();
            Render();
            break;
        case Hit::Type::CalendarDay:
            calSelectedDay_ = hit.value;
            calSelectedMonth_ = calMonth_;
            calSelectedYear_ = calYear_;
            BuildLayout();
            Render();
            break;
        case Hit::Type::TrayButton:
            host_->OnTrayIconActivated(hit.trayId);
            break;
        case Hit::Type::None:
        default:
            break;
    }
}

void PopupWindow::HandleKey(int vk) {
    if (vk == VK_ESCAPE) {
        BeginClose();
        return;
    }
    if (kind_ != PopupKind::Menu) return;
    int current = -1;
    for (size_t i = 0; i < hits_.size(); ++i) {
        if (static_cast<int>(i) == hoverHit_ && hits_[i].type == Hit::Type::MenuRow) current = static_cast<int>(i);
    }
    if (vk == VK_DOWN || vk == VK_UP) {
        int next = current;
        const int dir = vk == VK_DOWN ? 1 : -1;
        do {
            next += dir;
            if (next < 0) next = static_cast<int>(hits_.size()) - 1;
            if (next >= static_cast<int>(hits_.size())) next = 0;
        } while (!hits_.empty() && hits_[next].type != Hit::Type::MenuRow && next != current);
        hoverHit_ = next;
        Render();
    } else if (vk == VK_RETURN && current >= 0) {
        host_->OnPopupCommand(hits_[current].command);
    }
}

LRESULT CALLBACK PopupWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PopupWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
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
            if (self->kind_ == PopupKind::ControlCenter && !self->ccCardRects_.empty()) {
                for (const RECT& card : self->ccCardRects_) {
                    const RECT screen{wr.left + card.left, wr.top + card.top, wr.left + card.right,
                                      wr.top + card.bottom};
                    if (PointInRect(pt, screen)) return HTCLIENT;
                }
                return HTTRANSPARENT;
            }
            RECT shape{wr.left + self->inset_, wr.top + self->inset_, wr.right - self->inset_, wr.bottom - self->inset_};
            const float radius = self->Dip(15);
            if (!InsideRoundedRect(pt, shape, radius)) return HTTRANSPARENT;
            return HTCLIENT;
        }
        case WM_MOUSEMOVE: {
            if (!self->mouseTracking_) {
                TRACKMOUSEEVENT tme{};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                self->mouseTracking_ = true;
            }
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (self->dragging_ == Hit::Type::VolumeSlider) {
                self->SetSliderFromPoint(pt, true);
                return 0;
            }
            if (self->dragging_ == Hit::Type::BrightnessSlider) {
                self->SetSliderFromPoint(pt, false);
                return 0;
            }
            int hover = -1;
            for (size_t i = 0; i < self->hits_.size(); ++i) {
                if (!PointInRect(pt, self->hits_[i].rect)) continue;
                if (self->hits_[i].type == Hit::Type::MenuRow || self->hits_[i].type == Hit::Type::TrayButton) {
                    hover = static_cast<int>(i);
                    break;
                }
            }
            if (hover != self->hoverHit_) {
                self->hoverHit_ = hover;
                self->surface_->Render(self->MakeParams(), [self](ID2D1DeviceContext2* d2d) { self->DrawContent(d2d); });
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            self->mouseTracking_ = false;
            if (self->hoverHit_ != -1) {
                self->hoverHit_ = -1;
                if (self->surface_) {
                    self->surface_->Render(self->MakeParams(), [self](ID2D1DeviceContext2* d2d) { self->DrawContent(d2d); });
                }
            }
            return 0;
        case WM_LBUTTONDOWN: {
            self->OnClick(POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        }
        case WM_RBUTTONUP: {
            const POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const Hit hit = self->HitTest(pt);
            if (hit.type == Hit::Type::TrayButton) {
                self->host_->OnTrayIconContext(hit.trayId);
            }
            return 0;
        }
        case WM_LBUTTONUP:
            if (self->dragging_ != Hit::Type::None) {
                self->dragging_ = Hit::Type::None;
                ReleaseCapture();
            }
            return 0;
        case WM_CLOSE:
            self->BeginClose();
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void PopupWindow::DrawContent(ID2D1DeviceContext2* d2d) {
    switch (kind_) {
        case PopupKind::Menu: DrawMenu(d2d); break;
        case PopupKind::ControlCenter: DrawControlCenter(d2d); break;
        case PopupKind::Calendar: DrawCalendar(d2d); break;
        case PopupKind::Volume: DrawVolumePanel(d2d); break;
        case PopupKind::Notifications: DrawNotifications(d2d); break;
        case PopupKind::Tray: DrawTray(d2d); break;
        default: DrawInfoPanel(d2d); break;
    }
}

void PopupWindow::DrawMenu(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const float fs = static_cast<float>(13.0 * scale_ * host_->settings().fontScale);
    const int left = inset_ + static_cast<int>(Dip(11));
    const int right = inset_ + contentW_ - static_cast<int>(Dip(11));
    const int checkX = left;
    const int labelX = left + static_cast<int>(Dip(18));
    const float rowH = Dip(23);
    int hitIndex = 0;
    float y = static_cast<float>(inset_) + Dip(7);
    // Find the hit index for each row by walking hits in order.
    for (const auto& item : menu_.items) {
        if (item.separator) {
            ComPtr<ID2D1SolidColorBrush> sepBrush(ui_->Brush(d2d, ink.isDark() ? D2D1::ColorF(0, 0, 0, 0.10f)
                                                                            : D2D1::ColorF(1, 1, 1, 0.16f)));
            const float lineY = y + Dip(4);
            d2d->DrawLine(D2D1::Point2F(float(left + 6), lineY), D2D1::Point2F(float(right - 6), lineY), sepBrush.get(),
                          static_cast<float>(std::max(1.0, scale_)));
            y += Dip(8);
            continue;
        }
        const bool hovered = hitIndex < static_cast<int>(hits_.size()) && hitIndex == hoverHit_ &&
                             hits_[hitIndex].type == Hit::Type::MenuRow;
        D2D1_RECT_F rowRect{float(inset_ + 5), y, float(inset_ + contentW_ - 5), y + rowH};
        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> accent(ui_->Brush(d2d, AccentColor()));
            const float radius = Dip(5);
            d2d->FillRoundedRectangle(D2D1::RoundedRect(rowRect, radius, radius), accent.get());
        }
        const Ink rowInk = hovered ? Ink{0.0f} : ink;
        if (item.checked) {
            ComPtr<ID2D1SolidColorBrush> b(ui_->Brush(d2d, rowInk.text()));
            ui_->DrawGlyph(d2d, fluent::Check, Dip(12),
                           D2D1::RectF(float(checkX), y + rowH * 0.28f, float(checkX) + Dip(12),
                                       y + rowH * 0.72f),
                           b.get());
        }
        const float textY = y;
        D2D1_RECT_F labelRect{float(labelX), textY, float(right), textY + rowH};
        // If the item is not enabled, lower contrast.
        if (!item.enabled) {
            ComPtr<ID2D1SolidColorBrush> b(ui_->Brush(d2d, rowInk.secondary(0.65f)));
            ui_->DrawText(d2d, item.label, fs, DWRITE_FONT_WEIGHT_MEDIUM, labelRect, b.get());
        } else {
            ui_->DrawTextWithHalo(d2d, item.label, fs, DWRITE_FONT_WEIGHT_MEDIUM, labelRect, rowInk);
        }
        y += rowH;
        ++hitIndex;
    }
}

void PopupWindow::DrawControlCenter(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    const SystemStatus& st = host_->status();
    const MediaInfo& mediaInfo = host_->mediaInfo();
    const int pad = static_cast<int>(Dip(14));
    const int gap = static_cast<int>(Dip(10));
    const int pillGap = static_cast<int>(Dip(8));
    const int pillH = static_cast<int>(Dip(58));
    const int innerW = contentW_ - pad * 2;
    const int colGap = static_cast<int>(Dip(10));
    const int leftW = static_cast<int>(Dip(146));
    const int rightW = innerW - colGap - leftW;
    const int x0 = inset_ + pad;
    const int topY = inset_ + pad;
    const int topH = 3 * pillH + 2 * pillGap;
    const int rightX = x0 + leftW + colGap;

    const MaterialParams cardMaterial = ComputeMaterial(s, scale_, s.barHeight, false);
    const float cardAlpha = ink.isDark() ? 0.10f + 0.32f * cardMaterial.tintMix
                                         : 0.035f + 0.12f * cardMaterial.tintMix;
    const float sheenAlpha = 0.045f + 0.10f * cardMaterial.highlight;
    const float rimAlpha = 0.05f + 0.14f * cardMaterial.highlight;
    auto drawCard = [&](const RECT& r, float radiusOverride = 0.0f) {
        const float rad = radiusOverride > 0.0f ? radiusOverride : (float(r.bottom) - float(r.top)) * 0.5f;
        const D2D1_ROUNDED_RECT shape =
            D2D1::RoundedRect(D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)), rad, rad);
        ComPtr<ID2D1SolidColorBrush> base(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, cardAlpha)));
        d2d->FillRoundedRectangle(shape, base.get());
        D2D1_GRADIENT_STOP stops[2]{};
        stops[0].position = 0.0f;
        stops[0].color = D2D1::ColorF(1, 1, 1, sheenAlpha);
        stops[1].position = 1.0f;
        stops[1].color = D2D1::ColorF(1, 1, 1, 0.0f);
        ComPtr<ID2D1GradientStopCollection> collection;
        d2d->CreateGradientStopCollection(stops, 2, collection.put());
        if (collection) {
            ComPtr<ID2D1LinearGradientBrush> sheen;
            const D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES props = D2D1::LinearGradientBrushProperties(
                D2D1::Point2F(static_cast<float>(r.left), static_cast<float>(r.top)),
                D2D1::Point2F(static_cast<float>(r.left), static_cast<float>(r.top) + (r.bottom - r.top) * 0.55f));
            d2d->CreateLinearGradientBrush(props, collection.get(), sheen.put());
            if (sheen) d2d->FillRoundedRectangle(shape, sheen.get());
        }
        ComPtr<ID2D1SolidColorBrush> line(ui_->Brush(
            d2d, ink.isDark() ? D2D1::ColorF(0, 0, 0, rimAlpha) : D2D1::ColorF(1, 1, 1, rimAlpha)));
        d2d->DrawRoundedRectangle(shape, line.get(), 1.0f);
    };

    ComPtr<ID2D1SolidColorBrush> titleBrush(ui_->Brush(d2d, ink.text()));
    ComPtr<ID2D1SolidColorBrush> secondaryBrush(ui_->Brush(d2d, ink.secondary()));

    // Left: Wi-Fi, Bluetooth and Focus pills.
    const wchar_t topGlyphs[3] = {fluent::Wifi, fluent::Bluetooth, fluent::FocusSolid};
    const wchar_t* topTitlesZh[3] = {L"Wi-Fi", L"蓝牙", L"专注模式"};
    const wchar_t* topTitlesEn[3] = {L"Wi-Fi", L"Bluetooth", L"Focus"};
    const bool wifiOn = st.radioStateKnown ? st.wifiRadioOn : st.wifiConnected;
    const bool bluetoothOn = st.radioStateKnown ? st.bluetoothRadioOn : false;
    const bool focusOn = host_->focusOn();
    std::wstring topSubs[3] = {
        !wifiOn ? (zh ? L"已关闭" : L"Off")
                : st.wifiConnected ? st.wifiSsid : (zh ? L"未连接" : L"Not connected"),
        bluetoothOn ? (zh ? L"已开启" : L"On") : (zh ? L"已关闭" : L"Off"),
        focusOn ? (zh ? L"已开启" : L"On") : (zh ? L"已关闭" : L"Off"),
    };
    for (int i = 0; i < 3; ++i) {
        const int yy = topY + i * (pillH + pillGap);
        RECT r{x0, yy, x0 + leftW, yy + pillH};
        drawCard(r);
        const bool active = i == 0 ? wifiOn : i == 1 ? bluetoothOn : focusOn;
        ComPtr<ID2D1SolidColorBrush> iconBrush(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, active ? 0.95f : 0.72f)));
        ComPtr<ID2D1SolidColorBrush> discBrush(
            ui_->Brush(d2d, active ? AccentColor() : D2D1::ColorF(0.46f, 0.46f, 0.48f, 0.88f)));
        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(float(x0) + Dip(27), float(yy) + float(pillH) * 0.5f),
                                       Dip(17.5), Dip(17.5)),
                         discBrush.get());
        // Circle grows 125%; the glyph itself stays at the previous 22 DIP.
        // The Wi-Fi glyph stays Wi-Fi when off; only the disc turns grey.
        wchar_t glyph = topGlyphs[i];
        if (i == 0 && wifiToggleStartMs_ >= 0) {
            const double elapsed = NowMs() - wifiToggleStartMs_;
            if (elapsed >= 0 && elapsed < 300.0) {
                // Four strength states, 75 ms each: small → large.
                const wchar_t frames[4] = {fluent::Wifi1, fluent::Wifi2, fluent::Wifi3, fluent::Wifi};
                glyph = frames[std::clamp(static_cast<int>(elapsed / 75.0), 0, 3)];
            } else {
                wifiToggleStartMs_ = -1;
            }
        }
        ui_->DrawGlyph(d2d, glyph, Dip(22),
                       D2D1::RectF(float(x0) + Dip(16), float(yy) + float(pillH) * 0.5f - Dip(11),
                                   float(x0) + Dip(38), float(yy) + float(pillH) * 0.5f + Dip(11)),
                       iconBrush.get());
        const float titleH = Dip(18);
        const float subtitleH = Dip(16);
        const float textTop = float(yy) + (float(pillH) - titleH - subtitleH) * 0.5f;
        const float textRight = float(x0 + leftW) - Dip(8);
        ui_->DrawText(d2d, zh ? topTitlesZh[i] : topTitlesEn[i], Dip(13), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                      D2D1::RectF(float(x0) + Dip(48), textTop, textRight, textTop + titleH),
                      titleBrush.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        ui_->DrawText(d2d, topSubs[i], Dip(10.5), DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(x0) + Dip(48), textTop + titleH, textRight,
                                  textTop + titleH + subtitleH),
                      secondaryBrush.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Right: large now-playing card.
    RECT media{rightX, topY, rightX + rightW, topY + topH};
    drawCard(media, Dip(26));
    const float mediaLeft = float(media.left);
    const float mediaTop = float(media.top);
    const float mediaRight = float(media.right);
    auto drawRoundedImage = [&](ID2D1Bitmap1* bitmap, const D2D1_RECT_F& dst, float radiusDip, int srcW,
                                int srcH) {
        if (!bitmap || srcW <= 0 || srcH <= 0) return;
        ComPtr<ID2D1BitmapBrush> brush;
        const D2D1_BITMAP_BRUSH_PROPERTIES brushProps =
            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP);
        const D2D1_BRUSH_PROPERTIES brushConfig = D2D1::BrushProperties(1.0f);
        if (FAILED(d2d->CreateBitmapBrush(bitmap, &brushProps, &brushConfig, brush.put())) || !brush) return;
        const float dw = dst.right - dst.left;
        const float dh = dst.bottom - dst.top;
        ComPtr<ID2D1RoundedRectangleGeometry> rounded;
        const float radius = radiusDip / std::max(1.0f, dw) * static_cast<float>(srcW);
        gpu_->d2dFactory()->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(D2D1::RectF(0, 0, static_cast<float>(srcW), static_cast<float>(srcH)), radius, radius),
            rounded.put());
        if (!rounded) return;
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(dw / static_cast<float>(srcW),
                                                  dh / static_cast<float>(srcH)) *
                          D2D1::Matrix3x2F::Translation(dst.left, dst.top));
        d2d->FillGeometry(rounded.get(), brush.get());
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> edge(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.18f)));
        d2d->DrawRoundedRectangle(D2D1::RoundedRect(dst, radiusDip, radiusDip), edge.get(), 1.0f);
    };

    // Cover 125% of the previous 44 DIP, app icon 50% of the previous 44 DIP.
    const D2D1_RECT_F coverRect{mediaLeft + Dip(12), mediaTop + Dip(12), mediaLeft + Dip(67),
                                mediaTop + Dip(67)};
    drawRoundedImage(MediaCoverBitmap(d2d, mediaInfo), coverRect, Dip(10), mediaInfo.coverWidth,
                     mediaInfo.coverHeight);
    if (!MediaCoverBitmap(d2d, mediaInfo)) {
        ComPtr<ID2D1SolidColorBrush> placeholder(ui_->Brush(d2d, AccentColor()));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(coverRect, Dip(10), Dip(10)), placeholder.get());
        ComPtr<ID2D1SolidColorBrush> white(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.95f)));
        ui_->DrawGlyph(d2d, fluent::MusicNote, Dip(28),
                       D2D1::RectF(coverRect.left + Dip(12), coverRect.top + Dip(12),
                                   coverRect.right - Dip(12), coverRect.bottom - Dip(12)),
                       white.get());
    }

    const D2D1_RECT_F appIconRect{mediaRight - Dip(34), mediaTop + Dip(12), mediaRight - Dip(12),
                                  mediaTop + Dip(34)};
    auto* appIcon = MediaAppIconBitmap(d2d, mediaInfo);
    // The source-application badge is meaningful only while audio is actually
    // playing and we could resolve an icon; otherwise leave the corner empty.
    if (appIcon && mediaInfo.active && mediaInfo.playing) {
        drawRoundedImage(appIcon, appIconRect, Dip(4), mediaInfo.appIconWidth, mediaInfo.appIconHeight);
    }

    auto ellipsize = [&](std::wstring text, float px, DWRITE_FONT_WEIGHT weight, float width) {
        if (ui_->Measure(text, px, weight) <= width) return text;
        const std::wstring dots = L"…";
        while (!text.empty() && ui_->Measure(text + dots, px, weight) > width) text.pop_back();
        return text + dots;
    };
    const std::wstring title =
        mediaInfo.active && !mediaInfo.title.empty() ? mediaInfo.title : (zh ? L"未在播放" : L"Nothing playing");
    const std::wstring artist = !mediaInfo.artist.empty()
                                    ? mediaInfo.artist
                                    : (mediaInfo.active ? (zh ? L"系统媒体" : L"System media")
                                                        : (zh ? L"系统媒体" : L"System media"));
    const float textLeft = mediaLeft + Dip(12);
    const float textRight = mediaRight - Dip(12);
    ui_->DrawText(d2d, ellipsize(title, Dip(13.5), DWRITE_FONT_WEIGHT_SEMI_BOLD, textRight - textLeft),
                  Dip(13.5), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(textLeft, mediaTop + Dip(86), textRight, mediaTop + Dip(106)), titleBrush.get(),
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ui_->DrawText(d2d, ellipsize(artist, Dip(11), DWRITE_FONT_WEIGHT_MEDIUM, textRight - textLeft), Dip(11),
                  DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(textLeft, mediaTop + Dip(106), textRight, mediaTop + Dip(123)), secondaryBrush.get(),
                  DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    ComPtr<ID2D1SolidColorBrush> mediaBrush(ui_->Brush(d2d, ink.text()));
    const float buttonCenterY = mediaTop + Dip(158);
    const float transportCenter = mediaLeft + (mediaRight - mediaLeft) * 0.5f;
    const float transportGap = Dip(36);
    const wchar_t transport[3] = {fluent::Previous, mediaInfo.playing ? fluent::Pause : fluent::Play,
                                  fluent::Next};
    for (int i = 0; i < 3; ++i) {
        const float cx = transportCenter + (i - 1) * transportGap;
        ui_->DrawGlyph(d2d, transport[i], Dip(20),
                       D2D1::RectF(cx - Dip(10), buttonCenterY - Dip(10), cx + Dip(10),
                                   buttonCenterY + Dip(10)),
                       mediaBrush.get());
    }

    // Atrium-style sliders: a single wide pill filled from the left, with the
    // glyph sitting on the fill. No knob, no endpoint icons. The outer capsule
    // and the actual brightness/volume commands are unchanged.
    const int sliderY = topY + topH + gap;
    const int bh = static_cast<int>(Dip(72));
    auto drawSliderCard = [&](bool volume, int y) {
        RECT r{x0, y, x0 + innerW, y + bh};
        drawCard(r, Dip(20));
        const int value = volume ? st.volume : brightness_;
        // Text is intentionally 10 px further right than the previous layout.
        ui_->DrawText(d2d, volume ? (zh ? L"声音" : L"Sound") : (zh ? L"显示器" : L"Display"), Dip(12.5),
                      DWRITE_FONT_WEIGHT_SEMI_BOLD,
                      D2D1::RectF(float(x0) + Dip(24), float(y) + Dip(8), float(x0 + innerW) - Dip(60),
                                  float(y) + Dip(26)),
                      titleBrush.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        wchar_t pct[16];
        swprintf(pct, std::size(pct), L"%d%%", std::clamp(value, 0, 100));
        ui_->DrawText(d2d, pct, Dip(11.5), DWRITE_FONT_WEIGHT_NORMAL,
                      D2D1::RectF(float(x0 + innerW) - Dip(56), float(y) + Dip(8), float(x0 + innerW) - Dip(14),
                                  float(y) + Dip(26)),
                      secondaryBrush.get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

        const D2D1_RECT_F track{float(x0) + Dip(12), float(y) + Dip(38), float(x0 + innerW) - Dip(12),
                                float(y) + Dip(62)};
        const float trackH = track.bottom - track.top;
        const float radius = trackH * 0.5f;
        ComPtr<ID2D1SolidColorBrush> trackBrush(
            ui_->Brush(d2d, ink.isDark() ? D2D1::ColorF(0, 0, 0, 0.14f) : D2D1::ColorF(1, 1, 1, 0.18f)));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(track, radius, radius), trackBrush.get());
        const float k = static_cast<float>(std::clamp(value, 0, 100)) / 100.0f;
        const float glyphSize = trackH * 0.5f;
        const float glyphCenterY = (track.top + track.bottom) * 0.5f;
        const float glyphCenterX = track.left + Dip(2) + trackH * 0.5f;
        // The zero point is the right tangent of a circle of the track radius
        // centred on the glyph. 100% remains the track's right edge.
        const float zeroX = glyphCenterX + trackH * 0.5f;
        const float fillRight = zeroX + (track.right - zeroX) * k;
        const D2D1_RECT_F fill{track.left, track.top, fillRight, track.bottom};
        ComPtr<ID2D1SolidColorBrush> fillBrush(ui_->Brush(d2d, AccentColor()));
        d2d->FillRoundedRectangle(D2D1::RoundedRect(fill, radius, radius), fillBrush.get());
        const D2D1_RECT_F glyph{glyphCenterX - glyphSize * 0.5f, glyphCenterY - glyphSize * 0.5f,
                                glyphCenterX + glyphSize * 0.5f, glyphCenterY + glyphSize * 0.5f};
        ComPtr<ID2D1SolidColorBrush> glyphBrush(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.97f)));
        const int clamped = std::clamp(value, 0, 100);
        const wchar_t sliderGlyph = volume
            ? (clamped <= 0 ? fluent::Volume0
               : clamped < 34 ? fluent::Volume1
               : clamped < 67 ? fluent::Volume2
                              : fluent::Volume3)
            : (clamped <= 50 ? fluent::LowerBrightness : fluent::Brightness);
        ui_->DrawGlyph(d2d, sliderGlyph, glyphSize, glyph, glyphBrush.get());
    };
    drawSliderCard(false, sliderY);
    drawSliderCard(true, sliderY + bh + gap);
}

void PopupWindow::DrawCalendar(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    const SYSTEMTIME& now = host_->status().now;
    const int pad = static_cast<int>(Dip(14));
    const int cellW = (contentW_ - pad * 2) / 7;
    const int gridTop = inset_ + static_cast<int>(Dip(164));
    const int cellH = static_cast<int>(Dip(34));
    ComPtr<ID2D1SolidColorBrush> title(ui_->Brush(d2d, ink.text()));
    ComPtr<ID2D1SolidColorBrush> secondary(ui_->Brush(d2d, ink.secondary()));
    ComPtr<ID2D1SolidColorBrush> accent(ui_->Brush(d2d, AccentColor()));
    ComPtr<ID2D1SolidColorBrush> white(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.97f)));

    // Large digital time including seconds.
    std::wstring timeText;
    wchar_t timeBuf[64]{};
    if (s.use24Hour) {
        swprintf(timeBuf, std::size(timeBuf), L"%02u:%02u:%02u", now.wHour, now.wMinute, now.wSecond);
        timeText = timeBuf;
    } else {
        int hour = now.wHour % 12;
        if (hour == 0) hour = 12;
        const wchar_t* suffix = now.wHour < 12 ? (zh ? L"上午" : L"AM") : (zh ? L"下午" : L"PM");
        swprintf(timeBuf, std::size(timeBuf), L"%d:%02u:%02u %s", hour, now.wMinute, now.wSecond, suffix);
        timeText = timeBuf;
    }
    ui_->DrawText(d2d, timeText, Dip(43), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(float(inset_ + Dip(6)), float(inset_ + Dip(2)),
                              float(inset_ + contentW_ - Dip(6)), float(inset_ + Dip(76))),
                  title.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ui_->DrawText(d2d, FormatDateWithYear(now, s), Dip(12), DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(78)),
                              float(inset_ + contentW_ - pad), float(inset_ + Dip(102))),
                  secondary.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Mini calendar preview; its whole area opens the system Calendar app.
    ui_->DrawText(d2d, MonthTitle(s, calYear_, calMonth_), Dip(16), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(108)),
                              float(inset_ + contentW_ - pad), float(inset_ + Dip(136))),
                  title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    static const wchar_t* weekdaysZh[] = {L"日", L"一", L"二", L"三", L"四", L"五", L"六"};
    static const wchar_t* weekdaysEn[] = {L"Sun", L"Mon", L"Tue", L"Wed", L"Thu", L"Fri", L"Sat"};
    for (int i = 0; i < 7; ++i) {
        ui_->DrawText(d2d, zh ? weekdaysZh[i] : weekdaysEn[i], Dip(10.5), DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(inset_ + pad + i * cellW), float(inset_ + Dip(140)),
                                  float(inset_ + pad + (i + 1) * cellW), float(inset_ + Dip(162))),
                      secondary.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    const int firstWeekday = WeekdayOfFirst(calYear_, calMonth_);
    const int days = DaysInMonth(calYear_, calMonth_);
    const bool sameMonth = calMonth_ == now.wMonth && calYear_ == now.wYear;
    for (int i = 0; i < 42; ++i) {
        const int day = i - firstWeekday + 1;
        if (day < 1 || day > days) continue;
        const int col = i % 7;
        const int row = i / 7;
        RECT cell{inset_ + pad + col * cellW, gridTop + row * cellH, inset_ + pad + (col + 1) * cellW,
                  gridTop + (row + 1) * cellH};
        const bool isToday = sameMonth && day == now.wDay;
        if (isToday) {
            const float cx = (cell.left + cell.right) * 0.5f, cy = (cell.top + cell.bottom) * 0.5f;
            d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), Dip(12.5f), Dip(12.5f)), accent.get());
        }
        ComPtr<ID2D1SolidColorBrush> brush(ui_->Brush(d2d, isToday ? D2D1::ColorF(1, 1, 1, 1) : ink.text()));
        ui_->DrawText(d2d, std::to_wstring(day), Dip(12.5),
                      isToday ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(cell.left), float(cell.top), float(cell.right), float(cell.bottom)),
                      brush.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    // Hint under the preview.
    const std::wstring hint = zh ? L"打开系统日历" : L"Open system calendar";
    const float hintPx = Dip(10.5);
    const float hintW = ui_->Measure(hint, hintPx, DWRITE_FONT_WEIGHT_MEDIUM);
    const float hintTotal = hintW + Dip(18);
    const float hintX = float(inset_) + (float(contentW_) - hintTotal) * 0.5f;
    const float hintY = float(inset_ + contentH_) - Dip(26);
    ui_->DrawGlyph(d2d, fluent::Calendar, Dip(13),
                   D2D1::RectF(hintX, hintY, hintX + Dip(13), hintY + Dip(16)), secondary.get());
    ui_->DrawText(d2d, hint, hintPx, DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(hintX + Dip(18), hintY, hintX + hintTotal, hintY + Dip(16)),
                  secondary.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void PopupWindow::DrawVolumePanel(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    const SystemStatus& st = host_->status();
    const int pad = static_cast<int>(Dip(16));
    ComPtr<ID2D1SolidColorBrush> title(ui_->Brush(d2d, ink.text()));
    ComPtr<ID2D1SolidColorBrush> secondary(ui_->Brush(d2d, ink.secondary()));
    ui_->DrawText(d2d, zh ? L"声音" : L"Sound", Dip(16), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(12)), float(inset_ + contentW_ - pad),
                              float(inset_ + Dip(42))),
                  title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    const float trackY = float(inset_ + Dip(75));
    const float trackL = float(inset_ + pad + Dip(34));
    const float trackR = float(inset_ + contentW_ - pad - Dip(2));
    ComPtr<ID2D1SolidColorBrush> track(ui_->Brush(
        d2d, ink.isDark() ? D2D1::ColorF(0, 0, 0, 0.10f) : D2D1::ColorF(1, 1, 1, 0.22f)));
    d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackL, trackY - Dip(3), trackR, trackY + Dip(3)), Dip(3),
                                                 Dip(3)), track.get());
    const float t = static_cast<float>(std::clamp(st.volume, 0, 100)) / 100.0f;
    const float fillR = trackL + (trackR - trackL) * t;
    ComPtr<ID2D1SolidColorBrush> fill(ui_->Brush(d2d, D2D1::ColorF(0.04f, 0.52f, 1.0f, 0.95f)));
    d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(trackL, trackY - Dip(3), fillR, trackY + Dip(3)), Dip(3),
                                                 Dip(3)), fill.get());
    ComPtr<ID2D1SolidColorBrush> knobLine(ui_->Brush(d2d, D2D1::ColorF(0, 0, 0, 0.16f)));
    ComPtr<ID2D1SolidColorBrush> knob(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.98f)));
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillR, trackY), Dip(7), Dip(7)), knobLine.get());
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillR, trackY), Dip(6.2f), Dip(6.2f)), fill.get());
    d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(fillR, trackY), Dip(5.2f), Dip(5.2f)), knob.get());
    ComPtr<ID2D1SolidColorBrush> iconBrush(ui_->Brush(d2d, st.volumeMuted ? ink.secondary() : ink.text()));
    ui_->DrawGlyph(d2d, fluent::Volume, Dip(22),
                   D2D1::RectF(float(inset_ + pad), trackY - Dip(11), float(inset_ + pad + Dip(24)), trackY + Dip(11)),
                    iconBrush.get());
    ui_->DrawText(d2d, st.audioDevice.empty() ? (zh ? L"默认输出设备" : L"Default Output") : st.audioDevice, Dip(11),
                  DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + contentH_ - Dip(28)),
                              float(inset_ + contentW_ - pad), float(inset_ + contentH_ - Dip(8))),
                  secondary.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void PopupWindow::DrawInfoPanel(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    const SystemStatus& st = host_->status();
    const int pad = static_cast<int>(Dip(16));
    ComPtr<ID2D1SolidColorBrush> title(ui_->Brush(d2d, ink.text()));
    ComPtr<ID2D1SolidColorBrush> secondary(ui_->Brush(d2d, ink.secondary()));
    ComPtr<ID2D1SolidColorBrush> accent(ui_->Brush(d2d, AccentColor()));
    const wchar_t* heading = kind_ == PopupKind::Wifi ? L"Wi-Fi"
                            : kind_ == PopupKind::Battery ? (zh ? L"电池" : L"Battery")
                                                          : (zh ? L"通知" : L"Notifications");
    ui_->DrawText(d2d, heading, Dip(16), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(12)), float(inset_ + contentW_ - pad),
                              float(inset_ + Dip(42))),
                  title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (kind_ == PopupKind::Wifi) {
        const D2D1_RECT_F icon{float(inset_ + pad), float(inset_ + Dip(50)), float(inset_ + pad + Dip(24)),
                               float(inset_ + Dip(74))};
        ComPtr<ID2D1SolidColorBrush> b(ui_->Brush(d2d, ink.text()));
        ui_->DrawGlyph(d2d, st.wifiConnected ? fluent::Wifi : fluent::Network, Dip(24), icon, b.get());
        ui_->DrawText(d2d, st.wifiConnected ? st.wifiSsid : (zh ? L"未连接" : L"Not Connected"), Dip(14),
                      DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(inset_ + pad + Dip(36)), float(inset_ + Dip(46)),
                                  float(inset_ + contentW_ - pad), float(inset_ + Dip(68))),
                      title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        if (st.wifiConnected) {
            wchar_t buf[64];
            swprintf(buf, std::size(buf), zh ? L"信号强度 %d%%" : L"Signal %d%%", st.wifiSignal);
            ui_->DrawText(d2d, buf, Dip(11.5), DWRITE_FONT_WEIGHT_MEDIUM,
                          D2D1::RectF(float(inset_ + pad + Dip(36)), float(inset_ + Dip(68)),
                                      float(inset_ + contentW_ - pad), float(inset_ + Dip(88))),
                          secondary.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
    } else if (kind_ == PopupKind::Battery) {
        wchar_t buf[64] = {};
        swprintf(buf, std::size(buf), L"%d%%", st.batteryPercent < 0 ? 0 : st.batteryPercent);
        ui_->DrawText(d2d, buf, Dip(34), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                      D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(48)), float(inset_ + contentW_ - pad),
                                  float(inset_ + Dip(96))),
                      title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        std::wstring state = st.batteryCharging ? (zh ? L"正在充电" : L"Charging") : (zh ? L"使用电池" : L"On Battery");
        ui_->DrawText(d2d, state, Dip(11.5), DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(88)), float(inset_ + contentW_ - pad),
                                  float(inset_ + Dip(112))),
                      secondary.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    } else {
        ui_->DrawText(d2d, zh ? L"没有新通知" : L"No New Notifications", Dip(14), DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(60)), float(inset_ + contentW_ - pad),
                                  float(inset_ + Dip(96))),
                      title.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // Primary button.
    const RECT br = hits_.empty() ? RECT{} : hits_.back().rect;
    if (br.right > br.left) {
        d2d->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(float(br.left), float(br.top), float(br.right),
                                                                 float(br.bottom)), Dip(9), Dip(9)), accent.get());
        ComPtr<ID2D1SolidColorBrush> white(ui_->Brush(d2d, D2D1::ColorF(1, 1, 1, 0.98f)));
        ui_->DrawText(d2d, zh ? L"打开设置" : L"Open Settings", Dip(12), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                      D2D1::RectF(float(br.left), float(br.top), float(br.right), float(br.bottom)), white.get(),
                      DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

void PopupWindow::DrawNotifications(ID2D1DeviceContext2* d2d) {
    DrawInfoPanel(d2d);
}

std::vector<const TrayIcon*> PopupWindow::HiddenTray() const {
    std::vector<const TrayIcon*> hidden;
    if (!host_) return hidden;
    for (const auto& icon : host_->trayIcons()) {
        if (!IsTrayIconShown(icon, host_->settings())) hidden.push_back(&icon);
    }
    return hidden;
}

ID2D1Bitmap1* PopupWindow::TrayBitmap(ID2D1DeviceContext2* d2d, const TrayIcon& icon) {
    if (icon.imageId == 0 || icon.pixels.empty() || icon.width <= 0 || icon.height <= 0) return nullptr;
    auto it = trayBitmaps_.find(icon.imageId);
    if (it != trayBitmaps_.end()) return it->second.get();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    const HRESULT hr =
        d2d->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(icon.width), static_cast<UINT32>(icon.height)),
                          icon.pixels.data(), static_cast<UINT32>(icon.width * 4), props, bitmap.put());
    if (FAILED(hr)) return nullptr;
    auto [ins, ok] = trayBitmaps_.emplace(icon.imageId, std::move(bitmap));
    return ins->second.get();
}

ID2D1Bitmap1* PopupWindow::MediaAppIconBitmap(ID2D1DeviceContext2* d2d, const MediaInfo& media) {
    if (!media.active || media.appIconRevision == 0 || media.appIconPixels.empty() || media.appIconWidth <= 0 ||
        media.appIconHeight <= 0) {
        if (mediaAppIconBitmap_) {
            mediaAppIconBitmap_.reset();
            mediaAppIconRevision_ = 0;
        }
        return nullptr;
    }
    if (mediaAppIconBitmap_ && mediaAppIconRevision_ == media.appIconRevision) return mediaAppIconBitmap_.get();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    const HRESULT hr = d2d->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(media.appIconWidth), static_cast<UINT32>(media.appIconHeight)),
        media.appIconPixels.data(), static_cast<UINT32>(media.appIconWidth * 4), props, bitmap.put());
    if (FAILED(hr)) return nullptr;
    mediaAppIconBitmap_ = std::move(bitmap);
    mediaAppIconRevision_ = media.appIconRevision;
    return mediaAppIconBitmap_.get();
}

ID2D1Bitmap1* PopupWindow::MediaCoverBitmap(ID2D1DeviceContext2* d2d, const MediaInfo& media) {
    if (!media.active || media.coverRevision == 0 || media.coverPixels.empty() || media.coverWidth <= 0 ||
        media.coverHeight <= 0) {
        if (mediaCoverBitmap_) {
            mediaCoverBitmap_.reset();
            mediaCoverRevision_ = 0;
        }
        return nullptr;
    }
    if (mediaCoverBitmap_ && mediaCoverRevision_ == media.coverRevision) return mediaCoverBitmap_.get();
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
    ComPtr<ID2D1Bitmap1> bitmap;
    const HRESULT hr = d2d->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(media.coverWidth), static_cast<UINT32>(media.coverHeight)),
        media.coverPixels.data(), static_cast<UINT32>(media.coverWidth * 4), props, bitmap.put());
    if (FAILED(hr)) return nullptr;
    mediaCoverBitmap_ = std::move(bitmap);
    mediaCoverRevision_ = media.coverRevision;
    return mediaCoverBitmap_.get();
}

void PopupWindow::DrawTray(ID2D1DeviceContext2* d2d) {
    const Ink ink{popupInk_};
    const Settings& s = host_->settings();
    const bool zh = s.language != L"en-US";
    const int pad = static_cast<int>(Dip(14));
    const int header = static_cast<int>(Dip(34));
    const int cellW = (contentW_ - pad * 2) / 5;
    const int cellH = static_cast<int>(Dip(56));
    ComPtr<ID2D1SolidColorBrush> title(ui_->Brush(d2d, ink.text()));
    ComPtr<ID2D1SolidColorBrush> secondary(ui_->Brush(d2d, ink.secondary()));
    ui_->DrawText(d2d, zh ? L"系统托盘" : L"System Tray", Dip(14), DWRITE_FONT_WEIGHT_SEMI_BOLD,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + Dip(9)), float(inset_ + contentW_ - pad),
                              float(inset_ + Dip(31))),
                  title.get(), DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    ui_->DrawText(d2d, zh ? L"隐藏的图标" : L"Hidden icons", Dip(10.5), DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(float(inset_ + contentW_ - Dip(150)), float(inset_ + Dip(10)),
                              float(inset_ + contentW_ - pad), float(inset_ + Dip(31))),
                  secondary.get(), DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    const auto hidden = HiddenTray();
    if (hidden.empty()) {
        ui_->DrawText(d2d, zh ? L"没有隐藏的图标" : L"No hidden icons", Dip(13), DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(float(inset_ + pad), float(inset_ + header), float(inset_ + contentW_ - pad),
                                  float(inset_ + contentH_ - pad)),
                      secondary.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        return;
    }
    for (size_t i = 0; i < hidden.size(); ++i) {
        const int col = static_cast<int>(i % 5);
        const int row = static_cast<int>(i / 5);
        const float left = float(inset_ + pad + col * cellW);
        const float top = float(inset_ + header + row * cellH);
        const D2D1_RECT_F cell{left, top, left + float(cellW), top + float(cellH)};
        const bool hovered = static_cast<int>(i) == hoverHit_;
        if (hovered) {
            ComPtr<ID2D1SolidColorBrush> wash(ui_->Brush(d2d, ink.wash(0.03f)));
            d2d->FillRoundedRectangle(D2D1::RoundedRect(cell, Dip(8), Dip(8)), wash.get());
        }
        const float iconBox = Dip(26);
        const float ix = left + (float(cellW) - iconBox) * 0.5f;
        const float iy = top + Dip(7);
        ID2D1Bitmap1* bitmap = TrayBitmap(d2d, *hidden[i]);
        if (bitmap) {
            d2d->DrawBitmap(bitmap, D2D1::RectF(ix, iy, ix + iconBox, iy + iconBox), 0.98f,
                            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            ui_->DrawGlyph(d2d, 0xE7C3, iconBox * 1.2f, D2D1::RectF(ix, iy, ix + iconBox, iy + iconBox),
                           secondary.get());
        }
        std::wstring label = hidden[i]->label.empty() ? (zh ? L"应用" : L"App") : hidden[i]->label;
        const float labelPx = Dip(10.0);
        while (label.size() > 1 && ui_->Measure(label, labelPx, DWRITE_FONT_WEIGHT_MEDIUM) > cellW - Dip(6)) {
            label.resize(label.size() - 1);
        }
        if (!hidden[i]->label.empty() && label != hidden[i]->label) label += L"…";
        ui_->DrawText(d2d, label, labelPx, DWRITE_FONT_WEIGHT_MEDIUM,
                      D2D1::RectF(left + Dip(3), top + Dip(35), left + float(cellW) - Dip(3),
                                  top + float(cellH) - Dip(2)),
                      title.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }

    // The cells elide long names; show the complete name in the footer while
    // the pointer is over a cell.
    std::wstring hoveredName;
    for (size_t i = 0; i < hits_.size() && i < hidden.size(); ++i) {
        if (static_cast<int>(i) == hoverHit_ && hits_[i].type == Hit::Type::TrayButton) {
            hoveredName = hidden[i]->label;
            break;
        }
    }
    if (hoveredName.empty()) hoveredName = zh ? L"悬停查看完整名称" : L"Hover for full names";
    ui_->DrawText(d2d, hoveredName, Dip(10.0), DWRITE_FONT_WEIGHT_MEDIUM,
                  D2D1::RectF(float(inset_ + pad), float(inset_ + contentH_) - Dip(21),
                              float(inset_ + contentW_ - pad), float(inset_ + contentH_) - Dip(4)),
                  secondary.get(), DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

}  // namespace sm
