# Slice scaling and IDR splitting (measured)

Date: 2026-11-27. Agent: slice-count scaling (`SLICE_SCALING.md`).
Hardware: **GTX 1660 SUPER**, i5-12400F (6C/12T), MSVC 19.51, Vulkan SDK 1.4.363,
FFmpeg `N-126335-gb32f8d1c23-20260830` with `libx264` + `libvmaf`.

Isolated encoder: `C:\Users\multi\AppData\Local\Temp\opencode\slice_scale\`
(copied from `E:\__PROJECTS__\Vk264` excluding `build*/`, `dist/`, `.git/`).
Parent was never written: `git -C E:\__PROJECTS__\Vk264 status --porcelain` is empty
(parent HEAD `28a5874`; only docs commits after the `811bad4` copy point, so the
encoder code measured here is identical to the parent's).

All results: `results/slice_scale/`. Tables: `consolidated.csv`, `consolidated.json`,
`x264_idr/idr_timing25.json`, `x264_idr/idr_quality.json`.

## Q1. What do more P slices buy? (Wx264 CAVLC)

### Method

Source clips (both 1920×1080, 300 frames; read from the parent, never modified):
`build/native1080_300.yuv` (25 fps, game) and `build/obs_new_src.yuv`
(60 fps, duplicated capture). 6000 kbit/s target, `--preset slow`,
`--vmaf --check-recon --runs 3`. Exact harness command per cell:

```powershell
python harness/benchmark.py `
  --encoder C:\Users\multi\AppData\Local\Temp\opencode\slice_scale\build\Release\wx264.exe `
  --input E:\__PROJECTS__\Vk264\build\native1080_300.yuv `
  --out E:\__PROJECTS__\Vk264-CABAC-Lab\results\slice_scale\native25_rows2 `
  --width 1920 --height 1080 --frames 300 --fps 25 --bitrate 6000 `
  --encoders wx264-slow --p-slice-rows 2 --runs 3 --vmaf --check-recon
```

`--p-slice-rows 1` and `4` substitute that one value; the 60 fps clip uses
`--fps 60 --out ...\obs60_rows*`. The harness does a byte-for-byte GPU-recon
check and an `ffprobe -count_frames` / `err_detect explode` decode check.

The harness does **not** pass `--timestamps`, so GPU stage times are zero in its
logs. Entropy stage ms comes from six extra direct CLI runs with the same
arguments plus `--timestamps` (logs: `results/slice_scale/timestamps/*.json`).
`--timestamps` disables the async ring (`gpu.cpp:249`), so its loop fps is a
synchronous figure, reported separately below.

### Measured table

`entropy_ms` = `gpu_stages_mean_ms.entropy + mb_entropy`. `loop_fps` = median of
the three harness reports; `process_fps` = harness wall-clock median.

| Clip | rows/slice | P slices/frame | entropy ms/frame | loop fps | process fps | achieved kbit/s | VMAF | stream bytes | decode |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| native1080_300 25 fps | 1 | **136** | **7.477** | 73.3 | 50.5 | 6113.2 | 95.644 | 9,169,852 | 300/300 clean |
| native1080_300 25 fps | 2 | 34 | 0.227 | 133.2 | 91.6 | 6143.8 | 95.892 | 9,215,700 | 300/300 clean |
| native1080_300 25 fps | 4 | 17 | 0.259 | 137.1 | **98.9** | 6146.6 | **95.926** | 9,219,960 | 300/300 clean |
| obs_new_src 60 fps | 1 | **136** | **6.738** | 79.9 | 47.6 | 21,440.8 | 96.933 | 13,400,528 | 300/300 clean |
| obs_new_src 60 fps | 2 | 34 | 0.251 | 155.4 | 101.0 | 20,753.3 | 96.950 | 12,970,820 | 300/300 clean |
| obs_new_src 60 fps | 4 | 17 | 0.242 | 159.0 | **110.2** | 20,624.0 | **96.951** | 12,889,976 | 300/300 clean |

Synchronous (`--timestamps`) loop fps, for the entropy-window comparison:
65.1 / 114.5 / 125.0 (native, rows 1/2/4) and 75.1 / 128.1 / 143.4 (obs).

Slice counts were verified from the Annex B streams (`count_nals.py`): 1 row →
136 non-IDR slice NALs/frame, 2 rows → 34, 4 rows → 17. The brief's "68 slices"
assumes one full-width slice per MB row; the legacy 1-row path additionally
splits each 120-MB row at `sliceWidth = 64` (`presets.hpp:60-62`, `layout.glsl:36`),
so it really emits 136.

### Why 1 row is slower, not faster

`--p-slice-rows 1` is **not** "the parallel path with twice the slices".
`parallelCapable()` is `pc.sliceRows == 2 && ...` (`parallel_entropy.glsl:18`)
and `gpu.cpp:273-274`, so 1 row takes the legacy inline path in
`entropy.comp:69-109`: one invocation serialises all macroblocks of a slice
bit-by-bit. That path costs 6.7–7.5 ms/frame versus 0.23–0.26 ms for the
parallel MB-body path — a **26–33× entropy penalty**. It also forces
`references = 1, flatIntra = 0, aq = 0` (`main.cpp:158-162`) in addition to
losing `pairedRows()` spatial prediction (`layout.glsl:60`), which explains the
−0.25 VMAF (native) / −0.02 VMAF (obs) quality loss. Process fps collapses
by ~45–53%.

