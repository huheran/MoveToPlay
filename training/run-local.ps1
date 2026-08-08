param(
    [string]$RunId = "",
    [string]$DatasetManifest = "",
    [switch]$ValidateOnly,
    [switch]$RequireReferenceMatch,
    [switch]$Reinstall
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$VenvRoot = Join-Path $ProjectRoot ".venv-training"
$PythonExe = Join-Path $VenvRoot "Scripts\python.exe"
$Requirements = Join-Path $PSScriptRoot "requirements.txt"
$DefaultDatasetManifest = Join-Path $PSScriptRoot "datasets\movetoplay-latest-v2.json"

if (-not (Test-Path -LiteralPath $PythonExe)) {
    Write-Host "[setup] creating isolated training environment: $VenvRoot"
    python -m venv $VenvRoot
    $Reinstall = $true
}

if ($Reinstall) {
    Write-Host "[setup] installing pinned training dependencies"
    & $PythonExe -m pip install --upgrade pip
    & $PythonExe -m pip install --requirement $Requirements
}

$Arguments = @(
    (Join-Path $ProjectRoot "tools\run_training_pipeline.py"),
    "--dataset-manifest", $(if ($DatasetManifest) { $DatasetManifest } else { $DefaultDatasetManifest })
)
if ($RunId) {
    $Arguments += @("--run-id", $RunId)
}
if ($ValidateOnly) {
    $Arguments += "--validate-only"
}
if ($RequireReferenceMatch) {
    $Arguments += "--require-reference-match"
}

Write-Host "[run] $PythonExe $($Arguments -join ' ')"
& $PythonExe @Arguments
exit $LASTEXITCODE
