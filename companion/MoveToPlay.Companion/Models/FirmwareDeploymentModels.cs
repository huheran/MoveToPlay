namespace MoveToPlay.Companion.Models;

public sealed record FirmwareBuildPackage(
    string JobId,
    string PackageDirectory,
    string ManifestPath,
    string Chip,
    string Before,
    string After,
    IReadOnlyList<string> WriteFlashArgs,
    IReadOnlyList<FirmwareFlashFile> Files,
    long AppBytes);

public sealed record FirmwareFlashFile(
    string Name,
    string Offset,
    string Path,
    long Bytes,
    string Sha256);

public sealed record FirmwareDeploymentProgress(
    string Stage,
    string Detail,
    double Percent = 0,
    bool IsIndeterminate = false);
