# 托盘重构研究：Read-back + Host 双源托盘

- 状态：Phase 1 研究/设计记录；2026-11-21 实现完成并通过构建、单元测试与实机验证（结果见 §7）
- 日期与机器：2026-11-21，Windows 11 build 26300（UBR 10075），本机
- 目标仓库：`C:\fish\git\kevison-supermenu`
- 参考实现：Atrium Desktop（MIT），只读本地源 `C:\fish\git\atrium-desktop-main`
- 历史记录：`docs/research/tray-module.md`（旧 `TrayReader` 设计，其 `native/src/tray.{h,cpp}` 已删除）；当前架构与验证见 `docs/TRAY.md`

本文记录托盘区域两个用户可见问题的根因证据、方案对比、本次移植的目标架构与已知边界。所有数字与日志行均来自本机日志或只读探针；未复现的行为不写入“已证实”。

## 1. 现状与复现证据

### 1.1 用户可见的两个问题

1. **图标有时错误或缺失**：托盘列表来自 Explorer 的持久化缓存，而不是应用当前的图标流。
2. **左/右键不作用于真实应用**：读取路径没有可用的图标矩形时，点击退化为“启动 exe”；Host 协议点击只有在 Host 未让位且图标有 `hostKey` 时才可用，而本机 Host 长期处于让位状态（见 §1.3）。

旧实现（`native/src/tray.h/.cpp` 的 `TrayReader` + `native/src/app.cpp` 的 `ActivateTrayIcon`）同时混用注册表记录、`Shell_NotifyIconGetRect` 和合成鼠标点击；Host 收到的实时图标只是按 `exe + uid` 合并进登记记录。旧设计见 `docs/TRAY.md`，旧读取器细节见 `docs/research/tray-module.md`。

### 1.2 旧读取路径的实测数据

日志：`%LOCALAPPDATA%\SuperMenu\logs\supermenu.log`（2,722,395 字节，覆盖 2026-11-19 19:17:07 — 2026-11-21 11:32:13）。读取器每轮打印同一行：

```
[2026-11-21 11:32:12.757] TrayReader: 14 icons (1 promoted, 0 clickable) from 37 records in 43.75 ms (own=3 duplicates=2 stale=18)
```

即 37 条 `HKCU\Control Panel\NotifyIconSettings` 记录里 18 条判为 stale、3 条属本程序、2 条重复，最终 14 个图标，但**没有任何图标有可点击矩形**（`0 clickable`）。日志中 `no clickable rect` 共 18 次，全部退化为启动 exe：

```
[2026-11-21 11:15:23.256] tray: no clickable rect for Tailscale GUI client; launching c:\program files\tailscale\tailscale-ipn.exe
[2026-11-20 22:31:45.921] tray: no clickable rect for Weixin; launching c:\program files\tencent\weixin\weixin.exe
```

来源（旧实现，相关代码已随本次重构删除）：`native/src/app.cpp:742`。旧路径会先尝试“把鼠标移到屏幕底边唤醒自动隐藏的任务栏再重新读取矩形”，但对隐藏图标只能得到同一个溢出箭头矩形（`native/src/app.cpp:724-740`、`native/src/tray.cpp:922` 的 `overflowProxy`）。同一份日志里：18 次因无矩形而启动 exe（`no clickable rect`），8 次打开系统溢出区（`opening system overflow`），6 次在溢出面板打开后转发隐藏图标点击（`forwarding hidden`）；只有 Host 未让位且图标带 `hostKey` 时才有 Host 回调点击（`tray host: left/right click` 共 55/204 次）。而日志同时记录 928 次让位，让位期间的点击只能落到读取/溢出/启动 exe 回退，这就是“点击不能作用于真实应用”的直接原因之一。

注册表本身也确认是缓存：本机 37 个子键、37 个 `IconSnapshot`、10 个 `IconGuid`、9 个 `IsPromoted=1`、`UIOrderList` 296 字节（37 个 uint64）。`IconSnapshot` 是 Explorer 保存的静态 PNG，不含回调消息、`NOTIFYICON_VERSION` 或实时 HICON，不能作为“点击哪个应用”的身份。

### 1.3 两个托盘宿主的 TaskbarCreated 广播风暴

本机同时运行安装版 SuperMenu 与 Atrium，两者都注册隐藏 `Shell_TrayWnd` 抢收 `Shell_NotifyIcon`：

- SuperMenuBar.exe pid 12436：`C:\fish\SuperMenu\SuperMenuBar.exe`
- Atrium.exe pid 12528：`C:\fish\Atrium\Atrium.exe`
- explorer.exe pid 12644
- 只读窗口枚举同时看到 3 个 `Shell_TrayWnd`：Atrium `0x80326`（pid 12528，最前）、Explorer `0x10190`（pid 12644）、SuperMenu `0x1052E`（pid 12436），任务栏被自动隐藏时均 `IsWindowVisible=False`

