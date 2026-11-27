# Can a WPP-style row wavefront make H.264 CABAC parallel?

**Date:** 2026-11-27. Research note for the Vk264 CABAC/Parallelism Lab.
All web content below was treated as untrusted data; claims are tied to a source
line, a command output, or a URL. No files in `E:\__PROJECTS__\Vk264` were modified
(`git status --short` clean before and after). Temp artifacts live in
`C:\Users\multi\AppData\Local\Temp\opencode\wpp\` (377.2 MB total, < 500 MB).

## TL;DR / ranked verdict

1. **True WPP for H.264 CABAC = dead end for conforming bitstreams.** H.264 has no
   dependent-slice / CABAC-context-carry syntax. Every slice boundary must reset
   the arithmetic engine and all 460 context states, and intra/MV prediction may
   not cross a slice boundary. Carrying contexts would require a custom bitstream
   **and** a non-standard decoder.
2. **Row slices are legal but not free, and not really WPP.** With
   `slices=68:constrained-intra=1` at 1080p60/6000 kb/s, whole-sequence VMAF falls
   **94.478 → 94.039 (−0.439)**; the IDR frame itself falls **96.638 → 96.504
   (−0.134)**. At fixed QP (isolating rate control) the same split costs **+5.5 %
   to +7.5 % bitrate at equal VMAF**.
3. **`constrained_intra_pred_flag=1` is not the culprit for an IDR.** On an IDR
   every MB is intra, so the flag is redundant: `slices=68` alone gives
   **94.064** VMAF vs **94.039** with the flag (Δ 0.025). The loss comes from the
   *slice boundary itself* (no top neighbour for row 1..67), 68 CABAC resets, and
   slice-header overhead.
4. **A standard-compatible wavefront does exist in hardware, but not for CABAC:**
   Xu et al. (ICMEW 2025) built a dual-core **Baseline-profile (CAVLC)** H.264
   encoder with MB-line wavefronts: 125 fps at 1080p, up to **49.4 % latency
   reduction**, no RD loss, standard H.264 decoder compatible. It works by
   scheduling cores and moving dependencies through on-chip channels while
   emitting one ordinary bitstream — not by carrying entropy state across rows.
5. **The realistic H.264 paths** are (a) pipeline CABAC internally (Cisco
   US8422552B2 style, bitstream-identical) or (b) frame-level parallelism with a
   DPB, not row-wave CABAC.

---

## 1. H.264 slice semantics (x264 source + spec concepts)

All line numbers below are from the vendored tree
`E:\__PROJECTS__\Vk264\x264-vulkan\`, which is the x264 fork the parent project
ships (UPSTREAM-REVISION present).

### 1.1 Intra prediction stops at every slice boundary

`common/macroblock.c:673-846` (`macroblock_cache_load_neighbours`) builds the
neighbour-availability masks:

- `common/macroblock.c:689` resets `h->mb.i_neighbour_intra = 0` for each MB.
- `common/macroblock.c:778-785` (left), `:797-802` (top), `:822-827` (top-left),
  `:837-842` (top-right) each require
  `h->mb.slice_table[neighbour] == h->sh.i_first_mb` **before** setting
  `i_neighbour`/`i_neighbour_intra`. A neighbour in another slice is simply not
  available, *regardless of* `constrained_intra_pred_flag`.
- The constrained-intra check is an **additional** filter:
  `if (!h->param.b_constrained_intra || IS_INTRA(neighbour_mb_type))`
  at `:783`, `:801`, `:826`, `:841`. So `i_neighbour_intra` means
  "same slice AND (unconstrained OR intra-coded neighbour)".
- `common/macroblock.c:1734-1737` propagates the same rule into cached
  `intra4x4_pred_mode` for subsequent MBs.
- `encoder/analyse.c:615,692,1025,1042` derive the allowed intra mode sets from
  `h->mb.i_neighbour_intra` (`predict_16x16_mode_available`,
  `predict_chroma_mode_available`), so the encoder cannot even *choose* a mode
  that needs a cross-slice sample.

Spec concept (confirmed by Iain Richardson / Vcodex):
"in order to preserve independent decoding of slices, only samples within the
current slice are available for prediction"; the `most_probable_mode` shortcut
also requires A and B to be "both within the current slice".
<https://www.vcodex.com/h264avc-intra-prediction> (accessed 2026-11-27).

**Consequence for one-row slices:** every MB in rows 1..67 loses its top,
top-left and top-right predictors. For an IDR that is the dominant quality loss;
`constrained_intra_pred_flag=1` changes nothing because all MBs in the frame are
already intra.

The flag itself is PPS-level: `x264.h:380` (`b_constrained_intra`), parsed at
`common/base.c:1166`, default 0 at `common/base.c:393`, written into the PPS at
`encoder/set.c:499` and `:525` (`pps->b_constrained_intra_pred`,
`bs_write1(s, pps->b_constrained_intra_pred)`).

### 1.2 CABAC state is reset at every slice

- `encoder/encoder.c:2778-2801`: for each slice the encoder writes a new NAL and
  slice header (`:2791`), aligns the bitstream (`:2795`), then calls
  `x264_cabac_context_init(...)` and `x264_cabac_encode_init(...)` (`:2798-2799`).
- `common/cabac.c:30-45` precomputes the context tables; `:48-51`
  `x264_cabac_context_init()` `memcpy`s 460 context states (1024 for 4:4:4),
  selected by slice type, QP and `cabac_init_idc`, into `cb->state`.
- `common/cabac.c:53-67` `x264_cabac_encode_init_core()` resets `i_low = 0`,
  `i_range = 0x01FE`, `i_queue = -9`, `i_bytes_outstanding = 0`.
- `common/cabac.c:69-99` (`cabac_putbyte`) shows why the arithmetic state is not
  trivially splittable: a late carry can modify `cb->p[-1]`, i.e. a byte already
  emitted; the comment at `:85-86` notes this is safe only because a slice header
  always precedes CABAC data.
- H.264 spec concept: CABAC context models are "re-initialized at the slice
  boundaries using some pre-defined probability states" (Marpe, Schwarz,
  Wiegand, "Context-Based Adaptive Binary Arithmetic Coding in the H.264/AVC
  Video Compression Standard", IEEE TCSVT 2003,
  <https://iphome.hhi.de/marpe/download/cabac_ieee03.pdf>). There is no H.264
  syntax bit that says "continue the previous slice's contexts".

### 1.3 Deblocking across slice edges

- `encoder/encoder.c:203-208`: `sh->i_disable_deblocking_filter_idc =
  param->b_sliced_threads ? 2 : 0` (or 1 if filtering is off). For CLI
  `slices=68` (not sliced threads) this is **0**, i.e. the in-loop filter *does*
  run across slice boundaries.
- `common/deblock.c:340-376`: `deblock_on_slice_edges = idc != 2`; MB_LEFT / MB_TOP
  are enabled across slice edges when that is true.
- Consequence: our one-row slice bitstreams decode in standard decoders, but the
  deblocking stage still couples neighbouring slices. Fully independent row
  decode would additionally require `idc=2`, i.e. another quality change.

### 1.4 How `slices=68` becomes one MB row

`encoder/encoder.c:3193-3199`: with `i_slice_count` set and `b_sliced_threads`
false, `i_last_mb = (height*i_slice_num + round_bias)/i_slice_count*width - 1`.
At 1080p, height = 68 MB rows, width = 120 MBs, so 68 slices = 120 MBs each.
Verified on the smoke bitstream with `C:\Users\multi\AppData\Local\Temp\opencode\wpp\parse_slices.py`:
68 IDR slice NALs with `first_mb = 0, 120, 240, …, 8040`.

### 1.5 Why H.264 has no WPP but HEVC/VVC/AV1 do

- Fraunhofer HHI: "Unlike H.264/AVC, where parallelism was an afterthought, the
  current HEVC draft contains several proposals aiming at making the codec more
  'parallelizable' … using many slices to increase parallelism incurs significant
  coding losses." <https://www.hhi.fraunhofer.de/en/departments/vca/research-groups/multimedia-communications/research-topics/past-research-topics/wavefronts-for-hevc-parallelism.html>
  (accessed 2026-11-27).
- HHI, "Tools for Parallel Processing and Ultra-Low Delay Support": WPP runs one
  CTU row per thread with a two-CTU lag, and "the content of the partially
  adapted CABAC context variables are propagated from the encoded/decoded second
  CTU of the preceding CTU row to the first CTU of the current CTU row"; WPP
  partitions are carried as **dependent slice segments** with entry-point
  offsets. References: Henry & Pateux, JCT-VC E196 (March 2011); Schierl et al.,
  JCT-VC I0229 (April 2012). <https://www.hhi.fraunhofer.de/en/departments/vca/research-groups/video-coding-technologies/research-topics/past-research-topics/tools-for-parallel-processing-and-ultra-low-delay-support.html>
  (accessed 2026-11-27).
- H.264 has neither dependent slices nor entry-point offsets nor a signaled
  context carry; AV1 instead uses tile groups with entry-point offsets. A
  "WPP-style H.264" can therefore only mean *row slices with full resets* (a
  quality hit) or an *implementation-internal pipeline that still emits one
  ordinary slice*.

---

## 2. Published attempts / prior art (exact results)

| Work | Date / URL | Exact reported result | Standard-decoder compatible? | CABAC? |
|---|---|---|---|---|
| Xu, Liu, Chen, Wang, Liang, "Low-Latency H.264/AVC Encoder Hardware Design with Wavefront Parallelism on FPGA Platform", ICMEW 2025, DOI `10.1109/ICMEW68306.2025.11152256` | published 2025-06-30; abstract via Semantic Scholar API <https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/icmew68306.2025.11152256> | dual-core MB-line wavefront; core scheduling + a pair of SISO on-chip data channels maintain adjacent-MB-line dependencies; **125 fps at 1080p**; **up to 49.4 % lower overall encoding latency** vs MB-serial; "without the loss of H.264 standard compatibility and rate-distortion performance" | **Yes** — outputs a standard H.264 Baseline bitstream | **No** — Baseline profile, so CAVLC; no cross-row CABAC state |
| Cisco Technology, US8422552B2 "Entropy coding for video processing applications" (inventors Au, Farley, Heinrich) | priority 2007-03-29, granted 2013-04-16; <https://patents.google.com/patent/US8422552B2/en> | splits CABAC `encode-decision`, `renormalization` and `PutBit` into pipelined processes with FIFOs and a precomputed `S` parameter to remove the L/R feedback loop; "H.264 compliant", intended to allow **single-slice** HD encoding and faster rate-control feedback. **No quantitative speedup or BD-rate number is published in the patent.** | Yes (bitstream-equivalent) | Yes — pipelining *inside* one CABAC engine, not across rows |
| Chi, Alvarez-Mesa, Juurlink et al., "Parallel Scalability and Efficiency of HEVC Parallelization Approaches", IEEE TCSVT 2012 (via HHI + Fora Soft summary) | 2012; <https://www.hhi.fraunhofer.de/en/departments/vca/research-groups/multimedia-communications/research-topics/past-research-topics/wavefronts-for-hevc-parallelism.html>, <https://www.forasoft.com/learn/video-encoding/articles/slices-tiles-parallel-processing> (2026-10-08) | 12-core 3.33 GHz, 2160p: mean speedups **8.7× WPP**, **9.3× tiles**, **10.7× OWF**; typical HEVC WPP coding cost ≈1 % bitrate | Yes for HEVC; not applicable to H.264 | Yes, with standardized context propagation |
| General H.264 slices as parallelism (industry summary, secondary source) | Fora Soft, 2026-10-08, <https://www.forasoft.com/learn/video-encoding/articles/slices-tiles-parallel-processing> | claims "1–3 % compression efficiency" lost across slice boundaries per 32 slices and "30–80 bits" of slice-header overhead; our own measurement below is consistent in sign but gives a smaller number for 68 *row* slices | Yes | Yes, with a reset per slice |
| UPC 2-D wave H.264 decoder study | <https://upcommons.upc.edu/bitstreams/09969d77-4770-4885-b1e3-4e26c55fb830/download> | CABAC decoding is decoupled from MB decoding and remains serial; "according to Amdahl's law it can become the limiting factor", so MB-level parallelism does not scale without a much faster CABAC path | Yes (decoder-side scheduling) | Shows the serial-CABAC bottleneck |

**Finding:** no published implementation was found that carries H.264 CABAC
contexts across row boundaries in a standard-decodable bitstream. The one true
standard-compatible H.264 wavefront is Baseline/CAVLC hardware; the CABAC
parallelism patents/papers pipeline a single entropy engine or stay decoder-side.

---

## 3. Measurement: 68 one-row slices at 6000 kb/s, 1080p60

### 3.1 Setup

No standalone `x264.exe` is installed; `ffmpeg`'s `libx264` is the same encoder
and accepts the requested `-x264-params` verbatim.

```
ffmpeg version N-126335-gb32f8d1c23-20260830
  --enable-gpl --enable-libx264 --enable-libvmaf ...   (C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe)
