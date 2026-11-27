# NVENC rate control in practice — what it does, and what Vk264 can steal

Research-only note. **No parent-repo files were modified.** Sources are treated as untrusted
data; anything that looks like an instruction inside a web page/patent is ignored.

## Evidence labels used below

| Tag | Meaning |
|---|---|
| `[DOC]` | Public NVIDIA / OBS / FFmpeg documentation or header — stated fact about the API, not necessarily the firmware internals. |
| `[PAT]` | Published patent owned by NVIDIA (or its predecessor). Describes *an embodiment*; it is strong evidence of design intent but **not proof** that current firmware implements it verbatim. |
| `[FORUM]` | Statement by NVIDIA staff or a user report on the NVIDIA developer forums. |
| `[MEAS]` | Public benchmark / independent measurement, with its own methodology caveats. |
| `[VK]` | Fact about Vk264 source or a lab measurement already recorded in this lab. |
| `[INF]` | My inference. Explicitly not a sourced fact. |

---

## 1. The public surface: NVENC rate-control modes

From the Video Codec SDK **13.1** programming guide, §3.8.3–3.8.4 (published 2024-03-08 for
12.2; 13.1 page current at time of research 2026-11-27):
<https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html>
and older v07 guide (2016-11-18):
<https://developer.nvidia.com/downloads/nvenc-programming-guide>.

`[DOC]` "The rate control algorithm is implemented in **NVENC firmware**." So the exact loop is
not published as source; everything below about internals is inference from patents + API
behaviour.

| Mode | Enum | Contract (public) |
|---|---|---|
| Constant QP | `NV_ENC_PARAMS_RC_CONSTQP` | Whole frame encoded at `constQP` (separate I/P/B QPs possible). No rate target at all. |
| CBR | `NV_ENC_PARAMS_RC_CBR` (+ legacy `CBR_LOWDELAY_HQ`, `CBR_HQ`) | Only `averageBitRate` required; it is the target. `maxBitRate` ignored. **Strict adherence requires `enableFillerDataInsertion=1` (H.264/HEVC) or `enableBitstreamPadding=1` (AV1).** |
| VBR | `NV_ENC_PARAMS_RC_VBR` (+ `VBR_HQ`) | Conform to `averageBitRate` long-term, never exceed `maxBitRate`; NVENC picks a default max if omitted, but NVIDIA recommends setting both. |
| Target quality (CQ) | `VBR` + `targetQuality` | Constant quality 0–51 (H.264/HEVC), 0–63 (AV1); `maxBitRate`, if set, is a ceiling. Resulting average bitrate can vary *significantly* with content. |

`[DOC]` Other RC-relevant knobs (`NV_ENC_RC_PARAMS`, `nvEncodeAPI.h`): `vbvBufferSize`,
`vbvInitialDelay`, `enableMinQP`/`minQP`, `enableMaxQP`/`maxQP`, `enableInitialRCQP`/
`initialRCQP`, `enableAQ` + `aqStrength` (1..15), `enableLookahead` + `lookaheadDepth`,
`disableIadapt`, `disableBadapt`, `enableTemporalAQ`, `strictGOPTarget`, `lowDelayKeyFrameScale`,
`multiPass`, `zeroReorderDelay`, `enableNonRefP`, `qpMapMode`. QP range is 0–51 for H.264/HEVC
and 0–63 for AV1 (`[DOC]`, guide §3.8.3 and FFmpeg `nvenc.c` CQ mode comment).

