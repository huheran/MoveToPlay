[CmdletBinding()]
param(
    [string]$SshAlias = "movetoplay-server",
    [string]$Configuration = "Release",
    [switch]$SkipCloudSmoke
)

$ErrorActionPreference = "Stop"
$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$project = Join-Path $repoRoot "companion\MoveToPlay.Companion\MoveToPlay.Companion.csproj"
$smokeProject = Join-Path $repoRoot "companion\MoveToPlay.Companion.Smoke\MoveToPlay.Companion.Smoke.csproj"
$publishDir = Join-Path $repoRoot "output\companion-team-win-x64-self-contained"
$installerDir = Join-Path $repoRoot "output\installer"
$secretRoot = Join-Path $env:LOCALAPPDATA "MoveToPlay\installer-secrets"
$privateKeyPath = Join-Path $secretRoot "team_api_ed25519"
$publicKeyPath = "$privateKeyPath.pub"
$sshKeygen = Join-Path $env:WINDIR "System32\OpenSSH\ssh-keygen.exe"

New-Item -ItemType Directory -Force -Path $secretRoot, $installerDir | Out-Null
if (Test-Path -LiteralPath $publishDir) {
    Remove-Item -LiteralPath $publishDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $publishDir | Out-Null
if (-not (Test-Path -LiteralPath $privateKeyPath)) {
    & $sshKeygen -q -t ed25519 -N "" -C "movetoplay-team-installer-api-only" -f $privateKeyPath
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to generate the team SSH key."
    }
}
if (-not (Test-Path -LiteralPath $publicKeyPath)) {
    throw "Team SSH public key is missing: $publicKeyPath"
}

$sshConfig = & ssh -G $SshAlias
if ($LASTEXITCODE -ne 0) {
    throw "Unable to read SSH config: $SshAlias"
}
function Read-SshConfigValue([string]$name) {
    $line = $sshConfig | Where-Object { $_ -match "^$name\s+" } | Select-Object -First 1
    if (-not $line) { throw "SSH config is missing $name" }
    return ($line -split "\s+", 2)[1].Trim()
}
$hostName = Read-SshConfigValue "hostname"
$userName = Read-SshConfigValue "user"
$sshPort = [int](Read-SshConfigValue "port")

$hostPublicKey = (& ssh $SshAlias "cat /etc/ssh/ssh_host_ed25519_key.pub").Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($hostPublicKey)) {
    throw "Unable to read the server Ed25519 host key."
}
$hostKeyParts = $hostPublicKey -split "\s+"
if ($hostKeyParts.Length -lt 2 -or $hostKeyParts[0] -ne "ssh-ed25519") {
    throw "The server returned an invalid Ed25519 host key."
}
$hostKeyBytes = [Convert]::FromBase64String($hostKeyParts[1])
$hostKeySha256 = [Convert]::ToBase64String(
    [System.Security.Cryptography.SHA256]::HashData($hostKeyBytes)).TrimEnd('=')

$tokenCommand = 'awk -F= ''$1 == "MOVETOPLAY_API_TOKEN" {sub(/^[^=]*=/, ""); print; exit}'' /home/movetoplay/MoveToPlay/shared/server.env'
$apiToken = (& ssh $SshAlias $tokenCommand).Trim()
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($apiToken)) {
    throw "Unable to read the server API token."
}

