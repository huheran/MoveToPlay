namespace MoveToPlay.Companion.Models;

public sealed record ImuCollectionStatus(
    bool Running,
    string Detail,
    long SampleCount,
    int[] OnlineNodes,
    int EventCount);

public sealed record TrainingLabelOption(string Id, string DisplayName)
{
    public override string ToString() => DisplayName;
}

public sealed record TrainingEventOption(string Group, string Type, string DisplayName)
{
    public override string ToString() => DisplayName;
}

public enum BladeMarkingMode
{
    Immediate,
    Countdown,
}

public sealed record BladeMarkingModeOption(BladeMarkingMode Mode, string DisplayName)
{
    public override string ToString() => DisplayName;
}
