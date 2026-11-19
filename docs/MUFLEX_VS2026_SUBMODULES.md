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

## CMake and MSBuild

The project currently requires CMake >=4.0. The updated builder attempts the system CMake first, then Visual Studio's bundled CMake, and verifies support for the Visual Studio 18 2026 generator.

If your edition is not in C:\Program Files\Microsoft Visual Studio\18\Community, pass -VS with your installation path.

For compiler failures after dependency setup, send the first MSBuild error in build\muflex_v14_msbuild.log. This change has not been built on Windows here.

## V26

Once the tool compiles, run scripts/run-v26-probe.ps1 inside an isolated VM with the original GameServer binary. Never execute reconstructed Stage0 binaries directly.
