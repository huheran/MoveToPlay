using System.ComponentModel;
using System.Runtime.CompilerServices;
using System.Windows;
using System.Windows.Media;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.ViewModels;

public sealed class TelemetryViewModel : INotifyPropertyChanged
{
    private string _profileName = "原神 · 提瓦特旅途";
    private string _profileSubtitle = "ADVENTURE FITNESS";
    private string _actionName = "等待 Dongle";
    private string _actionHint = "请连接 MoveToPlay 接收器";
    private string _heartRate = "—";
    private string _calories = "0.0";
    private string _activeTime = "00:00";
    private int _goalProgress;
    private int _combo;
    private string _encouragement = "正在搜索 MoveToPlay Dongle";
    private string _statusText = "SEARCHING";
    private string _sourceLabel = "USB CDC";
    private string _connectionDetail = "正在搜索 MoveToPlay Dongle";
    private string _trackerStatus = "0/4";
    private string _bladeStatus = "OFF";
    private string _signalQuality = "0%";
    private string _chestBattery = "—";
    private string _rightHandBattery = "—";
    private string _leftHandBattery = "—";
    private string _legBattery = "—";
    private string _bladeBattery = "—";
    private Brush _connectionStatusBrush = BrushFrom("#F4D38A");
    private string? _sourceStatusText;
    private string _actionLabel = "当前动作";
    private string _goalLabel = "今日运动目标";
    private string _markerGlyph = "✦";
    private FontFamily _hudFontFamily = new("Microsoft YaHei UI");
    private CornerRadius _progressCornerRadius = new(3);
    private double _progressHeight = 4;
    private Brush _accentBrush = BrushFrom("#F4D38A");
    private Brush _accentSecondaryBrush = BrushFrom("#80D9FF");
    private Color _panelBaseColor = ColorFrom("#181B26");
    private double _panelOpacityPercent = 82;
    private Brush _panelBrush = BrushFrom("#D1181B26");
    private Brush _primaryTextBrush = BrushFrom("#FFF9F5E9");
    private Brush _mutedTextBrush = BrushFrom("#FFB8B2A5");

    public event PropertyChangedEventHandler? PropertyChanged;

    public string ProfileName { get => _profileName; private set => Set(ref _profileName, value); }
    public string ProfileSubtitle { get => _profileSubtitle; private set => Set(ref _profileSubtitle, value); }
    public string ActionName { get => _actionName; private set => Set(ref _actionName, value); }
    public string ActionHint { get => _actionHint; private set => Set(ref _actionHint, value); }
    public string HeartRate { get => _heartRate; private set => Set(ref _heartRate, value); }
    public string Calories { get => _calories; private set => Set(ref _calories, value); }
    public string ActiveTime { get => _activeTime; private set => Set(ref _activeTime, value); }
    public int GoalProgress { get => _goalProgress; private set => Set(ref _goalProgress, value); }
    public int Combo { get => _combo; private set => Set(ref _combo, value); }
    public string Encouragement { get => _encouragement; private set => Set(ref _encouragement, value); }
    public string StatusText { get => _statusText; private set => Set(ref _statusText, value); }
    public string SourceLabel { get => _sourceLabel; private set => Set(ref _sourceLabel, value); }
    public string ConnectionDetail { get => _connectionDetail; private set => Set(ref _connectionDetail, value); }
    public string TrackerStatus { get => _trackerStatus; private set => Set(ref _trackerStatus, value); }
    public string BladeStatus { get => _bladeStatus; private set => Set(ref _bladeStatus, value); }
    public string SignalQuality { get => _signalQuality; private set => Set(ref _signalQuality, value); }
    public string ChestBattery { get => _chestBattery; private set => Set(ref _chestBattery, value); }
    public string RightHandBattery { get => _rightHandBattery; private set => Set(ref _rightHandBattery, value); }
    public string LeftHandBattery { get => _leftHandBattery; private set => Set(ref _leftHandBattery, value); }
    public string LegBattery { get => _legBattery; private set => Set(ref _legBattery, value); }
    public string BladeBattery { get => _bladeBattery; private set => Set(ref _bladeBattery, value); }
    public Brush ConnectionStatusBrush { get => _connectionStatusBrush; private set => Set(ref _connectionStatusBrush, value); }
    public string ActionLabel { get => _actionLabel; private set => Set(ref _actionLabel, value); }
    public string GoalLabel { get => _goalLabel; private set => Set(ref _goalLabel, value); }
    public string MarkerGlyph { get => _markerGlyph; private set => Set(ref _markerGlyph, value); }
    public FontFamily HudFontFamily { get => _hudFontFamily; private set => Set(ref _hudFontFamily, value); }
    public CornerRadius ProgressCornerRadius { get => _progressCornerRadius; private set => Set(ref _progressCornerRadius, value); }
    public double ProgressHeight { get => _progressHeight; private set => Set(ref _progressHeight, value); }
    public Brush AccentBrush { get => _accentBrush; private set => Set(ref _accentBrush, value); }
    public Brush AccentSecondaryBrush { get => _accentSecondaryBrush; private set => Set(ref _accentSecondaryBrush, value); }
    public Brush PanelBrush { get => _panelBrush; private set => Set(ref _panelBrush, value); }
    public Brush PrimaryTextBrush { get => _primaryTextBrush; private set => Set(ref _primaryTextBrush, value); }
    public Brush MutedTextBrush { get => _mutedTextBrush; private set => Set(ref _mutedTextBrush, value); }

