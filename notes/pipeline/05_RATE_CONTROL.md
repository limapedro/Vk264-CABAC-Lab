# 05 — RATE CONTROL / BIT ALLOCATION stage breakdown and literature survey

**Scope.** Research only. Nothing in `E:\__PROJECTS__\Vk264` or elsewhere in this lab was modified.
All web material below is treated as untrusted data (numbers and claims only; no instructions
were followed). x264 source line numbers are from pinned commit
`90a61ec76424778c050524f682a33f115024be96` (`encoder/ratecontrol.c`, `encoder/slicetype.c`);
`common/base.c` line numbers are from `master` (defaults block is stable across these revisions).

---

## 0. Ground truth and the exact failure mode

Source: `E:\__PROJECTS__\Vk264\docs\improvement_roadmap.md` (HEAD `d24dc87`),
`docs\quality_baseline.md`, and the task brief.

| Fact | Exact number | Source |
|---|---|---|
| 1080p60 desktop capture (`obs_new_src.yuv`, 300f, `--gop 120`) ABR @6000 | **20.93 Mb/s, VMAF 96.98** | roadmap:37 |
| Same content, fixed-QP ceiling (QP28, `wx264-medium`) | **7646.8 kb/s, VMAF 92.372** | roadmap:137–138 |
| Task brief's matched ceiling figure | **QP28 = 7.65 Mb/s / VMAF 93.66** | task brief |
| x264-medium @6000 on that clip | **6.22 Mb/s, VMAF 93.39** | roadmap:37 |
| Frame-size bimodality | **~25 % of frames are 4× `aim`; the rest ~1/20 of it** (30→60 fps duplicated capture) | roadmap:68 |
| Adaptive-floor variants | **13 variants, all reverted**; best (v6) held ±5 % only on isolated cases and cost >10 VMAF on `yt_forza`/`porsche` | roadmap:68 |
| `qcomp` feedforward | **12 arms failed rate** on 6 masters | roadmap:71 |
| PR #505 debt clamp | `correction = clamp(bitDebt/(fps/2), -allocation/4, +allocation/4)`; VMAF 95.719→96.149 @6000 game clip | quality_baseline:239–263 |
| PR #516 floor | QP floor 18 at ≥50 fps: 23.75→5.86 Mb/s at VMAF 97.40→96.69 (easy content) | roadmap:129, scan.comp:57–61 |
| GOP | 5 s (125f @25fps, 300f @60fps) | presets.hpp:64–68 |
| 60 fps game ceiling vs ABR | ABR 88.70 VMAF @6938 kb/s; fixed QP30 91.57 @6121 kb/s → **perfect ABR worth +2.9 VMAF** | quality_baseline:330–332 |
| 60 fps trailer ceiling vs ABR | ABR 72.30 VMAF @7472 kb/s; fixed QP40 80.39 @5872 kb/s → **perfect ABR worth +8.1 VMAF** | quality_baseline:288–300 |

**Diagnosis.** Vk264's controller is *reactive and per-frame*: it measures `actual` bits after the
frame, forms `rateRatio = actual/aim`, and steps QP by
`clamp(round(3·log2(rateRatio)), -2, +2)` (`shaders/scan.comp:48–63`). On a source whose frame
costs are bimodal (four-fold on duplicated cadence, 1/20 otherwise), `log2(rateRatio)` alternates
between ≈+2 and ≈−4 every frame. The ±2 step and ±allocation/4 debt clamp can only smear this
oscillation; they cannot represent the underlying bimodal demand. x264 never tries: it
**pre-analyses complexity, allocates bits globally, then encodes** (ABR uses a half-resolution
lookahead, 2-pass uses pass-1 stats; both divide bits by a *smoothed* complexity exponent before
any feedback). The missing piece is not a better feedback gain — it is a **plan made before the
encode loop**, which our local-file stack can afford because all frames are already resident
(`src/main.cpp:202–224` reads the whole source before `EncoderGpu` is constructed at :226).

---

## 1. Stage breakdown: x264 `file:line` vs Vk264 counterpart

### 1.1 Summary table