两侧日志相互印证（同一秒一一对应）：

```
# SuperMenu supermenu.log
[2026-11-21 11:19:53.166] tray-host: tray host: on, apps asked to add their icons again
[2026-11-21 11:19:53.195] tray-host: tray host: another program's tray (pid 12528) is in front: left there
[2026-11-21 11:19:53.289] tray-host: tray host: in front again
[2026-11-21 11:19:53.493] tray-host: tray host: on, apps asked to add their icons again

# Atrium core.log
2026-11-21 11:19:53.166 [17088] tray host: Explorer restarted
2026-11-21 11:19:53.289 [17088] tray host: in front again
2026-11-21 11:19:53.492 [17088] tray host: another program's tray (pid 12436) is in front: left there
2026-11-21 11:19:53.492 [17088] tray host: Explorer restarted
```

完整计数（截至上述日志末尾）：

| 计数点 | SuperMenu `supermenu.log` | Atrium `core.log` |
| --- | ---: | ---: |
| 主动广播 `TaskbarCreated`（“apps asked to add their icons again”） | **937** | — |
| 判定为 Explorer 重启（“Explorer restarted”） | 17 | **819** |
| 让位（“another program's tray … left there”） | **928** | 268 |
| 抢回最前（“in front again”） | **925** | 266 |

（SuperMenu 的 928 次让位按对方 pid 拆分：pid 12528 = Atrium 634 次、pid 12868 = 281 次、pid 6808 = 11 次、pid 13812 = 1 次；日志在统计时仍在追加，与更早的只读快照可能相差 1–2 行。）

双方第一次事件出现在同一时刻 `2026-11-20 21:07:26.435`（SuperMenu 首次广播 / Atrium 首次“Explorer restarted”），说明循环从 SuperMenu Host 启动即开始。Atrium 的 819 行集中在这条循环上：突发期它每隔约 0.32 s 记录一次，和 SuperMenu 的广播**逐对同刻**（Atrium 的首行时间与 SuperMenu 首次广播时间完全一致）；SuperMenu 侧的 17 行则分布在两天内不同时刻（11-20 21:10、22:15、11-21 11:32 等），远少于广播次数、也不与循环逐次对应，更像真实 Explorer 重启（其自身 3 秒广播过滤把另一 Host 的突发广播挡在了同一分支外）。突发期连续 17 次 SuperMenu 广播的相邻间隔为 **0.317–0.343 s**，Atrium “Explorer restarted”的相邻间隔与之逐对一致。每个周期两边都会再次“add”全部图标（SuperMenu 单周期 3–5 行 add，Atrium 6–8 行）。

根因在重构前的 `native/src/atrium/shell/tray_host.cpp`（以下行号为修复前版本）：

- `WM_TASKBARCREATED` 处理只忽略 **自己 3 秒内**的广播，任何其他来源都当作 Explorer 重启（`:376-386`）；另一个 Host 的广播恰好落在这个窗口外，于是被消费为重启。
- `KeepFirst()` 在“in front again”时置 `kRecollectTimer`，200 ms 后 `Broadcast()`（`:559-565`、`:341-344`、`:272-277`）：抢回最前就再次广播，形成自激。
- 两个 Host 各自用 `Yielded()` 让位、又各自抢回，z-order 来回翻转，应用图标在整个突发期被反复 `NIM_ADD/NIM_MODIFY`。

结论：Host 本身是需要的（见 §2），但广播与所有权必须有确定的 shell 会话依据，不能靠“谁先到最前谁再广播”互相触发。

### 1.4 Windows 11 托盘窗口与 UIA 树（本机复测）

只读探针（PowerShell + 标准 Win32 窗口 API + `UIAutomationClient`；不移动鼠标、不操作进程、不显示窗口）在 2026-11-21 11:36 于本机复测：

- Explorer 的 `Shell_TrayWnd`（`0x10190`）无 `ToolbarWindow32`；全部后代窗口为
  `Windows.UI.Composition.DesktopWindowContentBridge`、`Windows.UI.Input.InputSite.WindowClass`、
  `Windows.UI.Core.CoreWindow`、`Start`、`TrayDummySearchControl`、`TrayNotifyWnd`、
  `ReBarWindow32`、`MSTaskSwWClass`、`MSTaskListWClass`。
