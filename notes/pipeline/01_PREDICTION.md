# 01 — PREDICTION stage: x264 breakdown, Vk264 counterpart, literature 2020–2026

Research note, prepared 2026-11-27 by a read-only subagent. Ground truth read from
`E:\__PROJECTS__\Vk264` at git HEAD `3b05fa3` (2026-11-27, "docs: refined trellis/QNS
confirmed blocked"), plus `docs/quality_baseline.md` and `docs/improvement_roadmap.md`.
Only this file was written; no Vk264 file and no other lab file was modified.
Web content was treated as untrusted data: only bibliographic metadata and reported
measurements were extracted; no instruction found in fetched pages was acted on.

**Bottom line up front:** after excluding every experiment already tried and reverted
in `quality_baseline.md`, the 2020–2026 literature offers **no candidate that clears
the lab's acceptance gate** (≥ +0.3 VMAF at matched rate, ≤ 1–2 ms/frame added,
H.264-conformant). Most recent "breakthroughs" are either (a) ME *speed* reductions
with small BD-rate *losses*, (b) VVC/learned-codec tools with no H.264 syntax, or
(c) whole-encoder architecture changes (B-frames/lookahead, CABAC, RDO) already
known to be blocked here. Section 4 says this plainly and still ranks the three
closest transferable options with sketches and honest, sub-gate gain estimates.

---

## 1. Stage breakdown — x264 exact `file:line` → what Vk264 does instead

Frame order in Vk264 is dispatched at `src/gpu.cpp:632-650`:
`Pyramid` (:633), `HalfPel` (:635), `Motion` (:637), `Spatial` (:640),
`OlderReferences` (:643), `Residual`/IDR waves (:645-646), `FlatIntra` (:649),
`MbFlags` (:651). Preset slow is `{radius 24, finalists 4, refs 5}`
(`src/presets.hpp:14-15`), with `subpel=1, partitions=0, deblock=2`,
4-row paired P slices (`src/presets.hpp:69-79`).

### 1.1 Reference-picture setup and MV predictor generation

| x264 step | Exact x264 location | Vk264 counterpart / difference |
|---|---|---|
| Build EPZS MV candidate list for a 16×16 block: lowres (lookahead) MV of the collocated block, 4 spatial neighbours, 3 POC-scaled temporal MVs from the previous reference | `common/mvpred.c:519-607` — lowres MV `:547-560`, spatial `:562-576`, temporal scaling `:580-603`; `common/common.h:790-805` (`x264_predictor_clip`) | Vk264's **primary predictor is already a collocated temporal MV field**: `shaders/layout.glsl:82` (`motionSeedIndex = 5*mbWidth*mbHeight + mb` scratches previous-frame winners), read by `shaders/spatial.comp:18-21` (`seed()`), combined with left/above/corner median in `spatial.comp:40-52` and 9 neighbours + zero + median + 4 axial offsets in `spatial.comp:23-38` (23 candidates). No POC scaling (all frames are P in one closed GOP). |
| Per-MV bit cost `BITS_MVD` = `p_cost_mvx[mx]+p_cost_mvy[my]`, i.e. real λ-coded MVD cost | `encoder/me.c:60-70` | `shaders/motion.comp:17-19` and `spatial.comp:54-57` use a log2 **heuristic** `findMSB(|dx|+1)+findMSB(|dy|+1)` with an integer weight. MV-penalty scaling ×4/×0 was measured and reverted (`quality_baseline.md:141`). |
| Multi-ref loop with `REF_COST(0,i_ref)` (λ·`bs_size_ue(refidx)`) and duplicate-reference bypass `ref_blind_dupe` | `encoder/analyse.c:1255-1332` (loop `:1268`, ref cost `:1270-1271`, dupe `:1279-1283`, P-skip probe `:1294-1307`) | `shaders/older_references.comp:11-16` uses candidates 0 / latest MV / latest MV×(ref+1) with bias `findMSB(index+1)` (`:59`), integer-pel only (`:66-88`), and a local early-out when ref-0 error < 64 (`:34-45`). Older references cover ~0.76 % of P MBs (`quality_baseline.md:145-146`). |

### 1.2 Integer-pel motion estimation

| x264 step | Exact x264 location | Vk264 counterpart / difference |
|---|---|---|
| Predictor pre-pass with subpel MVs (subme≥3) and clipping | `encoder/me.c:214-275` | No subpel candidate pre-pass; hierarchical pyramid seed instead. |
| Search methods: DIA, HEX (+square refine), UMH (cross + early termination + adaptive range), ESA/TESA (ADS + SAD, TESA adds SATD on shortlisted candidates) | `encoder/me.c:320-342` (DIA), `:344-419` (HEX/square), `:422-616` (UMH; early-out `:448-466`, range multipliers `:468-474`), `:618-771` (ESA/TESA; successive elimination `:633-641`, TESA ADS→SAD→SATD `:653-749`) | Quarter-res box pyramid `shaders/pyramid.comp:7-18`; 2/3 rings at stride 1,2,4 over a `(radius/4)*2+1` grid `motion.comp:41-70` (3 rings when `satdCandidates>=4`, `:51`); integer refinement in a 7×7+3×3 stencil `motion.comp:72-84`; 9-point diamond final pass `:114-123`. This is a parallel, hardware-friendly analogue of UMH/TZS, not a port of x264's serial patterns. |
| Cost = SAD + MVD bits | `encoder/me.c:63-70`, `:76-79` | `motion.comp:10-20` (16/64-sample SAD + log2 MV penalty), `spatial.comp:98-106` (cheap 64-sample SAD shortlist over all 23 candidates, computed by all 64 lanes). |