| # | Stage | x264 (`90a61ec`) | Vk264 counterpart | Gap / consequence |
|---|---|---|---|---|
| A | Frame-size / R–Q model | `ratecontrol.c:202–215` `qp2qscale`, `qscale2qp`, `qscale2bits` | `scan.comp:51` `actual = 32·total` (post-hoc word count); no predictive model | Vk264 has no `bits(q)` model, so it cannot invert a target into a QP |
| B | 1-pass ABR complexity EWMA | `ratecontrol.c:2538–2544` `short_term_cplxsum *= 0.5`, `count *= 0.5`, `blurred_complexity = sum/count` from half-res SATD (`x264_rc_analyse_slice`, :2537) | none (feedback only, `scan.comp:54–56`) | EWMA spans ~2 frames → tracks the 30→60 bimodality instead of averaging it |
| C | qcomp allocation | `ratecontrol.c:1989–2000` `q = pow(blurred_complexity, 1-qcompress) / rate_factor`; `base.c:423` `qcompress = 0.6` | no qcomp; `scan.comp:50` fixed `aim` with 0.5×–1.5× clamp | 12 fixed-exponent qcomp arms already failed (roadmap:71) because they lacked calibration |
| D | Long-term rate correction | `ratecontrol.c:2491–2497` `q *= pow(total_bits/expected_bits_sum, w)`, `w = clip(cur_time·100,0,1)`; short-term `abr_buffer` :2457, :2488–2490 | `scan.comp:52` `bitDebt` clamped to ±30·budget, corrected at `debt/(fps/2)` | Vk264 debt window is 30 frames *behind*; x264's long-term term uses all elapsed frames |
| E | MB-tree QP offsets | `slicetype.c:1032–1042`: `strength = 5·(1-qcompress)`; `qp_offset = f_qp_offset_aq − strength·(log2(intra+propagate) − log2(intra) + weightdelta)`; propagation `slicetype.c:1047–1078`; `ratecontrol.c:760–766` MB-tree forces `qcompress=1`, `pb_factor=1`; `ratecontrol.c:1994–1998` duration-based allocation | none (roadmap:11, ablation value −0.166 VMAF) | needs B-frames/DPB; excluded by ABI constraint |
| F | AQ / per-MB offsets | `ratecontrol.c:379–401` variance AQ `qp_adj = strength·(log2(ac_energy) − (14.427+2·(bitdepth−8)))`; `base.c:415–416` aq-mode 1, strength 1.0 | `aq` field exists (`gpu.hpp:20`) but AQ/psy-RD measured negative and reverted | do not retry (roadmap:79, quality_baseline:100,143) |
| G | Lookahead slice types | `base.c:417` `i_lookahead = 40`; half-res SATD `slicetype.c:35,50–52,511–...`; `slicetype.c:1741` `x264_slicetype_decide`, :1469 `x264_slicetype_analyse` | none; I/P fixed by `--gop`, `gpu.hpp:16–17` | Vk264 can emulate *complexity* lookahead from preloaded frames but not B-type decisions (no dual-ref ABI) |
| H | Scenecut | `slicetype.c:1380–1426` `scenecut_internal`, `scenecut` with flash suppression; :1519 threshold test; :1534 forces IDR | none; IDR strictly every `gop` (`presets.hpp:68`) | PR #505 showed a cut costs ~596 kB ≈20× per-frame budget; Vk264 repays it over many frames |
| I | VBV | `ratecontrol.c:2502–2517` lookahead VBV raise-QP; per-row `ratecontrol.c:1601–...` `x264_ratecontrol_mb` with row predictors and re-encode; `base.c:417` lookahead | `vbv=0` (quality_baseline:39); no VBV | local-file stack does not need VBV; do not add |
| J | Per-frame QP / zone override | `x264.c` `parse_qpfile` (`framenum type QP`); `ratecontrol.c:1216–1299` `parse_zone/parse_zones/get_zone`, :1502–1508 & :2012–2018 apply forced QP / bitrate factor; `--qpfile`/`--zones` documented in x264 `--fullhelp` | no field; `gpu.hpp:14–23` `FrameConfig` has no `plannedQp`; `layout.glsl:55–57` derives QP solely from `interQpOffset`/`idrQpOffset` | one `uint32` field + one host planner closes this gap with **no bitstream/ABI change** |

### 1.2 Key formulas, exact

- `qp2qscale(qp) = 0.85·2^((qp−12−QP_BD_OFFSET)/6)` — `ratecontrol.c:202–204`
- `bits(q) = (tex_bits+0.1)·(q_ref/q)^1.1 + mv_bits·(max(q_ref,1)/max(q,1))^0.5 + misc_bits` — `ratecontrol.c:215–...`
- 1-pass ABR EWMA: `short_term_cplxsum = 0.5·sum + satd/duration; count = 0.5·count + 1; blurred_complexity = sum/count` — `ratecontrol.c:2538–2544`
- Allocation: `q = pow(blurred_complexity, 1−qcompress)/rate_factor`, `rate_factor = wanted_bits_window/cplxr_sum` — `ratecontrol.c:2555–2559`, `1989–2000`
- MB-tree: `qp_offset = aq_offset − 5·(1−qcompress)·log2((intra+propagate)/intra)` — `slicetype.c:1032–1042`
- Vk264: `aim = clamp(allocation − clamp(bitDebt/(fps/2), ±allocation/4), allocation/2, 1.5·allocation)`,
  `step = clamp(round(3·log2(max(1,actual)/aim)), −2, +2)`,
  `qp = clamp(qp+step, fps>=50 ? 18 : 0, 51)` — `scan.comp:48–63`

**Takeaway for the design.** x264's quality comes from *division by measured complexity before
encoding*, not from tightening the feedback loop. Vk264's 13 failed floor variants and 12 failed
qcomp arms are consistent with that: both tried to fix a planning problem inside a controller that
has no plan.

---

## 2. Literature survey (2020–2026) with URLs, dates and measured gains

### 2.1 GPU / parallel-encoder rate control and lookahead-free allocation

