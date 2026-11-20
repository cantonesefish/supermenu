using System.ComponentModel;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace SuperMenu.Settings.Models;

/// <summary>
/// Flat, strongly typed view of <c>%LOCALAPPDATA%\SuperMenu\settings.json</c>.
/// The contract lives in docs/settings-schema.md; every key there is a property here.
/// The class raises <see cref="PropertyChanged"/> and <see cref="Changed"/> so the
/// window can reflect loaded values and track unapplied edits without a second view model.
/// </summary>
public sealed class AppSettings : INotifyPropertyChanged
{
    public const int CurrentVersion = 1;

    public static readonly string[] Languages = ["zh-CN", "en-US"];
    public static readonly string[] Themes = ["auto", "light", "dark"];
    public static readonly string[] GlassPresets = ["liquid", "frosted", "clear"];

    /// <summary>Glyph used by the leftmost (Apple menu) button; the native bar renders the same choice.</summary>
    public static readonly string[] LogoStyles = ["apple", "windows", "windows-mono"];

    /// <summary>camelCase keys owned by this contract, used to preserve unknown keys on save.</summary>
    public static readonly IReadOnlySet<string> KnownKeys = new HashSet<string>(StringComparer.Ordinal)
    {
        "version", "language", "theme", "barHeight", "monitor", "reserveWorkArea", "autoHide",
        "hideInFullscreen", "showSeconds", "use24Hour", "showDate", "showAppName", "showAppleMenu",
        "logoStyle",
        "showWifi", "showBattery", "showBatteryPercent", "showVolume", "showControlCenter", "showSearch",
        "showNotifications", "rightIconSpacing", "fontScale", "glassPreset", "transparency", "blur", "refraction",
        "edge", "dispersion", "highlight", "shadow", "tint", "noise", "saturation", "launchAtLogin", "menuAnimations",
        "trayShow", "trayHide",
    };

    private static readonly Regex TintPattern = new("^#[0-9a-fA-F]{6}$", RegexOptions.Compiled);

    private int _version = CurrentVersion;
    private string _language = "zh-CN";
    private string _theme = "auto";
    private double _barHeight = 28;
    private int _monitor;
    private bool _reserveWorkArea = true;
    private bool _autoHide;
    private bool _hideInFullscreen = true;
    private bool _showSeconds;
    private bool _use24Hour = true;
    private bool _showDate = true;
    private bool _showAppName = true;
    private bool _showAppleMenu = true;
    private string _logoStyle = "apple";
    private bool _showWifi = true;
    private bool _showBattery = true;
    private bool _showBatteryPercent;
    private bool _showVolume;
    private bool _showControlCenter = true;
    private bool _showSearch = true;
    private bool _showNotifications;
    private double _fontScale = 1.0;
    private string _glassPreset = "liquid";
    private double _transparency = 0.94;
    private double _blur = 0.52;
    private double _refraction = 18;
    private double _edge = 18;
    private double _dispersion = 0.30;
    private double _highlight = 0.80;
    private double _shadow = 0.35;
    private string _tint = "#FFFFFF";
    private double _noise = 0.015;
    private double _saturation = 1.15;
    private bool _launchAtLogin;
    private bool _menuAnimations = true;

    // Tray / right-side icon customisation (see docs/settings-schema.md).
    private int _rightIconSpacing = 20;
    private string _trayShow = string.Empty;
    private string _trayHide = string.Empty;

    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>Raised after any property changed; used for the unapplied-edit indicator and the preview.</summary>
    public event EventHandler? Changed;

