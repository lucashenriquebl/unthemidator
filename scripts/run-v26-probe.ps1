# V26: builds on the existing V25 Unicorn runtime probe.
# Run ONLY with the original protected PE inside an isolated Windows VM.
param(
    [string] $Repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string] $InputFile = "C:\S21Lab\GameServer(5).exe",
    [string] $Output = "C:\S21Lab\resultados_v26_gameserver",
    [ValidateRange(1000, 2000000)]
    [int] $Budget = 2000000,
    [ValidateRange(15, 600)]
    [int] $TimeoutSeconds = 600
)

$ErrorActionPreference = "Stop"
$Runner = Join-Path $PSScriptRoot "run-v25-probe.ps1"
if (-not (Test-Path -LiteralPath $Runner)) { throw "V25 runtime runner missing: $Runner" }

# The V26 observer is wired into the same bounded Unicorn execution path.
& $Runner -Repo $Repo -InputFile $InputFile -Output $Output -Budget $Budget -TimeoutSeconds $TimeoutSeconds
$Trace = Join-Path $Output "v26_L_followup.csv"
$Flags = Join-Path $Output "v26_runtime_flags.csv"
if (-not (Test-Path -LiteralPath $Trace)) {
    throw "V26 CSV was not produced. Check runtime_stderr.log and runtime_branch_summary.txt."
}
if (-not (Test-Path -LiteralPath $Flags)) {
    throw "V26 flags were not produced; check the V26 observer/build."
}

$Result = Join-Path (Split-Path -Parent ([System.IO.Path]::GetFullPath($Output))) "Resultados_V26.zip"
Compress-Archive -Path (Join-Path $Output "*") -DestinationPath $Result -Force
Write-Host "V26 report created: $Result" -ForegroundColor Green
Write-Host "Send Resultados_V26.zip for analysis. No rebuilt PE was executed." -ForegroundColor Yellow
