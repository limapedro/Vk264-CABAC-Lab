#!/usr/bin/env python3
"""Collect per-frame features and x264-slow QP labels for 9 1080p clips.

For each clip:
  * extract 16x16-stride luma features (mean, variance, edge, temporal SAD,
    scene-cut flag),
  * encode 300 frames with x264-slow ABR at 6000 kb/s (pass 1 stats),
  * parse the per-frame Q from x264 stats and per-packet bytes from ffprobe.

All raw outputs are temporary; the retained artifacts are small CSV/JSON files.
"""
import csv
import json
import re
import subprocess
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "experiments" / "learned" / "work"
RES = ROOT / "results" / "learned"
WORK.mkdir(parents=True, exist_ok=True)
RES.mkdir(parents=True, exist_ok=True)

FF = r"C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe"
FP = r"C:\ffmpeg\ffmpeg-master-latest-win64-gpl\bin\ffprobe.exe"
MASTERS = Path(r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters")
BUILD = Path(r"E:\__PROJECTS__\Vk264\build")
CLIPS = {
    "warzone_short": MASTERS / "warzone_short.yuv",
    "fortnite4k": MASTERS / "fortnite4k.yuv",
    "yt_forza": MASTERS / "yt_forza.yuv",
    "yt_sport": MASTERS / "yt_sport.yuv",
    "porsche_sport": MASTERS / "porsche_sport.yuv",
    "demons_souls": MASTERS / "demons_souls.yuv",
    "alphago_movie": MASTERS / "alphago_movie.yuv",
    "obs_new": BUILD / "obs_new_src.yuv",
    "obs_capture": BUILD / "obs_capture_src.yuv",
}
FPS = {"yt_sport": 50}  # lab convention: masters default 60 fps
NFRAMES = 300
TARGET = 6000  # kb/s, one of the lab's three sweep rates
W, H = 1920, 1080
STATS_RE = re.compile(r"in:(\d+)\s+out:(\d+)\s+type:(\w+).*?\bq:([\d.]+)")


def features(path):
    frame_bytes = W * H * 3 // 2
    y = np.memmap(path, dtype=np.uint8, mode="r", shape=(NFRAMES, H, W))
    small = np.asarray(y[:, ::16, ::16], dtype=np.float32)  # 68 x 120
    mean = small.mean(axis=(1, 2))
    var = small.var(axis=(1, 2))
    gx = np.abs(np.diff(small, axis=2)).mean(axis=(1, 2))
    gy = np.abs(np.diff(small, axis=1)).mean(axis=(1, 2))
    sad = np.zeros(NFRAMES, dtype=np.float32)
    sad[1:] = np.abs(small[1:] - small[:-1]).mean(axis=(1, 2))
    sad2 = np.zeros(NFRAMES, dtype=np.float32)
    sad2[2:] = np.abs(small[2:] - small[:-2]).mean(axis=(1, 2))
    return mean, var, gx, gy, sad, sad2


def encode_labels(clip, path, fps):
    log = WORK / f"{clip}-0.log"
    stream = WORK / f"{clip}.h264"
    for p in (log, stream):
        if p.exists():
            p.unlink()
    cmd = [FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "yuv420p",
           "-video_size", f"{W}x{H}", "-framerate", str(fps),
           "-i", str(path), "-frames:v", str(NFRAMES), "-an",
           "-c:v", "libx264", "-preset", "slow", "-sc_threshold", "0",
           "-g", "120", "-b:v", f"{TARGET}k", "-pass", "1",
           "-passlogfile", str(WORK / clip), "-f", "h264", str(stream)]
    t0 = time.time()
    subprocess.run(cmd, check=True, cwd=WORK)
    secs = time.time() - t0
    rows = {}
    for line in log.read_text(errors="replace").splitlines():
        m = STATS_RE.search(line)
        if m:
            rows[int(m.group(1))] = (int(m.group(2)), m.group(3), float(m.group(4)))
    sizes = subprocess.run(
        [FP, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=size", "-of", "csv=p=0", str(stream)],
        capture_output=True, text=True, check=True, cwd=WORK).stdout.split()
    bits = [int(s) * 8 for s in sizes if s.strip()]
    return rows, bits, secs, stream.stat().st_size


def main():
    feature_rows = []
    label_rows = []
    meta = {}
    for clip, path in CLIPS.items():
        fps = FPS.get(clip, 60)
        print(f"[{clip}] features...", flush=True)
        mean, var, gx, gy, sad, sad2 = features(path)
        print(f"[{clip}] x264-slow ABR {TARGET}k...", flush=True)
        rows, bits, secs, stream_bytes = encode_labels(clip, path, fps)
        if len(rows) != NFRAMES or len(bits) != NFRAMES:
            print(f"  WARNING stats={len(rows)} packets={len(bits)}", flush=True)
        n = min(len(bits), NFRAMES)
        for i in range(NFRAMES):
            out_idx, ftype, q = rows.get(i, (i, "P", float("nan")))
            b = bits[out_idx] if out_idx < len(bits) else 0
            edge = 0.5 * (gx[i] + gy[i])
            feature_rows.append({
                "clip": clip, "frame": i, "fps": fps, "target_kbps": TARGET,
                "mean": round(float(mean[i]), 3),
                "logvar": round(float(np.log1p(var[i])), 4),
                "edge": round(float(edge), 4),
                "logedge": round(float(np.log1p(edge)), 4),
                "sad": round(float(sad[i]), 4),
                "logsad": round(float(np.log1p(sad[i])), 4),
                "logsad2": round(float(np.log1p(sad2[i])), 4),
                "scenecut": int(sad[i] > 20.0),
                "gop_pos": i % 120,
                "complexity": round(float(edge / (mean[i] + 1.0)), 5),
            })
            label_rows.append({
                "clip": clip, "frame": i, "qp_x264_slow": round(q, 2),
                "bits": b, "type": ftype,
            })
        meta[clip] = {
            "fps": fps, "encode_seconds": round(secs, 2), "stream_bytes": stream_bytes,
            "total_bits": sum(bits), "frames_with_stats": len(rows),
            "frames_with_packets": len(bits),
            "qp_mean": round(float(np.nanmean([rows[i][2] for i in rows])), 3) if rows else None,
        }
        print(f"  {secs:.1f}s, {len(rows)} QP labels, {len(bits)} packet sizes, "
              f"stream={stream_bytes/1e6:.2f} MB", flush=True)
        (WORK / f"{clip}.h264").unlink(missing_ok=True)

    with (RES / "features.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(feature_rows[0]))
        w.writeheader(); w.writerows(feature_rows)
    with (RES / "labels_x264_slow.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(label_rows[0]))
        w.writeheader(); w.writerows(label_rows)
    (RES / "collection_meta.json").write_text(json.dumps(meta, indent=2))
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
