# Install a portable, trusted CMake in the user's local profile, without admin rights.
# Emits only the absolute cmake.exe path on stdout; progress goes to Write-Host.
param(
    [string] $Version = "4.2.6",
    [string] $InstallRoot = ""
)

$ErrorActionPreference = "Stop"
if (-not $InstallRoot) {
    $Root = $env:LOCALAPPDATA
    if (-not $Root) { $Root = [Environment]::GetFolderPath("LocalApplicationData") }
    if (-not $Root) { throw "LOCALAPPDATA indisponivel. Informe -InstallRoot para armazenar o CMake." }
    $InstallRoot = Join-Path $Root "MuFlex\Tools\CMake"
}
$InstallRoot = [System.IO.Path]::GetFullPath($InstallRoot)
$PackageName = "cmake-$Version-windows-x86_64"
$ArchiveName = "$PackageName.zip"
$InstallDir = Join-Path $InstallRoot $PackageName
$Exe = Join-Path $InstallDir "bin\cmake.exe"

function Test-ExpectedVersion([string] $CMakePath, [string] $ExpectedVersion) {
    if (-not (Test-Path -LiteralPath $CMakePath -PathType Leaf)) { return $false }
    $Banner = (& $CMakePath --version | Select-Object -First 1)
    $Pattern = '^cmake version ' + [regex]::Escape($ExpectedVersion) + '(?:\s|$)'
    return ($LASTEXITCODE -eq 0 -and [bool]($Banner -match $Pattern))
}

if (Test-ExpectedVersion $Exe $Version) {
    Write-Host "CMake portatil ja disponivel: $Exe" -ForegroundColor Green
    Write-Output $Exe
    return
}

New-Item -Path $InstallRoot -ItemType Directory -Force | Out-Null
$Tmp = Join-Path $InstallRoot ("download-" + [Guid]::NewGuid().ToString("N"))
New-Item -Path $Tmp -ItemType Directory -Force | Out-Null

$ReleaseBase = "https://github.com/Kitware/CMake/releases/download/v$Version"
$HashFile = "cmake-$Version-SHA-256.txt"
$ZipPath = Join-Path $Tmp $ArchiveName
$HashPath = Join-Path $Tmp $HashFile
$ExtractDir = Join-Path $Tmp "extract"

try {
    # Windows PowerShell 5.1 needs TLS 1.2 for GitHub releases.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Write-Host "Baixando CMake $Version oficial da Kitware (sem instalar no Windows)..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri "$ReleaseBase/$HashFile" -OutFile $HashPath -UseBasicParsing -ErrorAction Stop
    Invoke-WebRequest -Uri "$ReleaseBase/$ArchiveName" -OutFile $ZipPath -UseBasicParsing -ErrorAction Stop

    $ExpectedHash = $null
    foreach ($Line in (Get-Content -LiteralPath $HashPath)) {
        if ($Line -match '^\s*([a-fA-F0-9]{64})\s+\*?(.+?)\s*$') {
            if ($Matches[2] -eq $ArchiveName) {
                $ExpectedHash = $Matches[1]
                break
            }
        }
    }
    if (-not $ExpectedHash) {
        throw "Checksum nao encontrado no arquivo oficial $HashFile para $ArchiveName."
    }
    $ActualHash = (Get-FileHash -LiteralPath $ZipPath -Algorithm SHA256).Hash
    if (-not $ActualHash.Equals($ExpectedHash, [StringComparison]::OrdinalIgnoreCase)) {
        throw "SHA-256 da copia do CMake nao confere. Download descartado."
    }
    Write-Host "SHA-256 verificado." -ForegroundColor Green

    Expand-Archive -LiteralPath $ZipPath -DestinationPath $ExtractDir -Force
    $Extracted = Join-Path $ExtractDir $PackageName
    $ExtractedExe = Join-Path $Extracted "bin\cmake.exe"
    if (-not (Test-ExpectedVersion $ExtractedExe $Version)) {
        throw "ZIP oficial extraido sem o cmake.exe esperado: $ExtractedExe"
    }

    if (Test-Path -LiteralPath $InstallDir) {
        # Only removes an incomplete version-specific local cache, never user files.
        Remove-Item -LiteralPath $InstallDir -Recurse -Force
    }
    Move-Item -LiteralPath $Extracted -Destination $InstallDir
    if (-not (Test-ExpectedVersion $Exe $Version)) {
        throw "CMake portatil instalado, mas nao pode ser executado: $Exe"
    }
    Write-Host "CMake portatil pronto: $Exe" -ForegroundColor Green
    Write-Output $Exe
}
catch {
    throw "Falha ao preparar CMake $Version automaticamente: $($_.Exception.Message). Verifique acesso ao GitHub Releases ou instale CMake >=3.31 e use -CMakeExecutable."
}
finally {
    if (Test-Path -LiteralPath $Tmp) {
        Remove-Item -LiteralPath $Tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}
