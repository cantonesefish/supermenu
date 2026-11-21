# 架构

## 1. 进程与窗口

```
SuperMenuBar.exe (C++ / Win32 GUI, per-monitor DPI aware v2)
├─ 顶栏窗口      SuperMenuBarWindow    WS_POPUP + WS_EX_TOPMOST + WS_EX_NOACTIVATE + WS_EX_NOREDIRECTIONBITMAP
├─ 弹出面板窗口  SuperMenuPopupWindow  同上，按需显示/复用；玻璃 + 内容共用一个 swap chain
├─ 托盘消息窗口  SuperMenuTrayHost     TrayModel 事件、设置广播、app 自身托盘图标
└─ 主线程        消息循环 + 15 ms 定时器（渲染、捕获轮询、状态刷新）

SuperMenu.Settings.exe (.NET 10 WPF，独立进程)
└─ 读写 %LOCALAPPDATA%\SuperMenu\settings.json
   广播 RegisterWindowMessage("SuperMenu.SettingsChanged") → HWND_BROADCAST
```

菜单栏本身从不激活：窗口带 `WS_EX_NOACTIVATE`，`WM_MOUSEACTIVATE` 返回
`MA_NOACTIVATE`。这样打开菜单、点击菜单项时前台应用不会被抢走，发送到前台应用的
`SendInput` / `PostMessage` 命令才有意义。

## 2. 液态玻璃渲染管线

每个玻璃表面（顶栏或一个弹出面板）各自持有一个 `GlassSurface`：

```
Desktop Duplication / 壁纸回退（monitor 大小 BGRA 纹理）
        │
        ├── AcquireNextFrame(0) → CopyResource → ReleaseFrame
        │
        ▼
GlassSurface::UpdateBackdrop(monitorFrame)
        │  只复制与自身屏幕矩形相交的 ROI（monitor 坐标）
        │  ROI = window ∩ (window ± pad) ∩ monitor；边缘自然截断，
        │  着色器用 CLAMP 采样处理靠边情况
        ▼
ROI 纹理（B8G8R8A8_UNORM，MipLevels=0，D3D11_RESOURCE_MISC_GENERATE_MIPS）
        │  GenerateMips（仅在 ROI 更新后）
        │  UpdateLumaGrid：复制小 mip 到 staging，读回亮度网格
        ▼
全屏三角形像素着色器（glass.hlsl，运行时 D3DCompile）
        │  G3 连续曲率圆角 SDF / 覆盖
        │  边缘法线 → 折射偏移 → 色散采样
        │  模糊 mip 与锐利背景混合
        │  vibrancy / tint / rim specular / sheen / shadow / grain
        ▼
swap chain 后缓冲（B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED）
        │  同一后缓冲上用 Direct2D 画文字、图标、悬停底板
        │  预先解除 RTV/SRV，`SetTarget` → BeginDraw → EndDraw
        ▼
swap chain->Present(0,0) → DirectComposition visual → DWM 合成到桌面
```

关键点：

- **一次 Present 同时呈现玻璃和内容**，没有 Atrium 的双视觉层内容/玻璃同步问题。
- **自己的窗口必须从桌面复制中排除**，否则玻璃会采样自己的上一帧形成无限反馈。
  首选 `SetWindowCompositionAttribute(hwnd, WCA_EXCLUDED_FROM_DDA /*24*/)`；不可用时
  回退 `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`。恢复/重开窗口后会重新应用。
- **ROI 裁剪**到桌面范围，去掉 Atrium 的 `ExtendBackdropEdges` 边缘复制通道；采样器
  `CLAMP` 负责边缘。
- **文字墨色**来自 ROI 小 mip 的亮度网格（sRGB 空间 `.2126/.7152/.0722`），经
  tint 混合后按 0.50/0.42 双阈值决定黑/白，450 ms 缓动切换；区域分别计算左菜单、
  右侧状态、时钟。
