using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Interop;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

namespace SuperMenu.Settings.Services;

/// <summary>
/// One notification-area record as Windows 11 persists it under
/// <c>HKCU\Control Panel\NotifyIconSettings</c>. The settings app only reads this data;
/// it never registers or modifies tray icons (the native bar does the rendering).
/// </summary>
public sealed class TrayRecord
{
    /// <summary>Decimal registry subkey name; the same id the native bar consumes.</summary>
    public required ulong RecordId { get; init; }

    /// <summary>Cleaned InitialTooltip first line, else the executable's FileDescription, else its stem.</summary>
    public required string Label { get; init; }

    /// <summary>Absolute executable path when it could be resolved, otherwise empty.</summary>
    public string Exe { get; init; } = string.Empty;

    /// <summary>Windows' own IsPromoted flag (true = pinned in the taskbar notification area).</summary>
    public bool PlatformPromoted { get; init; }

    /// <summary>Position in Windows' UIOrderList; <see cref="ulong.MaxValue"/> when the record is not listed.</summary>
    public ulong Order { get; init; } = ulong.MaxValue;

    /// <summary>Raw PNG bytes of the record's IconSnapshot (null when the value is absent/invalid).</summary>
    public byte[]? SnapshotPng { get; init; }

    /// <summary>Decoded snapshot (falls back to null when the PNG cannot be decoded).</summary>
    public BitmapSource? Icon { get; internal set; }
}

/// <summary>
/// Read-only reader for Windows 11's notification-area persistence key. It mirrors the native
/// bar's <c>sm::TrayReader</c>: numeric subkeys, UIOrderList ordering, known-folder/environment
/// path resolution, bidi-cleaned labels, SuperMenu's own executables excluded and duplicate
/// records (same snapshot bytes + label) collapsed to the earliest UIOrderList record.
/// </summary>
public sealed class TrayCatalog
{
    /// <summary>HKCU relative path of the Windows 11 notification-area persistence key.</summary>
    public const string RegistryPath = @"Control Panel\NotifyIconSettings";

    private const string SnapshotValue = "IconSnapshot";
    private const string ExecutablePathValue = "ExecutablePath";
    private const string TooltipValue = "InitialTooltip";
    private const string PromotedValue = "IsPromoted";
    private const string OrderListValue = "UIOrderList";

    /// <summary>Records belonging to SuperMenu itself are not offered for customisation.</summary>
    private static readonly HashSet<string> OwnExecutableNames = new(StringComparer.OrdinalIgnoreCase)
    {
        "SuperMenuBar.exe",
        "SuperMenu.Settings.exe",
    };

    /// <summary>Used only when SHGetKnownFolderPath is unavailable; covers the GUIDs seen in practice.</summary>
    private static readonly Dictionary<Guid, string> KnownFolderFallbacks = BuildKnownFolderFallbacks();

    private TrayCatalog(
        IReadOnlyList<TrayRecord> records,
        int rawRecordCount,
        int excludedCount,
        int duplicateCount,
        string? error,
        double readMilliseconds,
        bool liveFilterApplied = false)
    {
        Records = records;
        RawRecordCount = rawRecordCount;
        ExcludedCount = excludedCount;
        DuplicateCount = duplicateCount;
        Error = error;
        ReadMilliseconds = readMilliseconds;
        LiveFilterApplied = liveFilterApplied;
    }

    /// <summary>Visible records in Windows order (own executables excluded, duplicates collapsed).</summary>
    public IReadOnlyList<TrayRecord> Records { get; }

    /// <summary>Number of numeric registry records that could be read (includes excluded/duplicates).</summary>
    public int RawRecordCount { get; }

    /// <summary>Records skipped because they belong to SuperMenuBar.exe / SuperMenu.Settings.exe.</summary>
    public int ExcludedCount { get; }

    /// <summary>Records collapsed because another record had the same snapshot bytes and label.</summary>
    public int DuplicateCount { get; }

    /// <summary>Unexpected read failure; missing registry key is not an error (empty catalog).</summary>
    public string? Error { get; }

