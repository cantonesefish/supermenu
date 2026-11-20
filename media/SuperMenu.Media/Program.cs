using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using Windows.Devices.Radios;
using Windows.Media.Control;
using Windows.Storage.Streams;

namespace SuperMenu.Media;

internal static class Program
{
    private static readonly SemaphoreSlim RadioGate = new(1, 1);

    private static async Task<int> Main(string[] args)
    {
        var radioArg = Array.IndexOf(args, "--radio");
        if (radioArg >= 0 && radioArg + 2 < args.Length)
            return await SetRadioCommand(args[radioArg + 1], args[radioArg + 2]);
        var focusArg = Array.IndexOf(args, "--focus");
        if (focusArg >= 0 && focusArg + 1 < args.Length)
        {
            var enabled = args[focusArg + 1].Equals("on", StringComparison.OrdinalIgnoreCase);
            var ok = QuietHours.SetEnabled(enabled);
            var focusDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SuperMenu", "radio");
            await WriteRadioStateAsync(Path.Combine(focusDir, "state.txt"));
            return ok ? 0 : 1;
        }

        var parentPid = 0;
        var outputDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "SuperMenu", "media");
        for (var i = 0; i < args.Length - 1; ++i)
        {
            if (args[i] == "--parent") int.TryParse(args[i + 1], out parentPid);
            else if (args[i] == "--dir") outputDir = args[i + 1];
        }

