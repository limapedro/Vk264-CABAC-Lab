# CPU CABAC cost and quality — direct measurement

Source: `E:\__PROJECTS__\Vk264\build\obs_new_src.yuv` (1920x1080, 300f, 60fps,
user's OBS desktop-capture content). Machine: i5-12400F (6C/12T), GPU work
from other agents may add noise to wall time but not to VMAF.

Commands:
```
ffmpeg -v error -y -f rawvideo -pixel_format yuv420p -video_size 1920x1080 \
  -framerate 60 -i <src> -frames:v 300 -an -c:v libx264 -preset slow \
  -b:v 6000k -maxrate 6000k -bufsize 12000k -g 120 \
  -x264-params "cabac=<0|1>:scenecut=0" -f h264 results/cpu_cabac<0|1>.h264
```

| variant | wall (300f) | fps | achieved kb/s | VMAF |
|---|---:|---:|---:|---:|
| CABAC off (CAVLC) | 7.14 s | 42.0 | 6174 | **94.064** |
| CABAC on | 6.92 s | 43.4 | 6153 | **94.054** |

## Findings

1. **CABAC is not the CPU bottleneck in x264-slow.** Turning it on changed
   wall time by only ~0.22 s / 300 f = ~0.7 ms/frame (and was slightly
   *faster*, likely noise/-rate interaction). Motion analysis and RD dominate.
2. **At matched bitrate on this content, CABAC gave no VMAF gain** (−0.010).
   This differs from the veryslow ablation (removing CABAC cost −0.552 VMAF),
   where CABAC interacts with trellis=2 and other mode-RD tools. Interpret
   CABAC's benefit as *rate efficiency* (it saves bits), which only converts
   into VMAF when the rate controller spends those savings on detail.
3. A CPU/GPU hybrid that moves bin encoding off the GPU would add a per-frame
   PCIe round-trip and a serial CPU stream, but the CPU bin encoding itself is
   ~0.7 ms/frame — comparable to our entire GPU CAVLC entropy budget (0.08 ms).
   The hybrid is therefore bounded by transfer/sync, not arithmetic.

Implication for the lab: chase **frame-level parallelism** (multiple frames
in flight) rather than trying to make one frame's CABAC faster; and remember
that CABAC alone may not move matched-rate VMAF without trellis/RD to exploit
the saved bits.
