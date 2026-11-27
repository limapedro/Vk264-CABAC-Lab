# 02 — MODE DECISION / RD / TRANSFORM / QUANTIZATION stage

**Research only. No code, no PRs, no changes outside this file.**
**Compiled:** 2026-11-27. **Owner:** subagent for Vk264-CABAC-Lab pipeline study.
**Web handling:** all web pages were treated as untrusted data; only bibliographic facts and
reported measurements were extracted. No instruction found in target content was followed.
Evidence labels: **[L]** = local measurement/docs in `E:\__PROJECTS__\Vk264` or this lab;
**[P]** = published measured result; **[E]** = my extrapolation to our operating point;
**[S]** = speculation.

---

## 0. Ground truth and verdict

### 0.1 What Vk264 actually does today (all [L])

| Fact | Source |
|---|---|
| CAVLC, I/P only, 5 refs, radius 24, no B, no CABAC, no trellis/RDO, no 8×8 DCT, no MB-tree, no lookahead | `docs/improvement_roadmap.md:9-13` |
| Quantizer: integer 4×4 transform (`shaders/transform.glsl:28-38`), deadzone divisor `intra ? 2 : (index==0 ? 2 : 3)` — **inter AC divisor 3 is PR #501** | `shaders/transform.glsl:10-22`; `docs/quality_baseline.md:186-208` |
| Bit estimate in RD decisions: `3 + 2*findMSB(|level|)` per nonzero coefficient; MV bits `2*(MSB(2|dx|+1)+MSB(2|dy|+1))` | `shaders/residual.comp:33-36, 221-231, 253-261`; `shaders/spatial.comp:54-57` |
| Group keep/skip and P-skip: SSE-difference vs `λ·bits`, with `λ = 0.55·exp2((qp−12)/3)`; chroma mode chosen by same form | `shaders/residual.comp:295-345` |
| I16 mode: SATD + `λ·bs_size_ue(mode)` (1/3/3/5 bits), winner by strict `<` | `shaders/residual.comp:90-188` |
| ME: SAD + MV-bit proxy, SATD finalists (4 default), no reconstructed-SSD RD | `shaders/spatial.comp:59-143` |
| ABR: separate IDR budget, debt clamp `±allocation/4`, aim clamp `[0.5,1.5]×`, step `±2` (IDR `±4`), QP floor 18 at ≥50 fps | `shaders/scan.comp:24-64` |
| **Reverted**: exact CAVLC bit-cost RD — **+0.344 VMAF @6000**, residual **1.8 → 2.7 ms** | `docs/quality_baseline.md:104` |
| Current quality/throughput: 1080p/300f, `--gop 125 --qp 14`: VMAF **96.15 @6000** (6178 kb/s), **97.11 @8000** (8129 kb/s), x264-slow 97.99/98.31; loop 132–140 fps, process 93–103 fps, GPU stage sum ~6.5–7.2 ms/frame | `docs/improvement_roadmap.md:15-29` |
| Measured deficit is **fine detail**: VIF scale0 **wx264 ≈ 0.52 vs x264 ≈ 0.68 @6 Mb/s**, while **ADM2 nearly equal**; boundary/mid-MB error ratios already match x264 | `notes/pipeline/MASTER.md:23-24`; `docs/quality_baseline.md:186-191` |

### 0.2 x264 tool-ablation ceiling (all [L], on `obs_new_src.yuv`, 300f 1080p60, 6000 kbps, VMAF v0.6.1)

x264-medium **93.426** → x264-veryslow **93.984** (**+0.558**). Removing one tool from veryslow costs:
**mode-RD/subme −0.863**, **trellis −0.650**, **CABAC −0.552**, **B-frames −0.411**,
**8×8dct −0.210**, **MB-tree −0.166**, **refs −0.147**; disabling AQ **gains +0.241**.
Individual gains are **non-additive** (sum +0.279 vs actual +0.558) — `docs/improvement_roadmap.md:153-159`.
At 8000 kbps the entropy/RDO contribution shrinks: the 81-pair CABAC/CAVLC sweep measured
**+0.603 VMAF @6000** but only **+0.350 @8000** (~0.58×) — `notes/CABAC_VS_CAVLC_SWEEP.md:22-26`.
I use **0.55–0.70×** as the 6000→8000 scaling for encoder-only RD/entropy techniques below.

### 0.3 Verdict

* The stage is **already partially solved at the cheap end**: SATD+λ bits for I16, ME, and
  group keep/skip exist; the missing pieces are (a) a rate-accurate `R` term, (b) per-coefficient
  reconstruction-aware rounding, (c) any perceptual/subband weighting of `D`.
* **Do not chase mode-RD (−0.86) or 8×8dct (−0.21)**: the former needs a reconstruction loop per
  candidate (≈16 re-encodes/MB), the latter is already tried and reverted with syntax breakage.
* A VIF-scale0-only deficit with near-equal ADM2 says **fine-scale information is being quantized
  away, not that artifacts are being added**. Favour **per-coefficient / per-subband distortion
  weighting and rounding** over sharpening, global AQ, or block-level QP moves. This also avoids
  the metric-exploit trap (§2.6).
* Top 3 transferable candidates: **(1) subband-weighted pruned QNS, (2) VIF-proxy
  distortion-propagation λ, (3) Cauchy/Laplace-calibrated analytic bit estimator** (§4).
  No single candidate closes the ≈1.8 VMAF gap to x264; a realistic stack yields ≈+0.4–0.9.

---

## 1. Stage breakdown: x264 `file:line` ↔ Vk264 counterpart

