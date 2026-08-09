using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using MoveToPlay.Companion.Models;

namespace MoveToPlay.Companion.Services;

public sealed partial class FirmwareDeploymentService
{
    private static readonly string[] RequiredModelFiles =
    [
        "rf_model_generated.c",
        "rf_model_generated.h",
        "rf_state_model_generated.c",
        "rf_state_model_generated.h",
    ];

    public async Task<FirmwareBuildPackage> BuildDongleAsync(
        string jobId,
        string modelCacheDirectory,
        IProgress<FirmwareDeploymentProgress>? progress = null,
        CancellationToken cancellationToken = default)
    {
        var projectRoot = FindProjectRoot();
        var generatedSource = Path.Combine(modelCacheDirectory, "generated");
        foreach (var fileName in RequiredModelFiles)
        {
            if (!File.Exists(Path.Combine(generatedSource, fileName)))
            {
                throw new FileNotFoundException($"训练产物缺少 {fileName}，请先重新加载云端任务。 ");
            }
        }

        var deploymentRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay",
            "firmware",
            jobId);
        var workspace = Path.Combine(deploymentRoot, "workspace");
        ValidateManagedPath(workspace);
        if (Directory.Exists(workspace))
        {
            Directory.Delete(workspace, recursive: true);
        }
        Directory.CreateDirectory(workspace);

        progress?.Report(new FirmwareDeploymentProgress("准备源码", "正在建立隔离的 Dongle 固件工作区", 5));
        CopyProjectFile(projectRoot, workspace, "CMakeLists.txt");
        CopyProjectFile(projectRoot, workspace, "dependencies.lock");
        CopyProjectFile(projectRoot, workspace, "partitions.csv");
        CopyProjectFile(projectRoot, workspace, "partitions_16mb.csv");
        CopyProjectFile(projectRoot, workspace, "sdkconfig.defaults");
        CopyProjectFile(projectRoot, workspace, "sdkconfig.defaults.16mb");
        CopyDirectory(Path.Combine(projectRoot, "main"), Path.Combine(workspace, "main"));
        CopyDirectory(Path.Combine(projectRoot, "managed_components"), Path.Combine(workspace, "managed_components"));

        var generatedDestination = Path.Combine(workspace, "main", "generated");
        foreach (var fileName in RequiredModelFiles)
        {
            File.Copy(Path.Combine(generatedSource, fileName), Path.Combine(generatedDestination, fileName), overwrite: true);
        }
        var appMainPath = Path.Combine(workspace, "main", "app_main.c");
        var appMain = await File.ReadAllTextAsync(appMainPath, cancellationToken);
        var dongleAppMain = BoardProfileRegex().Replace(appMain, "#define M2P_BOARD_PROFILE             1", count: 1);
        if (dongleAppMain == appMain && !appMain.Contains("#define M2P_BOARD_PROFILE             1", StringComparison.Ordinal))
        {
            throw new InvalidDataException("无法在固件源码中设置 Dongle 板型。 ");
        }
        await File.WriteAllTextAsync(appMainPath, dongleAppMain, new UTF8Encoding(false), cancellationToken);

        var idfPath = FindIdfPath(projectRoot);
        var activationScript = FindIdfActivationScript(idfPath);

        var buildPath = Path.Combine(workspace, "build-dongle");
        progress?.Report(new FirmwareDeploymentProgress("编译固件", "正在把云端模型 C 数组编译进 Dongle 固件，首次可能需要数分钟", 12));
        await RunIdfAsync(
            idfPath,
            activationScript,
            [
                "-B",
                buildPath,
                "-D",
                $"SDKCONFIG={Path.Combine(buildPath, "sdkconfig")}",
                "-D",
                "SDKCONFIG_DEFAULTS=sdkconfig.defaults.16mb",
                "build",
            ],
            workspace,
            progress,
            "编译固件",
            12,
            94,
            cancellationToken);

