# Build the MuFlex V14 diagnostics with Visual Studio 2026 MSBuild.
# Does not run or load a protected GameServer executable.
param(
    [string] $Source = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path,
    [string] $VS = "C:\Program Files\Microsoft Visual Studio\18\Community"
)

$ErrorActionPreference = "Stop"

$MSBuild = Join-Path $VS "MSBuild\Current\Bin\MSBuild.exe"
$CMake = Join-Path $VS "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$Build = Join-Path $Source "build"

if (-not (Test-Path $MSBuild)) { throw "MSBuild nao encontrado: $MSBuild" }
foreach ($dep in @("dependencies\unicorn\CMakeLists.txt", "dependencies\zydis\CMakeLists.txt")) {
    if (-not (Test-Path (Join-Path $Source $dep))) {
        throw "Dependencia ausente: $dep. Execute git submodule update --init --recursive"
    }
}

$Solution = Get-ChildItem $Build -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in ".sln", ".slnx" } | Select-Object -First 1

if (-not $Solution) {
    if (-not (Test-Path $CMake)) { throw "CMake do Visual Studio nao localizado: $CMake" }
    & $CMake -S $Source -B $Build -G "Visual Studio 18 2026" -A x64
    if ($LASTEXITCODE -ne 0) { throw "Falha ao gerar a solucao com CMake" }
    $Solution = Get-ChildItem $Build -File |
        Where-Object { $_.Extension -in ".sln", ".slnx" } | Select-Object -First 1
}
if (-not $Solution) { throw "Arquivo .sln/.slnx nao encontrado em $Build" }

$Log = Join-Path $Build "muflex_v14_msbuild.log"
& $MSBuild $Solution.FullName /t:Build /p:Configuration=Release /p:Platform=x64 /m:4 /v:minimal /fl "/flp:logfile=$Log;verbosity=normal"
if ($LASTEXITCODE -ne 0) {
    throw "Build falhou. Envie o primeiro erro de: $Log"
}

$Exe = Join-Path $Build "Release\unthemidator.exe"
if (-not (Test-Path $Exe)) { throw "MSBuild retornou sucesso, mas o EXE esta ausente: $Exe" }
Write-Host "BUILD V14 OK: $Exe" -ForegroundColor Green
Write-Host "LOG: $Log"
