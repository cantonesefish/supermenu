// The tray host (setting `trayHost`, "Host the tray"): Atrium receives the apps'
// notification-area icons itself, as the shell does, instead of reading them back
// from Explorer's tray.
//
// Shell_NotifyIcon sends its data to the first window of class Shell_TrayWnd
// (WM_COPYDATA). Atrium keeps a hidden window of that class ahead of Explorer's,
// takes a copy of every add, change and removal (the live picture, the tooltip,
// the callback message, the protocol version) and passes each call on to Explorer
// unchanged, so the system tray stays complete and takes over again the moment
// the window is gone. Every other message to the window goes on to Explorer's
// taskbar too. Apps that were already running add their icons again on
// TaskbarCreated, which the host broadcasts exactly once when it starts and once
// after Explorer itself is confirmed to have restarted (its shell session changed,
// not another tray host asking the same). Clicks and hovering are the callback
// messages the shell itself sends (shell/tray_protocol.h). Nothing is injected
// anywhere. The pattern is ManagedShell's TrayService (Cairo, RetroBar).
//
// Never in the way of Windows or of another tray host: the window has a thread of
// its own (a busy UI thread never holds up an app's call); a failure while reading
// a call still passes it on; the Shell_TrayWnd stream is claimed once, then either
// held or yielded for the whole session (one z-order challenge, never a ping-pong,
// and no TaskbarCreated from the z-order paths); and the guard process puts
// Explorer's taskbar in front if the window stops answering.
#pragma once

#include "icon_image.h"
#include "shell/tray_protocol.h"

#include <windows.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace atrium {

struct HostIcon {
    std::wstring key;  // "guid:{...}" or "hwnd:<hwnd>:<uid>"
    HWND hwnd = nullptr;
    UINT uid = 0;
    UINT callback = 0; // 0: the app takes no clicks
    UINT version = 0;  // NOTIFYICON_VERSION; 0 for the oldest protocol
    bool showTip = false; // NIF_SHOWTIP: a version 4 app wants the standard tooltip, no popup requests
    std::wstring tip;
    std::wstring exe;  // lower-case full path of the window's process
    DWORD pid = 0;
    std::shared_ptr<const IconImage> image;
    uint64_t imageVersion = 0;
    bool hidden = false; // NIS_HIDDEN
    uint64_t added = 0;  // order of arrival
    std::vector<ULONGLONG> pictureChanges; // the last few seconds' picture changes (a flashing icon)
    ULONGLONG flashUntil = 0; // flashing (a new message in QQ or WeChat) until then
};

class TrayHost {
public:
    // Posts `msg` (wParam 0) to `notify` when the icons change. `readyWaitMs` and `startDelayMs` are for tests only.
    TrayHost(HWND notify, UINT msg, DWORD readyWaitMs = 3000, DWORD startDelayMs = 0);
    ~TrayHost();
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    bool Running() const { return window_.load() != nullptr; }
    // The host holds the apps' Shell_NotifyIcon stream (it is the first Shell_TrayWnd).
    // False while the one startup ownership check is still running, and for a host that
    // yielded to another program's tray host (then it stays false for the session).
    bool Owned() const { return owned_.load(); }
    // Another program's tray host (RetroBar, a second shell) holds the stream: Atrium
    // leaves it there for the rest of the session and reads Explorer's tray meanwhile.
    // It never pushes back in front and never broadcasts TaskbarCreated again.
    bool Yielded() const { return yielded_.load(); }
    std::vector<HostIcon> Icons() const;
    // The programs whose icon flashes now: QQ and WeChat flash theirs for a new message (they
    // have no window to flash while they sit in the tray).
    std::vector<std::wstring> Flashing() const;
    // Sends the shell's notifications for `action` on `key` at the screen point `at`
    // (the panel's pointer). False when the app cannot be told (gone, no callback,
    // an elevated app's window refusing Atrium's messages).
    bool Send(const std::wstring& key, tray::HostAction action, POINT at);
    // The pointer rests on `key` (empty: on none): the arrival, and for a version 4
    // app drawing its own tooltip, its popup request after a moment and its close.
    void Hover(const std::wstring& key, POINT at);
    // Where icons are, for apps that ask (Shell_NotifyIconGetRect): around the last
    // click for the icon clicked, `anchor` (the bar's tray entry) for the others.
    void SetPlaces(RECT anchor);

private:
    // The shell's identity: captured when the host starts and compared on every
    // TaskbarCreated, so that only a changed shell session counts as Explorer
    // restarting (another tray host's broadcast is the same session and is ignored).
    struct ShellSession {
        HWND tray = nullptr;   // the shell's own Shell_TrayWnd
        DWORD pid = 0;         // the shell process (Explorer)
        ULONGLONG created = 0; // its process creation time (GetProcessTimes)
    };
    // The one z-order challenge the state machine allows at a time, checked again
    // after a short wait; None whenever the host is not waiting for an answer.
    enum class Attempt { None, Startup, Takeover };
    // Pending: the startup decision is not made yet; Owned/Yielded: made, for the session.
    enum class Ownership { Pending, Owned, Yielded };