`[DOC]` **Multi-pass is per-frame pre-analysis, not chapters**: 1-pass estimates MB QP and
encodes immediately; 2-pass quarter-res first pass estimates frame complexity and bit
distribution, second pass encodes; 2-pass full-res produces better statistics, quarter-res
catches larger motion vectors. Both get closer to target "especially for CBR encoding", at a
throughput cost. (§3.8.4; measured RTX 3090 cost in StreamFX wiki: P4 317→371 fps quarter-pass,
→236 fps full-pass at 1080p H.264 CBR HQ — `[MEAS]`, <https://github.com/Vhonowslend/StreamFX-Public/wiki/Encoder-FFmpeg-NVENC>).

`[DOC]` Look-ahead (§8.2): buffers frames, "estimate their complexity and allocate the bits
appropriately among these frames proportional to their complexity"; also dynamically allocates
B/P frames and (unless disabled) inserts I-frames at scene cuts. `lookaheadDepth` must be
`0..(31 - number of B-frames)` in SDK 13.1 (older v07 guide said "up to 32"). While lookahead
is filling, `NvEncEncodePicture` returns `NV_ENC_ERR_NEED_MORE_INPUT`.

`[DOC]` AQ (§8.5): spatial AQ moves QP on top of RC output toward flat regions; typically
**lowers PSNR**. Temporal AQ invests bits in low-motion/high-detail regions that will be used
as future references. Explicit caveat: "enabling temporal AQ may result in high fluctuation of
bits consumed per frame within a GOP. I/P frames will consume more bits than average P-frame
size and B frames lesser. **Although target bitrate will be maintained at the GOP level**, the
frame size will fluctuate from one frame to next... If a strict CBR profile is required for
every frame size within a GOP, it is not recommended to enable temporal AQ."

`[DOC]` Emphasis-map note (§8.10) is a useful admission of ordering: "QP adjustment is performed
**after** the rate control algorithm has run. Therefore, there is a possibility of VBV/rate
violations." Same lesson for any post-hoc QP hack.

`[DOC]` `NvEncReconfigureEncoder` can change bitrate on the fly, and `resetEncoder=1` resets
internal RC state for adaptation — evidence that RC keeps significant persistent history.

### Buffer windows NVIDIA actually recommends

| Use case | VBV buffer | Source |
|---|---|---|
| Recording / archiving (VBR) | **4 seconds** | SDK use-case table `[DOC]` |
| Game-casting / cloud transcoding (CBR) | **1 second** | SDK use-case table `[DOC]` |
| Low-latency game streaming / conferencing (CBR) | **single frame** = `bitrate/framerate` | SDK use-case table `[DOC]` |

`[MEAS]` FFmpeg's NVENC wrapper defaults to `vbvBufferSize = 2 * averageBitRate` (**2 seconds**)
when the user does not set `-bufsize` (`libavcodec/nvenc.c`, e.g. FFmpeg 7.1 source:
<https://ffmpeg.org/doxygen/7.1/nvenc_8c_source.html>, line ~1054). This is why many published
NVENC tests have noticeably burstier streams than the "1-second" recommendation. FFmpeg also
clamps lookahead to `min(nb_surfaces, async_depth) - frameIntervalP - 4` and silently disables
it if that is negative (same source, ~line 1137).

`[MEAS]` Jan Ozer, SME 2019, "Benchmarking FFmpeg's Hardware Codecs"
(<https://streaminglearningcenter.com/wp-content/uploads/2019/05/SME-2019-FFmpeg-Hardware.pdf>,
2019-05-07): explicitly flagged "data rate fluctuations due to 2 second VBV buffer", switched
to a 1-second buffer and reported "slightly higher overall bitrate and slightly more uniform
stream". His CBR H.264 test at 5 Mbit/s with 1 s VBV: peak 5468 kb/s, VMAF 82.19 (medium preset)
vs 2 s VBV peak 5468/5384 and VMAF 82.35; x264-medium CBR peak 5123, VMAF ~81.8–82.4. The
ladder used **1080p60 at 6 Mbit/s** as the top rung. This is the closest public measurement to
the 1080p60 6000 kb/s question, but note it is a presentation, not a controlled paper.

`[MEAS]` NVIDIA's own benchmark methodology doc, "Video Benchmark Assumptions" (2022-03-03
<https://developer.download.nvidia.com/designworks/video-codec-sdk/Video-Benchmark-Ada-July-2023.pdf>,
published 2023-07-24): "**we consider the bitrate actually generated by the encoder (and not the
target bitrate specified on the command line)**. We have observed that in certain cases, an
encoder may generate bitstream at bitrate different (sometimes significantly different) from the
target." Their latency-sensitive H.264/HEVC command is
`-preset p1 -rc cbr -tune ll -multipass 0 -b:v BITRATE -bufsize BITRATE/FRATE` — i.e. a
**single-frame VBV** and strict frame-size cap; the x264 comparison uses
`-b:v -bufsize BITRATE/FRATE -maxrate -minrate -x264opts no-sliced-threads:no-psy=1:aq-mode=0`.
The Turing blog (2019-02-13,
<https://developer.nvidia.com/blog/turing-h264-video-encoding-speed-and-quality>) used
high-quality VBR `bufsize = BITRATE*2`, `-bf 3 -b_ref_mode 2 -temporal-aq 1 -rc-lookahead 20`,
and low-latency CBR `bufsize = BITRATE/FRATE`, infinite GOP.

`[MEAS]` Low-quality secondary sources (blog posts, 2026) claim roughly "NVENC H.264 at 6000
kb/s ≈ x264 veryfast at 6000; NVENC at 8000 ≈ x264 medium at 6000"; one tabulation (ForaSoft,
2026-06-25) lists NVENC H.264 p7 at +5% BD-rate vs x264 medium. These are **not** rigorous
measurements; treat as directional only. I did **not** find a peer-reviewed public study giving
±% bitrate-accuracy numbers for NVENC at exactly 1080p60/6000 or 8000 kb/s. `[INF]` The absence
is itself instructive: even NVIDIA's benchmark doc refuses to use the requested bitrate as the
independent variable.

