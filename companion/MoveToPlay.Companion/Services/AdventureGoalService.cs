using System.IO;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class AdventureGoalService
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private readonly string _settingsPath;

    public AdventureGoalService()
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay");
        _settingsPath = Path.Combine(directory, "adventure-goal.json");
    }

    public AdventureGoalSettings Load()
    {
        try
        {
            if (File.Exists(_settingsPath))
            {
                var loaded = JsonSerializer.Deserialize<AdventureGoalSettings>(
                    File.ReadAllText(_settingsPath),
                    JsonOptions);
                if (loaded is not null)
                {
                    return Sanitize(loaded);
                }
            }
        }
        catch
        {
            // Invalid settings fall back to a safe demo-friendly default.
        }
        return new AdventureGoalSettings(AdventureGoalType.Calories, 100);
    }

    public void Save(AdventureGoalSettings settings)
    {
        try
        {
            var directory = Path.GetDirectoryName(_settingsPath);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
            }
            var temporaryPath = _settingsPath + ".tmp";
            File.WriteAllText(temporaryPath, JsonSerializer.Serialize(Sanitize(settings), JsonOptions));
            File.Move(temporaryPath, _settingsPath, true);
        }
        catch
        {
            // The selected goal still applies to the current session.
        }
    }

    private static AdventureGoalSettings Sanitize(AdventureGoalSettings settings)
    {
        var target = settings.Type switch
        {
            AdventureGoalType.ActiveMinutes => Math.Clamp(settings.Target, 1, 300),
            AdventureGoalType.Actions => Math.Clamp(settings.Target, 1, 10000),
            _ => Math.Clamp(settings.Target, 1, 3000),
        };
        return new AdventureGoalSettings(settings.Type, Math.Round(target, 1));
    }
}