- `TrayNotifyWnd` 直接子窗口数 **0**（childless）；桌面不存在 `NotifyIconOverflowWindow`（那是 Win10 经典路径）。
- UIA 根不是 `Shell_TrayWnd` 本身，而是其子窗口 `DesktopWindowContentBridge`（`0x102D0`）：其下 19 个后代中，`AutomationId=NotifyItemIcon` 的按钮恰好 **1 个**：`Tailscale: Connected. Click for options.`（class `SystemTray.NormalButton`，rect `2243,1438,32,48`，`IsOffscreen=False`）——即使 `Shell_TrayWnd` 和任务栏窗口都 `IsWindowVisible=False`，这个 promoted 按钮仍可枚举。
- 同层 9 个 `AutomationId=SystemTrayIcon` 的系统按钮；溢出箭头“显示隐藏的图标”是 `SystemTrayIcon` + `SystemTray.NormalButton`，排在唯一 `NotifyItemIcon` 之前、系统按钮之前，和移植后 `ShowOverflow()` 的结构签名（`before==1 && after>=1`，`native/src/atrium/shell/tray_model.cpp:531-590`）一致。
- 溢出区是独立的顶层窗口 `TopLevelWindowForOverflowXamlIsland`（`0x1708AA`，隐藏），内部同样有 `DesktopWindowContentBridge`（`0x960A1E`）；UIA 在其下枚举到 **16 个** `NotifyItemIcon` 按钮（SuperMenu、SZU 网络守护、华为控制中心、华为电脑管家、Atrium、Sunshine、华为云盘、云空间、TranslucentTB、Intel Killer、Intel 驱动助手、蓝牙设备、NVIDIA 设置、安全删除硬件、Windows 安全中心等），但全部 `IsOffscreen=True`、`BoundingRectangle=Empty`：**内容未物化前不能当点击目标**，这正是需要先经结构验证的 chevron 打开溢出区、再核验实际落点的原因。

任务记录中由 lead 的 MinGW/CUIAutomation8 探针验证的 `NotifyItemIcon` 枚举方式与上述复测一致；本复测补充了“隐藏任务栏下 promoted 按钮仍可读、溢出按钮存在但 offscreen/空矩形”的边界条件。

## 2. 方案调研

| 方案 | 代表实现 | 数据源 / 点击方式 | 主要问题 | 结论 |
| --- | --- | --- | --- | --- |
| (a) 注册表 + `Shell_NotifyIconGetRect` + 截屏/合成点击 | 旧 `TrayReader` | `IconSnapshot` PNG、`UIOrderList`、GetRect 矩形、`SendInput`/启动 exe | 快照是缓存、隐藏图标共享溢出箭头矩形、需要移动光标或唤醒任务栏、拿不到回调消息/版本，身份只能靠 exe/位置猜测 | **弃用** |
| (b) UI Automation 读回 | `tray_model.cpp` 的 read-back；Atrium/TRAY.md | `NotifyItemIcon` 按钮的 name/rect/RuntimeId/分区 + 注册表记录 + GUID/HWND/UID 存活探测；Invoke / `ShowContextMenu` / 经典 toolbar / `LegacyIAccessible` | UIA 结构未公开、隐藏图标需先物化、部分元素/程序不支持对应 Pattern | **采用（读取与 Explorer-only 图标的点击路径）** |
| (c) `Shell_NotifyIcon` Host 拦截 | `TrayHost`；ManagedShell `TrayService`（Cairo/RetroBar 使用） | 隐藏 `Shell_TrayWnd` 抢在 Explorer 前收到 `WM_COPYDATA`，保存实时 HICON/回调消息/版本/tooltip，再原样转发给 Explorer；点击按 shell 协议回发 | 需要与其他 Host 共存；提升权限应用可能拒绝回调；不能绕过 UIPI | **采用（Host-first 的实时图标与原生点击）** |
| (d) 跨进程 toolbar + `ReadProcessMemory` | ManagedShell `ExplorerTrayService` | 读取 Explorer 工具栏缓冲区里的 HWND/UID/回调消息/版本 | 与本项目“不读取其他进程内存、不注入”的约束冲突；依赖经典 toolbar（Win11 XAML 已没有） | **不采用** |
| (e) `SetWindowsHookEx` 注入 DLL | Seelen-UI `tray_hook_loader.rs`/`tray_icon.rs`；YASB `trayhook.cpp` | 把 DLL 注入 Explorer 替换窗口过程，截获注册数据和位图 | 注入风险与维护成本高，违反无注入约束 | **不采用** |
| (f) `ITrayNotify` COM 回调 | Chromium `status_tray_state_changer_win.cc`；Atrium 探针 | 进程外注册图标增删回调 | 只提供通知，不提供可枚举的图标数据；Atrium 探针 500 ms 内 count=0 | **仅作探测，不接入生产** |

选择 (b) + (c) 组合的理由：

