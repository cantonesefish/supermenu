#pragma once

// Windows 11 notification-area ("tray") icon metadata reader.
//
// Explorer persists per-icon state below
//     HKCU\Control Panel\NotifyIconSettings
// where every subkey is a decimal uint64 record id and carries an IconSnapshot
// PNG, an ExecutablePath (optionally redirected through a known-folder GUID),
// an InitialTooltip, an IsPromoted flag and (on some records) an IconGuid. The
// key's UIOrderList binary value lists the record ids as little-endian uint64 in
// Explorer's own display order.
//
// TrayReader turns that persistence data into a renderable list: decoded
// 32bpp premultiplied BGRA 32x32 icons, cleaned labels and absolute lower-case
// executable paths, ordered like Explorer and de-duplicated.
//
// Threading contract: the owning thread must already have called
// CoInitializeEx(COINIT_APARTMENTTHREADED). The constructor creates the WIC
// imaging factory once, and Refresh()/the destructor must run on that same
// thread.

#include "common.h"

#include <wincodec.h>

#include <unordered_map>

namespace atrium {
class TrayHost;
struct HostIcon;
}  // namespace atrium

namespace sm {

struct Settings;

struct TrayIcon {
    // Atrium tray-host identity. Non-empty means the icon was received from the
    // real Shell_NotifyIcon stream and can be clicked/hovered directly.
    std::wstring hostKey;
    bool hosted = false;
    std::wstring id;          // registry record id
    std::wstring label;       // cleaned InitialTooltip first line, else file description, else exe stem
    std::wstring exe;         // absolute lower-case path if resolved, else empty
    bool promoted = false;
    unsigned long long order = ~0ull;
    unsigned long long imageId = 0;  // stable hash of the decoded icon pixels
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;     // 32bpp BGRA, *premultiplied* alpha, width*height*4
    // Notification identity, used to prove an icon is currently registered.
    bool hasUid = false;
    unsigned uid = 0;
    GUID guid{};
    // Screen-space rectangle resolved from Shell_NotifyIconGetRect for a live,
    // visible tray icon. Used to forward real mouse clicks to Explorer's icon.
    RECT screenRect{};
    bool hasScreenRect = false;
    // Hidden records all resolve to the overflow chevron's shared rectangle;
    // clicking it opens Explorer's real hidden-icon panel.
    RECT overflowRect{};
    bool overflowProxy = false;
};

// Effective placement after the user's per-record overrides:
// trayHide wins, then trayShow, otherwise Explorer's IsPromoted.
bool IsTrayIconShown(const TrayIcon& icon, const Settings& settings);

class TrayReader {
public:
    TrayReader();
    ~TrayReader();

    // Re-reads HKCU\Control Panel\NotifyIconSettings and refreshes icons().
    // Returns true when the visible result changed (set/content/order/promotion/image).
    bool Refresh();
    const std::vector<TrayIcon>& icons() const { return icons_; }
    double lastReadMs() const { return lastReadMs_; }  // duration of the last Refresh() (diagnostics)
    // Atrium's hosted notification stream supplies live HICONs and direct click
    // identities. Registry records remain fallback metadata/order for Explorer-only
    // or not-yet-hosted icons.
    void SetHost(const atrium::TrayHost* host) { host_ = host; }

private:
    void MergeHosted(std::vector<TrayIcon>& icons);
    bool MakeHostedIcon(const atrium::HostIcon& host, TrayIcon& icon);
    bool EnsureWicFactory();
    struct CachedImage {
        unsigned long long snapshotHash = 0;
        std::wstring exe;
        std::vector<uint8_t> pixels;
        unsigned long long imageId = 0;
    };

    std::vector<TrayIcon> icons_;
    ComPtr<IWICImagingFactory> wic_;
    // Decoded icons and labels only change when the registry snapshot, the
    // executable path or the executable's version resource changes, so keep
    // them across Refresh() calls. The first Refresh() still decodes
    // everything; later calls only re-read the lightweight registry metadata.
    std::unordered_map<std::wstring, CachedImage> imageCache_;
    std::unordered_map<std::wstring, std::wstring> descriptionCache_;  // exe -> FileDescription
    std::unordered_map<std::wstring, std::wstring> folderCache_;       // "{GUID}" -> known folder
    // Live icons prefer the application's real icon over a possibly old shell
    // snapshot. Cached by executable path.
    std::unordered_map<std::wstring, std::vector<uint8_t>> executableIconCache_;
    double lastReadMs_ = 0.0;
    const atrium::TrayHost* host_ = nullptr;
};

}  // namespace sm