Paths are relative to `E:\__PROJECTS__\Vk264\x264-vulkan\` for x264 and
`E:\__PROJECTS__\Vk264\` for the shader encoder (read-only).

### 1.1 λ / cost tables and per-frame load

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| λ definition `λ = 2^(QP/6−2)` | `common/tables.c:96-110` (`x264_lambda_tab`) | — |
| `λ² = λ²·0.9·256`, capped | `common/tables.c:112-127` (`x264_lambda2_tab`) | — |
| Trellis λ²: inter `0.85²·2^(QP/3+10−LAMBDA_BITS)`, intra `0.65²·…` | `common/tables.c:129-166` (`x264_trellis_lambda2_tab[2][]`) | — |
| Chroma λ² offset table, QP-dependent | `common/tables.c:168-177` | `int chromaQp(int qp)` map only — `shaders/transform.glsl:5-7` |
| MV cost table `λ·bs_size_te`, `cost_i4x4_mode`, PCM cost cap | `encoder/analyse.c:143-204` (`init_costs`), `247-272` (`mb_analyse_load_costs`), `316-318` | no precomputed tables; `motionCost()` recomputes MSB proxies — `shaders/spatial.comp:54-57` |
| Psy/RD & trellis enablement (`i_psy_rd` only at subme≥6; `i_psy_trellis = f_psy_trellis/4`) | `encoder/encoder.c:1211-1219`; defaults `common/base.c:440-442` | none (psy-RD tested and reverted, negative) |
| Single heuristic λ actually used in all shader decisions | — | `float lambda = 0.55*exp2((qp-12)/3)` — `shaders/residual.comp:172` (I16) **and** `:315` (group/chroma/P-skip). ABR step uses `log2(rateRatio)` — `shaders/scan.comp:54-56` |

**Gap:** x264 carries **four distinct Lagrange multipliers** (mode λ, RD λ², trellis λ²
inter/intra, chroma-adjusted λ²). Vk264 has **one heuristic λ** for every decision and a 0.5
factor on the distortion side; it is not a calibrated RD slope.

### 1.2 Mode decision / RD

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| Intra I16/I8/I4 SATD pre-search | `encoder/analyse.c:561-979` (mode tables `:124-136`, I16 `:686-738`, I8 `:753-823`, I4 `:867-950`) | V/H/DC/Plane I16 SATD only — `shaders/residual.comp:90-188`. **I4×4 tried and reverted** (bitstream failures, 1080p IDR 65.7→352 ms) — `docs/quality_baseline.md:150-166` |
| Full RD intra refine | `intra_rd` `encoder/analyse.c:982-1015`, `intra_rd_refine` `:1016-1120` | none |
| P/B mode RD | `mb_analyse_p_rd` `encoder/analyse.c:2547-2621`; `mb_analyse_b_rd` `:2622-2772` | none (P-skip/group heuristic only, `residual.comp:295-345`) |
| Transform-size RD (4×4 vs 8×8) | `mb_analyse_transform_rd` `encoder/analyse.c:2773-2809`; `mb_analyse_qp_rd` `:2810-2920` | none |
| **RD cost** = encode → SSD → exact bit count | `rd_cost_mb` `encoder/rdo.c:162-197` (encode `:169`, optional deblock RDO `:171-172`, `ssd_mb` `:174`, skip `:176-179`, CABAC `:180-186`, **CAVLC `:187-191`**) | no reconstruction-based cost; decisions from SATD/SSE + heuristic bits at `residual.comp:221-231, 295-345` |
| Sub-8×8 / partition RD, 8 bit extra precision | `rd_cost_subpart` `encoder/rdo.c:201-231`; `x264_rd_cost_part` `:233-278` | 8×8 partitions compiled off in `slow`; `residual.comp:80-88` uses only summed match costs + mode penalty |
| Intra 8×8 / 4×4 / chroma RD | `rd_cost_i8x8` `encoder/rdo.c:279-313`, `rd_cost_i4x4` `:315-347`, `rd_cost_chroma` `:349-373` | none |
| Subpel RD refinement | `x264_me_refine_qpel_rd` calls `encoder/analyse.c:3256-3298, 3639-3664` | SAD+SATD finalist search `spatial.comp:105-143`; subpel for older refs tested and reverted |

**Key structural difference:** x264's `rd_cost_mb` **actually encodes the macroblock** and
writes its bitstream into a bit counter before scoring; Vk264 scores from features computed once
in a single residual dispatch. That is why x264's mode RD is worth −0.863 VMAF when removed but
is **not directly transferable** to a ~1–2 ms shader budget.

### 1.3 Psy-RD / psy-trellis

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| Psy distortion = SSD + `psy·λ·|SATD(fenc) − SATD(fdec)|`, with SA8D for 8×8 blocks to avoid transform-size bias | `encoder/rdo.c:114-148` (`ssd_plane`), cached complexity `:95-112`; chroma λ² offset `:150-159` | none |
| Psy-trellis DCT-domain cache init | `encoder/analyse.c:561-573`, called `:3727-3728` | none |
| Psy term inside trellis: `d²·coef_weight2 − psy·|recon|·coef_weight1` | `encoder/rdo.c:821-829` (4×4/8×8), `:993-1000` (CAVLC QNS) | none |
| Tunes (`film` 0.15 psy-trellis, `animation` 0.4, `stillimage` 2.0/0.7) | `x264.c:641-660` | not exposed; `--aq 1` measured **−1.28 VMAF**, psy/reverted per `improvement_roadmap.md:79, 143` |

**Note for ranking:** x264 psy-RD often *lowers* PSNR/VMAF while improving perception; Ozer's
2026 per-title study deliberately left psy at defaults because "these settings often lower VMAF
and PSNR" ([Streaming Learning Center, 2026-09-25](https://streaminglearningcenter.com/articles/optimizing-x264-settings-and-per-title-ladders.html)).
Do **not** reintroduce psy for a VMAF target. The transferable part of psy is the *idea* of
per-subband distortion weighting, not the SATD-complexity penalty.

### 1.4 Quantization and deadzones

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| Scalar quant core `level = (f + |coef|)·mf >> 16`, sign restored, `nz |= coef` | `common/quant.c:50-57` (`QUANT_ONE`), applied `quant_8x8 :59-65`, `quant_4x4 :67-73`, `quant_4x4_dc :88-94`, `quant_2x2_dc :96-104` | `quantLevel()` — `x264-vulkan/vulkan/quant_level.glsl:3-6` (fork); native equivalent in `shaders/transform.glsl:10-22` |
| Deadzone parameters: `i_luma_deadzone[2] = {inter=21, intra=11}` | `common/base.c:456-457`; `x264.h:431`; CLI `:829-830`; clamped `encoder.c:1148-1149` | inline constants in `shaders/transform.glsl:18`: `divisor = intra ? 2 : (index==0 ? 2 : 3)` |
| Deadzone→quant bias mapping `bias = min(deadzone·1024/mf, 32768/mf)` for 4×4 and 8×8 | `common/set.c:81-82, 179-180, 199-200` | none (equivalent expressed as integer divisor) |
| Quant matrix category (DC vs AC) | `shaders/transform.glsl:8` `category(index)` mirrors x264 `x264_dct4_weight` indexing | same file `:1-4` `quantScale[18]`, `inverseScale[18]` |
| Tuning history | — | divisor 6→4→**3 (PR #501 accepted)**; divisor 2 overshot, chroma DC 1 rejected, low/high-frequency split 2/4 rejected — `docs/quality_baseline.md:194-205` |

**Important for candidates:** the accepted PR #501 change was a **deadzone-width** change
(division factor 6→3) at *fixed* QP/λ. It did **not** add a reconstruction-aware rate term or a
per-subband *distortion weight*. The rejected "low-freq AC 2 / high-freq 4" variant was a second
deadzone-width split, not the same as a rounding-bias or RD-distortion-weight candidate (§4.1).

### 1.5 Trellis / QNS (coefficient-level RD)

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| Trellis node/level structures, score bias | `encoder/rdo.c:375-428` | none |
| CABAC trellis: `trellis_dc_shortcut :463`, `trellis_coef :495`, node helpers `:538-641` | `encoder/rdo.c:463-641` | none, and CABAC is excluded by user decision |
| CABAC coefficient loop: two roundings `{q, q−1}` only, `ssd = d²·coef_weight2`, subband weights from `x264_dct4_weight2_tab` / `x264_dct8_weight2_tab` | `encoder/rdo.c:760-895` (weight tables `:928-929`, distortion `:816-840`, branch cases `:844-859`) | none |
| **CAVLC "trellis" is explicitly a QNS hack**, not a true trellis: 5 coupled state machines (coeff_token, total_zero, zero_run, level state), no known near-optimal polynomial algorithm; brute force over 2¹⁶ rounding modes is feasible | `encoder/rdo.c:897-919` (`quant_trellis_cavlc :920-…`) | none |
| QNS distortion delta + heuristic rate per level change | `encoder/rdo.c:963-1000` | none |
| x264 ablation worth | **−0.650 VMAF when removed** — `docs/improvement_roadmap.md:155` | our exact-CAVLC-bit RD prototype **+0.344 VMAF but +0.9 ms** → reverted — `docs/quality_baseline.md:104` |

**This is the single largest transferable lever.** The literature (§2.1) says pruning and
vectorization make QNS nearly free; our own prototype failed on *unpruned exact-bit cost*, not on
the idea of per-coefficient RD.

### 1.6 CAVLC rate terms used in RD

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| **Exact** CAVLC bit cost for MB RD: `rdo.c:51-53` renames `x264_macroblock_write_cavlc` (`encoder/cavlc.c:487`) to `static macroblock_size_cavlc`; `rd_cost_mb` calls it at `rdo.c:189`, reads `bs.i_bits_encoded` at `:190` | `encoder/cavlc.c:487-…`, `encoder/rdo.c:51-53, 187-191` | **Heuristic** `3+2·MSB(level)` — `shaders/residual.comp:227, 259`; exact variant tried/reverted |
| Exact partition/subpartition/intra/chroma CAVLC costs | `encoder/cavlc.c:605-720` (`partition_size_cavlc :605`, `subpartition :661`, `i8x8 :685`, `i4x4 :695`, `chroma :704`) | none |
| MB type / sub-MB type VLC cost tables | `encoder/analyse.c:124-136`, added with `λ` at `:173, 738, 753, 867, 1413, 1468, 1719-1791` | `intraModeBits()` and fixed 8/16-bit mode penalties — `residual.comp:185`, `:85-86`, `spatial.comp:105` |
| MV differential cost | `encoder/analyse.c:247-258` (`cost_mv`), `bs_size_te` | `approximateMvBits` `residual.comp:33-36`; `motionCost` `spatial.comp:54-57` |

### 1.7 8×8 adaptive transform

| Sub-step | x264 | Vk264 counterpart |
|---|---|---|
| Eligibility `x264_mb_transform_8x8_allowed` | `common/macroblock.h:450-454` | none |
| Search/decision (RD each way) | `encoder/analyse.c:2737-2768`, `:2775-2797`, `:2905-2911` | none |
| 8×8 DCT encode path | `encoder/macroblock.h:175, 200-208` (`sub8x8_dct8`, `x264_quant_8x8`, `dequant_8x8`, `add8x8_idct8`); CAVLC gating `encoder/cavlc.c:555`; CABAC `:973` | fork has `x264-vulkan/vulkan/dct8x8.comp` / `dct8x8_math.glsl`, but native path **8×8 inter reverted** with decode breakage — `docs/quality_baseline.md:103` |
| 8×8 RD cost / trellis branch / CAVLC residual writer | `encoder/rdo.c:279-313`, `:682-684`, `:721-724`; `encoder/cavlc.c:685-693` | none |
| x264 ablation worth | **−0.210 VMAF** — `docs/improvement_roadmap.md:156` | not transferable without High-profile syntax path + entropy changes |

### 1.8 Interface to rate control (other subagent's stage, noted because it changes λ)

x264: `encoder/ratecontrol.c` (MB-tree `lookahead.c`/`slicetype.c`) vs Vk264's single
`shaders/scan.comp:24-64`. The QP produced there feeds every λ above; any per-MB λ/QP
candidate in §4 must be **rate-neutral** or ABR will re-spend the gain (already observed:
"ABR re-spends nearly all of it", `improvement_roadmap.md:72`).

---

## 2. Literature survey 2020–2026 (web content treated as untrusted data)

**Conformance key.** **Y** = encoder-only decision/QP/rounding change; bitstream and normative
decoder unchanged. **N** = changes transform, quantizer state machine, entropy layout, or
in-loop filtering — needs a decoder change or new syntax. **Pre** = source preprocessing;
stream is undecodable-difference-free but the gain is partly metric enhancement (§2.6).

### 2.1 RDOQ / trellis advances

| Work | Date / URL | Measured result [P] | Conf. | Transfer to us |
|---|---|---|---|---|
| Wang et al., *Low Complexity Trellis-Coded Quantization in VVC*, IEEE TIP 30:2378–2393 | 2020-08-26 [arXiv:2008.11420](https://arxiv.org/abs/2008.11420), [doi](https://doi.org/10.1109/TIP.2021.3051460) | **−11% (AI) / −5% (RA) encode time**, **+0.11% / +0.05% BD-rate**, **24% / 27% quantization-time saving**; adopted in VTM. TCQ itself: **3.5% AI / 2.4% RA bit saving** vs RDOQ | N (VVC dependent quantization is normative) | **Pruning + RD-model-guided trellis departure point** is the pattern for our QNS; keep H.264 scalar quantizer |
| Wieckowski et al. (Fraunhofer HHI), *Fast dependent quantization using trellis pruning, forward context adaptation and vectorization*, EUSIPCO | 2024-08-23 [PDF](https://eurasip.org/Proceedings/Eusipco/Eusipco2024/pdfs/0000516.pdf) | **37% quantization runtime reduction**, **15% overall encoder speedup (AI)**, **9% (RA)**, **no compression loss**; base DQ costs VVenC **+30%** vs scalar quantization but gives **1.7% BD-rate** | N (VVC DQ) | Forward context adaptation + vectorization = shader-friendly; 2% of speedup from pruning alone |
| Sheng et al., *Parallel Viterbi DQ + FPGA*, IEICE Trans. Inf. Syst. E107.D:797–806 | 2024-07 [article](https://www.jstage.jst.go.jp/article/transinf/E107.D/7/E107.D_2023EDP7243/_article/-char/en) | **56.96% fewer quantization cycles**; **+1.03% / +1.05% BDBR** (LDP/RA); 1080p60 on AMD FPGA | N | Parallel Viterbi decomposition maps to subgroup/warp work; quality cost shows why pruning alone is not a gain |
| Tang et al., *SIMD-based parallel RDOQ for AVS3*, 电信科学 2024(6) | 2024-07-15 [PDF](https://www.telecomsci.com/rc-pub/front/front-article/download/64852539/lowqualitypdf/%E5%9F%BA%E4%BA%8ESIMD%E7%9A%84AVS3%E5%B9%B6%E8%A1%8C%E7%8E%87%E5%A4%B1%E7%9C%9F%E4%BC%98%E5%8C%96%E9%87%8F%E5%8C%96%E7%AE%97%E6%B3%95.pdf) | **−29.46% encode time, +0.25% BD-rate** (AI). Earlier TCSVT scanline version: **31% time saving for 0.25–0.27% BD-rate regression** ([doi](https://doi.org/10.1109/tcsvt.2023.3349278)) | N (AVS3) but algorithm pattern general | RDOQ is a **speed** tool when parallelized, not a quality breakthrough — matches our reverted prototype's cost problem |
| Mediatek, *Hardware-friendly RDOQ*, US 12,395,640 B2 | granted 2025-08-19 [PDF](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12395640) | Replaces 5 final-neighbour contexts with **directly quantized** neighbours to break CG dependency; no published BD-rate in text reviewed | N (VVC context model) | Dependency-breaking idea relevant to CAVLC `nC` (neighbour blocks) |
| Yang & Yu, *Rate-Distortion Optimization in H.264* (soft-decision quantization framework) | 2006 [PDF](https://ita.ucsd.edu/workshop/06/papers/185.pdf) | Formal 5-symbol CAVLC trellis (head / run / level / end) — establishes why exact CAVLC QNS is exponential | Y (encoder only) | Confirms x264's `rdo.c:897-908` "gigantic hack" comment; use pruned 2-rounding QNS, not full trellis |
| Zhao et al., *Novel Statistical Modeling … RD Estimation for H.264/AVC*, IEEE TCSVT 20(5) | 2010 [PDF](http://idm.pku.edu.cn/__local/A/97/FD/5D777E6479CF8CD056DBE497D4A_E3F10CDF_9C592F.pdf?e=.pdf) | GGD-based block rate estimator + transform-domain distortion estimator: **~32% encode-time saving, negligible quality loss** | **Y** | Directly usable to replace `3+2·MSB` with a calibrated analytic R (§4.3) |
| Po et al., *Fast bit rate estimation for H.264 mode decision* (5 CAVLC token classes) | 2007 [PDF](https://www.ee.cityu.edu.hk/~lmpo/publications/2007_IEEE_TCSVT_Rate.pdf) | **34–47% encode-time saving**, negligible quality loss | **Y** | Same idea; our exact-CAVLC prototype was the opposite extreme |
| Wen et al., *Fast RDOQ for H.264/AVC*, DCC 2010 | 2010 [page](https://www.computer.org/csdl/proceedings-article/dcc/2010/05453532/12OmNBDyAaM) | Hierarchical coefficient filtering + top-M candidates; reduces 3¹⁶ combinations to a short list | **Y** | Blueprint for a bounded per-block candidate search in a shader |

**Takeaway:** 2020–2026 RDOQ literature is overwhelmingly **VVC/AVS3-dependent-quantization and
speed work**; the quality ceiling for H.264 CAVLC is the x264 ablation (**−0.650**) and our own
measured **+0.344** at unaffordable cost. Transfer the *pruning*, not the normatively different
quantizer.

### 2.2 Rate models: Cauchy / Gaussian / Laplacian / entropy models

| Work | Date / URL | Measured result [P] | Conf. | Expected here |
|---|---|---|---|---|
| Mao et al., *High Efficiency RC for VVC Based on Composite Cauchy Distribution*, IEEE TCSVT 32(4):2371–2384 | arXiv 2020-08-26 [PDF](https://arxiv.org/pdf/2008.11455); online 2021-06-29 [record](https://scholars.cityu.edu.hk/en/publications/high-efficiency-rate-control-for-versatile-video-coding-based-on-) | **1.03% BD-rate vs default RC**, **2.96% vs fixed QP** (low-delay); **1.29%** (RA), VTM-3.0 | **Y** (RC/QP only) | **[E] +0.05–0.25 VMAF @6000**, +0.03–0.15 @8000; 0.05–0.15 ms; low risk |
| Wang et al., *Transform coefficient distribution modelling based frame-level adaptive RC for VVC*, Displays 83:102726 | 2024-07 [record](https://www.sciencedirect.com/science/article/pii/S0141938224000908) | Cauchy + generalized-Gaussian PDFs per frame type; **0.72% (≤4K) / 1.47% (4K)** RD improvement, LDP | **Y** | Same order; relevant to our per-frame ABR, not coefficient RD |
| Chen et al., *RD optimization with adaptive content modeling for RA VVC*, Information Sciences 645 | 2023-10 [record](https://www.sciencedirect.com/science/article/abs/pii/S0020025523009106) | two-pass DMCRC statistics → global frame QP; VTM-4.0 | **Y** | Two-pass not available; single-pass per-MB version only |
| Li et al., *Learnable Laplace entropy model for differentiable rate estimation* | 2023-01-24 [arXiv:2301.09776](https://arxiv.org/abs/2301.09776) | Closed-form Laplace bit estimate, accurate on HEVC and H.264 | **Y** if used only to train/calibrate our R model | **[E] +0.05–0.3 @6000** if it improves R in group decisions; 0.1–0.4 ms |
| Duong et al., *DCT + learned quantization matrices + adaptive entropy coding for AV1 intra* | 2022-10-25 [PDF](https://arxiv.org/pdf/2210.14308) | Multi-rate transforms; "substantial BD-rate and perceptual improvements" vs nonlinear transforms at fraction of cost | N (AV1 entropy) | Only the "learned quant matrix" framing transfers |
| Ringis et al., *Per-clip / per-bitrate Lagrangian optimisation*, Trinity College Dublin | 2022-04 [per-clip](https://arxiv.org/html/2204.08965v1), [per-bitrate](https://arxiv.org/html/2204.09055v1) | Up to **21% BD-rate for one clip**; ~**2%** across a 2000-clip corpus; **180 encodes/clip** offline | **Y** | Offline calibration only — do not put a search loop in the encoder |
| Guo et al., *Block-level Lagrange multiplier adaptation based on distortion propagation factors*, APSIPA ASC | 2025-10-11 [PDF](https://www.apsipa.org/proceedings/2025/papers/APSIPA2025_P268.pdf) | **4.28% (LDB) / 4.47% (LDP) BD-rate** on VTM-23.4; **+4% encode time**; explicitly *smaller* at 1920×1080 | **Y** | Best literature support for §4.2; scale down for our 16×16 MBs: **[E] +0.2–0.5 @6000** |
| ACM Computing Surveys, *Towards Hybrid-Optimization Video Coding* | 2024-04-24 [record](https://dl.acm.org/doi/full/10.1145/3652148) | Online numerical optimization: **1.0% avg BD-rate all frames, 4.1% L1-only, up to 8.8%** | **Y** | Confirms λ adaptation is a live lever, not a solved one |

**Assessment [E]:** rate-model math is mature but each item is ~1% BD-rate; at VMAF ≈96 it is
worth a few tenths of a point. Its value here is **cheapness and composability**, not a step change.

### 2.3 Learned quantization — mostly non-conformant

| Work | Date / URL | Measured result [P] | Conf. | Verdict |
|---|---|---|---|---|
| VVC **dependent quantization** overview (Schwarz et al., IEEE TCSVT) | 2021-02-08 [PDF](http://www.ecodis.de/video/schwarz_tcsvt_vvc_rev2.pdf) | TCQ vs SDH: roughly **0.3–4.2% BD-rate** depending on class; encoder 101–111%, decoder ~97–101% | **N** | Normative 4-state quantizer switching — impossible in H.264 CAVLC |
| Li et al., *Deep Learning-based Image Compression with TCQ* | 2020-01-26 [arXiv:2001.09417](https://arxiv.org/html/2001.09417v1) | TCQ beats scalar quantizer at low rates, soft-to-hard training | **N** | Learned codec only; no H.264 mapping |
| DCVC-FM, *Neural Video Compression with Feature Modulation* | CVPR 2024 [PDF](https://openaccess.thecvf.com/content/CVPR2024/papers/Li_Neural_Video_Compression_with_Feature_Modulation_CVPR_2024_paper.pdf) | 11.4 dB quality range; **29.7% bitrate saving** + 16% MAC reduction vs prior NVC | **N** | Different codec entirely |
| *Emerging Advances in Learned Video Compression* (IJCAI-25 survey) | 2025-09-18 [PDF](https://www.ijcai.org/proceedings/2025/1165.pdf) | Quantization-aware training / fixed-bit activations needed for interoperability | **N** | Confirms no standard-conformant learned quantizer exists for us |
| Shi et al., *NeuroQuant* (INR video coding) | ICLR 2025 [record](https://proceedings.iclr.cc/paper_files/paper/2025/hash/f429c8a1472dc4dd2f8dd2f5b7ed8917-Abstract-Conference.html) | Up to **8× encoding speedup**, INT2 | **N** | Not a hybrid-codec tool |
| LAQP (MDPI Electronics 12(24):4905) | 2023-12 [record](https://www.mdpi.com/2079-9292/12/24/4905) | CNN per-MB QP from VMAF R–Q/D–Q; up to **5.82% bitrate saving / 4.28% VMAF gain vs x264**, 352×280–1280×720, CRF 29–37 | **Y** (QP offsets only) | Only transferable "learned" piece; see §4.2. Published gains are at much lower quality than our 96 |
| *Perceptual QP optimization for VVC with dual hybrid neural networks* | 2025-02 [record](https://link.springer.com/article/10.1007/s11227-025-06954-z) | CNN+ANN block QP; WPSNR/MS-SSIM/PSNR-HVS-M/VMAF | **Y** | Same family; 64×64/32×32 blocks, not our 16×16 |

**Verdict [E]:** no learned-quantization result in 2020–2026 gives a conformant H.264 step change.
The usable knowledge is **calibrated analytic rate models and per-block QP/λ predictions**, not
learned reconstructions.

### 2.4 Subband / perceptual distortion weighting (VIF- and ADM-driven)

| Work | Date / URL | Measured result [P] | Conf. | Relevance to our VIF-scale0 deficit |
|---|---|---|---|---|
| *Visual information fidelity based frame level rate control for H.265/HEVC*, Signal Processing: Image Communication | 2025-02 [record](https://www.sciencedirect.com/science/article/abs/pii/S0923596524001462) | Simplified VIF tied to MSE in the R–D loop; **3.4 / 4.0 / 3.3 average gains in PSNR / SSIM / VMAF** over default RC (units as reported: dB / dB / points) | **Y** | **Strongest direct support** for making `D` VIF-aware; VIF is the exact feature where we lose |
| Venkataramanan et al., *One Transform To Compute Them All* (VMAF atom fusion), arXiv:2304.03412 | 2023-11-18 [record](https://arxiv.org/html/2304.03412v2) | Defines **VIF-Aλ** (approximation subband) and **VIF-H/Vλ** (H/V wavelet subbands) with weights `αλ,θ = log2(1+σ²)`; shows VIF/ADM feature-level fusion | metric paper | Gives the **subband weighting formula** we can reuse for coefficient weights; VIF scale0 = finest λ |
| Li et al., *Bitrate Ladder Construction using VIF*, arXiv:2312.07780 | 2024-02-29 [record](https://arxiv.org/html/2312.07780) | VIF features + Extra-Trees; **−15.17% BD-rate / +3.66 BD-VMAF** vs fixed ladder | Pre / ladder | Shows VIF features carry independent information beyond PSNR; not an in-encoder tool |
| *Two-Stage Perceptual Quality Oriented Rate Control for HEVC*, ACM TOMM 20(5) | 2024-01-22 [record](https://dl.acm.org/doi/10.1145/3636510) | VMAF/JND two-stage RC | **Y** | Supports JND/VMAF-aware λ, but needs more compute than our budget |
| Yan et al. / LAQP family | 2023-12 [record](https://www.mdpi.com/2079-9292/12/24/4905) | VMAF-derived cubic R–Q and `D=1/VMAF` D–Q | **Y** | Per-MB weighting; see §4.2 |
| NVIDIA/Netflix, *VMAF-CUDA* | 2024-03-12 [blog](https://developer.nvidia.com/blog/calculating-video-quality-using-nvidia-gpus-and-vmaf-cuda/) | VIF/ADM/motion feature extractors on GPU in libvmaf 3.0 / FFmpeg 6.1; ≥2× metric speedup, 1424 fps single-frame at 1080p in example | measurement tool | Useful for **offline weight fitting** and NEG re-scoring; not in-loop per MB |

**Reading our diagnostic [E]:** VIF scale0 is the highest-resolution (finest) VIF scale and
measures preservation of information in fine detail; ADM2 is a coarser additive-detail metric.
A **low VIF scale0 with near-equal ADM2** means x264 is retaining fine-scale detail that we
quantize away, while we are not creating extra coarse artifacts. This favours
**per-coefficient rounding / per-subband distortion weights** (§4.1) over sharpening or coarse
block QP changes. It also warns against a pure PSNR/SSIM-driven QNS objective: our QNS must be
weighted toward high-frequency coefficients and validated on VIF scale0, not just VMAF.

### 2.5 Low-complexity transform selection

| Work | Date / URL | Measured result [P] | Conf. | Verdict |
|---|---|---|---|---|
| Malvar et al., *Low-Complexity Transform and Quantization in H.264/AVC*, IEEE TCSVT | 2003 (foundational) [PDF](https://mcube.lab.nycu.edu.tw/wiki/core/uploads/Research/SelectedTopics/Low-complexity%20transform%20and%20quantization%20in%20H.264_AVC.pdf) | Integer 4×4 transform, no multiplies; hierarchical DC transform | Y | Already implemented in our `transform.glsl` |
| Coelho et al., *Low-Complexity Loeffler DCT Approximations* | 2022-07-29 [arXiv:2207.14463](https://arxiv.org/pdf/2207.14463) | Embedded in x264; CIF videos, e.g. transform C3 **+7.48% BD-rate, −0.29 dB BD-PSNR**, C1 **+29.98% BD-rate** (positive = *loss*) | **N** (forward transform mismatch) | Approximate transforms lose quality; not worth it when our transform is already handful of adds |
| *Multiparametric low-complexity transforms*, Signal Processing / IEEE TCSVT 32(7) | 2020-11 / 2021-12 [record](https://www.sciencedirect.com/science/article/abs/pii/S0165168420302280), [doi](https://doi.org/10.1109/TCSVT.2021.3134054) | "close to or outperform" other approximations, not the exact DCT | **N** | Same |
| *Class of low-complexity DCT-like transforms*, arXiv:2206.00122 | 2022-12 [record](https://arxiv.org/html/2206.00122v2) | 8-/16-/32-point approximations, FPGA comparison | **N** | Not transferable: decoder must match |
| 8×8 FRExt transform hardware implementations | 2017 [PDF](https://dergipark.org.tr/en/download/article-file/381443) | Butterfly 200 MHz vs 83 MHz multiplier version | **Y** if normative 8×8 is already signalled | Speed reference only; our 8×8 path was reverted for syntax/decode reasons, not transform cost |

**Verdict:** low-complexity *transform math* is a dead end for us (+BD-rate loss, decoder
mismatch). The only live transform question is **when to use the normative 8×8 DCT**, and that is
worth at most **+0.21 VMAF** (x264 ablation) while being already reverted [L].

### 2.6 VMAF-optimised RD — with a metric-validity warning

| Work | Date / URL | Measured result [P] | Conf. | Verdict |
|---|---|---|---|---|
| Zhu et al., *HEVC VMAF-oriented Perceptual RDO using CNN*, PCS 2021 | 2021-11 [record](https://medialab.sjtu.edu.cn/publication/zhu-2021-hvp/) | **−2.80% VMAF-based BD-rate** on HM16.20 LDB, **+44% complexity**; online-trained CNN predicts block VMAF→SSE coefficient | **Y** (λ adjustment) | Concept transfers; 44% complexity and 64×64 blocks do not |
| Luo et al., *VMAF Oriented Perceptual Coding based on Piecewise Metric Coupling*, IEEE TIP 30:5109–5121 | 2021-05 [PubMed](https://pubmed.ncbi.nlm.nih.gov/33989154) | **3.61% bit saving (low-delay P)** and **2.67% (RA main)** for VMAF | **Y** | Best "VMAF→SSE coupling" reference; our §4.2 uses the same linearisation idea |
| MMSP 2020, VMAF-tailored compression pipeline | 2020-09 [record](https://ieeexplore.ieee.org/document/9287114) | **37% BD-rate reduction in VMAF** vs PSNR-optimised codec | **Pre + Y** | Large number is dominated by preprocessing; treat with suspicion |
| Li et al., *Differentiable VMAF*, ISCAS 2025 | 2025-05-28 [record](https://ieeexplore.ieee.org/document/11044280) | Differentiable approximation preserves VMAF, improves learned-codec VMAF at all rates | N (learned codec) | Training-time tool only |
| Li et al., *Learning Adaptive QP for Consistent Quality*, MDPI 2023 | 2023-12 [record](https://www.mdpi.com/2079-9292/12/24/4905) | Up to **5.82% bitrate / 4.28% VMAF** vs x264 at low res/CRF 29–37 | **Y** | See §2.3 |
| **Siniukov et al., Hacking VMAF and VMAF NEG**, ACM AICCC'21 | 2021/2022 [record](https://dl.acm.org/doi/fullHtml/10.1145/3508259.3508272), [arXiv](https://arxiv.org/abs/2107.04510) | Preprocessing raises VMAF by up to **218.8%** (avg 34–68% in table), VMAF-NEG up to **21.9% (ACM) / 23.6% (arXiv v2)**, with **subjective quality unchanged or worse** | Pre | **Reject sharpening/CLAHE/tone-map "gains"**; any candidate must be re-scored with VMAF-NEG/v1 |
| Netflix, *VMAF v1* | 2026-06-19 [blog](https://netflixtechblog.com/vmaf-v1-good-is-not-good-enough-60d7e4244ea8) | NEG behaviour on by default, new chroma feature; per-clip scores can move 1–3 points | measurement | **Pin model/version/NEG before optimizing** — a 1–3 point shift is the whole remaining gap |

**Verdict:** VMAF-driven *RD* (λ coupling) is legitimate and conformant; VMAF-driven
*preprocessing* is not a quality claim. Since our target is VMAF, use VIF-subband weighting to
improve the underlying fine detail, and **verify with VMAF-NEG/v1** so the gain survives.

---

## 3. Candidate scorecard — VMAF @6000 / @8000 and GPU cost

Anchors: x264 veryslow ablations (§0.2), our +0.344 exact-CAVLC prototype [L], 6000→8000
factor 0.55–0.70 from the CABAC sweep [L]. GPU budget: **~1–2 ms/frame**; current GPU stage sum
~6.5–7.2 ms, residual ~2.0–2.7 ms, entropy ~0.08–0.1 ms [L].

| # | Candidate | Conf. | Expected ΔVMAF @6000 **P** | @8000 **P** | GPU cost | Status |
|---:|---|---|---:|---:|---|---|
| 1 | **Subband-weighted pruned QNS (trellis-lite, 2 roundings/coef)** | Y | **+0.20…+0.40** (ceiling x264 trellis +0.65; local +0.344) | +0.12…+0.25 | **0.3–0.6 ms** (target ≤0.5) | Refined design open (`roadmap.md:73`); **not** the reverted exact-bit prototype |
| 1b | Cheap fallback: per-subband **rounding bias only**, no trellis | Y | +0.05…+0.15 | +0.03…+0.10 | <0.1 ms | New; must differ from rejected "AC 2/4 divisor split" |
| 2 | **VIF-proxy distortion-propagation λ** (no QP change) | Y | **+0.20…+0.50** (VVC 4.28–4.47% BD-rate scaled down; AQ caution) | +0.10…+0.30 | 0.1–0.4 ms | New; variance AQ (QP change) was negative, this is distinct |
| 3 | **Cauchy/Laplace analytic bit estimator** for `3+2·MSB` | Y | **+0.05…+0.20** | +0.03…+0.12 | 0–0.1 ms | New; older H.264 papers show 32–47% time saving, negligible loss |
| 4 | VMAF-linearised per-MB QP (LAQP-lite, CNN→tiny MLP) | Y | +0.20…+0.60 (published gains at far lower quality) | +0.10…+0.30 | 0.15–0.6 ms | Risk: our per-MB AQ path exists (`residual.comp:56-77`) but measured −1.28 |
| 5 | x264-style full reconstruction mode RD | Y | up to **+0.86** (ablation) | +0.5…+0.7 | **≫2 ms** (many re-encodes/MB) | **Excluded** for shader budget |
| 6 | Normative 8×8 DCT adaptive | Y | **+0.21** ceiling | +0.12…+0.15 | 0.3–1 ms + entropy/syntax | **Excluded**: tried, decode breakage |
| 7 | CABAC | N (excluded by user decision) | +0.35…+0.60 measured | +0.35 | ≥0.5 ms if exact chunk scheme works | **Excluded by scope** |
| 8 | VVC dependent quantization / TCQ / learned quantizer | N | up to 3.5% BD-rate in VVC | — | 0.2–1 ms | **Excluded**: decoder change |
| 9 | Low-complexity approximate DCT | N | **0 to negative** (published +7.48…+29.98% BD-rate) | — | ~0 | **Excluded** |
| 10 | VMAF preprocessing / sharpening / DPP | Pre | +0.3…+3 (v0), ≈0 under NEG/v1 | — | 0.1–0.5 ms | **Excluded**: metric exploit, subjective neutral/worse |
| 11 | Full in-loop VMAF feature computation | Y in principle | +0.2…+0.8 | +0.1…+0.4 | >2 ms even with VMAF-CUDA | **Excluded** by budget; use offline weighting only |
| 12 | Deblock α/β offsets (`Sensors`/Hanke) | Y | +0.05…+0.3 | +0.03…+0.15 | ~0 | Belongs to stage 04, not this note |
| 13 | AVS3/VVC RDOQ *parallelization pattern* | (pattern only) | **0** (speed tool; 0.25% BD-rate loss) | 0 | saves 29–37% of quant time | Adopt as implementation style, not as gain |

### 3.1 Hard exclusions — already tried and reverted [L]

Do **not** resubmit any of these without new evidence:

* **exact CAVLC bit-cost RD** (+0.344 VMAF, residual 1.8→2.7 ms) — `quality_baseline.md:104`.
  Candidate 1 is explicitly a *pruned, heuristic-rate* QNS with one final exact-count check, not
  this prototype. If the parent interprets "trellis" as wholly excluded, promote 1b and 3.
* **naive trellis at the old speed budget** — `improvement_roadmap.md:73`.
* **AQ (`--aq 1`)**: −0.63 PSNR / −1.28 VMAF — `quality_baseline.md:143`.
* **psy-RD**: negative — `improvement_roadmap.md:79`.
* **I4×4 intra**: bitstream failures, 1080p IDR 65.7→352 ms — `quality_baseline.md:150-166`.
* **8×8 inter transform**: decode breakage — `quality_baseline.md:103`.
* **AC divisor 2, DC divisor 1, low-freq 2 / high-freq 4 split, P-skip factors, flat-intra gate,
  ipratio offset, chroma QP offset, ABR RC constants/debt variants, SATD 4→8, SATD subpel,
  older-ref subpel, MV cost ×4/×0, more refs, veryslow as default** —
  `quality_baseline.md:91-105, 132-148, 194-213`; `improvement_roadmap.md:89-95`.
* **CABAC** — user decision + measured 23 fps prototype / 117 ms IDR.

---

## 4. Top 3 transferable to our shaders (≤1–2 ms/frame)

All three are **H.264-conformant**: they change only encoder decisions; the 4×4 integer
transform, divisor-3 quantizer and CAVLC writer stay untouched. All three target the measured
**VIF-scale0 fine-detail loss** while leaving ADM2 alone.

### Rank 1 — Subband-weighted pruned QNS with a single exact-count check
**Expected:** +0.20…+0.40 @6000, +0.12…+0.25 @8000. **GPU:** 0.3–0.6 ms target.

Sketch (algorithm, not code):
1. Work inside the existing residual dispatch (`shaders/residual.comp:198-232`), where lane
   <16 already holds the 16 forward-4×4 coefficients of one luma block, the quantized levels
   `coefficients[]`, and `estimatedBits[lane]`. No extra kernel launch.
2. For each coefficient with `|q| ≥ 1`, evaluate **two** candidate levels `{q, q−1}` (x264 also
   skips `q+1`, `rdo.c:811-814`). Reconstruction error `d²` in the dequantized domain, weighted
   by a 16-entry subband table `w[k]` (x264 uses `x264_dct4_weight2_tab`, `rdo.c:833, 928-929`).
   **Set `w[k]` from VIF scale0 sensitivity**, i.e. higher for mid/high-frequency `k ≥ 5`,
   rather than from PSNR; fit offline against VIF scale0 (Venkataramanan's `αλ = log2(1+σ²)`
   form, §2.4, is a reasonable starting prior).
3. Rate proxy per candidate: keep `3+2·MSB(level)` but add two cheap corrections learned from
   offline x264 data — trailing-one bonus and `total_zeros`/`run_before` penalty (the 5-token
   CAVLC model of Po et al. 2007 / Zhao et al. 2010, §2.1). **Never re-run the exact CAVLC
   writer per candidate** — that is the reverted prototype.
4. Cost `J = w[k]·d² + λ_trellis·ΔR` with a separate trellis λ (x264 uses a *different* λ² for
   inter/intra, `tables.c:129-166`), not the group-decision λ at `residual.comp:315`.
5. After the block's level vector is chosen, perform **one** exact CAVLC bit count in the
   existing entropy stage; if the measured rate exceeds the proxy by >2 bits/MB, flip at most
   one coefficient (highest `ΔR/ΔD` ratio) and stop. Bound: **≤16 coefficient flips/MB**.
6. Gate to inter P MBs with nonzero luma residual and `qp ≤ 40`; skip IDRs and flat/skip MBs in
   the first revision, so the IDR wavefront gain (PR #513) is untouched.
7. Acceptance gates: VMAF ≥ +0.3 @6000 with rate within ±5%; **VIF scale0 must rise** (target
   0.52 → ≥0.58) with ADM2 unchanged; residual stage ≤ current +0.5 ms; loop ≥100 fps;
   `ffmpeg -err_detect explode` clean; ctest 45/46.
8. Risk: highest of the three — this family was reverted twice. Mitigation: the pruned search
   and heuristic R make it a different algorithm; if it exceeds 0.6 ms or fails the VIF gate,
   ship Rank 1b (per-subband rounding bias only, <0.1 ms) instead.

### Rank 2 — VIF-proxy distortion-propagation λ (rate-neutral, no QP change)
**Expected:** +0.20…+0.50 @6000, +0.10…+0.30 @8000. **GPU:** 0.1–0.4 ms.

Sketch:
1. Build a per-MB weight from **features we already compute**: `lumaActivity` variance
   (`residual.comp:56-77`), luma DC energy from `dc[]`, I16 SATD from `modeCost[]` (`:167-180`),
   and a reference-reuse count from the 5-ref history. Normalize so the frame mean of `w_mb`
   is exactly 1.0 — this is what distinguishes it from the reverted variance AQ (which moved
   QP and broke ABR).
2. Clip `w_mb` to `[0.7, 1.4]`; prefer high weight where fine detail is present **and** the block
   will be reused by future P frames (the distortion-propagation factor of APSIPA 2025, §2.2).
3. Apply it only to the distortion side of decisions already in `residual.comp:315-345`:
   `predicted = 0.5·w_mb·predictionError[group]`, and likewise in the P-skip test `:342`.
   Equivalent form: `λ_eff = λ / w_mb`. **Do not touch `qp` or `mbSyntax[mb].y`** — no rate
   controller interaction, hence no ABR re-spend.
4. Optionally use a one-layer linear predictor on the same features (4–8 inputs) fitted offline
   to VIF scale0; a literal patch CNN over 8160 MBs would cost 1–5 ms and does not fit.
5. Acceptance: +0.3 VMAF @6000, VIF scale0 up, PSNR within −0.1 dB, rate within ±5%,
   group-decision stage <0.4 ms. Validate with VMAF-NEG and v1 as well as v0.6.1.

### Rank 3 — Cauchy/Laplace-calibrated analytic bit estimator
**Expected:** +0.05…+0.20 @6000, +0.03…+0.12 @8000. **GPU:** 0–0.1 ms (pure integer/LUT).

Sketch:
1. Replace `3 + 2·findMSB(|level|)` (`residual.comp:227, 259`) with a small table of
   `R(level, qp, context)`. Fit `R ≈ a(qp)·log2(1 + b(qp)·|level|)` (Cauchy/GGD R–Q model,
   Mao 2020 / Wang 2024, §2.2) or the closed-form Laplace bit estimate (arXiv:2301.09776),
   using offline x264 + Vk264 logs at QP 14–40. 18 `qp%6 × category` entries, no new buffer.
2. Add the CAVLC token corrections: `coeff_token` length from `(TotalCoeff, TrailingOnes)`,
   `total_zeros` and `run_before` from the reverse scan (Po et al., §2.1). These are **analytic
   estimates**, not exact bit generation, so they do not repeat the reverted work.
3. Use the same estimator in `approximateMvBits` (`residual.comp:33-36`) and `motionCost`
   (`spatial.comp:54-57`) so MV and residual bits are on one scale.
4. Zero extra kernel cost; pure ALU. Acceptance: measurable group/P-skip decision change,
   +0.1 VMAF or +0.1 dB at matched rate, rate within ±5%, bitstream decode clean.

### Why this order, given VIF scale0 ≈0.52 vs 0.68 and ADM2 equal
* The deficit is **information loss at the finest scale**, so the first lever must act **per
  coefficient** (Rank 1), not per MB or per frame. QNS with high-frequency distortion weights
  directly preserves the DCT coefficients that feed VIF scale0.
* The second lever (Rank 2) steers *where* bits go without changing QP, avoiding the ABR/AQ
  failure mode already measured; it amplifies the first lever on detail-heavy, frequently
  referenced MBs.
* The third lever (Rank 3) fixes the `R` term in the existing decisions — a prerequisite for
  both levers to choose correctly — at essentially zero cost.
* Deliberately **not** ranked: sharpening/preprocessing (would raise VIF/ADM features without
  genuine detail), full mode RD (too slow), 8×8 DCT (reverted), learned/VVC quantizers
  (non-conformant).

---

## 5. Validation protocol and caveats [L/E]

1. **Pin the metric first.** VMAF v1 (2026-06-19) enables NEG by default and can move scores
   1–3 points; re-score existing 6000/8000 bitstreams with v0.6.1, v1 and NEG before trusting any
   <0.5-point candidate. VMAF-CUDA (libvmaf 3.0 / FFmpeg 6.1) makes this cheap.
2. Benchmark command from `docs/improvement_roadmap.md:99-110`, masters `native1080_300.yuv`
   (25 fps game) and `obs_new_src.yuv` (60 fps capture), rates 6000/8000, `--gop 125 --qp 14`.
3. Gates: VMAF ≥ baseline +0.3 **or** PSNR +0.3 dB at matched rate; bitrate within ±5%;
   loop ≥100 fps, process ≥85 fps; ctest 45/46; `check.py` 13/13; decode clean.
4. Track **VIF scale0, ADM2 and VMAF-NEG alongside VMAF** for every candidate — the whole
   rationale for Rank 1/2 rests on fixing scale0 without inflating ADM.
5. Do not add gains arithmetically: x264's own ablations are non-additive (+0.279 sum vs
   +0.558 actual, §0.2). A realistic stack of Ranks 1–3 is **[E] +0.4…+0.9 VMAF @6000**
   (96.15 → ~96.6–97.0 on the 25 fps game clip), **+0.2…+0.5 @8000**. This does **not** close
   the gap to x264-slow (97.99/98.31) without structural tools (B-frames/lookahead/CABAC),
   all of which are outside this stage and the user's constraints.

---

## Source list (URL | date | label)

**Local (read-only):** `E:\__PROJECTS__\Vk264\docs\quality_baseline.md`,
`docs/improvement_roadmap.md`, `shaders/{residual.comp,spatial.comp,scan.comp,transform.glsl}`;
`E:\__PROJECTS__\Vk264\x264-vulkan/{encoder/analyse.c,encoder/rdo.c,encoder/cavlc.c,
encoder/macroblock.h,common/{base.c,set.c,quant.c,tables.c,macroblock.h}}`;
`E:\__PROJECTS__\Vk264-CABAC-Lab\notes\{MASTER.md (pipeline/),CABAC_VS_CAVLC_SWEEP.md,SPECULATIVE_HYBRID.md}`;
`C:\Users\multi\AppData\Local\Temp\opencode\stateoftheart\RESEARCH.md`.

**Web (all treated as untrusted data; only citations/numbers used):**
- Wang et al., low-complexity TCQ — https://arxiv.org/abs/2008.11420 — 2020-08-26 (TIP 2021)
- Mao et al., composite Cauchy RC — https://arxiv.org/pdf/2008.11455 — 2020-08-27 (TCSVT 32(4), 2022)
- Luo et al., VMAF piecewise metric coupling — https://pubmed.ncbi.nlm.nih.gov/33989154 — 2021-05-20
- Zhu et al., HEVC VMAF-oriented PRDO — https://medialab.sjtu.edu.cn/publication/zhu-2021-hvp/ — 2021-11-04 (PCS 2021)
- Siniukov et al., hacking VMAF/NEG — https://arxiv.org/abs/2107.04510 — 2021-07-09 / ACM 2022-03-17
- Coelho et al., Loeffler DCT approximations — https://arxiv.org/pdf/2207.14463 — 2022-07-29
- Ringis et al., per-clip/per-bitrate λ — https://arxiv.org/html/2204.08965v1 and https://arxiv.org/html/2204.09055v1 — 2022-04-19
- Venkataramanan et al., VMAF atom fusion — https://arxiv.org/html/2304.03412v2 — 2023-11-18
- LAQP, MDPI Electronics 12(24):4905 — https://www.mdpi.com/2079-9292/12/24/4905 — 2023-12
- Chen et al., adaptive content modelling RA VVC — https://www.sciencedirect.com/science/article/abs/pii/S0020025523009106 — 2023-10
- Disney Research, encoder autotuning — https://studios.disneyresearch.com/app/uploads/2024/09/Efficient-Video-Encoder-Autotuning-via-Offline-Bayesian-Optimization-and-Supervised-Learning-Paper.pdf — 2024-08-14
- Tang et al., SIMD AVS3 parallel RDOQ — https://www.telecomsci.com/rc-pub/front/front-article/download/64852539/lowqualitypdf/%E5%9F%BA%E4%BA%8ESIMD%E7%9A%84AVS3%E5%B9%B6%E8%A1%8C%E7%8E%87%E5%A4%B1%E7%9C%9F%E4%BC%98%E5%8C%96%E9%87%8F%E5%8C%96%E7%AE%97%E6%B3%95.pdf — 2024-07-15
- Sheng et al., parallel Viterbi DQ FPGA — https://www.jstage.jst.go.jp/article/transinf/E107.D/7/E107.D_2023EDP7243/_article/-char/en — 2024-07
- Wang et al., transform-coefficient-distribution RC — https://www.sciencedirect.com/science/article/pii/S0141938224000908 — 2024-07
- Wieckowski et al., fast dependent quantization — https://eurasip.org/Proceedings/Eusipco/Eusipco2024/pdfs/0000516.pdf — 2024-08-23
- NVIDIA, VMAF-CUDA — https://developer.nvidia.com/blog/calculating-video-quality-using-nvidia-gpus-and-vmaf-cuda/ — 2024-03-12
- Durbha et al., bitrate ladder using VIF — https://arxiv.org/html/2312.07780 — 2024-02-29
- Two-stage perceptual RC, ACM TOMM 20(5) — https://dl.acm.org/doi/10.1145/3636510 — 2024-01-22
- Guo et al., block-level λ / DPF — https://www.apsipa.org/proceedings/2025/papers/APSIPA2025_P268.pdf — 2025-10-11
- Li et al., differentiable VMAF, ISCAS — https://ieeexplore.ieee.org/document/11044280 — 2025-05-28
- VIF frame-level RC for HEVC — https://www.sciencedirect.com/science/article/abs/pii/S0923596524001462 — 2025-02
- Mediatek, hardware-friendly RDOQ, US 12,395,640 B2 — https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12395640 — 2025-08-19
- Netflix, VMAF v1 — https://netflixtechblog.com/vmaf-v1-good-is-not-good-enough-60d7e4244ea8 — 2026-06-19
- Ozer, x264 preset/per-title study — https://streaminglearningcenter.com/articles/optimizing-x264-settings-and-per-title-ladders.html — 2026-09-25