    public int Version { get => _version; set => Set(ref _version, value); }
    public string Language { get => _language; set => Set(ref _language, value); }
    public string Theme { get => _theme; set => Set(ref _theme, value); }
    public double BarHeight { get => _barHeight; set => Set(ref _barHeight, value); }
    public int Monitor { get => _monitor; set => Set(ref _monitor, value); }
    public bool ReserveWorkArea { get => _reserveWorkArea; set => Set(ref _reserveWorkArea, value); }
    public bool AutoHide { get => _autoHide; set => Set(ref _autoHide, value); }
    public bool HideInFullscreen { get => _hideInFullscreen; set => Set(ref _hideInFullscreen, value); }
    public bool ShowSeconds { get => _showSeconds; set => Set(ref _showSeconds, value); }
    public bool Use24Hour { get => _use24Hour; set => Set(ref _use24Hour, value); }
    public bool ShowDate { get => _showDate; set => Set(ref _showDate, value); }
    public bool ShowAppName { get => _showAppName; set => Set(ref _showAppName, value); }
    public bool ShowAppleMenu { get => _showAppleMenu; set => Set(ref _showAppleMenu, value); }
    public string LogoStyle { get => _logoStyle; set => Set(ref _logoStyle, value); }
    public bool ShowWifi { get => _showWifi; set => Set(ref _showWifi, value); }
    public bool ShowBattery { get => _showBattery; set => Set(ref _showBattery, value); }
    public bool ShowBatteryPercent { get => _showBatteryPercent; set => Set(ref _showBatteryPercent, value); }
    public bool ShowVolume { get => _showVolume; set => Set(ref _showVolume, value); }
    public bool ShowControlCenter { get => _showControlCenter; set => Set(ref _showControlCenter, value); }
    public bool ShowSearch { get => _showSearch; set => Set(ref _showSearch, value); }
    public bool ShowNotifications { get => _showNotifications; set => Set(ref _showNotifications, value); }
    public double FontScale { get => _fontScale; set => Set(ref _fontScale, value); }
    public string GlassPreset { get => _glassPreset; set => Set(ref _glassPreset, value); }
    public double Transparency { get => _transparency; set => Set(ref _transparency, value); }
    public double Blur { get => _blur; set => Set(ref _blur, value); }
    public double Refraction { get => _refraction; set => Set(ref _refraction, value); }
    public double Edge { get => _edge; set => Set(ref _edge, value); }
    public double Dispersion { get => _dispersion; set => Set(ref _dispersion, value); }
    public double Highlight { get => _highlight; set => Set(ref _highlight, value); }
    public double Shadow { get => _shadow; set => Set(ref _shadow, value); }
    public string Tint { get => _tint; set => Set(ref _tint, value); }
    public double Noise { get => _noise; set => Set(ref _noise, value); }
    public double Saturation { get => _saturation; set => Set(ref _saturation, value); }
    public bool LaunchAtLogin { get => _launchAtLogin; set => Set(ref _launchAtLogin, value); }
    public bool MenuAnimations { get => _menuAnimations; set => Set(ref _menuAnimations, value); }

    /// <summary>Gap (DIP) between the right-side status icons and the small tray-icon group.</summary>
    public int RightIconSpacing { get => _rightIconSpacing; set => Set(ref _rightIconSpacing, value); }

    /// <summary>Semicolon-separated decimal tray record ids forced into the top bar.</summary>
    public string TrayShow { get => _trayShow; set => Set(ref _trayShow, value); }

    /// <summary>Semicolon-separated decimal tray record ids forced into the hidden dropdown.</summary>
    public string TrayHide { get => _trayHide; set => Set(ref _trayHide, value); }

    public static AppSettings CreateDefault() => new();

