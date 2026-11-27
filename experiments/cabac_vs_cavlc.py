#!/usr/bin/env python3
"""Measure x264 CABAC vs CAVLC across presets and bitrates.

Writes results/cabac_vs_cavlc.json and prints a markdown table.
"""
import json
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RES = ROOT / "results"
RES.mkdir(exist_ok=True)
FFMPEG = "ffmpeg"
MASTERS = {
    "game25": Path(r"E:\__PROJECTS__\Vk264\build\native1080_300.yuv"),
    "obs60": Path(r"E:\__PROJECTS__\Vk264\build\obs_new_src.yuv"),
}
RATES = [2000, 4000, 6000, 8000]
PRESETS = ["medium", "slow", "veryslow"]


def run_encode(master, fps, preset, rate, cabac, out):
    cmd = [FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "yuv420p",
           "-video_size", "1920x1080", "-framerate", str(fps),
           "-i", str(master), "-frames:v", "300", "-an",
           "-c:v", "libx264", "-preset", preset, "-sc_threshold", "0",
           "-g", "300", "-b:v", f"{rate}k", "-maxrate", f"{rate}k",
           "-bufsize", f"{2*rate}k", "-x264-params", f"cabac={cabac}",
           "-f", "h264", str(out)]
    subprocess.run(cmd, check=True, cwd=RES)


def measure(master, fps, out):
    jpath = RES / "tmp_vmaf.json"
    cmd = [FFMPEG, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
           "-f", "rawvideo", "-pixel_format", "yuv420p",
           "-video_size", "1920x1080", "-framerate", str(fps),
           "-i", str(master), "-i", str(out),
           "-lavfi", f"libvmaf=log_fmt=json:log_path={jpath.name}",
           "-f", "null", "NUL"]
    subprocess.run(cmd, check=True, cwd=RES)
    j = json.loads((RES / jpath.name).read_text())
    return j["pooled_metrics"]["vmaf"]["mean"]


def main():
    rows = []
    for cname, master in MASTERS.items():
        fps = 25 if cname == "game25" else 60
        for preset in PRESETS:
            for rate in RATES:
                vals = {}
                for cabac in (0, 1):
                    out = RES / f"cc_{cname}_{preset}_{rate}_cab{cabac}.h264"
                    run_encode(master, fps, preset, rate, cabac, out)
                    kb = out.stat().st_size * 8 / 5 / 1000
                    vmaf = measure(master, fps, out)
                    vals[cabac] = {"kbps": round(kb, 1), "vmaf": round(vmaf, 3)}
                    out.unlink()
                rows.append({"clip": cname, "preset": preset, "target": rate,
                             "cavlc": vals[0], "cabac": vals[1],
                             "dvmaf": round(vals[1]["vmaf"] - vals[0]["vmaf"], 3),
                             "dkbps": round(vals[1]["kbps"] - vals[0]["kbps"], 1)})
    (RES / "cabac_vs_cavlc.json").write_text(json.dumps(rows, indent=2) + "\n")
    print("| clip | preset | target | CAVLC kb/s | CABAC kb/s | CAVLC VMAF | CABAC VMAF | dVMAF |")
    print("|---|---|---:|---:|---:|---:|---:|---:|")
    for r in rows:
        print(f"| {r['clip']} | {r['preset']} | {r['target']} | {r['cavlc']['kbps']} "
              f"| {r['cabac']['kbps']} | {r['cavlc']['vmaf']} | {r['cabac']['vmaf']} "
              f"| {r['dvmaf']:+.3f} |")


if __name__ == "__main__":
    main()