    /// <summary>
    /// True when the native bar's fresh tray-live snapshot was applied: only tray
    /// records that currently have a live notification icon are listed.
    /// Otherwise the list falls back to all persisted records with a warning.
    /// </summary>
    public bool LiveFilterApplied { get; }

    /// <summary>Wall-clock time spent building this catalog (diagnostics).</summary>
    public double ReadMilliseconds { get; }

    public int PromotedCount => Records.Count(record => record.PlatformPromoted);

    public int HiddenCount => Records.Count - PromotedCount;

    /// <summary>Reads the registry. Never throws: failures are reported through <see cref="Error"/>.</summary>
    public static TrayCatalog Load()
    {
        var stopwatch = Stopwatch.StartNew();
        try
        {
            using var root = Registry.CurrentUser.OpenSubKey(RegistryPath, writable: false);
            if (root is null)
                return new TrayCatalog([], 0, 0, 0, null, stopwatch.Elapsed.TotalMilliseconds);

            var orderList = ReadOrderList(root);
            var descriptionCache = new Dictionary<string, string?>(StringComparer.OrdinalIgnoreCase);
            var candidates = new List<TrayRecord>();
            var rawRecordCount = 0;
            var excludedCount = 0;

            foreach (var name in root.GetSubKeyNames())
            {
                if (!ulong.TryParse(name, NumberStyles.None, CultureInfo.InvariantCulture, out var recordId))
                    continue;

                TrayRecord? record;
                bool isOwn;
                try
                {
                    record = ReadRecord(root, name, recordId, orderList, descriptionCache, out isOwn);
                }
                catch (Exception ex) when (ex is not OutOfMemoryException)
                {
                    // A single malformed record must not hide the rest of the tray list.
                    continue;
                }

                if (record is null) continue;
                rawRecordCount++;
                if (isOwn)
                {
                    excludedCount++;
                    continue;
                }

                candidates.Add(record);
            }

            // Windows renders by UIOrderList; records outside it sort last by label then id.
            candidates.Sort(CompareByWindowsOrder);

            var seen = new HashSet<string>(StringComparer.Ordinal);
            var records = new List<TrayRecord>(candidates.Count);
            var duplicateCount = 0;
            foreach (var candidate in candidates)
            {
                if (!seen.Add(BuildDuplicateKey(candidate)))
                {
                    duplicateCount++;
                    continue;
                }

                records.Add(candidate);
            }

            var liveFilterApplied = ApplyLiveSnapshot(records);
            // Match the native bar: use the live program's actual application
            // icon; fall back to Explorer's persisted snapshot when that fails.
            foreach (var record in records) record.Icon = DecodeIcon(record.SnapshotPng, record.Exe);

            return new TrayCatalog(
                records,
                rawRecordCount,
                excludedCount,
                duplicateCount,
                null,
                stopwatch.Elapsed.TotalMilliseconds,
                liveFilterApplied);
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return new TrayCatalog([], 0, 0, 0, ex.Message, stopwatch.Elapsed.TotalMilliseconds);
        }
    }

    private static TrayRecord? ReadRecord(
        RegistryKey root,
        string name,
        ulong recordId,
        IReadOnlyDictionary<ulong, ulong> orderList,
        Dictionary<string, string?> descriptionCache,
        out bool isOwn)
    {
        isOwn = false;
        using var subKey = root.OpenSubKey(name, writable: false);
        if (subKey is null) return null;

        var snapshot = ReadBinary(subKey, SnapshotValue);
        var rawExecutablePath = ReadString(subKey, ExecutablePathValue);
        var tooltip = ReadString(subKey, TooltipValue);
        var promoted = ReadDword(subKey, PromotedValue) != 0;

        var executablePath = ResolveExecutablePath(rawExecutablePath);
        isOwn = IsOwnExecutable(rawExecutablePath, executablePath);

        var label = BuildLabel(tooltip, executablePath, rawExecutablePath, descriptionCache);

        return new TrayRecord
        {
            RecordId = recordId,
            Label = label,
            Exe = executablePath,
            PlatformPromoted = promoted,
            Order = orderList.TryGetValue(recordId, out var order) ? order : ulong.MaxValue,
            SnapshotPng = snapshot,
        };
    }