    /// <summary>
    /// Reads a flat camelCase JSON object leniently: a missing, mistyped or out-of-range key
    /// falls back to its documented default instead of failing the whole load.
    /// </summary>
    public static AppSettings FromJson(JsonObject? root)
    {
        var s = new AppSettings();
        if (root is null) return s;

        s._version = GetInt(root, "version", CurrentVersion);
        if (GetString(root, "language") is { } language) s._language = language;
        if (GetString(root, "theme") is { } theme) s._theme = theme;
        s._barHeight = GetDouble(root, "barHeight", s._barHeight);
        s._monitor = GetInt(root, "monitor", s._monitor);
        s._reserveWorkArea = GetBool(root, "reserveWorkArea", s._reserveWorkArea);
        s._autoHide = GetBool(root, "autoHide", s._autoHide);
        s._hideInFullscreen = GetBool(root, "hideInFullscreen", s._hideInFullscreen);
        s._showSeconds = GetBool(root, "showSeconds", s._showSeconds);
        s._use24Hour = GetBool(root, "use24Hour", s._use24Hour);
        s._showDate = GetBool(root, "showDate", s._showDate);
        s._showAppName = GetBool(root, "showAppName", s._showAppName);
        s._showAppleMenu = GetBool(root, "showAppleMenu", s._showAppleMenu);
        if (GetString(root, "logoStyle") is { } logoStyle) s._logoStyle = logoStyle;
        s._showWifi = GetBool(root, "showWifi", s._showWifi);
        s._showBattery = GetBool(root, "showBattery", s._showBattery);
        s._showBatteryPercent = GetBool(root, "showBatteryPercent", s._showBatteryPercent);
        s._showVolume = GetBool(root, "showVolume", s._showVolume);
        s._showControlCenter = GetBool(root, "showControlCenter", s._showControlCenter);
        s._showSearch = GetBool(root, "showSearch", s._showSearch);
        s._showNotifications = GetBool(root, "showNotifications", s._showNotifications);
        s._rightIconSpacing = GetInt(root, "rightIconSpacing", s._rightIconSpacing);
        s._fontScale = GetDouble(root, "fontScale", s._fontScale);
        if (GetString(root, "glassPreset") is { } preset) s._glassPreset = preset;
        s._transparency = GetDouble(root, "transparency", s._transparency);
        s._blur = GetDouble(root, "blur", s._blur);
        s._refraction = GetDouble(root, "refraction", s._refraction);
        s._edge = GetDouble(root, "edge", s._edge);
        s._dispersion = GetDouble(root, "dispersion", s._dispersion);
        s._highlight = GetDouble(root, "highlight", s._highlight);
        s._shadow = GetDouble(root, "shadow", s._shadow);
        if (GetString(root, "tint") is { } tint) s._tint = tint;
        s._noise = GetDouble(root, "noise", s._noise);
        s._saturation = GetDouble(root, "saturation", s._saturation);
        s._launchAtLogin = GetBool(root, "launchAtLogin", s._launchAtLogin);
        s._menuAnimations = GetBool(root, "menuAnimations", s._menuAnimations);
        if (GetString(root, "trayShow") is { } trayShow) s._trayShow = trayShow;
        if (GetString(root, "trayHide") is { } trayHide) s._trayHide = trayHide;

        s.Normalize();
        return s;
    }

    /// <summary>Serializes the documented keys in schema order, then re-appends any preserved unknown keys.</summary>
    public JsonObject ToJson(JsonObject? preservedUnknownKeys = null)
    {
        Normalize();
        var root = new JsonObject
        {
            ["version"] = Version,
            ["language"] = Language,
            ["theme"] = Theme,
            ["barHeight"] = Round(BarHeight),
            ["monitor"] = Monitor,
            ["reserveWorkArea"] = ReserveWorkArea,
            ["autoHide"] = AutoHide,
            ["hideInFullscreen"] = HideInFullscreen,
            ["showSeconds"] = ShowSeconds,
            ["use24Hour"] = Use24Hour,
            ["showDate"] = ShowDate,
            ["showAppName"] = ShowAppName,
            ["showAppleMenu"] = ShowAppleMenu,
            ["logoStyle"] = LogoStyle,
            ["showWifi"] = ShowWifi,
            ["showBattery"] = ShowBattery,
            ["showBatteryPercent"] = ShowBatteryPercent,
            ["showVolume"] = ShowVolume,
            ["showControlCenter"] = ShowControlCenter,
            ["showSearch"] = ShowSearch,
            ["showNotifications"] = ShowNotifications,
            ["rightIconSpacing"] = RightIconSpacing,
            ["fontScale"] = Round(FontScale),
            ["glassPreset"] = GlassPreset,
            ["transparency"] = Round(Transparency),
            ["blur"] = Round(Blur),
            ["refraction"] = Round(Refraction),
            ["edge"] = Round(Edge),
            ["dispersion"] = Round(Dispersion),
            ["highlight"] = Round(Highlight),
            ["shadow"] = Round(Shadow),
            ["tint"] = Tint,
            ["noise"] = Round(Noise),
            ["saturation"] = Round(Saturation),
            ["launchAtLogin"] = LaunchAtLogin,
            ["menuAnimations"] = MenuAnimations,
            ["trayShow"] = TrayShow,
            ["trayHide"] = TrayHide,
        };

        if (preservedUnknownKeys is not null)
        {
            foreach (var pair in preservedUnknownKeys)
            {
                if (!KnownKeys.Contains(pair.Key) && pair.Value is not null)
                    root[pair.Key] = pair.Value.DeepClone();
            }
        }

        return root;
    }

