#!/usr/bin/env python3
"""x264-PLACEBO per-frame QP labels via ffmpeg trace_headers.

Same 9 clips / 300 frames / 6000 kb/s as collect_data.py.  Saves
results/learned/placebo_qp.csv with QP decoded from slice_qp_delta and the
frame type decoded from slice_type.  Estimated VMAF evaluation uses these
labels as the 'placebo allocation' reference.
"""
import csv
import re
import subprocess
import time
from pathlib import Path

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
FPS = {"yt_sport": 50}
NFRAMES = 300
TARGET = 6000
W, H = 1920, 1080
TYPE_NAME = {0: "P", 1: "B", 2: "I", 3: "SP", 4: "SI"}


def parse_trace(stream):
    r = subprocess.run(
        [FF, "-hide_banner", "-loglevel", "debug", "-i", str(stream),
         "-c", "copy", "-bsf:v", "trace_headers", "-f", "null", "-"],
        capture_output=True, text=True)
    lines = r.stderr.splitlines()
    init, qps, types = 0, [], []
    for i, line in enumerate(lines):
        m = re.search(r"pic_init_qp_minus26\s+.*?=\s*(-?\d+)", line)
        if m:
            init = int(m.group(1))
        if "Slice Header" not in line:
            continue
        q = t = None
        for nxt in lines[i + 1:i + 70]:
            if q is None:
                mq = re.search(r"slice_qp_delta\s+.*?=\s*(-?\d+)", nxt)
                if mq:
                    q = int(mq.group(1))
            if t is None:
                mt = re.search(r"slice_type\s+.*?=\s*(\d+)", nxt)
                if mt:
                    t = int(mt.group(1)) % 5
            if q is not None and t is not None:
                break
        if q is not None:
            qps.append(26 + init + q)
            types.append(TYPE_NAME.get(t, "?"))
    return qps, types


def packet_bits(stream):
    r = subprocess.run(
        [FP, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=size", "-of", "csv=p=0", str(stream)],
        capture_output=True, text=True, check=True)
    return [int(s) * 8 for s in r.stdout.split() if s.strip()]


def main():
    rows, meta = [], {}
    for clip, path in CLIPS.items():
        fps = FPS.get(clip, 60)
        stream = WORK / f"placebo_{clip}.h264"
        t0 = time.time()
        subprocess.run(
            [FF, "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "rawvideo", "-pixel_format", "yuv420p",
             "-video_size", f"{W}x{H}", "-framerate", str(fps),
             "-i", str(path), "-frames:v", str(NFRAMES), "-an",
             "-c:v", "libx264", "-preset", "placebo", "-sc_threshold", "0",
             "-g", "120", "-b:v", f"{TARGET}k", "-f", "h264", str(stream)],
            check=True, cwd=WORK)
        secs = time.time() - t0
        qps, types = parse_trace(stream)
        bits = packet_bits(stream)
        if len(qps) != NFRAMES or len(bits) != NFRAMES:
            print(f"WARNING {clip}: qps={len(qps)} bits={len(bits)}", flush=True)
        n = min(NFRAMES, len(qps), len(bits))
        for i in range(n):
            rows.append({"clip": clip, "frame": i, "qp_placebo": qps[i],
                         "type_placebo": types[i], "bits_placebo": bits[i]})
        meta[clip] = {"fps": fps, "seconds": round(secs, 2),
                      "qp_labels": len(qps), "packets": len(bits),
                      "total_bits": sum(bits), "mean_qp": round(sum(qps) / max(1, len(qps)), 2),
                      "stream_bytes": stream.stat().st_size}
        print(f"[{clip}] {secs:.1f}s labels={len(qps)} packets={len(bits)} "
              f"meanQP={meta[clip]['mean_qp']}", flush=True)
        stream.unlink(missing_ok=True)
    with (RES / "placebo_qp.csv").open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader(); w.writerows(rows)
    (RES / "placebo_meta.json").write_text(__import__("json").dumps(meta, indent=2))
    print("DONE", flush=True)


if __name__ == "__main__":
    main()