| Work | Date / venue | Measured gain | URL | Relevance |
|---|---|---|---|---|
| *Rate Estimation Techniques For Encoder Parallelization* (Chaudhari, Chuang, Koba, Lalgudi, NVIDIA) | 2021-09-21, ICIP | Rate-control model accounts for internal pipeline delay; "improved accuracy" and improved bits-per-frame planning using full first-pass statistics (no numeric BD-rate given in abstract) | https://resourcecenter.ieee.org/conferences/icip-2021/spsicip21vid331 | Directly validates **plan before parallel encode**; our 4-row P-slice pipeline has a similar multi-frame delay |
| *Recent Advances in Rate Control: From Optimisation to Implementation and Beyond* (Wei et al.) | 2022-05-22, arXiv / IEEE TCSVT DOI 10.1109/TCSVT.2023.3287561 | Survey; notes frame-level parallelism limited by GOP and increases latency; slice-level RC almost unused; λ-domain and R–λ parallel RC schemes | https://arxiv.org/abs/2205.10815 | Confirms no mainstream RC uses slice-parallel feedback; plan-then-encode is the compatible route |
| NVIDIA Video Codec SDK 13.0, §3.8.3–3.8.4, §6.4, §8.2 | current (accessed 2026-11) | NVENC uses firmware RC; 2-pass-per-frame (quarter- or full-res) + lookahead ≤32 frames improve target adherence; 2-pass/CUDA lookahead cost is real but bounded | https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html | Industry precedent for a cheap first analysis pass; our GTX 1660 SUPER's NVENC P5 reaches only 91.77 VMAF @6000, so we should not copy its RC |
| *New Video Creation and Streaming Features Accelerated by the NVIDIA Video Codec SDK* | 2023-05-24 | "External look ahead" API exposes lookahead scaling (default 1/16 resolution); row/block QP and bit-count stats available | https://developer.nvidia.com/blog/new-video-creation-and-streaming-features-accelerated-by-the-nvidia-video-codec-sdk/ | 1/16-res analysis is exactly our proposed 16×16-downsample complexity pass |
| *All-intra rate control using low complexity video features for VVC* (Helmrich et al., Fraunhofer HHI) | 2023-06-29, arXiv | **First pass 0.40 fps → 40+ fps (100×); overall encode time −32.17 %; +2 % BD-rate; rate deviation ≈0**; Random Forest on 6 DCT-energy features (VCA) maps features→bits at each QP | https://arxiv.org/html/2306.16786 | Strongest precedent for a cheap analysis pass replacing a real encode. Our analogue: host-side SAD/variance features → bits/frame model |
| *A Scene Change and Noise Aware Rate Control Method for VVenC* | (Fraunhofer HHI, published ~2021–22) | Scene-cut frame-type adaptation + noise estimation stabilises RC on mixed material; five QP limiters `clipTargetQP()` | http://www.ecodis.de/video/helmrich_1prc_rev2.pdf | Our bimodal capture is "mixed material"; motivates duplicate/scene-aware QP pinning |
| *Finalization of VVenC's Screen Content Detector and Rate Control* | (Fraunhofer HHI) | Reuses MCTPF pre-analysis statistics for both screen-content detection and 2-pass RC | http://www.ecodis.de/video/helmrich_1prc_scc.pdf | Screen content ≈ desktop capture; shows one analysis pass can serve several decisions |

### 2.2 Lookahead-free / cheap bit allocation

| Work | Date / venue | Measured gain | URL | Relevance |
|---|---|---|---|---|
| *Bit Allocation using Optimization* (Xu et al.) | 2023, ICML (PMLR v202) | Notes feed-forward QP prediction (Li et al., ICASSP 2022) and fixed-λ heuristics; frames coded with learned allocation beat coarse heuristics | https://proceedings.mlr.press/v202/xu23c/xu23c.pdf | Confirms feed-forward QP is a recognised alternative to reactive RC |
| *Action-Constrained RL for Frame-Level Bit Allocation in HEVC through Frank-Wolfe Policy Optimization* | 2022-03-10, arXiv | VMAF-targeted NFWPO "comparable rate-distortion performance to 2-pass ABR of x265" | https://arxiv.org/abs/2203.05127 | Learning route exists but needs training; our deferred `LEARNED_MODEL_DESIGN.md` is the cheaper version |
| *Optimize x264/x265 Rate Control: lookahead in frame bit allocation and slice type decision* (Liu et al.) | 2018-12-17 (older baseline but foundational) | x265 2.4 medium: **+0.617 dB up to +1.705 dB BD-PSNR, +4.2 % rate accuracy, −1.13 % encode time** by using lookahead complexity changes and data dependencies for base QP | https://pubmed.ncbi.nlm.nih.gov/30571636/ | Directly relevant: base QP from *current+future* complexity, not history |
| *x264 doc/ratecontrol.txt* (Loren Merritt) | historical, still authoritative | 2-pass default allocation `complexity^0.6`; 1-pass ABR uses half-res SATD and past-only scaling; "overflow compensation tuned anywhere from near-2pass quality (±10 % size) to strict size/lower quality" | https://github.com/jpsdr/x264/blob/master/doc/ratecontrol.txt | Exact statement of why our reactive controller is the weak member |

### 2.3 ROI / perceptual (VMAF-driven) QP allocation

