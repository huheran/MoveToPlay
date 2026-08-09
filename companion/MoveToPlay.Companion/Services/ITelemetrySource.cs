using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public interface ITelemetrySource
{
    event EventHandler<TelemetrySnapshot>? SnapshotChanged;
    event EventHandler<TelemetrySourceStatus>? StatusChanged;
    bool StartHeartRateMeasurement(int durationSeconds);
    bool StopHeartRateMeasurement();
    void Start();
    void Stop();
}
