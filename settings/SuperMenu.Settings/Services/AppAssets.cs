using System.IO;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Microsoft.Win32;

namespace SuperMenu.Settings.Services;

/// <summary>
/// Bundled resources loaded once at startup: HarmonyOS Sans (private, never installed), the backdrop
/// wallpaper, and the semantic font family resources the XAML refers to with DynamicResource.
/// Everything degrades gracefully to system fonts / a gradient when the assets are absent.
/// </summary>
public static class AppAssets
{
    public const string WallpaperPackUri = "pack://application:,,,/Assets/Backgrounds/flow.jpg";
    public const string IconPackUri = "pack://application:,,,/Assets/SuperMenu.ico";
    public const string VectorIconPackUri = "pack://application:,,,/Assets/appicon.png";

    public static readonly string FontsDirectory = Path.Combine(AppContext.BaseDirectory, "Assets", "Fonts");

    /// <summary>Rough perceived luminance (0–1) of the wallpaper band shown behind the preview bar; drives ink colour.</summary>
    public static double PreviewBandLuminance { get; private set; } = 0.32;

    /// <summary>Frozen brush over the wallpaper, top-anchored, used as preview backdrop.</summary>
    public static ImageBrush? TopAnchoredWallpaperBrush { get; private set; }

    /// <summary>Frozen brush over the wallpaper used for the window-wide blurred backdrop.</summary>
    public static ImageBrush? WindowWallpaperBrush { get; private set; }

    public static bool FontsLoaded { get; private set; }

    public static void Initialize()
    {
        LoadWallpaper();
        LoadFonts();
    }

    public static bool IsSystemLightTheme()
    {
        try
        {
            var value = Registry.GetValue(
                @"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize",
                "AppsUseLightTheme",
                0);
            return value is int i ? i != 0 : Convert.ToInt64(value ?? 0) != 0;
        }
        catch
        {
            return false;
        }
    }

    private static void LoadWallpaper()
    {
        try
        {
            var image = new BitmapImage();
            image.BeginInit();
            image.UriSource = new Uri(WallpaperPackUri, UriKind.Absolute);
            image.CacheOption = BitmapCacheOption.OnLoad;
            image.CreateOptions = BitmapCreateOptions.IgnoreImageCache;
            image.EndInit();
            image.Freeze();

            TopAnchoredWallpaperBrush = new ImageBrush(image)
            {
                Stretch = Stretch.UniformToFill,
                AlignmentX = AlignmentX.Left,
                AlignmentY = AlignmentY.Center,
            };
            TopAnchoredWallpaperBrush.Freeze();

            WindowWallpaperBrush = new ImageBrush(image)
            {
                Stretch = Stretch.UniformToFill,
                AlignmentX = AlignmentX.Center,
                AlignmentY = AlignmentY.Center,
            };
            WindowWallpaperBrush.Freeze();

            PreviewBandLuminance = SamplePreviewBandLuminance(image);
        }
        catch
        {
            PreviewBandLuminance = 0.32; // fallback: assume a mid-dark backdrop and use light ink
        }
    }

    private static double SamplePreviewBandLuminance(BitmapImage image)
    {
        var converted = new FormatConvertedBitmap(image, PixelFormats.Bgra32, null, 0);
        var width = converted.PixelWidth;
        var top = converted.PixelHeight * 28 / 100;
        var height = Math.Max(1, converted.PixelHeight * 22 / 100);
        var stride = width * 4;
        var pixels = new byte[stride * height];
        converted.CopyPixels(new Int32Rect(0, top, width, height), pixels, stride, 0);

        double sum = 0;
        var count = 0;
        var step = Math.Max(1, width / 160); // coarse sampling is plenty for an average
        for (var y = 0; y < height; y += 2)
        {
            for (var x = 0; x < width; x += step)
            {
                var offset = y * stride + x * 4;
                var b = pixels[offset];
                var g = pixels[offset + 1];
                var r = pixels[offset + 2];
                sum += (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
                count++;
            }
        }

        return count > 0 ? sum / count : 0.32;
    }

    private static void LoadFonts()
    {
        var body = new FontFamily("Microsoft YaHei UI, Segoe UI Variable Text, Segoe UI");
        var demi = body;
        var semi = body;
        var light = body;
        var installed = false;

        try
        {
            if (Directory.Exists(FontsDirectory))
            {
                var families = new Dictionary<string, FontFamily>(StringComparer.OrdinalIgnoreCase);
                foreach (var family in Fonts.GetFontFamilies(new Uri(FontsDirectory + Path.DirectorySeparatorChar)))
                {
                    foreach (var name in family.FamilyNames.Values)
                    {
                        if (!string.IsNullOrWhiteSpace(name)) families[name] = family;
                    }
                }

                FontFamily Pick(string name, FontFamily fallback) =>
                    families.TryGetValue(name, out var found) ? found : fallback;

                // WPF exposes the four bundled HarmonyOS faces as one family with
                // Light/Regular/Medium/Bold weights; the XAML styles add the
                // corresponding FontWeight beside these DynamicResource values.
                body = Pick("HarmonyOS Sans SC", body);
                demi = Pick("HarmonyOS Sans SC", body);
                semi = Pick("HarmonyOS Sans SC", body);
                light = Pick("HarmonyOS Sans SC", body);
                installed = families.Keys.Any(k => k.StartsWith("HarmonyOS Sans SC", StringComparison.OrdinalIgnoreCase));
            }
        }
        catch
        {
            // Font parsing is best-effort: fall back to Segoe UI Variable / Microsoft YaHei UI.
        }

        FontsLoaded = installed;
        var app = Application.Current;
        if (app is null) return;

        app.Resources["Font.Body"] = body;
        app.Resources["Font.Demi"] = demi;
        app.Resources["Font.Semi"] = semi;
        app.Resources["Font.Light"] = light;
        app.Resources["Font.Mono"] = body;
        app.Resources["Font.Icon"] = new FontFamily("Segoe Fluent Icons, Segoe MDL2 Assets");

        if (!installed)
        {
            // Without the HarmonyOS Sans files the fallback chain above keeps the window usable;
            // report it in the About page instead of failing to start.
            app.Resources["Font.LoadedNote"] = "HarmonyOS Sans 未找到，已回退到 Segoe UI Variable";
        }
        else
        {
            app.Resources["Font.LoadedNote"] = "HarmonyOS Sans SC 私有加载";
        }
    }
}
