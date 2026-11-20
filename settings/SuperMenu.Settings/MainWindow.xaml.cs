using System.Collections.ObjectModel;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using SuperMenu.Settings.Models;
using SuperMenu.Settings.Services;

namespace SuperMenu.Settings;

/// <summary>
/// macOS-inspired settings window. The single <see cref="AppSettings"/> instance doubles as the
/// binding context: every control writes straight into it, the live preview re-reads it on change,
/// and "应用" persists + broadcasts the reload message to the native bar.
/// </summary>
public partial class MainWindow : Window
{
    private const double ShellMargin = 18;
    private const double NormalCornerRadius = 22;
    private const double TitleBarHeight = 52;

    private const int WmNcHitTest = 0x0084;
    private const int WmGetMinMaxInfo = 0x0024;
    private const int HtClient = 1;
    private const int HtCaption = 2;
    private const int HtLeft = 10;
    private const int HtRight = 11;
    private const int HtTop = 12;
    private const int HtTopLeft = 13;
    private const int HtTopRight = 14;
    private const int HtBottom = 15;
    private const int HtBottomLeft = 16;
    private const int HtBottomRight = 17;

    private static readonly Dictionary<string, (string Title, string Subtitle)> PageInfo = new(StringComparer.Ordinal)
    {
        ["appearance"] = ("外观", "调整菜单栏的高度、文字与整体行为"),
        ["glass"] = ("液态玻璃", "调节实时折射玻璃的材质参数，上方预览会即时响应"),
        ["menubar"] = ("菜单栏", "选择菜单栏两侧显示的内容与时钟格式"),
        ["about"] = ("关于", "版本、运行状态与致谢"),
    };

    private static readonly string[] MenuLabelsChinese = ["文件", "编辑", "显示", "窗口", "帮助"];

    private static readonly string[] MenuLabelsEnglish = ["File", "Edit", "View", "Window", "Help"];

    private static readonly string[] TintPresets =
    [
        "#FFFFFF", "#DDEBFF", "#FFE2B8", "#D2F6EE", "#2A2D36",
    ];

    private readonly SettingsStore _store;
    private readonly AppSettings _settings;
    private readonly JsonObject? _unknownKeys;
    private readonly List<Border> _menuHighlights = [];
    private readonly List<TextBlock> _menuTexts = [];
    private readonly List<RadioButton> _tintSwatches = [];
    private readonly ObservableCollection<TrayIconRow> _trayRows = [];

    private readonly DispatcherTimer _clockTimer;
    private readonly DispatcherTimer _statusTimer;
    private readonly DispatcherTimer _hoverTimer;
    private readonly DispatcherTimer _toastTimer;

    private string? _nativeBarPath;
    private string? _loadError;
    private DateTimeOffset? _lastSavedAt;
    private string _currentPage = "appearance";
    private TrayCatalog? _trayCatalog;
    private bool _ready;
    private bool _dirty;
    private bool _updatingTintUi;
    private bool _suppressPresetApply;
    private bool _syncingTrayRows;
    private bool _systemLightTheme;
    private int _hoverIndex;
    private HwndSource? _hwndSource;

    private void LoadEmbeddedIconFont()
    {
        try
        {
            var fontPath = System.IO.Path.Combine(AppContext.BaseDirectory, "Assets", "Fonts", "SegoeIcons.ttf");
            if (System.IO.File.Exists(fontPath))
            {
                var families = Fonts.GetFontFamilies(fontPath);
                var family = families.FirstOrDefault(f => f.Source.Contains("Segoe Fluent Icons", StringComparison.OrdinalIgnoreCase))
                             ?? families.FirstOrDefault();
                if (family is not null)
                {
                    Resources["Font.Icons"] = family;
                    return;
                }
            }
        }
        catch
        {
            // Fall through to the installed face; the settings UI remains usable.
        }
        Resources["Font.Icons"] = new FontFamily("Segoe Fluent Icons");
    }

    /// <summary>Rows of the 系统托盘 list (icon + label + show/hide toggle).</summary>
    public ObservableCollection<TrayIconRow> TrayRows => _trayRows;

