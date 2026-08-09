namespace MoveToPlay.Companion.Models;

public sealed record DeviceBatterySnapshot(
    int? Chest,
    int? RightHand,
    int? LeftHand,
    int? Leg,
    int? Blade);

public enum HeartRateMeasurementState
{
    Off = 0,
    WaitingForFinger = 1,
    Measuring = 2,
    Complete = 3,
    Failed = 4,
}

public sealed record TelemetrySnapshot(
    string ActionName,
    string ActionHint,
    int? HeartRate,
    HeartRateMeasurementState HeartRateState,
    int HeartRateRemainingSeconds,
    double Calories,
    TimeSpan ActiveTime,
    int GoalProgress,
    int Combo,
    string Encouragement,
    bool Celebrate,
    int ConfidencePercent,
    int IntensityPercent,
    bool Active,
    uint EventCount,
    string EventAction,
    int TrackerOnline,
    int SignalQuality,
    bool BladeOnline,
    DeviceBatterySnapshot Batteries);