    public void ApplyProfile(GameProfile profile)
    {
        ProfileName = TextOr(profile.DisplayName, "通用运动界面");
        ProfileSubtitle = TextOr(profile.Subtitle, "MOVE TO PLAY");
        StatusText = _sourceStatusText ?? TextOr(profile.StatusText, "ONLINE");
        ActionLabel = TextOr(profile.ActionLabel, "当前动作");
        GoalLabel = TextOr(profile.GoalLabel, "今日运动目标");
        MarkerGlyph = TextOr(profile.MarkerGlyph, "✦");
        HudFontFamily = FontFamilyFrom(profile.HudFontFamily);
        ProgressCornerRadius = profile.SquareProgress ? new CornerRadius(0) : new CornerRadius(3);
        ProgressHeight = profile.SquareProgress ? 7 : 4;
        AccentBrush = BrushFrom(profile.Accent);
        AccentSecondaryBrush = BrushFrom(profile.AccentSecondary);
        _panelBaseColor = ColorFrom(profile.PanelBackground);
        RefreshPanelBrush();
        PrimaryTextBrush = BrushFrom(profile.TextPrimary);
        MutedTextBrush = BrushFrom(profile.TextMuted);
    }

    public void ApplySnapshot(TelemetrySnapshot snapshot)
    {
        ActionName = snapshot.ActionName;
        ActionHint = snapshot.ActionHint;
        HeartRate = snapshot.HeartRate?.ToString() ?? "—";
        Calories = snapshot.Calories.ToString("0.0");
        ActiveTime = snapshot.ActiveTime.ToString(@"mm\:ss");
        GoalProgress = snapshot.GoalProgress;
        Combo = snapshot.Combo;
        Encouragement = snapshot.Encouragement;
        TrackerStatus = $"{snapshot.TrackerOnline}/4";
        BladeStatus = snapshot.BladeOnline ? "ON" : "OFF";
        SignalQuality = $"{snapshot.SignalQuality}%";
        ChestBattery = FormatBattery(snapshot.Batteries.Chest);
        RightHandBattery = FormatBattery(snapshot.Batteries.RightHand);
        LeftHandBattery = FormatBattery(snapshot.Batteries.LeftHand);
        LegBattery = FormatBattery(snapshot.Batteries.Leg);
        BladeBattery = FormatBattery(snapshot.Batteries.Blade);
    }

    public void SetEncouragement(string message) => Encouragement = message;

    public void SetSourceStatus(TelemetrySourceStatus status)
    {
        _sourceStatusText = status.StatusText;
        StatusText = status.StatusText;
        SourceLabel = status.StatusText == "DEMO" ? "DEMO SOURCE" : "USB CDC";
        ConnectionDetail = status.Detail;
        ConnectionStatusBrush = BrushFrom(status.Connected ? "#6EE7B7" : "#F4D38A");
        if (!status.Connected)
        {
            ActionName = "等待 Dongle";
            ActionHint = status.Detail;
            Encouragement = status.Detail;
            TrackerStatus = "0/4";
            BladeStatus = "OFF";
            SignalQuality = "0%";
            ChestBattery = "—";
            RightHandBattery = "—";
            LeftHandBattery = "—";
            LegBattery = "—";
            BladeBattery = "—";
        }
    }

    private static string FormatBattery(int? battery) =>
        battery.HasValue ? $"{Math.Clamp(battery.Value, 0, 100)}%" : "—";

    public void SetPanelOpacity(double percent)
    {
        _panelOpacityPercent = Math.Clamp(percent, 55, 100);
        RefreshPanelBrush();
    }

    private void RefreshPanelBrush()
    {
        var alpha = (byte)Math.Round(_panelOpacityPercent / 100.0 * byte.MaxValue);
        PanelBrush = new SolidColorBrush(Color.FromArgb(alpha, _panelBaseColor.R, _panelBaseColor.G, _panelBaseColor.B));
    }

    private static SolidColorBrush BrushFrom(string value)
    {
        try
        {
            return new SolidColorBrush((Color)ColorConverter.ConvertFromString(value));
        }
        catch
        {
            return new SolidColorBrush(Colors.White);
        }
    }

    private static FontFamily FontFamilyFrom(string? value)
    {
        try
        {
            return new FontFamily(TextOr(value, "Microsoft YaHei UI"));
        }
        catch
        {
            return new FontFamily("Microsoft YaHei UI");
        }
    }

    private static string TextOr(string? value, string fallback) =>
        string.IsNullOrWhiteSpace(value) ? fallback : value;

    private static Color ColorFrom(string value)
    {
        try
        {
            return (Color)ColorConverter.ConvertFromString(value);
        }
        catch
        {
            return Colors.Black;
        }
    }

    private void Set<T>(ref T field, T value, [CallerMemberName] string? propertyName = null)
    {
        if (EqualityComparer<T>.Default.Equals(field, value))
        {
            return;
        }

        field = value;
        PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(propertyName));
    }
}
