# Passive V27 candidate lifecycle probe; runs original protected PE in Unicorn only.
param(
    [string] $Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string] $InputFile = "C:\S21Lab\GameServer(5).exe",
    [string] $Output = "C:\S21Lab\resultados_v27_gameserver",
    [ValidateRange(1000, 2000000)] [int] $Budget = 2000000,
    [ValidateRange(15, 600)] [int] $TimeoutSeconds = 600
)
$ErrorActionPreference = "Stop"
$Runner = Join-Path $PSScriptRoot "run-v26-probe.ps1"
if (-not (Test-Path -LiteralPath $Runner -PathType Leaf)) {
    throw "V26 runner nao encontrado: $Runner"
}
& $Runner -Repo $Repo -InputFile $InputFile -Output $Output -Budget $Budget -TimeoutSeconds $TimeoutSeconds
$Files = @(
    "v27_candidate_intervals.csv",
    "v27_candidate_checkpoints.csv",
    "v27_candidate_top_pcs.csv",
    "v27_candidate_flags.txt"
)
foreach ($File in $Files) {
    if (-not (Test-Path -LiteralPath (Join-Path $Output $File) -PathType Leaf)) {
        throw "V27 nao produziu $File. Confira runtime_stdout.log, runtime_stderr.log e runtime_branch_summary.txt."
    }
}
$Result = Join-Path (Split-Path -Parent ([IO.Path]::GetFullPath($Output))) "Resultados_V27.zip"
Compress-Archive -Path (Join-Path $Output "*") -DestinationPath $Result -Force
Write-Host "Resultados V27: $Result" -ForegroundColor Green
Write-Host "Nao execute o GameServer Stage0 nem binarios reconstruidos." -ForegroundColor Yellow
