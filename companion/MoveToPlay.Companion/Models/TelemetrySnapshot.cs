namespace MoveToPlay.Companion.Models;

public sealed record DeviceBatterySnapshot(
    int? Chest,
    int? RightHand,
    int? LeftHand,
    int? Leg,
    int? Blade);

public sealed record TelemetrySnapshot(
    string ActionName,
    string ActionHint,
    int? HeartRate,
    double Calories,
    TimeSpan ActiveTime,
    int GoalProgress,
    int Combo,
    string Encouragement,
    bool Celebrate,
    int ConfidencePercent,
    int IntensityPercent,
    int TrackerOnline,
    int SignalQuality,
    bool BladeOnline,
    DeviceBatterySnapshot Batteries);