### 4-row prototype (implemented, not estimated)

Feasible. I patched **only the isolated copy**; the change is 17 small encoder/
shader edits (plus one harness choices edit) that generalise the hardcoded "2"
to `pc.sliceRows`:

- `src/main.cpp`: `--p-slice-rows` range 1–2 → 1–4; multi-ref guard `!= 2` → `< 2`.
- `src/gpu.cpp`: `pairedSlotWords` uses `* c.sliceRows`; `paired` uses `>= 2`;
  `sliceCount = (mbHeight + sliceRows - 1) / sliceRows` (init and per-frame);
  both scratch-size expressions use the same ceil division.
- `shaders/layout.glsl`: `pairedRows()` uses `>= 2`; `firstMb = slice * sliceRows * mbWidth`;
  `sliceLength = mbWidth * min(sliceRows, mbHeight - sliceRows * slice)`;
  `offsetInPair(mb) = mb % (sliceRows * mbWidth)`.
- `shaders/mb_entropy.comp` and `shaders/parallel_entropy.glsl`: slot mapping
  `mb / (sliceRows * mbWidth)`.
- `shaders/spatial.comp`: `approximateMvp` uses `index.y % sliceRows == 0`
  instead of `index.y & 1`, so row 2 of a 4-row slice still gets the above
  predictor (pure quality fix, identical behaviour at 2 rows).

`maxPairedSliceMbs = 512` needs **no** change at 1080p: 4×120 = 480 ≤ 512, so the
`shared` arrays in `mb_flags.comp` / `flat_intra.comp` still fit. It would need
raising for frames wider than 2048 px. `deblock_frame.comp` (default `deblock 2`)
and `deblock_frame_tiled` are slice-agnostic.

Evidence: `ffmpeg -bsf:v trace_headers` on a 4-row P frame shows
`first_mb_in_slice = 0, 480, 960, …`, i.e. 17 slices of 480 MBs;
`ffmpeg -err_detect explode` exit 0; `ctest` still 45/46 after the patch.

Result: entropy 0.259/0.242 ms (vs 0.227/0.251 at 2 rows — within ±14%, no
scaling trend), process fps **+8% / +9%**, loop fps +3% / +2%, and VMAF
**+0.034 / +0.001**. Fewer, larger slices win slightly: slice headers and three
emulation-prevention passes are amortised, while 17 workgroups still fill the GPU.

### 60 fps rate anomaly (context, not a slice effect)

