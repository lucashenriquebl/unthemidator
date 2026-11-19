# Visual Studio 2026 - Unicorn and Zydis dependencies

## Symptom

"Dependencia ausente: dependencies\unicorn\CMakeLists.txt."

The project's Git submodules are not populated. It is not a missing Visual Studio component. The repository declares dependencies/unicorn and dependencies/zydis. Zydis also needs dependencies/zycore as a nested submodule.

The updated scripts/build-v14-vs2026.ps1 initializes the missing submodules automatically, but only inside a real Git checkout with a .git directory.

## Recommended commands (PowerShell)

From a Git checkout, after merging the fix PR:

    cd C:\S21Lab\unthemidator
    git switch master
    git pull origin master
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1

The script runs git submodule sync --recursive and git submodule update --init --recursive before CMake when needed.

To check the dependencies manually:

    git submodule sync --recursive
    git submodule update --init --recursive
    Test-Path .\dependencies\unicorn\CMakeLists.txt
    Test-Path .\dependencies\zydis\CMakeLists.txt
    Test-Path .\dependencies\zydis\dependencies\zycore\CMakeLists.txt

All three Test-Path commands should print True.

## If using GitHub "Download ZIP"

That ZIP excludes the .git metadata and all submodule contents. The git submodule command cannot reconstruct pinned versions in this folder.

Clone into a fresh directory without destroying current files:

    git clone --recurse-submodules https://github.com/lucashenriquebl/unthemidator.git C:\S21Lab\unthemidator-git
    powershell -NoProfile -ExecutionPolicy Bypass -File C:\S21Lab\unthemidator-git\scripts\build-v14-vs2026.ps1

## CMake and MSBuild — VS2026

A previous build script incorrectly accepted CMake 4.0 but demanded the
"Visual Studio 18 2026" generator, introduced only in **CMake 4.2**.

The updated script now supports both paths with Visual Studio 2026:

- **CMake 4.2+ with VS2026 generator:** generates a Visual Studio 2026
  solution and builds it with MSBuild as before.
- **CMake 3.31–4.1 (or a newer CMake without VS2026 generator):** initializes
  the VS2026 x64 MSVC compiler environment using VsDevCmd.bat, uses Ninja to
  generate/build, and places the executable and static catalogs in
  `build\Release` so existing V25/V26/V27 scripts remain compatible.

Run:

    cd C:\S21Lab\unthemidator-git
    git switch master
    git pull origin master
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1

If the build takes the Ninja fallback path, the CMake cache is isolated under
`build-vs2026-ninja`, and the log is written to
`build-vs2026-ninja\muflex_v14_ninja.log`.

Required: the Visual Studio 2026 **Desktop development with C++** workload,
MSBuild, CMake 3.31+, and either Ninja or a CMake 4.2+ VS2026 generator.
The script checks these requirements and prints actionable errors.

If your VS edition is not installed at
`C:\Program Files\Microsoft Visual Studio\18\Community`, pass `-VS`
with the actual installation directory.

The compatibility fallback has been reviewed statically and requires a
Windows build to confirm it on the user's installed toolchain. Do not
execute reconstructed Stage0 binaries.

## V26 and later

After building, run the Unicorn diagnostic probe against the *original*
GameServer input in a disposable VM. The observation does not mean a complete
unpacked GameServer has been obtained.