- **Host 拿实时事实**：实时 HICON、`uCallbackMessage`、`NOTIFYICON_VERSION`、`NIF_SHOWTIP`、`NIS_HIDDEN` 只有 shell 协议里有；这是“点击作用于真实应用”的唯一如实路径，且不需要注入。
- **Read-back 拿 Windows 的顺序与可见性**：`UIOrderList`、`IsPromoted`、GUID/UID 存活探测、Explorer 自己的系统图标（音量/网络等）以及 Host 让位时的降级列表，都只能从 Explorer 的托盘与注册表读回。
- **两者按身份合并**，既不需要伪造回调，也不需要假设“位置/名字相同就是同一个图标”；`(d)(e)` 被项目的无注入/无跨进程内存约束排除，`(f)` 不具备枚举能力。

## 3. 目标架构（已实现）

### 3.1 双源一表

`atrium::TrayModel`（`native/src/atrium/shell/tray_model.h`）对外只暴露一个 `Icons()` 列表，内部由两个数据源合并：

- **read-back**：UIA 按钮（`NotifyItemIcon`，含溢出区）+ `HKCU\Control Panel\NotifyIconSettings` 记录 + `Shell_NotifyIconGetRect` 存活探测，经 `tray_match` 按身份/几何/名字/顺序/运行程序对账；
- **host**：`TrayHost` 收到的实时图标（HICON、callback、version、hidden、flashing），按 `guid:{...}` / `uid:<exe>|<uid>` / exe 与记录身份配对。

合并规则：同一真实身份只出现一次；Host 图标排前并用实时 HICON 覆盖快照；Host-only 图标按到达顺序追加；只有 Explorer 有的图标保留读取结果；SuperMenu 自身图标被过滤；排序用 Windows 自己的 `UIOrderList`（Host 图标也在对应记录位置），显示位置用 `IsPromoted`；Host 让位时整表退回 read-back。绝不按 exe/提示/位置把不同身份合并。

### 3.2 Read-back 流程

`tray_model.cpp` 的 worker（MTA）每次刷新依次：

1. **shell 会话**：`ShellTaskbar()` 取 Explorer 的 `Shell_TrayWnd` + PID + 进程创建时间（`tray_target.h` 的 `ShellSession`）。会话变化则丢弃本次读取，避免把旧壳的 RuntimeId/身份套到新壳上。
2. **UIA 枚举**：`DesktopWindowContentBridge` / `TopLevelWindowForOverflowXamlIsland` 下按 `AutomationId=NotifyItemIcon` 取按钮（name→tooltip、矩形、RuntimeId、promoted/overflow 分区）；没有 XAML 托盘时退回经典 `ToolbarWindow32`（按钮 ControlType）。
3. **记录**：读 `NotifyIconSettings` 的 `ExecutablePath`、`InitialTooltip`、`IconSnapshot`、`IsPromoted`、`UIOrderList`；记录只是缓存元数据。
4. **存活**：GUID 用空 HWND 的 `Shell_NotifyIconGetRect`；UID 只有实际窗口通过 GetRect 后才生成 `uid:PID:HWND:uID`；`S_OK` 只证明外壳认为图标存在，不证明窗口/PID 归属。
5. **对账**：`tray_match`（纯函数，无 Windows 头）按几何（记录中心落在按钮内且双向唯一）、名字（双向唯一最佳、精确优先）、顺序（完整分区数量相等且锚点一致）、运行程序（Win10/无记录场景）匹配；多个身份共享同一矩形时标记 `sharedPlace`，永不用于几何匹配；剩余存活记录按 `UIOrderList` 补充。
6. **图标**：`tray_image` 规定“live snapshot 优先于程序品牌图”；PNG/应用图标解码为 32bpp 预乘 BGRA，带稳定 `imageVersion` 作为渲染缓存键。

`TrayModel` 结果里的 `TrayIcon` 同时携带：`tip`、`exe`、`image`/`imageVersion`、`promoted`、`index`、`runtimeId`、`shell`、`recordId`（`guid:`/`uid:`/`host:`）、`recordKey`（数字记录 id，供设置中心覆盖）、`hostKey`、`appTooltip`、`flashing`、`order`、`matchMethod`。`TargetKey` 是点击时用于 identity 校验的完整身份，RuntimeId 不当作永久身份。

### 3.3 Host 流程与共存（针对 §1.3 修复）