- **`GlassParams`** 固定 400 字节，与 `glass.hlsl` 的 cbuffer 一一对应；
  `static_assert(sizeof(GlassParams) == 400)` 保证运行时上传正确。参数在
  `settings.cpp::ComputeMaterial` 中做窄栏夹紧（28 px 栏：`edge ≤ 0.45·height`，
  `refract ≤ 0.6·height`）。
- **亮度图**通过同步 Map 一个很小的 staging mip（宽 ≤ 160）得到；频率大约
  300 ms / 新帧一次，代价可忽略。

### 帧调度

- 主线程 15 ms 定时器；每 tick：刷新系统状态、`AcquireNextFrame(0)` 轮询、
  状态变化/动画/新帧时重绘；静止时不做无谓 Present。
- 弹出面板的打开/关闭使用 DirectComposition `visual->SetTransform(scale)` +
  `effect->SetOpacity()`，每帧更新约 190 ms / 130 ms；内容在 CPU 侧也已以 60 Hz
  重绘，但动画实际由合成器呈现。
- 显示/设备丢失：Present 返回 `DXGI_ERROR_DEVICE_REMOVED`，捕获返回
  `DXGI_ERROR_ACCESS_LOST` 时会重建 duplication；捕获失败/安全桌面/锁屏时壁纸回退。

## 3. 顶栏窗口与系统集成

- 窗口尺寸 = 屏幕宽 × (栏高 + 投影留白)，位于显示器物理像素 `(left, top)`；
  栏高 = `settings.barHeight × DPI scale`。
- `WM_NCHITTEST` 在栏下阴影区返回 `HTTRANSPARENT`，让鼠标穿透；栏本体接收点击。
- 顶栏默认为 AppBar（`SHAppBarMessage(ABM_NEW/QUERYPOS/ABM_SETPOS)`），
  预留 `屏幕顶端 ~ barHeight` 的工作区，最大化窗口自然停在栏下方；
  “不预留工作区”时最大化窗口会滑到玻璃后面，可看到实时折射。
- 自动隐藏：关闭 AppBar 预约，指针离开 550 ms 后 `ShowWindow(SW_HIDE)`；
  指针到屏幕顶端 2 px 内重新显示。
- 全屏检测：`SHQueryUserNotificationState` 为 D3D 全屏/演示模式，或无标题、
  非最大化的窗口完整覆盖屏幕时让出顶栏并释放 AppBar 预约。
- 前台应用名：`GetForegroundWindow` → PID → 进程路径 → `FileDescription`
  （资源管理器桌面显示“访达/Finder”；设置中心映射为“SuperMenu 设置”）。
- 退出时 `ABM_REMOVE`，工作区恢复。

## 4.0 托盘（atrium::TrayModel，双源）

- `native/src/atrium/shell/tray_model.*` 是唯一托盘数据源，运行在 MTA worker：所有
  UIA、注册表、`Shell_NotifyIconGetRect`、进程/版本查询、图片解码与点击确认都在
  worker 上；UI 线程只 `Refresh()`/`Click()`/`Hover()` 排队并读取缓存，不阻塞渲染。
- **read-back**：枚举 Explorer 托盘的 UIA 按钮（Win11 XAML 的
  `DesktopWindowContentBridge` / `TopLevelWindowForOverflowXamlIsland` 下的
  `NotifyItemIcon`；Win11 没有 `ToolbarWindow32`，Win10/旧壳退回经典 toolbar 按钮），
  读 `HKCU\Control Panel\NotifyIconSettings` 的 `UIOrderList`/`IsPromoted`/
  `IconSnapshot`/`ExecutablePath`，用 GUID 或 HWND+UID 做 GetRect 存活探测，再由
  `tray_match` 按身份/几何/名字/顺序/运行程序对账。记录与快照只是缓存元数据。
