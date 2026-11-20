using System.ComponentModel;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Windows.Media.Imaging;
using SuperMenu.Settings.Services;

namespace SuperMenu.Settings.Models;

/// <summary>
/// One row of the 系统托盘 list: a catalog record plus the user's desired show/hide state.
/// Toggling writes straight into <see cref="AppSettings.TrayShow"/> / <see cref="AppSettings.TrayHide"/>
/// through the callback the window supplies, so the existing dirty indicator and 应用 button work.
/// </summary>
public sealed class TrayIconRow : INotifyPropertyChanged
{
    private readonly Action<TrayIconRow, bool> _onDesiredChanged;
    private bool _shown;
    private bool _suppressCallback;

    public TrayIconRow(TrayRecord record, Action<TrayIconRow, bool> onDesiredChanged)
    {
        Record = record;
        _onDesiredChanged = onDesiredChanged;
    }

    public TrayRecord Record { get; }

    public ulong RecordId => Record.RecordId;

    public string Label => Record.Label;

    public string Exe => Record.Exe;

    public bool PlatformPromoted => Record.PlatformPromoted;

    public BitmapSource? Icon => Record.Icon;

    public string IconFallbackText
    {
        get
        {
            if (string.IsNullOrEmpty(Record.Label)) return "?";
            return StringInfo.GetNextTextElement(Record.Label, 0).ToUpperInvariant();
        }
    }

    /// <summary>Stable automation id so the UI test can enumerate the tray toggles.</summary>
    public string AutomationId => "TrayToggle-" + Record.RecordId.ToString(CultureInfo.InvariantCulture);

    public string ToolTip => string.IsNullOrWhiteSpace(Record.Exe)
        ? $"{Record.Label}（记录 {RecordId}）"
        : $"{Record.Label}\n{Record.Exe}\n记录 {RecordId}";

    /// <summary>Desired effective visibility (two-way bound to the row's capsule toggle).</summary>
    public bool Shown
    {
        get => _shown;
        set
        {
            if (_shown == value) return;
            _shown = value;
            OnPropertyChanged();
            if (!_suppressCallback) _onDesiredChanged(this, value);
        }
    }

    public event PropertyChangedEventHandler? PropertyChanged;

    /// <summary>Updates the toggle without writing the override lists (load / refresh / reset sync).</summary>
    public void SetShownSilently(bool value)
    {
        if (_shown == value) return;
        _suppressCallback = true;
        try
        {
            _shown = value;
            OnPropertyChanged(nameof(Shown));
        }
        finally
        {
            _suppressCallback = false;
        }
    }

    private void OnPropertyChanged([CallerMemberName] string? propertyName = null)
        => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
}
