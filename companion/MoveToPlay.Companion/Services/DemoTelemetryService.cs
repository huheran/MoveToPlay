using System.Windows.Threading;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class DemoTelemetryService : ITelemetrySource
{
    private sealed record DemoAction(string Name, string Hint, double Met, int BaseHeartRate);

    private readonly DemoAction[] _actions =
    [
        new("行走", "稳定步频", 3.8, 108),
        new("奔跑", "Shift + W", 8.5, 142),
        new("跳跃", "SPACE", 7.2, 151),
        new("右手挥砍", "鼠标左键", 5.6, 132),
        new("行走", "稳定步频", 3.8, 116),
        new("奥特曼光线", "Q", 6.0, 136),
    ];

    private readonly DispatcherTimer _timer = new() { Interval = TimeSpan.FromSeconds(1) };
    private readonly Random _random = new(20260726);
    private int _seconds;
    private int _actionIndex;
    private int _heartRate = 96;
    private double _calories;

    public DemoTelemetryService()
    {
        _timer.Tick += OnTick;
    }

    public event EventHandler<TelemetrySnapshot>? SnapshotChanged;
    public event EventHandler<TelemetrySourceStatus>? StatusChanged;

    public void Start()
    {
        if (_timer.IsEnabled)
        {
            return;
        }

        Publish(celebrate: true);
        StatusChanged?.Invoke(this, new TelemetrySourceStatus(true, "DEMO", "正在使用模拟运动数据"));
        _timer.Start();
    }

    public void Stop()
    {
        _timer.Stop();
        StatusChanged?.Invoke(this, new TelemetrySourceStatus(false, "OFFLINE", "模拟数据已停止"));
    }

    private void OnTick(object? sender, EventArgs e)
    {
        _seconds++;
        var celebrate = false;
        if (_seconds % 6 == 0)
        {
            _actionIndex = (_actionIndex + 1) % _actions.Length;
            celebrate = true;
        }

        var action = _actions[_actionIndex];
        _heartRate += Math.Sign(action.BaseHeartRate - _heartRate) * Math.Min(4, Math.Abs(action.BaseHeartRate - _heartRate));
        _heartRate += _random.Next(-2, 3);
        _heartRate = Math.Clamp(_heartRate, 82, 178);

        const double demoWeightKg = 68.0;
        _calories += action.Met * 3.5 * demoWeightKg / 200.0 / 60.0;
        Publish(celebrate);
    }

    private void Publish(bool celebrate)
    {
        var action = _actions[_actionIndex];
        var encouragement = celebrate
            ? action.Name switch
            {
                "奔跑" => "节奏拉满！向下一个目标冲刺",
                "跳跃" => "漂亮的跳跃，继续保持！",
                "右手挥砍" => "连续命中，动作很有力量！",
                "奥特曼光线" => "能量释放！完成一次技能动作",
                _ => "步伐稳定，正在积累运动能量",
            }
            : "身体在线 · 动作识别稳定";

        SnapshotChanged?.Invoke(this, new TelemetrySnapshot(
            action.Name,
            action.Hint,
            _heartRate,
            _calories,
            TimeSpan.FromSeconds(_seconds),
            Math.Clamp((int)(_calories / 8.0 * 100.0), 0, 100),
            12 + _seconds / 3,
            encouragement,
            celebrate,
            92,
            64,
            4,
            100,
            true,
            new DeviceBatterySnapshot(86, 92, 89, 83, 78)));
    }
}
