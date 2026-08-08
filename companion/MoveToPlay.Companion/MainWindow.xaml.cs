using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using MoveToPlay.Companion.Models;
using MoveToPlay.Companion.Services;
using MoveToPlay.Companion.ViewModels;
using Drawing = System.Drawing;
using Drawing2D = System.Drawing.Drawing2D;
using Forms = System.Windows.Forms;

namespace MoveToPlay.Companion;

public partial class MainWindow : Window
{
    private sealed record OverlayAnchorOption(string Id, string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    private sealed record DisplayResolutionOption(string Key, int Width, int Height, string DisplayName)
    {
        public override string ToString() => DisplayName;
    }

    private static readonly OverlayAnchorOption[] OverlayAnchorOptions =
    [
        new("TopLeft", "左上角"),
        new("TopCenter", "顶部居中"),
        new("TopRight", "右上角"),
        new("CenterLeft", "左侧居中"),
        new("Center", "屏幕中央"),
        new("CenterRight", "右侧居中"),
        new("BottomLeft", "左下角"),
        new("BottomCenter", "底部居中"),
        new("BottomRight", "右下角"),
    ];

    private static readonly DisplayResolutionOption[] DisplayResolutionOptions =
    [
        new("auto", 0, 0, "自动检测（推荐）"),
        new("1280x720", 1280, 720, "1280 × 720 · 16:9"),
        new("1366x768", 1366, 768, "1366 × 768 · 16:9"),
        new("1600x900", 1600, 900, "1600 × 900 · 16:9"),
        new("1920x1080", 1920, 1080, "1920 × 1080 · 16:9"),
        new("2560x1440", 2560, 1440, "2560 × 1440 · 2K"),
        new("3840x2160", 3840, 2160, "3840 × 2160 · 4K"),
        new("1280x800", 1280, 800, "1280 × 800 · 16:10"),
        new("1920x1200", 1920, 1200, "1920 × 1200 · 16:10"),
        new("2560x1080", 2560, 1080, "2560 × 1080 · 超宽屏"),
        new("3440x1440", 3440, 1440, "3440 × 1440 · 超宽屏"),
        new("3840x1600", 3840, 1600, "3840 × 1600 · 超宽屏"),
        new("5120x1440", 5120, 1440, "5120 × 1440 · 双 QHD"),
    ];

    private const int HotkeyId = 0x4D3250;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint VkF10 = 0x79;
    private const int WmHotkey = 0x0312;

    private readonly TelemetryViewModel _viewModel = new();
    private readonly ProfileService _profileService = new();
    private readonly GameWindowService _gameWindowService = new();
    private readonly OverlayPlacementService _overlayPlacementService = new();
    private readonly ITelemetrySource _telemetrySource;
    private readonly DispatcherTimer _placementSaveTimer = new()
    {
        Interval = TimeSpan.FromMilliseconds(350),
    };
    private IReadOnlyList<GameProfile> _profiles = [];
    private IReadOnlyList<GameWindowTarget> _windowTargets = [];
    private OverlayWindow? _overlayWindow;
    private TrainingWindow? _trainingWindow;
    private HwndSource? _hwndSource;
    private Forms.NotifyIcon? _trayIcon;
    private Drawing.Icon? _trayIconAsset;
    private bool _exitRequested;
    private bool _trayTipShown;
    private bool _updatingPlacementControls;
    private OverlayPlacement _currentOverlayPlacement = new();
    private string? _pendingPlacementProfileId;
    private string _currentResolutionKey = "auto";
    private int _previewResolutionWidth = 1920;
    private int _previewResolutionHeight = 1080;

    public MainWindow()
    {
        _telemetrySource = CreateTelemetrySource();
        DataContext = _viewModel;
        InitializeComponent();

        SourceInitialized += OnSourceInitialized;
        Loaded += OnLoaded;
        ContentRendered += OnContentRendered;
        Closing += OnClosing;
        Closed += OnClosed;
        System.Windows.Application.Current.SessionEnding += OnSessionEnding;
        _telemetrySource.SnapshotChanged += OnSnapshotChanged;
        _telemetrySource.StatusChanged += OnTelemetryStatusChanged;
        _placementSaveTimer.Tick += (_, _) => SavePendingOverlayPlacement();
        InitializeTrayIcon();
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
        OverlayAnchorSelector.ItemsSource = OverlayAnchorOptions;
        OverlayResolutionSelector.ItemsSource = DisplayResolutionOptions;
        _profiles = _profileService.LoadProfiles();
        ProfileSelector.ItemsSource = _profiles;
        var requestedProfileId = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--profile=", StringComparison.OrdinalIgnoreCase))?
            ["--profile=".Length..]
            .Trim('"');
        ProfileSelector.SelectedItem = _profiles.FirstOrDefault(profile =>
                                           profile.Id.Equals(requestedProfileId, StringComparison.OrdinalIgnoreCase))
                                       ?? _profiles[0];
        RefreshWindowTargets();
        _telemetrySource.Start();
    }

    private async void OnContentRendered(object? sender, EventArgs e)
    {
        var trainingCaptureArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--capture-training=", StringComparison.OrdinalIgnoreCase));
        if (trainingCaptureArgument is not null)
        {
            _trainingWindow = new TrainingWindow(() => { }, () => { }) { Owner = this };
            _trainingWindow.Show();
            await Task.Delay(800);
            var trainingOutputPath = Path.GetFullPath(trainingCaptureArgument["--capture-training=".Length..].Trim('"'));
            CaptureElementToPng(_trainingWindow, trainingOutputPath);
            _trainingWindow.Close();
            _trainingWindow = null;
            _exitRequested = true;
            Close();
            return;
        }

        if (Environment.GetCommandLineArgs().Any(argument => argument.Equals("--show-overlay", StringComparison.OrdinalIgnoreCase)))
        {
            await Task.Delay(500);
            ToggleOverlay();
        }

        var captureArgument = Environment.GetCommandLineArgs()
            .FirstOrDefault(argument => argument.StartsWith("--capture=", StringComparison.OrdinalIgnoreCase));
        if (captureArgument is null)
        {
            return;
        }

        await Task.Delay(900);
        var requestedPath = captureArgument["--capture=".Length..].Trim('"');
        var outputPath = Path.GetFullPath(requestedPath);
        CaptureElementToPng(MainShell, outputPath);
        _exitRequested = true;
        Close();
    }

    private void OnClosing(object? sender, CancelEventArgs e)
    {
        if (_exitRequested)
        {
            return;
        }

        e.Cancel = true;
        HideToTray();
    }

    private void OnSessionEnding(object? sender, SessionEndingCancelEventArgs e)
    {
        _exitRequested = true;
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        SavePendingOverlayPlacement();
        _telemetrySource.Stop();
        _telemetrySource.SnapshotChanged -= OnSnapshotChanged;
        _telemetrySource.StatusChanged -= OnTelemetryStatusChanged;
        _overlayWindow?.Close();
        System.Windows.Application.Current.SessionEnding -= OnSessionEnding;

        if (_trayIcon is not null)
        {
            _trayIcon.Visible = false;
            _trayIcon.Dispose();
            _trayIcon = null;
        }
        _trayIconAsset?.Dispose();
        _trayIconAsset = null;

        if (_hwndSource is not null)
        {
            _hwndSource.RemoveHook(WindowMessageHook);
        }

        _ = UnregisterHotKey(new WindowInteropHelper(this).Handle, HotkeyId);
    }

    private void InitializeTrayIcon()
    {
        _trayIconAsset = CreateTrayIcon();
        var menu = new Forms.ContextMenuStrip();
        var openItem = new Forms.ToolStripMenuItem("打开主界面");
        var overlayItem = new Forms.ToolStripMenuItem("显示/隐藏悬浮层");
        var exitItem = new Forms.ToolStripMenuItem("退出 MoveToPlay");

        openItem.Font = new Drawing.Font(openItem.Font, Drawing.FontStyle.Bold);
        openItem.Click += (_, _) => Dispatcher.BeginInvoke(new Action(RestoreFromTray));
        overlayItem.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ToggleOverlay));
        exitItem.Click += (_, _) => Dispatcher.BeginInvoke(new Action(ExitApplication));
        menu.Items.Add(openItem);
        menu.Items.Add(overlayItem);
        menu.Items.Add(new Forms.ToolStripSeparator());
        menu.Items.Add(exitItem);