- **host**：`native/src/atrium/shell/tray_host.*` 在独立线程维持隐藏
  `Shell_TrayWnd` 与 `TrayNotifyWnd`，抢在 Explorer 前接收 `Shell_NotifyIcon`
  （`WM_COPYDATA` `dwData=1`），保存实时 HICON、回调消息、版本、`NIF_SHOWTIP`/
  `NIS_HIDDEN` 与 tooltip，并原样转发给 Explorer（图标询问 `dwData=3`、AppBar 等消息
  同样转发）。Explorer 拒绝重复 `NIM_ADD` 时改发 `NIM_MODIFY`；系统图标不接管。
- **合并**：两个来源按真实身份（GUID / HWND+UID / 程序）合并；Host 图标用实时 HICON
  覆盖快照并排在前面，Host-only 图标追加，只有 Explorer 有的图标保留读回结果；排序用
  Windows 自己的 `UIOrderList`，显示位置用 `IsPromoted`；Host 让位时整表退回 read-back。
  `native/src/tray_view.h` 在 `IsPromoted` 之上应用 `trayShow`/`trayHide` 覆盖
  （按 `recordKey` 数字记录 id），SuperMenu 自身图标被过滤。
- **共存**：Host 启动时记录 shell 会话身份（`Shell_TrayWnd` HWND + Explorer PID +
  进程创建时间）；其他 Host 的 `TaskbarCreated` 广播不再当作 Explorer 重启，广播只在
  启动和确认 Explorer 重启后各发一次；启动时最多一次接管尝试，失败即本会话固定让位
  （`Yielded()`），不抢 z-order、不重发。
- **点击**：Host-owned 图标按应用 callback/version 发 shell 协议；否则 read-back 依次
  走 UIA Invoke/ShowContextMenu、Win10 经典 toolbar、`LegacyIAccessible`、恢复已有窗口、
  短暂显示任务栏 + 物理右键、最后打开 Explorer 溢出区。结果分
  observed / accepted-unconfirmed（不重试）/ system-tray / needs-user。
- `BarWindow` 的 AppBar 调用仍用 Atrium `AppBarMessage`，在有 tray host 窗口时把 host
  暂时放到 Explorer 后面再发 `SHAppBarMessage`。
- 细节与实测见 `docs/TRAY.md`，移植来源与边界见 `docs/research/tray-refactor.md`。

## 4.1 状态字形与系统托盘

- 自启：`ApplyLaunchAtLogin()` 通过 Task Scheduler COM 创建当前用户的
  `LogonTrigger` 任务（`InteractiveToken`、`LeastPrivilege`，无需管理员），任务在
  Explorer 的 Run 键启动阶段之前运行；删除旧 HKCU Run 值，只有 COM/任务创建失败时
  才回退 Run 键。`WaitForShellReady()` 在任务先于 shell 启动时等待 Explorer；
  `TaskbarCreated` 广播会重新注册 AppBar、刷新托盘并重绘。
- `SystemMonitor::RefreshInputMethod()` 每 500 ms 读取 `InputMethodTarget()` 的键盘
  布局与 `WM_IME_CONTROL/IMC_GETCONVERSIONMODE`，生成与系统托盘一致的状态文本：
  中文 中/英、日语 あ/A、韩语 한/A、英语 ENG、其他语言使用
  `LOCALE_SABBREVLANGNAME`。`inputMethodAvailable/Native/Label` 进入 `statusKey`，
  使 `MenuId::InputMethod` 在更新图标与状态块之间自动出现/刷新。
- 点击输入法图标调用 `OpenInputMethodMenu()`，发送 `Win+Space` 打开 Windows
  自己的键盘布局切换菜单，覆盖日语、五笔等所有已安装输入法。
- `SystemMonitor::RefreshPrivacy()` 每秒检测真实设备占用：摄像头读取
  `Microsoft-Windows-MF-FrameServer/Camera_FrameServer` 的最新 `FSStream`
  watchdog 事件（`WatchdogOperation 1 = start / 4 = stop`）；麦克风枚举默认
  capture endpoint 的 WASAPI 会话，存在 `AudioSessionStateActive` 即视为录音中。
  `CapabilityAccessManager\ConsentStore`（`LastUsedTimeStop == 0`）仅作为回退，
  因为部分 Windows 版本只把它当审计记录，不能实时反映隐私指示器。
  `MenuId::Privacy` 圆点不是普通布局项，而是在 `BarWindow::Layout` 末尾直接放入
  `rightMargin` 留白的独立命中区；因此圆点出现/消失不会移动日期或其他右侧图标。
  点击打开 Windows 隐私设置页。
