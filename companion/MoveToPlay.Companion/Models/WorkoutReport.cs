using System.Text.Json.Serialization;

namespace MoveToPlay.Companion.Models;

public enum AdventureGoalType
{
    ActiveMinutes = 0,
    Calories = 1,
    Actions = 2,
}

public sealed record AdventureGoalSettings(AdventureGoalType Type, double Target);

[JsonConverter(typeof(JsonStringEnumConverter))]
public enum HeartRateValueSource
{
    NotAvailable = 0,
    Measured = 1,
    EstimatedFromActivity = 2,
}

public sealed class WorkoutReport
{
    public string Id { get; set; } = string.Empty;
    public DateTimeOffset StartedAt { get; set; }
    public DateTimeOffset EndedAt { get; set; }
    public string GameProfile { get; set; } = string.Empty;
    public AdventureGoalType GoalType { get; set; }
    public double GoalTarget { get; set; }
    public int GoalProgressPercent { get; set; }
    public double TotalDurationSeconds { get; set; }
    public double ActiveDurationSeconds { get; set; }
    public double EstimatedCalories { get; set; }
    public int? PostWorkoutHeartRate { get; set; }
    public HeartRateValueSource PostWorkoutHeartRateSource { get; set; }
    public string? PostWorkoutHeartRateNote { get; set; }
    public uint ActionCount { get; set; }
    public int MaxCombo { get; set; }
    public int AverageIntensityPercent { get; set; }
    public int MaxIntensityPercent { get; set; }
    public Dictionary<string, uint> ActionBreakdown { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}
