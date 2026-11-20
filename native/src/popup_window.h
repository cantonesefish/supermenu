#pragma once

#include "common.h"
#include "capture.h"
#include "glass_surface.h"
#include "gpu.h"
#include "icons.h"
#include "media_monitor.h"
#include "menu.h"
#include "settings.h"
#include "system_status.h"
#include "tray.h"
#include "ui_util.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace sm {

enum class PopupKind { None, Menu, ControlCenter, Calendar, Wifi, Battery, Volume, Notifications, Tray };

class PopupHost {
public:
    virtual ~PopupHost() = default;
    virtual const Settings& settings() const = 0;
    virtual const SystemStatus& status() const = 0;
    virtual MenuContext menuContext() const = 0;
    virtual void OnPopupCommand(Command command) = 0;
    virtual void OnPopupVolume(int percent) = 0;
    virtual void OnPopupBrightness(int percent) = 0;
    virtual void OnPopupMute() = 0;
    virtual void OnPopupCloseRequested() = 0;
    virtual const std::vector<TrayIcon>& trayIcons() const = 0;
    virtual const MediaInfo& mediaInfo() const = 0;
    virtual bool focusOn() const = 0;
    virtual void OnTrayIconActivated(const std::wstring& id) = 0;
    virtual void OnTrayIconContext(const std::wstring& id) = 0;
};

class PopupWindow {
public:
    PopupWindow() = default;
    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;
    ~PopupWindow();

    bool Open(PopupKind kind, MenuId menuId, const MenuModel& menu, RECT anchorScreen, Gpu* gpu, Capture* capture,
              PopupHost* host, Ui* ui, Icons* icons, const RECT& monitorRect);
    bool OpenPanel(PopupKind kind, RECT anchorScreen, Gpu* gpu, Capture* capture, PopupHost* host, Ui* ui,
                   Icons* icons, const RECT& monitorRect);
    void BeginClose();
    void CloseNow();
    void SetBrightness(int percent) { brightness_ = std::clamp(percent, 0, 100); }

    bool IsOpen() const { return hwnd_ != nullptr; }
    bool IsVisible() const { return visible_; }
    PopupKind kind() const { return kind_; }
    MenuId menuId() const { return menuId_; }
    bool closing() const { return closing_; }
    bool animating() const { return animating_; }
    double openStartMs() const { return openStartMs_; }
    void BeginWifiToggleAnimation() { wifiToggleStartMs_ = NowMs(); }
    bool wifiToggleAnimating() const {
        // Draw animation lasts 300 ms; keep one static frame after it.
        return wifiToggleStartMs_ >= 0 && NowMs() - wifiToggleStartMs_ < 360.0;
    }

    bool UpdateBackdrop();
    void Render();
    void Tick(double nowMs);
    bool ContainsScreenPoint(POINT pt) const;
    RECT screenRect() const;
    void HandleKey(int vk);

    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

private:
    struct Hit {
        enum class Type { None, MenuRow, Button, VolumeSlider, BrightnessSlider, CalendarPrev, CalendarNext, CalendarDay, Mute, TrayButton } type = Type::None;
        RECT rect{};  // local window px
        Command command = CmdNone;
        int value = 0;
        std::wstring uri;  // button target
        std::wstring trayId;
    };

    bool CreateWindowOnly(Gpu*, Capture*, PopupHost*, Ui*, Icons*, const RECT& monitorRect);
    void ComputeContentSize();
    void PositionAndResize(RECT anchorScreen);
    void BuildLayout();
    void DrawContent(ID2D1DeviceContext2* d2d);
    void DrawMenu(ID2D1DeviceContext2* d2d);
    void DrawControlCenter(ID2D1DeviceContext2* d2d);
    void DrawCalendar(ID2D1DeviceContext2* d2d);
    void DrawInfoPanel(ID2D1DeviceContext2* d2d);
    void DrawVolumePanel(ID2D1DeviceContext2* d2d);
    void DrawNotifications(ID2D1DeviceContext2* d2d);
    void DrawTray(ID2D1DeviceContext2* d2d);
    ID2D1Bitmap1* TrayBitmap(ID2D1DeviceContext2* d2d, const TrayIcon& icon);
    ID2D1Bitmap1* MediaCoverBitmap(ID2D1DeviceContext2* d2d, const MediaInfo& media);
    ID2D1Bitmap1* MediaAppIconBitmap(ID2D1DeviceContext2* d2d, const MediaInfo& media);
    std::vector<const TrayIcon*> HiddenTray() const;
    Hit HitTest(POINT client) const;
    void OnClick(POINT client);
    void SetSliderFromPoint(POINT client, bool volume);
    GlassParams MakeParams() const;
    int ContentLeft() const { return inset_; }
    int ContentTop() const { return inset_; }
    float Dip(double v) const { return static_cast<float>(Pxf(v, scale_)); }

    HINSTANCE hinst_ = nullptr;
    HWND hwnd_ = nullptr;
    PopupKind kind_ = PopupKind::None;
    MenuId menuId_ = MenuId::None;
    MenuModel menu_;
    RECT monitorRect_{};
    RECT anchor_{};
    RECT windowRect_{};
    int inset_ = 12;
    double scale_ = 1.0;
    int contentW_ = 200;
    int contentH_ = 200;
    int width_ = 0, height_ = 0;
    Gpu* gpu_ = nullptr;
    Capture* capture_ = nullptr;
    PopupHost* host_ = nullptr;
    Ui* ui_ = nullptr;
    Icons* icons_ = nullptr;
    std::unique_ptr<GlassSurface> surface_;
    std::vector<Hit> hits_;
    std::vector<RECT> ccCardRects_;  // window-local glass/hit islands (no outer container)
    std::map<uint64_t, ComPtr<ID2D1Bitmap1>> trayBitmaps_;
    ComPtr<ID2D1Bitmap1> mediaCoverBitmap_;
    unsigned long long mediaCoverRevision_ = 0;
    ComPtr<ID2D1Bitmap1> mediaAppIconBitmap_;
    unsigned long long mediaAppIconRevision_ = 0;
    int hoverRow_ = -1;
    Hit::Type dragging_ = Hit::Type::None;
    bool visible_ = false;
    bool closing_ = false;
    bool animating_ = false;
    bool backdropValid_ = false;
    double openStartMs_ = 0;
    double closeStartMs_ = 0;
    double wifiToggleStartMs_ = -1;
    int clockSecond_ = -1;
    bool mouseTracking_ = false;
    int hoverHit_ = -1;
    float popupInk_ = 0.5f;
    bool pendingInkInit_ = true;
    int brightness_ = 70;

    // Calendar local state
    int calYear_ = 0, calMonth_ = 0, calSelectedDay_ = 0, calSelectedMonth_ = 0, calSelectedYear_ = 0;
    int hoverDay_ = -1;
};

}  // namespace sm