- `SystemMonitor::RefreshWindowsUpdate()` 每 5 秒读取 Windows Update
  `Auto Update\RebootRequired`、`PostRebootReporting` 与
  `Component Based Servicing\RebootPending`；任一存在时，`BarWindow::Layout` 在
  状态块左侧、托盘图标右侧插入 `MenuId::WindowsUpdate`，绘制蓝色 Fluent
  `WindowsUpdate` (F0C5) 字形；点击 `OnBarItem` 调用
  `ms-settings:windowsupdate`。该状态加入 `statusKey`，更新出现/消失时自动重排。
- 菜单栏、控制中心、搜索、媒体键和设置中心的功能图标使用 **Segoe Fluent Icons**。
  字体从本机 Windows 11 提取并复制到 `assets/fonts/SegoeIcons.ttf`，构建时放到
  `Fonts/SegoeIcons.ttf`；`Gpu::LoadIconFont()` 用 `IDWriteFactory5` 建立私有
  `IDWriteFontCollection`，`Ui::IconFormat()` 使用该集合，因此目标机没有安装
  Segoe Fluent Icons 也能正常显示。码位集中在 `native/src/fluent_glyphs.h`，依据
  Microsoft 的公开图标列表。
- 状态图标动画由 `App::Tick` 检测网络/音量变化时间戳、充电状态并触发栏重绘：
  网络切换播放 WifiAni1/2 或 EthernetAni，充电电池在 BatteryCharging 帧间循环，
  音量变化对扬声器做 450 ms 脉冲。控制中心只有圆形图标区域调用
  `CmdToggleWifi`/`CmdToggleBluetooth`/`CmdToggleFocus`，文字区域仍打开设置：
  前两者把命令原子写入 `%LOCALAPPDATA%\SuperMenu\radio\command.txt`，由已运行的
  媒体辅助进程用 60 ms 轮询、WinRT `Radio.SetStateAsync` 串行开关无线电并回写
  `state.txt`；原生侧先乐观更新状态并在 1.2 s 内抑制旧状态文件回写，因此高频连点
  即时重绘且最终状态与最后一次点击一致。点击 Wi‑Fi 后播放 Wifi1→Wifi3→Wifi
  四帧 0.3 秒强度动画；专注图标使用实心 `MobQuietHoursSolid`。专注命令同样写入
  `command.txt`，媒体辅助进程通过 `QuietHoursSettings` 本地 COM 服务的
  `UserSelectedProfile` 在 `Microsoft.QuietHoursProfile.PriorityOnly` 与
  `.Unrestricted` 间切换，并把结果写入 `state.txt` 的 `focus=` 行；`SHQuery` 之外
  的窗口通过该状态文件实时同步。
- 日期时间弹出层由 `PopupKind::Calendar` 承载：320 DIP 高的大号秒级时钟每秒
  `PopupWindow::Tick` 检测 `status.now.wSecond` 并重绘；下方迷你日历只读，整块区域
  是 `CmdOpenSystemCalendar` 按钮。`LaunchSystemCalendar()` 通过 `SendInput`
  发送 `Win+N`，打开系统任务栏自己的通知中心/日历面板；任务栏可见时也可直接命中
  托盘时钟，但统一走 `Win+N` 以免自动隐藏任务栏上的坐标失效。
- 控制中心与日期时间弹层的 DComposition visual 在打开时使用 Atrium `kPop` 物理弹簧：
  `response=0.36 s`、`damping=0.62`，闭式解每帧计算 0.92→1 的缩放（约 0.2 s 轻微
  过冲后回落），600 ms 结束；opacity 用三次缓出 160 ms 淡入。日期时间弹层共用同一
  弹簧。普通菜单/面板保持原 190 ms 缩放，避免所有弹窗都被弹簧化。