    /// <summary>Clamps every value into the documented range and repairs mistyped enums/colours.</summary>
    public void Normalize()
    {
        Version = CurrentVersion;
        if (!Languages.Contains(Language)) Language = "zh-CN";
        if (!Themes.Contains(Theme)) Theme = "auto";
        if (!LogoStyles.Contains(LogoStyle)) LogoStyle = "apple";
        BarHeight = Math.Clamp(BarHeight, 24, 44);
        Monitor = Math.Clamp(Monitor, 0, 15);
        FontScale = Math.Clamp(FontScale, 0.85, 1.25);
        if (!GlassPresets.Contains(GlassPreset)) GlassPreset = "liquid";
        Transparency = Math.Clamp(Transparency, 0, 1);
        Blur = Math.Clamp(Blur, 0, 1);
        Refraction = Math.Clamp(Refraction, 0, 40);
        Edge = Math.Clamp(Edge, 0, 32);
        Dispersion = Math.Clamp(Dispersion, 0, 1);
        Highlight = Math.Clamp(Highlight, 0, 1.5);
        Shadow = Math.Clamp(Shadow, 0, 1);
        Noise = Math.Clamp(Noise, 0, 0.1);
        Saturation = Math.Clamp(Saturation, 0.5, 2);
        RightIconSpacing = Math.Clamp(RightIconSpacing, 2, 24);
        TrayShow = NormalizeTrayIds(TrayShow);
        TrayHide = NormalizeTrayIds(TrayHide);
        if (!TintPattern.IsMatch(Tint)) Tint = "#FFFFFF";
        Tint = Tint.ToUpperInvariant();
    }

    /// <summary>Restores every control to the documented defaults (the caller decides when to save).</summary>
    public void ResetToDefaults()
    {
        var defaults = new AppSettings();
        foreach (var property in typeof(AppSettings).GetProperties())
        {
            if (!property.CanRead || !property.CanWrite) continue;
            if (property.Name is nameof(Version)) continue;
            var target = property.GetValue(defaults);
            if (!Equals(property.GetValue(this), target))
                property.SetValue(this, target);
        }

        Normalize();
    }

    /// <summary>Applies one of the three documented glass presets to the manual parameters.</summary>
    public void ApplyGlassPreset(string preset)
    {
        switch (preset)
        {
            case "frosted":
                GlassPreset = "frosted";
                Transparency = 0.82;
                Blur = 0.92;
                Refraction = 6;
                Edge = 12;
                Dispersion = 0.12;
                Highlight = 0.55;
                Shadow = 0.30;
                Saturation = 1.05;
                break;
            case "clear":
                GlassPreset = "clear";
                Transparency = 0.99;
                Blur = 0.10;
                Refraction = 26;
                Edge = 22;
                Dispersion = 0.55;
                Highlight = 1.10;
                Shadow = 0.22;
                Saturation = 1.30;
                break;
            default:
                GlassPreset = "liquid";
                Transparency = 0.94;
                Blur = 0.52;
                Refraction = 18;
                Edge = 18;
                Dispersion = 0.30;
                Highlight = 0.80;
                Shadow = 0.35;
                Saturation = 1.15;
                break;
        }
    }

    // =====================================================================================
    // Tray icon overrides
    // =====================================================================================

    /// <summary>
    /// Parses the documented semicolon-separated decimal id list. Invalid tokens are ignored,
    /// duplicates are removed and the result is sorted numerically (deterministic JSON).
    /// </summary>
    public static IReadOnlyList<ulong> ParseTrayIds(string? value)
    {
        if (string.IsNullOrWhiteSpace(value)) return Array.Empty<ulong>();

        var ids = new SortedSet<ulong>();
        foreach (var token in value.Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries))
        {
            if (ulong.TryParse(token, NumberStyles.None, CultureInfo.InvariantCulture, out var id) && id > 0)
                ids.Add(id);
        }

