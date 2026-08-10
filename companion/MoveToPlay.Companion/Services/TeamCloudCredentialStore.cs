using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace MoveToPlay.Companion.Services;

public sealed record TeamCloudCredential
{
    [JsonPropertyName("schema_version")]
    public int SchemaVersion { get; init; } = 1;

    [JsonPropertyName("host")]
    public string Host { get; init; } = string.Empty;

    [JsonPropertyName("port")]
    public int Port { get; init; } = 22;

    [JsonPropertyName("username")]
    public string UserName { get; init; } = string.Empty;

    [JsonPropertyName("host_key_sha256")]
    public string HostKeySha256 { get; init; } = string.Empty;

    [JsonPropertyName("api_token")]
    public string ApiToken { get; init; } = string.Empty;

    [JsonPropertyName("private_key_pem")]
    public string PrivateKeyPem { get; init; } = string.Empty;
}

public static class TeamCloudCredentialStore
{
    public const string BootstrapFileName = "team-cloud.bootstrap.json";

    private static readonly byte[] AdditionalEntropy =
        Encoding.UTF8.GetBytes("MoveToPlay.Companion.TeamCloudCredential.v1");

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        WriteIndented = true,
    };

    public static TeamCloudCredential? Load()
    {
        var protectedPath = ProtectedCredentialPath();
        if (File.Exists(protectedPath))
        {
            try
            {
                var protectedBytes = File.ReadAllBytes(protectedPath);
                var jsonBytes = ProtectedData.Unprotect(
                    protectedBytes, AdditionalEntropy, DataProtectionScope.CurrentUser);
                var credential = ParseAndValidate(jsonBytes, "本机团队云端凭据");
                DeleteBootstrapBestEffort();
                return credential;
            }
            catch (Exception exception) when (exception is CryptographicException or IOException or JsonException)
            {
                throw new InvalidOperationException(
                    "本机保存的团队云端凭据无法读取，请重新安装 Companion。", exception);
            }
        }

        var bootstrapPath = Path.Combine(AppContext.BaseDirectory, BootstrapFileName);
        if (!File.Exists(bootstrapPath))
        {
            return null;
        }

        try
        {
            var jsonBytes = File.ReadAllBytes(bootstrapPath);
            var credential = ParseAndValidate(jsonBytes, "安装包团队云端凭据");
            var protectedBytes = ProtectedData.Protect(
                jsonBytes, AdditionalEntropy, DataProtectionScope.CurrentUser);
            var directory = Path.GetDirectoryName(protectedPath)!;
            Directory.CreateDirectory(directory);
            var temporaryPath = protectedPath + ".tmp";
            File.WriteAllBytes(temporaryPath, protectedBytes);
            File.Move(temporaryPath, protectedPath, overwrite: true);

            DeleteBootstrapBestEffort();
            return credential;
        }
        catch (Exception exception) when (exception is CryptographicException or IOException or JsonException)
        {
            throw new InvalidOperationException("安装包携带的团队云端凭据无效。", exception);
        }
    }

    public static string ProtectedCredentialPath() =>
        Path.Combine(CredentialRoot(), "team-cloud.dat");

    private static string CredentialRoot()
    {
        var testOverride = Environment.GetEnvironmentVariable("MOVETOPLAY_CREDENTIAL_ROOT");
        return string.IsNullOrWhiteSpace(testOverride)
            ? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "MoveToPlay",
                "credentials")
            : Path.GetFullPath(testOverride);
    }

    private static TeamCloudCredential ParseAndValidate(byte[] jsonBytes, string source)
    {
        var credential = JsonSerializer.Deserialize<TeamCloudCredential>(jsonBytes, JsonOptions)
            ?? throw new JsonException($"{source}内容为空。");
        if (credential.SchemaVersion != 1 ||
            string.IsNullOrWhiteSpace(credential.Host) ||
            credential.Port is < 1 or > 65535 ||
            string.IsNullOrWhiteSpace(credential.UserName) ||
            string.IsNullOrWhiteSpace(credential.HostKeySha256) ||
            string.IsNullOrWhiteSpace(credential.ApiToken) ||
            !credential.PrivateKeyPem.Contains("PRIVATE KEY", StringComparison.Ordinal))
        {
            throw new JsonException($"{source}字段不完整。");
        }
        return credential;
    }

    private static void DeleteBootstrapBestEffort()
    {
        var bootstrapPath = Path.Combine(AppContext.BaseDirectory, BootstrapFileName);
        try
        {
            if (File.Exists(bootstrapPath))
            {
                File.Delete(bootstrapPath);
            }
        }
        catch (IOException)
        {
            // 安装目录暂时被安全软件占用不影响使用，下次启动仍会优先读取 DPAPI 文件。
        }
        catch (UnauthorizedAccessException)
        {
            // 凭据已经安全迁移；只读安装目录中的引导文件由卸载程序负责清理。
        }
    }
}
