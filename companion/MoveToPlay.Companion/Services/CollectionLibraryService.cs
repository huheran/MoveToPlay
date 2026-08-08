using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed class CollectionLibraryService
{
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web)
    {
        WriteIndented = true,
    };

    private readonly EventCatalogService _eventCatalog;

    public CollectionLibraryService(EventCatalogService eventCatalog)
    {
        _eventCatalog = eventCatalog;
    }

    public IReadOnlyList<LocalCollectionSession> Load(string collectionRoot)
    {
        if (!Directory.Exists(collectionRoot))
        {
            return [];
        }

        var displayNames = _eventCatalog.Load().ToDictionary(
            item => item.Type,
            item => item.DisplayName,
            StringComparer.Ordinal);
        var sessions = new List<LocalCollectionSession>();
        foreach (var directory in Directory.EnumerateDirectories(collectionRoot))
        {
            var samplesPath = Path.Combine(directory, "samples.csv");
            var eventsPath = Path.Combine(directory, "events.csv");
            if (!File.Exists(samplesPath) || !File.Exists(eventsPath))
            {
                continue;
            }

            try
            {
                var eventSummary = ReadEventSummary(eventsPath);
                var sampleCount = CountDataRows(samplesPath);
                var actions = eventSummary.EventTypes
                    .Select(type => displayNames.TryGetValue(type, out var displayName) ? displayName : type)
                    .Order(StringComparer.CurrentCulture)
                    .ToArray();
                var bytes = new FileInfo(samplesPath).Length + new FileInfo(eventsPath).Length;
                sessions.Add(new LocalCollectionSession
                {
                    SessionId = Path.GetFileName(directory),
                    DirectoryPath = Path.GetFullPath(directory),
                    SamplesPath = Path.GetFullPath(samplesPath),
                    EventsPath = Path.GetFullPath(eventsPath),
                    CreatedAt = SessionCreatedAt(directory),
                    SampleCount = sampleCount,
                    EventCount = eventSummary.EventCount,
                    ActionsDisplay = actions.Length == 0 ? "尚无动作标记" : string.Join("、", actions),
                    SizeDisplay = FormatBytes(bytes),
                });
            }
            catch (Exception exception) when (exception is IOException or UnauthorizedAccessException or InvalidDataException)
            {
                // A partial/corrupt directory is ignored instead of blocking every healthy session.
            }
        }
        return sessions.OrderByDescending(item => item.CreatedAt).ToArray();
    }

    public PreparedCollectionDataset Prepare(
        string collectionRoot,
        IEnumerable<LocalCollectionSession> selectedSessions)
    {
        var sessions = selectedSessions.ToArray();
        if (sessions.Length == 0)
        {
            throw new InvalidOperationException("请至少选择一次采集数据。 ");
        }
        foreach (var session in sessions)
        {
            ValidateSessionPath(collectionRoot, session.DirectoryPath);
            if (!File.Exists(session.SamplesPath) || !File.Exists(session.EventsPath))
            {
                throw new FileNotFoundException($"采集数据已不存在：{session.SessionId}");
            }
        }

        var selectionId = $"selection-{DateTime.Now:yyyyMMdd-HHmmss}-{Guid.NewGuid():N}";
        var preparedRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay",
            "training",
            "prepared",
            selectionId[..43]);
        Directory.CreateDirectory(preparedRoot);
        var samplesPath = Path.Combine(preparedRoot, "samples.csv");
        var eventsPath = Path.Combine(preparedRoot, "events.csv");
        try
        {
            MergeCsv(sessions.Select(item => item.SamplesPath), samplesPath);
            MergeCsv(sessions.Select(item => item.EventsPath), eventsPath);
            var prepared = new PreparedCollectionDataset(
                preparedRoot,
                samplesPath,
                eventsPath,
                sessions.Select(item => item.SessionId).ToArray(),
                sessions.Sum(item => item.SampleCount),
                sessions.Sum(item => item.EventCount));
            File.WriteAllText(
                Path.Combine(preparedRoot, "selection.json"),
                JsonSerializer.Serialize(new
                {
                    schema_version = 1,
                    created_at = DateTimeOffset.UtcNow,
                    sessions = prepared.SessionIds,
                    samples = prepared.SampleCount,
                    events = prepared.EventCount,
                }, JsonOptions));
            return prepared;
        }
        catch
        {
            Directory.Delete(preparedRoot, recursive: true);
            throw;
        }
    }

    public void Delete(string collectionRoot, LocalCollectionSession session)
    {
        var path = ValidateSessionPath(collectionRoot, session.DirectoryPath);
        var attributes = File.GetAttributes(path);
        if ((attributes & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidOperationException("出于安全原因，不能删除链接形式的采集目录。 ");
        }
        Directory.Delete(path, recursive: true);
    }

    private static string ValidateSessionPath(string collectionRoot, string sessionPath)
    {
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(collectionRoot));
        var path = Path.TrimEndingDirectorySeparator(Path.GetFullPath(sessionPath));
        if (path.Equals(root, StringComparison.OrdinalIgnoreCase) ||
            !path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ||
            !Directory.Exists(path))
        {
            throw new InvalidOperationException("采集目录不在当前“我的采集数据”范围内。 ");
        }
        return path;
    }

    private static (int EventCount, HashSet<string> EventTypes) ReadEventSummary(string path)
    {
        using var reader = new StreamReader(path, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        var header = reader.ReadLine() ?? throw new InvalidDataException($"CSV 没有表头：{path}");
        var columns = ParseCsvLine(header);
        var eventTypeIndex = columns.FindIndex(value => value.Equals("event_type", StringComparison.Ordinal));
        if (eventTypeIndex < 0)
        {
            throw new InvalidDataException($"events.csv 缺少 event_type：{path}");
        }
        var count = 0;
        var types = new HashSet<string>(StringComparer.Ordinal);
        while (reader.ReadLine() is { } line)
        {
            if (string.IsNullOrWhiteSpace(line))
            {
                continue;
            }
            count++;
            var values = ParseCsvLine(line);
            if (eventTypeIndex < values.Count && !string.IsNullOrWhiteSpace(values[eventTypeIndex]))
            {
                types.Add(values[eventTypeIndex]);
            }
        }
        return (count, types);
    }

    private static long CountDataRows(string path)
    {
        using var reader = new StreamReader(path, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        _ = reader.ReadLine() ?? throw new InvalidDataException($"CSV 没有表头：{path}");
        long count = 0;
        while (reader.ReadLine() is { } line)
        {
            if (!string.IsNullOrWhiteSpace(line))
            {
                count++;
            }
        }
        return count;
    }

    private static void MergeCsv(IEnumerable<string> sources, string destination)
    {
        var sourcePaths = sources.ToArray();
        var headers = new List<string>();
        foreach (var source in sourcePaths)
        {
            using var reader = new StreamReader(source, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
            var line = reader.ReadLine() ?? throw new InvalidDataException($"CSV 没有表头：{source}");
            foreach (var column in ParseCsvLine(line))
            {
                if (!headers.Contains(column, StringComparer.Ordinal))
                {
                    headers.Add(column);
                }
            }
        }

        using var writer = new StreamWriter(destination, append: false, new UTF8Encoding(encoderShouldEmitUTF8Identifier: false));
        writer.WriteLine(string.Join(',', headers.Select(Csv)));
        foreach (var source in sourcePaths)
        {
            using var reader = new StreamReader(source, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
            var sourceHeaders = ParseCsvLine(reader.ReadLine()!);
            var indices = headers.Select(header => sourceHeaders.FindIndex(value => value.Equals(header, StringComparison.Ordinal))).ToArray();
            while (reader.ReadLine() is { } line)
            {
                if (string.IsNullOrWhiteSpace(line))
                {
                    continue;
                }
                var values = ParseCsvLine(line);
                writer.WriteLine(string.Join(',', indices.Select(index => Csv(index >= 0 && index < values.Count ? values[index] : ""))));
            }
        }
    }

    private static List<string> ParseCsvLine(string line)
    {
        var values = new List<string>();
        var current = new StringBuilder();
        var quoted = false;
        for (var index = 0; index < line.Length; index++)
        {
            var character = line[index];
            if (character == '"')
            {
                if (quoted && index + 1 < line.Length && line[index + 1] == '"')
                {
                    current.Append('"');
                    index++;
                }
                else
                {
                    quoted = !quoted;
                }
            }
            else if (character == ',' && !quoted)
            {
                values.Add(current.ToString());
                current.Clear();
            }
            else
            {
                current.Append(character);
            }
        }
        if (quoted)
        {
            throw new InvalidDataException("CSV 存在未闭合的引号。 ");
        }
        values.Add(current.ToString());
        return values;
    }

    private static string Csv(string value) =>
        value.IndexOfAny([',', '"', '\r', '\n']) >= 0 ? $"\"{value.Replace("\"", "\"\"")}\"" : value;

    private static DateTime SessionCreatedAt(string directory)
    {
        var name = Path.GetFileName(directory);
        const string prefix = "session-";
        if (name.StartsWith(prefix, StringComparison.Ordinal) &&
            DateTime.TryParseExact(name[prefix.Length..], "yyyyMMdd-HHmmss", CultureInfo.InvariantCulture, DateTimeStyles.None, out var parsed))
        {
            return parsed;
        }
        return Directory.GetCreationTime(directory);
    }

    private static string FormatBytes(long bytes) => bytes switch
    {
        >= 1024L * 1024L => $"{bytes / 1024d / 1024d:0.0} MB",
        >= 1024L => $"{bytes / 1024d:0.0} KB",
        _ => $"{bytes} B",
    };
}
