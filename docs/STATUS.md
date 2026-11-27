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
  **Evaluation:** N=16 only marginally reaches the 100 fps gate, needs
  ~3.8 GiB duplicated scratch + DPB growth, AND a working CABAC encoder
  (which itself was measured at ~23 fps). Treat as a long-term architecture
  option, not the next step.
- notes/CPU_CABAC_COST.md: CPU CABAC ~0.7 ms/frame in x264-slow, no
  matched-rate VMAF gain on lab content.
- notes/SPECULATIVE_HYBRID.md + results/speculative_conflicts.json: naive
  checkpoint speculation DEAD (98.4% chunk conflicts); carry interface is
  only 1 bit; exact chunk-summary + serial context prefix estimated <2 ms vs
  117 ms measured IDR. **Evaluation: this is the most promising lab result —
  CPU reference prototype launched (`ses_de2ab9d72ffeYls67vGOkgB590`) to
  prove byte-exactness before any Vulkan work.**
- notes/WPP_H264.md: WPP H264 DEAD END. Contexts reset per slice;
  measured baseline 94.478 -> 94.039 VMAF with slices=68:constrained-intra=1;
  all-intra isolates +5.6-7.5% bitrate cost. Row slices are legal but cost
  ~6-8% bits and still couple through deblocking.
- notes/SPECULATIVE_HYBRID.md evaluated: exact chunk-summary design promoted
  to CPU reference prototype (agent 5, files under experiments/chunk_cabac/).
- results/slice_scale/: in progress (agent 3).

Parent-repo ABR fix agent (`ses_de2d0752affe5g5VSsbM249e0J`)
also still running; its result is needed before judging quality claims at
target bitrate.
