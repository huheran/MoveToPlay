using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion;

public partial class OverlayControlWindow : Window
{
    private sealed record Choice(string Id, string Name)
    {
        public override string ToString() => Name;
    }

    private static readonly Choice[] AnchorChoices =
    [
        new("TopLeft", "左上角"), new("TopCenter", "顶部居中"), new("TopRight", "右上角"),
        new("CenterLeft", "左侧居中"), new("Center", "屏幕中央"), new("CenterRight", "右侧居中"),
        new("BottomLeft", "左下角"), new("BottomCenter", "底部居中"), new("BottomRight", "右下角"),
    ];

    private static readonly Choice[] ResolutionChoices =
    [
        new("auto", "自动检测（推荐）"),
        new("1280x720", "1280 × 720 · 16:9"), new("1366x768", "1366 × 768 · 16:9"),
        new("1600x900", "1600 × 900 · 16:9"), new("1920x1080", "1920 × 1080 · 16:9"),
        new("2560x1440", "2560 × 1440 · 2K"), new("3840x2160", "3840 × 2160 · 4K"),
        new("1280x800", "1280 × 800 · 16:10"), new("1920x1200", "1920 × 1200 · 16:10"),
        new("2560x1080", "2560 × 1080 · 超宽屏"), new("3440x1440", "3440 × 1440 · 超宽屏"),
        new("3840x1600", "3840 × 1600 · 超宽屏"), new("5120x1440", "5120 × 1440 · 双 QHD"),
    ];

    private readonly Action<OverlayPlacement> _applyPlacement;
    private readonly Func<OverlayPlacement> _resetPlacement;
    private readonly Func<string, OverlayPlacement> _changeResolution;
    private readonly Action _toggleOverlay;
    private readonly Action _ensureOverlayVisible;
    private readonly Func<bool> _isOverlayVisible;
    private bool _updating;
    private string _profileName = "当前游戏";

    public OverlayControlWindow(
        OverlayPlacement placement,
        string resolutionKey,
        string profileName,
        Action<OverlayPlacement> applyPlacement,
        Func<OverlayPlacement> resetPlacement,
        Func<string, OverlayPlacement> changeResolution,
        Action toggleOverlay,
        Action ensureOverlayVisible,
        Func<bool> isOverlayVisible)
    {
        _applyPlacement = applyPlacement;
        _resetPlacement = resetPlacement;
        _changeResolution = changeResolution;
        _toggleOverlay = toggleOverlay;
        _ensureOverlayVisible = ensureOverlayVisible;
        _isOverlayVisible = isOverlayVisible;
        InitializeComponent();

        AnchorSelector.ItemsSource = AnchorChoices;
        ResolutionSelector.ItemsSource = ResolutionChoices;
        LoadPlacement(placement, resolutionKey, profileName);
        Loaded += (_, _) =>
        {
            PositionBesideMainWindow();
            _ensureOverlayVisible();
            UpdateOverlayButton();
            Activate();
        };
    }

    public void LoadPlacement(OverlayPlacement placement, string resolutionKey, string profileName)
    {
        _updating = true;
        try
        {
            _profileName = profileName;
            ProfileText.Text = $"{profileName} · 调整会实时显示在游戏屏幕";
            AnchorSelector.SelectedItem = AnchorChoices.First(choice =>
                choice.Id.Equals(OverlayPlacement.NormalizeAnchor(placement.Anchor), StringComparison.Ordinal));
            ResolutionSelector.SelectedItem = ResolutionChoices.FirstOrDefault(choice =>
                choice.Id.Equals(resolutionKey, StringComparison.OrdinalIgnoreCase)) ?? ResolutionChoices[0];
            OffsetXSlider.Value = placement.OffsetX;
            OffsetYSlider.Value = placement.OffsetY;
            HeaderScaleSlider.Value = placement.HeaderScale;
            ActionScaleSlider.Value = placement.ActionScale;
            MetricsScaleSlider.Value = placement.MetricsScale;
            GoalScaleSlider.Value = placement.GoalScale;
            ToastScaleSlider.Value = placement.ToastScale;
            UpdateValueLabels();
        }
        finally
        {
            _updating = false;
        }
    }

    private void ControlValueChanged(object sender, RoutedEventArgs e)
    {
        if (_updating || !IsLoaded || AnchorSelector.SelectedItem is not Choice anchor)
        {
            return;
        }

        var placement = ReadPlacement(anchor.Id);
        UpdateValueLabels();
        _applyPlacement(placement);
    }

    private void ResolutionSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (_updating || !IsLoaded || ResolutionSelector.SelectedItem is not Choice resolution)
        {
            return;
        }

        var placement = _changeResolution(resolution.Id);
        LoadPlacement(placement, resolution.Id, _profileName);
    }

    private OverlayPlacement ReadPlacement(string anchor) => new()
    {
        Anchor = anchor,
        OffsetX = OffsetXSlider.Value,
        OffsetY = OffsetYSlider.Value,
        HeaderScale = HeaderScaleSlider.Value,
        ActionScale = ActionScaleSlider.Value,
        MetricsScale = MetricsScaleSlider.Value,
        GoalScale = GoalScaleSlider.Value,
        ToastScale = ToastScaleSlider.Value,
    };

    private void ResetButton_Click(object sender, RoutedEventArgs e)
    {
        var placement = _resetPlacement();
        var resolution = (ResolutionSelector.SelectedItem as Choice)?.Id ?? "auto";
        LoadPlacement(placement, resolution, _profileName);
    }

    private void ToggleOverlayButton_Click(object sender, RoutedEventArgs e)
    {
        _toggleOverlay();
        UpdateOverlayButton();
    }

    private void UpdateOverlayButton() =>
        ToggleOverlayButton.Content = _isOverlayVisible() ? "暂时隐藏" : "重新显示";

    public void RefreshOverlayStatus() => UpdateOverlayButton();

    private void UpdateValueLabels()
    {
        OffsetXValue.Text = $"{OffsetXSlider.Value:+0;-0;0}";
        OffsetYValue.Text = $"{OffsetYSlider.Value:+0;-0;0}";
        HeaderScaleValue.Text = Percent(HeaderScaleSlider.Value);
        ActionScaleValue.Text = Percent(ActionScaleSlider.Value);
        MetricsScaleValue.Text = Percent(MetricsScaleSlider.Value);
        GoalScaleValue.Text = Percent(GoalScaleSlider.Value);
        ToastScaleValue.Text = Percent(ToastScaleSlider.Value);
    }

    private static string Percent(double value) => $"{Math.Round(value * 100):0}%";

    private void PositionBesideMainWindow()
    {
        var workingArea = SystemParameters.WorkArea;
        Left = Math.Max(workingArea.Left + 18, Owner?.Left - Width - 18 ?? workingArea.Left + 18);
        Top = Math.Clamp(Owner?.Top ?? workingArea.Top + 18,
            workingArea.Top + 18,
            Math.Max(workingArea.Top + 18, workingArea.Bottom - Height - 18));
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.LeftButton == MouseButtonState.Pressed)
        {
            DragMove();
        }
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();
}
