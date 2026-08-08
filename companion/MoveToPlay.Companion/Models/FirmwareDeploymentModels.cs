namespace MoveToPlay.Companion.Models;

public sealed record FirmwareBuildPackage(
    string JobId,
    string WorkspacePath,
    string BuildPath,
    string AppBinaryPath,
    string ManifestPath,
    long AppBytes);

public sealed record FirmwareDeploymentProgress(string Stage, string Detail);