### 1.3 Subpel interpolation and refinement

| x264 step | Exact x264 location | Vk264 counterpart / difference |
|---|---|---|
| `subpel_iterations[subme] = {refine_hpel, refine_qpel, me_hpel, me_qpel}` | `encoder/me.c:32-50` | Fixed: `subpel=1` (`presets.hpp:72`), so a single hpel/qpel diamond pass in `motion.comp:117-123`. |
| Fixed 6-tap H.264 luma interpolation (H/V/diagonal) | `common/mc.c` via `h->mc.get_ref` used at `me.c:76`, `:903-904`; chroma 4-tap | `shaders/halfpel.comp:9-11` sixTap `(a+f)+20(c+d)-5(b+e)`, `:21-34` horizontal/vertical/diagonal; `shaders/layout.glsl` `lumaPrediction()` consumes quarter-pel MVs. |
| HPEL diamond (4 candidates, x4 SIMD), SATD rescoring when the SATD and SAD comparators differ or chroma ME is on, multi-ref halfpel early-out at `bcost*7/8 > thresh`, QPEL diamond | `encoder/me.c:865-992` (hpel `:885-923`, SATD `:925-929`, early-out `:931-944`, qpel `:946-963`, subme=1 special case `:964-986`) | `motion.comp:114-123` 9-point refine at steps 2 then 1; **SATD is not used inside subpel** (tried: `quality_baseline.md:139`, motion 1.96→2.65 ms, −0.020 VMAF, reverted). No chroma ME. |
| Full RD subpel refinement at subme 9–11 (SATD pre-screen + real `COST_MV_RD` + square refine) | `encoder/me.c:1233-1355` (hex `:1307`, square `:1339-1345`) | Absent (no RDO path). |
| Bidirectional subpel refinement (`dia4d[33][4]`, SATD two-pass) for B slices | `encoder/me.c:1027-1190` (`dia4d` `:1064`, `me_refine_bidir_satd` `:1180`, `..._rd` `:1185`) | No B slices. |

### 1.4 Multi-reference selection and early termination

- x264 searches **all refs** for P16×16 (`analyse.c:1268-1314`) but uses a shared
  `p_halfpel_thresh` (`analyse.c:1260-1261`, `me.c:931-944`) so later refs skip
  expensive subpel when clearly worse; a near-zero vector at ref 0 triggers a
  P_SKIP probe (`analyse.c:1294-1307`); mixed-ref 8×8 uses early termination
  (`analyse.c:1334-1424`, `:1351`).
- Vk264 runs `older_references.comp` only for paired P slices with `references>1`
  and `frameIndex % gop > 1` (`gpu.cpp:642`), with integer-pel-only refinement that
  was measured at **+0.013/−0.071 dB, +0.017/−0.113 VMAF** and reverted
  (`quality_baseline.md:140`). The built-in local check (`older_references.comp:34-45`)
  is the surviving cheap early-out.

### 1.5 Weighted prediction

- x264: `x264_weights_analyse()` `encoder/slicetype.c:284-500` — initial linear
  guess `:330-357`, iterative scale/offset search `:422-460`, "0.2 % termination"
  for mostly-intra frames `:453-456`, `weight_cache` `:487-488`; applied inside
  ME via `h->mc.get_ref(..., &m->weight[0])` (`me.c:76`, `:903-904`) and in
  reconstruction via `x264_analyse_weight_frame()` (`analyse.c:219-244`);
  lookahead evaluates weighted costs at `slicetype.c:862`.
- Vk264: **none**. `shaders/bitstream.glsl:109` writes only deblock/constrained-intra
  flags; no `weighted_pred_flag`, no `pred_weight_table`; `lumaPrediction()` is
  unweighted. Adding it requires SPS/PPS + slice-header syntax, a weight-application
  path in motion, spatial, older-reference and residual shaders, and a decoder that
  honours it. Risk of `ffmpeg -err_detect explode` failure is high given the I4x4
  and native-CABAC syntax attempts (`quality_baseline.md:150-184`).

### 1.6 B-frames, slice type, lookahead, scene cut, MB-tree

| x264 step | Exact x264 location | Vk264 counterpart / difference |
|---|---|---|
| Low-resolution lookahead frame-cost and weighted pre-analysis | `encoder/slicetype.c:836-1000`, `:999-1057`, `:1186-1287` (VBV) | None. No lookahead buffer, no lowres ME of future frames. |
| B-adaptive decision: trellis path (`X264_B_ADAPT_TRELLIS`) and fast path comparing `cost_b` vs `cost_p` (`slicetype.c:1551-1621`, especially `:1613-1618`); simple fixed-B path `:1623-1645` | Same as left | No B slices. `b_frames=0`, `lookahead_frames=0` (`quality_baseline.md:38-39`). Baseline warns that B-frames **without lookahead lose 0.64 VMAF** (`quality_baseline.md:338-341`). |
| Scene-cut detection | `encoder/slicetype.c:1384-1473` (`scenecut_internal` `:1384`, `scenecut` `:1430`), called from `:1522-1528` and `:1653-1663` | No content scene-cut: GOP is periodic (default 5 s, `presets.hpp:64-68`). The ABR debt clamp was the actual fix for a hard cut at frame 206 (`quality_baseline.md:239-265`), not a slice-type decision. |
| Macroblock tree propagation | `encoder/slicetype.c:1675-1678`, path code `:1288-1383` | None. Roadmap ablation: MB-tree removal costs −0.166 VMAF in x264-veryslow (`improvement_roadmap.md:155-159`). |
| Frame-type finalisation / DPB reordering | `encoder/slicetype.c:1745-1975`, slice header reset `encoder/encoder.c:2791-2804` | I/P only, display order = decode order. B-frames would need a dual-reference ABI and host DPB reorder (`quality_baseline.md:338-341`). |