- 网络、电池、控制中心合并为一个 `statusCluster` BarItem：一个悬停底板、一次点击
  打开控制中心。`GetAdaptersAddresses` 每 1.5 s 检测物理以太网（`IF_TYPE_ETHERNET_*`，
  要求 OperStatus up 且存在网关），优先于 WLAN 的 `wifiConnected`；无连接时使用
  `wifi_off`。电池来自 `GetSystemPowerStatus`：按 `battery_low/medium/full/charging`
  切换，无电池时整个电池图标省略；状态变化会进入 `statusKey` 触发栏重绘。
- 右侧项间距由 `rightIconSpacing`（2–24 DIP，默认 20）控制，同时用于
  statusCluster 内部字形、搜索、时钟和托盘图标；托盘图标位图按 `imageVersion` 缓存。
- `Layout` 每帧重建 `BarItem`，但按 `MenuId`/托盘图标身份继承上一帧的 hover 进度；
  否则 hover 从 0 重新开始，悬停底板会逐帧闪烁。statusCluster 始终以整个 `item.plate`
  绘制一块连续强调底板，无论控制中心是否打开；悬停任意状态图标都会点亮整块。
  时钟文本的 `item.text` 左右各内缩 8 DIP，使右对齐的日期/时间在强调板中保持
  左右对称。
- 托盘 UI：`BarWindow` 按 `tray_view.h::TrayIconShown()` 选显示项（hide → show →
  `IsPromoted`），`IsPromoted` 图标直接排在右侧状态区之前，其余经 Chevron（`E70D`）
  打开 `PopupKind::Tray` 玻璃网格面板；面板打开期间列表原地刷新。标题用
  `TrayModel::Title()`；Host 图标用实时 HICON，read-back 图标用 Explorer 快照或程序
  图标，统一生成 32×32 预乘 BGRA 并按 `imageVersion` 缓存位图。面板命中项携带
  `atrium::TrayIcon` 身份，左/右键、双击与悬停都交给 `TrayModel` 解析分发，不按序号
  点击。
- 原生把存活记录的 `recordKey` 写入 `%LOCALAPPDATA%\SuperMenu\tray-live.txt`；设置中心
  读取 15 秒内的快照只显示当前存活图标，用户用 `trayShow`/`trayHide`（十进制 record
  id，分号分隔）覆盖 `IsPromoted`，设置热重载后按同一份数据重绘。
- 前台焦点为 Explorer 桌面时，`App::UpdateDesktopMenuState()` 关闭弹出面板并触发
  重排；`BarWindow::Layout` 隐藏 AppMenu/File/Edit/View/Window/Help 以及“访达”应用名，
  只保留玻璃顶栏、Apple 菜单和右侧状态区。全屏应用仍然隐藏整个顶栏。
- 控制中心内部卡片改为胶囊式：圆角半径等于卡高一半，填充为半透明白/暗渐变 + 亮边，
  叠加在面板自身的液态玻璃背板上；瓦片、滑块与媒体控制共用同一套 Segoe Fluent Icons
字形和光学尺寸。
- 亮度优先 DDC/CI（主显示器 VCP 0x10），失败回退 WMI
  `WmiMonitorBrightnessMethods`；`GetDisplayBrightness` 同时按 DDC→WMI 顺序探测。
- 打开设置时先枚举已存在的 `SuperMenu.Settings.exe` 顶层窗口，恢复并前台激活；
  没有才启动新实例。
- 控制中心媒体按钮通过 `SendInput` 发送 `VK_MEDIA_PLAY_PAUSE/NEXT/PREV`：必须把
  `cbSize` 设为单个 `INPUT` 的大小（不是数组总大小），并附带 `KEYEVENTF_EXTENDEDKEY`
  与 `MapVirtualKey` 扫描码；否则系统返回 `ERROR_INVALID_PARAMETER`，媒体键不会到达
  Edge、QQ 音乐等应用。