source: E:\__PROJECTS__\Vk264\build\obs_new_src.yuv, 300 f, 1920x1080, 60 fps, yuv420p
encode: -f rawvideo -pixel_format yuv420p -video_size 1920x1080 -framerate 60 \
        -i obs_new_src.yuv -frames:v 300 -c:v libx264 -preset slow -b:v 6000k \
        -pass 1/2 -passlogfile pass_<name> [-x264-params "..."] -an out.h264
vmaf:   ffmpeg -i out.h264 -f rawvideo -pixel_format yuv420p \
        -video_size 1920x1080 -framerate 60 -i obs_new_src.yuv \
        -filter_complex "[0:v]setpts=PTS-STARTPTS[d];[1:v]setpts=PTS-STARTPTS[r];[d][r]libvmaf=log_fmt=json:log_path=...:n_threads=12:shortest=1" \
        -f null NUL          (default model vmaf_v0.6.1, 300 frames scored)
```

Scripts/logs/JSON: `encode_matrix.ps1`, `vmaf_matrix.ps1`, `qp_matrix.ps1`,
`summarize.py`, `parse_slices.py`, `vmaf_*.json` in the temp directory.
ffmpeg reported 0 decode errors on the sliced bitstreams; standard decoder
accepted all of them.

### 3.2 Results — normal GOP (keyint 250, two IDRs in 300 frames)

| variant | achieved bitrate | avg QP (I / P / B) | VMAF (pooled) | VMAF frame 0 (IDR) |
|---|---:|---:|---:|---:|
| baseline (1 slice) | 6557 kb/s | 16.08 / 18.82 / 22.89 | **94.4784** | **96.6380** |
| `slices=68:constrained-intra=1` | 6406 kb/s | 17.00 / 19.58 / 24.64 | **94.0395** (Δ −0.439) | **96.5038** (Δ −0.134) |
| `slices=68` only | 6394 kb/s | 16.99 / 19.57 / 24.63 | **94.0641** (Δ −0.414) | **96.5030** |

- The ABR actually gave the sliced version **2.3 % fewer bits**, so the −0.44 VMAF
  is a lower bound on the boundary cost, not an upper bound.
- `constrained_intra_pred_flag=1` costs only **0.025 VMAF** relative to
  `slices=68` alone, confirming the flag is redundant on an all-intra IDR.
- The per-frame VMAF cost is paid on *every* frame that is split (P/B frames too),
  not just the two IDRs: first-10-frame means 96.665 → 96.509 (−0.156).

### 3.3 Results — every frame an IDR (`keyint=1`), 6000 kb/s

| variant | achieved bitrate | avg QP | VMAF (pooled) | VMAF frame 0 | PSNR Y/U/V avg |
|---|---:|---:|---:|---:|---:|
| baseline all-intra | 6163 kb/s | 44.55 | **26.4217** | **25.8649** | 27.956 dB |
| `slices=68:constrained-intra=1` all-intra | 5677 kb/s | 46.79 | **11.2107** (Δ −15.21) | **5.9673** (Δ −19.90) | 26.902 dB |

At this extreme operating point (1080p60 all-intra squeezed into 6 Mb/s, QP≈45)
the row split makes rate control push QP 2.24 steps higher and VMAF collapses
~15 points (PSNR only ~1 dB lower — VMAF's detail-loss term is very steep here).
This is a rate-control cliff, not purely a prediction loss.

### 3.4 Fixed-QP all-intra (isolates coding efficiency from rate control)

| QP | baseline bitrate / VMAF | 68 slices + CI bitrate / VMAF | bitrate cost at matched QP |
|---:|---:|---:|---:|
| 22 | 126.0 Mb/s / 96.4374 | 133.0 Mb/s / 96.4484 | **+5.6 %** |
| 26 | 91.7 Mb/s / 95.3621 | 97.6 Mb/s / 95.4050 | **+6.4 %** |
| 30 | 66.0 Mb/s / 93.2524 | 71.0 Mb/s / 93.3113 | **+7.5 %** |

So the intrinsic cost of 68 one-row slices is roughly **+6–8 % bitrate at equal
VMAF** (slightly *higher* VMAF at fixed QP, because the extra bits are spent).
The 15-point VMAF collapse in §3.3 is what happens when the bitrate is pinned at
a value where the encoder cannot afford that overhead.

---

## 4. Can CABAC contexts be carried across row-wave slices legally?

**No — dead end.**

- At the start of *every* slice, H.264 (and x264 at `encoder/encoder.c:2791-2801`)
  writes a slice header and reinitializes both the arithmetic engine and the 460
  context states from a QP/`cabac_init_idc` table (`common/cabac.c:30-67`). There
  is no syntax element in H.264 that means "inherit the previous slice's context
  states" — unlike HEVC's dependent slice segments and WPP entry points (§1.5).
- Carrying `low`/`range`/`queue`/`outstanding` and `state[]` from row N-1 into row
  N would require both a modified encoder *and* a modified decoder; a standard
  H.264 decoder would initialize from the table and immediately desynchronize.
- Even if the entropy hack were accepted, `macroblock_cache_load_neighbours`
  (`common/macroblock.c:778-843`) blocks intra and MV prediction across the same
  boundary. True WPP semantics need *both* context carry *and* cross-row
  prediction; H.264 provides neither.
- Therefore any "H.264 WPP with context carry" is a proprietary bitstream, not an
  H.264 bitstream. It cannot be a drop-in path for Vk264, whose output must play
  in OBS/players using standard H.264 decoders.

## 5. Ranked verdict

1. **Standard-conforming and quality-neutral:** pipeline the CABAC engine
   internally (encode-decision / renormalization / PutBit as in Cisco
   US8422552B2) and/or add frame-level parallelism with a DPB. This preserves
   single-slice quality and standard decoders.
2. **Standard-conforming but costly:** use row slices with full resets. Measured
   cost: −0.44 VMAF at 6000 kb/s normal GOP, ~+6–8 % bits at matched QP. This is
   *slice parallelism*, not WPP, and deblocking across slice edges
   (`encoder.c:206`) still couples the slices.
3. **Proprietary / non-conforming:** carry CABAC contexts across row waves.
   Requires a custom bitstream and decoder; additionally blocked by the
   cross-slice prediction ban. **Dead end for a conforming encoder.**
4. **Operationally dangerous:** 1-row slices on all-intra at very low bitrate —
   measured VMAF 26.4 → 11.2 at 6 Mb/s. Avoid.

**Bottom line:** WPP-style row wavefronts cannot make H.264 CABAC parallel in a
standard bitstream. The only conforming row-wave result in the literature is
Baseline/CAVLC hardware with ~49 % latency reduction and no RD loss; for CABAC,
the achievable parallelism is inside the entropy engine or across frames, and
splitting an IDR into 68 one-row slices buys boundary parallelism at ~6–8 %
bitrate (≈0.13–0.44 VMAF at 6 Mb/s, much more at extreme QP).

## References (all accessed 2026-11-27 unless noted)

- x264 fork source: `E:\__PROJECTS__\Vk264\x264-vulkan\{common\macroblock.c,common\cabac.c,common\deblock.c,encoder\encoder.c,encoder\set.c,x264.h,common\base.c}`.
- Iain Richardson, Vcodex, "H.264/AVC Intra Prediction" — <https://www.vcodex.com/h264avc-intra-prediction>
- Fraunhofer HHI, "Wavefronts for HEVC Parallelism" — <https://www.hhi.fraunhofer.de/en/departments/vca/research-groups/multimedia-communications/research-topics/past-research-topics/wavefronts-for-hevc-parallelism.html>
- Fraunhofer HHI, "Tools for Parallel Processing and Ultra-Low Delay Support" — <https://www.hhi.fraunhofer.de/en/departments/vca/research-groups/video-coding-technologies/research-topics/past-research-topics/tools-for-parallel-processing-and-ultra-low-delay-support.html>
- Marpe, Schwarz, Wiegand, IEEE TCSVT 2003, CABAC overview — <https://iphome.hhi.de/marpe/download/cabac_ieee03.pdf>
- Xu et al., ICMEW 2025, DOI 10.1109/ICMEW68306.2025.11152256 — <https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/icmew68306.2025.11152256>
- Cisco, US8422552B2 — <https://patents.google.com/patent/US8422552B2/en>
- Fora Soft, "Slices, Tiles and Parallel Processing", 2026-10-08 — <https://www.forasoft.com/learn/video-encoding/articles/slices-tiles-parallel-processing>
- UPC, 2-D wave H.264 decoding study — <https://upcommons.upc.edu/bitstreams/09969d77-4770-4885-b1e3-4e26c55fb830/download>