    private static Dictionary<ulong, ulong> ReadOrderList(RegistryKey root)
    {
        var order = new Dictionary<ulong, ulong>();
        try
        {
            if (root.GetValue(OrderListValue, null, RegistryValueOptions.DoNotExpandEnvironmentNames) is not byte[] bytes)
                return order;

            // UIOrderList is a plain sequence of little-endian uint64 record ids.
            for (var offset = 0; offset + 8 <= bytes.Length; offset += 8)
            {
                var id = BitConverter.ToUInt64(bytes, offset);
                if (!order.ContainsKey(id)) order[id] = (ulong)(offset / 8);
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            // A malformed order list only loses the explicit ordering.
        }

        return order;
    }

    private static int CompareByWindowsOrder(TrayRecord left, TrayRecord right)
    {
        var comparison = left.Order.CompareTo(right.Order);
        if (comparison != 0) return comparison;
        comparison = string.Compare(left.Label, right.Label, StringComparison.OrdinalIgnoreCase);
        if (comparison != 0) return comparison;
        return left.RecordId.CompareTo(right.RecordId);
    }

    /// <summary>Same snapshot bytes (or same exe when a record has no snapshot) and the same label.</summary>
    /// <summary>
    /// The native bar writes the record IDs of icons proven live by the shell into
    /// %LOCALAPPDATA%\SuperMenu\tray-live.txt every couple of seconds. When that
    /// snapshot is fresh, filter out dead persisted records (the same policy the
    /// bar uses), so the settings list matches the current notification area.
    /// </summary>
    private static bool ApplyLiveSnapshot(List<TrayRecord> records)
    {
        try
        {
            var path = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SuperMenu",
                "tray-live.txt");
            var info = new FileInfo(path);
            if (!info.Exists || DateTime.UtcNow - info.LastWriteTimeUtc > TimeSpan.FromSeconds(15))
                return false;
            var liveIds = new HashSet<ulong>();
            foreach (var line in File.ReadAllLines(path))
            {
                if (ulong.TryParse(line.Trim(), NumberStyles.None, CultureInfo.InvariantCulture, out var id))
                    liveIds.Add(id);
            }
            records.RemoveAll(record => !liveIds.Contains(record.RecordId));
            return true;
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return false;
        }
    }

    private static string BuildDuplicateKey(TrayRecord record)
    {
        var imageKey = record.SnapshotPng is { Length: > 0 } png
            ? Convert.ToHexString(SHA256.HashData(png))
            : "exe:" + record.Exe.ToUpperInvariant();
        return imageKey + "\u0000" + record.Label;
    }

    private static BitmapSource? DecodeIcon(byte[]? png, string executablePath)
    {
        var applicationIcon = DecodeApplicationIcon(executablePath);
        if (applicationIcon is not null) return applicationIcon;
        if (png is not { Length: > 0 }) return null;
        try
        {
            using var stream = new MemoryStream(png, writable: false);
            var image = new BitmapImage();
            image.BeginInit();
            // OnLoad + StreamSource means the PNG is fully decoded when EndInit returns, so the
            // stream can be disposed immediately. (Do not set IgnoreImageCache here: WPF requires
            // a UriSource for that option; a stream-only BitmapImage throws ArgumentNullException.)
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.StreamSource = stream;
            image.EndInit();
            image.Freeze();
            return image;
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            // A broken snapshot simply renders the placeholder glyph in the UI.
            return null;
        }
    }

