# MuFlex Season 21 - V26 export-name follow-up (GameServer x64)

## Purpose

Continue V25's bounded Unicorn investigation of the **original protected** Season 21 GameServer. In V25, 1,018 candidate names matched the beginning of the emulator's synthetic kernel32 export catalog. Twenty names began with 'L'. This probe captures the actual next executed PCs and register snapshots around each successful 'L' comparison to determine which path the protected startup takes.

This **does not** unpack Themida completely, recover game skill handlers, resolve true imports, or identify the original game entry point. The synthetic DLL belongs to the emulator, not the game binary.

## Source integration

The code is already integrated. No manual edits are necessary:

- src/helpers/muflex_runtime_export_followup.hpp: bounded, read-only trace.
- src/helpers/muflex_runtime_probe.hpp: starts, observes, and finishes V26 automatically when the V24 JNE signature validation enables the branch observer.
- scripts/run-v26-probe.ps1: uses the existing V25 runner and creates Resultados_V26.zip.

Every matched 'L' event has an independent window of up to 128 observed instruction transitions, even when windows overlap. There are at most 32 windows; excess matches are counted as dropped. The observer reads guest memory/registers only and does not alter the instruction limit. Names are validated against a restrictive ASCII set before inclusion in CSV.

Expected files: v26_L_followup.csv, v26_runtime_flags.csv, plus all V18-V25 diagnostics.

## Build in Visual Studio 2026 (Windows VM)

Check out the PR branch, or merge the PR and pull the updated master. In PowerShell:

~~~powershell
cd C:\S21Lab\unthemidator-v25
git fetch origin
git switch feat/muflex-v26-export-followup
git submodule update --init --recursive
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1 -Source "C:\S21Lab\unthemidator-v25"
~~~

Execute the original protected GameServer in **Unicorn emulation only**:

~~~powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-v26-probe.ps1 -Repo "C:\S21Lab\unthemidator-v25" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v26_gameserver" -Budget 2000000 -TimeoutSeconds 600
~~~

After execution, send C:\S21Lab\Resultados_V26.zip. A normal instruction-budget stop is not a successful unprotected PE, and the launcher may report an exit code of 2 for that bounded stop.

## Interpretation

- CSV event_id,name,insn_index,pc,size,opcode_hex,next_pc,flow_type,...: next PC is observed from the following code hook rather than inferred from status flags.
- flow_type=nonsequential is a flow transition, not by itself an API call.
- first_outside_themida is a coarse signal, **not** proof of reaching the original game's entry point.
- matched_L_events should be around 20 if the input/environment matches V25.
- If the branch signature is not verified, no V26 files are produced; inspect the earlier branch report.
- No Stage0 PE is safe to launch. Keep the original target and tool inside a disposable VM.

## Validation

The standalone observer header has passed a local GCC C++20 -fsyntax-only check against Unicorn headers. The Windows MSBuild build and full Unicorn runtime capture need to be performed on the user's machine; no claim of a Windows build or complete unpacking is made.
