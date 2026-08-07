using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public interface ITelemetrySource
{
    event EventHandler<TelemetrySnapshot>? SnapshotChanged;
    event EventHandler<TelemetrySourceStatus>? StatusChanged;
    void Start();
    void Stop();
}
