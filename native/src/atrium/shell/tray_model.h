// The notification area's icons and the actions on them.
//
// Ported from Atrium Desktop (MIT, https://github.com/wynter-yaxley/atrium-desktop)
// and adapted to SuperMenu. Evidence contracts and the fallback order are
// documented in docs/TRAY.md.
//
// Two sources, one list:
//   - read-back: the buttons of Explorer's own tray (UI Automation on the XAML
//     taskbar and its overflow window, or the classic ToolbarWindow32), the
//     record of every icon Explorer keeps in
//     HKCU\Control Panel\NotifyIconSettings, and a liveness probe through
//     Shell_NotifyIconGetRect. Icons are reconciled by identity (GUID, HWND/uID,
//     or program) - never by position or name alone.
//   - host: SuperMenu's TrayHost receives the apps' Shell_NotifyIcon calls ahead
//     of Explorer and holds the live HICONs, callback messages and protocol
//     versions. Those icons are merged first, with Explorer's own order and
//     promoted/hidden state, and are clicked by sending the shell's callbacks.
//
// All UIA, registry, GetRect, process, picture and click-confirmation work runs
// on a worker thread (MTA). Click()/Refresh()/Hover() only queue work.
#pragma once

#include "icon_image.h"
#include "shell/tray_click.h"
#include "shell/tray_host.h"
#include "shell/tray_pointer.h"
#include "shell/tray_target.h"

#include <windows.h>

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct TrayIcon {
    std::wstring tip;  // the tooltip; may span lines
    std::wstring exe;  // the program it belongs to (full path, lower case), when recognised
    std::shared_ptr<const IconImage> image; // null when unknown
    uint64_t imageVersion = 0; // renderer cache identity; allocator addresses can be reused
    // A one-shade glyph (white or black, as many tray icons are drawn for the
    // taskbar's theme) redrawn in the other ink. Null for coloured icons.
    std::shared_ptr<const IconImage> inked;
    bool inkedOnLight = false;
    bool promoted = false; // on the system taskbar rather than in its overflow
    int index = 0;         // position among the icons of the same place
    std::vector<int> runtimeId; // UIA identity, never an ordinal click target
    tray::ShellSession shell;   // PID + process creation time + taskbar HWND
    std::wstring matchMethod;
    bool fromRecord = false;
    std::wstring recordId;  // identity: "guid:{...}", "uid:pid:hwnd:uid", "host:..."
    // The numeric NotifyIconSettings record key when known. SuperMenu's settings
    // keep per-record show/hide overrides by this id; empty for run-only icons.
    std::wstring recordKey;
    std::wstring hostKey; // an icon the tray host received (TrayHost); empty: read from Explorer
    DWORD pid = 0;        // its window's process (hosted)
    bool appTooltip = false; // a version 4 app drawing its own tooltip: no standard one
    bool flashing = false;   // its icon flashes (hosted): a message waiting
    size_t order = SIZE_MAX; // place in Windows' own order of the tray icons, if known
    tray::TargetKey Target() const { return {shell, runtimeId, recordId, exe, tip}; }
};

class TrayModel {
public:
    // Posts `msg` to `notify`. wParam is the event:
    //   0 = the list changed           1 = observed app response
    //   2 = the system tray was opened 3 = needs user intervention
    //   4 = accepted/unconfirmed (quiet; do not retry)
    // No HRESULT is treated as an application acknowledgement.
    TrayModel(HWND notify, UINT msg);
    ~TrayModel();
    TrayModel(const TrayModel&) = delete;
    TrayModel& operator=(const TrayModel&) = delete;

    void Refresh(); // asynchronous
    std::vector<TrayIcon> Icons() const;
    bool Loaded() const;
    bool Available() const; // the shell's tray was found in the last read

    // A left click (Invoke) or a right click (the context menu) on `icon`.
    void Click(const TrayIcon& icon, bool right);
    void DoubleClick(const TrayIcon& icon, bool continuation = false);
    void MiddleClick(const TrayIcon& icon);
    void OpenSystemTray(); // explicit asynchronous fallback (opens Explorer's overflow)
    // The pointer rests on `icon` or on none (null): native hover, and the
    // version 4 app's own popup request.
    void Hover(const TrayIcon* icon);
    // Where the bar's tray area is, for apps that ask (Shell_NotifyIconGetRect).
    void SetHostPlaces(RECT anchor);

    // The tray host (apps' icons as they send them, clicked with the shell's own
    // messages). UI thread; the host has a thread of its own.
    void SetHosting(bool on);
    bool Hosting() const;
    // Another program's tray host (Atrium, RetroBar...) holds the stream: the
    // read-back list is used and no TaskbarCreated is broadcast.
    bool Yielded() const;
    // The programs whose tray icon flashes now (hosted icons only): a message waiting.
    std::vector<std::wstring> Flashing() const;

    // First line of a tooltip, for display.
    static std::wstring Title(const std::wstring& tip);
    // GetLastInputInfo time of the last real pointer click sent to a tray button (0: none).
    static DWORD LastSyntheticInput();
    // True while the physical pointer right-click fallback is moving the cursor.
    static bool SyntheticPointerInFlight() { return tray::g_pointerInFlight; }

private:
    struct Job {
        bool refresh = false;
        TrayIcon icon;
        tray::Action action = tray::Action::Left;
        bool systemTray = false;
    };
    void Worker();
    bool ClickHosted(const TrayIcon& icon, tray::HostAction action);

    HWND notify_;
    UINT msg_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    std::vector<TrayIcon> icons_;
    bool loaded_ = false, available_ = false, stop_ = false;
    std::thread thread_;
    std::unique_ptr<TrayHost> host_;
    // Ink for a hosted one-shade glyph, by picture version.
    mutable std::map<uint64_t, std::pair<std::shared_ptr<const IconImage>, bool>> hostInk_;
    mutable std::map<std::wstring, std::wstring> hostNames_; // program descriptions, for icons without a tooltip
    // Windows' order, promotion and numeric record key of the icons by
    // "guid:{...}", "uid:<exe>|<uid>" and "exe:<exe>" (the first of the program).
    std::map<std::wstring, size_t> windowsOrder_;
    std::map<std::wstring, bool> windowsPromoted_;
    std::map<std::wstring, std::wstring> windowsRecordKey_;
};

} // namespace atrium
