using System.Windows.Data;

namespace SuperMenu.Settings.Converters;

/// <summary>True hides, false shows. Used for "no native bar found" hints.</summary>
public sealed class InverseBooleanToVisibilityConverter : IValueConverter
{
    public object Convert(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
        => value is true ? System.Windows.Visibility.Collapsed : System.Windows.Visibility.Visible;

    public object ConvertBack(object value, Type targetType, object parameter, System.Globalization.CultureInfo culture)
        => value is System.Windows.Visibility.Collapsed;
}
