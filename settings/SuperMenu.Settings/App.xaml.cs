using System.IO;
using System.Windows;
using System.Windows.Threading;
using SuperMenu.Settings.Services;

namespace SuperMenu.Settings;

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        AppAssets.Initialize();

        var arguments = e.Args;
        var selfTestIndex = Array.FindIndex(arguments, argument => argument.Equals("--selftest", StringComparison.OrdinalIgnoreCase));
        if (selfTestIndex >= 0)
        {
            var resultPath = selfTestIndex + 1 < arguments.Length
                ? Path.GetFullPath(arguments[selfTestIndex + 1])
                : Path.Combine(Path.GetTempPath(), "supermenu-settings-selftest.json");
            var exitCode = SelfTest.Run(resultPath);
            Shutdown(exitCode);
            return;
        }

        DispatcherUnhandledException += OnDispatcherUnhandledException;

        var store = new SettingsStore();
        var load = store.Load();

        // The contract says a missing file is initialised with defaults by either side; do it here
        // so the native bar and this window agree from the first run.
        if (!load.FileExisted)
        {
            var created = store.Save(load.Settings, load.UnknownKeys);
            load = new SettingsLoadResult(load.Settings, load.UnknownKeys, created.Success, load.Error);
        }

        var requestedPage = GetArgumentValue(arguments, "--page");
        var window = new MainWindow(store, load, requestedPage);
        MainWindow = window;
        window.Show();
    }

    private void OnDispatcherUnhandledException(object sender, DispatcherUnhandledExceptionEventArgs e)
    {
        TryLog(e.Exception);
        MessageBox.Show(
            $"SuperMenu 设置遇到一个问题，已记录日志：\n{e.Exception.Message}",
            "SuperMenu 设置",
            MessageBoxButton.OK,
            MessageBoxImage.Warning);
        e.Handled = true;
    }

    private static void TryLog(Exception exception)
    {
        try
        {
            var directory = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SuperMenu");
            Directory.CreateDirectory(directory);
            File.AppendAllText(
                Path.Combine(directory, "settings-app.log"),
                $"[{DateTimeOffset.Now:O}] {exception}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never throw.
        }
    }

    private static string? GetArgumentValue(string[] arguments, string name)
    {
        var index = Array.FindIndex(arguments, argument => argument.Equals(name, StringComparison.OrdinalIgnoreCase));
        return index >= 0 && index + 1 < arguments.Length ? arguments[index + 1] : null;
    }
}