- 独立线程创建隐藏 `Shell_TrayWnd`（附带 `TrayNotifyWnd` 子窗口），保持在 Explorer 同名窗口之前；`WM_COPYDATA` `dwData=1` 图标写入、`dwData=3` 的 `Shell_NotifyIconGetRect` 询问、AppBar 与其他消息原样转发给 Explorer，Explorer 的返回值原样返回。
- 图标记录实时 HICON（当场复制）、callback、version、`NIF_SHOWTIP`、`NIS_HIDDEN`、tooltip；Explorer 拒绝重复 `NIM_ADD`（宿主自己广播造成）时改发 `NIM_MODIFY`；Explorer 已接受而宿主未见的 `NIM_MODIFY` 也补建条目；系统（Explorer 自己的）图标不接管。
- **广播策略**：`TaskbarCreated` 只在 Host 启动后广播一次、以及确认 Explorer 重启后广播一次，用于补收窗口空档期图标。
- **Explorer 重启判定**：收到注册消息时比对启动时记录的 shell 会话（HWND + PID + 创建时间）；会话未变则视为其他 Host 的广播，忽略并限频记录，不再当作 Explorer 重启。
- **所有权**：启动时若另一个 `Shell_TrayWnd`（非本进程、非 shell 进程）在最前，只做一次接管尝试，失败即在本会话内固定让位（`Yielded()`）；让位后不再反复抢前/广播。另一个 Host 在前时读回模式继续工作，不争抢。
- 点击：`TrayHost::Send(key, action, screenPoint)` 按 `tray_protocol.h` 的 `CallbackSequence`/`EncodeCallback` 发送 shell 协议（v0–v4 的 `WM_LBUTTONDOWN/UP`、`NIN_SELECT`、`WM_CONTEXTMENU`、指针坐标与 uid 编码），发送前 `AllowSetForegroundWindow` 给图标进程；提升权限窗口拒绝时返回失败，交给读取路径回退。

### 3.4 点击分发与证据

UI 线程只把点击排入 worker（FIFO），worker 重新读取并解析目标，不按打开菜单时的序号/标签直接点击：

1. **Host-first**：`hostKey` 有效且 Host 未让位 → 走 shell 回调协议。
2. **read-back**：UIA `Invoke`（左键）/ `IUIAutomationElement3::ShowContextMenu`（右键）；明确失败后经典 toolbar 消息（重新核对类名、shell PID、根窗口、按钮矩形与客户区坐标，`PostClassicGesture()` 发 MOVE/DOWN/UP，双击补 `LDBLCLK`；只证明排队成功）；左键最后可试 `LegacyIAccessible::DoDefaultAction`。
3. **回退**：明确不可用时，左键/双击只恢复该 exe **已经存在且可见或最小化**的窗口并检查是否到前台（不显示隐藏主窗口、不 ShellExecute、不启动 exe）；右键不以打开主窗口代替应用菜单。
4. **物理右键回退**：`ShowContextMenu` 被接受但观察不到菜单时，`tray_pointer` 在用户未输入的前提下做一次真实右键（光标移动 + `SendInput` 合并批、用后移回，用户任何输入立即取消），`tray_taskbar` 的 `TaskbarReveal` 仅为这次点击短暂显示自动隐藏的任务栏，之后恢复。
5. **系统溢出**：仍不可用时打开 Explorer 的溢出区让用户直接操作：Win11 用非本地化结构签名 Invoke chevron（签名不唯一则 fail closed），Win10 用 `TrayNotifyWnd` 下唯一 Button 的 `BM_CLICK`；只有溢出内容确实可见、矩形有效且 `WindowFromPoint` 命中溢出窗口才报告成功。
6. **结果**：向 UI 线程 post `wParam`：`0` 列表变化、`1` 观察到应用响应、`2` 系统托盘已打开、`3` 需要用户、`4` accepted-unconfirmed（安静结束、不重试）。**任何 HRESULT 都不当作应用回执**：只有动作前后观察到前台窗口变化、新窗口/新菜单等有限证据才记 `observed`；`S_OK` 或超时但无证据记 `accepted-unconfirmed`，不再用下一种 API 重复点击（可能把开关类动作反转）。系统溢出打开成功单独记 `system-tray`，不统计为原生点击成功；确实无法自动分发时才 `needs-user` 提示（限频）。

双击语义：单击在鼠标抬起时立即排队；第二击在系统双击时间内且身份一致时，经典托盘只发 `LDBLCLK` 续发（第一击不重放），XAML 没有公开双击 Pattern，因此第二击只再做一次 `Invoke`，不宣称原生等价双击。

### 3.5 线程与刷新

- 所有 UIA、注册表、`Shell_NotifyIconGetRect`、进程/版本查询、图片解码和点击确认都在 MTA worker；UI 线程只 `Refresh()`/`Click()`/`Hover()` 排队，不阻塞渲染。
- 刷新请求合并；Explorer 重启/首次启动触发刷新，失败时做有限次后台重试；跨 shell 实例的点击拒绝自动分发，跨实例刷新直接丢弃。
- 实测/已知耗时见上游 TRAY.md（本机旧读取器 24–44 ms）；本次实现的构建、单元测试与实机验证结果见 §7。

