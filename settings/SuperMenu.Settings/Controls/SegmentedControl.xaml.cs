using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Media;
using System.Windows.Media.Animation;

namespace SuperMenu.Settings.Controls;

/// <summary>
/// macOS-style segmented control. Set <see cref="Labels"/> and <see cref="Values"/> as
/// pipe-delimited strings and bind <see cref="SelectedValue"/> two-way to a settings property.
/// The selected pill slides between segments with a short eased animation.
/// </summary>
public partial class SegmentedControl : UserControl
{
    private static readonly TimeSpan SlideDuration = TimeSpan.FromMilliseconds(190);
    private static readonly IEasingFunction SlideEase = new CubicEase { EasingMode = EasingMode.EaseOut };

    private readonly List<RadioButton> _segments = [];
    private bool _syncing;
    private bool _initialised;

    public SegmentedControl()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            _initialised = true;
            UpdateCheckedStates();
            PositionPill(animate: false);
        };
        SizeChanged += (_, _) => PositionPill(animate: false);
    }

    public static readonly DependencyProperty LabelsProperty = DependencyProperty.Register(
        nameof(Labels), typeof(string), typeof(SegmentedControl),
        new PropertyMetadata(string.Empty, OnLabelsOrValuesChanged));

    public static readonly DependencyProperty ValuesProperty = DependencyProperty.Register(
        nameof(Values), typeof(string), typeof(SegmentedControl),
        new PropertyMetadata(string.Empty, OnLabelsOrValuesChanged));

    public static readonly DependencyProperty SelectedValueProperty = DependencyProperty.Register(
        nameof(SelectedValue), typeof(string), typeof(SegmentedControl),
        new FrameworkPropertyMetadata(null, FrameworkPropertyMetadataOptions.BindsTwoWayByDefault, OnSelectedValueChanged));

    /// <summary>Pipe-delimited display labels, e.g. <c>跟随系统|浅色|深色</c>.</summary>
    public string Labels
    {
        get => (string)GetValue(LabelsProperty);
        set => SetValue(LabelsProperty, value);
    }

    /// <summary>Pipe-delimited persisted values, e.g. <c>auto|light|dark</c>. Falls back to the labels.</summary>
    public string Values
    {
        get => (string)GetValue(ValuesProperty);
        set => SetValue(ValuesProperty, value);
    }

    public string? SelectedValue
    {
        get => (string?)GetValue(SelectedValueProperty);
        set => SetValue(SelectedValueProperty, value);
    }

    private static void OnLabelsOrValuesChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        => ((SegmentedControl)d).RebuildSegments();

    private static void OnSelectedValueChanged(DependencyObject d, DependencyPropertyChangedEventArgs e)
        => ((SegmentedControl)d).UpdateCheckedStates();

    private void RebuildSegments()
    {
        var labels = Split(Labels);
        if (labels.Length == 0) return;
        var values = Split(Values);
        if (values.Length != labels.Length) values = labels;

        _syncing = true;
        try
        {
            SegmentsGrid.Children.Clear();
            SegmentsGrid.Columns = labels.Length;
            _segments.Clear();

            var groupName = "Segment" + Guid.NewGuid().ToString("N");
            for (var i = 0; i < labels.Length; i++)
            {
                var segment = new RadioButton
                {
                    Content = labels[i],
                    Tag = values[i],
                    GroupName = groupName,
                    Style = (Style)FindResource("Style.SegmentButton"),
                    HorizontalContentAlignment = HorizontalAlignment.Center,
                };
                segment.Checked += OnSegmentChecked;
                _segments.Add(segment);
                SegmentsGrid.Children.Add(segment);
            }
        }
        finally
        {
            _syncing = false;
        }

        UpdateCheckedStates();
    }

    private void OnSegmentChecked(object sender, RoutedEventArgs e)
    {
        if (_syncing || sender is not RadioButton { Tag: string value }) return;
        if (string.Equals(SelectedValue, value, StringComparison.Ordinal)) return;

        _syncing = true;
        try
        {
            SelectedValue = value;
        }
        finally
        {
            _syncing = false;
        }

        PositionPill(animate: true);
    }

    private void UpdateCheckedStates()
    {
        if (!_initialised) return;
        _syncing = true;
        try
        {
            var selected = SelectedValue;
            foreach (var segment in _segments)
            {
                var isSelected = segment.Tag is string value && string.Equals(value, selected, StringComparison.Ordinal);
                if (segment.IsChecked != isSelected) segment.IsChecked = isSelected;
            }
        }
        finally
        {
            _syncing = false;
        }
    }

    private void PositionPill(bool animate)
    {
        var count = _segments.Count;
        var width = Track.ActualWidth;
        if (count == 0 || width <= 0) return;

        var index = 0;
        for (var i = 0; i < count; i++)
        {
            if (_segments[i].Tag is string value && string.Equals(value, SelectedValue, StringComparison.Ordinal))
            {
                index = i;
                break;
            }
        }

        var segmentWidth = Math.Max(0, (width - 6) / count);
        // Margin="3" positions the pill's layout slot; the transform only adds the segment offset.
        var targetX = index * segmentWidth;

        if (!animate)
        {
            PillTransform.BeginAnimation(TranslateTransform.XProperty, null);
            PillTransform.X = targetX;
            Pill.Width = segmentWidth;
            return;
        }

        var xAnimation = new DoubleAnimation(targetX, SlideDuration) { EasingFunction = SlideEase };
        var widthAnimation = new DoubleAnimation(segmentWidth, SlideDuration) { EasingFunction = SlideEase };
        PillTransform.BeginAnimation(TranslateTransform.XProperty, xAnimation);
        Pill.BeginAnimation(WidthProperty, widthAnimation);
    }

    private static string[] Split(string? value)
        => string.IsNullOrEmpty(value)
            ? []
            : value.Split('|', StringSplitOptions.TrimEntries);
}
