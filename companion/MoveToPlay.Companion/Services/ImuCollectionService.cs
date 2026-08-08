using System.Globalization;
using System.IO;
using System.IO.Ports;
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
        _lastNodeSeen.Clear();
        _samplesWriter = CreateWriter(SamplesPath);
        _eventsWriter = CreateWriter(EventsPath);
        _samplesWriter.WriteLine("pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,event_group,event_type,event_id,session_id");
        _eventsWriter.WriteLine("event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id");
        _samplesWriter.Flush();
        _eventsWriter.Flush();

        _cancellation = new CancellationTokenSource();
        _worker = Task.Run(() => ReadLoop(portName, _cancellation.Token));
    }

    public void MarkEvent(TrainingEventOption marker)
    {
        lock (_gate)
        {
            if (_eventsWriter is null || !IsRunning)
            {
                throw new InvalidOperationException("请先开始采集。 ");
            }
            _eventSequence++;
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var eventId = $"{_sessionId}-event-{_eventSequence:D5}";
            _eventsWriter.WriteLine(string.Join(',',
                Csv(eventId), Csv(marker.Group), Csv(marker.Type),
                now.ToString(CultureInfo.InvariantCulture), Csv(Volatile.Read(ref _stateLabel)), Csv(_sessionId)));
            _eventsWriter.Flush();
        }
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
            Publish(true, $"{portName} 已连接，正在接收 Data Collect 原始数据");
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

        var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
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
        StatusChanged?.Invoke(this, new ImuCollectionStatus(running, detail, Interlocked.Read(ref _sampleCount), online));
    }

    private static StreamWriter CreateWriter(string path) => new(
        new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.Read, 64 * 1024),
        new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));

    private static string Csv(string value) =>
        value.IndexOfAny([',', '"', '\r', '\n']) < 0 ? value : $"\"{value.Replace("\"", "\"\"")}\"";

    private static int PortNumber(string name) =>
        int.TryParse(new string(name.Where(char.IsDigit).ToArray()), out var number) ? number : int.MaxValue;
}
