using System.Windows;
using System.Windows.Media;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion;

public partial class CollectionCountdownWindow : Window
{
    public CollectionCountdownWindow()
    {
        InitializeComponent();
    }

    public void UpdateStatus(BladeCountdownStatus status)
    {
        ActionNameText.Text = status.ActionDisplayName;
        if (status.IsCompleted)
        {
            CountdownText.Text = "已标记";
            CountdownText.FontSize = 72;
            CountdownText.Foreground = Brush("#FF34D399");
            CountdownCard.BorderBrush = Brush("#FF34D399");
            HintText.Text = "动作标签已写入 CSV";
            return;
        }
        if (status.IsGo)
        {
            CountdownText.Text = "0";
            CountdownText.FontSize = 184;
            CountdownText.Foreground = Brush("#FFFFB020");
            CountdownCard.BorderBrush = Brush("#FFFFB020");
            HintText.Text = "开始动作！0.5 秒后记录标签";
            return;
        }

        CountdownText.Text = Math.Max(1, (int)Math.Ceiling(status.RemainingMilliseconds / 1000.0)).ToString();
        CountdownText.FontSize = 138;
        CountdownText.Foreground = Brush("#FF2DD4BF");
        CountdownCard.BorderBrush = Brush("#FF2DD4BF");
        HintText.Text = "准备动作";
    }

    private static Brush Brush(string color) =>
        (Brush)new BrushConverter().ConvertFromString(color)!;
}