### 1.7 Intra prediction

| x264 step | Exact x264 location | Vk264 counterpart / difference |
|---|---|---|
| I16 mode availability by neighbour availability | `encoder/analyse.c:473-483` (`i16x16_mode_available`) | Mirrored: `shaders/residual.comp:94-101` (top/left availability), `:175` availability test. |
| V/H/DC/Plane SATD scoring with `lambda * bs_size_ue(mode)`; Plane only tested if the best directional mode is already promising | `encoder/analyse.c:692-715` (x3 SIMD `:700`, λ bits `:701-703`, Plane gate `:708-715`) | `shaders/residual.comp:90-188`: 4 modes × 16 4×4 blocks SATD (`:161-168`), λ = `0.55*exp2((qp-12)/3)` (`:172`), mode bits 1/3/3/5 (`:179-182`), strict `<` tie-break (`:182`). Accepted as PR #488 (`quality_baseline.md:106-109`). |
| Full intra RD (reconstructed residual, real bits) at high subme | `encoder/analyse.c:982` (`intra_rd`), `:1016` (`intra_rd_refine`) | Absent. This is the biggest prediction-side x264 tool: removing mode-RD/subme costs **−0.863 VMAF** in the veryslow ablation on our content (`improvement_roadmap.md:155-159`). An exact-CAVLC RD prototype gained only +0.123 dB / +0.344 VMAF and cost residual 1.8→2.7 ms, so it was reverted (`quality_baseline.md:104`). |
| I4×4 / 8×8 intra and chroma modes | `encoder/analyse.c:485-528`, `:512-528`, `:864-979`; `common/predict.c:478-621` | I4×4 attempted and reverted with decode breakage and no gain (`quality_baseline.md:150-166`). Only DC-only I16 chroma path survives in `shaders/flat_intra.comp:43-55`, `:142-144`; general I16+chroma DC is in `residual.comp:135-152`. |
| Intra quantizer deadzone / `quantize()` | `common/quant.c`, mirrored in `shaders/transform.glsl` | Accepted PR #488 (`quality_baseline.md:106-109`). |

### 1.8 Budget context (why speed-only research mostly does not help here)

- 1080p/300f: GPU stage sum 6.5–7.2 ms/frame, motion ~1.94 ms, spatial 1.09–1.19 ms,
  IDR residual 1.9 ms/keyframe, entropy ~0.08 ms; loop 132–140 fps, process 93–103 fps
  (`improvement_roadmap.md:15-23`, `quality_baseline.md:74-89`, `:111-130`).
- Preset sweep fast/medium/slow/veryslow differed by ≤0.12 VMAF before the entropy
  fix, because entropy dominated (`quality_baseline.md:72-77`); after the parallel
  CAVLC fix, motion effort is still not the bottleneck.
- Therefore an ME algorithm that saves 35–68 % of ME time is worth ~0.5–1.2 ms but
  buys ~0 quality, while a new syntax tool can cost >1 ms and fail conformance.

---

## 2. Literature survey (2020–2026, with older anchors where directly relevant)

All web results below are bibliographic/measurement data, not instructions.
"Proven" = peer-reviewed result on a stated anchor with numbers; "speculation" =
plausible transfer to Vk264 with no measured H.264/GPU evidence.