        return ids.ToArray();
    }

    /// <summary>Formats record ids as the documented semicolon-separated string, sorted numerically.</summary>
    public static string FormatTrayIds(IEnumerable<ulong>? ids)
    {
        if (ids is null) return string.Empty;
        return string.Join(
            ";",
            ids.Where(id => id > 0)
                .Distinct()
                .OrderBy(id => id)
                .Select(id => id.ToString(CultureInfo.InvariantCulture)));
    }

    private static string NormalizeTrayIds(string? value) => FormatTrayIds(ParseTrayIds(value));

    /// <summary>
    /// Effective tray visibility, shared with the native bar: <c>trayHide</c> wins, then
    /// <c>trayShow</c>, otherwise Windows' own <c>IsPromoted</c> value is inherited.
    /// </summary>
    public bool IsTrayIconShown(ulong recordId, bool platformPromoted)
    {
        if (recordId == 0) return platformPromoted;
        if (ParseTrayIds(TrayHide).Contains(recordId)) return false;
        if (ParseTrayIds(TrayShow).Contains(recordId)) return true;
        return platformPromoted;
    }

    /// <summary>
    /// Applies the documented override rule for a single icon: inheriting when the desired state
    /// matches Windows, otherwise forcing it through <c>trayShow</c> / <c>trayHide</c>.
    /// Both lists stay sorted, deduplicated and mutually exclusive.
    /// </summary>
    public void SetTrayIconShown(ulong recordId, bool desired, bool platformPromoted)
    {
        if (recordId == 0) return;

        var show = new HashSet<ulong>(ParseTrayIds(TrayShow));
        var hide = new HashSet<ulong>(ParseTrayIds(TrayHide));

        if (desired == platformPromoted)
        {
            show.Remove(recordId);
            hide.Remove(recordId);
        }
        else if (desired)
        {
            show.Add(recordId);
            hide.Remove(recordId);
        }
        else
        {
            hide.Add(recordId);
            show.Remove(recordId);
        }

        TrayShow = FormatTrayIds(show);
        TrayHide = FormatTrayIds(hide);
    }

    /// <summary>Clears both override lists so every icon inherits Windows' IsPromoted again.</summary>
    public void ClearTrayOverrides()
    {
        TrayShow = string.Empty;
        TrayHide = string.Empty;
    }

    private bool Set<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value)) return false;
        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
        Changed?.Invoke(this, EventArgs.Empty);
        return true;
    }

    private static double Round(double value) => Math.Round(value, 4, MidpointRounding.AwayFromZero);

    private static bool TryGetValue(JsonObject root, string key, out JsonValue value)
    {
        value = null!;
        return root.TryGetPropertyValue(key, out var node) && node is JsonValue jsonValue && (value = jsonValue) is not null;
    }

    private static string? GetString(JsonObject root, string key)
    {
        if (!TryGetValue(root, key, out var value)) return null;
        return value.TryGetValue<string>(out var text) ? text : null;
    }

    private static double GetDouble(JsonObject root, string key, double fallback)
    {
        if (!TryGetValue(root, key, out var value)) return fallback;
        if (value.TryGetValue<double>(out var d)) return double.IsFinite(d) ? d : fallback;
        if (value.TryGetValue<long>(out var l)) return l;
        if (value.TryGetValue<string>(out var text) &&
            double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out var parsed) &&
            double.IsFinite(parsed))
            return parsed;
        return fallback;
    }

    private static bool GetBool(JsonObject root, string key, bool fallback)
    {
        if (!TryGetValue(root, key, out var value)) return fallback;
        return value.TryGetValue<bool>(out var b) ? b : fallback;
    }

    private static int GetInt(JsonObject root, string key, int fallback)
    {
        var d = GetDouble(root, key, fallback);
        if (!double.IsFinite(d)) return fallback;
        return (int)Math.Clamp(Math.Round(d), int.MinValue, int.MaxValue);
    }
}
