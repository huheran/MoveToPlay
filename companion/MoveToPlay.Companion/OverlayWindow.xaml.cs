using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using MoveToPlay.Companion.Models;
using MoveToPlay.Companion.Services;
using MoveToPlay.Companion.ViewModels;

namespace MoveToPlay.Companion;

public partial class OverlayWindow : Window
{
    private const int GwlExStyle = -20;
    private const int WsExTransparent = 0x00000020;
    private const int WsExToolWindow = 0x00000080;
    private const int WsExLayered = 0x00080000;
    private const int WsExNoActivate = 0x08000000;

    private readonly TelemetryViewModel _viewModel;
    private readonly GameWindowService _windowService;
    private readonly DispatcherTimer _positionTimer = new() { Interval = TimeSpan.FromMilliseconds(250) };
    private nint _windowHandle;
    private GameProfile? _profile;

    public OverlayWindow(TelemetryViewModel viewModel, GameWindowService windowService)
    {
        _viewModel = viewModel;
        _windowService = windowService;
        DataContext = viewModel;
        InitializeComponent();

        SourceInitialized += OnSourceInitialized;
        _positionTimer.Tick += (_, _) => RefreshPosition();
        Loaded += (_, _) =>
        {
            _positionTimer.Start();
            RefreshPosition();
        };
        Closed += (_, _) => _positionTimer.Stop();
    }

    public nint TargetHandle { get; set; }
    public bool FollowTarget { get; set; } = true;
    public bool HideWhenTargetInactive { get; set; } = true;

    public void ApplyProfile(GameProfile profile)
    {
        _profile = profile;
        if (profile.OverlayPosition.Equals("TopLeft", StringComparison.OrdinalIgnoreCase))
        {
            OverlayCard.HorizontalAlignment = HorizontalAlignment.Left;
        }
        else
        {
            OverlayCard.HorizontalAlignment = HorizontalAlignment.Right;
        }
    }

    public void RefreshPosition()
    {
        if (_windowHandle == nint.Zero)
        {
            return;
        }

        if (FollowTarget && TargetHandle != nint.Zero && _windowService.TryGetClientBounds(TargetHandle, out var bounds))
        {
            _windowService.PlaceTopmost(_windowHandle, bounds);
            OverlayRoot.Opacity = !HideWhenTargetInactive || _windowService.IsTargetForeground(TargetHandle) ? 1.0 : 0.0;
            return;
        }

        OverlayRoot.Opacity = 1.0;
        var previewBounds = new GameWindowService.WindowBounds(
            (int)SystemParameters.VirtualScreenLeft,
            (int)SystemParameters.VirtualScreenTop,
            (int)SystemParameters.PrimaryScreenWidth,
            (int)SystemParameters.PrimaryScreenHeight);
        _windowService.PlaceTopmost(_windowHandle, previewBounds);
    }

    public void ShowEncouragement()
    {
        var opacity = new DoubleAnimationUsingKeyFrames
        {
            Duration = TimeSpan.FromSeconds(3.1),
            KeyFrames =
            {
                new DiscreteDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.Zero)),
                new EasingDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(220))),
                new LinearDoubleKeyFrame(1, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(2.55))),
                new EasingDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(3.05))),
            },
        };
        Storyboard.SetTarget(opacity, EncouragementToast);
        Storyboard.SetTargetProperty(opacity, new PropertyPath(OpacityProperty));

        var movement = new DoubleAnimationUsingKeyFrames
        {
            Duration = opacity.Duration,
            KeyFrames =
            {
                new DiscreteDoubleKeyFrame(-18, KeyTime.FromTimeSpan(TimeSpan.Zero)),
                new EasingDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromMilliseconds(260))),
                new LinearDoubleKeyFrame(0, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(2.55))),
                new EasingDoubleKeyFrame(-10, KeyTime.FromTimeSpan(TimeSpan.FromSeconds(3.05))),
            },
        };
        Storyboard.SetTarget(movement, EncouragementToast);
        Storyboard.SetTargetProperty(movement, new PropertyPath("(UIElement.RenderTransform).(TranslateTransform.Y)"));

        var storyboard = new Storyboard();
        storyboard.Children.Add(opacity);
        storyboard.Children.Add(movement);
        storyboard.Begin(this, true);
    }

    private void OnSourceInitialized(object? sender, EventArgs e)
    {
        _windowHandle = new WindowInteropHelper(this).Handle;
        var style = GetWindowLongPtr(_windowHandle, GwlExStyle).ToInt64();
        style |= WsExTransparent | WsExToolWindow | WsExLayered | WsExNoActivate;
        _ = SetWindowLongPtr(_windowHandle, GwlExStyle, new nint(style));
    }

    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")]
    private static extern nint GetWindowLongPtr(nint hwnd, int index);

    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")]
    private static extern nint SetWindowLongPtr(nint hwnd, int index, nint newStyle);
}