        _trayIcon = new Forms.NotifyIcon
        {
            Icon = _trayIconAsset,
            Text = "MoveToPlay Companion · 运动伴侣",
            ContextMenuStrip = menu,
            Visible = true,
        };
        _trayIcon.DoubleClick += (_, _) => Dispatcher.BeginInvoke(new Action(RestoreFromTray));
    }

    private void HideToTray()
    {
        ShowInTaskbar = false;
        Hide();

        if (!_trayTipShown && _trayIcon is not null)
        {
            _trayTipShown = true;
            _trayIcon.ShowBalloonTip(
                2500,
                "MoveToPlay 仍在运行",
                "动作接收和运动统计已转入后台。双击托盘图标可以恢复主界面。",
                Forms.ToolTipIcon.Info);
        }
    }

    private void RestoreFromTray()
    {
        ShowInTaskbar = true;
        Show();
        if (WindowState == WindowState.Minimized)
        {
            WindowState = WindowState.Normal;
        }
        Activate();
    }

    private void ExitApplication()
    {
        _exitRequested = true;
        Close();
    }

    private static Drawing.Icon CreateTrayIcon()
    {
        using var bitmap = new Drawing.Bitmap(32, 32);
        using var graphics = Drawing.Graphics.FromImage(bitmap);
        graphics.SmoothingMode = Drawing2D.SmoothingMode.AntiAlias;
        graphics.Clear(Drawing.Color.Transparent);

        using var background = new Drawing.SolidBrush(Drawing.Color.FromArgb(244, 211, 138));
        graphics.FillEllipse(background, 1, 1, 30, 30);
        using var font = new Drawing.Font("Segoe UI", 17, Drawing.FontStyle.Bold, Drawing.GraphicsUnit.Pixel);
        using var foreground = new Drawing.SolidBrush(Drawing.Color.FromArgb(7, 16, 26));
        const string glyph = "M";
        var size = graphics.MeasureString(glyph, font);
        graphics.DrawString(glyph, font, foreground, (32 - size.Width) / 2, (32 - size.Height) / 2 - 1);

        var iconHandle = bitmap.GetHicon();
        try
        {
            using var borrowedIcon = Drawing.Icon.FromHandle(iconHandle);
            return (Drawing.Icon)borrowedIcon.Clone();
        }
        finally
        {
            _ = DestroyIcon(iconHandle);
        }
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        var handle = new WindowInteropHelper(this).Handle;
        _hwndSource = HwndSource.FromHwnd(handle);
        _hwndSource?.AddHook(WindowMessageHook);
        _ = RegisterHotKey(handle, HotkeyId, ModControl | ModShift, VkF10);
    }

    private nint WindowMessageHook(nint hwnd, int message, nint wParam, nint lParam, ref bool handled)
    {
        if (message == WmHotkey && wParam.ToInt32() == HotkeyId)
        {
            ToggleOverlay();
            handled = true;
        }

        return nint.Zero;
    }

    private void OnSnapshotChanged(object? sender, TelemetrySnapshot snapshot)
    {
        if (!Dispatcher.CheckAccess())
        {
            _ = Dispatcher.BeginInvoke(() => OnSnapshotChanged(sender, snapshot));
            return;
        }

        _viewModel.ApplySnapshot(snapshot);
        if (ProfileSelector.SelectedItem is GameProfile profile)
        {
            if (snapshot.Celebrate && profile.Encouragements.Length > 0)
            {
                var messageIndex = Math.Abs(snapshot.Combo) % profile.Encouragements.Length;
                _viewModel.SetEncouragement(profile.Encouragements[messageIndex]);
            }
        }

        if (snapshot.Celebrate)
        {
            _overlayWindow?.ShowEncouragement();
            AnimatePreviewToast();
        }
    }

    private void OnTelemetryStatusChanged(object? sender, TelemetrySourceStatus status)
    {
        if (!Dispatcher.CheckAccess())
        {
            _ = Dispatcher.BeginInvoke(() => OnTelemetryStatusChanged(sender, status));
            return;
        }

        _viewModel.SetSourceStatus(status);
    }

    private static ITelemetrySource CreateTelemetrySource()
    {
        var arguments = Environment.GetCommandLineArgs();
        if (arguments.Any(argument => argument.Equals("--demo", StringComparison.OrdinalIgnoreCase)))
        {
            return new DemoTelemetryService();
        }

        var requestedPort = arguments
            .FirstOrDefault(argument => argument.StartsWith("--port=", StringComparison.OrdinalIgnoreCase))?
            ["--port=".Length..]
            .Trim('"');
        var requestedWeight = arguments
            .FirstOrDefault(argument => argument.StartsWith("--weight=", StringComparison.OrdinalIgnoreCase))?
            ["--weight=".Length..]
            .Trim('"');
        var weightKg = double.TryParse(requestedWeight,
                                       System.Globalization.NumberStyles.Float,
                                       System.Globalization.CultureInfo.InvariantCulture,
                                       out var parsedWeight)
            ? parsedWeight
            : 68.0;
        return new UsbCdcTelemetryService(requestedPort, weightKg);
    }

    private void ProfileSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ProfileSelector.SelectedItem is not GameProfile profile)
        {
            return;
        }

        SavePendingOverlayPlacement();
        _viewModel.ApplyProfile(profile);
        _viewModel.SetEncouragement(profile.IdleMessage);
        var resolutionKey = _overlayPlacementService.GetSelectedResolution(profile.Id);
        UpdateOverlayResolutionControl(resolutionKey);
        var placement = _overlayPlacementService.GetForProfile(profile, _currentResolutionKey);
        UpdateOverlayPlacementControls(placement);
        _overlayWindow?.ApplyProfile(profile);
        _overlayWindow?.ApplyPlacement(placement);
        ApplyPreviewPosition(placement);

        var matchingTarget = _gameWindowService.FindProfileWindow(_windowTargets, profile);
        WindowSelector.SelectedItem = matchingTarget
                                      ?? _windowTargets.FirstOrDefault(target => target.Handle == nint.Zero);
    }

    private void RefreshWindowsButton_Click(object sender, RoutedEventArgs e) => RefreshWindowTargets();

    private void RefreshWindowTargets()
    {
        var previousHandle = (WindowSelector.SelectedItem as GameWindowTarget)?.Handle ?? nint.Zero;
        _windowTargets = _gameWindowService.GetCandidateWindows();
        WindowSelector.ItemsSource = _windowTargets;

        var selected = _windowTargets.FirstOrDefault(target => target.Handle == previousHandle)
                       ?? (ProfileSelector.SelectedItem is GameProfile profile
                           ? _gameWindowService.FindProfileWindow(_windowTargets, profile)
                           : null)
                       ?? _windowTargets[0];
        WindowSelector.SelectedItem = selected;
    }

    private void WindowSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateOverlayOptions();
        UpdatePreviewResolution();
    }

    private void OverlayOptionChanged(object sender, RoutedEventArgs e) => UpdateOverlayOptions();

    private void OverlayAnchorSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        ApplyOverlayPlacementFromControls();
    }

    private void OverlayResolutionSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!IsLoaded ||
            _updatingPlacementControls ||
            ProfileSelector.SelectedItem is not GameProfile profile ||
            OverlayResolutionSelector.SelectedItem is not DisplayResolutionOption resolution)
        {
            return;
        }

        SavePendingOverlayPlacement();
        _currentResolutionKey = resolution.Key;
        _overlayPlacementService.SaveSelectedResolution(profile.Id, resolution.Key);

        var placement = _overlayPlacementService.GetForProfile(profile, resolution.Key);
        UpdateOverlayPlacementControls(placement);
        _overlayWindow?.ApplyPlacement(placement);
        ApplyPreviewPosition(placement);
        UpdatePreviewResolution();
    }

    private void OverlayOffsetSlider_ValueChanged(object sender, RoutedPropertyChangedEventArgs<double> e)
    {
        ApplyOverlayPlacementFromControls();
    }

    private void ResetOverlayPlacementButton_Click(object sender, RoutedEventArgs e)
    {
        if (ProfileSelector.SelectedItem is not GameProfile profile)
        {
            return;
        }

        var placement = OverlayPlacement.FromProfile(profile);
        UpdateOverlayPlacementControls(placement);
        _overlayWindow?.ApplyPlacement(placement);
        ApplyPreviewPosition(placement);
        _overlayPlacementService.Save(profile.Id, _currentResolutionKey, placement);
        _pendingPlacementProfileId = null;
        _placementSaveTimer.Stop();
    }

    private void ApplyOverlayPlacementFromControls()
    {
        if (!IsLoaded ||
            _updatingPlacementControls ||
            ProfileSelector.SelectedItem is not GameProfile profile ||
            OverlayAnchorSelector.SelectedItem is not OverlayAnchorOption anchor)
        {
            return;
        }

        var placement = new OverlayPlacement
        {
            Anchor = anchor.Id,
            OffsetX = OverlayOffsetXSlider.Value,
            OffsetY = OverlayOffsetYSlider.Value,
        };
        _currentOverlayPlacement = placement;
        UpdateOverlayOffsetLabels();
        _overlayWindow?.ApplyPlacement(placement);
        ApplyPreviewPosition(placement);

        _pendingPlacementProfileId = profile.Id;
        _placementSaveTimer.Stop();
        _placementSaveTimer.Start();
    }

    private void UpdateOverlayPlacementControls(OverlayPlacement placement)
    {
        _updatingPlacementControls = true;
        try
        {
            _currentOverlayPlacement = placement.Clone();
            OverlayAnchorSelector.SelectedItem = OverlayAnchorOptions.First(option =>
                option.Id.Equals(OverlayPlacement.NormalizeAnchor(placement.Anchor), StringComparison.Ordinal));
            OverlayOffsetXSlider.Value = Math.Clamp(placement.OffsetX,
                                                    OverlayOffsetXSlider.Minimum,
                                                    OverlayOffsetXSlider.Maximum);
            OverlayOffsetYSlider.Value = Math.Clamp(placement.OffsetY,
                                                    OverlayOffsetYSlider.Minimum,
                                                    OverlayOffsetYSlider.Maximum);
            UpdateOverlayOffsetLabels();
        }
        finally
        {
            _updatingPlacementControls = false;
        }
    }

    private void UpdateOverlayResolutionControl(string resolutionKey)
    {
        _updatingPlacementControls = true;
        try
        {
            var selected = DisplayResolutionOptions.FirstOrDefault(option =>
                               option.Key.Equals(resolutionKey, StringComparison.OrdinalIgnoreCase))
                           ?? DisplayResolutionOptions[0];
            _currentResolutionKey = selected.Key;
            OverlayResolutionSelector.SelectedItem = selected;
        }
        finally
        {
            _updatingPlacementControls = false;
        }

        UpdatePreviewResolution();
    }

    private void UpdateOverlayOffsetLabels()
    {
        OverlayOffsetXText.Text = $"{OverlayOffsetXSlider.Value:+0;-0;0}";
        OverlayOffsetYText.Text = $"{OverlayOffsetYSlider.Value:+0;-0;0}";
    }

    private void SavePendingOverlayPlacement()
    {
        _placementSaveTimer.Stop();
        if (string.IsNullOrWhiteSpace(_pendingPlacementProfileId))
        {
            return;
        }

        _overlayPlacementService.Save(_pendingPlacementProfileId,
                                      _currentResolutionKey,
                                      _currentOverlayPlacement);
        _pendingPlacementProfileId = null;
    }

    private void PreviewHost_SizeChanged(object sender, SizeChangedEventArgs e) =>
        UpdatePreviewViewportSize();

    private void UpdatePreviewResolution()
    {
        var selected = OverlayResolutionSelector.SelectedItem as DisplayResolutionOption
                       ?? DisplayResolutionOptions[0];
        if (selected.Width > 0 && selected.Height > 0)
        {
            _previewResolutionWidth = selected.Width;
            _previewResolutionHeight = selected.Height;
        }
        else if (WindowSelector.SelectedItem is GameWindowTarget { Handle: not 0 } target &&
                 _gameWindowService.TryGetClientBounds(target.Handle, out var bounds))
        {
            _previewResolutionWidth = bounds.Width;
            _previewResolutionHeight = bounds.Height;
        }
        else
        {
            var primaryBounds = Forms.Screen.PrimaryScreen?.Bounds;
            _previewResolutionWidth = primaryBounds?.Width ?? 1920;
            _previewResolutionHeight = primaryBounds?.Height ?? 1080;
        }

        PreviewResolutionText.Text =
            $"TRANSPARENT OVERLAY PREVIEW · {_previewResolutionWidth} × {_previewResolutionHeight}";
        UpdatePreviewViewportSize();
    }

    private void UpdatePreviewViewportSize()
    {
        if (PreviewHost.ActualWidth <= 0 ||
            PreviewHost.ActualHeight <= 0 ||
            _previewResolutionWidth <= 0 ||
            _previewResolutionHeight <= 0)
        {
            return;
        }

        var aspectRatio = (double)_previewResolutionWidth / _previewResolutionHeight;
        var availableRatio = PreviewHost.ActualWidth / PreviewHost.ActualHeight;
        if (availableRatio > aspectRatio)
        {
            PreviewViewport.Height = PreviewHost.ActualHeight;
            PreviewViewport.Width = PreviewHost.ActualHeight * aspectRatio;
        }
        else
        {
            PreviewViewport.Width = PreviewHost.ActualWidth;
            PreviewViewport.Height = PreviewHost.ActualWidth / aspectRatio;
        }
    }

    private void UpdateOverlayOptions()
    {
        if (_overlayWindow is null)
        {
            return;
        }

        _overlayWindow.TargetHandle = (WindowSelector.SelectedItem as GameWindowTarget)?.Handle ?? nint.Zero;
        _overlayWindow.FollowTarget = FollowGameCheckBox.IsChecked == true;
        _overlayWindow.HideWhenTargetInactive = HideInactiveCheckBox.IsChecked == true;
        _overlayWindow.RefreshPosition();
    }

    private void ToggleOverlayButton_Click(object sender, RoutedEventArgs e) => ToggleOverlay();

    private void OpenTrainingWindow_Click(object sender, RoutedEventArgs e)
    {
        if (_trainingWindow is { IsLoaded: true })
        {
            _trainingWindow.Activate();
            return;
        }
        _trainingWindow = new TrainingWindow(_telemetrySource.Stop, _telemetrySource.Start)
        {
            Owner = this,
        };
        _trainingWindow.Closed += (_, _) => _trainingWindow = null;
        _trainingWindow.Show();
    }

    private void ToggleOverlay()
    {
        EnsureOverlayWindow();
        if (_overlayWindow!.IsVisible)
        {
            _overlayWindow.Hide();
            ToggleOverlayButton.Content = "显示悬浮层";
        }
        else
        {
            _overlayWindow.Show();
            _overlayWindow.RefreshPosition();
            _overlayWindow.ShowEncouragement();
            ToggleOverlayButton.Content = "隐藏悬浮层";
        }
    }

    private void EnsureOverlayWindow()
    {
        if (_overlayWindow is not null)
        {
            return;
        }

        _overlayWindow = new OverlayWindow(_viewModel, _gameWindowService);
        if (ProfileSelector.SelectedItem is GameProfile profile)
        {
            _overlayWindow.ApplyProfile(profile);
            _overlayWindow.ApplyPlacement(_currentOverlayPlacement);
        }
        UpdateOverlayOptions();
    }

    private void EncouragementButton_Click(object sender, RoutedEventArgs e)
    {
        EnsureOverlayWindow();
        if (!_overlayWindow!.IsVisible)
        {
            _overlayWindow.Show();
            ToggleOverlayButton.Content = "隐藏悬浮层";
        }
        _overlayWindow.ShowEncouragement();
        AnimatePreviewToast();
    }

    private void ApplyPreviewPosition(OverlayPlacement placement)
    {
        var anchor = OverlayPlacement.NormalizeAnchor(placement.Anchor);
        PreviewOverlayCard.HorizontalAlignment = anchor switch
        {
            "TopLeft" or "CenterLeft" or "BottomLeft" => HorizontalAlignment.Left,
            "TopCenter" or "Center" or "BottomCenter" => HorizontalAlignment.Center,
            _ => HorizontalAlignment.Right,
        };
        PreviewOverlayCard.VerticalAlignment = anchor switch
        {
            "CenterLeft" or "Center" or "CenterRight" => VerticalAlignment.Center,
            "BottomLeft" or "BottomCenter" or "BottomRight" => VerticalAlignment.Bottom,
            _ => VerticalAlignment.Top,
        };
        PreviewOverlayCard.Margin = new Thickness(28, 52, 28, 28);
        PreviewOverlayCard.RenderTransform = new TranslateTransform(
            placement.OffsetX * 0.22,
            placement.OffsetY * 0.22);

        PreviewToast.HorizontalAlignment = PreviewOverlayCard.HorizontalAlignment switch
        {
            HorizontalAlignment.Left => HorizontalAlignment.Right,
            HorizontalAlignment.Right => HorizontalAlignment.Left,
            _ => HorizontalAlignment.Center,
        };
        PreviewToast.Margin = new Thickness(32, 62, 32, 0);
    }

    private void AnimatePreviewToast()
    {
        var animation = new DoubleAnimationUsingKeyFrames
        {
            Duration = TimeSpan.FromSeconds(2.6),
            KeyFrames =
            {
                new DiscreteDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)),
                new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(180))),
                new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(2.1))),
                new EasingDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(2.55))),
            },
        };
        PreviewToast.BeginAnimation(OpacityProperty, animation);
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.LeftButton == MouseButtonState.Pressed)
        {
            DragMove();
        }
    }

    private void MinimizeButton_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    private void CloseButton_Click(object sender, RoutedEventArgs e) => HideToTray();

    private static void CaptureElementToPng(FrameworkElement element, string path)
    {
        var dpi = VisualTreeHelper.GetDpi(element);
        var width = Math.Max(1, (int)Math.Ceiling(element.ActualWidth * dpi.DpiScaleX));
        var height = Math.Max(1, (int)Math.Ceiling(element.ActualHeight * dpi.DpiScaleY));
        var bitmap = new RenderTargetBitmap(width, height, dpi.PixelsPerInchX, dpi.PixelsPerInchY, PixelFormats.Pbgra32);
        bitmap.Render(element);

        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrWhiteSpace(directory))
        {
            Directory.CreateDirectory(directory);
        }

        var encoder = new PngBitmapEncoder();
        encoder.Frames.Add(BitmapFrame.Create(bitmap));
        using var stream = File.Create(path);
        encoder.Save(stream);
    }

    [DllImport("user32.dll")]
    private static extern bool RegisterHotKey(nint hwnd, int id, uint modifiers, uint virtualKey);

    [DllImport("user32.dll")]
    private static extern bool UnregisterHotKey(nint hwnd, int id);

    [DllImport("user32.dll")]
    private static extern bool DestroyIcon(nint iconHandle);
}
