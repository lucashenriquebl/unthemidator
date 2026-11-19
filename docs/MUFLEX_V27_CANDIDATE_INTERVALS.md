# MuFlex S21 - V27: candidate lifecycle after each matching 'L'

## Why V27?

Real V26 results: 29 archive entries, CRC valid, 2,000,000 Unicorn instructions observed (all classified as .themida). Twenty matching first-byte L events were followed for 128 instructions each, producing 2,560 rows. The captured windows include 1,287 nonsequential PC transitions over 105 distinct PCs.

The first three PCs are identical across all 20 windows: 0x14A3B0FC1 -> 0x14A3B0FC7 -> 0x14A753EC2. V26 therefore does not establish that LoadLibraryA, or any other L-named export, was selected, imported or called. It also does not identify the native game OEP.

## What's new

- The code-hook tracker watches the first-byte equality at RVA 0xA3B0FC1 after the verified branch signature.
- Each matching name opens a candidate segment. The segment ends at the **next** comparison at that PC, at the 131,072 instruction safety cap, or when the probe ends.
- Segment summaries record total instructions, sequential vs nonsequential PC flow, up to 4096 distinct PC keys, writes to the original image vs external guest memory, and whether any PC was outside the known .themida RVA bounds.
- Sparse checkpoints sample powers of two for the first 128 instructions and then every 1024 instructions (registers: RAX, RCX, RDX, RDI, RSI, RSP and EFLAGS).
- Each segment saves the 12 most frequent PCs; all data is bounded and read-only. The V26 detailed 128-instruction windows are preserved.

**Boundary register warning:** the boundary_rax/rcx/... columns reflect the Unicorn state at the NEXT comparison (or cap), not registers at the last executed instruction in the previous segment. The last_pc and last_instruction columns describe the last observed instruction belonging to that segment. boundary_registers_valid=0 means no Unicorn state was available when the probe ended.

## Output

- v27_candidate_intervals.csv: one summary per matching export-name candidate.
- v27_candidate_checkpoints.csv: sparse progression through long segments.
- v27_candidate_top_pcs.csv: top guest PCs for each candidate.
- v27_candidate_flags.txt: event and truncation totals.
- All V26, V25, and earlier probe outputs remain available in the same directory.

## Use on Windows with Visual Studio 2026

Merge the V27 PR, then from a real Git clone:

    cd C:\S21Lab\unthemidator
    git switch master
    git pull origin master
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\build-v14-vs2026.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\run-v27-probe.ps1 -Repo "C:\S21Lab\unthemidator" -InputFile "C:\S21Lab\GameServer(5).exe" -Output "C:\S21Lab\resultados_v27_gameserver" -Budget 2000000 -TimeoutSeconds 600

After the run, send C:\S21Lab\Resultados_V27.zip. The PE must be the original protected GameServer, not the Stage0 reconstruction. Run only in a disposable isolated VM.

## Limits and interpretation

This is a passive research instrument, not a complete Themida unpacker. An observed guest PC, candidate hash, memory write, dummy-DLL location, or branch is not proof of original game control flow or a resolved import. No reconstructed PE is executed, and the default 2M instruction budget remains enforced by the existing V18 framework. The instrumentation is compiled into the tool; Windows build and full V27 run have not been verified in this environment.

If 131,072 instructions is too short after LoadLibraryA, the cap may be raised in a later PR only if a new result shows a specific need; avoid unbounded trace logging.
