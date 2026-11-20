# 构建指南

本项目由两部分组成：

| 部分 | 语言 / 框架 | 工具链 |
|---|---|---|
| `native/` 菜单栏 | C++20、Win32、D3D11、D2D1、DirectWrite、DirectComposition | MSYS2 UCRT64 GCC + CMake + Ninja |
| `settings/` 设置中心 | C# / .NET 10 WPF | `dotnet` SDK |
| `media/` 媒体元数据 helper | C# / .NET 10 + WinRT | `dotnet` SDK |

不需要 Visual Studio、Windows SDK 或 `fxc.exe`；HLSL 在运行时使用 Windows 自带的
`d3dcompiler_47.dll` 编译。

## 1. 安装工具链

### MSYS2 UCRT64

```powershell
winget install --id MSYS2.MSYS2 -e --accept-source-agreements --accept-package-agreements
C:\msys64\usr\bin\pacman.exe -S --noconfirm --needed `
  mingw-w64-ucrt-x86_64-gcc `
  mingw-w64-ucrt-x86_64-cmake `
  mingw-w64-ucrt-x86_64-ninja
```

验证：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
g++ --version          # 16.2.0 或更新
cmake --version        # >= 3.25
ninja --version
```

> 如果没有把 `C:\msys64\ucrt64\bin` 放进 PATH，`g++` 会静默以 1 退出（找不到 `cc1plus.exe`）。

### .NET 10 SDK

```powershell
dotnet --list-sdks
# 应包含 10.0.x
```

WPF 需要 `Microsoft.WindowsDesktop.App` 运行时 10；SDK 安装程序默认会带上。

## 2. 一键构建 / 发布

仓库根目录：

```powershell
.\build.ps1 -Version 1.0.0
```

发布脚本会：

1. 检查 MSYS2 UCRT64、CMake、Ninja、.NET SDK。
2. 把 `C:\msys64\ucrt64\bin` 加入当前进程的 PATH（若存在）。
3. 用 `native\cmake\mingw-ucrt64.cmake` + Ninja 以 Release 配置并编译 `native/`；
   静态链接 MinGW 运行时，产物不依赖 `libstdc++-6.dll` / `libwinpthread-1.dll`。
4. 运行 `dotnet publish` 发布 `settings/SuperMenu.Settings`（框架依赖、win-x64）。
5. 发布 `media/SuperMenu.Media` 到 `dist/Media`（WinRT 正在播放元数据 helper）。
6. 组装 `dist/`：`SuperMenuBar.exe`、`Fonts/`、`Settings/`、`Media/`、文档和第三方说明。
7. 运行设置中心 `--selftest`（要求全部通过）。
8. 生成 `release/SuperMenu-<version>-win-x64/`、
   `release/SuperMenu-<version>-win-x64.zip` 和 `.sha256`。

参数：

```powershell
.\build.ps1 -Version 1.0.1   # 指定版本
.\build.ps1 -Clean           # 清理 native/build、dist、release 后重建
.\build.ps1 -SkipZip         # 只构建，不打包
.\build.ps1 -SkipBuild       # 用当前 dist 重新验证并打包
```

底层构建脚本仍可单独使用：

```powershell
.\scripts\build.ps1 -Zip
.\scripts\build.ps1 -SkipSettings   # 只构建原生菜单栏
.\scripts\build.ps1 -KeepRunning    # 不自动结束正在运行的菜单栏
```

## 3. 手动构建

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cmake -S native -B native/build -G Ninja `
  -DCMAKE_TOOLCHAIN_FILE=native/cmake/mingw-ucrt64.cmake `
  -DCMAKE_BUILD_TYPE=Release
cmake --build native/build

dotnet publish settings/SuperMenu.Settings/SuperMenu.Settings.csproj `
  -c Release -r win-x64 --self-contained false -o dist/Settings
dotnet publish media/SuperMenu.Media/SuperMenu.Media.csproj `
  -c Release -r win-x64 --self-contained false -o dist/Media

Copy-Item native/build/SuperMenuBar.exe dist/
```

原生程序的原生产物目录为 `native/build/dist/`；CMake 的 POST_BUILD 会把字体复制到
`native/build/dist/Fonts`。`dist/` 的最终组装仍由构建脚本负责。图标字体
`assets/fonts/SegoeIcons.ttf` 从本机 Windows 11 的 `C:\Windows\Fonts\SegoeIcons.ttf`
提取后随仓库保存；原生栏和设置中心都使用随包副本，构建时不需要系统字体。若该文件
缺失，原生栏会回退到系统 `Segoe Fluent Icons` / `Segoe MDL2 Assets`。

## 4. 运行

```powershell
.\dist\SuperMenuBar.exe
```

- 设置文件：`%LOCALAPPDATA%\SuperMenu\settings.json`
- 日志：`%LOCALAPPDATA%\SuperMenu\logs\supermenu.log`
- 第一次运行会创建设置文件；右键顶栏打开设置中心。
- 退出：Apple 菜单 → 退出 SuperMenu；退出时会移除 AppBar 预约，工作区自动恢复。

## 5. 故障排查

| 现象 | 处理 |
|---|---|
| `g++: error: CreateProcess: No such file or directory` / 静默退出 1 | `C:\msys64\ucrt64\bin` 不在 PATH |
| CMake 找不到 `windres` | 安装 `mingw-w64-ucrt-x86_64-binutils`，检查工具链文件路径 |
| 运行时提示缺少 `libstdc++-6.dll` | 没有用 `-static` 链接；确认 CMake 的 `target_link_options(... -static)` 生效 |
| 设置中心打不开 | 需要 .NET 10 Desktop Runtime；运行 `dotnet --list-runtimes` 检查 `Microsoft.WindowsDesktop.App 10.x` |
| 玻璃是纯色、日志里出现 `DuplicateOutput` 失败 | Desktop Duplication 不可用（被录屏软件占用、HDR/旋转屏、安全桌面）；程序会回退到壁纸背景，不影响顶栏操作 |
| 老 Windows 10 上玻璃显示空白 | 需要 Windows 10 19041+；`d3dcompiler_47.dll` 缺失时没有运行时着色器编译 |
| 退出后工作区仍留着顶端 42 px | 强制结束进程时 Windows 可能不会立刻回收 AppBar；重开再正常退出，或注销/重启 Explorer |
| 多显示器 / DPI 切换后位置不对 | v1 设计为单栏，切换显示器或缩放后重启菜单栏（设置里有提示） |

## 6. 构建环境记录

本机实际验证环境（2026-11）：

- Windows 11 build 26300，2800×1840 @ 150%
- MSYS2 UCRT64：GCC 16.2.0、CMake 4.5.0、Ninja 1.13.2、binutils 2.47
- .NET SDK 10.0.404，WindowsDesktop.App 10.0.15
- NVIDIA GeForce RTX 3060 Laptop GPU
- 生成 `SuperMenuBar.exe` 约 4.1 MB，仅依赖系统 DLL；`dist` 可整体复制运行。
- `Media/` 使用 .NET 10 的 Windows SDK WinRT 投影，体积主要来自
  `Microsoft.Windows.SDK.NET.dll`；它只读取 SMTC 元数据并写轻量快照，不在原生进程内加载 WinRT。
