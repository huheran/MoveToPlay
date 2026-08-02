using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using MoveToPlay.Companion.Models;
using MoveToPlay.Companion.Services;
using MoveToPlay.Companion.ViewModels;

namespace MoveToPlay.Companion;

public partial class MainWindow : Window
{
    private const int HotkeyId = 0x4D3250;
    private const uint ModControl = 0x0002;
    private const uint ModShift = 0x0004;
    private const uint VkF10 = 0x79;
    private const int WmHotkey = 0x0312;

    private readonly TelemetryViewModel _viewModel = new();
    private readonly ProfileService _profileService = new();
    private readonly GameWindowService _gameWindowService = new();
    private readonly ITelemetrySource _telemetrySource = new DemoTelemetryService();
    private IReadOnlyList<GameProfile> _profiles = [];
    private IReadOnlyList<GameWindowTarget> _windowTargets = [];
    private OverlayWindow? _overlayWindow;
    private HwndSource? _hwndSource;

    public MainWindow()
    {
        DataContext = _viewModel;
        InitializeComponent();

        SourceInitialized += OnSourceInitialized;
        Loaded += OnLoaded;
        ContentRendered += OnContentRendered;
        Closed += OnClosed;
        _telemetrySource.SnapshotChanged += OnSnapshotChanged;
    }

    private void OnLoaded(object sender, RoutedEventArgs e)
    {
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
        Application.Current.Shutdown();
    }

    private void OnClosed(object? sender, EventArgs e)
    {
        _telemetrySource.Stop();
        _telemetrySource.SnapshotChanged -= OnSnapshotChanged;
        _overlayWindow?.Close();

        if (_hwndSource is not null)
        {
            _hwndSource.RemoveHook(WindowMessageHook);
        }

        _ = UnregisterHotKey(new WindowInteropHelper(this).Handle, HotkeyId);
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
        _viewModel.ApplySnapshot(snapshot);
        if (ProfileSelector.SelectedItem is GameProfile profile)
        {
            if (snapshot.Celebrate && profile.Encouragements.Length > 0)
            {
                var messageIndex = Math.Abs(snapshot.Combo) % profile.Encouragements.Length;
                _viewModel.SetEncouragement(profile.Encouragements[messageIndex]);
            }
            else if (!snapshot.Celebrate && !string.IsNullOrWhiteSpace(profile.IdleMessage))
            {
                _viewModel.SetEncouragement(profile.IdleMessage);
            }
        }

        if (snapshot.Celebrate)
        {
            _overlayWindow?.ShowEncouragement();
            AnimatePreviewToast();
        }
    }

    private void ProfileSelector_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (ProfileSelector.SelectedItem is not GameProfile profile)
        {
            return;
        }

        _viewModel.ApplyProfile(profile);
        _viewModel.SetEncouragement(profile.IdleMessage);
        _overlayWindow?.ApplyProfile(profile);
        ApplyPreviewPosition(profile);

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

    private void WindowSelector_SelectionChanged(object sender, SelectionChangedEventArgs e) => UpdateOverlayOptions();

    private void OverlayOptionChanged(object sender, RoutedEventArgs e) => UpdateOverlayOptions();

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

    private void ApplyPreviewPosition(GameProfile profile)
    {
        var cardOnLeft = profile.OverlayPosition.Equals("TopLeft", StringComparison.OrdinalIgnoreCase);
        PreviewOverlayCard.HorizontalAlignment = cardOnLeft ? HorizontalAlignment.Left : HorizontalAlignment.Right;
        PreviewToast.HorizontalAlignment = cardOnLeft ? HorizontalAlignment.Right : HorizontalAlignment.Left;
        PreviewToast.Margin = cardOnLeft ? new Thickness(0, 62, 32, 0) : new Thickness(32, 62, 0, 0);
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

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

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
}