    private static BitmapSource? DecodeApplicationIcon(string executablePath)
    {
        if (string.IsNullOrWhiteSpace(executablePath) || !File.Exists(executablePath)) return null;
        IntPtr[] icons = new IntPtr[1];
        try
        {
            if (ExtractIconExW(executablePath, 0, icons, null, 1) == 0 || icons[0] == IntPtr.Zero) return null;
            var image = Imaging.CreateBitmapSourceFromHIcon(
                icons[0],
                System.Windows.Int32Rect.Empty,
                BitmapSizeOptions.FromEmptyOptions());
            image.Freeze();
            return image;
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return null;
        }
        finally
        {
            if (icons[0] != IntPtr.Zero) DestroyIcon(icons[0]);
        }
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    private static extern uint ExtractIconExW(string file, int index, IntPtr[] largeIcons, IntPtr[]? smallIcons, uint count);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(IntPtr icon);

    // =====================================================================================
    // Registry value helpers (all tolerant: wrong types fall back to defaults)
    // =====================================================================================

    private static byte[]? ReadBinary(RegistryKey key, string name)
    {
        try
        {
            return key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames) as byte[];
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return null;
        }
    }

    private static string ReadString(RegistryKey key, string name)
    {
        try
        {
            return key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames) switch
            {
                string value => value,
                byte[] bytes => Encoding.Unicode.GetString(bytes).TrimEnd('\0'),
                _ => string.Empty,
            };
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return string.Empty;
        }
    }

    private static int ReadDword(RegistryKey key, string name)
    {
        try
        {
            return key.GetValue(name, null, RegistryValueOptions.DoNotExpandEnvironmentNames) switch
            {
                int value => value,
                long value => unchecked((int)value),
                string text when int.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsed) => parsed,
                byte[] { Length: >= 4 } bytes => BitConverter.ToInt32(bytes, 0),
                _ => 0,
            };
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            return 0;
        }
    }

    // =====================================================================================
    // Executable path / label resolution
    // =====================================================================================

    /// <summary>Resolves a leading <c>{FOLDERID-GUID}\...</c> via the shell and expands <c>%ENV%</c>.</summary>
    private static string ResolveExecutablePath(string? rawPath)
    {
        if (string.IsNullOrWhiteSpace(rawPath)) return string.Empty;
        var value = rawPath.Trim();

        if (value.StartsWith('{'))
        {
            var closing = value.IndexOf('}');
            if (closing > 1 && Guid.TryParse(value[1..closing], out var folderId))
            {
                var folder = GetKnownFolderPath(folderId);
                if (!string.IsNullOrEmpty(folder))
                {
                    var relative = value[(closing + 1)..].TrimStart('\\', '/');
                    value = relative.Length == 0 ? folder : Path.Combine(folder, relative);
                }
            }
        }

        try
        {
            value = Environment.ExpandEnvironmentVariables(value);
        }
        catch (ArgumentException)
        {
            // An unparseable %VAR% is kept verbatim; the basename fallback still works.
        }

        return value;
    }

    private static string? GetKnownFolderPath(Guid folderId)
    {
        var pointer = IntPtr.Zero;
        try
        {
            var id = folderId;
            if (SHGetKnownFolderPath(ref id, 0, IntPtr.Zero, out pointer) == 0 && pointer != IntPtr.Zero)
            {
                var path = Marshal.PtrToStringUni(pointer);
                if (!string.IsNullOrWhiteSpace(path)) return path;
            }
        }
        catch (Exception ex) when (ex is DllNotFoundException or EntryPointNotFoundException)
        {
            // Fall through to the hard-coded well-known locations below.
        }
        finally
        {
            if (pointer != IntPtr.Zero) Marshal.FreeCoTaskMem(pointer);
        }

        return KnownFolderFallbacks.TryGetValue(folderId, out var fallback) ? fallback : null;
    }

    private static Dictionary<Guid, string> BuildKnownFolderFallbacks()
    {
        var folders = new Dictionary<Guid, string>();
        void Add(string guid, string path)
        {
            if (Guid.TryParse(guid, out var id) && !string.IsNullOrEmpty(path)) folders[id] = path;
        }

        Add("6D809377-6AF0-444B-8957-A3773F02200E", Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles));
        Add("7C5A40EF-A0FB-4BFC-874A-C0F2E0B9FA8E", Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86));
        Add("F38BF404-1D43-42F2-9305-67DE0B28FC23", Environment.GetFolderPath(Environment.SpecialFolder.Windows));
        Add("1AC14E77-02E7-4E5D-B744-2EB1AE5198B7", Environment.GetFolderPath(Environment.SpecialFolder.System));
        Add("62AB5D82-FDC1-4DC3-A9DD-070D1D495D97", Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData));
        Add("F1B32785-6FBA-4FCF-9D55-7B8E7F157091", Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData));
        Add("3EB685DB-65F9-4CF6-A03A-E3EF65729F3D", Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData));
        Add("5E6C858F-0E22-4760-9AFE-EA3317B67173", Environment.GetFolderPath(Environment.SpecialFolder.UserProfile));
        Add("374DE290-123F-4565-9164-39C4925E467B", Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads"));
        return folders;
    }

    private static bool IsOwnExecutable(string rawPath, string resolvedPath)
        => IsOwnName(FileName(resolvedPath)) || IsOwnName(FileName(rawPath));

    private static bool IsOwnName(string fileName) => fileName.Length > 0 && OwnExecutableNames.Contains(fileName);

    /// <summary>Filename part of a Win32 path without needing a valid absolute path.</summary>
    private static string FileName(string? path)
    {
        if (string.IsNullOrWhiteSpace(path)) return string.Empty;
        var index = path.LastIndexOfAny(['\\', '/']);
        return index >= 0 ? path[(index + 1)..] : path;
    }

    // =====================================================================================
    // Label cleaning
    // =====================================================================================

    private static string BuildLabel(
        string? tooltip,
        string resolvedExecutablePath,
        string rawExecutablePath,
        Dictionary<string, string?> descriptionCache)
    {
        var label = CleanLabel(tooltip);
        // Some apps store a localization resource id in InitialTooltip (e.g. HUAWEI Cloud's
        // IDS_MAIN_PAGE_TITLE); the native TrayReader treats it as empty, so do the same here.
        if (label.StartsWith("IDS_", StringComparison.OrdinalIgnoreCase)) label = string.Empty;
        if (label.Length > 0) return label;

        var description = CleanLabel(TryGetFileDescription(resolvedExecutablePath, descriptionCache));
        if (description.Length > 0) return description;

        var fileName = FileName(resolvedExecutablePath);
        if (fileName.Length == 0) fileName = FileName(rawExecutablePath);
        var stem = Path.GetFileNameWithoutExtension(fileName);
        return string.IsNullOrWhiteSpace(stem) ? "未知图标" : stem.Trim();
    }

    private static string? TryGetFileDescription(string executablePath, Dictionary<string, string?> cache)
    {
        if (string.IsNullOrEmpty(executablePath)) return null;
        if (cache.TryGetValue(executablePath, out var cached)) return cached;

        string? description = null;
        try
        {
            if (File.Exists(executablePath))
            {
                var info = FileVersionInfo.GetVersionInfo(executablePath);
                var value = info.FileDescription;
                if (!string.IsNullOrWhiteSpace(value)) description = value;
            }
        }
        catch (Exception ex) when (ex is not OutOfMemoryException)
        {
            // Missing/unreadable version resources fall back to the file stem.
        }

        cache[executablePath] = description;
        return description;
    }

    /// <summary>
    /// Removes bidi control characters, keeps only the first line, replaces tabs with spaces and
    /// trims (including NBSP / U+3000) — matching the native bar's label cleaning.
    /// </summary>
    private static string CleanLabel(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return string.Empty;
        var builder = new StringBuilder(value.Length);
        foreach (var character in value)
        {
            if (character is '\r' or '\n') break;
            if (IsBidiControl(character)) continue;
            builder.Append(character == '\t' ? ' ' : character);
        }

        return builder.ToString().Trim().Trim('\u00A0', '\u3000').Trim();
    }

    private static bool IsBidiControl(char character)
        => (character >= '\u202A' && character <= '\u202E')
           || character == '\u200E'
           || character == '\u200F'
           || (character >= '\u2066' && character <= '\u2069');

    [DllImport("shell32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int SHGetKnownFolderPath(ref Guid folderId, uint flags, IntPtr token, out IntPtr path);
}
