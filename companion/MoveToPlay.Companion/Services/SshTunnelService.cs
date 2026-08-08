using System.Diagnostics;
using System.IO;
using System.Net.Http;
using System.Net.Sockets;

namespace MoveToPlay.Companion.Services;

public sealed class SshTunnelService : IDisposable
{
    private readonly string _sshAlias;
    private Process? _tunnelProcess;

    public SshTunnelService(string sshAlias = "movetoplay-server")
    {
        _sshAlias = sshAlias;
    }

    public Uri? BaseUri { get; private set; }

    public bool IsRunning => _tunnelProcess is { HasExited: false };

    public async Task<string> ConnectAsync(CancellationToken cancellationToken = default)
    {
        var token = await ReadApiTokenAsync(cancellationToken);
        if (string.IsNullOrWhiteSpace(token))
        {
            throw new InvalidOperationException("服务器没有返回 API 令牌，请检查 shared/server.env。 ");
        }

        var port = FindFreeTcpPort();
        var startInfo = CreateSshStartInfo();
        startInfo.ArgumentList.Add("-N");
        startInfo.ArgumentList.Add("-T");
        startInfo.ArgumentList.Add("-o");
        startInfo.ArgumentList.Add("BatchMode=yes");
        startInfo.ArgumentList.Add("-o");
        startInfo.ArgumentList.Add("ExitOnForwardFailure=yes");
        startInfo.ArgumentList.Add("-o");
        startInfo.ArgumentList.Add("ServerAliveInterval=20");
        startInfo.ArgumentList.Add("-o");
        startInfo.ArgumentList.Add("ServerAliveCountMax=3");
        startInfo.ArgumentList.Add("-L");
        startInfo.ArgumentList.Add($"127.0.0.1:{port}:127.0.0.1:8000");
        startInfo.ArgumentList.Add(_sshAlias);

        _tunnelProcess = Process.Start(startInfo)
            ?? throw new InvalidOperationException("无法启动 Windows OpenSSH。 ");
        BaseUri = new Uri($"http://127.0.0.1:{port}");

        using var healthClient = new HttpClient { Timeout = TimeSpan.FromSeconds(2) };
        var deadline = DateTime.UtcNow.AddSeconds(12);
        while (DateTime.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (_tunnelProcess.HasExited)
            {
                var error = await _tunnelProcess.StandardError.ReadToEndAsync(cancellationToken);
                throw new InvalidOperationException($"SSH 隧道启动失败：{SanitizeError(error)}");
            }
            try
            {
                using var response = await healthClient.GetAsync(new Uri(BaseUri, "/health"), cancellationToken);
                if (response.IsSuccessStatusCode)
                {
                    return token.Trim();
                }
            }
            catch (HttpRequestException)
            {
                // ssh.exe may need a moment to finish authentication and forwarding.
            }
            catch (TaskCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                // Retry until the outer deadline.
            }
            await Task.Delay(250, cancellationToken);
        }

        Dispose();
        throw new TimeoutException("SSH 已启动，但 12 秒内没有连通服务器健康检查。 ");
    }

    public void Dispose()
    {
        if (_tunnelProcess is null)
        {
            return;
        }
        try
        {
            if (!_tunnelProcess.HasExited)
            {
                _tunnelProcess.Kill(entireProcessTree: true);
                _tunnelProcess.WaitForExit(2000);
            }
        }
        catch (InvalidOperationException)
        {
            // The process already exited between checks.
        }
        _tunnelProcess.Dispose();
        _tunnelProcess = null;
        BaseUri = null;
    }

    private async Task<string> ReadApiTokenAsync(CancellationToken cancellationToken)
    {
        var startInfo = CreateSshStartInfo();
        startInfo.ArgumentList.Add("-T");
        startInfo.ArgumentList.Add("-o");
        startInfo.ArgumentList.Add("BatchMode=yes");
        startInfo.ArgumentList.Add(_sshAlias);
        startInfo.ArgumentList.Add(
            "awk -F= '$1 == \"MOVETOPLAY_API_TOKEN\" {sub(/^[^=]*=/, \"\"); print; exit}' /home/movetoplay/MoveToPlay/shared/server.env");

        using var process = Process.Start(startInfo)
            ?? throw new InvalidOperationException("无法启动 Windows OpenSSH。 ");
        var outputTask = process.StandardOutput.ReadToEndAsync(cancellationToken);
        var errorTask = process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        var output = await outputTask;
        var error = await errorTask;
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"SSH 身份验证失败：{SanitizeError(error)}");
        }
        return output.Trim();
    }

    private static ProcessStartInfo CreateSshStartInfo() => new()
    {
        FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "OpenSSH", "ssh.exe"),
        UseShellExecute = false,
        CreateNoWindow = true,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
    };

    private static int FindFreeTcpPort()
    {
        var listener = new TcpListener(System.Net.IPAddress.Loopback, 0);
        listener.Start();
        var port = ((System.Net.IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        return port;
    }

    private static string SanitizeError(string error)
    {
        var value = string.IsNullOrWhiteSpace(error) ? "未知错误" : error.Trim();
        return value.Length <= 500 ? value : value[..500];
    }
}
