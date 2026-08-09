using System.IO;
using System.IO.Ports;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Microsoft.Win32;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class UsbCdcTelemetryService : ITelemetrySource
{
    private const string DongleRegistryPrefix = "VID_303A&PID_4005";
    private const double DefaultGoalCalories = 100.0;

    private sealed record ActionPresentation(string Name, string Hint, double Met);

    private sealed class DonglePacket
    {
        [JsonPropertyName("v")]
        public int Version { get; init; }

        [JsonPropertyName("source")]
        public string? Source { get; init; }

        [JsonPropertyName("seq")]
        public uint Sequence { get; init; }

        [JsonPropertyName("time_ms")]
        public ulong TimeMs { get; init; }

        [JsonPropertyName("type")]
        public string? Type { get; init; }

        [JsonPropertyName("action")]
        public string? Action { get; init; }

        [JsonPropertyName("confidence")]
        public int Confidence { get; init; }

        [JsonPropertyName("intensity")]
        public int Intensity { get; init; }

        [JsonPropertyName("active")]
        public bool Active { get; init; }

        [JsonPropertyName("event_count")]
        public uint EventCount { get; init; }

        [JsonPropertyName("event_action")]
        public string? EventAction { get; init; }

        [JsonPropertyName("event")]
        public string? Event { get; init; }

        [JsonPropertyName("tracker_online")]
        public int TrackerOnline { get; init; }

        [JsonPropertyName("quality")]
        public int Quality { get; init; }

        [JsonPropertyName("blade_online")]
        public bool BladeOnline { get; init; }

        [JsonPropertyName("battery")]
        public int[]? Battery { get; init; }
    }

    private static readonly IReadOnlyDictionary<string, ActionPresentation> Actions =
        new Dictionary<string, ActionPresentation>(StringComparer.OrdinalIgnoreCase)
        {
            ["hands_cross_forehead"] = new("双手交叉", "角色切换", 3.0),
            ["hands_press_down"] = new("双手下压", "F", 3.8),
            ["hands_shoot"] = new("双手射击", "鼠标右键", 4.3),
            ["idle"] = new("待机", "等待有效动作", 1.0),
            ["jump"] = new("跳跃", "SPACE", 7.5),
            ["kick"] = new("踢腿", "E", 5.5),
            ["left_hand_raise"] = new("左手抬起", "M", 3.0),
            ["move_noise"] = new("动作不确定", "请保持完整动作", 1.0),
            ["right_hand_raise"] = new("右手抬起", "X", 3.0),
            ["right_hand_slash"] = new("右手挥砍", "鼠标左键", 5.8),
            ["run"] = new("奔跑", "SHIFT + W", 8.3),
            ["turn_left"] = new("向左转", "视角向左", 3.5),
            ["turn_right"] = new("向右转", "视角向右", 3.5),
            ["ultraman_beam"] = new("奥特曼光线", "Q", 5.0),
            ["walk"] = new("行走", "W", 3.8),
        };

    private readonly object _portGate = new();
    private readonly string? _requestedPort;
    private readonly double _weightKg;
    private CancellationTokenSource? _cancellation;
    private Task? _worker;
    private SerialPort? _activePort;
    private string? _lastStatusKey;
    private ulong? _lastDeviceTimeMs;
    private uint _lastEventCount;
    private bool _eventCountInitialized;
    private ulong _lastEventDeviceTimeMs;
    private double _calories;
    private double _activeSeconds;
    private int _combo;

    public UsbCdcTelemetryService(string? requestedPort = null, double weightKg = 68.0)
    {
        _requestedPort = string.IsNullOrWhiteSpace(requestedPort) ? null : requestedPort.Trim();
        _weightKg = Math.Clamp(weightKg, 30.0, 250.0);
    }

    public event EventHandler<TelemetrySnapshot>? SnapshotChanged;
    public event EventHandler<TelemetrySourceStatus>? StatusChanged;

    public void Start()
    {
        if (_worker is { IsCompleted: false })
        {
            return;
        }

        _cancellation?.Dispose();
        _cancellation = new CancellationTokenSource();
        _lastStatusKey = null;
        PublishStatus(false, "SEARCHING", "正在搜索 MoveToPlay Dongle");
        _worker = Task.Run(() => RunAsync(_cancellation.Token));
    }

    public void Stop()
    {
        var cancellation = _cancellation;
        var worker = _worker;
        cancellation?.Cancel();
        lock (_portGate)
        {
            try
            {
                _activePort?.Close();
            }
            catch
            {
                // Closing a removed USB device may throw; shutdown should still continue.
            }
            _activePort = null;
        }
        if (worker is not null && worker.Id != Task.CurrentId)
        {
            try
            {
                worker.Wait(TimeSpan.FromSeconds(3));
            }
            catch (AggregateException)
            {
                // Cancellation or USB removal can complete the reader with an exception.
            }
        }
        if (ReferenceEquals(_worker, worker))
        {
            _worker = null;
        }
        if (ReferenceEquals(_cancellation, cancellation))
        {
            _cancellation = null;
        }
        cancellation?.Dispose();
        _lastStatusKey = null;
        PublishStatus(false, "OFFLINE", "Dongle 连接已关闭");
    }

    private async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var ports = FindDonglePorts();
            if (ports.Count == 0)
            {
                PublishStatus(false,
                              "SEARCHING",
                              _requestedPort is null
                                  ? "未发现 MoveToPlay Dongle，正在自动重试"
                                  : $"正在等待 {_requestedPort}");
                await DelayBeforeRetry(cancellationToken);
                continue;
            }

            var connected = false;
            foreach (var portName in ports)
            {
                if (cancellationToken.IsCancellationRequested)
                {
                    return;
                }

                if (ReadPort(portName, cancellationToken))
                {
                    connected = true;
                    break;
                }
            }

            if (!connected && !cancellationToken.IsCancellationRequested)
            {
                PublishStatus(false, "RECONNECTING", "Dongle 暂时不可用，正在重新连接");
                await DelayBeforeRetry(cancellationToken);
            }
        }
    }

    private bool ReadPort(string portName, CancellationToken cancellationToken)
    {
        using var port = new SerialPort(portName, 115200, Parity.None, 8, StopBits.One)
        {
            Encoding = Encoding.UTF8,
            NewLine = "\n",
            ReadTimeout = 750,
            WriteTimeout = 250,
            DtrEnable = true,
            RtsEnable = false,
            Handshake = Handshake.None,
        };

        try
        {
            port.Open();
            lock (_portGate)
            {
                _activePort = port;
            }
            _lastDeviceTimeMs = null;
            _eventCountInitialized = false;
            PublishStatus(true, "CONNECTED", $"{portName} 已连接，等待运动数据");

            while (!cancellationToken.IsCancellationRequested && port.IsOpen)
            {
                try
                {
                    var line = port.ReadLine().Trim();
                    if (TryProcessPacket(line, out var trackerOnline, out var quality, out var bladeOnline))
                    {
                        PublishStatus(true,
                                      "DONGLE ONLINE",
                                      $"{portName} · Tracker {trackerOnline}/4 · " +
                                      $"Blade {(bladeOnline ? "已连接" : "未连接")} · 信号 {quality}%");
                    }
                }
                catch (TimeoutException)
                {
                    // A timeout keeps cancellation responsive and is not a disconnect.
                }
            }
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            if (!cancellationToken.IsCancellationRequested)
            {
                PublishStatus(false, "RECONNECTING", $"{portName} 已断开，正在重连");
            }
        }
        finally
        {
            lock (_portGate)
            {
                if (ReferenceEquals(_activePort, port))
                {
                    _activePort = null;
                }
            }
        }

        return false;
    }

    private bool TryProcessPacket(string line,
                                  out int trackerOnline,
                                  out int quality,
                                  out bool bladeOnline)
    {
        trackerOnline = 0;
        quality = 0;
        bladeOnline = false;
        if (line.Length == 0 || line[0] != '{')
        {
            return false;
        }

        DonglePacket? packet;
        try
        {
            packet = JsonSerializer.Deserialize<DonglePacket>(line);
        }
        catch (JsonException)
        {
            return false;
        }

        if (packet is null ||
            packet.Version != 1 ||
            !string.Equals(packet.Source, "MoveToPlay-Dongle", StringComparison.Ordinal) ||
            !string.Equals(packet.Type, "state", StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(packet.Action))
        {
            return false;
        }

        trackerOnline = Math.Clamp(packet.TrackerOnline, 0, 4);
        quality = Math.Clamp(packet.Quality, 0, 100);
        bladeOnline = packet.BladeOnline;
        var confidence = Math.Clamp(packet.Confidence, 0, 100);
        var intensity = Math.Clamp(packet.Intensity, 0, 100);
        var deltaSeconds = CalculateDeltaSeconds(packet.TimeMs);
        var presentation = PresentationFor(packet.Action);

        if (packet.Active && deltaSeconds > 0)
        {
            _activeSeconds += deltaSeconds;
            var intensityFactor = 0.65 + intensity / 100.0 * 0.70;
            var adjustedMet = Math.Max(1.0, presentation.Met * intensityFactor);
            _calories += adjustedMet * 3.5 * _weightKg / 200.0 * deltaSeconds / 60.0;
        }

        var celebrate = UpdateEventCount(packet.EventCount, packet.TimeMs);
        if (!packet.Active &&
            _lastEventDeviceTimeMs > 0 &&
            packet.TimeMs > _lastEventDeviceTimeMs + 8000)
        {
            _combo = 0;
        }

        var encouragement = BuildEncouragement(packet.Action, intensity, trackerOnline, celebrate);
        SnapshotChanged?.Invoke(this, new TelemetrySnapshot(
            presentation.Name,
            presentation.Hint,
            null,
            _calories,
            TimeSpan.FromSeconds(_activeSeconds),
            Math.Clamp((int)Math.Round(_calories / DefaultGoalCalories * 100.0), 0, 100),
            _combo,
            encouragement,
            celebrate,
            confidence,
            intensity,
            trackerOnline,
            quality,
            packet.BladeOnline,
            new DeviceBatterySnapshot(
                BatteryAt(packet.Battery, 0),
                BatteryAt(packet.Battery, 1),
                BatteryAt(packet.Battery, 2),
                BatteryAt(packet.Battery, 3),
                BatteryAt(packet.Battery, 4))));
        return true;
    }

    private static int? BatteryAt(int[]? batteries, int index)
    {
        if (batteries is null || index < 0 || index >= batteries.Length)
        {
            return null;
        }

        return batteries[index] is >= 0 and <= 100 ? batteries[index] : null;
    }

    private double CalculateDeltaSeconds(ulong deviceTimeMs)
    {
        if (_lastDeviceTimeMs is null || deviceTimeMs < _lastDeviceTimeMs.Value)
        {
            _lastDeviceTimeMs = deviceTimeMs;
            return 0;
        }

        var deltaMs = deviceTimeMs - _lastDeviceTimeMs.Value;
        _lastDeviceTimeMs = deviceTimeMs;
        return Math.Clamp(deltaMs / 1000.0, 0.0, 0.5);
    }

    private bool UpdateEventCount(uint eventCount, ulong deviceTimeMs)
    {
        if (!_eventCountInitialized || eventCount < _lastEventCount)
        {
            _lastEventCount = eventCount;
            _eventCountInitialized = true;
            return false;
        }

        var delta = eventCount - _lastEventCount;
        _lastEventCount = eventCount;
        if (delta == 0)
        {
            return false;
        }

        _combo = Math.Clamp(_combo + (int)Math.Min(delta, 100U), 0, 9999);
        _lastEventDeviceTimeMs = deviceTimeMs;
        return true;
    }

    private static ActionPresentation PresentationFor(string action) =>
        Actions.TryGetValue(action, out var presentation)
            ? presentation
            : new ActionPresentation(action, "动作识别", 3.0);

    private static string BuildEncouragement(string action,
                                             int intensity,
                                             int trackerOnline,
                                             bool celebrate)
    {
        if (trackerOnline < 4)
        {
            return $"Tracker 在线 {trackerOnline}/4 · 请检查设备";
        }
        if (celebrate)
        {
            return action switch
            {
                "jump" => "漂亮的跳跃，继续保持！",
                "kick" => "踢腿完成，动作很有力量！",
                "right_hand_slash" => "挥砍命中，保持动作节奏！",
                "ultraman_beam" => "能量释放，完成技能动作！",
                _ => "动作完成，继续积累运动能量！",
            };
        }
        if (intensity >= 75)
        {
            return "高强度运动中 · 注意保持动作稳定";
        }
        if (intensity >= 40)
        {
            return "身体在线 · 当前节奏良好";
        }
        return "设备连接正常 · 等待下一个动作";
    }

    private IReadOnlyList<string> FindDonglePorts()
    {
        if (_requestedPort is not null)
        {
            return [_requestedPort];
        }

        var ports = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var usbRoot = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Enum\USB");
            if (usbRoot is null)
            {
                return [];
            }

            foreach (var deviceName in usbRoot.GetSubKeyNames()
                         .Where(name => name.StartsWith(DongleRegistryPrefix, StringComparison.OrdinalIgnoreCase)))
            {
                using var device = usbRoot.OpenSubKey(deviceName);
                if (device is null)
                {
                    continue;
                }

                foreach (var instanceName in device.GetSubKeyNames())
                {
                    using var parameters = device.OpenSubKey($@"{instanceName}\Device Parameters");
                    if (parameters?.GetValue("PortName") is string portName &&
                        !string.IsNullOrWhiteSpace(portName))
                    {
                        ports.Add(portName);
                    }
                }
            }
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException)
        {
            return [];
        }

        return ports.OrderBy(name => name, StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private void PublishStatus(bool connected, string statusText, string detail)
    {
        var key = $"{connected}|{statusText}|{detail}";
        if (string.Equals(_lastStatusKey, key, StringComparison.Ordinal))
        {
            return;
        }
        _lastStatusKey = key;
        StatusChanged?.Invoke(this, new TelemetrySourceStatus(connected, statusText, detail));
    }

    private static async Task DelayBeforeRetry(CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(TimeSpan.FromSeconds(1.5), cancellationToken);
        }
        catch (OperationCanceledException)
        {
            // Normal shutdown path.
        }
    }
}