---

## 2. What the algorithm actually looks like (patent evidence)

`[PAT]` **US 7,535,959 B2**, "Apparatus, system, and method for video encoder rate control",
NVIDIA, filed 2003-10-16, granted 2009-05-19, expired 2025-10-31:
<https://patents.google.com/patent/US7535959B2/en>.
Its CBR controller is a structure our `scan.comp` resembles in spirit but is much richer:

1. *Picture analysis*: classify macroblocks by type (intra I, intra P/B, non-intra P,
   bidirectional B), compute per-type frequency `ρ_k(i)` and per-type energy from MAD
   (mean absolute difference) of input or motion-compensated residual. Time-averaged energy
   estimates `ē_k(i)` are IIR filters.
2. *Intra energy* `E_intra(i)` is measured **every frame**, not just at I frames, to anticipate
   sudden complexity changes and avoid VBV under/overflow — this is essentially "lookahead by
   statistical anticipation".
3. *Complexity model*: per-MB-type complexity `x_k(i) = bits·q`, smoothed by time-aging and
   combined with frequencies into a predicted picture complexity `X̂(i)`.
4. *Ideal bit allocation* over a forward-looking window of **N pictures (a multiple of GOP)**:
   `B_m(i) = X_m(i-1)·W_m·B(i) / Σ_n X_n(i-1)·W_n·N_n`. Typical weights disclosed:
   `W_P = 1.0`, `W_B = 1.4`, `W_I = 1.0` implied.
5. *VBV fullness adjustment*: modify nominal budget using actual vs "picture-adjusted ideal"
   VBV fullness.
6. *VBV compliance adjustment*: **iteratively predict future VBV fullness for the next N
   pictures**; if the predicted path drops below `VBV_min`, reduce the current allocation.
7. *Picture-level QP assignment*: convert final bits to a quantizer step size.
8. A parallel *VBR core* uses a **second-order PI controller** on the long-term average rate
   with a selectable time constant τ ("preferably longer than the longest expected scene"),
   and an arbiter picks the **larger quantization step size** (i.e. the more conservative of
   CBR and VBR). Explicit knobs: `Ravg`, `Rpeak`, `Bvbv`, `Q0`, `Qmin`, `Qmax`, `Qtarget`; the
   patent warns that `Qmax` may prevent reaching `Ravg` and `Qmin` may prevent achieving the
   target on simple content — exactly the failure mode Vk264's `smoothAbr` hit.

`[PAT]` **US 10,462,473 B2**, "Method and apparatus for rate control for constant-bit-rate
finite-buffer-size video encoder", originally ATI Technologies, priority 2000-04-18, granted
2019-10-29:
<https://patents.google.com/patent/US10462473B2/en>.
Key elements: hierarchical **GOP-level / picture-level / macroblock-level** bit predictors;
"normalized VBV fullness" that removes GOP-position dependence from the control error;
GOP-level rate correction proportional to VBV fullness error
(`R = R0 + (E_R,M - E_0,M)/(N_t - N)·Δ_M`); scene-change detection **from the prediction-error
image** (ratio of intra-coded to non-intra-coded MBs) used to **reset global complexity
history** `X_ipb`; an "emergency quantization mode" when a partially encoded frame exceeds a
bit threshold — remaining MBs encode only DC coefficient, essentially a hard frame-size cap.
It also admits that pre-analysing future frames gives high quality/stability but costs delay
and memory, hence the design explicitly targets single-pass using *past and current* frames.
This is the clearest public description of how a hardware CBR controller handles **bursts and
scene cuts without blowing the VBV**.

`[PAT]` **US 8,897,365 B2** (NVIDIA, 2014-11-25), "Video rate control processor for a video
encoding process": a dedicated rate-control processor takes encoding-complexity parameters from
the ME engine per MB and per frame, and dynamically adjusts QP to hit the target.
<https://patents.google.com/patent/US8897365/en>

