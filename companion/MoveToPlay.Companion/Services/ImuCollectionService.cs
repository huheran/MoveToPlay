using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Media;
using System.Text;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class ImuCollectionService : IDisposable
{
    private readonly object _gate = new();
    private CancellationTokenSource? _cancellation;
    private Task? _worker;
    private SerialPort? _port;
    private StreamWriter? _samplesWriter;
    private StreamWriter? _eventsWriter;
    private StreamWriter? _timingWriter;
    private string _stateLabel = "idle";
    private string _sessionId = "";
    private long _sampleCount;
    private int _eventSequence;
    private int _eventCount;
    private TrainingEventOption? _selectedEvent;
    private BladeMarkingMode _bladeMarkingMode = BladeMarkingMode.Immediate;
    private int _bladeCountdownMs = 5000;
    private int _bladeLatencyCompensationMs = 50;
    private bool _bladeCountdownPending;
    private int _automaticTargetCount = 30;
    private long _lastBladeSeen;
    private readonly Dictionary<int, long> _lastNodeSeen = [];

    public event EventHandler<ImuCollectionStatus>? StatusChanged;

    public event EventHandler<BladeCountdownStatus>? CountdownChanged;

    public event EventHandler? AutomaticSequenceCompleted;

    public string? SamplesPath { get; private set; }

    public string? EventsPath { get; private set; }

    public string? TimingDiagnosticsPath { get; private set; }

    public bool IsRunning => _worker is { IsCompleted: false };

    public static string[] GetPortNames() => SerialPort.GetPortNames()
        .OrderBy(PortNumber)
        .ThenBy(name => name, StringComparer.OrdinalIgnoreCase)
        .ToArray();

    public void SetStateLabel(string label)
    {
        if (!string.IsNullOrWhiteSpace(label))
        {
            Volatile.Write(ref _stateLabel, label.Trim());
        }
    }

    public void ConfigureBladeMarker(
        TrainingEventOption? selectedEvent,
        BladeMarkingMode mode,
        int countdownMs,
        int latencyCompensationMs,
        int automaticTargetCount)
    {
        lock (_gate)
        {
            _selectedEvent = selectedEvent;
            _bladeMarkingMode = mode;
            _bladeCountdownMs = Math.Clamp(countdownMs, 300, 5000);
            _bladeLatencyCompensationMs = Math.Clamp(latencyCompensationMs, -500, 500);
            _automaticTargetCount = Math.Clamp(automaticTargetCount, 1, 500);
        }
    }

    public void Start(string portName, string outputDirectory)
    {
        if (IsRunning)
        {
            throw new InvalidOperationException("采集已经在运行。 ");
        }
        Directory.CreateDirectory(outputDirectory);
        _sessionId = $"session-{DateTime.Now:yyyyMMdd-HHmmss}";
        var sessionDirectory = Path.Combine(outputDirectory, _sessionId);
        Directory.CreateDirectory(sessionDirectory);
        SamplesPath = Path.Combine(sessionDirectory, "samples.csv");
        EventsPath = Path.Combine(sessionDirectory, "events.csv");
        TimingDiagnosticsPath = Path.Combine(sessionDirectory, "timing_diagnostics.csv");
        _sampleCount = 0;
        _eventSequence = 0;
        _eventCount = 0;
        _bladeCountdownPending = false;
        _lastBladeSeen = 0;
        _lastNodeSeen.Clear();
        _samplesWriter = CreateWriter(SamplesPath);
        _eventsWriter = CreateWriter(EventsPath);
        _timingWriter = CreateWriter(TimingDiagnosticsPath);
        _samplesWriter.WriteLine("pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,event_group,event_type,event_id,session_id");
        _eventsWriter.WriteLine("event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id");
        _timingWriter.WriteLine("session_id,event_type,blade_edge_timestamp_us,dongle_receive_timestamp_ms,pc_receive_timestamp_ms,blade_sequence");
        _samplesWriter.Flush();
        _eventsWriter.Flush();
        _timingWriter.Flush();

        _cancellation = new CancellationTokenSource();
        _worker = Task.Run(() => ReadLoop(portName, _cancellation.Token));
    }

    public void MarkEvent(TrainingEventOption marker)
    {
        BladeMarkingMode mode;
        lock (_gate)
        {
            mode = _bladeMarkingMode;
        }
        if (mode == BladeMarkingMode.Countdown)
        {
            Publish(true, "倒计时模式由软件自动循环，无需手动触发");
            return;
        }
        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        RecordEvent(marker, now);
    }

    public void Stop()
    {
        _cancellation?.Cancel();
        try
        {
            _port?.Close();
        }
        catch
        {
            // Device removal and cancellation can race with Close.
        }
        try
        {
            _worker?.Wait(TimeSpan.FromSeconds(2));
        }
        catch (AggregateException)
        {
            // The worker reports the useful error through StatusChanged.
        }
        lock (_gate)
        {
            _samplesWriter?.Flush();
            _eventsWriter?.Flush();
            _timingWriter?.Flush();
            _samplesWriter?.Dispose();
            _eventsWriter?.Dispose();
            _timingWriter?.Dispose();
            _samplesWriter = null;
            _eventsWriter = null;
            _timingWriter = null;
        }
        _worker = null;
        _bladeCountdownPending = false;
        _cancellation?.Dispose();
        _cancellation = null;
        Publish(false, "采集已停止，CSV 已安全保存");
    }

    public void Dispose() => Stop();

    private void ReadLoop(string portName, CancellationToken cancellationToken)
    {
        using var port = new SerialPort(portName, 115200, Parity.None, 8, StopBits.One)
        {
            Encoding = Encoding.UTF8,
            NewLine = "\n",
            ReadTimeout = 750,
            DtrEnable = true,
            RtsEnable = false,
            Handshake = Handshake.None,
        };
        try
        {
            port.Open();
            _port = port;
            Publish(true, $"{portName} 已连接，正在接收 Dongle 采集态原始数据");
            StartAutomaticCountdownSequenceIfConfigured();
            while (!cancellationToken.IsCancellationRequested && port.IsOpen)
            {
                try
                {
                    ProcessLine(port.ReadLine());
                }
                catch (TimeoutException)
                {
                    Publish(true, $"{portName} 已连接，等待 Dongle 原始 CSV");
                }
            }
        }
        catch (Exception exception) when (exception is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            if (!cancellationToken.IsCancellationRequested)
            {
                Publish(false, $"串口采集失败：{exception.Message}");
            }
        }
        finally
        {
            _port = null;
        }
    }

    private void ProcessLine(string line)
    {
        if (TryProcessDeviceStatus(line))
        {
            return;
        }
        var parts = line.Trim().Split(',');
        if (TryProcessBladeMarker(parts))
        {
            return;
        }
        if (parts.Length < 8 ||
            !double.TryParse(parts[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var boardTimestamp) ||
            !int.TryParse(parts[1], NumberStyles.Integer, CultureInfo.InvariantCulture, out var nodeId))
        {
            return;
        }
        for (var index = 2; index < 8; index++)
        {
            if (!double.TryParse(parts[index], NumberStyles.Float, CultureInfo.InvariantCulture, out _))
            {
                return;
            }
        }

        var receiptTime = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        var now = receiptTime;
        lock (_gate)
        {
            if (_samplesWriter is null)
            {
                return;
            }
            _samplesWriter.WriteLine(string.Join(',',
                now.ToString(CultureInfo.InvariantCulture),
                boardTimestamp.ToString("0.###", CultureInfo.InvariantCulture),
                nodeId.ToString(CultureInfo.InvariantCulture),
                parts[2].Trim(), parts[3].Trim(), parts[4].Trim(),
                parts[5].Trim(), parts[6].Trim(), parts[7].Trim(),
                Csv(Volatile.Read(ref _stateLabel)), "none", "none", "", Csv(_sessionId)));
            _sampleCount++;
            _lastNodeSeen[nodeId] = now;
            if (_sampleCount % 100 == 0)
            {
                _samplesWriter.Flush();
                Publish(true, "正在采集并实时写入 CSV");
            }
        }
    }

    private void Publish(bool running, string detail)
    {
        var cutoff = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - 1500;
        int[] online;
        lock (_gate)
        {
            online = _lastNodeSeen.Where(pair => pair.Value >= cutoff).Select(pair => pair.Key).Order().ToArray();
        }
        var bladeOnline = Interlocked.Read(ref _lastBladeSeen) >= cutoff;
        StatusChanged?.Invoke(this, new ImuCollectionStatus(
            running,
            detail,
            Interlocked.Read(ref _sampleCount),
            online,
            Volatile.Read(ref _eventCount),
            bladeOnline));
    }

    public void RefreshStatus() => Publish(IsRunning, "设备状态已手动刷新");

    private bool TryProcessDeviceStatus(string line)
    {
        if (string.IsNullOrWhiteSpace(line) || line[0] != '{')
        {
            return false;
        }
        try
        {
            using var document = JsonDocument.Parse(line);
            var root = document.RootElement;
            if (!root.TryGetProperty("source", out var source) ||
                source.GetString() != "MoveToPlay-Dongle" ||
                !root.TryGetProperty("type", out var type) ||
                type.GetString() != "state")
            {
                return false;
            }
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var trackerMask = root.TryGetProperty("tracker_mask", out var maskElement)
                ? maskElement.GetInt32()
                : 0;
            var bladeOnline = root.TryGetProperty("blade_online", out var bladeElement) &&
                bladeElement.ValueKind == JsonValueKind.True;
            lock (_gate)
            {
                for (var nodeId = 1; nodeId <= 4; nodeId++)
                {
                    if ((trackerMask & (1 << (nodeId - 1))) != 0)
                    {
                        _lastNodeSeen[nodeId] = now;
                    }
                    else
                    {
                        _lastNodeSeen.Remove(nodeId);
                    }
                }
            }
            Interlocked.Exchange(ref _lastBladeSeen, bladeOnline ? now : 0);
            Publish(true, "设备状态已更新");
            return true;
        }
        catch (Exception exception) when (exception is JsonException or InvalidOperationException or FormatException)
        {
            return false;
        }
    }

    private bool TryProcessBladeMarker(string[] parts)
    {
        if (parts.Length < 4 ||
            !parts[0].Equals("#M2P_EVENT", StringComparison.Ordinal) ||
            !parts[1].Equals("blade_click", StringComparison.Ordinal) ||
            !double.TryParse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture, out var dongleTimestamp) ||
            !uint.TryParse(parts[3], NumberStyles.Integer, CultureInfo.InvariantCulture, out var bladeSequence))
        {
            return false;
        }

        var receiptTime = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
        uint? bladeEdgeTimestampUs = null;
        if (parts.Length >= 5 &&
            uint.TryParse(parts[4], NumberStyles.Integer, CultureInfo.InvariantCulture, out var parsedBladeEdge))
        {
            bladeEdgeTimestampUs = parsedBladeEdge;
        }
        TrainingEventOption? selected;
        BladeMarkingMode mode;
        int compensationMs;
        lock (_gate)
        {
            selected = _selectedEvent;
            mode = _bladeMarkingMode;
            compensationMs = _bladeLatencyCompensationMs;
        }

        if (selected is null)
        {
            Publish(true, "收到 Blade 单击，但尚未预选动作事件");
            return true;
        }

        WriteTimingDiagnostic(selected, bladeEdgeTimestampUs, dongleTimestamp, receiptTime, bladeSequence);

        if (mode == BladeMarkingMode.Immediate)
        {
            RecordEvent(selected, receiptTime - compensationMs);
            Publish(true,
                $"Blade 已标记：{selected.DisplayName}；物理边沿 {bladeEdgeTimestampUs?.ToString() ?? "旧固件未提供"} µs，" +
                $"Dongle 收包 {dongleTimestamp:0.###} ms，电脑收包 {receiptTime} ms");
            return true;
        }

        Publish(true, "倒计时模式正在自动采集，本次 Blade 单击不额外增加标签");
        return true;
    }

    private void StartAutomaticCountdownSequenceIfConfigured()
    {
        TrainingEventOption? selected;
        BladeMarkingMode mode;
        CancellationToken cancellationToken;
        int countdownMs;
        int targetCount;
        lock (_gate)
        {
            selected = _selectedEvent;
            mode = _bladeMarkingMode;
            if (mode != BladeMarkingMode.Countdown || selected is null || _bladeCountdownPending)
            {
                return;
            }
            _bladeCountdownPending = true;
            countdownMs = _bladeCountdownMs;
            targetCount = _automaticTargetCount;
            cancellationToken = _cancellation?.Token ?? CancellationToken.None;
        }
        Publish(true, $"自动采集即将开始：计划 {targetCount} 次 {selected.DisplayName}");
        _ = RunAutomaticCountdownSequenceAsync(selected, countdownMs, targetCount, cancellationToken);
    }

    private async Task RunAutomaticCountdownSequenceAsync(
        TrainingEventOption marker,
        int countdownMs,
        int targetCount,
        CancellationToken cancellationToken)
    {
        try
        {
            for (var completed = 0; completed < targetCount; completed++)
            {
                var startedAt = Environment.TickCount64;
                var previousSecond = -1;
                while (true)
                {
                    var elapsed = (int)Math.Min(int.MaxValue, Environment.TickCount64 - startedAt);
                    var remaining = Math.Max(0, countdownMs - elapsed);
                    var second = (int)Math.Ceiling(remaining / 1000.0);
                    if (second != previousSecond)
                    {
                        previousSecond = second;
                        CountdownChanged?.Invoke(this,
                            new BladeCountdownStatus(marker.DisplayName, remaining, IsGo: false, IsCompleted: false,
                                CompletedCount: completed, TargetCount: targetCount));
                    }
                    if (remaining <= 0)
                    {
                        break;
                    }
                    await Task.Delay(Math.Min(50, remaining), cancellationToken);
                }
                SystemSounds.Asterisk.Play();
                CountdownChanged?.Invoke(this,
                    new BladeCountdownStatus(marker.DisplayName, 0, IsGo: true, IsCompleted: false,
                        CompletedCount: completed, TargetCount: targetCount));
                await Task.Delay(500, cancellationToken);
                RecordEvent(marker, DateTimeOffset.UtcNow.ToUnixTimeMilliseconds());
                var completedNow = completed + 1;
                CountdownChanged?.Invoke(this,
                    new BladeCountdownStatus(marker.DisplayName, 0, IsGo: false, IsCompleted: true,
                        CompletedCount: completedNow, TargetCount: targetCount));
                Publish(true, $"已完成 {completedNow}/{targetCount} 次，剩余 {targetCount - completedNow} 次");
                if (completedNow < targetCount)
                {
                    await Task.Delay(650, cancellationToken);
                }
            }
            AutomaticSequenceCompleted?.Invoke(this, EventArgs.Empty);
        }
        catch (OperationCanceledException)
        {
            CountdownChanged?.Invoke(this,
                new BladeCountdownStatus(marker.DisplayName, 0, IsGo: false, IsCompleted: false, IsCancelled: true));
        }
        catch (InvalidOperationException)
        {
            // Collection may stop between the delay and writing the marker.
        }
        finally
        {
            lock (_gate)
            {
                _bladeCountdownPending = false;
            }
        }
    }

    private void RecordEvent(TrainingEventOption marker, long timestampMs)
    {
        lock (_gate)
        {
            if (_eventsWriter is null || !IsRunning)
            {
                throw new InvalidOperationException("请先开始采集。 ");
            }
            _eventSequence++;
            _eventCount++;
            var eventId = $"{_sessionId}-event-{_eventSequence:D5}";
            _eventsWriter.WriteLine(string.Join(',',
                Csv(eventId), Csv(marker.Group), Csv(marker.Type),
                timestampMs.ToString(CultureInfo.InvariantCulture),
                Csv(Volatile.Read(ref _stateLabel)), Csv(_sessionId)));
            _eventsWriter.Flush();
        }
    }

    private void WriteTimingDiagnostic(
        TrainingEventOption marker,
        uint? bladeEdgeTimestampUs,
        double dongleReceiveTimestampMs,
        long pcReceiveTimestampMs,
        uint bladeSequence)
    {
        lock (_gate)
        {
            if (_timingWriter is null)
            {
                return;
            }
            _timingWriter.WriteLine(string.Join(',',
                Csv(_sessionId), Csv(marker.Type),
                bladeEdgeTimestampUs?.ToString(CultureInfo.InvariantCulture) ?? "",
                dongleReceiveTimestampMs.ToString("0.###", CultureInfo.InvariantCulture),
                pcReceiveTimestampMs.ToString(CultureInfo.InvariantCulture),
                bladeSequence.ToString(CultureInfo.InvariantCulture)));
            _timingWriter.Flush();
        }
    }

    private static StreamWriter CreateWriter(string path) => new(
        new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read, 64 * 1024),
        new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

    private static string Csv(string value) =>
        value.IndexOfAny([',', '"', '\r', '\n']) < 0 ? value : $"\"{value.Replace("\"", "\"\"")}\"";

    private static int PortNumber(string name) =>
        int.TryParse(new string(name.Where(char.IsDigit).ToArray()), out var number) ? number : int.MaxValue;
}
