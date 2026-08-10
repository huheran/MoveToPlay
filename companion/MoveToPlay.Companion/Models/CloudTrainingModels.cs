using System.Text.Json;
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

    [JsonPropertyName("base_dataset_id")]
    public string? BaseDatasetId { get; init; }

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

    [JsonPropertyName("created_at")]
    public string? CreatedAt { get; init; }

    [JsonPropertyName("started_at")]
    public string? StartedAt { get; init; }

    [JsonPropertyName("finished_at")]
    public string? FinishedAt { get; init; }

    [JsonPropertyName("progress_stage")]
    public string? ProgressStage { get; init; }

    [JsonPropertyName("progress_detail")]
    public string? ProgressDetail { get; init; }

    [JsonPropertyName("progress_percent")]
    public double ProgressPercent { get; init; }

    [JsonPropertyName("elapsed_seconds")]
    public int ElapsedSeconds { get; init; }

    [JsonPropertyName("estimated_remaining_seconds")]
    public int? EstimatedRemainingSeconds { get; init; }

    [JsonPropertyName("model_version")]
    public string? ModelVersion { get; init; }

    [JsonPropertyName("model_name")]
    public string? ModelName { get; init; }

    [JsonPropertyName("model_name_updated_at")]
    public string? ModelNameUpdatedAt { get; init; }

    [JsonPropertyName("is_official_baseline")]
    public bool IsOfficialBaseline { get; init; }

    [JsonPropertyName("dataset_name")]
    public string? DatasetName { get; init; }

    [JsonPropertyName("base_dataset_id")]
    public string? BaseDatasetId { get; init; }

    [JsonPropertyName("is_active_model")]
    public bool IsActiveModel { get; init; }

    [JsonPropertyName("oss_backup_status")]
    public string? OssBackupStatus { get; init; }

    [JsonPropertyName("oss_object_key")]
    public string? OssObjectKey { get; init; }

    [JsonPropertyName("oss_backed_up_at")]
    public string? OssBackedUpAt { get; init; }

    [JsonPropertyName("oss_backup_error")]
    public string? OssBackupError { get; init; }

    [JsonPropertyName("artifacts_cleaned_at")]
    public string? ArtifactsCleanedAt { get; init; }

    [JsonPropertyName("firmware_status")]
    public string FirmwareStatus { get; init; } = "not_requested";

    [JsonPropertyName("firmware_detail")]
    public string? FirmwareDetail { get; init; }

    [JsonPropertyName("firmware_progress_percent")]
    public double FirmwareProgressPercent { get; init; }

    [JsonPropertyName("firmware_built_at")]
    public string? FirmwareBuiltAt { get; init; }

    [JsonPropertyName("firmware_error")]
    public string? FirmwareError { get; init; }

    public string ShortId => Id.Length > 8 ? Id[..8] : Id;
    public string CreatedDisplay => DateTimeOffset.TryParse(CreatedAt, out var value)
        ? value.ToLocalTime().ToString("yyyy-MM-dd HH:mm")
        : "时间未知";
    public string StatusDisplay => Status switch
    {
        "queued" => "排队中",
        "running" => "训练中",
        "passed" => "训练通过",
        "validated" => "校验完成",
        "failed" => "失败",
        _ => Status,
    };
    public string ProgressDisplay => Status == "running"
        ? $"{ProgressPercent:0}% · {ProgressDetail ?? "训练中"}"
        : StatusDisplay;
    public string VersionDisplay => string.IsNullOrWhiteSpace(ModelVersion) ? $"JOB {ShortId}" : ModelVersion;
    public string ModelNameDisplay => !string.IsNullOrWhiteSpace(ModelName)
        ? ModelName
        : IsOfficialBaseline
            ? "MoveToPlay 官方基线模型"
            : $"自训练模型 · {CreatedDisplay}";
    public string ModelKindDisplay => IsOfficialBaseline ? "官方基线" : "玩家模型";
    public string ModelSourceDisplay => IsOfficialBaseline
        ? "数据来源：完整官方原始数据"
        : !string.IsNullOrWhiteSpace(BaseDatasetId)
            ? "数据来源：官方数据 + 玩家所选采集数据"
            : $"数据来源：{DatasetName ?? "独立数据集"}";
    public string TaskTitleDisplay => Mode == "validate"
        ? "数据校验任务"
        : !string.IsNullOrWhiteSpace(ApprovedAt)
            ? ModelNameDisplay
            : "模型训练任务";
    public string DatasetDisplay => $"数据集：{DatasetName ?? DatasetId}";
    public string ActiveDisplay => IsActiveModel ? "当前采用" : "历史版本";
    public string BackupDisplay => OssBackupStatus switch
    {
        "completed" => "OSS 已备份",
        "uploading" or "pending" => "OSS 备份中",
        "failed" => "OSS 备份失败",
        "not_configured" => "OSS 未配置",
        _ => "等待 OSS 备份",
    };
    public string FirmwareDisplay => FirmwareStatus switch
    {
        "queued" => "固件排队中",
        "building" => $"固件编译中 {FirmwareProgressPercent:0}%",
        "ready" => "云端固件可下载",
        "failed" => "固件编译失败",
        _ => "尚未生成固件",
    };
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

public sealed class CloudSystemConfig
{
    [JsonPropertyName("official_dataset_id")]
    public string? OfficialDatasetId { get; init; }

    [JsonPropertyName("oss_backup_configured")]
    public bool OssBackupConfigured { get; init; }

    [JsonPropertyName("cleanup_policy")]
    public Dictionary<string, JsonElement>? CleanupPolicy { get; init; }
}

public sealed record CloudUploadProgress(
    string Stage,
    long CompletedBytes,
    long TotalBytes,
    double Percent);