## 4. 文件布局

新托盘子系统（`native/src/atrium/shell/`，namespace `atrium` / `atrium::tray`）。2026-11-21 已全部落地：下表模块与 `native/tests/tray_*_test.cpp`、`tray_probe.cpp` 均已落盘，`tray_host.*` 的会话/所有权修复完成，旧 `native/src/tray.{h,cpp}` 已删除；构建、单元测试与实机验证见 §7：

| 文件 | 职责 |
| --- | --- |
| `tray_model.h` / `tray_model.cpp` | 唯一的 Win32/UIA 适配层与 `TrayModel`：read-back 枚举、注册表读取、GetRect 存活、`tray_match` 对账、host 合并、UIA/经典/物理/溢出点击路由、证据观察、worker 线程与事件。头文件已按 SuperMenu 适配（`recordKey` 供设置覆盖，事件 `wParam` 契约同 §3.4）。 |
| `tray_match.h` / `tray_match.cpp` | 纯函数对账（不含 Windows 头）：`Button`/`Record`/`Program` → `Pair`、`aliases`、`sharedPlace`；名字/几何/顺序/运行程序匹配规则。 |
| `tray_click.h` | 纯点击策略：`Action`/`Route`/`Outcome`、`Dispatch()` 的回退顺序与“接受后不重试”、经典鼠标手势序列 `PostClassicGesture()`、`kProviderTimeout`。 |
| `tray_target.h` | 纯身份校验：`ShellSession`、`TargetKey`、`ResolveTarget()`（runtime/record/program，冲突/壳变更拒绝）、`StableOrder()`、双击手势同目标校验。 |
| `tray_pointer.h` | 物理右键回退：`SendInput` 批量移动+右击、用户输入取消、光标恢复、`g_pointerInFlight`。 |
| `tray_image.h` | 图标素材选择：实时快照优先于程序品牌图。 |
| `tray_taskbar.h` | `ShellTrayWindows()` + `TaskbarReveal`：仅为一次物理点击临时显示自动隐藏的 Explorer 任务栏，之后隐藏自己显示的窗口。 |
| `tray_protocol.h` | Host 回发应用的 shell 回调协议（`CallbackSequence`/`EncodeCallback`，v0–v4），已有文件。 |
| `tray_host.h` / `tray_host.cpp` | 隐藏 `Shell_TrayWnd` 宿主：`WM_COPYDATA` 图标/GetRect、转发给 Explorer、广播与共存（本轮同时修复 §1.3 的广播风暴：会话判定、单次广播、一次性所有权、让位后不抢前）。 |
| `shell_taskbar.h` | Explorer 任务栏查找（跳过本进程与其他 Host）、`AppBarMessage` 转发、shell 响应性探测、`Shell_NotifyIconGetRect` 包装（已有文件，沿用）。 |
| `shell_icon.h/.cpp`、`icon_image.h` | HICON → 预乘 BGRA `IconImage`（含 inked 单色变体），已有文件。 |

退出/迁移：

- `native/src/tray.h` / `native/src/tray.cpp`（旧 `TrayReader`、`ClickTrayIcon`、`MergeHosted` 混合实现）已删除；`native/CMakeLists.txt` 改为编译 `atrium/shell/tray_model.cpp`、`tray_match.cpp`、`tray_host.cpp` 等新模块。
- UI 侧由 `App`/`BarWindow`/`PopupWindow` 改为只持有 `std::unique_ptr<atrium::TrayModel>` 和 `TrayIcon` 缓存；新增 `native/src/tray_view.h` 处理 `trayShow`/`trayHide` 覆盖与标题显示；设置中心继续用数字 `recordKey` 写 `tray-live.txt`。
- 纯函数测试：`native/tests/tray_match_test.cpp`、`tray_click_test.cpp`、`tray_pointer_test.cpp`、`tray_protocol_test.cpp`（由移植任务提供并接入构建/CTest），实时只读探针 `native/tests/tray_probe.cpp`。
- 文档：`docs/TRAY.md` 在 Phase 2 改写为新的双源架构与验证结果；`docs/ARCHITECTURE.md` §4.0/§6 的旧托盘描述同步更新；`THIRD_PARTY_NOTICES.md` 补充新移植模块的 Atrium MIT 归属。

## 5. 已知边界与不确定性