    public MainWindow(SettingsStore store, SettingsLoadResult load, string? requestedPage)
    {
        InitializeComponent();
        LoadEmbeddedIconFont();

        _store = store;
        _settings = load.Settings;
        _unknownKeys = load.UnknownKeys;
        _loadError = load.Error;
        _nativeBarPath = NativeBridge.FindNativeBar();
        _systemLightTheme = AppAssets.IsSystemLightTheme();

        DataContext = _settings;
        _settings.Changed += Settings_Changed;
        _settings.PropertyChanged += Settings_PropertyChanged;

        _menuHighlights.AddRange([MenuHover0, MenuHover1, MenuHover2, MenuHover3, MenuHover4]);
        _menuTexts.AddRange([MenuText0, MenuText1, MenuText2, MenuText3, MenuText4]);

        _clockTimer = new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => UpdateClock(), Dispatcher);
        _statusTimer = new DispatcherTimer(TimeSpan.FromSeconds(3), DispatcherPriority.Background, (_, _) => UpdateNativeStatus(), Dispatcher);
        _hoverTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(2100), DispatcherPriority.Background, (_, _) => AnimateMenuHover(), Dispatcher);
        _toastTimer = new DispatcherTimer(DispatcherPriority.Normal, Dispatcher);
        _toastTimer.Tick += (_, _) =>
        {
            _toastTimer.Stop();
            HideToast();
        };

        BuildTintSwatches();
        TrayIconList.ItemsSource = _trayRows;
        LoadTrayIcons();
        WindowShell.SizeChanged += (_, _) => UpdateShellClip();
        VersionText.Text = $"版本 {CurrentVersionText}";
        AboutVersionText.Text = $"版本 {CurrentVersionText} · .NET 10 WPF";
        AboutConfigPathText.Text = _store.Path;
        AboutConfigPathText.ToolTip = _store.Path;
        AboutNativePathText.Text = _nativeBarPath ?? "未找到 SuperMenuBar.exe（构建菜单栏后即可重启）";
        AboutNativePathText.ToolTip = _nativeBarPath ?? "请把设置中心放在 SuperMenuBar.exe 同级的 Settings 目录";
        AboutFontText.Text = AppAssets.FontsLoaded ? "HarmonyOS Sans SC（私有加载，未安装到系统）" : "Segoe UI Variable（未找到 HarmonyOS Sans）";
        RestartBarButton.Visibility = _nativeBarPath is null ? Visibility.Collapsed : Visibility.Visible;
        AboutRestartButton.Visibility = _nativeBarPath is null ? Visibility.Collapsed : Visibility.Visible;

        var initialPage = PageInfo.ContainsKey(requestedPage ?? string.Empty) ? requestedPage! : "appearance";
        ShowPage(initialPage, animate: false);
        SelectNavigation(initialPage);
    }

    private static string CurrentVersionText
    {
        get
        {
            var version = typeof(MainWindow).Assembly.GetName().Version;
            return version is null ? "1.0.0" : $"{version.Major}.{version.Minor}.{version.Build}";
        }
    }

    // =====================================================================================
    // Window lifecycle / chrome
    // =====================================================================================

    private void Window_Loaded(object sender, RoutedEventArgs e)
    {
        if (AppAssets.WindowWallpaperBrush is not null)
        {
            if (AppAssets.TopAnchoredWallpaperBrush is ImageBrush previewBrush)
                Resources["PreviewWallpaper"] = previewBrush;
            WindowBackdrop.Source = new BitmapImage(new Uri(AppAssets.WallpaperPackUri, UriKind.Absolute));
        }

        try
        {
            Icon = new BitmapImage(new Uri(AppAssets.IconPackUri, UriKind.Absolute));
        }
        catch
        {
            // A missing icon must never prevent the window from opening.
        }

        UpdateShellClip();
        UpdatePreview();
        UpdateTintUi();
        UpdateClock();
        UpdateNativeStatus();
        UpdateFooterStatus();
        _ready = true;

        _clockTimer.Start();
        _statusTimer.Start();
        _hoverTimer.Start();

        StartMovingLight();
        PlayWindowIntro();

        if (_loadError is not null)
            ShowToast(_loadError, success: false);
    }

    private void PlayWindowIntro()
    {
        WindowShell.Opacity = 0;
        ShellTranslate.Y = 10;
        ShadowHost.Opacity = 0;

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        WindowShell.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(190)) { EasingFunction = ease });
        ShellTranslate.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(10, 0, TimeSpan.FromMilliseconds(240)) { EasingFunction = ease });
        ShadowHost.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 0.75, TimeSpan.FromMilliseconds(260)) { EasingFunction = ease });
    }

    private void StartMovingLight()
    {
        var animation = new DoubleAnimation
        {
            From = -260,
            To = 1240,
            Duration = TimeSpan.FromSeconds(13),
            RepeatBehavior = RepeatBehavior.Forever,
        };
        MovingLightTransform.BeginAnimation(TranslateTransform.XProperty, animation);
    }

    private void Window_SourceInitialized(object? sender, EventArgs e)
    {
        _hwndSource = PresentationSource.FromVisual(this) as HwndSource;
        _hwndSource?.AddHook(WndProc);
    }

    private void Window_StateChanged(object? sender, EventArgs e)
    {
        var maximised = WindowState == WindowState.Maximized;
        OuterRoot.Margin = maximised ? new Thickness(0) : new Thickness(ShellMargin);
        ContentPanel.Width = maximised ? 1160 : double.NaN;
        ContentPanel.HorizontalAlignment = maximised ? HorizontalAlignment.Center : HorizontalAlignment.Stretch;
        WindowShell.CornerRadius = new CornerRadius(maximised ? 0 : NormalCornerRadius);
        ShadowHost.Visibility = maximised ? Visibility.Collapsed : Visibility.Visible;
        UpdateShellClip();
    }

    private void UpdateShellClip()
    {
        var width = WindowShell.ActualWidth;
        var height = WindowShell.ActualHeight;
        if (width <= 0 || height <= 0) return;

        ShellClip.Rect = new Rect(0, 0, width, height);
        var radius = WindowState == WindowState.Maximized ? 0 : NormalCornerRadius;
        ShellClip.RadiusX = radius;
        ShellClip.RadiusY = radius;
    }

    private void Window_Activated(object? sender, EventArgs e) => TrafficLightsPanel.Opacity = 1;

    private void Window_Deactivated(object? sender, EventArgs e) => TrafficLightsPanel.Opacity = 0.55;

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.S && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            ApplyButton_Click(sender, e);
            e.Handled = true;
        }
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    private void MinimiseButton_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void ZoomButton_Click(object sender, RoutedEventArgs e)
        => WindowState = WindowState == WindowState.Maximized ? WindowState.Normal : WindowState.Maximized;

    private IntPtr WndProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        switch (message)
        {
            case WmNcHitTest:
                handled = true;
                return new IntPtr(HitTestWindow(lParam));
            case WmGetMinMaxInfo:
                handled = true;
                AdjustMaximisedBounds(lParam);
                return IntPtr.Zero;
            default:
                return IntPtr.Zero;
        }
    }

    /// <summary>
    /// The window is borderless with an 18 DIP transparent margin used for the painted shadow, so
    /// resizing has to target the visible shell edge instead of the window rectangle.
    /// </summary>
    private int HitTestWindow(IntPtr lParam)
    {
        var value = lParam.ToInt64();
        var screenPoint = new Point((short)(value & 0xFFFF), (short)((value >> 16) & 0xFFFF));
        var point = PointFromScreen(screenPoint);
        var width = ActualWidth;
        var height = ActualHeight;

        if (WindowState == WindowState.Normal)
        {
            var margin = OuterRoot.Margin.Left;
            var edgeIn = 8.0;   // into the visible glass
            var edgeOut = 10.0; // into the transparent shadow margin

            var left = point.X >= margin - edgeOut && point.X <= margin + edgeIn;
            var right = point.X >= width - margin - edgeIn && point.X <= width - margin + edgeOut;
            var top = point.Y >= margin - edgeOut && point.Y <= margin + edgeIn;
            var bottom = point.Y >= height - margin - edgeIn && point.Y <= height - margin + edgeOut;

            if (top && left) return HtTopLeft;
            if (top && right) return HtTopRight;
            if (bottom && left) return HtBottomLeft;
            if (bottom && right) return HtBottomRight;
            if (left) return HtLeft;
            if (right) return HtRight;
            if (top) return HtTop;
            if (bottom) return HtBottom;
        }

        var titleTop = WindowState == WindowState.Maximized ? 0 : OuterRoot.Margin.Top;
        if (point.Y >= titleTop && point.Y < titleTop + TitleBarHeight)
        {
            // Leave the traffic-light corner to WPF so the buttons receive their clicks.
            var lightsLeft = (WindowState == WindowState.Maximized ? 0 : OuterRoot.Margin.Left) + 6;
            if (point.X < lightsLeft + 82) return HtClient;
            return HtCaption;
        }

        return HtClient;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct NativePoint
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MinMaxInfo
    {
        public NativePoint Reserved;
        public NativePoint MaxSize;
        public NativePoint MaxPosition;
        public NativePoint MinTrackSize;
        public NativePoint MaxTrackSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MonitorInfo
    {
        public int Size;
        public RectStruct Monitor;
        public RectStruct Work;
        public int Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct RectStruct
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    [DllImport("user32.dll")]
    private static extern IntPtr MonitorFromWindow(IntPtr hwnd, int flags);

    [DllImport("user32.dll")]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GetMonitorInfo(IntPtr monitor, ref MonitorInfo info);

    private void AdjustMaximisedBounds(IntPtr lParam)
    {
        try
        {
            var monitor = MonitorFromWindow(new WindowInteropHelper(this).Handle, 2 /* MONITOR_DEFAULTTONEAREST */);
            var info = new MonitorInfo { Size = Marshal.SizeOf<MonitorInfo>() };
            if (!GetMonitorInfo(monitor, ref info)) return;

            var minMax = Marshal.PtrToStructure<MinMaxInfo>(lParam);
            var work = info.Work;
            var monitorRect = info.Monitor;

            minMax.MaxPosition.X = work.Left - monitorRect.Left;
            minMax.MaxPosition.Y = work.Top - monitorRect.Top;
            minMax.MaxSize.X = work.Right - work.Left;
            minMax.MaxSize.Y = work.Bottom - work.Top;
            minMax.MaxTrackSize = minMax.MaxSize;

            var dpi = VisualTreeHelper.GetDpi(this).DpiScaleX;
            minMax.MinTrackSize.X = (int)(MinWidth * dpi);
            minMax.MinTrackSize.Y = (int)(MinHeight * dpi);

            Marshal.StructureToPtr(minMax, lParam, fDeleteOld: false);
        }
        catch
        {
            // Falling back to the default maximise behaviour is acceptable.
        }
    }

    // =====================================================================================
    // Navigation
    // =====================================================================================

    private void NavItem_Checked(object sender, RoutedEventArgs e)
    {
        if (sender is RadioButton { Tag: string page })
            ShowPage(page, animate: _ready);
    }

    private void SelectNavigation(string page)
    {
        var target = page switch
        {
            "glass" => NavGlass,
            "menubar" => NavMenubar,
            "about" => NavAbout,
            _ => NavAppearance,
        };
        if (target.IsChecked != true) target.IsChecked = true;
    }

    private void ShowPage(string page, bool animate)
    {
        _currentPage = PageInfo.ContainsKey(page) ? page : "appearance";
        var active = GetPageElement(_currentPage);
        var pages = new FrameworkElement[] { PageAppearance, PageGlass, PageMenubar, PageAbout };
        foreach (var element in pages)
        {
            element.Visibility = ReferenceEquals(element, active) ? Visibility.Visible : Visibility.Collapsed;
            element.BeginAnimation(OpacityProperty, null);
            if (element.RenderTransform is TranslateTransform)
                element.RenderTransform = Transform.Identity;
        }

        PreviewCard.Visibility = _currentPage == "about" ? Visibility.Collapsed : Visibility.Visible;

        var (title, subtitle) = PageInfo[_currentPage];
        PageTitleText.Text = title;
        PageSubtitleText.Text = subtitle;

        if (animate)
        {
            var offset = new TranslateTransform(0, 7);
            active.RenderTransform = offset;
            active.BeginAnimation(OpacityProperty, new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(180))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            });
            offset.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(7, 0, TimeSpan.FromMilliseconds(220))
            {
                EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
            });
        }

        if (PageScroll.VerticalOffset > 0) PageScroll.ScrollToTop();
        UpdateNativeStatus();
    }

    private FrameworkElement GetPageElement(string page) => page switch
    {
        "glass" => PageGlass,
        "menubar" => PageMenubar,
        "about" => PageAbout,
        _ => PageAppearance,
    };

    // =====================================================================================
    // Settings → UI
    // =====================================================================================

    private void Settings_Changed(object? sender, EventArgs e)
    {
        UpdatePreview();
        UpdateTintUi();
        if (!_ready) return;

        _dirty = true;
        UpdateFooterStatus();
    }

    private void Settings_PropertyChanged(object? sender, System.ComponentModel.PropertyChangedEventArgs e)
    {
        if (e.PropertyName == nameof(AppSettings.GlassPreset) && !_suppressPresetApply)
        {
            _suppressPresetApply = true;
            try
            {
                _settings.ApplyGlassPreset(_settings.GlassPreset);
            }
            finally
            {
                _suppressPresetApply = false;
            }
        }

        // 恢复默认 / 应用后的重新读取等外部改动同步回托盘开关，不重复写覆盖列表。
        if (e.PropertyName is nameof(AppSettings.TrayShow) or nameof(AppSettings.TrayHide))
            SyncTrayRowsFromSettings();
    }

    private void UpdatePreview()
    {
        if (!IsInitialized) return;

        var settings = _settings;
        var displayBarHeight = Math.Clamp(settings.BarHeight * 1.55, 34, 74);
        PreviewBarHost.Height = displayBarHeight;

        BarBlurEffect.Radius = 1.5 + settings.Blur * 16.0;
        var backdropScale = 1.02 + settings.Refraction / 40.0 * 0.06;
        BarBackdropScale.ScaleX = backdropScale;
        BarBackdropScale.ScaleY = backdropScale;

        // Theme wash + tint, composited to one translucent colour (wash on top of tint).
        var tint = ParseHexColor(settings.Tint);
        var tintAlpha = (1 - settings.Transparency) * 0.9;
        var light = settings.Theme == "light" || (settings.Theme == "auto" && _systemLightTheme);
        var wash = light ? Color.FromRgb(0xF3, 0xF5, 0xFA) : Color.FromRgb(0x07, 0x09, 0x0E);
        var washAlpha = light ? 0.55 : 0.14;

        var combinedAlpha = 1 - (1 - washAlpha) * (1 - tintAlpha);
        byte Mix(byte washChannel, byte tintChannel) =>
            (byte)Math.Clamp((washChannel * washAlpha + tintChannel * tintAlpha * (1 - washAlpha)) / combinedAlpha, 0, 255);

        var glassColor = Color.FromArgb((byte)Math.Clamp(combinedAlpha * 255, 0, 255), Mix(wash.R, tint.R), Mix(wash.G, tint.G), Mix(wash.B, tint.B));
        BarTintLayer.Background = new SolidColorBrush(glassColor);

        // Ink adapts to the composited backdrop: dark ink only on a genuinely bright bar.
        var glassLuminance = Luminance(glassColor);
        var effectiveLuminance = glassLuminance * combinedAlpha + AppAssets.PreviewBandLuminance * (1 - combinedAlpha) + 0.03;
        var ink = effectiveLuminance > 0.56 ? Color.FromRgb(0x1D, 0x1D, 0x1F) : Color.FromRgb(0xFF, 0xFF, 0xFF);
        Resources["PreviewInk"] = new SolidColorBrush(ink);
        Resources["PreviewChip"] = new SolidColorBrush(Color.FromArgb(0x20, ink.R, ink.G, ink.B));
        BarTextShadow.Opacity = effectiveLuminance > 0.56 ? 0 : 0.28;

        // Rim, sheen and dispersion respond to the manual parameters.
        BarSheen.Opacity = Math.Clamp(0.08 + settings.Highlight * 0.30, 0, 0.55);
        BarRimTop.Height = 0.6 + settings.Highlight * 0.8;
        BarRimTop.Opacity = Math.Clamp(0.12 + settings.Highlight * 0.32, 0, 0.9);
        BarRimBottom.Height = 0.6 + settings.Highlight * 0.9;
        BarRimBottom.Opacity = Math.Clamp(0.08 + settings.Highlight * 0.28, 0, 0.9);
        BarDispersionWarm.Opacity = Math.Clamp(settings.Dispersion * 0.55 * settings.Highlight, 0, 0.7);
        BarDispersionCool.Opacity = Math.Clamp(settings.Dispersion * 0.45 * settings.Highlight, 0, 0.7);

        BarShadowEffect.Opacity = Math.Clamp(settings.Shadow * 0.9, 0, 0.95);
        BarShadowEffect.BlurRadius = 10 + settings.Shadow * 24;
        BarShadowEffect.ShadowDepth = 3 + settings.Shadow * 9;
        MovingLight.Opacity = Math.Clamp(0.10 + settings.Highlight * 0.22, 0, 0.45);

        // Typography.
        var menuFont = Math.Clamp(12.5 * settings.FontScale, 10.5, 17);
        foreach (var text in _menuTexts) text.FontSize = menuFont;
        PreviewAppName.FontSize = menuFont + 1.1;
        ClockText.FontSize = menuFont;
        BatteryPercentText.FontSize = Math.Max(9.5, menuFont - 1.5);

        // Modules. showAppleMenu gates the whole leftmost button; logoStyle only picks its glyph.
        var windowsLogo = string.Equals(settings.LogoStyle, "windows", StringComparison.OrdinalIgnoreCase);
        var monoWindowsLogo = string.Equals(settings.LogoStyle, "windows-mono", StringComparison.OrdinalIgnoreCase);
        AppleMark.Visibility = settings.ShowAppleMenu && !windowsLogo && !monoWindowsLogo
            ? Visibility.Visible
            : Visibility.Collapsed;
        WindowsMark.Visibility = settings.ShowAppleMenu && (windowsLogo || monoWindowsLogo)
            ? Visibility.Visible
            : Visibility.Collapsed;
        WindowsMarkPath.Fill = windowsLogo
            ? new SolidColorBrush(Color.FromRgb(0x00, 0x78, 0xD4))
            : (Brush)FindResource("PreviewInk");
        PreviewAppName.Visibility = settings.ShowAppName ? Visibility.Visible : Visibility.Collapsed;

        // Menu language.
        var labels = string.Equals(settings.Language, "en-US", StringComparison.OrdinalIgnoreCase)
            ? MenuLabelsEnglish
            : MenuLabelsChinese;
        for (var i = 0; i < _menuTexts.Count; i++) _menuTexts[i].Text = labels[i];
        PreviewAppName.Text = string.Equals(settings.Language, "en-US", StringComparison.OrdinalIgnoreCase) ? "Finder" : "访达";

        UpdateClock();
    }

    private void UpdateClock()
    {
        var now = DateTime.Now;
        var english = string.Equals(_settings.Language, "en-US", StringComparison.OrdinalIgnoreCase);
        string text;
        if (english)
        {
            var date = _settings.ShowDate ? now.ToString("MMM d, ddd ", CultureInfo.GetCultureInfo("en-US")) : string.Empty;
            var time = _settings.Use24Hour
                ? now.ToString(_settings.ShowSeconds ? "HH:mm:ss" : "HH:mm", CultureInfo.InvariantCulture)
                : now.ToString(_settings.ShowSeconds ? "h:mm:ss tt" : "h:mm tt", CultureInfo.GetCultureInfo("en-US"));
            text = date + time;
        }
        else
        {
            var date = _settings.ShowDate
                ? $"{now.Month}月{now.Day}日 {CultureInfo.GetCultureInfo("zh-CN").DateTimeFormat.GetAbbreviatedDayName(now.DayOfWeek)} "
                : string.Empty;
            string time;
            if (_settings.Use24Hour)
            {
                time = now.ToString(_settings.ShowSeconds ? "HH:mm:ss" : "HH:mm", CultureInfo.InvariantCulture);
            }
            else
            {
                var hour = now.Hour % 12;
                if (hour == 0) hour = 12;
                var suffix = _settings.ShowSeconds ? $":{now.Second:00}" : string.Empty;
                time = $"{(now.Hour < 12 ? "上午" : "下午")}{hour}:{now.Minute:00}{suffix}";
            }

            text = date + time;
        }

        ClockText.Text = text;
    }

    private void AnimateMenuHover()
    {
        if (!_settings.MenuAnimations)
        {
            foreach (var menuHighlight in _menuHighlights) menuHighlight.BeginAnimation(OpacityProperty, null);
            return;
        }

        if (!IsActive || PreviewCard.Visibility != Visibility.Visible) return;

        var highlight = _menuHighlights[_hoverIndex];
        _hoverIndex = (_hoverIndex + 1) % _menuHighlights.Count;

        var animation = new DoubleAnimationUsingKeyFrames();
        animation.KeyFrames.Add(new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)));
        animation.KeyFrames.Add(new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(140)))
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseOut },
        });
        animation.KeyFrames.Add(new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(720))));
        animation.KeyFrames.Add(new EasingDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(1180))));

        highlight.BeginAnimation(OpacityProperty, animation);
    }

    // =====================================================================================
    // Tint colour
    // =====================================================================================

    private void BuildTintSwatches()
    {
        var group = "TintSwatch" + Guid.NewGuid().ToString("N");
        foreach (var hex in TintPresets)
        {
            var swatch = new RadioButton
            {
                Style = (Style)FindResource("Style.TintSwatch"),
                Background = new SolidColorBrush(ParseHexColor(hex)),
                Tag = hex,
                GroupName = group,
            };
            AutomationProperties.SetName(swatch, $"玻璃色调 {hex}");
            swatch.Checked += (_, _) =>
            {
                if (_updatingTintUi || swatch.Tag is not string value) return;
                _settings.Tint = value;
            };
            _tintSwatches.Add(swatch);
            TintSwatches.Children.Add(swatch);
        }
    }

    private void UpdateTintUi()
    {
        _updatingTintUi = true;
        try
        {
            if (!TintHexBox.IsKeyboardFocused) TintHexBox.Text = _settings.Tint;
            TintHexBox.Foreground = (Brush)FindResource("Brush.Text.Primary");
            foreach (var swatch in _tintSwatches)
                swatch.IsChecked = string.Equals(swatch.Tag as string, _settings.Tint, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            _updatingTintUi = false;
        }
    }

    private void TintHexBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        if (_updatingTintUi) return;

        var text = TintHexBox.Text.Trim();
        if (!text.StartsWith('#')) text = "#" + text;
        if (text.Length == 7 && IsHexColour(text))
        {
            TintHexBox.Foreground = (Brush)FindResource("Brush.Text.Primary");
            if (!string.Equals(_settings.Tint, text, StringComparison.OrdinalIgnoreCase))
                _settings.Tint = text.ToUpperInvariant();
        }
        else
        {
            TintHexBox.Foreground = (Brush)FindResource("Brush.Danger");
        }
    }

    private static bool IsHexColour(string value)
    {
        if (value.Length != 7 || value[0] != '#') return false;
        for (var i = 1; i < value.Length; i++)
        {
            if (!Uri.IsHexDigit(value[i])) return false;
        }

        return true;
    }

    private static Color ParseHexColor(string value)
    {
        try
        {
            return (Color)ColorConverter.ConvertFromString(value);
        }
        catch
        {
            return Colors.White;
        }
    }

    private static double Luminance(Color color)
        => (0.2126 * color.R + 0.7152 * color.G + 0.0722 * color.B) / 255.0;

    // =====================================================================================
    // 系统托盘 icon customisation
    // =====================================================================================

    /// <summary>
    /// Rebuilds the tray list from <c>HKCU\Control Panel\NotifyIconSettings</c> (read-only).
    /// The effective toggle state follows: trayHide, then trayShow, then Windows IsPromoted.
    /// </summary>
    private void LoadTrayIcons()
    {
        _trayCatalog = TrayCatalog.Load();

        _syncingTrayRows = true;
        try
        {
            _trayRows.Clear();
            foreach (var record in _trayCatalog.Records)
            {
                var row = new TrayIconRow(record, OnTrayIconToggled);
                row.SetShownSilently(_settings.IsTrayIconShown(record.RecordId, record.PlatformPromoted));
                _trayRows.Add(row);
            }
        }
        finally
        {
            _syncingTrayRows = false;
        }

        UpdateTraySummary();
    }

    private void OnTrayIconToggled(TrayIconRow row, bool desired)
    {
        if (_syncingTrayRows) return;
        _settings.SetTrayIconShown(row.RecordId, desired, row.PlatformPromoted);
        UpdateTraySummary();
    }

    /// <summary>Re-reads trayShow/trayHide (e.g. after 恢复默认) without writing them back.</summary>
    private void SyncTrayRowsFromSettings()
    {
        if (_trayRows.Count == 0) return;

        _syncingTrayRows = true;
        try
        {
            foreach (var row in _trayRows)
                row.SetShownSilently(_settings.IsTrayIconShown(row.RecordId, row.PlatformPromoted));
        }
        finally
        {
            _syncingTrayRows = false;
        }

        UpdateTraySummary();
    }

    private void UpdateTraySummary()
    {
        if (TraySummaryText is null) return;

        var shown = _trayRows.Count(row => row.Shown);
        var overrides = AppSettings.ParseTrayIds(_settings.TrayShow).Count + AppSettings.ParseTrayIds(_settings.TrayHide).Count;
        var summary = $"共 {_trayRows.Count} 个图标 · 顶栏显示 {shown} · 隐藏面板 {_trayRows.Count - shown}";
        summary += _trayCatalog?.LiveFilterApplied == true
            ? " · 仅显示当前运行图标"
            : " · 未取得菜单栏存活快照，显示历史记录";
        if (overrides > 0) summary += $" · 已覆盖 {overrides} 项";
        if (_trayCatalog?.Error is { } error) summary += $" · 读取失败：{error}";
        TraySummaryText.Text = summary;
    }

    private void RefreshTrayButton_Click(object sender, RoutedEventArgs e)
    {
        LoadTrayIcons();
        if (_trayCatalog?.Error is { } error)
        {
            ShowToast($"刷新托盘图标失败：{error}", success: false);
            return;
        }

        ShowToast($"已刷新托盘图标，共 {_trayRows.Count} 个");
    }

    private void ResetTrayButton_Click(object sender, RoutedEventArgs e)
    {
        _settings.ClearTrayOverrides();
        SyncTrayRowsFromSettings();
        UpdateFooterStatus();
        ShowToast("已恢复 Windows 默认的托盘显示，点击“应用”写入");
    }

    // =====================================================================================
    // Actions
    // =====================================================================================

    private void ApplyButton_Click(object sender, RoutedEventArgs e)
    {
        var result = _store.Save(_settings, _unknownKeys);
        if (result.Success)
        {
            _dirty = false;
            _loadError = null;
            _lastSavedAt = result.SavedAt;
            UpdateFooterStatus();
            ShowToast("已应用设置，菜单栏已收到刷新通知");
            UpdateNativeStatus();
        }
        else
        {
            ShowToast($"保存失败：{result.Error}", success: false);
        }
    }

    private void ResetButton_Click(object sender, RoutedEventArgs e)
    {
        _suppressPresetApply = true;
        try
        {
            _settings.ResetToDefaults();
        }
        finally
        {
            _suppressPresetApply = false;
        }

        _dirty = true;
        SyncTrayRowsFromSettings();
        UpdatePreview();
        UpdateTintUi();
        UpdateFooterStatus();
        ShowToast("已恢复默认值，点击“应用”写入");
    }

    private void OpenConfigButton_Click(object sender, RoutedEventArgs e)
        => NativeBridge.RevealSettingsFile(_store.Path);

    private void RestartBarButton_Click(object sender, RoutedEventArgs e)
    {
        var error = NativeBridge.RestartNativeBar();
        if (error is null)
        {
            ShowToast("已重启菜单栏");
            var timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1200) };
            timer.Tick += (_, _) =>
            {
                timer.Stop();
                UpdateNativeStatus();
            };
            timer.Start();
        }
        else
        {
            ShowToast(error, success: false);
        }

        UpdateNativeStatus();
    }

    // =====================================================================================
    // Status / toast
    // =====================================================================================

    private void UpdateNativeStatus()
    {
        var running = NativeBridge.IsNativeBarRunning();
        var nativePath = _nativeBarPath ?? NativeBridge.FindNativeBar();
        if (!string.Equals(_nativeBarPath, nativePath, StringComparison.OrdinalIgnoreCase))
        {
            _nativeBarPath = nativePath;
            AboutNativePathText.Text = nativePath ?? "未找到 SuperMenuBar.exe（构建菜单栏后即可重启）";
            AboutNativePathText.ToolTip = nativePath;
            RestartBarButton.Visibility = nativePath is null ? Visibility.Collapsed : Visibility.Visible;
            AboutRestartButton.Visibility = nativePath is null ? Visibility.Collapsed : Visibility.Visible;
        }

        NativeStatusText.Text = running ? "菜单栏运行中" : "菜单栏未运行";
        NativeStatusDot.Fill = (Brush)FindResource(running ? "Brush.Success" : "Brush.Text.Tertiary");
        AboutNativeStateText.Text = running ? "运行中" : "未运行";
        AboutNativeDot.Fill = (Brush)FindResource(running ? "Brush.Success" : "Brush.Text.Tertiary");
    }

    private void UpdateFooterStatus()
    {
        DirtyPill.Visibility = _dirty ? Visibility.Visible : Visibility.Collapsed;

        if (_dirty)
        {
            FooterStatusText.Text = "有未应用的更改";
            FooterStatusDot.Fill = (Brush)FindResource("Brush.Accent");
            return;
        }

        if (_loadError is not null)
        {
            FooterStatusText.Text = _loadError;
            FooterStatusDot.Fill = (Brush)FindResource("Brush.Danger");
            return;
        }

        FooterStatusDot.Fill = (Brush)FindResource("Brush.Success");
        FooterStatusText.Text = _lastSavedAt is not null
            ? $"已应用 · {_lastSavedAt.Value:HH:mm:ss}"
            : "已加载设置";
    }

    private void ShowToast(string message, bool success = true)
    {
        ToastText.Text = message;
        ToastDot.Fill = (Brush)FindResource(success ? "Brush.Success" : "Brush.Danger");

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        ToastRoot.BeginAnimation(OpacityProperty, new DoubleAnimation(ToastRoot.Opacity, 1, TimeSpan.FromMilliseconds(160)) { EasingFunction = ease });
        ToastTranslate.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(10, 0, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });

        _toastTimer.Stop();
        _toastTimer.Interval = TimeSpan.FromSeconds(2.8);
        _toastTimer.Start();
    }

    private void HideToast()
    {
        var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
        ToastRoot.BeginAnimation(OpacityProperty, new DoubleAnimation(ToastRoot.Opacity, 0, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
        ToastTranslate.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, 8, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
    }

    private void PreviewCard_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        if (PreviewCard.ActualWidth <= 0 || PreviewCard.ActualHeight <= 0) return;
        PreviewCard.Clip = new RectangleGeometry(new Rect(0, 0, PreviewCard.ActualWidth, PreviewCard.ActualHeight), 18, 18);
    }
}
