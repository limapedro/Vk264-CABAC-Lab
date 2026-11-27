# Status

## 2026-11-27 — Lab created

- Parent repo `E:\__PROJECTS__\Vk264` at HEAD `811bad4`, clean.
- Known baseline from parent project:
  - GPU CAVLC loop 150–160 fps on 1080p60 capture content, process ~100 fps.
  - Slice-parallel CABAC prototype: ~23 fps, 14 µs/MB, 117 ms/whole-frame IDR.
  - x264-slow on 12-core i5-12400F: 75–105 fps full encode at 1080p60.
- Research agents LAUNCHED 2026-11-27:
  1. `ses_de2b4ea0effelwdNSVyxpVwyWP` — frame-parallel CABAC architecture +
     minimum DPB design → notes/FRAME_PARALLEL.md
  2. `ses_de2b4c5edffeV7Ph8HjDAMfKyQ` — WPP / row-wavefront H.264 legality
     and measured quality cost → notes/WPP_H264.md
  3. `ses_de2b4a168ffeINCaWiG28l3jWB` — slice-count scaling measurements
     (1/2/4 rows per slice, IDR split legality) → notes/SLICE_SCALING.md
  4. `ses_de2b47dc8ffeSPozCC0rkmoHSD` — speculative checkpoint/rollback and
     CPU-GPU hybrid feasibility → notes/SPECULATIVE_HYBRID.md

Results in:
- notes/FRAME_PARALLEL.md + results/frame_parallel_estimates.json: CABAC state
  is per-slice; frame-parallelism capacity capped by recon slots (6/17);
  N=8 -> 60-68 fps, N=16 -> 105-120 fps with ~3.8 GiB duplicated scratch;
  rate feedback lag N frames makes serial bitstream identity impossible.
- notes/CPU_CABAC_COST.md: CPU CABAC ~0.7 ms/frame in x264-slow, no
  matched-rate VMAF gain on lab content.
- results/slice_scale/: in progress (agent 3).

Parent-repo ABR fix agent (`ses_de2d0752affe5g5VSsbM249e0J`)
also still running; its result is needed before judging quality claims at
target bitrate.