1. **UIA 结构不是公开稳定接口**：`AutomationId=NotifyItemIcon`、`SystemTrayIcon`、`SystemTray.NormalButton` 等均是本机 build 26300 的实测；Windows 版本、语言、任务栏形态或未来更新可能改变层级/标识。识别必须基于非本地化结构并 fail closed，不能按翻译后的名字猜。
2. **隐藏图标只在溢出区打开/内容物化后可点**：本机溢出区 16 个 `NotifyItemIcon` 元素都存在，但隐藏时全部 `IsOffscreen=True`、矩形为空；只有经结构验证的 chevron 打开、内容可见且落点校验通过后才是点击目标。`ShowWindow(溢出 HWND)` 本身不等于内容可用。
3. **自动隐藏任务栏**：`Shell_TrayWnd` 隐藏时本机仍可枚举 promoted 的 Tailscale 按钮（`IsOffscreen=False`、矩形有效），但 `Shell_NotifyIconGetRect` 的坐标在旧实现中被当作不可靠；物理回退需要 `TaskbarReveal` 临时显示任务栏，用户任何输入会取消，且任务栏保持隐藏时 fail closed。
4. **提升权限应用**：UIPI 会拒绝普通权限 Host 发送的回调消息，此时只能回退读取路径；读取路径也只做有限确认（窗口/系统溢出/提示），可能不等价于原应用操作。
5. **XAML 没有原生双击 Pattern**：第二击只是第二次 `Invoke`，与经典托盘 `LDBLCLK` 不等价；上游 Atrium 至今未解决 Steam 等 XAML 托盘的原生双击。
6. **其他托盘宿主**：Atrium、RetroBar、ExplorerPatcher 等可先占用 `Shell_TrayWnd` 流；本设计选择一次性裁决后让位、读回模式继续，因此让位期间拿不到对方 Host 的实时 HICON/callback，图标仍来自 Explorer 记录（可能不完整）。广播风暴的修复必须保持“不争抢、不重复广播”。
7. **注册表/快照是缓存**：`IconSnapshot` 只是 Explorer 上次保存的 PNG，不能代表实时动画、闪烁或角标；`UIOrderList`/`IsPromoted` 也可能滞后于应用当前状态；旧记录不能“复活”。
8. **Win10 经典路径未在本机验证**：本机没有 `ToolbarWindow32`（`TrayNotifyWnd` childless、Win11 XAML），Win10 的经典 toolbar 消息与 `TrayNotifyWnd` Button `BM_CLICK` 只能在 Win10 实机确认；本机没有 `NotifyIconOverflowWindow`。
9. **身份与 RuntimeId**：UIA RuntimeId 允许复用，不能永久缓存为 exe 映射；点击前必须重新解析并交叉核对 exe/tip/recordId，同一 shell 会话变化即拒绝。
10. **未解决的枚举缺口**：既无注册表记录、又无 UIA 元素的图标无法发现（上游验证中 Explorer 未给新测试图标生成记录）；部分 UIA 树/同名记录歧义时可能多显示占位项；系统图标不经过 Host 路径。
11. **证据语义有限**：`observed` 是有限时间窗内的响应证据（前台窗口/新窗口/菜单），不是应用协议回执；`accepted-unconfirmed` 是安静结束状态，不是失败重试的理由；`system-tray`/`needs-user` 都不能统计为原生点击成功。
12. **不承诺完美镜像**：不保证任意 Windows 版本、任意程序、任意任务栏布局都“不多、不漏、不配错”；本次目标是让已知 bug 有确定的证据链与可回退路径。

## 6. 参考链接

上游实现（MIT）：

- Atrium Desktop 源码与 `docs/TRAY.md`：<https://github.com/wynter-yaxley/atrium-desktop>
  - 本地只读副本：`C:\fish\git\atrium-desktop-main\docs\TRAY.md`（设计、证据链、验证记录）
  - 本地只读源码：`C:\fish\git\atrium-desktop-main\native\core\shell\tray_model.cpp`、`tray_match.*`、`tray_click.h`、`tray_target.h`、`tray_pointer.h`、`tray_image.h`、`tray_host.*`、`shell_taskbar.h`
- 本项目已有归属声明：`THIRD_PARTY_NOTICES.md`（Atrium Desktop，MIT，Copyright (c) 2026 wynter-yaxley and Atrium Desktop contributors）；本次新移植模块（`tray_model.*`、`tray_match.*`、`tray_click.h`、`tray_target.h`、`tray_pointer.h`、`tray_image.h`、`tray_taskbar.h`）同样源自该 MIT 代码，Phase 2 补充文件清单。

社区实现对照（上游固定版本阅读，按机制独立实现，不复制源码）：