| Work | Date / venue | Measured gain | URL | Relevance |
|---|---|---|---|---|
| *HEVC VMAF-oriented Perceptual RDO using CNN* | 2021-06-01, Picture Coding Symposium | **−2.80 % VMAF-based BD-rate** vs HEVC anchor | https://doi.org/10.1109/pcs50896.2021.9477459 | Block-level; not transferable without new syntax, but supports frame-level VMAF weighting |
| *VMAF Oriented End-to-End Quality Prediction Model for Perceptual VVC* | 2024-10-17, ATC | Predicted perceptual quality feeds RD optimisation to build QP map; lower fluctuation at fewer bits (no single BD number) | https://doi.org/10.1109/atc63255.2024.10908127 | Concept only |
| *Perceptual Quality-Oriented Rate Allocation via Distillation from End-to-End Image Compression* (Yang et al.) | 2024-04-25, ACM TOMM 20(7), art.198 | **>10 % BD-rate** vs VTM/HPM anchors under MS-SSIM/LPIPS; importance map = ratio of perceptual vs MSE latent rate maps, converted to QP by R–λ model | https://dl.acm.org/doi/10.1145/3650034 | Distillation idea could give a frame-level importance prior; too heavy for our 100 fps gate |
| *A Hybrid Contrast and Texture Masking Model … HEVC* | 2024-08-22, Electronics 13(16):3341 | **2.5–4.48 % BD-rate** (contrast), **5.79 % combined** with SVM texture-masked QP offsets | https://www.mdpi.com/2079-9292/13/16/3341 | Per-CU DQP; we cannot signal per-MB QP, so only frame-aggregate use |
| *Joint Frame-Level and Block-Level Rate-Perception Optimized Preprocessing* | 2024-12-28, ACM MM Asia '24 | **−12.43 % BD-rate on VMAF** (vs −7.46 % for Kang, −0.33 % MCTF), −1.79 % on SSIM; VVenC 1.10, 4 QPs, IPPP GOP8 | http://dl.acm.org/doi/10.1145/3696409.3700226 | Preprocessing changes source pixels; out of scope unless we accept non-conformant source filtering (we do not) |
| *Consistent Quality in Video Coding: Perceptual RDO and Neural Network-Based QP Prediction* | 2025, IEEE Access | VMAF-QP network + RDO; **4.5 % bitrate reduction** vs HEVC at matched quality, smaller quality fluctuation | https://doi.org/10.1109/access.2025.3574303 | Closest to our desired frame-level "QP for a target VMAF" mapping |
| *VVC adaptive QP offset algorithm based on visual perception* | 2023-10-20, ICSIPC | **0.74 % VMAF-BD-rate gain, ~8 % VMAF score improvement** | https://doi.org/10.1117/12.3005138 | Cheap HVS-based QP offset; possible frame-level simplification |
| *Bit Allocation Transfer for Perceptual Quality Enhancement of Traditional Video Codecs* | 2026-08 (arXiv 2608.07799), accessed 2026-11 | QP-map generation measured **0.2 s/frame on GTX 1080 Ti, 5.3 s on CPU**; VVC encode >1 min/frame | https://arxiv.org/html/2608.07799v1 | Explicitly **too slow** for our 100 fps gate; confirms we must stay frame-level and host-side |

### 2.4 qcomp / MB-tree alternatives and content-adaptive presets

| Work | Date / venue | Measured gain | URL | Relevance |
|---|---|---|---|---|
| *Efficient Video Encoder Autotuning via Offline Bayesian Optimization and Supervised Learning* (Disney Research) | 2024-10-01/02, IEEE MMSP | Search finds **−14.49 % BD-rate (0.77 BD-PSNR) / −11.59 % BD-rate (2.12 BD-VMAF)** vs x264-veryslow; XGBoost/MLP recover **~80 %** (≈−9.2 % BD-rate) with **one fast encode** | https://studios.disneyresearch.com/2024/10/01/efficient-video-encoder-autotuning-via-offline-bayesian-optimization-and-supervised-learning/ ; paper PDF https://studios.disneyresearch.com/app/uploads/2024/09/Efficient-Video-Encoder-Autotuning-via-Offline-Bayesian-Optimization-and-Supervised-Learning-Paper.pdf | Roadmap item #3 already flags this; orthogonal to rate accuracy (preset choice, not bit allocation) |
| *Machine-Learning-Based Method for Content-Adaptive Video Encoding* (MSU) | published ~2023–24, video processing benchmarks | **x264 filesize −17.8 %**, x265 −7.9 % at matched quality; faster selection than competing methods | https://videoprocessing.github.io/ml-method-for-video-encoding | Same family as Disney |
| *Content-Adaptive Encoder Preset Prediction for Adaptive Live Streaming* (CAPS) | 2022-10-19, arXiv | Predicts preset from content + thread/speed budget | https://arxiv.org/abs/2210.10330 | Live focus; our local-file case is easier |
| *Optimal Transcoding Preset Selection for Live Video* | 2024-11 (arXiv 2411.14613) | **+1.5 dB PSNR** over default Twitch config on UGC | https://arxiv.org/pdf/2411.14613 | UGC ≈ desktop capture; but ours is offline |
| *JND-Aware Two-Pass Per-Title Encoding (JTPS)* | 2023-06-28, IEEE TCSVT | **−18.80 % bitrate @PSNR, −32.59 % @VMAF** vs fixed HLS ladder; −68.96 % storage, −18.58 % encode time at JND=6 VMAF | https://doi.org/10.1109/tcsvt.2023.3290725 | Two-pass, low-complexity DCT-energy features, random-forest CRF; directly analogous |

