namespace MoveToPlay.Companion.Models;

public sealed record TelemetrySnapshot(
    string ActionName,
    string ActionHint,
    int HeartRate,
    double Calories,
    TimeSpan ActiveTime,
    int GoalProgress,
    int Combo,
    string Encouragement,
    bool Celebrate);
