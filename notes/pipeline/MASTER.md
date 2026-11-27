# Master synthesis — H.264 pipeline stage study

Status: all five stage studies complete (2026-11-27). Parent Vk264 at
`47c21e6`, clean. Lab notes: `01_PREDICTION.md`, `02_RD_TRANSFORM_QUANT.md`,
`03_ENTROPY.md`, `04_FILTERING_POSTPROCESS.md`, `05_RATE_CONTROL.md`.

## 1. Stage-by-stage verdict

| Stage | What Vk264 already does | Literature verdict for further gain |
|---|---|---|
| Prediction | pyramid ME, quarter-pel, 4 SATD finalists, 5 refs, 6-tap halfpel, I16 directional intra, 4-row P slices | **Nothing conforming clears +0.3 VMAF in ≤2 ms.** TZS/BZS save 60-68% ME time at +0.08-0.30% BD-rate (speed only, already near that). Learned interpolation/DMVR are VVC and non-conformant here. Weighted prediction = 0 gain on our corpora. |
| RD / transform / quant | SSE RD, heuristic `3+2·MSB` bit cost, inter AC divisor 3, no trellis, no 8×8dct | Only untried conforming idea with upside: **VIF-proxy per-block λ** (+0.2-0.5 VMAF, 0.1-0.4 ms, mean-normalised so ABR cannot re-spend). Trellis stack (+0.2-0.4) was prototyped and blocked (PR #528 note). 8×8dct +0.15 needs full syntax path. |
| Entropy | parallel CAVLC, 0.08-0.1 ms/frame, per-MB bodies + chunked EPB | **Quality ~3% bitrate only** (81-measurement CABAC sweep). No conforming parallel-CABAC breakthrough in 2015-2026 literature. Remaining value: exact bit-cost feedback to RD (+0.13-0.26 VMAF, <0.05 ms) and throughput (chunk-summary design, but CABAC excluded by user). |
| Filtering / post | full-frame deblock wavefront, standard α/β/BS tables | Vk264 already applies every normative H.264 filter. Conformant filter gains are tiny (α/β ≤0.05 VMAF; SEI-22 display bilateral +0.1-0.5 but not in reconstructed frames). Closing 1.84 VMAF would need ~55% BD-rate — no filter does that. |
| Rate control | lagged debt integrator, ±allocation/4 clamp, QP floor 18, 5 s GOP | **Dominant failure.** x264 uses SATD-EWMA + `qcomp=0.6`, MB-tree, 40-60 frame lookahead. Offline plan-then-encode (we preload all frames) is the strongest lead: cited -17% to -32% bitrate; estimated +2.9 VMAF game / +8.1 trailer; prototype running. |

## 2. Ranked breakthrough candidates (no CABAC)

| # | Candidate | Expected gain | GPU/host cost | Conformance | Evidence / status |
|---:|---|---:|---:|---|---|
| 1 | **Offline probe-calibrated per-frame QP plan** (host: features → bits_i(q) fit → water-fill; push `plannedQp`) | +2.9 game / +8.1 trailer estimated; fixes 21→6 Mb/s | host 0.1-3.4 s per 300f clip; loop unchanged 132-140 fps | fully conformant (QP only) | literature: Netflix DO -17.1%, JTPS -18.8%/-32.59% BD-rate; prototype in progress |
| 2 | **VIF-proxy distortion-propagation λ** (per-block λ from activity/SATD/ref-reuse, mean-normalised to 1, no QP change) | +0.2-0.5 VMAF @6000 | 0.1-0.4 ms | conformant | RD study; avoids ABR re-spending the gain (unlike AQ -1.28) |
| 3 | **Exact CAVLC bit-cost feedback to RD** (bit lengths from existing tables into group-keep/skip decision) | +0.13-0.26 VMAF | <0.05 ms | conformant | entropy study; 0.2-8% bitrate saving in literature; our full exact-RD prototype gained +0.344 but cost +0.9 ms and was reverted — a cheaper bit-count-only variant is untried |
| 4 | **Conformant deblock α/β offset tuning** | ≤0.05 VMAF | 0 ms | conformant | filtering study; slice-level offsets currently ignored |
| 5 | **Display-only bilateral/detail pass via SEI 22** | +0.1-0.5 VMAF (display only) | 0.5-1 ms | bitstream-conformant, decoder-optional | filtering study; must gate `sharp=0` to avoid VMAF-v0 VIF hack |
| 6 | **Pyramid-confidence adaptive search range** | speed only (0.5-1.2 ms saved) | -0.5 to -1.2 ms | conformant | prediction study |
| 7 | Chunk-summary exact CABAC | +0.35 VMAF @8000 | 2.9-5.9 ms prefix | conformant | **excluded by user decision** |
| 8 | B-frames + lookahead + MB-tree | +0.41-0.86 VMAF | new ABI | conformant but needs dual-ref DPB | blocked (XL) |

## 3. Honest ceiling

Stacking the conforming, ≤2 ms items (#2+#3+#4) gives roughly **+0.4 to +0.9
VMAF** at matched rate. Item #1 (offline QP plan) can recover the ABR-induced
loss on difficult content (+2.9 to +8.1 estimated) but cannot exceed the
fixed-QP codec ceiling. On the user's OBS content that ceiling is
**VMAF 92.1 @6000 / 93.7 @8000** (fixed QP30/28), versus x264-slow 93.6/94.8.
So even a perfect controller leaves ~1.4-1.8 VMAF that only structural tools
(B-frames/CABAC/RDO with lookahead) can close.

## 4. Measurement caveat

Our harness uses ffmpeg's default libvmaf and the JSON contains
`integer_vif_scale0` → **VMAF v0**. Netflix VMAF v1 (libvmaf 3.2.0, June 2026)
removed VIF as a core feature and enables NEG by default; re-scoring the same
streams can move numbers 1-3 points. Unsharp masking can raise VMAF v0 by +34%
while subjective preference falls (original preferred 58.3%). All quality
claims here are VMAF v0; the lab recommends re-scoring finalists with v1/NEG
and a source-identity control before declaring a breakthrough genuine.
