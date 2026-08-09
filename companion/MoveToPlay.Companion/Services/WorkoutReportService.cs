using System.IO;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class WorkoutReportService
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    public string Save(WorkoutReport report)
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay",
            "reports");
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, $"workout-{report.EndedAt:yyyyMMdd-HHmmss}.json");
        File.WriteAllText(path, JsonSerializer.Serialize(report, JsonOptions));
        return path;
    }
}
