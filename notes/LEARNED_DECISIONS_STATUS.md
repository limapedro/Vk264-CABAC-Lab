# Learned encoder decisions — status (work deferred by user)

The neural-network decision model is **deferred**. This note records the data
collected before the stop so it can be resumed later.

## Completed

- `experiments/learned/collect_data.py`, `build_features_v2.py`,
  `collect_placebo.py` (scripts retained).
- `results/learned/features.csv` — 2,700 rows (9 clips × 300 frames,
  1920×1080, 6 Mb/s, x264-slow ABR).
- `results/learned/labels_x264_slow.csv` — per-frame QP labels + ffprobe packet
  bits, 300/300 frames per clip. Mean QP range 21.4 (alphago_movie) to 34.7
  (warzone_short).
- Observed correlations: QP vs `logvar` −0.318, QP vs `logsad` −0.330 —
  clip-level content dominates.

## Placebo labels — incomplete

- `collect_placebo.py` ran on `warzone_short` only: encode 98.5 s,
  trace_headers parsed **125/300** QPs (parser needs fixing for multi-slice),
  mean parsed QP 28.07. `placebo_qp.csv` was never written.
- Blocker: ffmpeg's libx264 wrapper has no `qpfile`; `-x264-params zones=…`
  does not force exact per-frame QP, so the per-frame-schedule encode path is
  unresolved.
- Web research on learned encoder decisions was gathered but not written up.

## Why deferred

The user chose to push the current codebase first and revisit learned models
later. See `LEARNED_MODEL_DESIGN.md` for the planned 9.4k-param host-side model
and deployment path if this is resumed.
