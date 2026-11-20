# 托盘图标
## 架构

- 独立线程创建隐藏的 `Shell_TrayWnd`（`TrayHost`，`native/src/atrium/shell/tray_host.*`），
  保持在 Explorer 同名窗口之前。
- 应用的 `Shell_NotifyIcon` 先到 SuperMenu：
  - 立即把 `HICON` 复制成预乘 BGRA；
  - 记录 HWND/UID/GUID、回调消息、版本、NIF_SHOWTIP、NIS_HIDDEN、提示文本；
  - 原样转发给 Explorer，Explorer 的返回值原样返回给应用。
- Explorer 的 TaskbarCreated 重启事件：主机重新置前并再次广播，补收集窗口空档期的图标。
- 另一个托盘宿主（Atrium、RetroBar 等）在前时不争抢；本机轮询到它时 `Yielded()` 为真，
  SuperMenu 不使用可能不完整的 host 列表，回到 Explorer 读取路径。
- 点击直接发送 shell 协议回调（`tray_protocol.h`）：
  - 左键：MOVE/LDOWN/LUP，v3+ 追加 NIN_SELECT；
  - 右键：MOVE/RDOWN/RUP，v3+ 追加 WM_CONTEXTMENU；
  - 使用图标的 callback message、uid、NOTIFYICON_VERSION 编码；
  - v4 的 wParam 为指针屏幕坐标，lParam 低字通知、高字 uid；
  - 发送前 `AllowSetForegroundWindow` 给图标进程。
  - 因此**可见和隐藏图标都能直接交互**，不再需要先打开系统溢出区、也不再依赖
    `Shell_NotifyIconGetRect` 的屏幕矩形。

## 与 SuperMenu 的合并

`TrayReader` 仍是设置/排序元数据的来源：

1. 读取 `NotifyIconSettings` 得到显示顺序、IsPromoted 和用户在设置中心的
   `trayShow`/`trayHide` 覆盖。
2. 读取 Explorer 的 `Shell_NotifyIconGetRect` 做 Explorer-only 回退存活校验。
3. `MergeHosted()` 把 host 图标按 GUID 或 `exe + uid` 配对到登记记录：
   - 配对的图标用 host 的实时 HICON 覆盖旧快照/屏幕抓图；
   - 没有登记记录的图标作为 host-only 条目补入，顺序在已知记录之后；
   - 过滤 SuperMenu 自身托盘图标；
   - host 被其他托盘程序挡住时跳过整个 host 合并。
4. 点击优先用 `TrayHost::Send(hostKey, ...)`；只有 host 不可用、图标没有 hostKey
   或发送被 UIPI 拒绝时，才退回旧的矩形/溢出区路径。

设置中心仍用数字登记 ID 的 `tray-live.txt`；host-only 图标不会写入该文件，
因此不会破坏设置中心的 ID 编辑。

## 安全边界

- 主机窗口从不显示、从不激活，跟随 Explorer 任务栏的大小和位置；退出或崩溃后
  Explorer 自动重新成为第一个 `Shell_TrayWnd`，应用无需恢复。
- `WM_COPYDATA` 记录失败时仍然转发给 Explorer；主机线程与 UI 线程分离，避免 UI 卡顿
  拖住应用的托盘调用。
- 提升权限进程的窗口会拒绝普通权限主机发送的回调（UIPI）；此时退回 Explorer 读取路径。
- 系统（Explorer 自己的）图标不进入主机列表。
- 与 Atrium/其他托盘宿主共存时让位，不争抢。
- 双击、XAML 双击、自绘动画位图的完整行为仍受 Windows 不公开协议限制；本轮优先保证
  实时图标 + 真实左/右键。

## 验证

- 启动后日志显示 `tray host: on`、`tray host: N icons`，host 列表包含 Tailscale等真实 HICON 与 tooltip。
- 点击 host-only 图标同样走 `tray host: left click`。
- 上述与 Explorer 同时运行；Explorer 任务栏保持完整。

