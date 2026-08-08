using System.Text.Json.Serialization;

namespace MoveToPlay.Companion.Models;

public sealed class CloudFileState
{
    [JsonPropertyName("filename")]
    public string Filename { get; init; } = "";

    [JsonPropertyName("expected_bytes")]
    public long ExpectedBytes { get; init; }

    [JsonPropertyName("received_bytes")]
    public long ReceivedBytes { get; init; }

    [JsonPropertyName("sha256")]
    public string Sha256 { get; init; } = "";
}

public sealed class CloudDataset
{
    [JsonPropertyName("id")]
    public string Id { get; init; } = "";

    [JsonPropertyName("name")]
    public string Name { get; init; } = "";

    [JsonPropertyName("status")]
    public string Status { get; init; } = "";

    [JsonPropertyName("files")]
    public Dictionary<string, CloudFileState> Files { get; init; } = [];

    [JsonPropertyName("error")]
    public string? Error { get; init; }
}

public sealed class CloudJob
{
    [JsonPropertyName("id")]
    public string Id { get; init; } = "";

    [JsonPropertyName("dataset_id")]
    public string DatasetId { get; init; } = "";

    [JsonPropertyName("mode")]
    public string Mode { get; init; } = "";

    [JsonPropertyName("status")]
    public string Status { get; init; } = "";

    [JsonPropertyName("error")]
    public string? Error { get; init; }

    [JsonPropertyName("approved_at")]
    public string? ApprovedAt { get; init; }

    [JsonPropertyName("approved_by")]
    public string? ApprovedBy { get; init; }
}

public sealed class CloudArtifact
{
    [JsonPropertyName("path")]
    public string Path { get; init; } = "";

    [JsonPropertyName("bytes")]
    public long Bytes { get; init; }

    [JsonPropertyName("sha256")]
    public string? Sha256 { get; init; }
}

public sealed record CloudUploadProgress(
    string Stage,
    long CompletedBytes,
    long TotalBytes,
    double Percent);