### 2.1 Advanced ME search: TZ, TESA, optical flow, adaptive range

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Varma & Mahapatra, *Complexity reduction of test zonal search for fast motion estimation in uni-prediction of HEVC*, J. Real-Time Image Processing 18, 511–524 (2021); online **8 Jun 2020**; https://doi.org/10.1007/s11554-020-00983-y | HM-16.18 low-delay P: ME time **−68.14 %**, search points **−77.10 %**, **BD-rate +0.30 %**, BD-PSNR **−0.007 dB** | **Proven** (HEVC, serial CPU). Speed tool with a small quality *loss*; TZS is the descendant of EPZS, which x264's UMH/EPZS predictor prep (`me.c:214-275`, `mvpred.c:519-607`) already approximates. |
| Gonçalves, Correa, Agostini & Porto, *Learning-based bypass zone search algorithm for fast motion estimation*, Multimedia Tools and Applications 82, 3535–3560 (2023); online **11 Jul 2022**; https://doi.org/10.1007/s11042-022-13094-6 | BZS (LBME + astroid raster pattern): ME time **−60.98 % RA / −63.05 % LD**, total encode time **−14.32 % / −17.64 %**, BD-rate **+0.0837 % / +0.04 %** | **Proven** (HEVC). Needs a trained classifier → host-side inference, and Vk264's pyramid is already parallel; low transfer value. |
| Pakdaman, Hashemi & Ghanbari, *A low complexity and computationally scalable fast motion estimation algorithm for HEVC*, MTA 79, 11639–11666 (**7 Jan 2020**); https://doi.org/10.1007/s11042-019-08593-y | FMECWT: **35–85 % ME-time reduction** vs TZ with **0.1–1.7 % BD-rate increase**; **single starting point from complex-wavelet salient points, no neighbour dependency** | **Proven** (HEVC). The no-neighbour-dependency property matches our per-MB workgroups; wavelet extraction is the expensive part. |
| Soroushmehr, Samavi & Shirani, *Adaptive search area for fast motion estimation*, arXiv:2204.04546, **9 Apr 2022**; https://arxiv.org/abs/2204.04546 | Rectangular per-block search area from temporal+spatial MV correlation; **≥7× faster than full search** | **Proven speed only** (no standard BD-rate table in the abstract). Similar in spirit to `motion.comp:41-70`'s already-adaptive pyramid rings. |
| Paramkusam, Darimireddy, Sridhar & Siripurapu, *All Directional Search Motion Estimation Algorithm*, Electronics 11(22), 3736, **15 Nov 2022**; https://doi.org/10.3390/electronics11223736 | Computation reduction **47.85 / 38.27 / 33.53 / 28.31 / 23.96 %** vs five known BM algorithms | **Proven speed only**; pattern-design paper, no rate distortion at matched rate reported in the accessible abstract. |
| Zhang et al., *Recent Advances on HEVC Inter-frame Coding*, arXiv:1910.09770 (2019, IEEE TCSVT; https://arxiv.org/abs/1910.09770) | Survey: TZS is HM's built-in fast ME, descendant of EPZS; tabulates precision-ME BD-rates around −1.4…−3.8 % at ~100 % encode time | **Proven survey**; older anchor that explains why x264's `me.c:320-419` patterns and our pyramid are already near the practical optimum. |
| Li et al., *MRFGNet: Multiscale Reference Frame Generation Network for VVC Inter-Coding*, ACM TOMCCAP, **2025**; https://doi.org/10.1145/3750049 | BD-rate **{−5.10 % Y, −12.14 % U, −11.51 % V}** and **{−5.98 % Y, −15.32 % U, −14.22 % V}** vs VTM_11.0-NNVC_4.0 (RA and LD configs) | **Proven in learned VVC**, but requires neural inference at multiple scales and new syntax → **speculative/non-conformant** for Vk264. |
| "Optical-flow ME" deep-codec papers, e.g. Sheng et al. / Lu et al., *Offline and Online Optical Flow Enhancement for Deep Video Compression*, AAAI 2023 (https://ojs.aaai.org/index.php/AAAI/article/view/28317), reporting +12.8 % PSNR over DCVC baseline | Neural codec only | **Speculation for Vk264**: no H.264 syntax for flow vectors; an encoder-side flow guide could at best improve MV *search*, with unmeasured gain. |

### 2.2 Subpel interpolation improvements

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Murn, Blasi, Smeaton & Mrak, *Improved CNN-based Learning of Interpolation Filters for Low-Complexity Inter Prediction in Video Coding*, arXiv:2106.08936v1, **16 Jun 2021** (submitted 18 May 2021); https://arxiv.org/abs/2106.08936; code https://github.com/bbc/cnn-fractional-motion-compensation | Linear 13×13 non-separable switchable filters (CompetitionCNN): luma BD-rate **−0.77 % RA, −1.27 % LDB, −2.25 % LDP** average lower-res; CTC C&D **−0.77 / −1.27 / −2.25 %** on VTM 6.0; Table II also −0.62/−1.06/−2.21 %. Compared with prior CNN filters that cost 50–700× decode time, this collapses to a single matrix multiply | **Proven in VVC with a switchable-filter syntax bit.** H.264 fixes 6-tap luma / bilinear chroma; substituting coefficients would break every conformant decoder → **not transferable** as reconstruction. Encoder-side-only use would be speculation. |
| HKUST thesis, *Sub-pixel interpolation filter design and combined inter-intra prediction for video coding* (https://researchportal.hkust.edu.hk/en/studentTheses/sub-pixel-interpolation-filter-design-and-combined-inter-intra-pr/) | Up to **0.39 dB** coding gain ≈ **11.39 % average bit-rate reduction** on high-resolution video | **Proven in its own framework**; same conformance obstacle for H.264. |
| Gao et al., *Decoder-Side Motion Vector Refinement in VVC: Algorithm and Hardware Implementation Considerations*, IEEE TCSVT 31(8), 3197–3211, Aug 2021 (online **2020**); https://doi.org/10.1109/TCSVT.2020.3037024 | Bilateral-matching DMVR, **no extra signalling**: BD-rate **−0.80 % (tool-off) / −2.81 % (tool-on)**, **+4 % decode time**, negligible external bandwidth (VTM 8.0) | **Proven in VVC**; needs bi-prediction/merge, i.e. B slices + dual-reference ABI that Vk264 explicitly lacks (`quality_baseline.md:338-341`). |

### 2.3 Multi-reference selection without lookahead

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Chen et al., *Fast algorithm for multi-reference and variable block size motion estimation in H.264/AVC*, ACM, 2010 (https://dl.acm.org/doi/10.1145/1734605.1734641) | Early termination of unnecessary reference searches; **53 % encoding-time saving** | **Proven older H.264 result**; Vk264 already has a cheap equivalent (`older_references.comp:34-45`) and older refs cover 0.76 % of P MBs. |
| Hsieh et al., *Fast Multiple Reference Frame Motion Estimation For H.264/AVC*, VCIP 2003 (https://homepage.ntu.edu.tw/~lgchen/publication/paper/[C][2003][VCIP][Bing-Yu.Hsieh][1].PDF); and *Analysis and complexity reduction of multiple reference frames ME in H.264/AVC*, IEEE TCSVT, 2006 (https://doi.org/10.1109/tcsvt.2006.872783) | MV continuity across refs used to stop after the first ref; ~50 % ME time saving at negligible PSNR loss | **Proven older anchors**; directly mirrored by `older_references.comp:11-16` (latest MV scaled by ref index). |
| **No 2020–2026 paper found** that improves *quality* of multi-ref selection without lookahead beyond these predictors | — | The modern literature moved to neural reference generation (MRFGNet, above) or VVC merge/DMVR. For Vk264 this axis is essentially exhausted. |

### 2.4 Weighted prediction

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Zhao, Ye et al., *Content Adaptive Weighted Prediction for Video Coding Beyond AV1*, VCIP 2023 (published **29 Dec 2023**); https://www.bohrium.com/paper-details/content-adaptive-weighted-prediction-for-video-coding-beyond-av1/1088686575832793111-0; DBLP VCIP 2023 | Average **0.31 % / 0.24 % BD-rate** coding gain (configurations per abstract snippet; full PDF not accessible) | **Proven in an AV1-style codec**, tiny gain on general content. |
| *Accurate parameter estimation and efficient fade detection for weighted prediction in H.264 video compression*, ICIP 2008; https://doi.org/10.1109/icip.2008.4712385 | Mathematically derived scale/offset for a fade model; **5–30 % bit-rate reduction at equal quality on fade sequences** | **Proven older H.264 anchor**; gain is content-conditional — near zero on the game/capture corpora used for Vk264 baselines. |
| *Weighted prediction in the H.264/MPEG AVC video coding standard*, 2003; https://ieeexplore.ieee.org/document/1328865 | Defines explicit and implicit WP (multiplicative factor + offset), Main/Extended profiles | **Standard reference**; confirms P-slice explicit WP is H.264-conformant *if* signalling is added. |

### 2.5 B-frame / slice-type decisions with lightweight lookahead

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Liu, Wang, Li & Ji, *Optimize x265 Rate Control: An Exploration of Lookahead in Frame Bit Allocation and Slice Type Decision*, IEEE Trans. Image Processing 28(5), 2558–2573 (**2019**); https://doi.org/10.1109/tip.2018.2887200 | x265 2.4 medium: **0.617 dB average, up to 1.705 dB BDPSNR**, encode time **−1.13 %**, rate accuracy **+4.2 %** | **Proven (CPU x265)**. Shows slice-type + bit allocation are worth ~0.5 VMAF-class gains, but requires a lookahead buffer and frame reordering; not a 1–2 ms shader patch. |
| *An Adaptive Slice Type Decision Algorithm for HEVC Based on Local Luminance Histogram*, Springer 2021/2022; https://link.springer.com/chapter/10.1007/978-3-030-90196-7_28 | ~**5 % complexity reduction** vs x265 slice-type decisions at similar quality | **Proven (x265)**; host-side heuristic only, no H.264/GPU transfer evidence. |
| Internal x264-veryslow ablation on our own content (`improvement_roadmap.md:153-159`) | B-frames removal costs **−0.411 VMAF**; MB-tree **−0.166**; refs **−0.147**; total veryslow−medium **+0.558 VMAF**, non-additive (individual sum +0.279) | **Measured on Vk264's corpus**, strongest local evidence; architecture-blocked here. |

### 2.6 Intra prediction refinements

| Paper / date / URL | Reported result | Proven vs speculation |
|---|---|---|
| Zhao et al., *Improved Intra Coding Beyond AV1 Using Adaptive Prediction Angles and Reference Lines*, ICIP 2020 (https://ieeetv.ieee.org/ondemand/improved-intra-coding-beyond-av1-using-adaptive-prediction-angles-and-reference-lines); journal extension *Improved Intra Mode Coding Beyond AV1*, IEEE (doc 9413420, https://ieeexplore.ieee.org/document/9413420) | One method: **0.4 % BD-rate reduction and 4 % encoding-time saving**; combined methods ≈**1.2 % coding gain** | **Proven in AV1-style intra coding**; H.264 I16 has only 4 modes, so multi-angle/MRL have no syntax to live in → non-conformant. |
| Zhao et al., *two-step progressive intra prediction for versatile video coding*, 2020 (https://hpc.pku.edu.cn/docs/pdf/a20200915260.pdf) | **0.87 % BD-rate reduction** in VTM-7.0 | **Proven VVC**; non-conformant for H.264. |
| *Fusion-Based VVC Intra Prediction with Template Matching and Linear Prediction*, 2022 (https://europepmc.org/article/MED/36015738) and PDPC/DIMD work, e.g. https://ieeexplore.ieee.org/document/9675443 | Standard VVC tools (PDPC, MRL, ISP, TIMD) give percent-level intra gains | **Proven VVC**; all require syntax absent from H.264. |
| Internal ablation (`improvement_roadmap.md:155-159`) | mode-RD/subme is the **largest single x264 tool** at **−0.863 VMAF** when removed; that is the only intra-related gap with a large measured number on our corpus | **Measured locally**; partly recoverable only with a cheaper RD approximation, not with new prediction modes. |

---

## 3. Candidate ledger — expected gain, speed cost, evidence, exclusion status

Gain columns are at matched bitrate on Vk264's own corpora where available; otherwise
the paper's anchor is named. "Gate" = +0.3 VMAF, rate within ±5 %, loop ≥100 fps,
process ≥85 fps, ctest 45/46, `check.py` 13/13, `ffmpeg -err_detect explode` clean
(`improvement_roadmap.md:99-110`).

| Candidate | Expected gain | Speed cost | Evidence | Verdict |
|---|---|---|---|---|
| **Explicit weighted prediction (P slices), fade detection from existing quarter-res pyramid** | **0 VMAF on game/capture corpora** (no fades); 5–30 % bitrate saving on fade sequences; 0.31/0.24 % BD-rate general content | ~0.1–0.3 ms/frame (scale/offset on a downsampled grid + header) | ICIP 2008 (https://doi.org/10.1109/icip.2008.4712385); VCIP 2023; `slicetype.c:284-500` | **Conformant with syntax work; high conformance risk.** Conditionally useful, not a fix for the measured gap. |
| **IDR-gated intra mode-RD-lite (reconstructed-domain, no new syntax)** | ≤ +0.03 VMAF overall; the ungated exact-CAVLC version was +0.123 dB/+0.344 VMAF but below gate | ~0.06 ms/frame amortised (4× 1.9 ms IDR residual ÷ GOP 125) | Mode-RD ablation −0.863 VMAF (`improvement_roadmap.md:155`); `quality_baseline.md:104` | Small but real; **does not clear gate**. |
| **TZ/FMECWT/ADS adaptive search-range early-out in `motion.comp`** | **0 quality** (or tiny BD-rate loss 0.1–1.7 %) | **saves ~0.5–1.2 ms/frame** of the 1.94 ms motion stage | Varma 2020, Pakdaman 2020, Gonçalves 2022, ADS 2022 | Best *speed* transfer; bank time, not quality. |
| **B-frames + lookahead + MB-tree** | +0.4–0.6 VMAF (local ablation), but **−0.64 VMAF without lookahead** | host DPB + reorder + dual-ref ABI; >2 ms | `improvement_roadmap.md:78`, `quality_baseline.md:338-341`; x265 TIP 2019 | **Architecture-blocked**, not a shader patch. |
| **DMVR / BIO / bilateral-matching subpel refinement** | 0.80–2.81 % BD-rate (VVC) | +4 % decode time there | TCSVT 2021 (`10.1109/TCSVT.2020.3037024`) | **Non-conformant for H.264 P**; needs B+merge. Exclude. |
| **Learned 13×13 switchable interpolation filters** | 0.77–2.25 % BD-rate (VVC) | one matrix-multiply per sample in VVC; GPU cost unmeasured here | arXiv:2106.08936 (2021) | **Non-conformant**: H.264 has fixed 6-tap. Exclude. |
| **Neural/optical-flow reference generation (MRFGNet et al.)** | −5.10 % Y BD-rate (VVC) | heavy NN inference | ACM TOMCCAP 2025 | **Non-conformant + far outside 1–2 ms.** Exclude. |
| **More SATD finalists 4→8** | +0.020 dB, **−0.005 VMAF**; another sweep 93.960→93.962 | spatial 1.19→1.76 ms, process 86.5→82.1 | `quality_baseline.md:102,138` | **Explicitly tried/reverted. Exclude.** |
| **SATD inside subpel search** | +0.014 dB, **−0.020 VMAF** | motion 1.96→2.65 ms | `quality_baseline.md:139` | **Tried/reverted. Exclude.** |
| **Subpel refinement of older references** | +0.013/−0.071 dB, +0.017/−0.113 VMAF | process →72.5 fps | `quality_baseline.md:140` | **Tried/reverted. Exclude.** |
| **MV cost penalty ×4 / ×0** | −0.226/−0.014 dB, −0.093/−0.020 VMAF | — | `quality_baseline.md:141` | **Tried/reverted. Exclude.** |
| **8×8 inter partitions / 8×8 transform** | −1.03 dB/−0.97 VMAF (`--partitions 1`); decode breakage for 8×8 transform | entropy 0.09→7.04 ms | `quality_baseline.md:103,142` | **Tried/reverted. Exclude.** |
| **I4×4 intra** | no gain; `ffmpeg -err_detect explode` failures; IDR residual 65.7→352 ms | > +250 ms/keyframe | `quality_baseline.md:150-166` | **Tried/reverted. Exclude.** |
| **AQ / psy-RD** | −0.63 dB/−1.28 VMAF | process →67 fps | `quality_baseline.md:143`, `improvement_roadmap.md:79,92` | **Tried/reverted. Exclude.** |
| **Exact CAVLC bit-cost RD (all MBs)** | +0.123 dB/+0.344 VMAF, below gate | residual 1.8→2.7 ms | `quality_baseline.md:104` | **Tried/reverted.** Only an IDR-gated variant is new (row 2). |
| **Native CABAC** | +0.35 VMAF @8000, +0.60 @6000, +0.35/+1.74 elsewhere; veryslow ablation −0.552 | ~23 fps process, 14 µs/MB, 117 ms/IDR | `quality_baseline.md:168-184`, `improvement_roadmap.md:77,120-130` | **Blocked by user decision and speed gate.** Out of scope here (see `03_ENTROPY.md`). |
| **Temporal scaled MV predictor (x264 `mvpred.c:580-603`)** | already present | 0 | `layout.glsl:82`, `spatial.comp:18-21` | **Already implemented. Exclude from candidates.** |
| **veryslow preset as default (radius 32, 8 finalists, 16 refs)** | fast/medium/slow/veryslow 93.84/93.93/93.96/93.95 VMAF | no speed benefit; entropy/loop bound | `quality_baseline.md:72-77`, `improvement_roadmap.md:95` | **Explicitly rejected. Exclude.** |

---

## 4. Top 3 transferable candidates (ranked) — and the plain verdict

> **Plain verdict: none of the surveyed 2020–2026 results clears the +0.3 VMAF
> matched-rate / ≤1–2 ms / conformant gate for Vk264's PREDICTION stage.** The three
> below are the closest transferable options, ranked by evidence strength and
> shader fit. All are expected to land **below** the quality gate; #1 is a speed
> play, #2 a small quality play, #3 a content-conditional standard tool. If the
> project wants a ≥+0.3 VMAF win in prediction, there is no such result in this
> literature that is also H.264-conformant and shader-scale.

### Rank 1 — Pyramid-confidence adaptive search range (TZ/FMECWT-style early-out)
**Expected:** quality ≈ 0 (published cost +0.1–1.7 % BD-rate); saves ~0.5–1.2 ms/frame
of motion. **Evidence:** Varma 2020 (ME time −68.14 %, BD-rate +0.30 %), Pakdaman
2020 (−35…−85 % ME, +0.1–1.7 % BD-rate), Soroushmehr 2022 (≥7× vs FS), ADS 2022
(−24…−48 % computation). **Conformant:** yes — MV choice unchanged in bitstream.

Sketch (shader-local, no host/ABI change):
1. In `pyramid.comp:7-18`, alongside the packed 8-bit src/ref pair, store a per-cell
   absolute difference `max(|src-ref|)` into the unused high bits of `quarterLuma`
   (currently `src | (ref<<8)`; bits 16–23 are free if the buffer is widened, else
   add a second 32-bit array).
2. In `motion.comp:41-70`, `pyramidSeed()` already returns a winner cell and its
   stride. Use the ring-0 minimum SAD to classify the MB: if `minSad < T0`, set a
   per-MB `radius` of `radius/2` for `refineInteger()` (`:72-84`); if `minSad > T1`,
   keep full radius. Thresholds can be derived from the current QP via the existing
   `bitPrice` pattern in `flat_intra.comp:140`.
3. In `motion.comp:117-123`, skip the step-2 refinement when the pyramid winner's
   stride was 1 and step-1 SAD is already below `T0/4`.
4. Keep the `score()` tie-break bits (`:17-19`) byte-identical so streams stay
   unchanged when the fast path is inactive; add a `--me-adaptive` knob default off
   until a byte-identical fixture sweep passes.
5. Validate: 45/46 ctest, `check.py` 13/13, and confirm motion stage drops from
   ~1.94 ms to ~1.2 ms with VMAF within ±0.02 at 6000/8000.

**Why rank 1:** it is the only item with strong literature evidence *and* zero
syntax/conformance risk. It does not improve quality directly; it buys ~0.7 ms that
could fund Rank 2 without breaking the loop gate.

### Rank 2 — IDR-gated intra mode-RD-lite (reconstructed-domain, no new syntax)
**Expected:** ≤ +0.03 VMAF overall (ungated exact-CAVLC prototype +0.123 dB / +0.344
VMAF was already below gate; IDRs are ~1/125 frames). **Evidence:** mode-RD/subme is
the largest x264 tool on our corpus (−0.863 VMAF, `improvement_roadmap.md:155`), and
the +0.123 dB / +0.344 VMAF prototype is measured (`quality_baseline.md:104`).
**Conformant:** yes, because it only changes which of the four existing I16 modes
is chosen; no bitstream syntax changes.

Sketch (extend `shaders/residual.comp:94-188`, gated at `:94`):
1. After the existing 4-mode SATD loop (`:161-168`), and only when
   `intra && idrWholeFrame()` (or `flatIntra` P MBs), re-score each *available* mode
   in the reconstructed domain: take the mode's 4×4 Hadamard coefficients
   (`modeCost` is SATD only; add a `[4][16]` level scratch), call the existing
   `dequantize(level,qp,0)` / inverse `hadamard4` from `transform.glsl`, and compute
   16-block SSE against `source[][]`.
2. Bit price = `intraModeBits` (`layout.glsl:85-88`) + `levelCost(level)` from
   `flat_intra.comp:61`; use `J = SSE + λ2*bits` with λ2 ≈ `0.11*exp2((qp-12)/3)`
   (x264's `i_lambda2` convention), replacing the SATD+λ cost only for this gate.
3. Keep all coefficients in scratch until the winner is chosen, as the I4x4 attempt
   required (`quality_baseline.md:154`); keep strict `<` tie-break so existing
   fixtures stay byte-identical when the gate is off.
4. Budget: IDR residual is 1.9 ms/keyframe (`improvement_roadmap.md:21`); 4× the
   intra branch ≈ 7.6 ms/keyframe ⇒ **0.06 ms/frame amortised at GOP 125**, well
   inside 1–2 ms.
5. Risk: 1-LSB reconstruction mismatch was the failure mode of I4x4
   (`quality_baseline.md:161-164`); require the exact same `quantize()` path and
   `ffmpeg -err_detect explode` before any adoption.

**Why rank 2:** it is the only prediction-side change that attacks an ablation worth
>0.8 VMAF, and it fits the budget — but the measured fraction of that gain reachable
from IDRs alone is small.

### Rank 3 — Explicit P-slice weighted prediction with pyramid-based fade detection
**Expected:** 0 VMAF on the current game/capture masters; up to 5–30 % bitrate saving
on fade sequences, 0.31/0.24 % BD-rate on general content. **Evidence:** ICIP 2008,
VCIP 2023, x264 `slicetype.c:284-500`. **Conformant:** yes in Main profile *with*
correct SPS/PPS `weighted_pred_flag`, per-slice `pred_weight_table`, and weight
application at encoder and decoder; **high syntax risk** here.

Sketch:
1. Host: set `weighted_pred_flag=1` in PPS (Main profile) and add a per-slice
   `pred_weight_table` (luma log2_denom, weight/offset for ref 0; explicit only).
2. Compute scale/offset once per frame from the **existing quarter-res pyramid**
   (`pyramid.comp:7-18` already packs src and ref means): match mean luma of the
   incoming frame vs ref 0, derive `weight = round(128*mean_ref/mean_cur)` clipped to
   [1,254] and an offset, reusing x264's termination logic
   (`slicetype.c:453-456`: skip WP if >99.8 % of blocks are better unweighted).
3. Apply the weight in `lumaPrediction()` for ref 0 in `motion.comp`, `spatial.comp`
   and `residual.comp`, and in reconstruction; x264's equivalent hooks are
   `me.c:76` (`get_ref(...,&weight)`) and `analyse.c:219-244`.
4. Budget: mean computation reuses the pyramid; ~0.1–0.3 ms/frame plus header bits.
5. Gate: only enable when the measured mean shift exceeds ~5 %; otherwise emit the
   byte-identical unweighted stream.
6. Risk: the I4x4 and CABAC attempts both died on slice-header/syntax integration
   (`quality_baseline.md:150-184`); budget substantial validation time before any
   quality claim.

**Why rank 3:** it is a genuinely conformant H.264 tool with large upside *on fade
content*, but our measured corpora contain hard cuts, not fades, so the expected
gain here is zero and the syntax risk is the highest of the three.

### Explicitly *not* recommended
Do **not** re-attempt TESA/SATD-exhaustive search, more SATD finalists, subpel on
older refs, MV-penalty retuning, I4×4, 8×8, AQ/psy-RD, more refs, or veryslow as
default — all are on the rejected list (`quality_baseline.md:91-166`,
`improvement_roadmap.md:89-95`). Do not port DMVR/BIO, learned interpolation
filters or neural reference generation: they need syntax our decoder does not have.
Do not start B-frames without a scope decision on the dual-reference ABI and host
DPB reorder (`quality_baseline.md:338-341`).

---

## 5. Quick reference — all URLs cited

- https://doi.org/10.1007/s11554-020-00983-y — Varma & Mahapatra, TZS complexity reduction, online 2020-06-08
- https://doi.org/10.1007/s11042-022-13094-6 — Gonçalves et al., BZS, online 2022-07-11
- https://doi.org/10.1007/s11042-019-08593-y — Pakdaman et al., FMECWT, 2020-01-07
- https://arxiv.org/abs/2204.04546 — Soroushmehr et al., adaptive search area, 2022-04-09
- https://doi.org/10.3390/electronics11223736 — Paramkusam et al., ADS, 2022-11-15
- https://arxiv.org/abs/1910.09770 — Zhang et al., HEVC inter-frame survey (TCSVT 2019)
- https://doi.org/10.1145/3750049 — Li et al., MRFGNet, ACM TOMCCAP 2025
- https://ojs.aaai.org/index.php/AAAI/article/view/28317 — offline/online optical-flow enhancement, AAAI 2023
- https://arxiv.org/abs/2106.08936 — Murn et al., learned interpolation filters, 2021-06-16
- https://researchportal.hkust.edu.hk/en/studentTheses/sub-pixel-interpolation-filter-design-and-combined-inter-intra-pr/ — HKUST subpel filter thesis
- https://doi.org/10.1109/TCSVT.2020.3037024 — Gao et al., VVC DMVR, online 2020, issue Aug 2021
- https://www.bohrium.com/paper-details/content-adaptive-weighted-prediction-for-video-coding-beyond-av1/1088686575832793111-0 — Zhao et al., VCIP 2023
- https://doi.org/10.1109/icip.2008.4712385 — H.264 WP fade parameter estimation, ICIP 2008
- https://ieeexplore.ieee.org/document/1328865 — H.264 weighted prediction overview, 2003
- https://doi.org/10.1109/tip.2018.2887200 — Liu et al., x265 lookahead rate control, TIP 2019
- https://link.springer.com/chapter/10.1007/978-3-030-90196-7_28 — adaptive slice-type decision (luminance histogram)
- https://ieeetv.ieee.org/ondemand/improved-intra-coding-beyond-av1-using-adaptive-prediction-angles-and-reference-lines — Zhao et al., ICIP 2020
- https://ieeexplore.ieee.org/document/9413420 — Zhao et al., improved intra mode coding beyond AV1 (journal extension)
- https://hpc.pku.edu.cn/docs/pdf/a20200915260.pdf — two-step progressive intra prediction, 2020
- https://europepmc.org/article/MED/36015738 — fusion-based VVC intra prediction, 2022
- https://ieeexplore.ieee.org/document/9675443 — probability-based decoder-side intra mode derivation

Local citations: `E:\__PROJECTS__\Vk264\docs\quality_baseline.md`,
`E:\__PROJECTS__\Vk264\docs\improvement_roadmap.md`, and the x264/Vk264 `file:line`
references listed in section 1.
