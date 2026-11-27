#!/usr/bin/env python3
"""CABAC vs CAVLC across many videos, presets and bitrates.

Each encode: 120 frames, 1920x1080, g=120, scenecut=0, fixed bitrate.
Writes results incrementally to results/cabac_sweep.json so interrupted runs
still yield data.
"""
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results"
RES.mkdir(exist_ok=True)
OUT = RES / "cabac_sweep.json"
TMP = Path(r"C:\Users\multi\AppData\Local\Temp\opencode\cabac_sweep")
TMP.mkdir(parents=True, exist_ok=True)
FF = "ffmpeg"
FPS = {"native25": 25, "yt_sport": 50}

CLIPS = {
    "warzone_short": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\warzone_short.yuv",
    "fortnite4k": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\fortnite4k.yuv",
    "yt_forza": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\yt_forza.yuv",
    "yt_sport": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\yt_sport.yuv",
    "porsche_sport": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\porsche_sport.yuv",
    "demons_souls": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\demons_souls.yuv",
    "alphago_movie": r"C:\Users\multi\AppData\Local\Temp\opencode\corpus\masters\alphago_movie.yuv",
    "obs_new": r"E:\__PROJECTS__\Vk264\build\obs_new_src.yuv",
    "native25": r"E:\__PROJECTS__\Vk264\build\native1080_300.yuv",
}
PRESETS = ["medium", "slow", "veryslow"]
RATES = [2000, 6000, 8000]


def encode(master, fps, preset, rate, cab, out):
    cmd = [FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "yuv420p",
           "-video_size", "1920x1080", "-framerate", str(fps),
           "-i", master, "-frames:v", "120", "-an",
           "-c:v", "libx264", "-preset", preset, "-sc_threshold", "0",
           "-g", "120", "-b:v", f"{rate}k", "-maxrate", f"{rate}k",
           "-bufsize", f"{2*rate}k", "-x264-params", f"cabac={cab}",
           "-f", "h264", str(out)]
    subprocess.run(cmd, check=True, cwd=TMP)


def vmaf(master, fps, stream):
    log = TMP / "vm.json"
    cmd = [FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "yuv420p",
           "-video_size", "1920x1080", "-framerate", str(fps),
           "-i", master, "-i", str(stream),
           "-lavfi", "libvmaf=log_fmt=json:log_path=vm.json",
           "-f", "null", "NUL"]
    subprocess.run(cmd, check=True, cwd=TMP)
    return json.loads((TMP / "vm.json").read_text())["pooled_metrics"]["vmaf"]["mean"]


def main():
    rows = json.loads(OUT.read_text()) if OUT.exists() else []
    done = {(r["clip"], r["preset"], r["target"]) for r in rows}
    for clip, master in CLIPS.items():
        fps = FPS.get(clip, 60)
        for preset in PRESETS:
            for rate in RATES:
                if (clip, preset, rate) in done:
                    continue
                vals = {}
                for cab in (0, 1):
                    out = TMP / f"s.h264"
                    t0 = time.time()
                    encode(master, fps, preset, rate, cab, out)
                    kb = out.stat().st_size * 8 / 120 / (1 / fps) / 1000
                    v = vmaf(master, fps, out)
                    vals[cab] = {"kbps": round(kb, 1), "vmaf": round(v, 3)}
                    out.unlink()
                row = {"clip": clip, "preset": preset, "target": rate,
                       "cavlc": vals[0], "cabac": vals[1],
                       "dvmaf": round(vals[1]["vmaf"] - vals[0]["vmaf"], 3),
                       "dkbps": round(vals[1]["kbps"] - vals[0]["kbps"], 1)}
                rows.append(row)
                OUT.write_text(json.dumps(rows, indent=1) + "\n")
                print(f"{clip:16s} {preset:9s} {rate:5d} "
                      f"CAVLC {vals[0]['kbps']:8.0f}/{vals[0]['vmaf']:6.2f} "
                      f"CABAC {vals[1]['kbps']:8.0f}/{vals[1]['vmaf']:6.2f} "
                      f"dVMAF {row['dvmaf']:+.3f}", flush=True)
    print("\nSUMMARY")
    for preset in PRESETS:
        sel = [r for r in rows if r["preset"] == preset]
        print(preset, "n", len(sel),
              "mean dVMAF", round(sum(r["dvmaf"] for r in sel) / max(1, len(sel)), 3),
              "mean dkb%", round(100 * sum(r["dkbps"] / r["target"] for r in sel) / max(1, len(sel)), 2))


if __name__ == "__main__":
    main()