`[PAT]` **US 8,798,140 B2** (NVIDIA, 2014-08-05), "Encoding video frames in a video encoder":
limit bits per frame to a common upper bound; claim 20 says the upper limit equals **3× the
target average bitrate**. Complexity is "average of variances of macroblocks", using MTB
(maximum target bits) per MB.
<https://patents.google.com/patent/US8798140B2/en>

`[INF]` Reading these together with the API docs, NVENC's RC is best described as:

> **a VBV/token-bucket controller with per-frame QP derived from predicted complexity, plus
> optional pre-analysis (lookahead and 2-pass), plus filler data when the bucket would
> otherwise under-run.**

It is **not** a pure "measure last frame → adjust QP" feedback loop like Vk264's
`scan.comp:75-86`. But it is **not** pure offline pre-analysis either: the patents explicitly
aim for single-pass and the SDK exposes dynamic bitrate reconfiguration. It is a hybrid with a
*feed-forward complexity model* as the primary controller and *VBV feedback* as the safety
layer.

---

## 3. Behaviour on static/duplicated frames and sudden bursts

### Static / duplicated frames

`[FORUM]` 2020-06-08, "NVENC CBR bitrate way below configured average bitrate"
(<https://forums.developer.nvidia.com/t/nvenc-cbr-bitrate-way-below-configured-average-bitrate/126787>):
user reports 300 kb/s target → ~100 kb/s on non-dynamic video; NVIDIA staff (mandar_godse,
2020-06-12): "You can try setting `enableFillerDataInsertion` ... to 1"; user confirms it works
and that there is no way to get accurate bitrate *without* filler. Same pattern on still-image
input in 2017 (Quadro M2000, 20 Mb/s requested → 1.2 Mb/s achieved):
<https://forums.developer.nvidia.com/t/how-to-inclease-the-bitrate-of-video-of-still-image-using-h264-nvenc-encode/47400>.

`[INF]` Mechanism: when motion is zero, the encoder emits skip MBs and headers only. The VBV
bucket **fills but is never spent**; since NVENC's QP has a lower bound (minQP, or driver's
implicit floor), it cannot force more meaningful bits into a frame that has nothing to code.
Hence a "CBR" stream under-runs unless the client explicitly pads. Filler data is exactly the
hardware equivalent of sending dummy NAL bytes.

`[DOC]` Temporal AQ is the *quality*-oriented answer to static content: on a static
high-detail background, put extra bits into the **reference** frame so the following residuals
cost less. The explicit cost is larger frame-to-frame size swings inside a GOP (§8.5.2).

`[VK]` Vk264 has the same pathology with a different sign. `scan.comp:52-73` (opt-in
`smoothAbr`) uses a 1-second EMA of frame bits and an asymmetric log2 QP step, but the lab
recorded "EMA ratchets QP to 51 while still undershooting the target, starving fine detail" on
bimodal 60 fps capture, and added a QP floor of 18 / ceiling of 32 at ≥50 fps. Default ABR at
`scan.comp:75-86` has a QP floor of 18 at ≥50 fps because easy content was spending 4–6× the
target for <1 VMAF. `[INF]` NVENC's minQP is the same idea, exposed as an API knob instead of a
hardcoded guard.

### Bursts / scene cuts

`[DOC]` Lookahead's `disableIadapt` controls adaptive I insertion at scene cuts;
`lowDelayKeyFrameScale` (newer SDK) explicitly exists "to avoid channel congestion in case
I frame ends up generating high number of bits". `strictGOPTarget` exists to "minimize GOP-to-GOP
rate fluctuations". These are three separate mechanisms for the same problem.

`[PAT]` US10462473: scene-cut detection from intra/non-intra MB ratio in the prediction error
resets the complexity history, so a cut does not poison the next N frames' bit allocation;
emergency quantization caps a runaway frame. US7535959: VBV compliance iteratively reduces the
current bit budget if the *predicted* VBV path underflows.