    static ShellSession CaptureSession();
    static bool NewSession(const ShellSession& now, const ShellSession& was);
    static bool SameSession(const ShellSession& a, const ShellSession& b);

    void Run();
    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
    static void CALLBACK ShellEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG, DWORD, DWORD);
    LRESULT Message(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT OnNotifyIcon(WPARAM w, const COPYDATASTRUCT* cds);
    void Record(const COPYDATASTRUCT* cds, std::shared_ptr<const IconImage> image, LRESULT result);
    LRESULT OnIconRect(WPARAM w, const COPYDATASTRUCT* cds);
    LRESULT Forward(UINT m, WPARAM w, LPARAM l);
    void Broadcast(const wchar_t* why);
    void MaybeBroadcastStartup();
    void TakeOwnership();
    void YieldStream(DWORD ownerPid); // (not "Yield": winbase.h has that as a macro)
    void OnTaskbarCreated(HWND h);
    void OnRecollect(HWND h);
    void KeepFirst();
    void Prune();
    void LogList();
    void Popup(bool open);

    HWND notify_;
    UINT msg_;
    DWORD startDelayMs_ = 0;
    UINT taskbarCreated_ = 0, quit_ = 0, hover_ = 0;
    HANDLE ready_ = nullptr, stop_ = nullptr;
    std::atomic<HWND> window_{nullptr};
    std::atomic_bool dirty_ = false;   // changed since the last notice (posted at most every 100 ms)
    std::atomic_bool owned_ = false;   // the host holds the Shell_NotifyIcon stream
    std::atomic_bool yielded_ = false; // another program's tray host holds it, for the session
    std::atomic_bool stopping_ = false; // going: no longer moves itself in front
    std::thread thread_;
    mutable std::mutex mutex_;
    std::map<std::wstring, HostIcon> icons_;
    uint64_t nextAdded_ = 0, nextImage_ = 0;
    RECT anchor_{}, clicked_{};
    std::wstring clickedKey_;
    std::wstring hoverKey_, popupKey_; // host thread: the icon under the pointer, the one asked to show its popup
    POINT hoverAt_{};
    ULONGLONG lostAt_ = 0, broadcastAt_ = 0;
    std::wstring loggedKeys_; // host thread: the list as last logged
    bool flashing_ = false;   // host thread: some icon flashed at the last look
    // All of the tray state below is the host thread's own.
    ShellSession session_;          // the shell the host last knew (baseline for a restart)
    ShellSession restartSession_;   // the shell a restart being confirmed led to
    Ownership own_ = Ownership::Pending;
    Attempt attempt_ = Attempt::None;
    ULONGLONG attemptAt_ = 0;       // when the one challenge in flight is checked again
    bool takeoverUsed_ = false;     // the one later takeover attempt was spent
    bool restartPending_ = false;   // Explorer restarted: waiting for the shell, then one broadcast
    bool startupNotice_ = true;     // the startup broadcast is still owed
    int recollectTries_ = 0;        // how often a confirmed restart still found no taskbar
    bool inFrontLogged_ = false;    // (yielded) in front again: logged once, no z-order change
    ULONGLONG ignoredNoticeAt_ = 0; // last "another host's TaskbarCreated" line (at most every 5 s)
};

} // namespace atrium
