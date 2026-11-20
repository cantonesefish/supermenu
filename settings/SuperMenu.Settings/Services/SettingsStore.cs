using System.IO;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;
using SuperMenu.Settings.Models;

namespace SuperMenu.Settings.Services;

/// <summary>Result of a tolerant settings load.</summary>
public sealed record SettingsLoadResult(AppSettings Settings, JsonObject? UnknownKeys, bool FileExisted, string? Error);

/// <summary>Result of an atomic settings save.</summary>
public sealed record SettingsSaveResult(bool Success, string Path, string? Error, DateTimeOffset? SavedAt);

/// <summary>
/// Reads and writes the flat JSON file shared with the native bar.
/// Saves are atomic (write sibling temp file, flush, then rename over the target) so the
/// native side never observes a half-written document.
/// </summary>
public sealed class SettingsStore
{
    /// <summary>Test-only override so round-trip checks never touch the user's real settings.</summary>
    public const string PathOverrideVariable = "SUPERMENU_SETTINGS_PATH";

    public static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    private static readonly UTF8Encoding Utf8NoBom = new(encoderShouldEmitUTF8Identifier: false);

    public SettingsStore(string? path = null)
    {
        Path = path ?? ResolveDefaultPath();
    }

    /// <summary>Full path of settings.json currently in use.</summary>
    public string Path { get; }

    public static string ResolveDefaultPath()
    {
        var overridden = Environment.GetEnvironmentVariable(PathOverrideVariable);
        if (!string.IsNullOrWhiteSpace(overridden))
            return System.IO.Path.GetFullPath(overridden);

        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        return System.IO.Path.Combine(localAppData, "SuperMenu", "settings.json");
    }

    public SettingsLoadResult Load()
    {
        try
        {
            if (!File.Exists(Path))
                return new SettingsLoadResult(AppSettings.CreateDefault(), null, false, null);

            var text = File.ReadAllText(Path, Utf8NoBom);
            if (string.IsNullOrWhiteSpace(text))
                return new SettingsLoadResult(AppSettings.CreateDefault(), null, true, "配置文件为空，已加载默认值");

            JsonNode? parsed;
            try
            {
                parsed = JsonNode.Parse(text);
            }
            catch (JsonException)
            {
                return new SettingsLoadResult(AppSettings.CreateDefault(), null, true, "配置文件格式有误，已加载默认值");
            }

            if (parsed is not JsonObject root)
                return new SettingsLoadResult(AppSettings.CreateDefault(), null, true, "配置文件结构不受支持，已加载默认值");

            var settings = AppSettings.FromJson(root);
            var unknown = new JsonObject();
            foreach (var pair in root)
            {
                if (!AppSettings.KnownKeys.Contains(pair.Key))
                    unknown[pair.Key] = pair.Value?.DeepClone();
            }

            return new SettingsLoadResult(settings, unknown, true, null);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            return new SettingsLoadResult(AppSettings.CreateDefault(), null, File.Exists(Path), "读取配置失败，已加载默认值");
        }
    }

    /// <summary>Atomically persists the settings and returns a snapshot of what was written.</summary>
    public SettingsSaveResult Save(AppSettings settings, JsonObject? unknownKeys)
    {
        var directory = System.IO.Path.GetDirectoryName(Path);
        var tempPath = Path + "." + Guid.NewGuid().ToString("N")[..8] + ".tmp";
        try
        {
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);

            var json = settings.ToJson(unknownKeys);
            var bytes = Utf8NoBom.GetBytes(json.ToJsonString(JsonOptions));

            using (var stream = new FileStream(tempPath, FileMode.Create, FileAccess.Write, FileShare.None, 4096, FileOptions.WriteThrough))
            {
                stream.Write(bytes, 0, bytes.Length);
                stream.Flush(flushToDisk: true);
            }

            if (File.Exists(Path))
            {
                // Replace is atomic and keeps the target's ACLs; fall back to Move on filesystems
                // that do not support it (e.g. some network shares), retrying briefly if the native
                // bar happens to be reading the file at this instant.
                for (var attempt = 1; ; attempt++)
                {
                    try
                    {
                        File.Replace(tempPath, Path, destinationBackupFileName: null, ignoreMetadataErrors: true);
                        break;
                    }
                    catch (IOException) when (attempt < 3)
                    {
                        Thread.Sleep(60 * attempt);
                    }
                    catch (IOException)
                    {
                        File.Move(tempPath, Path, overwrite: true);
                        break;
                    }
                }
            }
            else
            {
                File.Move(tempPath, Path, overwrite: true);
            }

            NativeBridge.BroadcastSettingsChanged();
            return new SettingsSaveResult(true, Path, null, DateTimeOffset.Now);
        }
        catch (Exception ex)
        {
            return new SettingsSaveResult(false, Path, ex.Message, null);
        }
        finally
        {
            try
            {
                if (File.Exists(tempPath)) File.Delete(tempPath);
            }
            catch
            {
                // A leftover temp file is harmless; never mask the real save outcome.
            }
        }
    }
}