`[FORUM]` 2016-10-25 "Constant Bitrate help" (<https://forums.developer.nvidia.com/t/constant-bitrate-help/45736>)
reports a 1080p30 500 kb/s target producing 745 kb/s due to spikes; NVIDIA moderator: with all
MBs at QP 50 and still 1.5 Mb/s, "it hits the compression limit. For getting lower bitrate, we
have to drop frames." I.e. at the extreme, QP bounds are not enough and frame skipping is the
only lever — Vk264 cannot use that lever for offline file encoding.

`[VK]` Lab measurement: a scene cut can spike one frame to ~20× the per-frame budget; the
unbounded debt correction drove QP to its floor and then ramped +2/frame for the rest of the
GOP (QP 33–34, PSNR 42.9 dB, VMAF 91.3 on frames 218–227 of the game clip). Current fix at
`scan.comp:48-50`: `correction = clamp(bitDebt / max(8, fps/2), ±allocation/4)` and
`aim = clamp(allocation - correction, allocation/2, 3*allocation/2)`. `[INF]` This is a
one-frame *ad hoc* version of the patent's "predict the VBV path over N frames and cap the
current allocation"; the patent version is cleaner and generalizes better.

### Per-frame vs GOP-level accuracy

`[DOC]` Temporal AQ quote above: target maintained **at GOP level**, not per frame. `[INF]`
This is the key conceptual distinction for Vk264: our ABR tries to correct after *every* frame
(`bitDebt += actual - allocation`), which creates the oscillation the lab measured. NVENC's
controller aims at a **window** (typically one GOP or ~1 s) and lets individual frames deviate,
using the VBV only as a hard floor/ceiling.

---

## 4. Transferable design for Vk264

### What Vk264 has today `[VK]`

- `shaders/scan.comp:24-28`: after all slices are scanned, writes `outputWords[0..3]`
  (bytes, overflow flag, `qp | depth<<8`, intra/older-block counts).
- `scan.comp:30`: `targetBits == 0` disables RC entirely (fixed-QP mode).
- `scan.comp:33-41`: reserves IDR bits across the GOP (`idrExtra = budget * min(5, interFrames/8)`)
  and shares them back into per-frame allocations.
- `scan.comp:48-50`: bounded debt correction and `aim ∈ [0.5×, 1.5×] allocation`.
- `scan.comp:75-86`: `bitDebt = clamp(bitDebt + actual - allocation, ±30*budget)`,
  `step = clamp(round(3·log2(actual/aim)), ±2)`, `qp' = clamp(qp+step, 18, 51)` for ≥50 fps.
- `scan.comp:52-73`: opt-in EMA "smoothed ABR" with asymmetric log2 steps, clamped to
  `[18,32]` above 50 fps.
- `src/main.cpp:207-229`: **whole source YUV is preloaded** into one `std::vector<char>`
  before encoding (optionally on a background thread). `[INF]` This is the crucial asset: a
  host-side complexity analyser can look at *future* frames for free, like NVENC's lookahead,
  without adding encode latency.
- Lab status: `PARITY_ROADMAP.md:101` records "reactive `scan.comp:24-82` (no predictive
  `bits(q)`, no EWMA, no qcomp)", VBV off, and that ABR instability costs up to +8 VMAF on
  trailer / +2.9 on 60 fps game; `docs/STATUS.md:47`.

### Proposed state fields (host side, one per session + a few per frame)

```cpp
struct RcBucket {
    int64_t capacityBits;      // B  = R_target * windowSeconds  (window 1.0 s for 60fps)
    int64_t occupancyBits;     // e, clamped [0, capacityBits]; sign convention: full = room to spend
    int64_t targetBits;        // R_target / fps
    int64_t idrReserveBits;    // amortised IDR cost, spread over interFrames
    int64_t sceneCutCredit;    // deferred extra bits owed by a cut, paid back over N frames
    double  emaBits;           // EMA of actual frame bits, window ~fps
    double  rmsRatio;          // sqrt(EMA((actual/budget)^2)) — burst-sensitive, not ratcheting
    double  xFrame, xI, xP;    // smoothed complexity per picture type (MAD/variance based)
    int     qpMin, qpMax;      // H.264 legal [0,51]; practical floor 18 for >=50fps
    int     qpStepMax;         // slew limit, keep 2 (current behaviour, lab-validated)
    int     resetCountdown;    // after scene cut: reset complexity history for N frames
};
struct FramePlan {
    int     plannedQp;         // already signalled via slice_qp_delta (lab note confirms path exists)
    int64_t allocBits;         // budget for this frame from window allocation
    uint8_t isSceneCut;        // from prediction-error intra/non-intra ratio
    uint8_t isStatic;          // SAD(frame, ref) < eps -> skip/near-skip expected
};
```

`[VK]` A `uint32 plannedQp` in `FrameConfig` is already identified as conformant in
`PARITY_ROADMAP.md:151` (QP is signalled via `slice_qp_delta`), so no bitstream change is needed.

### Formulas

