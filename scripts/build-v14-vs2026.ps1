# Build MuFlex diagnostics with Visual Studio 2026 and CMake >= 4.0.
# Automatically initializes missing Git submodules in a real Git checkout.
# Does not run or load a protected GameServer executable.
param(
    [string] $Source = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string] $VS = "C:\Program Files\Microsoft Visual Studio\18\Community"
)

$ErrorActionPreference = "Stop"
$Source = (Resolve-Path -LiteralPath $Source).Path
$MSBuild = Join-Path $VS "MSBuild\Current\Bin\MSBuild.exe"
$VSCMake = Join-Path $VS "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Build = Join-Path $Source "build"

if (-not (Test-Path -LiteralPath (Join-Path $Source "CMakeLists.txt") -PathType Leaf)) {
    throw "Pasta de codigo-fonte invalida: $Source (CMakeLists.txt ausente)."
}

$Deps = @(
    "dependencies\unicorn\CMakeLists.txt",
    "dependencies\zydis\CMakeLists.txt",
    "dependencies\zydis\dependencies\zycore\CMakeLists.txt"
)

function Get-MissingDependencies {
    @($Deps | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $Source $_) -PathType Leaf)
    })
}

$Missing = @(Get-MissingDependencies)
if ($Missing.Count -gt 0) {
    Write-Host "Dependencias ausentes: $($Missing -join ', ')" -ForegroundColor Yellow

    # A GitHub Download ZIP is not a Git checkout and contains no submodule
    # history or gitlink revisions; cloning recursively is the reliable fix.
    if (-not (Test-Path -LiteralPath (Join-Path $Source ".git"))) {
        throw @"
Esta pasta nao possui metadados .git (possivelmente foi extraida de um ZIP).
Nao e possivel restaurar os submodulos com seguranca nessa copia.
Clone o repositorio com os submodulos e compile na copia clonada:
  git clone --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git C:\S21Lab\unthemidator-git
  powershell -ExecutionPolicy Bypass -File C:\S21Lab\unthemidator-git\scripts\build-v14-vs2026.ps1
"@
    }

    $GitCommand = Get-Command git -ErrorAction SilentlyContinue
    if (-not $GitCommand) {
        throw "Git para Windows nao encontrado no PATH. Instale Git e execute: git submodule update --init --recursive"
    }

    Write-Host "Sincronizando URLs dos submodulos..." -ForegroundColor Cyan
    & $GitCommand.Source -C $Source submodule sync --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "git submodule sync falhou (exit code $LASTEXITCODE). Verifique rede, Git e .gitmodules."
    }

    Write-Host "Baixando Unicorn, Zydis e seus submodulos..." -ForegroundColor Cyan
    & $GitCommand.Source -C $Source submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw "git submodule update falhou (exit code $LASTEXITCODE). Verifique rede e permissoes do Git."
    }

    $Missing = @(Get-MissingDependencies)
    if ($Missing.Count -gt 0) {
        throw "Dependencias continuam ausentes apos o Git: $($Missing -join ', '). Confira git submodule status --recursive."
    }
    Write-Host "Submodulos restaurados com sucesso." -ForegroundColor Green
}

if (-not (Test-Path -LiteralPath $MSBuild -PathType Leaf)) {
    throw "MSBuild do Visual Studio 2026 nao encontrado em: $MSBuild. Use -VS para indicar a pasta de instalacao."
}

# Prefer the user's CMake installed on PATH, then Visual Studio's bundled CMake.
# The project's CMakeLists.txt currently requires CMake 4.0 or newer.
$Candidates = @()
$PathCMake = Get-Command cmake -ErrorAction SilentlyContinue
if ($PathCMake) { $Candidates += $PathCMake.Source }
if (Test-Path -LiteralPath $VSCMake -PathType Leaf) { $Candidates += $VSCMake }
$CMake = $null
foreach ($Candidate in ($Candidates | Select-Object -Unique)) {
    $VersionLine = (& $Candidate --version | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or $VersionLine -notmatch 'cmake version (\d+\.\d+(?:\.\d+)?)') { continue }
    $VersionText = $Matches[1]
    if ([version]$VersionText -lt [version]"4.0") { continue }

    $HelpText = (& $Candidate --help | Out-String)
    if ($LASTEXITCODE -ne 0 -or $HelpText -notmatch "Visual Studio 18 2026") { continue }

    $CMake = $Candidate
    Write-Host "CMake encontrado: $CMake ($VersionText)" -ForegroundColor Cyan
    break
}
if (-not $CMake) {
    throw "CMake 4.0+ com gerador 'Visual Studio 18 2026' nao encontrado. Instale/atualize o CMake e verifique cmake --version e cmake --help."
}

if (-not (Test-Path -LiteralPath $Build)) {
    New-Item -ItemType Directory -Path $Build -Force | Out-Null
}

$Solution = Get-ChildItem -LiteralPath $Build -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in ".sln", ".slnx" } | Select-Object -First 1

if (-not $Solution) {
    & $CMake -S $Source -B $Build -G "Visual Studio 18 2026" -A x64
    if ($LASTEXITCODE -ne 0) { throw "Falha ao gerar solucao com CMake (exit code $LASTEXITCODE)." }
    $Solution = Get-ChildItem -LiteralPath $Build -File |
        Where-Object { $_.Extension -in ".sln", ".slnx" } | Select-Object -First 1
}
if (-not $Solution) { throw "Arquivo .sln/.slnx nao encontrado em $Build" }

$Log = Join-Path $Build "muflex_v14_msbuild.log"
& $MSBuild $Solution.FullName /t:Build /p:Configuration=Release /p:Platform=x64 /m:4 /v:minimal /fl "/flp:logfile=$Log;verbosity=normal"
if ($LASTEXITCODE -ne 0) { throw "Build falhou. Envie o primeiro erro de: $Log" }

$Exe = Join-Path $Build "Release\unthemidator.exe"
if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "MSBuild retornou sucesso, mas EXE esta ausente: $Exe"
}
Write-Host "BUILD OK: $Exe" -ForegroundColor Green
Write-Host "LOG: $Log"
