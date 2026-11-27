#!/usr/bin/env python3
"""Feature set v2: past/future SAD, variance, edge, scene cuts, target bits,
GOP position and previous QP (from the x264-placebo label stream).
"""
import csv
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[2]
WORK = ROOT / "experiments" / "learned" / "work"
RES = ROOT / "results" / "learned"
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
FPS = {"yt_sport": 50}
NFRAMES, W, H = 300, 1920, 1080


def main():
    qp = {}
    with (RES / "placebo_qp.csv").open() as f:
        for r in csv.DictReader(f):
            qp[(r["clip"], int(r["frame"]))] = int(r["qp_placebo"])
    rows = []
    for clip, path in CLIPS.items():
        fps = FPS.get(clip, 60)
        target_bits = round(6000 * 1000 / fps)
        y = np.memmap(path, dtype=np.uint8, mode="r", shape=(NFRAMES, H, W))
        s = np.asarray(y[:, ::16, ::16], dtype=np.float32)
        mean = s.mean(axis=(1, 2))
        var = s.var(axis=(1, 2))
        gx = np.abs(np.diff(s, axis=2)).mean(axis=(1, 2))
        gy = np.abs(np.diff(s, axis=1)).mean(axis=(1, 2))
        edge = 0.5 * (gx + gy)
        sad_prev = np.zeros(NFRAMES)
        sad_prev[1:] = np.abs(s[1:] - s[:-1]).mean(axis=(1, 2))
        sad_next1 = np.zeros(NFRAMES)
        sad_next1[:-1] = sad_prev[1:]
        sad_next2 = np.zeros(NFRAMES)
        sad_next2[:-2] = np.abs(s[:-2] - s[2:]).mean(axis=(1, 2))
        prev_qp = np.full(NFRAMES, 30, dtype=float)
        prev_qp[1:] = [qp[(clip, i - 1)] for i in range(1, NFRAMES)]
        for i in range(NFRAMES):
            rows.append({
                "clip": clip, "frame": i, "fps": fps,
                "target_bits": target_bits,
                "mean_luma": round(float(mean[i]), 3),
                "logvar": round(float(np.log1p(var[i])), 4),
                "edge": round(float(edge[i]), 4),
                "logedge": round(float(np.log1p(edge[i])), 4),
                "logsad_prev": round(float(np.log1p(sad_prev[i])), 4),
                "logsad_next1": round(float(np.log1p(sad_next1[i])), 4),
                "logsad_next2": round(float(np.log1p(sad_next2[i])), 4),
                "scenecut": int(sad_prev[i] > 20),
                "scenecut_future": int(sad_next1[i] > 20),
                "gop_pos": i % 120,
                "gop_pos5s": i % (5 * fps),
                "complexity": round(float(edge[i] / (mean[i] + 1.0)), 5),
                "prev_qp": int(prev_qp[i]),
            })
    with (RES / "features_v2.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader(); w.writerows(rows)
    print(f"wrote {len(rows)} rows", flush=True)


if __name__ == "__main__":
    main()
