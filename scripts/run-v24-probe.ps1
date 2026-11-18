# Diagnostic-only Unicorn runtime probe; never executes the protected PE natively.
# Run in a disposable Windows VM with the original files copied to C:\S21Lab.
param(
    [string] $Repo = "C:\S21Lab\unthemidator-v24",
    [string] $InputFile = "C:\S21Lab\GameServer(5).exe",
    [string] $Output = "C:\S21Lab\resultados_v24_gameserver",
    [ValidateRange(1000, 2000000)]
    [int] $Budget = 250000,
    [ValidateRange(15, 600)]
    [int] $TimeoutSeconds = 180
)

$ErrorActionPreference = "Stop"
$Exe = Join-Path $Repo "build\Release\unthemidator.exe"
if (-not (Test-Path -LiteralPath $Exe)) { throw "Unthemidator V24 nao encontrado: $Exe" }
if (-not (Test-Path -LiteralPath $InputFile)) { throw "PE de entrada nao encontrado: $InputFile" }

$Output = [System.IO.Path]::GetFullPath($Output)
New-Item -ItemType Directory -Path $Output -Force | Out-Null

$ExeDirectory = Split-Path $Exe -Parent
foreach ($file in @("catalog.json", "catalog_32.json")) {
    $dest = Join-Path $ExeDirectory $file
    if (-not (Test-Path -LiteralPath $dest)) {
        $source = Join-Path (Join-Path $Repo "static") $file
        if (-not (Test-Path -LiteralPath $source)) {
            throw "Catalogo do Unthemidator ausente: $file"
        }
        Copy-Item -LiteralPath $source -Destination $dest
    }
}

# Log reproducibility. The input is only read by the Unthemidator emulator.
(Get-FileHash -LiteralPath $InputFile -Algorithm SHA256).Hash |
    Set-Content -LiteralPath (Join-Path $Output "input.sha256.txt")

$Stdout = Join-Path $Output "runtime_stdout.log"
$Stderr = Join-Path $Output "runtime_stderr.log"
$Args = '--runtime-probe-dir "' + $Output + '" --runtime-budget ' + $Budget +
        ' "' + [System.IO.Path]::GetFullPath($InputFile) + '"'
Write-Host "Emulando somente com Unicorn; limite: $Budget instrucoes" -ForegroundColor Cyan
Write-Host "Entrada: $InputFile"
Write-Host "Saida:   $Output"

$StartArgs = @{
    FilePath = $Exe
    ArgumentList = $Args
    WorkingDirectory = $ExeDirectory
    RedirectStandardOutput = $Stdout
    RedirectStandardError = $Stderr
    PassThru = $true
    NoNewWindow = $true
}
$Process = Start-Process @StartArgs

if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
    Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
    "timeout_seconds=$TimeoutSeconds" | Set-Content -LiteralPath (Join-Path $Output "timeout.txt")
    Write-Warning "Tempo limite atingido. Envie os logs; nao execute o Stage0."
} else {
    $Process.Refresh()
    $ExitStatus = $Process.ExitCode
    if ($null -eq $ExitStatus) {
        "exit_code=unknown; see runtime_summary.txt" |
            Set-Content -LiteralPath (Join-Path $Output "exit_status.txt")
        Write-Warning "Codigo de saida nao disponivel; utilize runtime_summary.txt."
    } else {
        "exit_code=$ExitStatus" | Set-Content -LiteralPath (Join-Path $Output "exit_status.txt")
        Write-Host "Codigo de saida: $ExitStatus"
    }
    Write-Host "O resumo V19 informa o motivo de parada (budget, retorno ou erro)."
}
Write-Host "Arquivos:"
Get-ChildItem -LiteralPath $Output -File | Select-Object Name,Length
Write-Host "IMPORTANTE: nenhum PE reconstruido foi validado ou deve ser executado." -ForegroundColor Yellow