### 2.5 Lightweight complexity models and offline/qpfile planning (local files)

| Work | Date / venue | Measured gain | URL | Relevance |
|---|---|---|---|---|
| Netflix *Dynamic Optimizer* blog | 2018-03-05 (foundational) | **17.1 % bitrate savings vs best fixed-QP** with HVMAF; **22.5 % with PSNR**; per-shot convex hull + constant-slope trellis | https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f | The canonical "offline planning for local files" architecture |
| Netflix *Optimized shot-based encodes for 4K* | 2020-08-28 | **~50 % BD-rate gain** vs fixed ladder over 100 titles; highest 4K point 8 Mb/s vs 16 Mb/s; >65 % fewer rebuffers | https://netflixtechblog.com/optimized-shot-based-encodes-for-4k-now-streaming-47b516b10bbb | Proof that offline per-segment bit allocation is production-grade |
| *Efficient Bitrate Ladder Construction for Content-Optimized Adaptive Streaming* | 2021-02-08, arXiv | **−89.06 % encodes** vs exhaustive, **−61.46 %** vs interpolation, only **+1.78 % BD-rate** | https://arxiv.org/pdf/2102.04550.pdf | Feature→parameter prediction is cheap and accurate enough |
| *VMAF-based Bitrate Ladder Estimation* (Katsenou et al.) | 2021-03-12, PCS | **−77.4 % encodes**, **1.12 % BD-rate cost**, 74.3 % identical ladder points | https://arxiv.org/abs/2103.07564 | Gaussian Process on spatio-temporal features |
| *Unravelling the Power of Single-Pass Look-Ahead … Transcoding Deployment* | 2024-04-08, arXiv | 1-pass ≈ 2-pass quality; 2-pass overhead **~20 % x264, ~18 % x265, 4–9 % SVT-AV1**, ~5 % bitrate saving | https://arxiv.org/abs/2404.05321 | Important: a *full* 2-pass x264 is not needed; a cheap first analysis is enough |
| VMAFx research digest 0044 / ADR-0237 | 2026-05-02 / 2026-05-05 | Per-shot CRF bisect ±0.5 VMAF in ≤5 encodes; explicit **x264 `--qpfile`** as Phase-D output format; notes qpfile overrides VBV/CBR and can conflict with RC | https://github.com/VMAFx/vmafx/blob/master/docs/research/0044-quality-aware-encode-automation.md ; https://github.com/VMAFx/vmafx/blob/master/docs/adr/0312-ffmpeg-patches-vmaf-tune-integration.md | Confirms qpfile-style per-frame plans are a standard offline deliverable; our encoder is under our control so the libx264 wrapper blocker in `LEARNED_DECISIONS_STATUS.md:23–25` does not apply |
| VMAFx `tune-per-shot` docs | current (accessed 2026-11) | Shot-split + per-shot VMAF bisect + concat plan JSON; explicitly emits a plan, not encodes | https://vmafx.dev/usage/vmaf-tune-per-shot/ | Plan file format to imitate (we need only frame index + QP) |
| x264 `--qpfile` / `--zones` reference | x264 core 160, current | `qpfile`: `framenum frametype QP`; `zones`: `start,end,q=N` or `b=factor`; QP forced via `pic->i_qpplus1`, zones via `get_qscale()` | https://dev.beandog.org/x264_fullhelp.html ; https://github.com/mirror/x264/blob/90a61ec76424778c050524f682a33f115024be96/encoder/ratecontrol.c | Exact semantics to reproduce host-side |

**Survey conclusion.** Three results matter for us: (1) a 100× cheaper first pass is achievable
with ~6 low-complexity features and gives ~2 % BD-rate cost (VVenC paper); (2) offline shot/plan
optimisation yields 10–50 % bitrate gains and is the normal path for *local files*; (3) perceptual
block-level methods (VMAF-RDO, ROI, distillation) give 0.7–12 % gains but require per-block syntax
we do not have — only their frame-level aggregate is transferable.

---

## 3. Proposed OFFLINE plan-then-encode architecture (local-file stack)

### 3.1 Dataflow

```
preloaded YUV (main.cpp:204, 933 MB for 300f 1080p)
   │
   ├─ Pass 0a  HOST: 16×16-downsample per frame → c_i = {SAD(i,i−1), SAD(i,i+1), SAD(i,i+2),
   │                 luma var, edge energy, chroma var, duplicate flag, scene-cut flag}
   │           cost ~0.03–0.3 s total for 300f (≈7.3 M byte diffs), no GPU work
   │
   ├─ Pass 0b  HOST: 1–3 fixed-QP probe encodes on a 30–60f subsample (or reuse the existing
   │           encode loop with `--qp`), collect measured bits/frame b_i(q0).
   │           cost 0.6–3 s at ~99 fps; optional, can be skipped for the cheap variant (§4.3)
   │
   ├─ Planner  HOST: fit a_i from b_i(q0) via bits_i(q) = a_i·2^(−(q−q0)/6·1.1) + hdr_i;
   │           binary-search global λ so Σ_i bits_i(q_i) = target·frames;
   │           q_i = clip(argmin_q [bits_i(q) − λ·c_i^α], 18, 51) with α≈0.6 only as a
   │           *smoothing prior*, never as the sole allocator; IDR reserve = 5 s GOP logic
   │           from scan.comp:33–41; cost <1 ms for 300f
   │
   └─ Pass 2   GPU encode: plannedQp pushed in FrameConfig; scan.comp keeps ±allocation/4
               debt clamp as a slow safety net only (no ±2 reactive step)
```

