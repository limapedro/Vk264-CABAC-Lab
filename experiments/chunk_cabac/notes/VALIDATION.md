# Validation of the exact chunk-summary CABAC design (SPECULATIVE_HYBRID.md §4)

**Date:** 2026-11-27
**Scope:** CPU reference prototype only. Parent repo `E:\__PROJECTS__\Vk264` was
read-only; numeric tables (`common/tables.c`) and semantics (`common/cabac.c`)
were read at runtime. Artifacts live in
`experiments/chunk_cabac/` and `C:\Users\multi\AppData\Local\Temp\opencode\chunkproto\`.

## Command

```
cd E:\__PROJECTS__\Vk264-CABAC-Lab\experiments\chunk_cabac
build.bat && chunk_cabac.exe
```

Machine: i5-12400F (6C/12T, 2.5 GHz base / 4.4 GHz turbo; invariant TSC ≈ 2.5 GHz).
Timing = median of 5 full process runs, each averaging 7 inner reps on a
250,000-bin stream. Raw data: `results/chunk_cabac_results.json`,
`results/timing_summary.json`.

---

## 1. Byte-exactness (task 1) — PASS

The serial path is a line-for-line port of `cabac.c:53-183` (`putbyte`,
`renorm`, `decision`, `bypass`, `terminal`, flush). A one-byte sentinel models
the slice-header byte targeted by x264's `p[-1] += carry` at `cabac.c:89`; the
same sentinel is present in both paths. Tables are parsed from the parent's
`tables.c` (comments stripped, so `/* 0 - 10 */` labels do not pollute the data).

| Stream | Bins | CABAC bytes | 8/16/32/64/128/256 exact | sparse checkpoints exact |
|---|---:|---:|---|---|
| state-driven QP26 I, seed 264 | 4,096 | 458 | PASS (all 6) | PASS |
| state-driven QP18 P model 1, seed 777 | 8,192 | 895 | PASS (all 6) | PASS |
| structured mb_type / skip / CBP / residual | 6,000 | 632 | PASS (all 6) | PASS |
| structured residual (sign bypass + terminal) | 12,000 | 1,289 | PASS (all 6) | PASS |
| 250k-bin stress stream | 250,000 | 26,932 | PASS (all 6) | PASS |

Also verified for every stream × chunk size:

* **EPB output identical** when emulation-prevention insertion is applied
  after the merge (`epb_exact_after_merge = true`).
* **carry_in_variant_mismatches = 0**: the two carry-in variants emit
  byte-identical local streams and the same `carry_out`. Section 4's
  "2-variant summary" is conservative — in x264's formulation the incoming
  carry never changes this chunk's own bytes; it only patches the previous
  chunk's last byte. One variant suffices; halve the chunk-output buffer.
* Cross-chunk `carry_out` edits occur in ~7-14 % of 64-bin chunks on the small
  streams and 405/3,907 = 10.4 % on the stress stream (prior simulator: 4.7 %;
  same order, stream-dependent).
* Max `i_bytes_outstanding` at a 64-bin chunk boundary was 1 in our streams
  (never zero-risk: the count is carried in the checkpoint).
* Chunks that emit no local byte occur at chunk size 8 (up to 2,870/31,250 on
  the stress stream) but are handled: the merge patches the global last byte,
  which may belong to an earlier chunk.

A genuine x264 *bin-level* trace was not available: `ffmpeg trace_headers` and
the x264 CLI expose syntax elements, not CABAC bins, and instrumenting the
parent encoder would require modifying the read-only tree. Following the
lab's earlier note, streams are deterministic and state-driven; two are
structured to exercise H.264 syntax classes (mb_skip runs on ctx 11, mb_type,
CBP 76-83, significant/last/level flags, bypass signs, terminal bins).

---

## 2. Stage-1 cost (task 2) — mixed verdict

Measured TSC cycles/bin (median of 5 trials), converted to a **single-threaded
GPU lane at 1.5 GHz** with 300k bins. CPU ns/bin is also given; TSC frequency
assumed 2.5 GHz.

| Serial prefix variant | CPU cycles/bin | CPU ns/bin | CPU ms @300k | GPU ms @300k (same cycles, 1.5 GHz) |
|---|---:|---:|---:|---:|
| **Literal §4 Stage 1** — context transitions only | **5.15** | 2.06 | 0.62 | **1.03** |
| Corrected Stage 1 — contexts + light arithmetic, no snapshot writes | 14.68 | 5.88 | 1.76 | 2.94 |
| Corrected Stage 1 + dense 460 B context snapshot/chunk | 14.47 | 5.80 | 1.74 | 2.89 |
| Corrected Stage 1 + snapshot vector allocation (as implemented) | 29.33 | 11.75 | 3.53 | 5.87 |
| Reference: full serial arithmetic encode (for scale) | 20.38 | 8.17 | 2.45 | 4.08 |
| Reference: whole CPU chunked encode (prefix + all chunks) | 96.48 | 38.65 | 11.60 | — |

**Verdict on the <2 ms estimate:** it **holds for the literal context-only
pass** (~1.0 ms on a 1.5 GHz single lane), but that pass alone cannot seed the
arithmetic chunks (below). The *usable* prefix costs ~2.9-5.9 ms at 1.5 GHz
under the same-cycles assumption — above the spec's 0.7-2 ms claim, still
20-40× better than the measured 117 ms IDR prototype and inside the 16.7 ms
frame budget. Even a 3× GPU IPC penalty on the lean 2.9 ms variant leaves ~9 ms. The estimate should be
revised to **~3-6 ms/frame for a single-lane serial checkpoint prefix**, with
the per-context parallel variant as the path to sub-2 ms.

Caveat: GPU threads will not match x86 cycles/bin; random context gathers are
less coalesced. This is an extrapolation, not a GPU measurement. The CPU
number for the literal context-only pass (5.15 cycles/bin) agrees with the
spec's 5-10 cycles/bin assumption.

---

## 3. Data layout for a Vulkan implementation (task 3)

Assumptions: 300k bins/frame, 64-bin chunks → 4,688 chunks, ~32.3 KB CABAC
payload/frame (measured 26,932 B at 250k bins = 9.27 bins/byte), 39 distinct
contexts/chunk (measured 38.66).

| Buffer | Layout | Count | Bytes/frame |
|---|---|---:|---:|
| Stage 0 bin events | `uint16 ctx; uint8 bin; uint8 kind` = 4 B (`ctx`=0..459; bypass/terminal tagged) | 300,000 | 1,200,000 |
| Dense per-chunk checkpoint | 460 B contexts + 16 B (`low,range,queue,outstanding`) = 476 B | 4,688 | 2,231,488 |
| Sparse per-chunk checkpoint | 39 × (`uint16 ctx; uint8 state`) = 117 B + 16 B arith ≈ 133 B | 4,688 | 623,504 |
| Sparse, packed (`uint8 ctx; uint8 state`) | 39 × 2 B + 16 B ≈ 94 B | 4,688 | 440,672 |
| Local bytes, one variant | ~6.9 B/chunk + 1 B `carry_out` (or 1 bit packed) | 4,688 | ~37,000 |
| Local bytes, second variant (unnecessary — see §1) | same | 4,688 | ~37,000 |
| Merge buffer | CABAC payload + 1 slice-header prefix byte | 1 | 32,319 |
| EPB output | ≤ payload + insertions | 1 | ~32,350 |

**Total:** ~3.5 MB/frame with dense checkpoints, ~1.7 MB/frame with sparse
records and one variant, ~1.5 MB with packed sparse records. Two-variant
output adds only ~37 KB, so keeping it is cheap if the shader team wants the
interface exactly as written in §4; it is not needed for correctness.

Recommended GPU structures:

```c
struct BinEvent { uint16_t ctx; uint8_t bin; uint8_t kind; }; // 4 B
struct ArithCP  { int32_t low, range, queue, outstanding; };  // 16 B
struct SparseCP { uint32_t entry_offset, count; ArithCP a; }; // 12 B
struct SparseEntry { uint16_t ctx; uint8_t state, pad; };     // 4 B (or 2 B packed)
struct ChunkSummary { uint32_t byte_offset, byte_count; uint8_t carry_out; };
```

Stage 1 writes `SparseCP` + `SparseEntry`; Stage 2 writes `ChunkSummary` into
a private region per chunk (no atomics if each chunk owns a fixed slot);
Stage 3 is a parallel prefix over `carry_out` bits applying one `+1` per set
bit to the preceding emitted byte, then concatenates; Stage 4 inserts EPB.

---

## 4. Correctness traps (task 4)

1. **Carry across 0xFF runs.** `cabac.c:69-99` defers 0xFF bytes in
   `i_bytes_outstanding` and flushes them as `(carry-1)` when a non-FF byte is
   finally emitted. A chunk starting with `outstanding > 0` must inherit the
   count; its first non-FF emission flushes the run *into its own local
   buffer* while its `carry_out` patches the previous chunk's last byte. We
   saw `outstanding = 1` at boundaries; the design carries the count, so this
   is handled. A chunk with zero emitted bytes simply passes `outstanding`
   forward.
2. **EPB interaction.** Emulation-prevention bytes must be inserted **after**
   merge: inserting per chunk would need the previous chunk's final bytes
   (`00 00 xx`), and the carry patch can change the final byte (e.g. `03→04`,
   removing a required EPB, or `02→03`, requiring one). Our post-merge EPB
   output matches the serial path for all streams; one EPB insertion was
   observed in the 250k stream.
3. **Slice / context reset.** `cabac.c:48-59` reinitialises contexts per slice
   type/QP/model and resets `low/range/queue/outstanding`. Chunks must never
   span a slice boundary; Stage 0 must emit a slice-start marker and the last
   chunk of each slice runs `x264_cabac_encode_flush` semantics, including the
   `(0x35a4e4f5 >> (frame & 31)) & 1` final bit (`cabac.c:174`). Our prototype
   passes the frame index to the last chunk's flush.
4. **Contexts that depend on earlier bins (mb_type skip runs, CBP, residual).**
   These context indices are functions of *already-decided syntax values*
   (neighbour MB types/skip flags, CBPs, neighbouring coefficients), not of
   CABAC output. Stage 0 has already made those syntax decisions on the GPU
   and can emit `(ctx, bin)` in exact decode order — including long skip runs
   on ctx 11 — before entropy coding starts. Stage 1 walks that ordered list
   and applies `transition[state][bin]`; no prediction, no rollback, no
   conflict. The failure mode is a Stage 0 that reorders or groups bins by
   syntax element for parallelism: that changes every downstream context
   state and breaks conformance. Our structured streams exercise this path.
5. **Literal §4 Stage 1 is insufficient (measured blocker).** With true
   contexts but arithmetic state reset to slice init, chunk merging was
   **not** byte-exact for any of the 30 stream × chunk-size combinations
   (`context_only_checkpoint_sufficient = false` everywhere). `low`, `range`,
   `queue` and `outstanding` evolve with the bins and are needed to seed each
   chunk. Section 4 must add a light arithmetic pass (no byte writes, no `p`
   cursor, no EPB) to Stage 1; it costs ~9-10 cycles/bin more than
   context-only on this CPU.
6. **Carry before the stream start.** `cabac.c:89` can patch `p[-1]` when the
   very first emission of a slice carries. A merge buffer that starts at the
   CABAC payload would write out of bounds; reserve one prefix byte for the
   slice header's last byte (our sentinel).
7. **`low` masking and signed `queue`.** The checkpoint must store `low`
   *after* the `(0x400<<queue)-1` mask and `queue` as signed (starts at −9);
   storing unmasked `low` produces different first bytes in the chunk.
8. **Bypass sign convention.** `x264_cabac_encode_bypass_c` documents that
   `b` is negated by the caller. Stage 0 must emit the same convention or all
   bypass bins invert (byte-exactness test would catch it).
9. **Terminal bins.** They change `range` but not contexts and must be
   included in Stage 1's arithmetic pass; our residual stream covers them.
10. **Small chunks emit no bytes.** At 8 bins, 5,870/31,250 = 18.8 % of
    stress-stream chunks emit zero local bytes; `carry_out` must still be
    propagated, and the merge patch must target the global last emitted byte,
    not a local one.

---

## 5. Verdict and implementation plan (task 5)

**Verdict: YES — worth a full Vulkan prototype**, with one design correction:
Stage 1 must compute the full light checkpoint (contexts **and**
`low/range/queue/outstanding`), not contexts alone. The core premise is
confirmed: byte-exact chunking needs only a 1-bit carry interface; no
speculation, no conflict/rollback, no CPU round trip. Expected CABAC time
~3-6 ms/frame single-lane (still ≪ 117 ms and < 16.7 ms), with a clear
optimisation path to ~1 ms via the per-context scan in §4.

### Step-by-step plan with byte-exactness gates

| Step | Work | Gate (must pass before next step) |
|---|---|---|
| 0 | Freeze CPU golden vectors: bin events, per-chunk checkpoints, per-chunk local bytes, merged bytes, EPB bytes for 5 streams × 6 chunk sizes (this prototype already produces them) | `chunk_cabac.exe` exit 0; `all_byte_exact = true` |
| 1 | Implement Stage 0 on GPU: emit 4 B `BinEvent` in decode order, with slice markers and frame index | Event arrays hash-match CPU golden for all fixtures |
| 2 | Implement Stage 1 serial kernel: context transitions + light arithmetic, sparse checkpoint records | Every checkpoint (`ctx, low, range, queue, outstanding`) matches CPU golden bit-for-bit |
| 3 | Implement Stage 2 kernel: one warp per 64-bin chunk, private byte buffer, `carry_out` (one variant) | Every chunk's local bytes and `carry_out` equal CPU golden; two-variant check optional |
| 4 | Implement Stage 3 carry-merge kernel + concatenation | Merged CABAC bytes (with sentinel) byte-identical to serial golden for all fixtures |
| 5 | Implement Stage 4 EPB + flush on last chunk of each slice | Final NAL payload byte-identical; ffmpeg decodes recon identically |
| 6 | Benchmark on GTX 1660 SUPER vs 23 fps / 117 ms baseline and 16.7 ms budget | CABAC stage < 6 ms; whole-frame > 60 fps |
| 7 (optional) | Replace serial Stage 1 with per-context parallel scan (460 threads + sparse records) | Re-run gates 2-5 unchanged; no bitstream difference |

**Do not** implement speculative checkpoint/rollback (dead end) or CPU/GPU
offload (dead end) — lab notes §2 and §5 already measured those.
