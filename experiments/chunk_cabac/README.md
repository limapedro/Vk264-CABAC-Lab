# Exact chunk-summary CABAC — CPU reference prototype

Validates `notes/SPECULATIVE_HYBRID.md` §4 (read-only from the lab repo) before
any Vulkan work. Parent repo `E:\__PROJECTS__\Vk264` is **read only**: this
program reads numeric tables and source semantics from it at runtime, and never
writes to it.

## Files

| File | Purpose |
|---|---|
| `chunk_cabac.cpp` | x264-compatible serial CABAC + chunk-summary encoder, 5 bin streams, byte-exactness gates, timing |
| `build.bat` | MSVC x64 build (`/O2 /std:c++17`) |
| `results/chunk_cabac_results.json` | per-stream / per-chunk-size verification data |
| `results/timing_summary.json` | median of 5 timing trials + GPU extrapolation |
| `notes/VALIDATION.md` | full findings: gates, timing, data layout, traps, verdict, implementation plan |

## Build & run

```
cd E:\__PROJECTS__\Vk264-CABAC-Lab\experiments\chunk_cabac
build.bat
chunk_cabac.exe
```

Optional arguments: `chunk_cabac.exe <path-to-tables.c> <output.json>`.

Exit code 0 = all byte-exactness gates passed; 2 = a gate failed; 1 = error.

## What it tests

* Serial path is a verbatim port of `common/cabac.c:53-183` (`putbyte`,
  `renorm`, `decision`, `bypass`, `terminal`, flush), with a one-byte sentinel
  modelling the slice-header byte that x264's `p[-1]` carry patch targets.
* Chunk path: Stage 1 checkpoint (contexts + `low/range/queue/outstanding`),
  Stage 2 independent per-chunk encode with 2 carry-in variants and a 1-bit
  `carry_out`, Stage 3 merge, Stage 4 EPB insertion.
* Chunk sizes **8 / 16 / 32 / 64 / 128 / 256** on **5** deterministic streams.
* Dense and sparse (touched-context-only) checkpoints.
* Negative control: context-only checkpoint with reset arithmetic state.
* Timing on 250,000 bins, extrapolated to 300k bins at 1.5 GHz.
