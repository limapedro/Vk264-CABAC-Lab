# Master synthesis — H.264 pipeline stage study (in progress)

Five parallel studies are running, one per encoder stage:

| File | Stage | Status |
|---|---|---|
| `01_PREDICTION.md` | ME, subpel, refs, weighted pred, B-frames, lookahead, intra | **DONE** — nothing clears gate; best are speed-only pyramid-confidence range (0.5-1.2 ms), intra RD-lite (<=+0.03 VMAF), weighted pred (0 on current corpora) |
| `02_RD_TRANSFORM_QUANT.md` | mode RD, lambda, trellis/QNS, quantizer, 8x8 DCT | **DONE** — top ideas: VIF-proxy distortion λ (+0.2-0.5, 0.1-0.4 ms, no QP change so ABR cannot re-spend), subband QNS (+0.2-0.4, already prototyped and blocked), Cauchy bit estimator (+0.05-0.2). Realistic stack +0.4-0.9 VMAF, not enough alone |
| `03_ENTROPY.md` | CAVLC/CABAC, bins, carry, EPB, parallel entropy | running |
| `04_FILTERING_POSTPROCESS.md` | deblock, SAO-like, neural post-filter, VMAF vs genuine quality | running |
| `05_RATE_CONTROL.md` | ABR, qcomp, MB-tree, lookahead, offline QP planning | **DONE** — offline probe-calibrated QP plan ranked #1; cited −17 % to −32 % bitrate savings; estimated +2.9 VMAF game / +8.1 trailer, ~132-140 fps unchanged |

This file will be replaced with the ranked cross-stage synthesis when all
five report. Ground rules: every candidate needs (a) measured VMAF or BD-rate
from literature, (b) GPU cost estimate against our ~1-2 ms headroom,
(c) H.264 conformance status, (d) explicit exclusion of anything already
tried and reverted in `E:\__PROJECTS__\Vk264\docs\quality_baseline.md`.

## Known constraints for the synthesis

- No CABAC by user decision; CAVLC entropy is 0.08-0.1 ms/frame (solved).
- GTX 1660 SUPER: 1080p loop ~137 fps, process ~100 fps; GPU stage sum ~6.5 ms.
- Measured codec gap at matched rate: ~1.7-2.0x bitrate vs x264; VIF scale0
  deficit (0.52 vs 0.68) while ADM2 nearly equal.
- Biggest x264 tool ablations (veryslow): mode-RD/subme -0.86 VMAF,
  trellis -0.65, CABAC -0.55, B-frames -0.41, 8x8dct -0.21, MB-tree -0.17.
- CABAC measured across 81 encodes: +1.74 VMAF @2000, +0.60 @6000, +0.35 @8000,
  ~3% bitrate saving — not the breakthrough.
- ABR is structurally broken on 30->60 duplicated capture; 13 variants failed.
