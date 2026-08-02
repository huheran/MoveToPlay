namespace MoveToPlay.Companion.Models;

public sealed class GameProfile
{
    public string Id { get; set; } = "generic";
    public int SortOrder { get; set; } = 100;
    public bool IsDefault { get; set; }
    public string DisplayName { get; set; } = "通用运动界面";
    public string Subtitle { get; set; } = "MOVE TO PLAY";
    public string[] ProcessNames { get; set; } = [];
    public WindowMatchRule[] WindowMatchRules { get; set; } = [];
    public string Accent { get; set; } = "#6EE7B7";
    public string AccentSecondary { get; set; } = "#22D3EE";
    public string PanelBackground { get; set; } = "#E6101724";
    public string TextPrimary { get; set; } = "#FFF8FAFC";
    public string TextMuted { get; set; } = "#FF94A3B8";
    public string OverlayPosition { get; set; } = "TopRight";
    public string HudFontFamily { get; set; } = "Microsoft YaHei UI";
    public string StatusText { get; set; } = "ONLINE";
    public string ActionLabel { get; set; } = "当前动作";
    public string GoalLabel { get; set; } = "今日运动目标";
    public string MarkerGlyph { get; set; } = "✦";
    public bool SquareProgress { get; set; }
    public string IdleMessage { get; set; } = "身体在线 · 动作识别稳定";
    public string[] Encouragements { get; set; } = ["保持节奏，继续前进！"];

    public override string ToString() => DisplayName;
}

public sealed class WindowMatchRule
{
    public string[] ProcessNames { get; set; } = [];
    public string[] TitleKeywords { get; set; } = [];
}
