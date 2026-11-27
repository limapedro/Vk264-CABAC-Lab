# Status

## 2026-11-27 — Lab created

- Parent repo `E:\__PROJECTS__\Vk264` at HEAD `811bad4`, clean.
- Known baseline from parent project:
  - GPU CAVLC loop 150–160 fps on 1080p60 capture content, process ~100 fps.
  - Slice-parallel CABAC prototype: ~23 fps, 14 µs/MB, 117 ms/whole-frame IDR.
  - x264-slow on 12-core i5-12400F: 75–105 fps full encode at 1080p60.
- Research agents to be launched:
  1. Frame-parallel CABAC architecture + minimum DPB design.
  2. WPP / row-wavefront H.264 legality and quality cost.
  3. Slice-count scaling measurements (34 → 68 P slices, IDR split options).
  4. Speculative checkpoint / CPU-GPU hybrid feasibility.

No results yet.
