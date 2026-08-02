using System.IO;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class ProfileService
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
    };

    public IReadOnlyList<GameProfile> LoadProfiles()
    {
        var profileDirectory = Path.Combine(AppContext.BaseDirectory, "Profiles");
        if (Directory.Exists(profileDirectory))
        {
            var profiles = Directory.EnumerateFiles(profileDirectory, "*.json")
                .Select(LoadProfile)
                .Where(profile => profile is not null)
                .Cast<GameProfile>()
                .GroupBy(profile => profile.Id, StringComparer.OrdinalIgnoreCase)
                .Select(group => group.First())
                .OrderBy(profile => profile.IsDefault ? 0 : 1)
                .ThenBy(profile => profile.SortOrder)
                .ThenBy(profile => profile.DisplayName)
                .ToList();

            if (profiles.Count > 0)
            {
                return profiles;
            }
        }

        return
        [
            new GameProfile
            {
                Id = "generic",
                DisplayName = "通用运动界面",
                ProcessNames = [],
                Encouragements = ["保持节奏，继续前进！"],
            },
        ];
    }

    private static GameProfile? LoadProfile(string path)
    {
        try
        {
            var profile = JsonSerializer.Deserialize<GameProfile>(File.ReadAllText(path), JsonOptions);
            if (profile is null || string.IsNullOrWhiteSpace(profile.Id) || string.IsNullOrWhiteSpace(profile.DisplayName))
            {
                return null;
            }

            profile.ProcessNames ??= [];
            profile.WindowMatchRules ??= [];
            foreach (var rule in profile.WindowMatchRules)
            {
                rule.ProcessNames ??= [];
                rule.TitleKeywords ??= [];
            }

            profile.Encouragements ??= [];
            return profile;
        }
        catch
        {
            return null;
        }
    }
}
