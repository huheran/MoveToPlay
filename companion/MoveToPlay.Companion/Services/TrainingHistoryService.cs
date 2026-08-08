using System.IO;
using System.Text.Json;

namespace MoveToPlay.Companion.Services;

public sealed class TrainingHistoryService
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web) { WriteIndented = true };
    private readonly string _root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MoveToPlay", "training");

    public string? LoadLastJobId()
    {
        try
        {
            var path = Path.Combine(_root, "last-job.json");
            if (!File.Exists(path))
            {
                return null;
            }
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            return document.RootElement.GetProperty("job_id").GetString();
        }
        catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
    }

    public void SaveLastJob(string jobId, string datasetId, string status)
    {
        Directory.CreateDirectory(_root);
        var path = Path.Combine(_root, "last-job.json");
        var temporary = path + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(new
        {
            job_id = jobId,
            dataset_id = datasetId,
            status,
            saved_at = DateTimeOffset.UtcNow,
        }, JsonOptions));
        File.Move(temporary, path, overwrite: true);
    }

    public string CacheDirectory(string jobId) => Path.Combine(_root, "cache", jobId);

    public string ManifestPath(string jobId) => Path.Combine(CacheDirectory(jobId), "run_manifest.json");
}
