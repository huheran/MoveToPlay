using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed partial class FirmwareDeploymentService
{
    private const string EsptoolSha256 = "C674F46A5D7DC2AD70A0D306A7A4B6B5F8B014B4089C6B46D33F75DEF48AE514";
    private static readonly JsonSerializerOptions JsonOptions = new(JsonSerializerDefaults.Web);

    public async Task<FirmwareBuildPackage> PrepareCloudFirmwareAsync(
        string jobId,
        string bundlePath,
        IProgress<FirmwareDeploymentProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (!File.Exists(bundlePath))
        {
            throw new FileNotFoundException("云端固件包尚未下载。", bundlePath);
        }
        progress?.Report(new FirmwareDeploymentProgress("校验固件", "正在解压云端固件包", 82));
        var packageRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay", "firmware", jobId, "package");
        ValidateManagedPath(packageRoot);
        if (Directory.Exists(packageRoot))
        {
            Directory.Delete(packageRoot, recursive: true);
        }
        Directory.CreateDirectory(packageRoot);
        var packageRootWithSeparator = Path.GetFullPath(packageRoot) + Path.DirectorySeparatorChar;
        using (var archive = ZipFile.OpenRead(bundlePath))
        {
            foreach (var entry in archive.Entries)
            {
                cancellationToken.ThrowIfCancellationRequested();
                if (string.IsNullOrEmpty(entry.Name))
                {
                    continue;
                }
                var destination = Path.GetFullPath(Path.Combine(packageRoot, entry.FullName));
                if (!destination.StartsWith(packageRootWithSeparator, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("固件压缩包包含不安全的文件路径。 ");
                }
                Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
                entry.ExtractToFile(destination, overwrite: true);
            }
        }

        var manifestPath = Path.Combine(packageRoot, "firmware-manifest.json");
        if (!File.Exists(manifestPath))
        {
            throw new InvalidDataException("云端固件包缺少 firmware-manifest.json。 ");
        }
        await using var manifestStream = File.OpenRead(manifestPath);
        var manifest = await JsonSerializer.DeserializeAsync<CloudFirmwareManifest>(
            manifestStream, JsonOptions, cancellationToken)
            ?? throw new InvalidDataException("固件清单为空。 ");
        if (manifest.SchemaVersion != 2 || manifest.JobId != jobId || manifest.BoardProfile != 1 || manifest.Chip != "esp32s3")
        {
            throw new InvalidDataException("固件清单与当前 Dongle 任务不匹配。 ");
        }
        if (manifest.Files.Count == 0 || manifest.Files.Any(file => !OffsetRegex().IsMatch(file.Offset)))
        {
            throw new InvalidDataException("固件清单没有有效的烧录文件或地址。 ");
        }

        var flashFiles = new List<FirmwareFlashFile>();
        foreach (var file in manifest.Files)
        {
            if (Path.GetFileName(file.Name) != file.Name)
            {
                throw new InvalidDataException("固件清单包含不安全的文件名。 ");
            }
            var path = Path.Combine(packageRoot, file.Name);
            if (!File.Exists(path) || new FileInfo(path).Length != file.Bytes)
            {
                throw new InvalidDataException($"固件文件 {file.Name} 缺失或大小不一致。 ");
            }
            var actualHash = await FileSha256Async(path, cancellationToken);
            if (!actualHash.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException($"固件文件 {file.Name} 的 SHA-256 校验失败。 ");
            }
            flashFiles.Add(new FirmwareFlashFile(file.Name, file.Offset, path, file.Bytes, file.Sha256));
        }
        var appBytes = flashFiles.FirstOrDefault(file => file.Offset.Equals("0x10000", StringComparison.OrdinalIgnoreCase))?.Bytes
            ?? flashFiles.Max(file => file.Bytes);
        progress?.Report(new FirmwareDeploymentProgress("固件就绪", "云端固件包和全部二进制已通过 SHA-256 校验", 100));
        return new FirmwareBuildPackage(
            jobId, packageRoot, manifestPath, manifest.Chip,
            manifest.Before, manifest.After, manifest.WriteFlashArgs, flashFiles, appBytes);
    }

    public async Task FlashDongleAsync(
        FirmwareBuildPackage package,
        string portName,
        IProgress<FirmwareDeploymentProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (!PortRegex().IsMatch(portName))
        {
            throw new InvalidOperationException("请选择有效的 COM 串口。 ");
        }
        foreach (var file in package.Files)
        {
            if (!File.Exists(file.Path) ||
                !(await FileSha256Async(file.Path, cancellationToken)).Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException($"烧录前校验失败：{file.Name} 已损坏或被替换。 ");
            }
        }
        var esptool = Path.Combine(AppContext.BaseDirectory, "Tools", "esptool.exe");
        if (!File.Exists(esptool) ||
            !(await FileSha256Async(esptool, cancellationToken)).Equals(EsptoolSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("内置烧录工具缺失或完整性校验失败，请重新安装 MoveToPlay Companion。 ");
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = esptool,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        foreach (var value in new[]
        {
            "--chip", package.Chip, "--port", portName, "--baud", "460800",
            "--before", package.Before, "--after", package.After, "write-flash",
        })
        {
            startInfo.ArgumentList.Add(value);
        }
        foreach (var value in package.WriteFlashArgs)
        {
            startInfo.ArgumentList.Add(value);
        }
        foreach (var file in package.Files.OrderBy(file =>
                     int.Parse(file.Offset.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture)))
        {
            startInfo.ArgumentList.Add(file.Offset);
            startInfo.ArgumentList.Add(file.Path);
        }

        progress?.Report(new FirmwareDeploymentProgress("烧录 Dongle", $"正在通过 {portName} 写入已校验云端固件，请勿拔线", 3));
        using var process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        var tail = new Queue<string>();
        void Capture(string? line)
        {
            if (string.IsNullOrWhiteSpace(line)) return;
            tail.Enqueue(line);
            while (tail.Count > 30) tail.Dequeue();
            var match = FlashProgressRegex().Match(line);
            var percent = 3d;
            if (match.Success && double.TryParse(match.Groups[1].Value, NumberStyles.Float,
                    CultureInfo.InvariantCulture, out var flashed))
            {
                percent = 3 + Math.Clamp(flashed, 0, 100) * 0.95;
            }
            progress?.Report(new FirmwareDeploymentProgress("烧录 Dongle", line, percent, !match.Success));
        }
        process.OutputDataReceived += (_, args) => Capture(args.Data);
        process.ErrorDataReceived += (_, args) => Capture(args.Data);
        try
        {
            if (!process.Start()) throw new InvalidOperationException("无法启动内置烧录工具。 ");
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            throw;
        }
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"内置烧录工具执行失败（退出码 {process.ExitCode}）。\n{string.Join(Environment.NewLine, tail)}");
        }
        progress?.Report(new FirmwareDeploymentProgress("烧录完成", "请长按 Dongle 按钮或重新上电回到绿色 Play 模式", 100));
    }

    private static async Task<string> FileSha256Async(string path, CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, true);
        return Convert.ToHexString(await SHA256.HashDataAsync(stream, cancellationToken));
    }

    private static void ValidateManagedPath(string path)
    {
        var expectedRoot = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MoveToPlay", "firmware") + Path.DirectorySeparatorChar;
        if (!Path.GetFullPath(path).StartsWith(Path.GetFullPath(expectedRoot), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("固件工作目录不在应用缓存范围内。 ");
        }
    }

    private sealed class CloudFirmwareManifest
    {
        [JsonPropertyName("schema_version")] public int SchemaVersion { get; init; }
        [JsonPropertyName("job_id")] public string JobId { get; init; } = "";
        [JsonPropertyName("board_profile")] public int BoardProfile { get; init; }
        [JsonPropertyName("chip")] public string Chip { get; init; } = "";
        [JsonPropertyName("before")] public string Before { get; init; } = "default-reset";
        [JsonPropertyName("after")] public string After { get; init; } = "hard-reset";
        [JsonPropertyName("write_flash_args")] public List<string> WriteFlashArgs { get; init; } = [];
        [JsonPropertyName("files")] public List<CloudFirmwareFile> Files { get; init; } = [];
    }

    private sealed class CloudFirmwareFile
    {
        [JsonPropertyName("name")] public string Name { get; init; } = "";
        [JsonPropertyName("offset")] public string Offset { get; init; } = "";
        [JsonPropertyName("bytes")] public long Bytes { get; init; }
        [JsonPropertyName("sha256")] public string Sha256 { get; init; } = "";
    }

    [GeneratedRegex(@"^COM\d+$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex PortRegex();
    [GeneratedRegex(@"^0x[0-9a-f]+$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex OffsetRegex();
    [GeneratedRegex(@"\(\s*(\d+(?:\.\d+)?)\s*%\s*\)")]
    private static partial Regex FlashProgressRegex();
}