- 正在播放元数据由随包的 **`Media\SuperMenu.Media.exe`**（.NET 10 + WinRT
  `GlobalSystemMediaTransportControlsSessionManager`）采集：它每 500 ms 把
  `state.txt`（标题/艺术家/专辑/播放状态/位置/时长/封面 hash/程序图标 hash）、
  `cover.bin`（播放器原始缩略图）和 `appicon.png`（从 AUMID 匹配到的进程 exe
  提取的程序图标）原子写入 `%LOCALAPPDATA%\SuperMenu\media`。
  `MediaMonitor`（`native/src/media_monitor.cpp`）轮询这个轻量快照，用 WIC 把封面
  和程序图标解码为 96×96 预乘 BGRA；原生栏启动 helper，并在退出时终止它（helper
  也会在父进程消失后自行退出）。
- 面板媒体卡用 bitmap-brush 填充圆角 geometry 来裁切封面和程序图标（避免 D2D
  geometric-mask layer 在 MinGW + 旧 D2D 驱动上的空内容问题）；标题/作者先按
  DirectWrite 测量宽度做省略。媒体卡不再显示进度条，标题/作者占用原来的进度区域。
- 控制中心去掉外层面板：`GlassParams.PartRect/PartRadii/PartCount` 把玻璃着色器
  限制成 6 个独立圆角岛（3 个连接/专注胶囊、190×190 媒体卡、2 个滑杆）。`MakeParams()`
  与 `BuildLayout()` 共用同一组矩形；`WM_NCHITTEST`、弹出层外点关闭钩子也只把这些
  岛算作“控制中心区域”，其余空隙/窗口外部都按点击外部处理。
- 控制中心不再使用 popup 专用材质，而是和顶栏一样调用
  `ComputeMaterial(s, scale, s.barHeight, false)`：玻璃预设、透明度、模糊、折射、
  高光、阴影全部来自同一用户设置；只有在关闭外层面板时才额外把材质限制到 PartRect 岛。
- `BarWindow` 收到 `WM_DISPLAYCHANGE` / `WM_DPICHANGED` 时调用
  `App::OnDisplayChanged()`；1.2 s 防抖后 `RestartForDisplayChange()` 以
  `--restart-after <oldPid>` 启动新进程。新进程等待旧进程退出（释放 AppBar 和
  singleton mutex）后再创建栏，避免分辨率/DPI 改变后沿用旧尺寸或捕获 ROI。

## 4. 输入与菜单

```
点击顶栏项
  ├─ 顶级菜单 → PopupWindow::Open(Menu, MenuModel, anchor)
  ├─ 状态项   → PopupWindow::OpenPanel(Wifi/Battery/Volume/ControlCenter/Calendar/Notifications)
├─ 顶栏/托盘右键 → BuildBarContextMenu() → PopupWindow::Open(Menu, Context, anchor=cursor)
├─ 菜单绘制      → DrawMenu 不再绘制 menu_.items[].shortcut，宽度计算不计快捷键列
  └─ 搜索     → 关闭面板并发送 Win+S
打开面板时安装低级鼠标/键盘钩子：
  ├─ 鼠标点到面板与顶栏之外 → 关闭
  ├─ Esc → 关闭
  └─ 菜单中 ↑/↓ 选择、Enter 执行
关闭时移除钩子；不拦截普通点击/按键
```

菜单命令的“前台窗口”在打开菜单时记录为 `targetHwnd_`；点击菜单项后：

- 关闭 Application：`PostMessage(WM_CLOSE)`
- 最小化 / 最大化 / 半屏：`ShowWindow` 或注入 `Win+←/→`
- 撤销/复制/粘贴/全选：`SendInput(Ctrl+…)`
- 系统动作：`LockWorkStation`、`ExitWindowsEx`、`SetSuspendState`（重启/关机/注销前弹确认）
- 打开系统设置：`ShellExecute("ms-settings:…")`

