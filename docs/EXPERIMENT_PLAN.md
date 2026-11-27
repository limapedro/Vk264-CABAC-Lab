# Lab experiment plan (ranked)

Each experiment records: exact command, stream SHA-256 (where parity is
claimed), 1080p loop/process fps, achievable kb/s, PSNR-Y/SSIM/VMAF, ctest,
`ffmpeg -err_detect explode`. Negative results are committed with the same
rigour as positive ones.

## E1 — Slice-count scaling (cheapest, agent 3)

Existing CLI knob `--p-slice-rows {1,2}` plus a 4-row prototype in an isolated
copy. Measure 17/34/68 P-slices at 6000 kb/s:
- entropy stage ms/frame, loop fps, process fps
- quality cost of 1-row slices (paired prediction disabled)
- stream size delta
Gate: any byte-identical path with >5% loop gain is a candidate for Vk264.

## E2 — CPU vs GPU CABAC cost (done, see notes/CPU_CABAC_COST.md)

Measured: CPU CABAC ~0.7 ms/frame in x264-slow; no matched-rate VMAF gain on
this content (94.054 vs 94.064 CAVLC). Conclusion: CPU hybrid is bounded by
transfer/sync, not arithmetic.

## E3 — WPP legality and quality cost (agent 2)

x264 CLI `-x264-params slices=68:constrained-intra=1` vs baseline on the lab
master; VMAF at matched bitrate. If constrained-intra costs >0.5 VMAF, WPP is
dead; if small, the remaining problem is non-standard context carry.

## E4 — Frame-parallel CABAC (agent 1)

Design first, then prototype: N frames in flight, each with its own CABAC
state/context buffer and Slices scratch; Rate feedback delayed by N frames;
descriptor sets reused from the reference ring. Gate: >=60 fps loop with
CABAC and clean decode; >=100 fps required to land in Vk264.

## E5 — Speculative checkpoint/rollback (agent 4)

CPU simulator on real bin streams: checkpoint state size, conflict rate per
64-bin chunk, expected speedup. Gate: conflict rate <1% and state <64 B/chunk
before GPU prototype.

## Decision rule

A path is "promising" only if it keeps H.264-conformant decode and reaches
>=100 fps loop at 1080p, OR if the user explicitly accepts a slower mode as
an opt-in quality preset. Otherwise record the blocker and move on.
