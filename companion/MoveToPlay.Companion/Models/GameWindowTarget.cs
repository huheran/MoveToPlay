namespace MoveToPlay.Companion.Models;

public sealed record GameWindowTarget(nint Handle, string Title, string ProcessName)
{
    public string DisplayName => Handle == nint.Zero
        ? Title
        : $"{Title}  ·  {ProcessName}";

    public override string ToString() => DisplayName;
}
