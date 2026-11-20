#pragma once

#include "common.h"
#include "gpu.h"
#include "capture.h"
#include "glass_surface.h"
#include "menu.h"
#include "settings.h"
#include "system_status.h"
#include "tray.h"
#include "ui_util.h"
#include "icons.h"

#include <map>
#include <memory>

namespace sm {

class BarHost {
public:
    virtual ~BarHost() = default;
    virtual const Settings& settings() const = 0;
    virtual const SystemStatus& status() const = 0;
    virtual MenuContext menuContext() const = 0;
    virtual void OnBarItem(MenuId id, RECT anchorScreen) = 0;
    virtual void OnBarContextMenu(POINT screenPoint) = 0;
    virtual void OnBarHover(MenuId id) = 0;
    virtual void OnBarEmptyClick() = 0;
    virtual MenuId openMenu() const = 0;
    virtual bool popupVisible() const = 0;
    virtual bool controlCenterOpen() const = 0;
    virtual bool trayOverflowOpen() const = 0;
    virtual const std::vector<TrayIcon>& trayIcons() const = 0;
    virtual void OnTrayIconActivated(const std::wstring& id) = 0;
    virtual void OnTrayIconContext(const std::wstring& id) = 0;
    virtual void OnTrayOverflow(RECT anchorScreen) = 0;
    virtual void OnDisplayChanged() = 0;
    virtual void OnOpenSettings() = 0;
    virtual void OnQuitRequested() = 0;
};

class BarWindow {
public:
    BarWindow() = default;
    BarWindow(const BarWindow&) = delete;
    BarWindow& operator=(const BarWindow&) = delete;
    ~BarWindow();

    bool Create(HINSTANCE hinst, Gpu* gpu, Capture* capture, BarHost* host, const MonitorInfo& monitor);
    void Destroy();

    bool UpdateBackdrop(bool updateLuma);
    void Render(bool force);
    bool animating() const { return animating_; }
    bool contentDirty() const { return contentDirty_; }
    void Invalidate() { contentDirty_ = true; }

    // Re-reads settings dimensions and repositions/resizes the window.
    void ApplySettings();
    void RegisterAppBar(bool reserve);
    void Show(bool show);
    void HideTrayTooltipNow() { HideTrayTooltip(); }

    HWND hwnd() const { return hwnd_; }
    Ui* ui() const { return ui_; }
    Icons* icons() const { return icons_; }
    UINT barHeightPx() const { return barHeight_; }
    int shadowPadPx() const { return shadowPad_; }
    RECT plateScreenRect(MenuId id) const;
    RECT windowScreenRect() const;
    const std::vector<MenuModel>& topMenus() const { return topMenus_; }

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct BarItem {
        MenuId id = MenuId::None;
        bool status = false;
        D2D1_RECT_F plate{};  // physical window px
        D2D1_RECT_F text{};
        std::wstring label;
        float textPx = 13;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_MEDIUM;
        bool drawsIcon = false;
        bool apple = false;
        bool tray = false;
        bool overflow = false;
        bool statusCluster = false;
        std::wstring trayId;
        std::wstring tooltip;
        std::wstring trayLabel;
        uint64_t trayImageId = 0;
        int trayW = 0, trayH = 0;
        float hover = 0;
        std::vector<D2D1_RECT_F> hoverParts;  // per-glyph hover plates in the status cluster
        bool showBatteryPercent = false;
        float iconSize = 16;
    };

    void Layout(float width, float barHeight, double scale);
    void DrawContent(ID2D1DeviceContext2* d2d);
    void UpdateHoverTargets();
    void UpdateInk(double dt);
    BarItem* FindItem(POINT client);
    void EnsureMouseTracking();
    GlassParams MakeParams() const;
    void PositionWindow();
    RECT ToScreen(const D2D1_RECT_F& r) const;
    ID2D1Bitmap1* TrayBitmap(ID2D1DeviceContext2* d2d, const TrayIcon& icon);
    void DrawTrayIcon(ID2D1DeviceContext2* d2d, const BarItem& item, ID2D1Brush* fallbackBrush);
    void UpdateTrayTooltip(const BarItem* item);
    void HideTrayTooltip();

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND tooltip_ = nullptr;
    std::wstring tooltipKey_;
    Gpu* gpu_ = nullptr;
    Capture* capture_ = nullptr;
    BarHost* host_ = nullptr;
    MonitorInfo monitor_{};
    std::unique_ptr<GlassSurface> surface_;
    Ui* ui_ = nullptr;
    Icons* icons_ = nullptr;
    std::vector<MenuModel> topMenus_;
    std::vector<BarItem> items_;
    MenuId hovered_ = MenuId::None;
    std::wstring hoveredTrayId_;
    bool hoveredOverflow_ = false;
    int hoveredStatusPart_ = -1;
    std::map<uint64_t, ComPtr<ID2D1Bitmap1>> trayBitmaps_;
    UINT barHeight_ = 28;
    int shadowPad_ = 16;
    bool mouseTracking_ = false;
    bool contentDirty_ = true;
    bool animating_ = false;
    bool appBarRegistered_ = false;
    double lastFrameMs_ = 0;
    double lastContentMs_ = 0;
    float inkLeft_ = 0.7f, inkRight_ = 0.7f, inkClock_ = 0.7f;
    bool inkLeftInit_ = false, inkRightInit_ = false, inkClockInit_ = false;
    bool firstFrame_ = true;
    std::map<MenuId, float> hoverVel_;
};

}  // namespace sm
