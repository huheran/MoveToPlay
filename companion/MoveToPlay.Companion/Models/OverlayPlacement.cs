namespace MoveToPlay.Companion.Models;

public sealed class OverlayPlacement
{
    public string Anchor { get; set; } = "TopRight";
    public double OffsetX { get; set; }
    public double OffsetY { get; set; }

    public OverlayPlacement Clone() => new()
    {
        Anchor = Anchor,
        OffsetX = OffsetX,
        OffsetY = OffsetY,
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
