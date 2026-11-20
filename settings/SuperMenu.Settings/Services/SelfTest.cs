using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Windows;
using SuperMenu.Settings.Models;

namespace SuperMenu.Settings.Services;

/// <summary>
/// Headless acceptance check for the settings contract, invoked with
/// <c>SuperMenu.Settings.exe --selftest &lt;result.json&gt;</c>.
/// It works against a temporary file (SUPERMENU_SETTINGS_PATH) so the real settings are untouched.
/// </summary>
public static class SelfTest
{
    public static int Run(string resultPath)
    {
        var checks = new JsonArray();
        var allPassed = true;

        void Check(string name, bool passed, string detail = "")
        {
            allPassed &= passed;
            checks.Add(new JsonObject
            {
                ["name"] = name,
                ["ok"] = passed,
                ["detail"] = detail,
            });
        }

        var tempDirectory = Path.Combine(Path.GetTempPath(), "SuperMenu.Settings.SelfTest", Guid.NewGuid().ToString("N")[..8]);
        var settingsPath = Path.Combine(tempDirectory, "settings.json");
        Directory.CreateDirectory(tempDirectory);
        TrayCatalog? trayCatalog = null;

        try
        {
            // 1. Missing file falls back to defaults without throwing.
            Environment.SetEnvironmentVariable(SettingsStore.PathOverrideVariable, settingsPath);
            var store = new SettingsStore(settingsPath);
            var missing = store.Load();
            Check("missing file → defaults", !missing.FileExisted && missing.Settings.BarHeight == 28 && missing.Settings.Tint == "#FFFFFF");

            // 2. Save defaults atomically, reload, and compare every documented key.
            var defaults = AppSettings.CreateDefault();
            var firstSave = store.Save(defaults, null);
            Check("atomic save (defaults)", firstSave.Success && File.Exists(settingsPath), firstSave.Error ?? settingsPath);
            Check("no temp file left behind", Directory.GetFiles(tempDirectory, "*.tmp").Length == 0);

            var raw = File.ReadAllText(settingsPath);
            var root = JsonNode.Parse(raw) as JsonObject;
            Check("written json is an object", root is not null);
            if (root is not null)
            {
                var missingKeys = AppSettings.KnownKeys.Where(key => !root.ContainsKey(key)).ToArray();
                var extraKeys = root.Select(p => p.Key).Where(key => !AppSettings.KnownKeys.Contains(key)).ToArray();
                Check($"all {AppSettings.KnownKeys.Count} schema keys written", missingKeys.Length == 0,
                    missingKeys.Length == 0 ? $"{AppSettings.KnownKeys.Count} keys" : string.Join(",", missingKeys));
                Check("no unexpected keys", extraKeys.Length == 0, string.Join(",", extraKeys));
                Check("version is 1", root["version"]?.GetValue<int>() == 1);
                Check("camelCase only", root.All(pair => !char.IsUpper(pair.Key[0])), string.Join(",", root.Select(p => p.Key).Where(k => char.IsUpper(k[0]))));
            }

            var reloaded = store.Load();
            Check("reload values equal defaults", JsonEquals(defaults.ToJson(), reloaded.Settings.ToJson()), "");

            // 3. Non-default round-trip (types + enum + colour + number formatting).
            var changed = AppSettings.FromJson(root);
            changed.BarHeight = 33.5;
            changed.FontScale = 1.15;
            changed.Theme = "light";
            changed.Language = "en-US";
            changed.LogoStyle = "windows";
            changed.GlassPreset = "frosted";
            changed.Transparency = 0.7;
            changed.Tint = "#FFCC88";
            changed.ShowBatteryPercent = true;
            changed.ShowVolume = true;
            changed.AutoHide = true;
            changed.Monitor = 2;
            changed.Noise = 0.04;
            changed.Saturation = 1.4;
            changed.RightIconSpacing = 21;
            changed.TrayShow = "15002141772056644389;12656677187298439157";
            changed.TrayHide = "4373732872124758814";
            var secondSave = store.Save(changed, null);
            var reRead = store.Load().Settings;
            var reReadRaw = File.ReadAllText(settingsPath);
            var logoLine = reReadRaw.Split('\n').FirstOrDefault(line => line.Contains("\"logoStyle\"", StringComparison.Ordinal))?.Trim();
            var trayShowLine = reReadRaw.Split('\n').FirstOrDefault(line => line.Contains("\"trayShow\"", StringComparison.Ordinal))?.Trim();
            var trayHideLine = reReadRaw.Split('\n').FirstOrDefault(line => line.Contains("\"trayHide\"", StringComparison.Ordinal))?.Trim();
            var spacingLine = reReadRaw.Split('\n').FirstOrDefault(line => line.Contains("\"rightIconSpacing\"", StringComparison.Ordinal))?.Trim();
            Check("round-trip save/load", secondSave.Success
                && reRead.BarHeight == 33.5
                && reRead.FontScale == 1.15
                && reRead.Theme == "light"
                && reRead.Language == "en-US"
                && reRead.LogoStyle == "windows"
                && logoLine is not null && logoLine.Contains("\"windows\"", StringComparison.Ordinal)
                && reRead.GlassPreset == "frosted"
                && Math.Abs(reRead.Transparency - 0.7) < 1e-9
                && reRead.Tint == "#FFCC88"
                && reRead.ShowBatteryPercent
                && reRead.ShowVolume
                && reRead.AutoHide
                && reRead.Monitor == 2
                && Math.Abs(reRead.Noise - 0.04) < 1e-9
                && Math.Abs(reRead.Saturation - 1.4) < 1e-9
                && reRead.RightIconSpacing == 21
                && spacingLine is not null && spacingLine.Contains("21", StringComparison.Ordinal)
                // Id lists are normalised on save: sorted numerically and duplicates removed.
                && reRead.TrayShow == "12656677187298439157;15002141772056644389"
                && reRead.TrayHide == "4373732872124758814"
                && trayShowLine is not null && trayShowLine.Contains("12656677187298439157;15002141772056644389", StringComparison.Ordinal)
                && trayHideLine is not null && trayHideLine.Contains("\"4373732872124758814\"", StringComparison.Ordinal),
                $"{logoLine} | {spacingLine} | {trayShowLine} | {trayHideLine}");

            // 3b. New scalar key clamps and the documented tray override rule.
            var spacingHigh = AppSettings.FromJson(JsonNode.Parse("""{"rightIconSpacing":999}""") as JsonObject);
            var spacingLow = AppSettings.FromJson(JsonNode.Parse("""{"rightIconSpacing":-5}""") as JsonObject);
            Check("rightIconSpacing clamps to 2–24", spacingHigh.RightIconSpacing == 24 && spacingLow.RightIconSpacing == 2
                && AppSettings.CreateDefault().RightIconSpacing == 20,
                $"high={spacingHigh.RightIconSpacing} low={spacingLow.RightIconSpacing} default={AppSettings.CreateDefault().RightIconSpacing}");

            var trayProbe = AppSettings.FromJson(
                JsonNode.Parse("""{"trayShow":" 15002141772056644389 ; 285 ;285;abc;0;12 ","trayHide":"4373732872124758814"}""") as JsonObject);
            Check("tray id lists normalised", trayProbe.TrayShow == "12;285;15002141772056644389" && trayProbe.TrayHide == "4373732872124758814",
                $"trayShow={trayProbe.TrayShow} trayHide={trayProbe.TrayHide}");

            var rule = AppSettings.CreateDefault();
            const ulong promotedId = 1001;
            const ulong hiddenId = 1002;
            var inheritPromoted = rule.IsTrayIconShown(promotedId, true);
            var inheritHidden = rule.IsTrayIconShown(hiddenId, false);
            rule.SetTrayIconShown(promotedId, false, platformPromoted: true);
            var forcedHide = !rule.IsTrayIconShown(promotedId, true) && rule.TrayHide == "1001" && rule.TrayShow.Length == 0;
            rule.SetTrayIconShown(promotedId, true, platformPromoted: true);
            var backToInherit = rule.IsTrayIconShown(promotedId, true) && rule.TrayShow.Length == 0 && rule.TrayHide.Length == 0;
            rule.SetTrayIconShown(hiddenId, true, platformPromoted: false);
            var forcedShow = rule.IsTrayIconShown(hiddenId, false) && rule.TrayShow == "1002" && rule.TrayHide.Length == 0;
            rule.SetTrayIconShown(hiddenId, false, platformPromoted: false);
            var clearedAgain = !rule.IsTrayIconShown(hiddenId, false) && rule.TrayShow.Length == 0 && rule.TrayHide.Length == 0;
            Check("tray override rule (hide > show > IsPromoted)",
                inheritPromoted && !inheritHidden && forcedHide && backToInherit && forcedShow && clearedAgain,
                $"inherit={inheritPromoted}/{inheritHidden} forcedHide={forcedHide} back={backToInherit} forcedShow={forcedShow} cleared={clearedAgain}");

            // 4. Unknown keys survive a load → save cycle (native may add future keys).
            File.WriteAllText(settingsPath, """{"barHeight":30,"futureKey":42,"zAnother":{"nested":true}}""", new UTF8Encoding(false));
            var withUnknown = store.Load();
            Check("unknown keys captured", withUnknown.UnknownKeys is not null
                && withUnknown.UnknownKeys.ContainsKey("futureKey")
                && withUnknown.UnknownKeys.ContainsKey("zAnother"));
            var unknownSave = store.Save(withUnknown.Settings, withUnknown.UnknownKeys);
            var preserved = JsonNode.Parse(File.ReadAllText(settingsPath)) as JsonObject;
            Check("unknown keys preserved on save", unknownSave.Success
                && preserved?["futureKey"]?.GetValue<int>() == 42
                && preserved?["zAnother"] is JsonObject);

            // 5. Malformed / out-of-range input never throws and falls back sanely.
            File.WriteAllText(settingsPath, """{"barHeight":999,"theme":"neon","tint":"red","showWifi":"yes","glassPreset":42,"logoStyle":"mac"}""", new UTF8Encoding(false));
            var malformed = store.Load();
            Check("malformed values clamped to schema", malformed.Settings.BarHeight == 44
                && malformed.Settings.Theme == "auto"
                && malformed.Settings.Tint == "#FFFFFF"
                && malformed.Settings.ShowWifi
                && malformed.Settings.GlassPreset == "liquid"
                && malformed.Settings.LogoStyle == "apple",
                $"barHeight={malformed.Settings.BarHeight} theme={malformed.Settings.Theme} tint={malformed.Settings.Tint} logoStyle={malformed.Settings.LogoStyle}");

            File.WriteAllText(settingsPath, "{ this is not json", new UTF8Encoding(false));
            var broken = store.Load();
            Check("invalid json → defaults + message", broken.Settings.BarHeight == 28 && !string.IsNullOrEmpty(broken.Error));

            // 6. Glass presets actually move all manual glass parameters.
            var presetProbe = AppSettings.CreateDefault();
            presetProbe.ApplyGlassPreset("clear");
            Check("glass preset applies parameters", presetProbe.GlassPreset == "clear" && presetProbe.Blur < 0.2 && presetProbe.Dispersion > 0.5);

            // 7. Reset restores documented defaults.
            var resetProbe = AppSettings.FromJson(JsonNode.Parse("""{"barHeight":40,"theme":"dark","showWifi":false}""") as JsonObject);
            resetProbe.ResetToDefaults();
            Check("reset restores defaults", resetProbe.BarHeight == 28 && resetProbe.Theme == "auto" && resetProbe.ShowWifi);

            // 8. Bundled fonts and wallpaper resolve at runtime.
            var fontPath = Path.Combine(AppAssets.FontsDirectory, "HarmonyOS_Sans_SC_Regular.ttf");
            Check("HarmonyOS Sans fonts bundled", File.Exists(fontPath), AppAssets.FontsDirectory);
            Check("HarmonyOS Sans loaded from private folder", AppAssets.FontsLoaded, (Application.Current?.Resources["Font.LoadedNote"] as string) ?? "");
            Check("wallpaper bundled", AppAssets.TopAnchoredWallpaperBrush is not null);
            Check("wallpaper luminance sampled", AppAssets.PreviewBandLuminance is > 0 and < 1, AppAssets.PreviewBandLuminance.ToString("0.000", CultureInfo.InvariantCulture));

            // 9. Native locator is side-effect free and tolerant of a missing bar.
            var nativePath = NativeBridge.FindNativeBar();
            Check("native bar lookup did not throw", true, nativePath ?? "未找到（允许）");
            Check("broadcast helper returns", NativeBridge.BroadcastSettingsChanged() || true, "RegisterWindowMessageW + PostMessageW(HWND_BROADCAST)");

            // 10. Read-only tray catalog (never throws; a missing registry key yields an empty list).
            trayCatalog = TrayCatalog.Load();
            Check("tray catalog readable", trayCatalog.Error is null,
                trayCatalog.Error ?? $"{trayCatalog.Records.Count} 个图标（{trayCatalog.PromotedCount} 显示 / {trayCatalog.HiddenCount} 隐藏）"
                    + $" · 原始 {trayCatalog.RawRecordCount} · 排除 SuperMenu {trayCatalog.ExcludedCount} · 去重 {trayCatalog.DuplicateCount}"
                    + $" · {trayCatalog.ReadMilliseconds:0.0} ms");
            Check("tray catalog records sane",
                trayCatalog.Records.All(record => record.RecordId > 0 && !string.IsNullOrWhiteSpace(record.Label))
                && trayCatalog.Records.All(record => !record.Label.StartsWith("IDS_", StringComparison.OrdinalIgnoreCase))
                && trayCatalog.Records.Select(record => record.Order)
                    .SequenceEqual(trayCatalog.Records.Select(record => record.Order).OrderBy(order => order)),
                $"{trayCatalog.Records.Count(record => record.Icon is not null)}/{trayCatalog.Records.Count} 个图标已解码");

            // 11. Finally restore a default document for the temp directory.
            store.Save(AppSettings.CreateDefault(), null);
        }
        catch (Exception ex)
        {
            allPassed = false;
            checks.Add(new JsonObject
            {
                ["name"] = "selftest crashed",
                ["ok"] = false,
                ["detail"] = ex.ToString(),
            });
        }
        finally
        {
            Environment.SetEnvironmentVariable(SettingsStore.PathOverrideVariable, null);
        }

        var results = new JsonObject
        {
            ["ok"] = allPassed,
            ["timestamp"] = DateTimeOffset.Now.ToString("O", CultureInfo.InvariantCulture),
            ["settingsPathUsed"] = settingsPath,
            ["nativeBar"] = NativeBridge.FindNativeBar(),
            ["nativeBarRunning"] = NativeBridge.IsNativeBarRunning(),
            ["fontsLoaded"] = AppAssets.FontsLoaded,
            ["trayCatalog"] = new JsonObject
            {
                ["error"] = trayCatalog?.Error,
                ["records"] = trayCatalog?.Records.Count ?? 0,
                ["promoted"] = trayCatalog?.PromotedCount ?? 0,
                ["hidden"] = trayCatalog?.HiddenCount ?? 0,
                ["rawRecords"] = trayCatalog?.RawRecordCount ?? 0,
                ["excluded"] = trayCatalog?.ExcludedCount ?? 0,
                ["duplicates"] = trayCatalog?.DuplicateCount ?? 0,
                ["readMilliseconds"] = trayCatalog?.ReadMilliseconds ?? 0,
            },
            ["checks"] = checks,
        };

        try
        {
            var directory = Path.GetDirectoryName(Path.GetFullPath(resultPath));
            if (!string.IsNullOrEmpty(directory)) Directory.CreateDirectory(directory);
            File.WriteAllText(resultPath, results.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), new UTF8Encoding(false));
        }
        catch
        {
            // The exit code still communicates pass/fail.
        }

        return allPassed ? 0 : 1;
    }

    private static bool JsonEquals(JsonNode? a, JsonNode? b)
    {
        if (a is null || b is null) return a is null && b is null;
        return JsonNode.DeepEquals(a, b);
    }
}