Write-Host "[1/5] Publishing self-contained win-x64 app"
dotnet publish $project -c $Configuration -r win-x64 --self-contained true `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:DebugType=None -p:DebugSymbols=false -o $publishDir
if ($LASTEXITCODE -ne 0) { throw "Companion self-contained publish failed." }

$credential = [ordered]@{
    schema_version = 1
    host = $hostName
    port = $sshPort
    username = $userName
    host_key_sha256 = "SHA256:$hostKeySha256"
    api_token = $apiToken
    private_key_pem = Get-Content -LiteralPath $privateKeyPath -Raw
}
$bootstrapPath = Join-Path $publishDir "team-cloud.bootstrap.json"
$credential | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $bootstrapPath -Encoding utf8NoBOM

Write-Host "[2/5] Verifying published files"
$required = @(
    "MoveToPlay.Companion.exe",
    "Profiles\genshin.json",
    "Tools\esptool.exe",
    "Tools\esptool-LICENSE.txt",
    "team-cloud.bootstrap.json"
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $publishDir $relative))) {
        throw "Published file is missing: $relative"
    }
}

if (-not $SkipCloudSmoke) {
    Write-Host "[3/5] Running cloud smoke test with bundled credentials"
    dotnet build $smokeProject -c $Configuration
    if ($LASTEXITCODE -ne 0) { throw "Companion smoke project build failed." }
    $smokeDir = Join-Path $repoRoot "companion\MoveToPlay.Companion.Smoke\bin\$Configuration\net8.0-windows"
    $smokeBootstrap = Join-Path $smokeDir "team-cloud.bootstrap.json"
    Copy-Item -LiteralPath $bootstrapPath -Destination $smokeBootstrap -Force
    $testCredentialRoot = Join-Path $env:TEMP ("movetoplay-team-credential-smoke-" + [Guid]::NewGuid().ToString("N"))
    $oldCredentialRoot = $env:MOVETOPLAY_CREDENTIAL_ROOT
    try {
        $env:MOVETOPLAY_CREDENTIAL_ROOT = $testCredentialRoot
        & dotnet (Join-Path $smokeDir "MoveToPlay.Companion.Smoke.dll")
        if ($LASTEXITCODE -ne 0) { throw "Team credential cloud smoke test failed." }
        if (-not (Test-Path -LiteralPath (Join-Path $testCredentialRoot "team-cloud.dat"))) {
            throw "Team credentials were not migrated to a DPAPI protected file."
        }
        if (Test-Path -LiteralPath $smokeBootstrap) {
            throw "Bootstrap credentials were not removed after first use."
        }
    }
    finally {
        $env:MOVETOPLAY_CREDENTIAL_ROOT = $oldCredentialRoot
        if (Test-Path -LiteralPath $smokeBootstrap) { Remove-Item -LiteralPath $smokeBootstrap -Force }
        if (Test-Path -LiteralPath $testCredentialRoot) { Remove-Item -LiteralPath $testCredentialRoot -Recurse -Force }
    }
}
else {
    Write-Host "[3/5] Cloud smoke test skipped"
}

Write-Host "[4/5] Compiling installer"
$isccCandidates = @(
    (Join-Path ${env:ProgramFiles(x86)} "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:ProgramFiles "Inno Setup 6\ISCC.exe"),
    (Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe")
) | Where-Object { $_ -and (Test-Path -LiteralPath $_) }
$iscc = $isccCandidates | Select-Object -First 1
if (-not $iscc) {
    throw "Inno Setup 6 was not found."
}
$commit = (git -C $repoRoot rev-parse --short=8 HEAD).Trim()
$version = "1.0." + [int](Get-Date -Format "MMdd") + "." + [int](Get-Date -Format "HHmm")
& $iscc "/DPublishDir=$publishDir" "/DOutputDir=$installerDir" "/DAppVersion=$version" `
    (Join-Path $PSScriptRoot "MoveToPlay.Companion.iss")
if ($LASTEXITCODE -ne 0) { throw "Installer compilation failed." }

$installerPath = Join-Path $installerDir "MoveToPlay.Companion.Team.Setup.exe"
if (-not (Test-Path -LiteralPath $installerPath)) {
    throw "Installer output is missing."
}
$hash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
$checksumPath = "$installerPath.sha256.txt"
"$hash  $([System.IO.Path]::GetFileName($installerPath))" |
    Set-Content -LiteralPath $checksumPath -Encoding ascii
Write-Host "[5/5] Completed"
Write-Host "Installer: $installerPath"
Write-Host "Version: $version ($commit)"
Write-Host "SHA-256: $hash"
