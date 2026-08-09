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
        if (status.IsPreparation)
        {
            CountdownText.Text = status.IsGo
                ? "开始"
                : Math.Max(1, (int)Math.Ceiling(status.RemainingMilliseconds / 1000.0)).ToString();
            CountdownText.FontSize = status.IsGo ? 92 : 138;
            CountdownText.Foreground = Brush(status.IsGo ? "#FF34D399" : "#FF2DD4BF");
            CountdownCard.BorderBrush = Brush(status.IsGo ? "#FF34D399" : "#FF2DD4BF");
            HintText.Text = status.IsGo ? "采集已经开始" : "请站好并做好准备";
            return;
        }
        if (status.IsCompleted)
        {
            CountdownText.Text = status.TargetCount > 0
                ? $"{status.CompletedCount}/{status.TargetCount}"
                : "已标记";
            CountdownText.FontSize = 72;
            CountdownText.Foreground = Brush("#FF34D399");
            CountdownCard.BorderBrush = Brush("#FF34D399");
            HintText.Text = status.TargetCount > 0 && status.CompletedCount < status.TargetCount
                ? $"已记录，剩余 {status.TargetCount - status.CompletedCount} 次"
                : "动作标签已写入 CSV";
            return;
        }
        if (status.IsGo)
        {
            CountdownText.Text = "0";
            CountdownText.FontSize = 184;
            CountdownText.Foreground = Brush("#FFFFB020");
            CountdownCard.BorderBrush = Brush("#FFFFB020");
            HintText.Text = status.TargetCount > 0
                ? $"开始动作！0.5 秒后记录 · 剩余 {status.TargetCount - status.CompletedCount} 次"
                : "开始动作！0.5 秒后记录标签";
            return;
        }

        CountdownText.Text = Math.Max(1, (int)Math.Ceiling(status.RemainingMilliseconds / 1000.0)).ToString();
        CountdownText.FontSize = 138;
        CountdownText.Foreground = Brush("#FF2DD4BF");
        CountdownCard.BorderBrush = Brush("#FF2DD4BF");
        HintText.Text = status.TargetCount > 0
            ? $"准备第 {status.CompletedCount + 1}/{status.TargetCount} 次动作"
            : "准备动作";
    }

    private static Brush Brush(string color) =>
        (Brush)new BrushConverter().ConvertFromString(color)!;
}
