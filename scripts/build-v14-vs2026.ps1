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
    throw "MSBuild do Visual Studio 2026 nao encontrado: $MSBuild. Informe -VS com a instalacao correta."
}

# Visual Studio 18 2026 was added as a CMake generator in CMake 4.2.
# CMake 3.31/4.1 can still compile the same MSVC sources via Ninja.
$Candidates = @()
$PathCMake = Get-Command cmake.exe -ErrorAction SilentlyContinue
if ($PathCMake) { $Candidates += $PathCMake.Source }
if (Test-Path -LiteralPath $VSCMake -PathType Leaf) { $Candidates += $VSCMake }

$CMakeVS = $null
$CMakeNinja = $null
foreach ($Candidate in ($Candidates | Select-Object -Unique)) {
    $VersionLine = (& $Candidate --version | Select-Object -First 1)
    if ($LASTEXITCODE -ne 0 -or $VersionLine -notmatch 'cmake version (\d+\.\d+(?:\.\d+)?)') {
        continue
    }
    $VersionText = $Matches[1]
    if ([version]$VersionText -lt [version]"3.31") {
        Write-Host "Ignorando CMake antigo: $Candidate ($VersionText)" -ForegroundColor Yellow
        continue
    }
    if (-not $CMakeNinja) { $CMakeNinja = $Candidate }

    # CMake --help includes the generator name only when actually supported.
    if ([version]$VersionText -ge [version]"4.2") {
        $HelpText = (& $Candidate --help | Out-String)
        if ($LASTEXITCODE -eq 0 -and $HelpText.Contains("Visual Studio 18 2026")) {
            $CMakeVS = $Candidate
            Write-Host "CMake $VersionText com gerador Visual Studio 2026: $CMakeVS" -ForegroundColor Cyan
            break
        }
    }
}

if (-not $CMakeNinja) {
    throw "CMake 3.31+ nao encontrado. O CMakeLists.txt exige 4.0+. Execute cmake --version e instale CMake 4.2+."
}

$ExeDir = Join-Path $Build "Release"
$Exe = Join-Path $ExeDir "unthemidator.exe"

if ($CMakeVS) {
    # Native Visual Studio generator: preserve the existing .sln/MSBuild path.
    New-Item -ItemType Directory -Path $Build -Force | Out-Null
    $Cache = Join-Path $Build "CMakeCache.txt"
    if (Test-Path -LiteralPath $Cache) {
        $CachedGenerator = Get-Content -LiteralPath $Cache |
            Where-Object { $_ -like "CMAKE_GENERATOR:INTERNAL=*" } | Select-Object -First 1
        if ($CachedGenerator -and $CachedGenerator -ne "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026") {
            throw "A pasta build usa outro gerador: $CachedGenerator. Mova a pasta build para um backup e execute novamente (nao apague seus arquivos)."
        }
    }

    & $CMakeVS -S $Source -B $Build -G "Visual Studio 18 2026" -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake nao conseguiu configurar o Visual Studio 2026 (exit code $LASTEXITCODE)." }

    $Solution = Get-ChildItem -LiteralPath $Build -File |
        Where-Object { $_.Extension -in ".sln", ".slnx" } | Select-Object -First 1
    if (-not $Solution) { throw "CMake nao gerou .sln/.slnx em $Build." }

    $Log = Join-Path $Build "muflex_v14_msbuild.log"
    & $MSBuild $Solution.FullName /t:Build /p:Configuration=Release /p:Platform=x64 /m:4 /v:minimal /fl "/flp:logfile=$Log;verbosity=normal"
    if ($LASTEXITCODE -ne 0) { throw "MSBuild falhou. Envie o primeiro erro de: $Log" }
}
else {
    # Fallback: MSVC from Visual Studio 2026 + Ninja. No VS18 CMake generator needed.
    Write-Host "Este CMake nao oferece 'Visual Studio 18 2026'. Utilizando Ninja com o MSVC do VS2026." -ForegroundColor Yellow
    $VsDevCmd = Join-Path $VS "Common7\Tools\VsDevCmd.bat"
    if (-not (Test-Path -LiteralPath $VsDevCmd -PathType Leaf)) {
        throw "VsDevCmd.bat ausente: $VsDevCmd. Instale o componente C++ Desktop no Visual Studio 2026."
    }
    $Ninja = $null
    $PathNinja = Get-Command ninja.exe -ErrorAction SilentlyContinue
    if ($PathNinja) { $Ninja = $PathNinja.Source }
    $VSNinja = Join-Path $VS "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
    if (-not $Ninja -and (Test-Path -LiteralPath $VSNinja -PathType Leaf)) { $Ninja = $VSNinja }
    if (-not $Ninja) {
        throw "Ninja nao encontrado. Instale Ninja (ou atualize para CMake 4.2+ e use o gerador VS2026)."
    }

    # Import the VS2026 compiler environment into the current PowerShell process.
    $DevCommand = 'call "' + $VsDevCmd + '" -no_logo -arch=amd64 -host_arch=amd64 >nul && set'
    $EnvLines = & $env:ComSpec /c $DevCommand
    if ($LASTEXITCODE -ne 0) {
        throw "Falha ao inicializar o compilador VS2026 com VsDevCmd.bat (exit code $LASTEXITCODE)."
    }
    foreach ($Line in $EnvLines) {
        if ($Line -match '^([^=]+)=(.*)$') {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], "Process")
        }
    }
    if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
        throw "cl.exe nao encontrado apos VsDevCmd.bat. Instale o workload Desktop development with C++."
    }

    # Separate configuration directory avoids CMake generator-cache collisions.
    # Output stays in build\Release, required by the V25/V26 runtime runners.
    $NinjaBuild = Join-Path $Source "build-vs2026-ninja"
    New-Item -ItemType Directory -Path $NinjaBuild -Force | Out-Null
    New-Item -ItemType Directory -Path $ExeDir -Force | Out-Null
    $NinjaLog = Join-Path $NinjaBuild "muflex_v14_ninja.log"

    & $CMakeNinja -S $Source -B $NinjaBuild -G "Ninja" "-DCMAKE_BUILD_TYPE:STRING=Release" "-DCMAKE_MAKE_PROGRAM:FILEPATH=$Ninja" "-DCMAKE_RUNTIME_OUTPUT_DIRECTORY:PATH=$ExeDir"
    if ($LASTEXITCODE -ne 0) {
        throw "CMake/Ninja falhou ao configurar. Verifique o MSVC, o Ninja e o CMake (exit code $LASTEXITCODE)."
    }

    & $CMakeNinja --build $NinjaBuild --parallel 4 2>&1 | Tee-Object -FilePath $NinjaLog
    if ($LASTEXITCODE -ne 0) { throw "Build Ninja/MSVC falhou. Envie o primeiro erro de: $NinjaLog" }
    $Log = $NinjaLog
}

if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) {
    throw "Build retornou sucesso, mas EXE esta ausente: $Exe"
}
Write-Host "BUILD OK: $Exe" -ForegroundColor Green
Write-Host "LOG: $Log"
