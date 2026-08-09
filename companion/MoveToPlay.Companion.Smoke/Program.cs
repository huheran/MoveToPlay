using MoveToPlay.Companion.Services;

if (args.SequenceEqual(["--collection-library"]))
{
    var root = Path.Combine(Path.GetTempPath(), $"movetoplay-collection-smoke-{Guid.NewGuid():N}");
    string? preparedRoot = null;
    try
    {
        Directory.CreateDirectory(root);
        for (var index = 1; index <= 2; index++)
        {
            var session = Path.Combine(root, $"session-20260809-00000{index}");
            Directory.CreateDirectory(session);
            await File.WriteAllTextAsync(
                Path.Combine(session, "samples.csv"),
                "pc_timestamp_ms,board_timestamp_ms,node_id,ax,ay,az,gx,gy,gz,state_label,session_id\n" +
                $"100{index},10,1,0.1,0.2,0.3,1,2,3,idle,session-{index}\n");
            await File.WriteAllTextAsync(
                Path.Combine(session, "events.csv"),
                "event_id,event_group,event_type,pc_timestamp_ms,state_label,session_id\n" +
                $"event-{index},jump_event,jump,100{index},idle,session-{index}\n");
        }
        var library = new CollectionLibraryService(new EventCatalogService());
        var sessions = library.Load(root);
        if (sessions.Count != 2 || sessions.Any(item => item.SampleCount != 1 || item.EventCount != 1))
        {
            throw new InvalidOperationException("采集会话扫描计数不正确");
        }
        var prepared = library.Prepare(root, sessions);
        preparedRoot = prepared.DirectoryPath;
        if (File.ReadLines(prepared.SamplesPath).Count() != 3 ||
            File.ReadLines(prepared.EventsPath).Count() != 3)
        {
            throw new InvalidOperationException("所选会话合并结果不正确");
        }
        library.Delete(root, sessions[0]);
        if (library.Load(root).Count != 1)
        {
            throw new InvalidOperationException("定向删除会话失败");
        }
        Console.WriteLine("[smoke] 采集数据扫描、选择合并、定向删除 PASS");
        return;
    }
    finally
    {
        if (Directory.Exists(root))
        {
            Directory.Delete(root, recursive: true);
        }
        if (!string.IsNullOrWhiteSpace(preparedRoot) && Directory.Exists(preparedRoot))
        {
            Directory.Delete(preparedRoot, recursive: true);
        }
    }
}

if (args.Length == 3 && args[0].Equals("--firmware-package", StringComparison.OrdinalIgnoreCase))
{
    var jobId = args[1];
    var bundlePath = Path.GetFullPath(args[2]);
    var deployment = new FirmwareDeploymentService();
    var package = await deployment.PrepareCloudFirmwareAsync(
        jobId,
        bundlePath,
        new Progress<MoveToPlay.Companion.Models.FirmwareDeploymentProgress>(value =>
            Console.WriteLine($"[firmware] {value.Stage}: {value.Detail}")));
    if (package.Files.Count < 3 || package.Files.Any(file => !File.Exists(file.Path)) || !File.Exists(package.ManifestPath))
    {
        throw new InvalidOperationException("固件产物或清单不存在");
    }
    Console.WriteLine($"[smoke] 云端 Dongle 固件包校验 PASS：{package.PackageDirectory}");
    return;
}

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
