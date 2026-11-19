# Visual Studio 2026 - Unicorn, Zydis and installed CMake

## Submodule dependencies

The project uses Git submodules for Unicorn and Zydis. Zydis also has the
nested Zycore submodule. If a dependency is missing in a real Git checkout:

    cd C:\S21Lab\unthemidator-git
    git submodule sync --recursive
    git submodule update --init --recursive

Check that these files exist:

    Test-Path .\dependencies\unicorn\CMakeLists.txt
    Test-Path .\dependencies\zydis\CMakeLists.txt
    Test-Path .\dependencies\zydis\dependencies\zycore\CMakeLists.txt

All three commands should print True.

A GitHub "Download ZIP" does not include Git metadata or submodule contents.
Use a recursive clone in a new folder rather than overwriting the existing
source tree:

    git clone --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git C:\S21Lab\unthemidator-git

## Use the CMake already installed on Windows

The build script DOES NOT download, install, extract, or update CMake.
It only locates and runs an installation that is already present.

Detection order includes:

- cmake.exe available on the current PowerShell PATH (Get-Command and where.exe)
- CMake bundled with the selected Visual Studio installation
- Standard Kitware install locations under Program Files and LocalAppData
- Common Chocolatey, Scoop and WinGet installation folders
- Windows uninstall registry entries that expose a CMake install location

After installing CMake, close and reopen PowerShell so the updated PATH is
loaded. Confirm the installation:

    Get-Command cmake.exe -All
    where.exe cmake
    cmake --version

The project requires CMake 3.31 or newer. The build script uses the native
"Visual Studio 18 2026" generator when the installed CMake supports it
(CMake 4.2+). With CMake 3.31 through 4.1, it uses Ninja with the MSVC
compiler environment from Visual Studio 2026.

### If CMake is installed in a custom folder

Pass the full path to the installed executable. This does not download
anything:

    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1 -CMakeExecutable "D:\Tools\CMake\bin\cmake.exe"

You can also pass a directory containing bin\cmake.exe.

### Build

After accepting the PR and updating the local Git checkout:

    cd C:\S21Lab\unthemidator-git
    git switch master
    git pull origin master
    git submodule update --init --recursive
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1

If CMake is not detected, the script stops and prints diagnostic commands
and an example -CMakeExecutable invocation. It does not try a network
download or modify the system PATH.

## Output and logs

The executable remains at:

    build\Release\unthemidator.exe

For the Ninja fallback, the build log is:

    build-vs2026-ninja\muflex_v14_ninja.log

Required Visual Studio component: Desktop development with C++.
The fallback requires Ninja, either on PATH or bundled with Visual Studio.

## V26 and later

Run the diagnostic probe against the original GameServer binary in an
isolated VM. A bounded runtime trace does not by itself mean the original
GameServer has been completely unpacked. Do not execute reconstructed
Stage0 binaries.
