# CABAC vs CAVLC: 81-measurement sweep across videos, presets and bitrates

Script: `experiments/cabac_sweep.py`. Raw data: `results/cabac_sweep.json`.
Command shape per point:

```
ffmpeg -f rawvideo -pixel_format yuv420p -video_size 1920x1080 \
  -framerate <fps> -i <master.yuv> -frames:v 120 -an \
  -c:v libx264 -preset <preset> -sc_threshold 0 -g 120 \
  -b:v <rate>k -maxrate <rate>k -bufsize <2*rate>k \
  -x264-params cabac=<0|1> -f h264 out.h264
```

9 masters (warzone_short, fortnite4k, yt_forza, yt_sport, porsche_sport,
demons_souls, alphago_movie, obs_new, native25) × presets
{medium, slow, veryslow} × rates {2000, 6000, 8000} = **81 paired A/B
measurements** (CABAC on vs off, same preset/rate/source). VMAF via libvmaf
v0.6.1, 120 frames each.

## Aggregate result

| Rate | n | mean ΔVMAF (CABAC−CAVLC) | median ΔVMAF | mean bitrate change |
|---:|---:|---:|---:|---:|
| 2000 kb/s | 27 | **+1.738** | +1.591 | **−3.30 %** |
| 6000 kb/s | 27 | **+0.603** | +0.480 | **−3.04 %** |
| 8000 kb/s | 27 | **+0.350** | +0.225 | **−3.18 %** |

By preset (all rates): medium +0.853 VMAF / −2.79 % bits; slow +0.944 /
−3.47 %; veryslow +0.894 / −3.26 %.

## Content dependence (at 6000 kb/s)

| Clip | ΔVMAF range across presets |
|---|---:|
| demons_souls (dark detailed game) | +2.22 … +2.42 |
| yt_sport | +0.52 … +0.82 |
| yt_forza | +0.73 … +0.77 |
| porsche_sport | +0.33 … +0.72 |
| fortnite4k | +0.37 … +0.47 |
| native25 | +0.10 |
| alphago_movie (static movie) | +0.02 |
| obs_new (desktop capture) | +0.02 … +0.03 |

At 8000 kb/s alphago/obs_new are essentially zero (−0.023 … +0.047), while
demons_souls still gains +1.4 … +1.6.

## Interpretation

- CABAC is worth roughly **3 % bitrate efficiency**, not 10–20 %. At matched
  bitrate that converts to ~+0.35 VMAF at 8000, ~+0.6 at 6000, ~+1.7 at 2000
  (where entropy overhead matters most relative to payload).
- On easy/static content at 6000–8000 kb/s the gain is **~0** — entropy coding
  is not the bottleneck there.
- This agrees with the earlier single-clip ablation (removing CABAC from
  veryslow cost −0.552 VMAF) and with our CPU measurement (CABAC on/off on
  obs_new was 94.054 vs 94.064).
- Conclusion: a parallel CABAC path alone cannot deliver a breakthrough at
  our main targets. The larger levers are **mode RD / trellis** (x264
  veryslow ablation −0.86 and −0.65 when removed) and **B-frame/lookahead**
  structure. CABAC should be treated as a modest additive gain (~0.3–0.6
  VMAF) that only pays off if we can keep ~100 fps.