        var appBinary = Path.Combine(buildPath, "esp_idf_template.bin");
        var bootloader = Path.Combine(buildPath, "bootloader", "bootloader.bin");
        var partitions = Path.Combine(buildPath, "partition_table", "partition-table.bin");
        foreach (var path in new[] { appBinary, bootloader, partitions })
        {
            if (!File.Exists(path))
            {
                throw new FileNotFoundException($"固件编译完成但缺少产物：{path}");
            }
        }

        var manifestPath = Path.Combine(deploymentRoot, "firmware-manifest.json");
        var manifest = new
        {
            schema_version = 1,
            job_id = jobId,
            created_at = DateTimeOffset.UtcNow,
            board_profile = 1,
            files = new[]
            {
                await FirmwareFileAsync("bootloader.bin", bootloader, "0x0", cancellationToken),
                await FirmwareFileAsync("partition-table.bin", partitions, "0x8000", cancellationToken),
                await FirmwareFileAsync("esp_idf_template.bin", appBinary, "0x10000", cancellationToken),
            },
        };
        await File.WriteAllTextAsync(
            manifestPath,
            JsonSerializer.Serialize(manifest, new JsonSerializerOptions(JsonSerializerDefaults.Web) { WriteIndented = true }),
            cancellationToken);
        progress?.Report(new FirmwareDeploymentProgress("固件就绪", $"Dongle 应用固件 {new FileInfo(appBinary).Length / 1024d / 1024d:0.00} MiB", 100));
        return new FirmwareBuildPackage(
            jobId,
            workspace,
            buildPath,
            appBinary,
            manifestPath,
            new FileInfo(appBinary).Length);
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
        if (!File.Exists(package.AppBinaryPath))
        {
            throw new FileNotFoundException("待烧录的 Dongle 固件不存在，请重新生成。 ");
        }
        var projectRoot = FindProjectRoot();
        var idfPath = FindIdfPath(projectRoot);
        var activationScript = FindIdfActivationScript(idfPath);
        progress?.Report(new FirmwareDeploymentProgress("烧录 Dongle", $"正在通过 {portName} 写入已批准模型，请勿拔线", 3));
        await RunIdfAsync(
            idfPath,
            activationScript,
            ["-B", package.BuildPath, "-p", portName, "flash"],
            package.WorkspacePath,
            progress,
            "烧录 Dongle",
            3,
            98,
            cancellationToken);
        progress?.Report(new FirmwareDeploymentProgress("烧录完成", "请长按 Dongle 按钮或重新上电回到绿色 Play 模式", 100));
    }

    private static async Task<object> FirmwareFileAsync(
        string name,
        string path,
        string offset,
        CancellationToken cancellationToken) => new
        {
            name,
            offset,
            bytes = new FileInfo(path).Length,
            sha256 = await FileSha256Async(path, cancellationToken),
        };

    private static async Task<string> FileSha256Async(string path, CancellationToken cancellationToken)
    {
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 81920, useAsync: true);
        var hash = await System.Security.Cryptography.SHA256.HashDataAsync(stream, cancellationToken);
        return Convert.ToHexString(hash);
    }

    private static async Task RunIdfAsync(
        string idfPath,
        string? activationScript,
        IReadOnlyList<string> arguments,
        string workingDirectory,
        IProgress<FirmwareDeploymentProgress>? progress,
        string toolStage,
        double progressStart,
        double progressEnd,
        CancellationToken cancellationToken)
    {
        var usePowerShell = !string.IsNullOrWhiteSpace(activationScript);
        var commandScript = Path.Combine(
            workingDirectory,
            $".movetoplay-command-{Guid.NewGuid():N}.{(usePowerShell ? "ps1" : "cmd")}");
        if (usePowerShell)
        {
            var invocation = string.Join(' ', arguments.Select(value => $"'{value.Replace("'", "''")}'"));
            await File.WriteAllTextAsync(
                commandScript,
                "$ErrorActionPreference = 'Stop'\r\n" +
                $". '{activationScript!.Replace("'", "''")}'\r\n" +
                $"& idf.py {invocation}\r\n" +
                "exit $LASTEXITCODE\r\n",
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: true),
                cancellationToken);
        }
        else
        {
            var exportScript = Path.Combine(idfPath, "export.bat");
            if (!File.Exists(exportScript))
            {
                throw new FileNotFoundException("找不到 ESP-IDF export.bat，请先安装或配置 ESP-IDF。", exportScript);
            }
            var invocation = string.Join(' ', arguments.Select(CmdQuote));
            await File.WriteAllTextAsync(
                commandScript,
                $"@echo off\r\ncall \"{exportScript}\" >nul && idf.py {invocation}\r\n",
                new UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                cancellationToken);
        }
        var startInfo = new ProcessStartInfo
        {
            FileName = usePowerShell ? "powershell.exe" : Environment.GetEnvironmentVariable("COMSPEC") ?? "cmd.exe",
            WorkingDirectory = workingDirectory,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
        };
        if (usePowerShell)
        {
            startInfo.ArgumentList.Add("-NoProfile");
            startInfo.ArgumentList.Add("-NonInteractive");
            startInfo.ArgumentList.Add("-ExecutionPolicy");
            startInfo.ArgumentList.Add("Bypass");
            startInfo.ArgumentList.Add("-File");
        }
        else
        {
            startInfo.ArgumentList.Add("/d");
            startInfo.ArgumentList.Add("/c");
        }
        startInfo.ArgumentList.Add(commandScript);
        using var process = new Process { StartInfo = startInfo, EnableRaisingEvents = true };
        var tail = new Queue<string>();
        void Capture(string? line)
        {
            if (string.IsNullOrWhiteSpace(line))
            {
                return;
            }
            tail.Enqueue(line);
            while (tail.Count > 30)
            {
                tail.Dequeue();
            }
            var percent = ParseToolProgress(line, progressStart, progressEnd);
            progress?.Report(new FirmwareDeploymentProgress(toolStage, line, percent, percent <= progressStart));
        }
        process.OutputDataReceived += (_, eventArgs) => Capture(eventArgs.Data);
        process.ErrorDataReceived += (_, eventArgs) => Capture(eventArgs.Data);
        try
        {
            if (!process.Start())
            {
                throw new InvalidOperationException("无法启动 ESP-IDF 工具。 ");
            }
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            if (!process.HasExited)
            {
                process.Kill(entireProcessTree: true);
            }
            throw;
        }
        finally
        {
            File.Delete(commandScript);
        }
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"ESP-IDF 工具执行失败（退出码 {process.ExitCode}）。\n{string.Join(Environment.NewLine, tail)}");
        }
    }

    private static double ParseToolProgress(string line, double start, double end)
    {
        var ninja = NinjaProgressRegex().Match(line);
        if (ninja.Success &&
            double.TryParse(ninja.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var current) &&
            double.TryParse(ninja.Groups[2].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var total) && total > 0)
        {
            return start + (end - start) * Math.Clamp(current / total, 0, 1);
        }
        var flash = FlashProgressRegex().Match(line);
        if (flash.Success && double.TryParse(
                flash.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var written))
        {
            return start + (end - start) * Math.Clamp(written / 100.0, 0, 1);
        }
        return start;
    }

    private static string CmdQuote(string value) => $"\"{value.Replace("\"", "\"\"")}\"";

    private static string FindProjectRoot()
    {
        foreach (var start in new[] { AppContext.BaseDirectory, Environment.CurrentDirectory })
        {
            var directory = new DirectoryInfo(start);
            while (directory is not null)
            {
                if (File.Exists(Path.Combine(directory.FullName, "CMakeLists.txt")) &&
                    File.Exists(Path.Combine(directory.FullName, "main", "app_main.c")))
                {
                    return directory.FullName;
                }
                directory = directory.Parent;
            }
        }
        throw new DirectoryNotFoundException("找不到 MoveToPlay 固件工程；当前版本需要从工程发布目录运行。 ");
    }

    private static string FindIdfPath(string projectRoot)
    {
        var environmentPath = Environment.GetEnvironmentVariable("IDF_PATH");
        if (!string.IsNullOrWhiteSpace(environmentPath) && Directory.Exists(environmentPath))
        {
            return Path.GetFullPath(environmentPath);
        }
        var settingsPath = Path.Combine(projectRoot, ".vscode", "settings.json");
        if (File.Exists(settingsPath))
        {
            using var document = JsonDocument.Parse(File.ReadAllText(settingsPath));
            if (document.RootElement.TryGetProperty("idf.currentSetup", out var configured))
            {
                var value = configured.GetString();
                if (!string.IsNullOrWhiteSpace(value) && Directory.Exists(value))
                {
                    return Path.GetFullPath(value);
                }
            }
        }
        throw new DirectoryNotFoundException("未找到 ESP-IDF。请先在 VS Code 中完成 ESP-IDF 配置。 ");
    }

    private static string? FindIdfActivationScript(string idfPath)
    {
        var candidates = new[]
        {
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".espressif", "eim_idf.json"),
            Path.Combine(Path.GetPathRoot(Environment.SystemDirectory) ?? "C:\\", "Espressif", "tools", "eim_idf.json"),
        };
        foreach (var candidate in candidates.Where(File.Exists))
        {
            try
            {
                using var document = JsonDocument.Parse(File.ReadAllText(candidate));
                if (!document.RootElement.TryGetProperty("idfInstalled", out var installations))
                {
                    continue;
                }
                foreach (var installation in installations.EnumerateArray())
                {
                    var configuredIdf = installation.GetProperty("path").GetString();
                    var script = installation.TryGetProperty("activationScript", out var activation)
                        ? activation.GetString()
                        : null;
                    if (!string.IsNullOrWhiteSpace(configuredIdf) && !string.IsNullOrWhiteSpace(script) &&
                        Path.GetFullPath(configuredIdf).Equals(Path.GetFullPath(idfPath), StringComparison.OrdinalIgnoreCase) &&
                        File.Exists(script))
                    {
                        return Path.GetFullPath(script);
                    }
                }
            }
            catch (Exception exception) when (exception is IOException or JsonException or InvalidOperationException)
            {
                // Continue to the legacy export.bat activation path.
            }
        }
        return null;
    }

    private static void CopyProjectFile(string sourceRoot, string destinationRoot, string relativePath)
    {
        var source = Path.Combine(sourceRoot, relativePath);
        if (!File.Exists(source))
        {
            throw new FileNotFoundException($"固件工程缺少 {relativePath}", source);
        }
        File.Copy(source, Path.Combine(destinationRoot, relativePath), overwrite: true);
    }

    private static void CopyDirectory(string source, string destination)
    {
        if (!Directory.Exists(source))
        {
            throw new DirectoryNotFoundException($"固件工程目录不存在：{source}");
        }
        Directory.CreateDirectory(destination);
        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, Path.Combine(destination, Path.GetFileName(file)), overwrite: true);
        }
        foreach (var directory in Directory.EnumerateDirectories(source))
        {
            CopyDirectory(directory, Path.Combine(destination, Path.GetFileName(directory)));
        }
    }

    private static void ValidateManagedPath(string path)
    {
        var expectedRoot = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MoveToPlay",
            "firmware") + Path.DirectorySeparatorChar;
        var fullPath = Path.GetFullPath(path);
        if (!fullPath.StartsWith(Path.GetFullPath(expectedRoot), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException("固件工作目录不在应用缓存范围内。 ");
        }
    }

    [GeneratedRegex(@"^#define\s+M2P_BOARD_PROFILE\s+\d+\s*$", RegexOptions.Multiline | RegexOptions.CultureInvariant)]
    private static partial Regex BoardProfileRegex();

    [GeneratedRegex(@"\[(\d+)\s*/\s*(\d+)\]")]
    private static partial Regex NinjaProgressRegex();

    [GeneratedRegex(@"\(\s*(\d+(?:\.\d+)?)\s*%\s*\)")]
    private static partial Regex FlashProgressRegex();

    [GeneratedRegex(@"^COM\d+$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex PortRegex();
}
