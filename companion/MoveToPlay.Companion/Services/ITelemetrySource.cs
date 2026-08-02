using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public interface ITelemetrySource
{
    event EventHandler<TelemetrySnapshot>? SnapshotChanged;
    void Start();
    void Stop();
}
