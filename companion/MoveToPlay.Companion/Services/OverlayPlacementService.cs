using System.IO;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class OverlayPlacementService
{
    private sealed class PlacementSettingsFile
    {
        public Dictionary<string, string> SelectedResolutions { get; set; } =
            new(StringComparer.OrdinalIgnoreCase);
        public Dictionary<string, OverlayPlacement> Placements { get; set; } =
            new(StringComparer.OrdinalIgnoreCase);
    }

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
    };

    private readonly string _settingsPath;
    private readonly Dictionary<string, OverlayPlacement> _placements;
    private readonly Dictionary<string, string> _selectedResolutions;

    public OverlayPlacementService()
    {
        var settingsDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay");
        _settingsPath = Path.Combine(settingsDirectory, "overlay-placements.json");
        var settings = LoadSettings();
        _placements = new Dictionary<string, OverlayPlacement>(settings.Placements,
                                                               StringComparer.OrdinalIgnoreCase);
        _selectedResolutions = new Dictionary<string, string>(settings.SelectedResolutions,
                                                              StringComparer.OrdinalIgnoreCase);
    }

    public string GetSelectedResolution(string profileId)
    {
        return _selectedResolutions.TryGetValue(profileId, out var saved)
            ? NormalizeResolutionKey(saved)
            : "auto";
    }

    public void SaveSelectedResolution(string profileId, string resolutionKey)
    {
        if (string.IsNullOrWhiteSpace(profileId))
        {
            return;
        }

        _selectedResolutions[profileId] = NormalizeResolutionKey(resolutionKey);
        WriteSettings();
    }

    public OverlayPlacement GetForProfile(GameProfile profile, string resolutionKey)
    {
        var scopedKey = PlacementKey(profile.Id, resolutionKey);
        if (_placements.TryGetValue(scopedKey, out var saved))
        {
            return Sanitize(saved);
        }

        // Migrate settings written before resolution-specific positions existed.
        if (_placements.TryGetValue(profile.Id, out saved))
        {
            return Sanitize(saved);
        }

        return OverlayPlacement.FromProfile(profile);
    }

    public void Save(string profileId, string resolutionKey, OverlayPlacement placement)
    {
        if (string.IsNullOrWhiteSpace(profileId))
        {
            return;
        }

        _placements[PlacementKey(profileId, resolutionKey)] = Sanitize(placement);
        WriteSettings();
    }

    private void WriteSettings()
    {
        try
        {
            var directory = Path.GetDirectoryName(_settingsPath);
            if (!string.IsNullOrWhiteSpace(directory))
            {
                Directory.CreateDirectory(directory);
            }

            var temporaryPath = _settingsPath + ".tmp";
            var settings = new PlacementSettingsFile
            {
                SelectedResolutions = _selectedResolutions,
                Placements = _placements,
            };
            File.WriteAllText(temporaryPath, JsonSerializer.Serialize(settings, JsonOptions));
            File.Move(temporaryPath, _settingsPath, true);
        }
        catch
        {
            // Position changes still apply for this session if persistence fails.
        }
    }

    private PlacementSettingsFile LoadSettings()
    {
        try
        {
            if (!File.Exists(_settingsPath))
            {
                return new PlacementSettingsFile();
            }

            var json = File.ReadAllText(_settingsPath);
            using var document = JsonDocument.Parse(json);
            if (document.RootElement.TryGetProperty(nameof(PlacementSettingsFile.Placements), out _))
            {
                var settings = JsonSerializer.Deserialize<PlacementSettingsFile>(json, JsonOptions)
                               ?? new PlacementSettingsFile();
                settings.SelectedResolutions ??= new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
                settings.Placements ??= new Dictionary<string, OverlayPlacement>(StringComparer.OrdinalIgnoreCase);
                return settings;
            }

            var legacyPlacements = JsonSerializer.Deserialize<Dictionary<string, OverlayPlacement>>(json, JsonOptions);
            return new PlacementSettingsFile
            {
                Placements = legacyPlacements ?? new Dictionary<string, OverlayPlacement>(StringComparer.OrdinalIgnoreCase),
            };
        }
        catch
        {
            return new PlacementSettingsFile();
        }
    }

    private static string PlacementKey(string profileId, string resolutionKey) =>
        $"{profileId}::{NormalizeResolutionKey(resolutionKey)}";

    private static string NormalizeResolutionKey(string? resolutionKey)
    {
        if (string.IsNullOrWhiteSpace(resolutionKey) ||
            resolutionKey.Equals("auto", StringComparison.OrdinalIgnoreCase))
        {
            return "auto";
        }

        var parts = resolutionKey.Split('x', StringSplitOptions.TrimEntries);
        return parts.Length == 2 &&
               int.TryParse(parts[0], out var width) &&
               int.TryParse(parts[1], out var height) &&
               width is >= 800 and <= 7680 &&
               height is >= 600 and <= 4320
            ? $"{width}x{height}"
            : "auto";
    }

    private static OverlayPlacement Sanitize(OverlayPlacement placement) => new()
    {
        Anchor = OverlayPlacement.NormalizeAnchor(placement.Anchor),
        OffsetX = Math.Clamp(placement.OffsetX, -800.0, 800.0),
        OffsetY = Math.Clamp(placement.OffsetY, -500.0, 500.0),
        HeaderScale = Math.Clamp(placement.HeaderScale, 0.6, 1.8),
        ActionScale = Math.Clamp(placement.ActionScale, 0.6, 1.8),
        MetricsScale = Math.Clamp(placement.MetricsScale, 0.6, 1.8),
        GoalScale = Math.Clamp(placement.GoalScale, 0.6, 1.8),
        ToastScale = Math.Clamp(placement.ToastScale, 0.6, 1.8),
    };
}