`obs_new_src` achieves 20.6–21.4 Mb/s against a 6000 kbit/s target at
`min_qp 14 / max_qp 22`. This is the known ABR limitation documented in the
parent (`docs/improvement_roadmap.md`, commit `45b8794`: "ABR adaptive floor
confirmed blocked", "baseline ABR at 21 Mb/s reaches VMAF 96.996"). The slice
comparison is valid as a *relative* A/B at equal settings; do not read the
absolute kbit/s as rate accuracy.

## Q2. Can the whole-frame IDR be split legally?

### Rule and source cite

H.264 intra prediction may only use samples in the **same slice**. x264 enforces
this in `x264-vulkan/common/macroblock.c:778-785` (left), `797-802` (top),
`822-827` (top-left), `837-842` (top-right): each neighbour sets
`i_neighbour_intra` only when `h->mb.slice_table[neighbour] == h->sh.i_first_mb`;
`analyse.c:615,692,1025,1042` then restrict the available prediction modes.
`common.h:495` labels that field "for constrained intra pred", and
`macroblock.c:788` notes "We can't predict from the previous threadslice since
it hasn't been encoded yet."

So: **yes**, a 1080p IDR can be emitted as 1, 34 or 68 legal slices. At the first
MB row of every slice the above/above-left/above-right samples are unavailable,
so x264 falls back to DC/available modes; `Wx264` avoids this by using one
whole-frame IDR slice (`layout.glsl:48-52`, `gpu.cpp:582-587`). Splitting is legal
but costs prediction efficiency.

### x264 timing (no standalone x264.exe installed)

Used the FFmpeg GPL build's `libx264` with the same `-x264-params` (x264 core).
No entropy-only timer is exposed, so I encoded 25 repeated identical IDR frames
(`keyint=1`) per process and divided wall time; `--qp 18 --preset slow`.
Five process repeats per cell; command template:

```powershell
ffmpeg -nostdin -hide_banner -loglevel error -y `
  -f rawvideo -pixel_format yuv420p -video_size 1920x1080 -framerate 25 `
  -stream_loop 24 -i <scratch>\idr1.yuv -frames:v 25 -an `
  -c:v libx264 -preset slow -qp 18 -threads 12 `
  -x264-params "slices=68:keyint=1:scenecut=0:constrained-intra=0" -f h264 out.h264
```

Median ms per 1080p IDR (25-frame average, 5 process repeats):

| threads | slices | constrained-intra | ms/IDR | IDR NALs/frame | decode |
|---:|---:|---:|---:|---:|---|
| 1 | 1 | 0 | 149.2 (noisy: 136–381) | 1 | clean |
| 1 | 34 | 0 | 160.7 | 34 | clean |
| 1 | 68 | 0 | 131.4 | 68 | clean |
| 12 | 1 | 0 | **24.33** | 1 | clean |
| 12 | 34 | 0 | 25.18 | 34 | clean |
| 12 | 68 | 0 | **23.72** | 68 | clean |
| 12 | 68 | 1 | 23.66 | 68 | clean |

At 12 threads the 1→68-slice change is **−0.6 ms (−2.5%)**, i.e. inside run
noise: x264 already parallelises a single IDR across threads, so more slices
buy essentially no extra throughput on this CPU. The 1→12-thread change at
1 slice is 149→24 ms (≈6×), so the serial-IDR cost is a *threading* issue, not
a *slice-count* issue in real x264. This is unlike the GPU prototype's
117 ms whole-frame IDR (`README` of the lab), where slices would create the
only available parallel lanes.

### constrained-intra and quality cost

Full 300-frame `native1080_300.yuv`, 6000 kbit/s, `-g 50 -threads 12`,
VMAF via `libvmaf` (commands and per-frame logs in `x264_idr/`):

| variant | achieved kbit/s | VMAF | encode fps | frames |
|---|---:|---:|---:|---:|
| `slices=1` | 5763.3 | **97.949** | 46.9 | 300 |
| `slices=34` | 5897.9 | 97.735 | 51.7 | 300 |
| `slices=68` | 5995.8 | 97.601 | 52.2 | 300 |
| `slices=68:constrained-intra=1` | 5995.3 | **97.473** | 48.8 | 300 |

- 1→34 slices: **−0.214 VMAF** at +2.3% bits; 1→68: **−0.348 VMAF** at +4.0%
  bits. The loss is the lost above-row intra prediction plus ~66 extra slice
  67 extra slice headers per frame.
- `constrained-intra=1` at 68 slices: a further **−0.128 VMAF** at equal bits and
  ~6.5% lower encode fps. `ffmpeg -bsf trace_headers` confirms
  `constrained_intra_pred_flag = 1` in the SPS, and 68 `nal_unit_type 5` slice
  NALs with `first_mb_in_slice = 0,120,240,…`. All streams decode clean.
- On the IDR-only test, `constrained-intra=1` changed the stream by 1 byte
  (the SPS flag) and not the payload — every MB is intra, so the restriction is
  a no-op there. The −0.128 VMAF appears at P-frame intra MBs.

`constrained-intra=1` does **not** make a split IDR "more legal": it is a
different restriction (intra prediction may only use intra-coded neighbours).
Both ci=0 and ci=1 68-slice IDRs decode cleanly because x264 already drops
out-of-slice neighbours.

## Ranked conclusion

1. **Keep the paired parallel path; prefer fewer, larger slices.** 17 slices
   (4 rows) is the best measured point: +8–9% process fps and equal-or-better
   VMAF versus 34 slices, with entropy still ~0.25 ms. The marginal gain from
   34→17 is small; going below ~10 slices would starve the GPU of workgroups,
   and 8 rows (9 slices) overflows `maxPairedSliceMbs` (`8×120 = 960 > 512`)
   so that constant and the slot-word sizing would need work first.
2. **Do not adopt 1-row slices as a speed optimisation.** 136 slices are 26–33×
   slower in entropy (−45–53% process fps) because they fall off the parallel
   path, and they cost up to −0.25 VMAF. If 68+ slices are wanted for CABAC
   latency, they must use a *parallel* per-slice entropy design, not
   `entropy.comp`'s inline loop.
3. **IDR splitting is legal but not free.** 68-slice IDRs decode clean, but cost
   −0.35 VMAF at +4% bits (x264) and do not reduce IDR wall time on CPU. Keep
   Wx264's whole-frame IDR unless the GPU prototype's 117 ms serial IDR is the
   binding constraint; if splitting it, expect DC-fallback quality loss at each
   of the 68 slice starts.
4. **Do not use `constrained-intra=1` as the IDR-splitting fix.** It costs an
   extra −0.128 VMAF and ~6% throughput while leaving IDR payload unchanged.
   A row-wavefront with context carry (the other lab workstream) is the
   mechanism that recovers the lost prediction.

## Gates

- ctest in isolated copy: **45/46 passed** before and after the 4-row patch;
  only pre-existing `32 - x264_cavlc_packet_parity` fails ("Unknown kernel test").
- Decode: all six Wx264 streams verified with `ffprobe -count_frames` and
  `--check-recon`; all x264 streams `ffmpeg -err_detect explode` exit 0.
- Artifacts: `Vk264-CABAC-Lab` 109.6 MB total, `results/slice_scale` 99.9 MB,
  isolated copy 68.5 MB — all well under 1 GB.
- No commits or PRs in the parent repo; all edits confined to the isolated copy.