- [ManagedShell ExplorerTrayService](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/ExplorerTrayService.cs)（跨进程 toolbar 读取）
- [ManagedShell TrayService](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/TrayService.cs)（Host 架构来源）
- [ManagedShell NotifyIcon](https://github.com/cairoshell/ManagedShell/blob/855222035e927910ca53fd3215caef056337a048/src/ManagedShell.WindowsTray/NotifyIcon.cs)、[RetroBar NotifyIcon 控件](https://github.com/dremin/RetroBar/blob/d2b0b8535a395f3f13578d2825ca2094845c1779/RetroBar/Controls/NotifyIcon.xaml.cs)
- [Seelen-UI tray_hook_loader.rs](https://github.com/eythaann/Seelen-UI/blob/1ed1a73eb3f3a6d2a541e9ceb0f280a7faaa98d5/src/background/system_modules/system_tray/application/windows/tray_hook_loader.rs)、[tray_icon.rs](https://github.com/eythaann/Seelen-UI/blob/1ed1a73eb3f3a6d2a541e9ceb0f280a7faaa98d5/src/background/system_modules/system_tray/application/windows/tray_icon.rs)（DLL 注入，不采用）
- [YASB trayhook.cpp](https://github.com/amnweb/yasb/blob/37928dfeacc579948c4b4bf12e169bcbf6006ff2/src/core/widgets/services/systray/hook/trayhook.cpp)（`SetWindowsHookEx`，不采用）
- [Chromium status_tray_state_changer_win.cc](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/chrome/browser/ui/views/status_icons/status_tray_state_changer_win.cc)（`ITrayNotify` 探测，不接入）

Microsoft Learn：

- [NOTIFYICONDATA](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/ns-shellapi-notifyicondataw)（含 `NIN_SELECT`/`NIN_POPUPOPEN` 与版本化回调规则）
- [Shell_NotifyIcon](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyiconw)（Host 接收的注册/修改/删除入口）
- [Shell_NotifyIconGetRect](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-shell_notifyicongetrect)（存活探测）
- [BM_CLICK](https://learn.microsoft.com/en-us/windows/win32/controls/bm-click)（Win10 经典溢出箭头）
- [IUIAutomationInvokePattern::Invoke](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationinvokepattern-invoke)
- [IUIAutomationElement3::ShowContextMenu](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationelement3-showcontextmenu)
- [IUIAutomationLegacyIAccessiblePattern::DoDefaultAction](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationlegacyiaccessiblepattern-dodefaultaction)
- [UIA error codes](https://learn.microsoft.com/en-us/windows/win32/winauto/uiauto-error-codes)（`UIA_E_TIMEOUT` 等）
- [IUIAutomationElement::GetRuntimeId](https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationelement-getruntimeid)（RuntimeId 可复用）
- [AllowSetForegroundWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-allowsetforegroundwindow)（Host 点击前的前台授权）

## 7. 验证结果（实现后，2026-11-21）

本节由 Phase 2 补充：移植代码已构建、通过单元测试并完成实机验证（Windows 11 build 26300）。

- **构建与单元测试**：native Release 构建通过；`ctest --test-dir native/build` 4/4 通过，共 **137** 项检查（`tray_match` 22、`tray_click` 60、`tray_protocol` 15、`tray_pointer` 40）。
- **Host 模式实机（只运行 SuperMenu）**：整场恰好 **1** 次 `TaskbarCreated` 广播、**0** 次让位、**0** 次“in front again”、**0** 次 Explorer 重启（对应 §1.3 旧日志的 937 / 928 / 925）。read-back 对账 37 条记录 → **15** 个存活图标、**0** 个未知占位；顶栏显示 1 个 promoted（Tailscale）+ chevron，隐藏面板 13 个 = 15 − 1 − 本程序图标。promoted 左键打开了 Tailscale 的真实 Win32 菜单；promoted 右键、隐藏图标（华为云盘、SZU 网络守护）左/右键与双击续发均记录原生回调与 `-> sent`。
- **read-back 回退（探针，无 Host）**：对 Tailscale 使用 UIA `ShowContextMenu` 右键，日志 `uia-context-menu ... observed=1`，最终结果 `observed`。
- **与 Atrium 共存（旧风暴配置）**：启动 **1** 次广播；日志 1 次“another program's tray took the stream: one attempt to take it back”，接管成功；最终 `Shell_TrayWnd` z-order 为 SuperMenu → Atrium → Explorer；**0** 让位 / **0** 抢回 / **0** 重启 / **0** 额外广播；Atrium 同窗口日志仅 1 次“left there”、0 次“in front again”。共存下点击仍是原生回调。read-back 16 个图标（含 Atrium 自己的图标），面板 14 隐藏 + 1 promoted；两个无法读取进程路径的提升权限应用（华为电脑管家 / 华为控制中心）按 GUID/HWND 身份合并，没有重复显示。
- **验证范围**：以上均为本机 Windows 11 build 26300；Win10 经典 toolbar 路径与更多第三方程序仍属 §5 的未覆盖范围。验证后已重启本机安装版 SuperMenu 与 Atrium，登录计划任务指回 `C:\fish\SuperMenu\SuperMenuBar.exe`。
