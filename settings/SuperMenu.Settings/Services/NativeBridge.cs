using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;

namespace SuperMenu.Settings.Services;

/// <summary>
/// The only native-bar integration: locate the sibling bar executable, tell a running bar to
/// reload (registered window message), and optionally restart it. No other IPC is invented.
/// </summary>
public static class NativeBridge
{
    public const string SettingsChangedMessage = "SuperMenu.SettingsChanged";
    public const string NativeExecutableName = "SuperMenuBar.exe";

    private const int HwndBroadcast = 0xFFFF;
    private const int MaxParentLevels = 6;

    private static readonly string[] RelativeNativePaths =
    [
        NativeExecutableName,
        @"dist\" + NativeExecutableName,
        @"build\Release\" + NativeExecutableName,
        @"build\" + NativeExecutableName,
        @"native\build\dist\" + NativeExecutableName,
        @"native\build\Release\" + NativeExecutableName,
        @"native\build\" + NativeExecutableName,
        @"out\build\x64-Release\" + NativeExecutableName,
    ];

    [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern uint RegisterWindowMessageW(string message);

    [DllImport("user32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool PostMessageW(IntPtr hWnd, uint message, IntPtr wParam, IntPtr lParam);

    /// <summary>Broadcasts "SuperMenu.SettingsChanged" to every top-level window (HWND_BROADCAST).</summary>
    public static bool BroadcastSettingsChanged()
    {
        try
        {
            var message = RegisterWindowMessageW(SettingsChangedMessage);
            if (message == 0) return false;
            return PostMessageW(new IntPtr(HwndBroadcast), message, IntPtr.Zero, IntPtr.Zero);
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            return false;
        }
    }

    /// <summary>
    /// Finds SuperMenuBar.exe without assuming an exact install layout:
    /// next to this exe, one or more parents up, or in a dist/build folder of a parent.
    /// Returns null when the bar has not been built/installed yet; callers never fail because of it.
    /// </summary>
    public static string? FindNativeBar()
    {
        try
        {
            var start = new DirectoryInfo(AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
            for (var level = 0; level <= MaxParentLevels && start is not null; level++)
            {
                foreach (var relative in RelativeNativePaths)
                {
                    var candidate = Path.GetFullPath(Path.Combine(start.FullName, relative));
                    if (File.Exists(candidate)) return candidate;
                }

                start = start.Parent;
            }

            // Last resort: settings app launched from the repository root via `dotnet run --project settings/...`.
            var fromCurrentDirectory = Path.GetFullPath(Path.Combine(Environment.CurrentDirectory, NativeExecutableName));
            if (File.Exists(fromCurrentDirectory)) return fromCurrentDirectory;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            // Missing/unreadable locations simply mean "no restart button".
        }

        return null;
    }

    /// <summary>True when a SuperMenuBar process is running (drives the sidebar status dot).</summary>
    public static bool IsNativeBarRunning()
    {
        try
        {
            return Process.GetProcessesByName(Path.GetFileNameWithoutExtension(NativeExecutableName)).Length > 0;
        }
        catch
        {
            return false;
        }
    }

    /// <summary>Kills any running bar and starts the located executable. Returns an error message on failure.</summary>
    public static string? RestartNativeBar()
    {
        var executable = FindNativeBar();
        if (executable is null)
            return "未找到菜单栏程序（SuperMenuBar.exe）";

        try
        {
            foreach (var process in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(NativeExecutableName)))
            {
                try
                {
                    process.Kill(entireProcessTree: true);
                    process.WaitForExit(3000);
                }
                catch
                {
                    // If a process is already exiting, starting a fresh one is still correct.
                }
                finally
                {
                    process.Dispose();
                }
            }

            Process.Start(new ProcessStartInfo
            {
                FileName = executable,
                WorkingDirectory = Path.GetDirectoryName(executable) ?? Environment.CurrentDirectory,
                UseShellExecute = true,
            });
            return null;
        }
        catch (Exception ex)
        {
            return $"重启菜单栏失败：{ex.Message}";
        }
    }

    /// <summary>Opens Explorer at the settings file (selecting it when it exists).</summary>
    public static void RevealSettingsFile(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                Process.Start(new ProcessStartInfo("explorer.exe", $"/select,\"{path}\"")
                {
                    UseShellExecute = true,
                });
                return;
            }

            var directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{directory ?? "."}\"")
            {
                UseShellExecute = true,
            });
        }
        catch
        {
            // Opening Explorer is a convenience; a failure must not crash the settings window.
        }
    }
}
