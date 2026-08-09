using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed partial class EventCatalogService
{
    private static readonly TrainingEventOption[] Defaults =
    [
        new("attack_event", "hands_shoot", "双手射击"),
        new("attack_event", "kick", "踢腿"),
        new("jump_event", "jump", "跳跃"),
        new("skill_event", "hands_press_down", "双手下压"),
        new("skill_event", "hands_cross_forehead", "双手交叉额前"),
        new("skill_event", "ultraman_beam", "奥特曼光线"),
        new("pause_event", "right_hand_raise", "右手抬起"),
        new("pause_event", "left_hand_raise", "左手抬起"),
        new("turn_event", "turn_left", "向左转"),
        new("turn_event", "turn_right", "向右转"),
    ];

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.SnakeCaseLower,
        WriteIndented = true,
    };

    public string CatalogPath { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments),
        "MoveToPlay",
        "event_catalog.json");

    public IReadOnlyList<TrainingEventOption> Load()
    {
        try
        {
            if (File.Exists(CatalogPath))
            {
                var loaded = JsonSerializer.Deserialize<TrainingEventOption[]>(
                    File.ReadAllText(CatalogPath), JsonOptions) ?? [];
                var valid = loaded.Where(IsValid).DistinctBy(item => item.Type).ToArray();
                if (valid.Length > 0)
                {
                    return valid;
                }
            }
        }
        catch (IOException)
        {
            // Fall back to the built-in catalog; the user can save it again later.
        }
        catch (UnauthorizedAccessException)
        {
            // Fall back when the Documents directory is read-only.
        }
        catch (JsonException)
        {
            // Invalid user JSON must not prevent the training window from opening.
        }

        return Defaults;
    }

    public void Save(IEnumerable<TrainingEventOption> events)
    {
        var normalized = events
            .Select(item => new TrainingEventOption(
                item.Group.Trim().ToLowerInvariant(),
                item.Type.Trim().ToLowerInvariant(),
                item.DisplayName.Trim()))
            .Where(IsValid)
            .DistinctBy(item => item.Type)
            .ToArray();
        if (normalized.Length == 0)
        {
            throw new InvalidOperationException("动作事件库至少要保留一个有效动作。 ");
        }

        var directory = Path.GetDirectoryName(CatalogPath)!;
        Directory.CreateDirectory(directory);
        var temporary = CatalogPath + ".tmp";
        File.WriteAllText(temporary, JsonSerializer.Serialize(normalized, JsonOptions));
        File.Move(temporary, CatalogPath, overwrite: true);
    }

    public IReadOnlyList<TrainingEventOption> RestoreMissingDefaults()
    {
        var restored = Load().ToList();
        foreach (var item in Defaults)
        {
            if (restored.All(existing => !existing.Type.Equals(item.Type, StringComparison.Ordinal)))
            {
                restored.Add(item);
            }
        }
        Save(restored);
        return restored;
    }

    public static bool IsValid(TrainingEventOption item) =>
        !string.IsNullOrWhiteSpace(item.DisplayName) &&
        EventIdRegex().IsMatch(item.Type) &&
        EventGroupRegex().IsMatch(item.Group);

    [GeneratedRegex("^[a-z][a-z0-9_]{0,30}$", RegexOptions.CultureInvariant)]
    private static partial Regex EventIdRegex();

    [GeneratedRegex("^[a-z][a-z0-9_]{0,30}$", RegexOptions.CultureInvariant)]
    private static partial Regex EventGroupRegex();
}
