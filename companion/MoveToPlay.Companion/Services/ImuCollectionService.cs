using System.Globalization;
using System.IO;
using System.IO.Ports;
using System.Media;
using System.Text;
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
    private string _stateLabel = "idle";
    private string _sessionId = "";
    private long _sampleCount;
    private int _eventSequence;
    private int _eventCount;
    private TrainingEventOption? _selectedEvent;
    private BladeMarkingMode _bladeMarkingMode = BladeMarkingMode.Immediate;
    private int _bladeCountdownMs = 1000;
    private int _bladeLatencyCompensationMs = 50;
    private bool _bladeCountdownPending;
    private double? _dongleToPcOffsetMs;
    private readonly Dictionary<int, long> _lastNodeSeen = [];

    public event EventHandler<ImuCollectionStatus>? StatusChanged;

    public string? SamplesPath { get; private set; }

    public string? EventsPath { get; private set; }

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
        int latencyCompensationMs)
    {
        lock (_gate)
        {
            _selectedEvent = selectedEvent;
            _bladeMarkingMode = mode;
            _bladeCountdownMs = Math.Clamp(countdownMs, 300, 5000);
            _bladeLatencyCompensationMs = Math.Clamp(latencyCompensationMs, -500, 500);
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
        _sampleCount = 0;
        _eventSequence = 0;
        _eventCount = 0;
        _bladeCountdownPending = false;
        _dongleToPcOffsetMs = null;
        _lastNodeSeen.Clear();
        _samplesWriter = CreateWriter(SamplesPath);
        _eventsWriter = CreateWriter(EventsPath);
        _samplesWriter.WriteLine("pc_timestamp_ms,board_timestamp_ms,dongle_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,event_group,event_type,event_id,session_id");
        _eventsWriter.WriteLine("event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id,source,dongle_timestamp_ms,blade_sequence,latency_compensation_ms");
        _samplesWriter.Flush();
        _eventsWriter.Flush();

        _cancellation = new CancellationTokenSource();
        _worker = Task.Run(() => ReadLoop(portName, _cancellation.Token));
    }

    public void MarkEvent(TrainingEventOption marker)
    {
        RecordEvent(
            marker,
            DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
            "manual",
            null,
            null,
            0);
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
            _samplesWriter?.Dispose();
            _eventsWriter?.Dispose();
            _samplesWriter = null;
            _eventsWriter = null;
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
        double? dongleTimestamp = null;
        if (parts.Length >= 10 &&
            double.TryParse(parts[9], NumberStyles.Float, CultureInfo.InvariantCulture, out var parsedDongleTimestamp))
        {
            dongleTimestamp = parsedDongleTimestamp;
        }
        var now = dongleTimestamp.HasValue ?
            MapDongleTimestamp(dongleTimestamp.Value, receiptTime) :
            receiptTime;
        lock (_gate)
        {
            if (_samplesWriter is null)
            {
                return;
            }
            _samplesWriter.WriteLine(string.Join(',',
                now.ToString(CultureInfo.InvariantCulture),
                boardTimestamp.ToString("0.###", CultureInfo.InvariantCulture),
                dongleTimestamp?.ToString("0.###", CultureInfo.InvariantCulture) ?? "",
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
        StatusChanged?.Invoke(this, new ImuCollectionStatus(
            running,
            detail,
            Interlocked.Read(ref _sampleCount),
            online,
            Volatile.Read(ref _eventCount)));
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
        var alignedTime = MapDongleTimestamp(dongleTimestamp, receiptTime);
        TrainingEventOption? selected;
        BladeMarkingMode mode;
        int countdownMs;
        int compensationMs;
        lock (_gate)
        {
            selected = _selectedEvent;
            mode = _bladeMarkingMode;
            countdownMs = _bladeCountdownMs;
            compensationMs = _bladeLatencyCompensationMs;
        }

        if (selected is null)
        {
            Publish(true, "收到 Blade 单击，但尚未预选动作事件");
            return true;
        }

        if (mode == BladeMarkingMode.Immediate)
        {
            RecordEvent(
                selected,
                alignedTime - compensationMs,
                "blade_immediate",
                dongleTimestamp,
                bladeSequence,
                compensationMs);
            Publish(true, $"Blade 已标记：{selected.DisplayName}");
            return true;
        }

        CancellationToken cancellationToken;
        lock (_gate)
        {
            if (_bladeCountdownPending)
            {
                Publish(true, "倒计时尚未结束，本次 Blade 单击已忽略");
                return true;
            }
            _bladeCountdownPending = true;
            cancellationToken = _cancellation?.Token ?? CancellationToken.None;
        }
        Publish(true, $"{selected.DisplayName}：{countdownMs / 1000.0:0.0} 秒后听提示开始动作");
        _ = RunCountdownMarkerAsync(selected, countdownMs, cancellationToken);
        return true;
    }

    private async Task RunCountdownMarkerAsync(
        TrainingEventOption marker,
        int countdownMs,
        CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(countdownMs, cancellationToken);
            SystemSounds.Asterisk.Play();
            RecordEvent(
                marker,
                DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(),
                "blade_countdown",
                null,
                null,
                0);
            Publish(true, $"提示音已响并标记：{marker.DisplayName}");
        }
        catch (OperationCanceledException)
        {
            // Stopping collection cancels a pending countdown.
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

    private long MapDongleTimestamp(double dongleTimestampMs, long receiptTimeMs)
    {
        lock (_gate)
        {
            var candidateOffset = receiptTimeMs - dongleTimestampMs;
            if (!_dongleToPcOffsetMs.HasValue || candidateOffset < _dongleToPcOffsetMs.Value)
            {
                _dongleToPcOffsetMs = candidateOffset;
            }
            return (long)Math.Round(dongleTimestampMs + _dongleToPcOffsetMs.Value);
        }
    }

    private void RecordEvent(
        TrainingEventOption marker,
        long timestampMs,
        string source,
        double? dongleTimestampMs,
        uint? bladeSequence,
        int latencyCompensationMs)
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
                Csv(Volatile.Read(ref _stateLabel)), Csv(_sessionId), Csv(source),
                dongleTimestampMs?.ToString("0.###", CultureInfo.InvariantCulture) ?? "",
                bladeSequence?.ToString(CultureInfo.InvariantCulture) ?? "",
                latencyCompensationMs.ToString(CultureInfo.InvariantCulture)));
            _eventsWriter.Flush();
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
