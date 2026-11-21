# Verification record

Environment: Windows 11 build 26300, currently 2560×1440 @100% DPI (also tested at
2800×1840 @150% before the display configuration change), NVIDIA RTX 3060 Laptop,
MSYS2 UCRT64 GCC 16.2.0 / CMake 4.5.0 / Ninja 1.13.2, .NET SDK 10.0.404,
WindowsDesktop.App 10.0.15. Date: 2026-11-19.

## Build

- `scripts/build.ps1` completed: native configure + 15 translation units + link,
  `dotnet publish` of the WPF settings app, dist assembly.
- `SuperMenuBar.exe` is statically linked against the MinGW runtime (−static), ~4.09 MB
  (the media monitor's WIC/file parsing pulls in more static runtime than before);
  no `libstdc++-6.dll` / `libwinpthread-1.dll` needed.
- `SuperMenuBar.exe` resources: icon 101 (48×48 extract), embedded VersionInfo 1.1.0.0.
- Settings app: clean WPF build and publish, `net10.0-windows`, x64, no NuGet packages.
  Its own `--selftest` reports **27/27** checks: settings round-trip, clamping, bad JSON,
  actual executable icons via `ExtractIconEx`, presets, private HarmonyOS Sans SC, `logoStyle`,
  `rightIconSpacing` (999→24, −5→2, 20 default),
  tray ID normalization, hide→show→IsPromoted override semantics, and a read-only
  `TrayCatalog` probe. It confirmed **all 38 schema keys** and a live snapshot of
  **15 tray records** (1 shown / 14 hidden; raw 37, own 2 excluded, duplicates 2,
  33 persisted records before liveness). UI Automation toggled a hidden icon on
  (`trayShow=4007179161694544598`), a promoted icon off (`trayHide=12656677187298439157`),
  changed spacing to 21, applied, and reset successfully in the previous 33-record
  state; the same override rule now operates on the live 15.

## Runtime

| Check | Result |
|---|---|
| Top bar appears at screen top with live glass | PASS (screenshots in this repo) |
| Desktop Duplication live capture | PASS (`capture: Desktop Duplication live 2800x1840`) |
| Own windows excluded from DDA | PASS (no feedback ring; screenshots show the bar, DDA does not) |
| Adaptive ink on dark wallpaper | PASS (white text) |
| Adaptive ink on bright test wallpaper | PASS (fade to near-black text, 450 ms) |
| Apple/App/File/Edit/View/Window/Help menus | PASS |
| Menu hover switching (Apple → File) | PASS |
| Control Center, calendar, Wi-Fi/battery/volume panels | PASS |
| Control Center sliders + media buttons | PASS (volume/brightness commands unchanged; rendering replaced with Atrium-style blue left-filled pill, glyph on fill, no knob/end icons; outer capsule unchanged) |
| Menu keyboard via low-level hook | Esc PASS (manual); ↑/↓/Enter share the same hook path |
| Settings reload while running | PASS (height 28→34→28, language zh→en, theme auto→dark, auto-hide) |
| First run creates `%LOCALAPPDATA%\SuperMenu\settings.json` | PASS |
| Settings app opened from Apple menu | PASS (`dist\Settings\SuperMenu.Settings.exe`) |
| Second instance `--settings` forwards to running bar | PASS |
| AppBar work area reservation | PASS (work area top = 42 px at 150%, 28 px at 100%) |
| Quit restores work area | PASS (work area top returns to 0; process exits) |
| Auto-hide | PASS (hidden after pointer leaves, shown at top edge) |
| Tray icon resource / SM_UNICODE / DPI manifest | PASS (icon embedded; per-monitor v2) |
| Capture loss recovery | PASS (`AcquireNextFrame` access-lost → duplication restarted) |
| WinUI/Fluent native status glyphs | PASS (Segoe Fluent Icons detected at startup; Ethernet `E839`, Wi‑Fi `E701`, battery levels/charging, Control Center `E9E9`, search `E721`, volume and notifications from the system font) |
| Merged network/battery/Control Center block | PASS (one statusCluster plate; clicking Ethernet/battery/equalizer opens Control Center; no separate Wi‑Fi/battery/CC hit targets) |
| Automatic network glyph | PASS (`GetAdaptersAddresses` found the active physical Ethernet adapter while WLAN was disconnected; top bar showed Ethernet `E839`; wired preferred over Wi‑Fi; no link shows dim `E968`) |
| Automatic battery state | PASS (`GetSystemPowerStatus`: present=yes/59%, AC online/not charging; battery glyph follows level; absent-battery branch omits the glyph; charging switches to `E85A–E862`, `E83E`/`EA93`) |
| Configurable right-side spacing | PASS (`rightIconSpacing` 2–24, default 20; applied to status block internals, search, clock and tray icons; live 20 setting visibly wider; settings slider writes clamp range) |
| Clock date order and spacing | PASS (rightmost label now renders `11月20日 周五 20:22`; Chinese/English formatting and settings preview use month/day → weekday → time; right-side icon spacing set to 20 DIP) |
| Tray liveness filter (Atrium-style) | PASS (raw 37 → 33 persisted records → **15 live**, stale 18 hidden; `Shell_NotifyIconGetRect` GUID or UID+window probe; top bar and dropdown only show live icons; `tray-live.txt` written for settings) |
| User tray show/hide | PASS (`trayShow`/`trayHide` IDs override `IsPromoted`; manual test hid WeMeet and forced VLC while both were live; settings list now shows the live 15 only; 23 UI assertions in previous 33-record state) |
| Desktop focus hides menu buttons, not the bar | PASS (clicking desktop logged `bar: desktop focused, hiding menu buttons`; screenshot showed glass bar + Apple + Finder + status area with File/Edit/View/Window/Help removed; Alt+Tab restored them) |
| Display change auto-restart | PASS (posted `WM_DISPLAYCHANGE` to the bar; log `scheduling restart` → `restarted after display change`, old PID exited, new PID recreated the bar at 2560×1440 with AppBar re-reserved) |
| Control Center icon scale | PASS (accent circles 17.5 DIP radius, 22 DIP Fluent glyphs; left text vertically centered; screenshot shows balanced tiles) |
| Black/white Windows logo | PASS (`logoStyle=windows-mono`; native four-pane mark drawn with backdrop ink; settings segmented control offers 苹果 / Windows / 黑白) |
| Segoe Fluent Icons embedding | PASS (`assets/fonts/SegoeIcons.ttf` extracted from Windows 11; native log `font: private Segoe Fluent Icons loaded` / `gpu: icon font Segoe Fluent Icons`; bar, Control Center, menu, sliders, media buttons and WPF settings nav/preview use the font glyphs) |
| All menu shortcut hints | PASS (shortcut drawing was removed globally from `DrawMenu`, and menu width no longer reserves a shortcut column; left app menu screenshot shows 关于/设置/隐藏/隐藏其他/全部显示/退出 with no ⌘ hints; File/Edit/View/Window/Help, top-bar context and tray context use the same no-hint renderer; screenshot `docs/screenshots/menu.png`) |
| Right-click context menus | PASS (right-clicking the bar at 1000,14 opened a glass menu under the cursor with 打开 SuperMenu 设置/打开配置文件夹/关于/重新载入顶栏/退出; the same custom glass menu with no shortcut hints also replaces the native tray-icon right-click menu; no Settings process was launched automatically; screenshot `docs/screenshots/context-menu.png`) |
| Top-bar hover background | PASS (root cause: `Layout()` cleared `items_` every render frame, resetting every hover value to 0; the wash restarted at partial opacity each frame and appeared to flicker/offset. Hover is now inherited by MenuId/trayId; closed-state hover and open Control Center both draw one stable integrated status plate. A 6-frame cursor-hidden comparison confined differences to the cursor overlay area.) |
| Integrated status highlight | PASS (the network/battery/Control-Center glyphs always share one continuous rounded plate, both closed and while the Control Center is open; hovering any one icon lights the full three-icon plate) |
| Clock highlight alignment | PASS (clock `item.text` is inset 8 DIP from both plate edges; hover screenshot shows equal left/right padding instead of a left-shifted plate) |
| Date/time clock popup | PASS (clicking the date/time opens a 330×420 glass popup with a 43 px HH:MM:SS clock; two captures 1.1 s apart showed 19:22:22 → 19:22:23; month grid preview and “打开系统日历” hint; whole calendar area is `CmdOpenSystemCalendar`, sending `Win+N`; screenshot confirmed the Windows taskbar notification center/calendar pane opened on the right, including with the taskbar auto-hidden) |
| Date/time popup spring | PASS (uses the same Atrium `kPop` spring as Control Center: 0.92→1 over 0.6 s with 160 ms fade) |
| Control Center physics | PASS (Atrium `kPop` spring response 0.36 s/damping 0.62; DComp visual scales 0.92→1 over 0.6 s with a small overshoot, 160 ms cubic fade; captured frames show the smaller translucent first frame, near-full overshoot and settled layout; other popups keep 190 ms scale-in) |
| Fluent icon state animations | PASS (Control Center no longer animates Wi‑Fi on open; clicking Wi‑Fi tile plays `Wifi1→Wifi2→Wifi3→Wifi` in 0.3 s; network transitions use WifiAni/EthernetAni; charging battery cycles BatteryCharging frames; volume changes pulse the speaker 450 ms; media uses Play/Pause state glyphs) |
| Control Center direct switches | PASS (no switch chrome; only the circular icon area toggles, text/other tile area opens settings; icon area uses a dedicated hit appended over the card. Wi‑Fi and Bluetooth use WinRT Radio via the running media helper; toggling Wi‑Fi keeps CC open; Wi‑Fi off/on verified `wifi=0`/`wifi=1`; Bluetooth off/on verified likewise; Focus uses Windows `QuietHoursSettings` COM to set `Microsoft.QuietHoursProfile.PriorityOnly` / `.Unrestricted` (system Do Not Disturb); helper CLI verified `focus=1`/`focus=0` state; native Focus click changed state to `focus=1` then back to `focus=0`; icon uses solid `MobQuietHoursSolid`; off state grey disc keeps Wi‑Fi glyph) |
| Rapid radio toggles | PASS (three rapid Wi‑Fi clicks logged target 1→0→1 and state file settled at `wifi=1`; four clicks settled at `wifi=1`; UI uses optimistic target and 1.2 s state-file suppression; helper radio command loop polls every 60 ms) |
| Larger battery glyph | PASS (top-bar battery glyph 1.28× cluster size in its own wider slot; settings preview battery 18 px) |
| Global interface font | PASS (native DirectWrite private collection loaded `HarmonyOS Sans SC` from `Fonts/HarmonyOS_Sans_SC_{Light,Regular,Medium,Bold}.ttf`; WPF settings loaded the same four private faces and selftest confirmed `HarmonyOS Sans fonts bundled` / `HarmonyOS Sans loaded from private folder`. Body/menu/date use Regular 400, emphasis Medium 500, titles Bold 700; icon font remains Segoe Fluent Icons. Old MiSans files removed from assets/dist; HarmonyOS license bundled.) |
| Atrium trayHost refactor | PASS (hidden host `Shell_TrayWnd` took first place, logged `tray host: on` and 10 real HICONs; bar showed live Tailscale/Huawei/Intel icons instead of snapshots. Clicking a visible Intel icon logged `tray host: left click 英特尔® Graphics Software`; clicking a hidden SZU icon logged `tray host: left click SZU 网络守护 - 守护中` with no overflow-panel fallback. Starting Atrium alongside logged `another program's tray ... left there` and SuperMenu yielded without fighting. Explorer stayed responsive throughout; full technical notes in `docs/TRAY.md`.) |
| Early logon autostart | PASS (native created scheduled task `\SuperMenu` with `MSFT_TaskLogonTrigger`, `InteractiveToken`, `LeastPrivilege`, action `dist\SuperMenuBar.exe`; HKCU Run `SuperMenu` value removed; log `autostart: logon task installed` / `task=1 runKeyFallback=0`. `Start-ScheduledTask SuperMenu` launched a fresh bar process. Task is non-admin, starts at logon before Explorer's Run-key startup phase; `WaitForShellReady` and `TaskbarCreated` recovery cover shell-start races.) |
| Windows Update restart indicator | PASS (machine had `WindowsUpdate\Auto Update\RebootRequired` and CBS `RebootPending`; a blue WindowsUpdate glyph appeared between the Tailscale tray icon and the network/battery/CC status block; clicking it opened Settings → Windows 更新 showing “需要重启 (估计: 6 分钟)” and “正在等待重启”; statusKey includes the flag so it appears/disappears automatically; screenshot `docs/screenshots/update-icon.png`) |
| Input method indicator and switcher | PASS (bar showed blue keyboard + `中` between update icon and status block; when foreground switched to the English layout the bar automatically showed `ENG` (`input: layout=0409 label=ENG`); clicking logged `opening system keyboard-layout switcher` and Windows returned to Chinese layout `layout=0804 label=中`. Labels are derived from keyboard language plus IME native mode, so Japanese あ/A, Korean 한/A, Wubi 中/英 and other language abbreviations follow the system indicator; state refreshed every 500 ms; screenshot `docs/screenshots/input-method.png`) |
| Calendar popup year | PASS (opened clock panel showed `2026年11月20日 周五` above the mini calendar; screenshot `docs/screenshots/clock.png`) |
| macOS-style privacy indicators | PASS (opened the real Windows Camera app with live USB camera preview; the bar logged `privacy: camera=1` and showed a green dot immediately right of the date; closing the app logged `camera=0` and the dot disappeared. Camera state now comes from the real FrameServer FSStream watchdog event because ConsentStore was not updated in real time on this build; microphone uses WASAPI active capture sessions with ConsentStore fallback. Before/after captures showed the date at the identical x-position because the dot item is placed in the existing right margin and does not consume layout width; screenshot `docs/screenshots/privacy.png`) |
| AC vs battery glyph | PASS (`GetSystemPowerStatus().ACLineStatus` tracked as `batteryOnAc`; current AC-connected 58% machine renders `BatteryCharging` with a visible ⚡ next to the battery instead of the plain `Battery` glyph; statusKey includes AC so icon updates on plug/unplug; active charging still cycles BatteryCharging frames) |
| Value-dependent slider glyphs | PASS (brightness ≤50 `LowerBrightness`, >50 `Brightness`; volume 0/1–33/34–66/67–100 `Volume0/1/2/3`; icon size stays 12 DIP) |
| Actual application tray icons | PASS (visible promoted icons are captured from the live taskbar pixels every 2 s with local background subtraction; Tailscale's top-bar icon now shows the current white/gray dot pattern instead of the old uniform snapshot. `WindowFromPoint` rejects captures while the taskbar is auto-hidden. Hidden records share the overflow-arrow rect, are marked `overflowProxy`, and clicking one logs `opening system overflow` then `forwarding hidden … click at x,y` after the overflow opens; promoted Tailscale left/right clicks logged forwarding to its unique rect.) |
| Hidden tray interaction | PASS (SZU hidden icon click: opened Explorer overflow panel, refreshed rects to `2163,1215`, forwarded the left click to the actual hidden icon; no exe-launch fallback) |
| Rounded-rectangle Control Center | PASS (cards and sliders use translucent gradient fill + rim over the popup glass; 72 DIP/20 DIP-radius slider modules; Segoe Fluent Icons at uniform optical sizes) |
| Control Center brightness DDC/CI | PASS (primary monitor DDC VCP 0x10; async worker set 94/90/74/55/22% during a fast drag and `brightness: set ... (ok)`; later restored to 100%; WMI fallback retained) |
| Desktop focus removes Finder | PASS (Win+D foreground desktop: left side contained only the Windows mark, no “访达” and no File/Edit buttons; bar remained visible) |
| Existing Settings window reuse | PASS (minimized one settings instance, right-clicked bar: process count stayed 1, foreground PID became the existing settings PID) |
| Media metadata card | PASS (QQ Music “死別 / シャノン”: 300×300 JPEG cover decoded to 96×96 PBGRA and painted as 55 DIP rounded art (125%); Tencent.QQMusic.exe icon extracted and painted at 22 DIP (50%) top-right; title/artist centered where the progress bar used to be; no progress bar remains) |
| Connectivity pill typography | PASS (blue circle 28→35 DIP, inner glyph stays 22 DIP; title/subtitle block horizontally and vertically centered in the remaining right-hand area) |
| Program icon visibility | PASS (QQ Music playing: 22 DIP icon visible; paused: icon hidden; no media / no resolved exe icon: corner stays empty) |
| Media transport buttons | PASS (root cause: `SendInput` was called with `cbSize=sizeof(INPUT[2])=80` → `ERROR_INVALID_PARAMETER 87`, so media keys were never delivered; fixed to `sizeof(INPUT)=40` plus `KEYEVENTF_EXTENDEDKEY`/scancode. Edge local media changed PAUSED→PLAYING via the Control Center button; QQ Music uses the same system media-key path) |
| Async slider hardware writes | PASS (UI thread never calls DDC/audio during drag; coalescing worker applied brightness 94/90/74/55/22% while the drag loop remained non-blocking; volume uses the same worker path) |
| Fluent status icons | PASS (Segoe Fluent Icons glyphs for Ethernet/battery/equalizer/search/chevron; screenshot shows consistent optical sizes) |
| Media transport alignment | PASS (hit boxes and drawn icons use identical centers; three controls now symmetric on the right of the media capsule) |
| Clock right margin | PASS (right margin 10→24 DIP; date/time moved left with no right-cluster overlap) |
| iOS-style Control Center layout | PASS (374 DIP containerless panel: left 146×58 Wi-Fi/Bluetooth/Focus pills, right 190×190 square now-playing card, two Atrium-style blue slider capsules; no outer background, six floating glass islands) |
| Glass settings follow top bar | PASS (Control Center uses the same `ComputeMaterial(s, scale, s.barHeight, false)` path as the bar; changing transparency/blur/refraction/highlight or glassPreset affects both; card overlays derive from the same MaterialParams) |
| Control Center outside click | PASS (click on media card background stays open; click outside the island/window closes; gaps are transparent to the desktop) |
| Filled Control Center glyphs | PASS (Segoe Fluent Icons for Wi‑Fi/Bluetooth/focus/display/sun/speaker/play/skip; native font collection loaded) |
| Settings app Fluent icons | PASS (nav + live preview glyphs render from `Assets/Fonts/SegoeIcons.ttf` via `Fonts.GetFontFamilies`; fallback to installed face if absent) |
| Narrower/taller cards | PASS (left pills 161×48→146×58 DIP; media card 146×190→190×190 square; slider row remains full-width; text reflowed) |
| Atrium slider port | PASS (72 DIP / 20 DIP-radius rounded-rectangle modules, title shifted right 10 DIP, percent top-right, wide 24 DIP pill, blue fill edge exactly at value%; sun/speaker glyphs reduced from 24 to 12 DIP = 50%; brightness/volume set paths unchanged) |
| Slider value/position alignment | PASS (0% anchor is the right tangent of a 12 DIP-radius circle centered on the glyph; hit mapping starts at the same tangent; midpoint click → brightness 51%, fill edge under pointer) |
| System tray registry mirror | PASS (`TrayReader`: 37 raw → 33 persisted after filtering 2 own entries + collapsing 2 duplicate records → **15 live** after `Shell_NotifyIconGetRect` liveness; 1 promoted / 14 hidden, ordered by `UIOrderList`) |
| Tray snapshot decode | PASS (WIC PNG → 32×32 premultiplied BGRA; all 15 live icons non-empty; cold refresh with liveness + actual exe icons ~90–110 ms, warm cached refresh single-digit ms) |
| Promoted tray icons in top bar | PASS (1 live promoted icon + native chevron left of status glyphs; tooltip shows the record label) |
| Hidden tray dropdown | PASS (`PopupKind::Tray`, glass grid, 5 columns, footer full label on hover, click activates/launches associated program) |
| Leftmost logo option | PASS (`logoStyle=apple|windows|windows-mono`; native setting reload + Windows 11 vector mark colored/ink; settings segmented control + live preview, 27/27 selftest, all 38 schema keys) |
| Atrium overlay fullscreen false-positive | PASS (AtriumSurface is tool-window/no-activate; no longer hides the bar; bar stays visible over Atrium) |

## 2026-11-21 托盘重构（atrium::TrayModel）

Windows 11 build 26300 实机验证；架构、点击分发与完整边界见 `docs/TRAY.md`，
根因与方案见 `docs/research/tray-refactor.md`。本节结论取代本文件 2026-11-19 记录中
旧 `TrayReader` 只读镜像/`overflowProxy` 相关条目（保留为历史记录）。

| 检查 | 结果 |
|---|---|
| native Release 构建 | PASS（新托盘模块与迁移后的 app/bar/popup 编译、链接通过） |
| 托盘单元测试 | PASS（`ctest --test-dir native/build` 4/4；共 137 checks：tray_match 22、tray_click 60、tray_protocol 15、tray_pointer 40） |
| Host 模式实机 | PASS（恰好 1 次 `TaskbarCreated` 广播、0 让位、0 “in front again”、0 Explorer 重启；旧日志为 937/928/925。read-back 37 记录 → 15 存活图标、0 未知占位；顶栏 1 promoted Tailscale + chevron，隐藏面板 13 = 15−1−本程序图标） |
| 原生点击 | PASS（promoted 左键打开 Tailscale 真实 Win32 菜单；promoted 右键、隐藏图标（华为云盘、SZU 网络守护）左/右键与双击续发均记录原生回调与 `-> sent`） |
| read-back 回退 | PASS（无 Host 探针：Tailscale UIA `ShowContextMenu` → `uia-context-menu ... observed=1`，result=observed） |
| 与 Atrium 共存 | PASS（启动 1 次广播；一次接管尝试成功；z-order SuperMenu → Atrium → Explorer；0 让位/0 抢回/0 重启；Atrium 同窗口仅 1 次 “left there”、0 次 “in front again”；点击仍原生；read-back 16 图标（含 Atrium），面板 14 隐藏 + 1 promoted；两个提升权限应用按 GUID/HWND 合并、未重复） |
| 收尾 | PASS（重启安装版 SuperMenu 与 Atrium；登录计划任务指回 `C:\fish\SuperMenu\SuperMenuBar.exe`） |

Win10 经典 toolbar 路径未在本机实机验证。

## Known residual limitations

- A display mode/DPI/hot-plug change asks for a restart (single-display design).
- Capture feedback avoidance uses the undocumented `WCA_EXCLUDED_FROM_DDA`; on systems
  where it fails, the fallback hides the bar from all screenshots (WDA) or drops to the
  wallpaper backdrop.
- Settings app remains dark-themed by design; only the bar follows `theme`.
- Notification panel is not connected to the Windows notification store.
- 托盘：UIA 结构未公开且随 Windows 版本/任务栏形态变化；隐藏溢出按钮只在面板打开后
  物化（平时由存活注册表记录补充）；v4 应用在不再次 `NIM_SETVERSION` 时可能按 version 0
  编码点击；提升权限应用可能拒绝宿主回调（回退 read-back）；顽固的其他 tray host 只做
  一次接管尝试，失败后本次会话让位并只用 read-back；Explorer 自己的系统图标与 SuperMenu
  自身图标不参与接管；XAML 没有原生双击 Pattern（第二击为第二次 Invoke）；Win10 经典
  toolbar 路径未在本机实机验证。详见 `docs/TRAY.md` 的已知边界。
- `trayShow` / `trayHide` are keyed by Explorer record ID; if Explorer rebuilds a record,
  the stale override is ignored and should be refreshed in the settings list.
- Brightness uses DDC/CI on the primary monitor with WMI fallback; some monitors or
  adapters expose neither protocol, so the slider may remain inert on that hardware.
- Focus on the Explorer desktop hides the app name (“访达”) and File/Edit/View/Window/Help;
  the glass bar, Apple menu and right status cluster remain. Full-screen apps hide the whole bar.
- Automated display-change restart requires a working `--restart-after` mutex handoff;
  if the replacement cannot start, the old process remains and logs the failure.