        Directory.CreateDirectory(outputDir);
        var statePath = Path.Combine(outputDir, "state.txt");
        var coverPath = Path.Combine(outputDir, "cover.bin");
        var appIconPath = Path.Combine(outputDir, "appicon.png");
        var radioDir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "SuperMenu", "radio");
        var radioStatePath = Path.Combine(radioDir, "state.txt");
        ulong coverRevision = 0;
        byte[]? lastCover = null;
        ulong appIconRevision = 0;
        string lastAppKey = "";

        string lastKey = "";
        var parent = parentPid > 0 ? TryProcess(parentPid) : null;
        if (parent is null && parentPid > 0) return 0;

        try
        {
            await WriteRadioStateAsync(radioStatePath);
            _ = RadioCommandLoopAsync(radioDir);
            var manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
            var lastRadioPoll = DateTimeOffset.MinValue;
            while (true)
            {
                if (parent is not null && parent.HasExited) break;
                if (DateTimeOffset.UtcNow - lastRadioPoll > TimeSpan.FromSeconds(2))
                {
                    lastRadioPoll = DateTimeOffset.UtcNow;
                    await WriteRadioStateAsync(radioStatePath);
                }
                if (parent is null && parentPid > 0) break;
                try
                {
                    var session = manager.GetCurrentSession();
                    if (session is null)
                    {
                        WriteState(statePath, active: false, title: "", artist: "", album: "", playing: false,
                            positionMs: 0, durationMs: 0, coverRevision: 0, appIconRevision: 0);
                        lastKey = "";
                        lastCover = null;
                        lastAppKey = "";
                        appIconRevision = 0;
                    }
                    else
                    {
                        var properties = await session.TryGetMediaPropertiesAsync();
                        var playback = session.GetPlaybackInfo();
                        var timeline = session.GetTimelineProperties();
                        var start = timeline.StartTime;
                        var duration = timeline.EndTime - start;
                        var playing = playback.PlaybackStatus == GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing;
                        var position = timeline.Position - start;
                        if (playing)
                        {
                            // SMTC's timeline is a snapshot; advance it locally so the
                            // progress bar moves smoothly between helper polls.
                            var sinceUpdate = DateTimeOffset.Now - timeline.LastUpdatedTime;
                            if (sinceUpdate > TimeSpan.Zero && sinceUpdate < TimeSpan.FromMinutes(10)) position += sinceUpdate;
                        }
                        var durationMs = (long)Math.Max(0, Math.Min(duration.TotalMilliseconds, int.MaxValue));
                        var positionMs = (long)Math.Max(0, Math.Min(position.TotalMilliseconds, durationMs > 0 ? durationMs : int.MaxValue));
                        var title = Clean(properties.Title);
                        var artist = Clean(string.IsNullOrWhiteSpace(properties.Artist) ? properties.AlbumArtist : properties.Artist);
                        var album = Clean(properties.AlbumTitle);
                        if (string.IsNullOrWhiteSpace(title))
                            title = Clean(session.SourceAppUserModelId);

                        var key = title + "\u001f" + artist + "\u001f" + album;
                        // Artwork can arrive one poll after the title changes, and
                        // some players intermittently return null. Keep the last
                        // valid cover and retry while we have none.
                        if (properties.Thumbnail is not null &&
                            (key != lastKey || coverRevision == 0))
                        {
                            var bytes = await ReadThumbnail(properties.Thumbnail);
                            if (bytes.Length > 0 && (lastCover is null || !bytes.AsSpan().SequenceEqual(lastCover)))
                            {
                                await File.WriteAllBytesAsync(coverPath + ".tmp", bytes);
                                File.Move(coverPath + ".tmp", coverPath, overwrite: true);
                                lastCover = bytes;
                                coverRevision = HashCover(bytes);
                            }
                        }
                        lastKey = key;
                        var appId = Clean(session.SourceAppUserModelId);
                        if (appId != lastAppKey)
                        {
                            lastAppKey = appId;
                            appIconRevision = 0;
                            var icon = ExtractApplicationIcon(appId);
                            if (icon.Length > 0)
                            {
                                await File.WriteAllBytesAsync(appIconPath + ".tmp", icon);
                                File.Move(appIconPath + ".tmp", appIconPath, overwrite: true);
                                appIconRevision = HashCover(icon);
                            }
                        }
                        WriteState(statePath, active: true, title, artist, album, playing, positionMs, durationMs,
                            coverRevision, appIconRevision);
                    }
                }
                catch
                {
                    WriteState(statePath, active: false, title: "", artist: "", album: "", playing: false,
                        positionMs: 0, durationMs: 0, coverRevision: 0, appIconRevision: 0);
                }
                await Task.Delay(500);
            }
        }
        catch
        {
            // No WinRT/SMTC on this machine: still terminate quietly; the bar keeps its fallback card.
        }
        return 0;
    }

    private static Process? TryProcess(int pid)
    {
        try { return Process.GetProcessById(pid); }
        catch { return null; }
    }

    private static string Clean(string? value)
    {
        if (string.IsNullOrEmpty(value)) return "";
        var text = value.Replace('\r', ' ').Replace('\n', ' ').Trim();
        return text.Length > 300 ? text[..300] : text;
    }

    private static ulong HashCover(byte[] bytes)
    {
        var hash = SHA256.HashData(bytes);
        return BitConverter.ToUInt64(hash, 0);
    }

    private static byte[] ExtractApplicationIcon(string appUserModelId)
    {
        if (string.IsNullOrWhiteSpace(appUserModelId)) return [];
        var wanted = appUserModelId.ToLowerInvariant();
        string executable = "";
        var candidates = new List<Process>();
        foreach (var process in Process.GetProcesses())
        {
            try
            {
                var name = process.ProcessName.ToLowerInvariant();
                if (string.IsNullOrWhiteSpace(name)) continue;
                if (!(wanted.Contains(name) || name.Contains(wanted))) continue;
                candidates.Add(process);
                if (process.MainWindowHandle != IntPtr.Zero)
                {
                    executable = process.MainModule?.FileName ?? "";
                    if (executable.Length > 0) break;
                }
            }
            catch
            {
                // Process exited or is inaccessible.
            }
        }
        if (executable.Length == 0)
        {
            foreach (var process in candidates)
            {
                try
                {
                    executable = process.MainModule?.FileName ?? "";
                    if (executable.Length > 0) break;
                }
                catch
                {
                }
            }
        }
        if (executable.Length == 0 || !File.Exists(executable)) return [];

        IntPtr[] icons = new IntPtr[1];
        UInt32[] ids = new UInt32[1];
        try
        {
            if (PrivateExtractIconsW(executable, 0, 64, 64, icons, ids, 1, 0) == 0 || icons[0] == IntPtr.Zero) return [];
            var source = Imaging.CreateBitmapSourceFromHIcon(
                icons[0], Int32Rect.Empty, BitmapSizeOptions.FromWidthAndHeight(64, 64));
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(source));
            using var memory = new MemoryStream();
            encoder.Save(memory);
            return memory.ToArray();
        }
        catch
        {
            return [];
        }
        finally
        {
            if (icons[0] != IntPtr.Zero) DestroyIcon(icons[0]);
        }
    }

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern uint PrivateExtractIconsW(string file, int index, int cx, int cy, IntPtr[] icons,
        UInt32[] iconIds, uint count, uint flags);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr icon);

    private static async Task<byte[]> ReadThumbnail(IRandomAccessStreamReference thumbnail)
    {
        using var stream = await thumbnail.OpenReadAsync();
        using var memory = new MemoryStream();
        using var source = stream.AsStreamForRead();
        await source.CopyToAsync(memory);
        return memory.ToArray();
    }

    private static void WriteState(string path, bool active, string title, string artist, string album, bool playing,
        long positionMs, long durationMs, ulong coverRevision, ulong appIconRevision)
    {
        var builder = new StringBuilder();
        builder.Append("active=").Append(active ? '1' : '0').Append('\n');
        builder.Append("title=").Append(title).Append('\n');
        builder.Append("artist=").Append(artist).Append('\n');
        builder.Append("album=").Append(album).Append('\n');
        builder.Append("playing=").Append(playing ? '1' : '0').Append('\n');
        builder.Append("position=").Append(Math.Clamp(positionMs, 0, int.MaxValue)).Append('\n');
        builder.Append("duration=").Append(Math.Clamp(durationMs, 0, int.MaxValue)).Append('\n');
        builder.Append("cover_rev=").Append(coverRevision.ToString(CultureInfo.InvariantCulture)).Append('\n');
        builder.Append("app_icon_rev=").Append(appIconRevision.ToString(CultureInfo.InvariantCulture)).Append('\n');
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, builder.ToString(), new UTF8Encoding(false));
        File.Move(temporary, path, overwrite: true);
    }

    private static async Task RadioCommandLoopAsync(string directory)
    {
        while (true)
        {
            try
            {
                await ProcessRadioCommandAsync(directory);
            }
            catch
            {
                // Never let a transient file race kill the command loop.
            }
            await Task.Delay(60);
        }
    }

    private static async Task ProcessRadioCommandAsync(string directory)
    {
        var path = Path.Combine(directory, "command.txt");
        try
        {
            if (!File.Exists(path)) return;
            var text = File.ReadAllText(path).Trim();
            File.Delete(path);
            var parts = text.Split('=', 2);
            if (parts.Length != 2) return;
            var on = parts[1].Equals("on", StringComparison.OrdinalIgnoreCase);
            if (parts[0].Equals("focus", StringComparison.OrdinalIgnoreCase))
            {
                QuietHours.SetEnabled(on);
                await WriteRadioStateAsync(Path.Combine(directory, "state.txt"));
                return;
            }
            var wanted = parts[0].Equals("wifi", StringComparison.OrdinalIgnoreCase) ? RadioKind.WiFi
                : parts[0].Equals("bluetooth", StringComparison.OrdinalIgnoreCase) ? RadioKind.Bluetooth
                : (RadioKind?)null;
            if (wanted is null) return;
            var radios = await Radio.GetRadiosAsync();
            var radio = radios.FirstOrDefault(r => r.Kind == wanted.Value);
            if (radio is null) return;
            await radio.SetStateAsync(on ? RadioState.On : RadioState.Off);
            await WriteRadioStateAsync(Path.Combine(directory, "state.txt"));
        }
        catch
        {
            // Keep the media loop alive if a command races with shutdown.
        }
    }

    private static async Task WriteRadioStateAsync(string path)
    {
        await RadioGate.WaitAsync();
        try
        {
            var radios = await Radio.GetRadiosAsync();
            var wifi = radios.FirstOrDefault(r => r.Kind == RadioKind.WiFi);
            var bluetooth = radios.FirstOrDefault(r => r.Kind == RadioKind.Bluetooth);
            var builder = new StringBuilder();
            if (wifi is not null) builder.Append("wifi=").Append(wifi.State == RadioState.On ? '1' : '0').Append('\n');
            if (bluetooth is not null)
                builder.Append("bluetooth=").Append(bluetooth.State == RadioState.On ? '1' : '0').Append('\n');
            if (QuietHours.TryGetEnabled(out var focus))
                builder.Append("focus=").Append(focus ? '1' : '0').Append('\n');
            if (builder.Length == 0) return;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var temporary = path + ".tmp";
            File.WriteAllText(temporary, builder.ToString(), new UTF8Encoding(false));
            File.Move(temporary, path, overwrite: true);
        }
        catch
        {
            // A machine without a radio (or a restricted session) simply has no
            // radio state file; the native UI keeps its last known state.
        }
        finally
        {
            RadioGate.Release();
        }
    }

    private static async Task<int> SetRadioCommand(string kind, string state)
    {
        var on = state.Equals("on", StringComparison.OrdinalIgnoreCase);
        var wanted = kind.Equals("wifi", StringComparison.OrdinalIgnoreCase) ? RadioKind.WiFi
            : kind.Equals("bluetooth", StringComparison.OrdinalIgnoreCase) ? RadioKind.Bluetooth
            : (RadioKind?)null;
        if (wanted is null) return 2;
        try
        {
            var radios = await Radio.GetRadiosAsync();
            var radio = radios.FirstOrDefault(r => r.Kind == wanted.Value);
            if (radio is null) return 3;
            var result = await radio.SetStateAsync(on ? RadioState.On : RadioState.Off);
            var radioStatePath = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SuperMenu", "radio", "state.txt");
            await WriteRadioStateAsync(radioStatePath);
            return result == RadioAccessStatus.Allowed ? 0 : 4;
        }
        catch
        {
            return 1;
        }
    }
}
