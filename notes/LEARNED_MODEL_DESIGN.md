# Design: ~10k-parameter decision model for Vk264

## Architecture

Tiny MLP, int8-quantisable, ~9.4k parameters:

```
input  32 features
hidden 96 units   (32*96 + 96  = 3,168)
hidden 64 units   (96*64 + 64  = 6,208)
output 1 (QP)     (64 + 1      =    65)
                               total 9,441
```

~9 KB of int8 weights. Host inference costs ~10k MACs/frame (<0.01 ms),
invisible to the 100 fps GPU loop.

## Features (per frame, computed from preloaded source)

- 16×16-downsampled SAD vs previous frame (the signal our per-frame ABR
  mishandles on 30→60 duplicated capture)
- SAD vs future frames i+1 and i+2 — available because the CLI preloads all
  frames before the encode loop (`src/main.cpp:191-195`); this mimics x264
  lookahead at zero GPU cost
- luma variance, luma edge energy, chroma variance
- scene-cut flag, target bits/frame, GOP position, previous QP, bitDebt

## Labels

x264 **placebo** at matched bitrate; per-frame QP extracted from the bitstream:
`ffmpeg -bsf:v trace_headers` → `pic_init_qp_minus26` + `slice_qp_delta`.

## Two training targets

1. **Direct QP imitation**: `features -> QP_placebo`. Simple, but placebo's
   QP choices assume B-frames/CABAC/MB-tree/16 refs, which Vk264 lacks, so
   transfer may be poor.
2. **Learned rate model**: `bits = f(features, QP)` trained on placebo data,
   then water-fill QP allocation to hit our own target. More robust; closer
   to x264's qcomp/MB-tree logic without inheriting its tool assumptions.

Leave-one-clip-out validation across the 11 available masters. Report rate
error and estimated VMAF versus our current ABR.

## Deployment

- Host computes the QP schedule once at init (or per frame) and pushes a
  `plannedQp` field in `FrameConfig` (one uint32).
- `shaders/scan.comp` prefers `plannedQp` while keeping the accepted
  ±allocation/4 debt clamp as a safety net.
- **No bitstream/decoder change**: the model only chooses a legal QP.

## Non-goal

Per-MB skip/CBP/MV prediction with a shader MLP: the hand-tuned decisions are
only a few integer ops, so inference would cost more than it saves. The
per-frame host-side path is the worthwhile one.
