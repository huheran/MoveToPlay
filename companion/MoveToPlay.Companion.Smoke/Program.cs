using MoveToPlay.Companion.Services;

using var tunnel = new SshTunnelService();
Console.WriteLine("[smoke] 正在建立 SSH 隧道");
var token = await tunnel.ConnectAsync();
using var api = new CloudTrainingApiClient(tunnel.BaseUri!, token);
if (!await api.CheckHealthAsync())
{
    throw new InvalidOperationException("健康检查失败");
}
var jobs = await api.ListJobsAsync();
Console.WriteLine($"[smoke] 健康检查通过，云端任务数={jobs.Length}");
var latest = jobs.FirstOrDefault(job => job.Status == "passed") ?? jobs.FirstOrDefault();
if (latest is not null)
{
    var artifacts = await api.ListArtifactsAsync(latest.Id);
    Console.WriteLine($"[smoke] 最新任务={latest.Id} status={latest.Status} artifacts={artifacts.Length}");
    var manifest = artifacts.FirstOrDefault(artifact => artifact.Path == "run_manifest.json");
    if (manifest is not null)
    {
        var manifestText = await api.DownloadArtifactTextAsync(latest.Id, manifest.Path);
        using var document = System.Text.Json.JsonDocument.Parse(manifestText);
        var modelCount = document.RootElement.GetProperty("models").GetArrayLength();
        Console.WriteLine($"[smoke] run_manifest 可解析，models={modelCount}");

        var history = new TrainingHistoryService();
        history.SaveLastJob(latest.Id, latest.DatasetId, latest.Status);
        var cacheDirectory = history.CacheDirectory(latest.Id);
        Directory.CreateDirectory(cacheDirectory);
        await File.WriteAllTextAsync(history.ManifestPath(latest.Id), manifestText);
        foreach (var artifact in artifacts.Where(artifact =>
                     (artifact.Path.StartsWith("generated/", StringComparison.Ordinal) ||
                      artifact.Path.Contains("/generated/", StringComparison.Ordinal)) &&
                     (artifact.Path.EndsWith(".c", StringComparison.OrdinalIgnoreCase) ||
                      artifact.Path.EndsWith(".h", StringComparison.OrdinalIgnoreCase) ||
                      artifact.Path.EndsWith(".json", StringComparison.OrdinalIgnoreCase))))
        {
            var destination = Path.Combine(cacheDirectory, artifact.Path.Replace('/', Path.DirectorySeparatorChar));
            await api.DownloadArtifactAsync(latest.Id, artifact.Path, destination, artifact.Sha256);
        }
        Console.WriteLine($"[smoke] 离线缓存={cacheDirectory}");
    }
}
if (args.Length == 2)
{
    var samplesPath = Path.GetFullPath(args[0]);
    var eventsPath = Path.GetFullPath(args[1]);
    Console.WriteLine("[smoke] 创建可续传上传数据集");
    var dataset = await api.CreateDatasetAsync($"CSharp-smoke-{DateTime.UtcNow:yyyyMMdd-HHmmss}", samplesPath, eventsPath);
    dataset = await api.UploadDatasetAsync(dataset, samplesPath, eventsPath,
        new Progress<MoveToPlay.Companion.Models.CloudUploadProgress>(value =>
            Console.WriteLine($"[smoke] {value.Stage} {value.Percent:F1}%")));
    Console.WriteLine($"[smoke] 数据集={dataset.Id} status={dataset.Status}");
    var validationJob = await api.CreateJobAsync(dataset.Id, "validate");
    validationJob = await api.WaitForJobAsync(validationJob.Id,
        job => Console.WriteLine($"[smoke] 校验任务={job.Id} status={job.Status}"));
    if (validationJob.Status != "validated")
    {
        throw new InvalidOperationException($"云端校验失败：{validationJob.Error}");
    }
}
Console.WriteLine("[smoke] PASS");
