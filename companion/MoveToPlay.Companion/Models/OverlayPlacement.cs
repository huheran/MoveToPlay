namespace MoveToPlay.Companion.Models;

public sealed class OverlayPlacement
{
    public string Anchor { get; set; } = "TopRight";
    public double OffsetX { get; set; }
    public double OffsetY { get; set; }
    public double HeaderScale { get; set; } = 1.0;
    public double ActionScale { get; set; } = 1.0;
    public double MetricsScale { get; set; } = 1.0;
    public double GoalScale { get; set; } = 1.0;
    public double ToastScale { get; set; } = 1.0;

    public OverlayPlacement Clone() => new()
    {
        Anchor = Anchor,
        OffsetX = OffsetX,
        OffsetY = OffsetY,
        HeaderScale = HeaderScale,
        ActionScale = ActionScale,
        MetricsScale = MetricsScale,
        GoalScale = GoalScale,
        ToastScale = ToastScale,
    };

    public static OverlayPlacement FromProfile(GameProfile profile) => new()
    {
        Anchor = NormalizeAnchor(profile.OverlayPosition),
    };

    public static string NormalizeAnchor(string? anchor) => anchor switch
    {
        "TopLeft" => "TopLeft",
        "TopCenter" => "TopCenter",
        "TopRight" => "TopRight",
        "CenterLeft" => "CenterLeft",
        "Center" => "Center",
        "CenterRight" => "CenterRight",
        "BottomLeft" => "BottomLeft",
        "BottomCenter" => "BottomCenter",
        "BottomRight" => "BottomRight",
        _ => "TopRight",
    };
}
