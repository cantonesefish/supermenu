# 托盘图标

本次重构后，托盘只有一个数据源：`native/src/atrium/shell/tray_model.*` 的
`atrium::TrayModel`。它把 **read-back（读回 Explorer 的托盘）** 与 **host（自己接管
`Shell_NotifyIcon` 流）** 两个来源按真实身份合并成一份列表；UI 线程只排队刷新、读取
缓存并绘制。旧 `sm::TrayReader`（`native/src/tray.h/.cpp`，注册表截图 + 合成点击 +
启动 exe 回退）已删除。

代码移植自 Atrium Desktop（MIT，<https://github.com/wynter-yaxley/atrium-desktop>），
归属见 [`THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md)；根因、方案对比与完整
边界见 [`research/tray-refactor.md`](research/tray-refactor.md)。

## 架构：两个数据源，一份列表

```
应用 Shell_NotifyIcon ──WM_COPYDATA──> TrayHost（隐藏 Shell_TrayWnd，独立线程）
                                         │ 实时 HICON / 回调消息 / 版本 / tooltip
                                         └─ 原样转发 ──> Explorer（系统托盘保持完整）
Explorer 托盘（Win11 XAML；无 ToolbarWindow32）
  └─ UIA 按钮 + HKCU\Control Panel\NotifyIconSettings 记录 + GetRect 存活探测
                                         │
                                         ▼
                    atrium::TrayModel（MTA worker；UIA/注册表/图片/点击确认都在这里）
                       read-back ──┐
                                   ├── 按身份合并 ──> 一份列表
                       host ───────┘    （Windows UIOrderList 顺序 + IsPromoted）
                                         │  postMessage(wParam)
                                         │  0=列表变化 1=观察到的应用响应
                                         │  2=系统托盘已打开 3=需要用户 4=accepted-unconfirmed
                                         ▼
                     UI 线程：BarWindow 顶栏 + PopupKind::Tray 隐藏图标面板
```

- **read-back**：UIA 枚举 Win11 XAML 任务栏 `DesktopWindowContentBridge` 下的
  `NotifyItemIcon` 按钮，以及 `TopLevelWindowForOverflowXamlIsland` 溢出区的按钮
  （溢出内容只在面板可见/物化时可枚举）；Win10/旧壳退回经典 `ToolbarWindow32`。
  同时读 `NotifyIconSettings` 的 `ExecutablePath`、`InitialTooltip`、`IconSnapshot`、
  `IsPromoted` 与 `UIOrderList`，用 `Shell_NotifyIconGetRect` 做 GUID / HWND+UID
  存活探测，再由 `tray_match` 按几何、名字、顺序、运行程序对账。记录只是缓存元数据，
  `S_OK` 只证明外壳认为图标存在，不证明窗口/PID 归属。
- **host**：独立线程的隐藏 `Shell_TrayWnd` 抢在 Explorer 之前收到应用的
  `Shell_NotifyIcon`，当场复制实时 HICON，记录回调消息、`NOTIFYICON_VERSION`、
  `NIF_SHOWTIP`、`NIS_HIDDEN` 与 tooltip，随后把每次调用原样转发给 Explorer
  （`WM_COPYDATA` `dwData=1` 图标、`dwData=3` 的 GetRect 询问；AppBar 等消息同样转发）。
  Explorer 拒绝重复 `NIM_ADD` 时改发 `NIM_MODIFY`；Explorer 自己的系统图标
  （音量/网络/电池等）不接管。
- **合并**：同一真实身份（GUID、HWND+UID 或程序）只出现一次，绝不按位置/名字合并；
  Host 图标排前并用实时 HICON 覆盖 Explorer 快照，Host-only 图标追加在已知记录之后，
  只有 Explorer 有的图标保留读取结果并标记来源与匹配方式。顺序用 Windows 自己的
  `UIOrderList`，显示位置用 Explorer 的 `IsPromoted`；Host 让位时整表退回 read-back。
- **设置覆盖**：`tray_view.h` 在 `IsPromoted` 之上应用 `trayShow` / `trayHide`
  （按 `recordKey` 数字记录 id）；没有覆盖时顶栏与 Windows 托盘完全一致。
  SuperMenu 自己的托盘图标被过滤；存活记录 id 仍写入
  `%LOCALAPPDATA%\SuperMenu\tray-live.txt` 供设置中心显示。

## 与其他 tray host 共存

旧版本在 Atrium 同时运行时出现过 `TaskbarCreated` 广播风暴：两个 Host 互相把对方的
广播当作 Explorer 重启，反复抢 z-order 并重发图标（旧日志 937 次广播 / 928 次让位 /
925 次抢回 / Atrium 819 次“Explorer restarted”）。现在：

- Host 启动时记录 shell 会话身份（`Shell_TrayWnd` HWND + Explorer PID + 进程创建
  时间）。收到 `TaskbarCreated` 时只有会话变化才当作 Explorer 重启；其他 Host 的
  广播被忽略（限频记录），不再触发重发。
- 只在启动后广播一次、以及确认 Explorer 重启后广播一次。
- 启动时若另一程序的 `Shell_TrayWnd` 在最前，只做一次接管尝试；失败就在本会话内固定
  让位（`Yielded()`），之后不争抢、不再广播，读取模式继续工作。
- 转发语义不变：每次图标更新仍转发给 Explorer，Host 退出/崩溃后 Explorer 自动完整
  接管，应用不需要恢复。

## 点击分发

UI 线程把点击排给 worker，worker 重新解析目标身份后再动作：

1. **Host-owned 图标**：按应用注册的 callback message 与 `NOTIFYICON_VERSION` 发送
   shell 协议（v0–v4 的 MOVE/LDOWN/LUP、v3+ 的 `NIN_SELECT`、右键的
   `WM_CONTEXTMENU`、v4 的指针坐标 + uid 编码；双击第二击发 `LBUTTONDBLCLK`
   续发），发送前 `AllowSetForegroundWindow` 给图标进程。可见和隐藏图标都适用。
2. **read-back**：UIA `Invoke`（左键）/ `IUIAutomationElement3::ShowContextMenu`
   （右键）；明确失败后依次尝试 Win10 经典 toolbar 消息、`LegacyIAccessible`
   的 `DoDefaultAction`。
3. **回退**：恢复该程序已经存在且可见/最小化的窗口并检查前台；右键不以打开主窗口
   代替应用菜单。仍不行时，仅为一次点击短暂显示被自动隐藏的任务栏，用合并的
   `SendInput` 做真实右键（`tray_pointer` + `tray_taskbar`；用户任何输入立即取消，
   用后恢复光标与任务栏）。
4. **系统溢出**：最后打开 Explorer 自己的溢出区让用户操作——Win11 用非本地化的结构
   签名 Invoke chevron（签名不唯一则拒绝猜测），Win10 用 `TrayNotifyWnd` 下唯一
   Button 的 `BM_CLICK`；只有内容确实可见、矩形有效且落点命中才算成功。

结果通过 postMessage 报告：`observed`（观察到前台窗口/新窗口/菜单等响应证据）、
`accepted-unconfirmed`（请求被接受但无证据，安静结束，不再换 API 重试，避免开关类
动作反转）、`system-tray`（系统溢出已打开，不算原生点击成功）、`needs-user`
（确实无法自动分发时的提示）。**任何 HRESULT 都不当作应用回执。**

双击：单击立即排队；第二击在系统双击时间内且身份一致时按 shell 语义续发。XAML 没有
公开双击 Pattern，第二击只再做一次 Invoke，不宣称原生等价双击（见边界）。

## 线程与刷新

- 所有 UIA、注册表、GetRect、进程/版本查询、图片解码与点击确认都在 MTA worker；
  UI 线程 `Refresh()`/`Click()`/`Hover()` 只排队，不阻塞渲染。
- 刷新请求合并；Explorer 重启/首次启动触发刷新，失败时做有限次后台重试；
  跨 shell 实例的点击拒绝自动分发。
- 托盘面板打开期间列表原地刷新（Host 可以 post 图标更新），不再因列表变化关闭重开；
  刷新按图标身份保留指针的悬停底板（cell 序号变了也跟随同一图标），
  面板尺寸未变化时不重建玻璃表面——后台图标更新不会让底板闪动或闪烁。

## 验证（2026-11-21，Windows 11 build 26300）

- **悬停底板稳定性（回归）**：测试托盘图标每 100 ms 发一次 `NIM_MODIFY`（Host 每 100 ms
  刷新一次面板）并让真实指针在隐藏面板图标上移动：修复前 20 帧中悬停底板丢失 2 帧、
  4 次状态跳变（用户可见闪动）；修复后 20/20 帧保持、0 次跳变。
- **构建/单测**：native Release 构建通过；`ctest --test-dir native/build` 4/4 通过，
  共 137 项检查（tray_match 22、tray_click 60、tray_protocol 15、tray_pointer 40）。
- **Host 模式实机（只运行 SuperMenu）**：恰好 1 次 `TaskbarCreated` 广播、0 次让位、
  0 次“in front again”、0 次 Explorer 重启（旧日志为 937 / 928 / 925）。read-back 对账
  37 条记录 → 15 个存活图标、0 个未知占位；顶栏显示 1 个 promoted（Tailscale）+ chevron，
  隐藏面板 13 个 = 15 − 1 − 本程序图标。promoted 左键打开了 Tailscale 真实 Win32 菜单，
  promoted 右键、隐藏图标左/右键（华为云盘、SZU 网络守护）与双击续发都记录了
  `-> sent` 的原生回调与 `tray: left/right click on ...`。
- **read-back 回退（探针、无 Host）**：对 Tailscale 用 UIA `ShowContextMenu` 右键，
  日志 `uia-context-menu ... observed=1`，结果 `observed`。
- **与 Atrium 共存（旧风暴配置）**：启动 1 次广播；日志 1 次“another program's tray
  took the stream: one attempt to take it back”后接管成功；最终 `Shell_TrayWnd`
  z-order 为 SuperMenu → Atrium → Explorer；0 让位/0 抢回/0 重启/0 额外广播。
  Atrium 同窗口日志只有 1 次“left there”、0 次“in front again”。共存下点击仍是原生
  回调。read-back 16 个图标（含 Atrium 自己的图标），面板 14 隐藏 + 1 promoted；两个
  无法读取进程路径的提升权限应用（华为电脑管家/华为控制中心）按 GUID/HWND 身份合并，
  没有重复显示。
- 验证后已重启本机安装版 SuperMenu 与 Atrium，登录计划任务指回
  `C:\fish\SuperMenu\SuperMenuBar.exe`。

## 已知边界

1. **UIA 托盘结构未公开**：`NotifyItemIcon` / `SystemTrayIcon` 等标识、层级与可见性
   随 Windows 版本/任务栏形态变化；识别基于非本地化结构并 fail closed。
2. **隐藏图标要物化后才能点**：溢出区按钮只在弹出面板打开、内容真实可见后才作为点击
   目标；隐藏图标平时由存活注册表记录补充显示，不显示假矩形/占位。
3. **v4 版本回退**：应用重新注册后若没有再次 `NIM_SETVERSION`，Host 可能按 version 0
   编码点击；对多数应用可用，但不保证全部。
4. **提升权限应用**：UIPI 可能拒绝普通权限 Host 的回调消息，此时改用 read-back 路径
   （可能只恢复到窗口/系统溢出，不等价于应用原操作）。
5. **顽固的其他 tray host**：只做一次接管尝试，失败后本会话让位并只用 read-back；
   让位期间拿不到对方 Host 的实时图标，列表可能不完整。
6. **系统图标**：音量/网络/电池等 Explorer 自己的图标仍由 Explorer 提供，不经过 Host。
7. **本程序图标**：SuperMenu 自己的托盘图标会被过滤，不在顶栏/面板显示。
8. **不同身份不合并**：两个同名“Windows 资源管理器”记录若 GUID 不同，按不同身份保留。
9. **XAML 双击**：没有公开双击 Pattern，第二击只是第二次 Invoke，不等于原生双击。
10. **平台验证范围**：本机只验证了 Windows 11 build 26300；Win10 经典 toolbar 路径
    未在本机实机验证（代码保留并按公开消息实现）。
11. **不做“完美镜像”承诺**：read-back 的图标图片仍是 Explorer 的缓存快照；实时动画、
    闪烁/角标只有在 Host 接管时才是应用当前 HICON，且不同 Windows 版本/程序不保证
    100% 一致。

## 代码位置

| 文件 | 职责 |
| --- | --- |
| `native/src/atrium/shell/tray_model.{h,cpp}` | 唯一托盘模型：read-back 枚举/对账、host 合并、点击路由、证据观察、worker |
| `native/src/atrium/shell/tray_match.{h,cpp}` | 纯函数身份对账（按钮/记录/运行程序 → 配对、别名、共享位置） |
| `native/src/atrium/shell/tray_click.h` | 纯点击策略与回退顺序、经典鼠标手势、结果分类 |
| `native/src/atrium/shell/tray_target.h` | 点击/双击的身份解析与 shell 会话校验 |
| `native/src/atrium/shell/tray_pointer.h` | 物理右键回退（用户输入取消、光标恢复） |
| `native/src/atrium/shell/tray_image.h` | 图标素材选择（实时快照优先于程序品牌图） |
| `native/src/atrium/shell/tray_taskbar.h` | 仅为一次物理点击临时显示自动隐藏的任务栏 |
| `native/src/atrium/shell/tray_protocol.h` | Host 回发应用的 shell 回调协议（v0–v4） |
| `native/src/atrium/shell/tray_host.{h,cpp}` | 隐藏 `Shell_TrayWnd` 宿主、转发、广播与会话/所有权 |
| `native/src/tray_view.h` | UI 侧纯函数：显示判定、标题、`recordKey` 解析 |
| `native/tests/tray_*_test.cpp`、`tray_probe.cpp` | 纯函数回归与只读/实机探针（`SUPERMENU_BUILD_TESTS=ON`） |
