namespace MoveToPlay.Companion.Models;

public sealed record TelemetrySourceStatus(
    bool Connected,
    string StatusText,
    string Detail);