### 3.2 Why this fixes the specific failure

- The plan is made with **knowledge of i+1 and i+2** (all frames preloaded), so a 4× frame is
  assigned a low QP *before* it is coded and a duplicate twin inherits the same QP instead of
  triggering a −4 `log2(rateRatio)` step. This is the x265 lookahead result (+0.617 to +1.705 dB
  BD-PSNR) without B-frames.
- Global λ enforces the **sequence total**, replacing the ±30-frame debt window
  (`scan.comp:52`). The bimodality sums linearly, so water-filling handles it exactly.
- `q_i` is clamped to the already-accepted floor 18 (`scan.comp:61`), so easy content cannot
  waste 4–6× bitrate (PR #516 behaviour is preserved).
- No CABAC, no B-frames, no new DPB/ABI: the only interface change is one `uint32 plannedQp`
  in `FrameConfig` (`src/gpu.hpp:17`) consumed by `layout.glsl:55–57`; `RateState`
  (`layout.glsl:24`) is unchanged. This is legal H.264 — QP is signalled per slice by
  `slice_qp_delta` (the existing path already does this via `interQpOffset`).

### 3.3 Cost and quality estimates

| Item | Estimate | Basis |
|---|---|---|
| Pass 0a feature extraction | **0.03–0.3 s / 300f** | 120×68 downsample ≈8.2k px ×3 refs ×300 = 7.3 M abs-diff ops; host SIMD ≥1 GB/s; or reuse 1.09 ms spatial-SAD shader (`quality_baseline:126`) → ~0.33 s |
| Pass 0b probe encode | **0.6 s (60f subsample) to 3 s (300f)** @ 99 fps process | roadmap:18–21 |
| Planner (λ search) | **<1 ms** | 300 frames × ~20 λ iterations, scalar |
| Pass 2 encode | **unchanged: 132–140 fps loop, 93–103 fps process** | roadmap:15–21 |
| Total added wall time | **0.1–3.4 s per 300f clip (5 s of content)** | sum above; acceptable for local files, loop gate unaffected |
| Rate accuracy | **target ±5 %** expected (acceptance gate roadmap:107–110) | water-fill solves total directly; x264's own 2-pass achieves this |
| `obs_new_src` @6000 | 20.93 Mb/s → **≈6.0 Mb/s**, VMAF ≈92.0–92.4 (fixed-QP ceiling; x264 93.39) | roadmap:37 and :137; the plan cannot exceed the codec's fixed-QP ceiling |
| 60 fps game clip @6000 | 88.70 @6938 kb/s → **≈91.6 @6121 kb/s (+2.9 VMAF)** | quality_baseline:330–332 |
| 60 fps trailer @6000 | 72.30 @7472 kb/s → **≈80.4 @5872 kb/s (+8.1 VMAF)** | quality_baseline:288–300 |
| Residual quality gap | ~1.2–1.5 VMAF vs x264 at matched rate (game/obs content) | structural: CABAC/B-frames/trellis/MB-tree absent (roadmap:135–142) |

**Honest framing:** on `obs_new_src` the "gain" is primarily *rate legality* (21→6 Mb/s); VMAF
numerically falls because we stop spending 3.5× the target. The correct comparison is at matched
rate: the plan lands at the fixed-QP ceiling instead of the controller's pathological point.

### 3.4 Risks and mitigations

1. **QP plan ignores encoder feedback** → measured bits differ from model. Mitigation: keep the
   debt clamp and apply at most ±1 QP correction per frame; re-fit `a_i` multiplicatively from the
   running bit total (the VVenC "following frames compensate" rule).
2. **Duplicate frames get same QP but still cost bits** → they are already ~1/20 aim; pinning the
   QP prevents the oscillation, it does not need to zero their cost.
3. **Scene cuts** → detect in Pass 0a (SAD(i,i−1) spike) and reserve an IDR-like allocation; this is
   the PR #505 lesson applied *before* the cut rather than after it.
4. **QP floor 18 conflict** → clamp plan to [18,51] at ≥50 fps; the plan then cannot cause the
   easy-content overshoot that PR #516 fixed.
5. **VMAF-version drift** → roadmap:160–164 warns Netflix VMAF v1 (June 2026) can move scores
   1–3 points; always compare plan vs fixed-QP on the same harness run.

---

## 4. Ranked top 3 (exact implementation sketch + measurement gate)

All three are **new**; none appears in the rejected lists of `quality_baseline.md:91–104,132–166`
or `improvement_roadmap.md:89–95`. Explicitly excluded: AQ/psy-RD, qpfile-exponent sweeps
(12 arms), larger debt windows, asymmetric clamps, QP ceilings 30/38, I4x4, 8×8dct, chroma-QP
offset, ipratio offset, SATD finalists, older-ref subpel, MV penalty, CABAC, B-frames/MB-tree,
content-adaptive preset as a *primary* RC fix (roadmap #3 is orthogonal).

### Rank 1 — Probe-calibrated per-frame QP plan (the architecture of §3)

**Implementation sketch (files/lines, no code):**
1. Add `plannedQp` (`uint32`, 0 = "unused") to `FrameConfig` at `src/gpu.hpp:17` and to the CLI
   option parser near `src/main.cpp:124–137`.
2. After the preload at `src/main.cpp:202–224` and before `EncoderGpu gpu(config, …)` at :226,
   call a new host-only planner module. Its inputs: the resident source buffer, `config.width`,
   `config.mbWidth/mbHeight`, `config.gop` (:68), `config.fps`, `config.targetBits`, floor 18.
3. Pass 0a: for each frame build the 120×68 luma thumbnail; compute SAD to i−1, i+1, i+2, variance
   and edge energy; flag duplicates (SAD≈0) and cuts (SAD spike >4× median).
4. Pass 0b: run 1–3 fixed-QP probe encodes on a 60-frame strided subsample, harvest per-frame
   `bytes` from `EncodedFrame` (`gpu.hpp:25–30`) at q0 ∈ {24,28,32}.
5. Fit `a_i`, `hdr_i`; solve `Σ bits_i(q_i) = targetBits·frameCount` by bisection on λ; enforce
   IDR reserve with the existing `idrExtra` rule (`scan.comp:33–41`); write `plannedQp[i]`.
6. In `layout.glsl:55–57`, if `plannedQp != 0` return it (plus the IDR offset) instead of the
   reactive offset; in `scan.comp:48–63`, replace the `log2(rateRatio)` step with a ±1 slow
   drift correction, keeping the ±allocation/4 debt clamp and floor 18.
7. Bitstream unchanged (QP still signalled through the existing `slice_qp_delta` path);
   OBS ABI unchanged (`obs/obs-vk264.cpp:94–109` needs no new field for ABR mode).

**Measurement gate:** on `obs_new_src.yuv` and `native1080_300.yuv`, 300f, `--bitrate 6000/8000`,
`--fps 60/25`: achieved rate within **±5 %**; VMAF ≥ **fixed-QP ceiling −0.3** at that rate;
encode loop **≥100 fps**, process **≥85 fps**; `ctest` **45/46**, `check.py` **13/13**,
`ffmpeg -err_detect explode` clean. Stretch: game/trailer 60f recovers ≥+2.9/+8.1 VMAF.

### Rank 2 — Duplicate-cadence QP pinning (minimal, targets the root cause)

**Implementation sketch:**
1. Pass 0a only (no probe encode): in the preloaded buffer, test each frame against i−1 at
   16×16 downsample. If `SAD(i,i−1) < ε`, mark `duplicate[i] = true` and set the twin's planned QP
   equal to `QP[i−1]`.
2. For duplicated frames, `scan.comp` bypasses the `rateRatio` step entirely and does **not**
   add `actual−allocation` to `bitDebt` (`scan.comp:52`); the frame is charged against its twin's
   allocation. Non-duplicated frames continue with the existing controller and floor-18 clamp.
3. For 30→60 fps duplicated capture this removes ~25 % of the oscillating feedback samples and
   stops the 4×/1/20 alternation from driving the other 75 % to the floor.
4. Zero new struct fields are strictly required if the duplicate map is stored in a host-side
   side-channel keyed by `frameIndex` and passed via the same `plannedQp` slot as Rank 1.

**Measurement gate:** rate on `obs_new_src` @6000 within **±5 %** (from 20.93 Mb/s), no >2 VMAF
regression on the 25 fps game master or `yt_forza`/`porsche`; same speed/decode gates as Rank 1.
Decision rule: if Rank 1 misses the rate gate, ship this as the cheap fallback; if both pass,
Rank 1 wins on generalisation.

### Rank 3 — Cheap `complexity^0.6` water-fill with `cplxblur` (no probe encode)

**Implementation sketch:**
1. Reuse Pass 0a features; define `c_i = 0.5·SAD(i,i+1) + 0.25·SAD(i,i−1) + 0.25·var_i`
   (lookahead-weighted, per the x265 Liu et al. result).
2. Smooth `c_i` over ±20 frames (`cplxblur=20` from `base.c:425`) to suppress the 30→60 cadence;
   set target bits `t_i ∝ c_i^0.6` (x264 `doc/ratecontrol.txt` and `ratecontrol.c:2000`),
   renormalise to the GOP budget, and invert `bits(q)` from `ratecontrol.c:215` analytically
   (no probe encode, no trained model).
3. Use `α` as a *prior* blended 50/50 with equal allocation; this is the specific thing the 12
   failed fixed-exponent arms lacked (they used a pure exponent with no calibration and no blur).
4. Same `plannedQp` path as Rank 1; keep ±1 feedback safety.

**Measurement gate:** rate within **±8 %** on four masters @6000; VMAF ≥ fixed-QP ceiling −0.5;
loop ≥100 fps, process ≥85 fps. If it misses ±5 %, fall back to Rank 1's probe calibration.

---

## 5. Reference list (all URLs cited above, with dates)

- x264 `ratecontrol.c` pinned commit: https://github.com/mirror/x264/blob/90a61ec76424778c050524f682a33f115024be96/encoder/ratecontrol.c
- x264 `doc/ratecontrol.txt`: https://github.com/jpsdr/x264/blob/master/doc/ratecontrol.txt
- x264 `--fullhelp` (qpfile/zones/qcomp defaults): https://dev.beandog.org/x264_fullhelp.html
- MB-tree paper (x264 lookahead/MB-tree algorithm): https://huyunf.github.io/blogs/2017/12/06/x264_slice_type_decision/MBtree%20paper.pdf
- Chaudhari et al., rate estimation for encoder parallelization, 2021-09-21: https://resourcecenter.ieee.org/conferences/icip-2021/spsicip21vid331
- Wei et al., rate-control survey, 2022-05-22: https://arxiv.org/abs/2205.10815
- NVIDIA Video Codec SDK 13.0 guide: https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/index.html
- NVIDIA SDK blog, 2023-05-24: https://developer.nvidia.com/blog/new-video-creation-and-streaming-features-accelerated-by-the-nvidia-video-codec-sdk/
- Helmrich et al., all-intra low-complexity first pass, 2023-06-29: https://arxiv.org/html/2306.16786
- Helmrich et al., scene-change/noise-aware VVenC RC: http://www.ecodis.de/video/helmrich_1prc_rev2.pdf
- Helmrich et al., VVenC SCC + RC: http://www.ecodis.de/video/helmrich_1prc_scc.pdf
- Liu et al., x265 lookahead RC, 2018-12-17: https://pubmed.ncbi.nlm.nih.gov/30571636/
- Xu et al., bit allocation optimisation, 2023: https://proceedings.mlr.press/v202/xu23c/xu23c.pdf
- NFWPO frame-level bit allocation, 2022-03-10: https://arxiv.org/abs/2203.05127
- VMAF-oriented perceptual VVC, 2024-10-17: https://doi.org/10.1109/atc63255.2024.10908127
- Yang et al., perceptual rate allocation distillation, 2024-04-25: https://dl.acm.org/doi/10.1145/3650034
- HEVC VMAF RDO CNN, 2021-06-01: https://doi.org/10.1109/pcs50896.2021.9477459
- Contrast/texture masking, 2024-08-22: https://www.mdpi.com/2079-9292/13/16/3341
- Joint frame/block rate-perception preprocessing, 2024-12-28: http://dl.acm.org/doi/10.1145/3696409.3700226
- Consistent quality / VMAF-QP network, 2025: https://doi.org/10.1109/access.2025.3574303
- VVC adaptive QP offset based on visual perception, 2023-10-20: https://doi.org/10.1117/12.3005138
- Bit allocation transfer, 2026-08: https://arxiv.org/html/2608.07799v1
- Disney encoder autotuning, 2024-10-01: https://studios.disneyresearch.com/2024/10/01/efficient-video-encoder-autotuning-via-offline-bayesian-optimization-and-supervised-learning/
- MSU content-adaptive encoding: https://videoprocessing.github.io/ml-method-for-video-encoding
- CAPS, 2022-10-19: https://arxiv.org/abs/2210.10330
- Optimal transcoding preset selection, 2024-11: https://arxiv.org/pdf/2411.14613
- JND-aware two-pass per-title, 2023-06-28: https://doi.org/10.1109/tcsvt.2023.3290725
- Netflix Dynamic Optimizer, 2018-03-05: https://netflixtechblog.com/dynamic-optimizer-a-perceptual-video-encoding-optimization-framework-e19f1e3a277f
- Netflix optimized 4K shot-based encodes, 2020-08-28: https://netflixtechblog.com/optimized-shot-based-encodes-for-4k-now-streaming-47b516b10bbb
- Efficient bitrate ladder construction, 2021-02-08: https://arxiv.org/pdf/2102.04550.pdf
- VMAF-based bitrate ladder estimation, 2021-03-12: https://arxiv.org/abs/2103.07564
- Single-pass vs multi-pass lookahead, 2024-04-08: https://arxiv.org/abs/2404.05321
- VMAFx research digest 0044, 2026-05-02: https://github.com/VMAFx/vmafx/blob/master/docs/research/0044-quality-aware-encode-automation.md
- VMAFx ADR 0312, 2026-05-05: https://github.com/VMAFx/vmafx/blob/master/docs/adr/0312-ffmpeg-patches-vmaf-tune-integration.md
- VMAFx `tune-per-shot`: https://vmafx.dev/usage/vmaf-tune-per-shot/

**Lab-internal sources:** `E:\__PROJECTS__\Vk264\docs\improvement_roadmap.md`,
`docs\quality_baseline.md`, `E:\__PROJECTS__\Vk264-CABAC-Lab\notes\LEARNED_MODEL_DESIGN.md`,
`notes\LEARNED_DECISIONS_STATUS.md`, and the pinned source files listed in §1.