菜单数据在 `menu.cpp::BuildMenu` 中按当前设置的语言构建；`MenuBar` 每次布局/渲染时
读取最新前台应用名，因此切换应用会即时反映。

## 5. 设置同步

```
Settings.exe --保存--> settings.json.tmp --Replace--> settings.json
                             │
                             ├─ Broadcast "SuperMenu.SettingsChanged"
                             ▼
SuperMenuBar::TrayWndProc 收到广播 → settingsStamp_ = 0 → 下一个 tick 重载
（另有 700 ms 文件时间戳轮询兜底，适用于直接编辑文件的情况）
```

原生设置解析器是内置的扁平 JSON 解析器（不依赖第三方库）；所有键都有类型/范围校验，
坏文件不会崩溃。设置变化后可以热应用：栏高、材质、模块、语言、主题、自动隐藏、
工作区预约；显示器索引变化会提示重启。

## 6. 源文件

| 文件 | 职责 |
|---|---|
| `main.cpp` | DPI、COM、单实例、启动 |
| `app.cpp/h` | 应用编排、窗口/菜单/面板、系统命令、托盘、钩子、设置重载 |
| `bar_window.cpp/h` | 顶栏窗口、布局、绘制、鼠标命中、AppBar |
| `popup_window.cpp/h` | 弹出面板窗口、菜单/控制中心/日历/信息面板绘制与交互 |
| `menu.cpp/h` | 菜单模型、命令 ID、中英文本 |
| `glass_surface.cpp/h` | ROI、mip、luma、swap chain、DComp、D2D 叠加 |
| `capture.cpp/h` | Desktop Duplication、窗口捕获排除、壁纸回退（WIC） |
| `gpu.cpp/h` | 显示器/适配器、D3D11、D2D、DWrite、运行时着色器编译、HarmonyOS Sans SC 与 Segoe Fluent Icons 私有字体集合 |
| `settings.cpp/h`、`json.cpp/h` | 设置文件解析/校验/材质参数 |
| `system_status.cpp/h` | 前台应用、电池、WiFi、音量、亮度、系统命令、托盘程序激活 |
| `atrium/shell/tray_model.*` | 托盘唯一数据源：read-back（UIA/注册表/GetRect）+ Host 合并、点击路由与证据（Atrium Desktop MIT） |
| `atrium/shell/tray_match.*`、`tray_click.h`、`tray_target.h`、`tray_pointer.h`、`tray_image.h`、`tray_taskbar.h` | 纯函数/辅助模块：身份对账、点击策略、目标校验、物理右键、素材选择、任务栏临时显示（Atrium Desktop MIT） |
| `atrium/shell/tray_host.*`、`tray_protocol.h` | 隐藏 `Shell_TrayWnd` 托盘宿主、shell 回调协议、会话/所有权与广播 |
| `tray_view.h` | 托盘图标显示判定、标题与设置覆盖的纯函数 |
| `ui_util.cpp/h`、`fluent_glyphs.h`、`icons.cpp/h` | 字体/排版、Segoe Fluent 私有字形、品牌标志矢量、墨色、悬停动画 |
| `shaders/glass.hlsl` | 液态玻璃着色器 |
| `resources/` | manifest、图标、版本信息 |

## 7. 性能与边界

- 栏窗口 2800×66 @ 150%：ROI 2800×120 ≈ 1.3 MB，mip 链约 +1.7 MB；一次玻璃
  绘制约 6 万像素，帧代价很小。静态桌面时只有系统状态时钟/电量变化才重绘。
- 捕获失败是正常状态（锁屏、UAC、远程桌面、其它录屏程序占用 duplication）：
  自动回退壁纸，顶栏继续可用，恢复后自动重建。
- Windows 10 19041+；`DCompositionWaitForCompositorClock` 未使用（MinGW 头文件
  缺失），用定时器 + DWM 合成即可。

