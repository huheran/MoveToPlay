using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class CloudTrainingApiClient : IDisposable
{
    private const int ChunkBytes = 4 * 1024 * 1024;
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);
    private readonly HttpClient _httpClient;

    public CloudTrainingApiClient(Uri baseUri, string token)
    {
        _httpClient = new HttpClient
        {
            BaseAddress = baseUri,
            Timeout = TimeSpan.FromMinutes(3),
        };
        _httpClient.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", token);
    }

    public void Dispose() => _httpClient.Dispose();

    public async Task<bool> CheckHealthAsync(CancellationToken cancellationToken = default)
    {
        using var response = await _httpClient.GetAsync("/health", cancellationToken);
        return response.IsSuccessStatusCode;
    }

    public async Task<CloudDataset> CreateDatasetAsync(
        string name,
        string samplesPath,
        string eventsPath,
        CancellationToken cancellationToken = default)
    {
        var samples = new FileInfo(samplesPath);
        var events = new FileInfo(eventsPath);
        var payload = new
        {
            name,
            samples = new { filename = samples.Name, bytes = samples.Length, sha256 = await Sha256Async(samplesPath, cancellationToken) },
            events = new { filename = events.Name, bytes = events.Length, sha256 = await Sha256Async(eventsPath, cancellationToken) },
            event_id_scope = "global",
        };
        return await SendJsonAsync<CloudDataset>(HttpMethod.Post, "/api/v1/datasets", payload, cancellationToken);
    }

    public Task<CloudDataset> GetDatasetAsync(string datasetId, CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudDataset>(HttpMethod.Get, $"/api/v1/datasets/{datasetId}", null, cancellationToken);

    public async Task<CloudDataset> UploadDatasetAsync(
        CloudDataset dataset,
        string samplesPath,
        string eventsPath,
        IProgress<CloudUploadProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        var paths = new Dictionary<string, string>
        {
            ["samples"] = samplesPath,
            ["events"] = eventsPath,
        };
        var totalBytes = paths.Values.Sum(path => new FileInfo(path).Length);
        foreach (var kind in new[] { "samples", "events" })
        {
            dataset = await GetDatasetAsync(dataset.Id, cancellationToken);
            await VerifyResumeFileAsync(dataset, kind, paths[kind], cancellationToken);
            var offset = dataset.Files[kind].ReceivedBytes;
            await using var stream = new FileStream(
                paths[kind], FileMode.Open, FileAccess.Read, FileShare.Read, ChunkBytes, useAsync: true);
            stream.Position = offset;
            var buffer = new byte[ChunkBytes];
            while (offset < stream.Length)
            {
                var read = await stream.ReadAsync(buffer.AsMemory(0, (int)Math.Min(buffer.Length, stream.Length - offset)), cancellationToken);
                if (read <= 0)
                {
                    throw new EndOfStreamException($"读取 {kind} 时意外到达文件末尾。 ");
                }
                using var request = new HttpRequestMessage(HttpMethod.Put, $"/api/v1/datasets/{dataset.Id}/files/{kind}");
                request.Headers.Add("X-Upload-Offset", offset.ToString(System.Globalization.CultureInfo.InvariantCulture));
                request.Content = new ByteArrayContent(buffer, 0, read);
                request.Content.Headers.ContentType = new MediaTypeHeaderValue("application/octet-stream");
                using var response = await _httpClient.SendAsync(request, cancellationToken);
                if (response.StatusCode == HttpStatusCode.Conflict)
                {
                    dataset = await GetDatasetAsync(dataset.Id, cancellationToken);
                    offset = dataset.Files[kind].ReceivedBytes;
                    stream.Position = offset;
                    continue;
                }
                dataset = await ReadResponseAsync<CloudDataset>(response, cancellationToken);
                offset = dataset.Files[kind].ReceivedBytes;
                var completed = paths.Where(pair => Array.IndexOf(new[] { "samples", "events" }, pair.Key) < Array.IndexOf(new[] { "samples", "events" }, kind))
                    .Sum(pair => new FileInfo(pair.Value).Length) + offset;
                progress?.Report(new CloudUploadProgress(
                    kind == "samples" ? "上传样本" : "上传事件",
                    completed,
                    totalBytes,
                    totalBytes == 0 ? 0 : completed * 100.0 / totalBytes));
            }
        }
        return await SendJsonAsync<CloudDataset>(HttpMethod.Post, $"/api/v1/datasets/{dataset.Id}/complete", null, cancellationToken);
    }

    public Task<CloudJob> CreateJobAsync(string datasetId, string mode = "train", CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudJob>(HttpMethod.Post, "/api/v1/jobs", new { dataset_id = datasetId, mode }, cancellationToken);

    public Task<CloudJob> GetJobAsync(string jobId, CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudJob>(HttpMethod.Get, $"/api/v1/jobs/{jobId}", null, cancellationToken);

    public Task<CloudJob[]> ListJobsAsync(CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudJob[]>(HttpMethod.Get, "/api/v1/jobs", null, cancellationToken);

    public async Task<CloudJob> WaitForJobAsync(
        string jobId,
        Action<CloudJob>? changed = null,
        CancellationToken cancellationToken = default)
    {
        string? previous = null;
        while (true)
        {
            var job = await GetJobAsync(jobId, cancellationToken);
            if (!string.Equals(job.Status, previous, StringComparison.Ordinal))
            {
                previous = job.Status;
                changed?.Invoke(job);
            }
            if (job.Status is not ("queued" or "running"))
            {
                return job;
            }
            await Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
        }
    }

    public Task<CloudArtifact[]> ListArtifactsAsync(string jobId, CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudArtifact[]>(HttpMethod.Get, $"/api/v1/jobs/{jobId}/artifacts", null, cancellationToken);

    public Task<CloudJob> ApproveAsync(string jobId, string approvedBy, CancellationToken cancellationToken = default) =>
        SendJsonAsync<CloudJob>(HttpMethod.Post, $"/api/v1/jobs/{jobId}/approve", new { approved_by = approvedBy }, cancellationToken);

    public async Task DownloadArtifactAsync(
        string jobId,
        string artifactPath,
        string destination,
        string? expectedSha256 = null,
        CancellationToken cancellationToken = default)
    {
        var escapedPath = string.Join('/', artifactPath.Split('/').Select(Uri.EscapeDataString));
        using var response = await _httpClient.GetAsync($"/api/v1/jobs/{jobId}/artifacts/{escapedPath}", HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        await EnsureSuccessAsync(response, cancellationToken);
        Directory.CreateDirectory(Path.GetDirectoryName(destination) ?? ".");
        var temporary = destination + ".part";
        await using (var input = await response.Content.ReadAsStreamAsync(cancellationToken))
        await using (var output = new FileStream(temporary, FileMode.Create, FileAccess.Write, FileShare.None, 81920, useAsync: true))
        {
            await input.CopyToAsync(output, cancellationToken);
        }
        if (!string.IsNullOrWhiteSpace(expectedSha256))
        {
            var actualSha256 = await Sha256Async(temporary, cancellationToken);
            if (!actualSha256.Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(temporary);
                throw new InvalidDataException($"下载产物 {artifactPath} 的 SHA-256 与清单不一致。 ");
            }
        }
        File.Move(temporary, destination, overwrite: true);
    }

    public async Task<string> DownloadArtifactTextAsync(string jobId, string artifactPath, CancellationToken cancellationToken = default)
    {
        var escapedPath = string.Join('/', artifactPath.Split('/').Select(Uri.EscapeDataString));
        using var response = await _httpClient.GetAsync($"/api/v1/jobs/{jobId}/artifacts/{escapedPath}", cancellationToken);
        await EnsureSuccessAsync(response, cancellationToken);
        return await response.Content.ReadAsStringAsync(cancellationToken);
    }

    private async Task<T> SendJsonAsync<T>(HttpMethod method, string path, object? payload, CancellationToken cancellationToken)
    {
        using var request = new HttpRequestMessage(method, path);
        if (payload is not null)
        {
            request.Content = new StringContent(JsonSerializer.Serialize(payload, JsonOptions), Encoding.UTF8, "application/json");
        }
        using var response = await _httpClient.SendAsync(request, cancellationToken);
        return await ReadResponseAsync<T>(response, cancellationToken);
    }

    private static async Task<T> ReadResponseAsync<T>(HttpResponseMessage response, CancellationToken cancellationToken)
    {
        await EnsureSuccessAsync(response, cancellationToken);
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        return await JsonSerializer.DeserializeAsync<T>(stream, JsonOptions, cancellationToken)
            ?? throw new InvalidDataException("服务器返回了空 JSON。 ");
    }

    private static async Task EnsureSuccessAsync(HttpResponseMessage response, CancellationToken cancellationToken)
    {
        if (response.IsSuccessStatusCode)
        {
            return;
        }
        var body = await response.Content.ReadAsStringAsync(cancellationToken);
        if (body.Length > 1000)
        {
            body = body[..1000];
        }
        throw new HttpRequestException($"云端 API {(int)response.StatusCode}：{body}", null, response.StatusCode);
    }

    private static async Task VerifyResumeFileAsync(CloudDataset dataset, string kind, string path, CancellationToken cancellationToken)
    {
        var remote = dataset.Files[kind];
        var local = new FileInfo(path);
        if (local.Length != remote.ExpectedBytes)
        {
            throw new InvalidOperationException($"{kind} 本地文件大小已变化，不能继续原上传。 ");
        }
        var hash = await Sha256Async(path, cancellationToken);
        if (!hash.Equals(remote.Sha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException($"{kind} 本地文件哈希已变化，不能继续原上传。 ");
        }
    }

    private static async Task<string> Sha256Async(string path, CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, useAsync: true);
        var hash = await SHA256.HashDataAsync(stream, cancellationToken);
        return Convert.ToHexString(hash);
    }
}