Let `R` = target bits/s, `f` = fps, `Bf = R/f` = per-frame budget, `W = f` frames = 1 s window
(NVENC's gamecast recommendation; use `W = gop` if you prefer GOP-level accounting).

1. **Complexity measurement from preloaded source (cheap, `[VK]` preload makes it free):** for
   frame `i`, compute `MAD_i` against the already-decoded reconstruction (or SAD against the
   previous source frame as an approximation) and spatial variance `σ²_i`. Define
   `X_i = (MAD_i + c)·(1 + λ·σ²_i/σ²_ref)`, clipped. This is the patent's "energy" estimate.
2. **Bit allocation with type weights (patent Eq. for `B_m`):**
   `alloc_i = W · Bf · (w_type(i)·X̄_type(i)) / Σ_{n=i}^{i+W-1} w_type(n)·X̄_type(n)`,
   with `w_I ≈ 2–3` for Vk264's I/P-only stream (the patent uses `W_P=1.0, W_B=1.4, W_I=1.0`;
   adjust empirically — `[VK]` an explicit `ipratio` offset *lost* 0.3 VMAF because ABR already
   budgets IDRs, so keep the separate `idrReserveBits` and don't double-count).
3. **VBV / token bucket:** each frame the bucket accrues `Bf`; after encoding it is debited by
   `actualBits`: `e ← clamp(e + Bf - actualBits, 0, B)`. Enforce:
   - `e - predictedBits(qp) >= e_min` (no underflow) → raise QP until it fits;
   - `e + predictedBits(qp) <= B` (no overflow) → lower QP, but never below `qpMin`.
   `predictedBits(qp) ≈ X_i · N_mb · k / 2^((qp-4)/6)` (H.264 Qstep law; fit `k` once from a
   fixed-QP probe — this supplies the "predictive `bits(q)`" the lab says is missing).
4. **Bounded QP slew (keep what works):** `qp_i = clamp(qp_{i-1} + clamp(round(3·log2(actual/aim)), -2, +2), qpMin, qpMax)`.
   `[VK]` ±2/frame and floor 18 at ≥50 fps are already measured guards; keep them. Using the
   VBV error `(e_min - e)/Bf` in place of `actual/aim` would make the step proportional to how
   close the bucket is to empty, which is smoother.
5. **RMS rate averaging instead of a plain EMA:** keep both
   `ema = ema + (actual - ema)/W` and `rms = sqrt(ema2)` where
   `ema2 = ema2 + (actual²/budget² - ema2)/W`. Use `rms` only to widen the QP step during
   bursts (`if rms > 1.2: stepMax = 3`) and `ema` for the slow drift. `[INF]` A squared metric
   reacts to a single large frame without letting a long run of tiny frames ratchet QP upward —
   precisely the `smoothAbr` ratchet the lab recorded.
6. **Packet-size smoothing:** cap per-frame spend at `aim ∈ [0.5, 1.5]·alloc_i` (already
   present), and amortise a cut: when `isSceneCut`, add `excess = cutCost - alloc_i` to
   `sceneCutCredit` and repay at `credit/max(8, f/2)` bits per frame for the next `N` frames,
   instead of dumping it into the debt term at once. This is the patent's "predict the VBV path
   over N frames" in cheap form and subsumes the current `allocation/4` clamp.
7. **Static frames:** if `SAD(frame, ref) < ε`, plan a skip frame (`qp = qpMax`, near-zero bits)
   and **do not** try to fill the bucket with quality bits. For an offline file (Vk264's use
   case) allow the achieved rate to undershoot on static content; only add filler/padding if we
   ever target a strict CBR transport. `[INF]` Chasing a bitrate on a frame with no residual is
   what produced the 4–6× overspend the lab measured.
8. **Scene-cut detection without lookahead:** `ratio = intraMBs / (intraMBs + nonIntraMBs)` from
   the residual image; if `ratio > τ`, mark cut, force IDR, and set `resetCountdown = W` so the
   complexity history `X̄` is re-seeded (patent's "reset global complexity on scene change").

### Where each piece lands

- `host/src/main.cpp` after the preload (line ~229), before `EncoderGpu` submits work: compute
  `X_i` for all frames, scene-cut flags, initial `FramePlan[]`. This is the analogue of NVENC
  lookahead but costs no encode latency because all YUV is resident.
- `shaders/scan.comp`: keep the existing read-modify-write of its own state slot; replace the
  `bitDebt` update with the bucket update (fields above) and consume `plannedQp` as a hint
  before applying the bounded slew. Do **not** add cross-frame reads — `FRAME_PARALLEL.md:173`
  records that concurrent scans race on shared state, so each in-flight frame needs its own
  slot + host shadow (`FRAME_PARALLEL.md:193,259`).
- `rc.bitDebt` can be reused as the EMA state (as `scan.comp:58` already does) to avoid an ABI
  change; add a separate `rc.bucketOccupancy` word only if the N-slot state struct is extended.

### What *not* to copy

- Do not enable temporal-AQ-style deliberate reference-frame overspend if we ever want tight
  per-frame packet sizes: NVIDIA itself says it breaks strict per-frame CBR.
- Do not rely on filler for Vk264: it helps only when a real CBR transport needs it, and it
  burns bits without improving VMAF. Preloaded offline files should let the bucket undershoot.
- Do not chase x264's very-slow RD tools through RC; the lab has already established the RC
  gap is the cheap, host-side part (`PARITY_ROADMAP.md:101,151`).

---

## 5. Direct answers to the three specific questions

**Q: Frame-level feedback loop like ours, token bucket/VBV, or pre-analysis?**
`[DOC]+[PAT]+[INF]` Firmware-internal, so no direct source. Public evidence supports
**all three, layered**: a VBV/token-bucket compliance layer (public API exposes the bucket;
patents describe fullness adjustment and iterative underflow prediction), a **feed-forward
complexity→QP model** as the primary controller (patents), and optional **pre-analysis**
(lookahead 0..31−B frames; two-pass quarter/full per frame). A post-frame QP feedback loop of
the `bitDebt` kind exists only indirectly, as the slow VBR/PI term in US7535959 — and even there
it is subordinated to the VBV and to a *larger-Q-step* arbiter. It is **not** a
"last-frame-size → next-frame-QP" loop as the dominant mechanism.

**Q: Typical buffer windows and QP bounds?**
`[DOC]` VBV: 1 s for CBR gamecast, 4 s for VBR archive, one frame (`R/f`) for low latency;
FFmpeg default 2 s. Lookahead: up to `31 − Bframes` frames (SDK 13.1), typical published 20.
QP: 0–51 H.264/HEVC (0–63 AV1), with client-set `minQP`/`maxQP` and `initialRCQP` hint;
spatial AQ strength 1–15. QP slew per frame is **not published**. `[VK]` Vk264's own measured
good values: ±2/frame, floor 18 at ≥50 fps, `aim` clamp 0.5–1.5×, debt window `fps/2`.

**Q: Static frames and sudden bursts?**
`[DOC]+[FORUM]` Static frames under-run CBR (1.2 vs 20 Mb/s reported; 100 vs 300 kb/s
reported); filler insertion is NVIDIA's answer; temporal AQ shifts bits to static reference
regions at the cost of frame-size fluctuation. Bursts are handled by lookahead bit allocation,
adaptive I insertion, `lowDelayKeyFrameScale`, a 2-pass pre-analysis, and a VBV compliance check
that trims the current allocation when predicted fullness would underflow; at the hard limit,
MBs enter an emergency nearly-zero-bit mode (`[PAT]`) or frames are dropped (`[FORUM]`).

---

## 6. Fact vs inference, compactly

| Claim | Status |
|---|---|
| RC algorithm is in firmware; exact code not public | `[DOC]` fact |
| Four RC modes; CBR needs filler for strict adherence; VBR max is a ceiling | `[DOC]` fact |
| VBV buffer exposed in bits; recommended 1 s / 4 s / 1 frame windows | `[DOC]` fact |
| Lookahead estimates complexity and allocates bits proportionally; adaptive I/B | `[DOC]` fact |
| Temporal AQ keeps target at GOP level but causes per-frame fluctuation | `[DOC]` fact |
| FFmpeg default VBV = 2× bitrate; lookahead clamped by surface count | `[MEAS]` fact (source inspected) |
| CBR undershoots on static content; filler fixes it | `[FORUM]` fact (staff + user confirmation) |
| NVIDIA benchmark uses actual, not requested, bitrate | `[MEAS]` fact (NVIDIA doc) |
| Complexities, MB-type energies, VBV fullness/compliance adjustments, PI controller, scene-cut reset | `[PAT]` disclosed embodiments — strong evidence, not proof of current firmware |
| W_P=1.0, W_B=1.4, 3× per-frame cap, τ longer than a scene | `[PAT]` disclosed values; may be old |
| NVENC "uses a token bucket + feed-forward complexity model" | `[INF]` synthesis of the above |
| Proposed Vk264 bucket fields/formulas | `[INF]` recommendations, not measured; must be A/B tested |
| ±2 QP/frame, floor 18, 0.5–1.5 aim clamp, ~20× cut spike | `[VK]` lab measurements |

---

## 7. Sources (all URLs checked 2026-11-27)

1. NVIDIA Video Codec SDK 13.1 Programming Guide — RC §3.8.3–3.8.4, lookahead §8.2, AQ §8.5,
   emphasis map §8.10, use-case table. <https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvenc-video-encoder-api-prog-guide/index.html>
2. NVIDIA Video Codec SDK 12.2 Guide (published 2024-03-08). <https://docs.nvidia.com/video-technologies/video-codec-sdk/12.2/nvenc-video-encoder-api-prog-guide/index.html>
3. NVIDIA NVENC Interface guide v07 (2016-11-18), older lookahead (up to 32) / 2-pass naming. <https://developer.nvidia.com/downloads/nvenc-programming-guide>
4. `nvEncodeAPI.h` (NVIDIA video-sdk-samples, master). <https://github.com/NVIDIA/video-sdk-samples/blob/master/Samples/NvCodec/NvEncoder/nvEncodeAPI.h>
5. OBS Advanced NVENC Options (31.0), 2024-08-21 — plain-language field list. <https://obsproject.com/kb/advanced-nvenc-options>
6. FFmpeg `libavcodec/nvenc.c` (7.1 source, generated 2025-08-22) — VBV default, lookahead bound, CQ mode. <https://ffmpeg.org/doxygen/7.1/nvenc_8c_source.html>
7. FFmpeg devel patches on CBR filler consistency, 2024-10-28 and 2025-03-30/31. <https://ffmpeg.org/pipermail/ffmpeg-devel/2025-March/341700.html>
8. NVIDIA Developer Forum: "NVENC CBR bitrate way below configured average bitrate", 2020-06-08…15. <https://forums.developer.nvidia.com/t/nvenc-cbr-bitrate-way-below-configured-average-bitrate/126787>
9. NVIDIA Developer Forum: "How to increase the bitrate of video of still image using h264_nvenc", 2017-01-19. <https://forums.developer.nvidia.com/t/how-to-inclease-the-bitrate-of-video-of-still-image-using-h264-nvenc-encode/47400>
10. NVIDIA Developer Forum: "Nvenc ffmpeg plugin CBR options", 2019-09-13 (filler/muxer discussion). <https://forums.developer.nvidia.com/t/nvenc-ffmpeg-plugin-cbr-options/81654>
11. NVIDIA Developer Forum: "Constant Bitrate help", 2016-10-25…11-04. <https://forums.developer.nvidia.com/t/constant-bitrate-help/45736>
12. NVIDIA Developer Forum: "Why buffer frames in low-latency scenarios?", 2017-02-02 (3-frame pipeline explanation). <https://forums.developer.nvidia.com/t/why-buffer-frames-in-low-latency-scenarios/47656>
13. US 7,535,959 B2 (NVIDIA, 2009-05-19). <https://patents.google.com/patent/US7535959B2/en>
14. US 10,462,473 B2 (ATI/NVIDIA lineage, 2019-10-29). <https://patents.google.com/patent/US10462473B2/en>
15. US 8,897,365 B2 (NVIDIA, 2014-11-25). <https://patents.google.com/patent/US8897365B2/en>
16. US 8,798,140 B2 (NVIDIA, 2014-08-05). <https://patents.google.com/patent/US8798140B2/en>
17. Jan Ozer, SME 2019 "Benchmarking FFmpeg's Hardware Codecs", 2019-05-07 (1080p60 ladder, 1 s vs 2 s VBV, x264 comparison). <https://streaminglearningcenter.com/wp-content/uploads/2019/05/SME-2019-FFmpeg-Hardware.pdf>
18. NVIDIA "Video Benchmark Assumptions" / Ada update, 2022-03-03 / 2023-07-24. <https://developer.download.nvidia.com/designworks/video-codec-sdk/Video-Benchmark-Ada-July-2023.pdf>
19. NVIDIA Technical Blog, "Turing H.264 Video Encoding Speed and Quality", 2019-02-13. <https://developer.nvidia.com/blog/turing-h264-video-encoding-speed-and-quality>
20. StreamFX NVENC wiki — multipass throughput measurements. <https://github.com/Vhonowslend/StreamFX-Public/wiki/Encoder-FFmpeg-NVENC>
21. Weak/secondary 2026 comparisons (directional only): <https://streamersize.com/blog/nvenc-vs-x264-streaming>, <https://www.forasoft.com/learn/video-quality/articles-vqm/encoder-comparison-x264-x265-svt-av1>.
22. Lab context (read only): `shaders/scan.comp`, `src/main.cpp:207-229`, `notes/FRAME_PARALLEL.md`, `notes/PARITY_ROADMAP.md`, `docs/STATUS.md` in `E:\__PROJECTS__\Vk264`.
