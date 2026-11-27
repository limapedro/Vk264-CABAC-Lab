# X264-SLOW PARITY ROADMAP — Vk264 at 1080p60 / 6000–8000 kb/s

**Research only. No source edits, no PRs, no parent-repo changes.**
Compiled 2026-11-27. Parent `E:\__PROJECTS__\Vk264` was read-only at git HEAD
`2b586c6` (`git status --porcelain` empty before and after). This file is the
only artifact written. All web content used earlier in the lab was treated as
untrusted data; only bibliographic facts and reported measurements are carried
forward here.

**How to read the numbers.** `[L]` = measured in the parent repo or this lab;
`[B]` = the task brief supplied with this request (numbers not present in the
parent docs); `[P]` = published measurement; `[E]` = my extrapolation. All VMAF
numbers are v0.6.1 unless stated (the harness passes no `version=` option —
`harness/benchmark.py:52`; `notes/pipeline/MASTER.md:40-48` warns VMAF v1 can
move scores 1–3 points).

---

## 0. Bottom line

1. **The 1080p60 target is not met, and no parameter or shader tweak will meet
   it.** At matched rate the task brief has wx264 fixed-QP40 = **8.55 Mb/s /
   VMAF 76.38** against x264-slow **~8–10 Mb/s / VMAF 86.3** `[B]` — a ~9.9
   VMAF structural gap. The parent docs' closest 1080p60 audits agree in kind:
   trailer fixed-QP40 = **5872 kb/s / 80.39** vs x264 @6000 = **95.43**
   (`docs/quality_baseline.md:288-300`); 1080p60 game ABR = **88.70 @6938
   kb/s** vs x264 **95.72**, with the fixed-QP ceiling only **91.57 @6121
   kb/s** (`quality_baseline.md:302-304,330-332`).
2. **CABAC is excluded by user decision.** Even if that decision were reversed,
   the measured slice-parallel prototype runs at **~23 fps**, **14 µs/MB**,
   **117 ms** for a whole-frame 1080p IDR (`quality_baseline.md:168-184`); the
   currently plausible conforming exact chunk-summary design projects a
   **2.9–5.9 ms serial prefix** (`notes/03_ENTROPY.md:131-139`,
   `notes/SPECULATIVE_HYBRID.md:182-218`), i.e. still a large cost relative to
   today's **0.08–0.1 ms** parallel CAVLC stage.
3. **B-frames need a dual-reference ABI plus host DPB reordering**; without a
   lookahead/slicetype path they *lose* **0.64 VMAF** (`quality_baseline.md:338-341`;
   `docs/improvement_roadmap.md:78`). MB-tree and scene-cut decisions depend on
   the same lookahead (`x264-vulkan/encoder/slicetype.c:1675-1678`, `:1384-1473`).
4. **Trellis/QNS is confirmed blocked** (6 variants reverted byte-identically;
   best V3 = **+0.035 VMAF / +0.016 dB** for **+0.726 ms** residual and
   **−15 fps**, `improvement_roadmap.md:73`). The earlier exact-CAVLC-RD
   prototype gained **+0.123 dB / +0.344 VMAF** but cost residual **1.8→2.7 ms**
   and was reverted (`quality_baseline.md:104`). **AQ/psy actively hurt**
   (`--aq 1`: **93.96→93.06** at 1080p/6000, `quality_baseline.md:100`;
   **−0.63 dB / −1.28 VMAF** on 300f, `:143`; psy negative,
   `improvement_roadmap.md:79,92`). Do not re-attempt any of these.
5. **Realistic ceiling without a structural rewrite:** **~93–94 VMAF on 1080p60
   game content** and **~76–80 on the user's capture** `[B]` — i.e. below
   x264-slow's 86.3 on the brief capture and below x264-slow 95.4–98.3 on the
   game/capture masters in the docs. On the *easier 25 fps* game clip the
   current encoder already reaches **96.15 @6000 / 97.11 @8000** vs x264
   **97.99 / 98.31** (`improvement_roadmap.md:24-28`); that number must not be
   used to claim 1080p60 parity. On `obs_new_src.yuv` the verified fixed-QP
   codec ceiling is **92.372 @7646.8 kb/s** (medium; slow 92.376 @7485.8),
   versus x264-slow **94.76 @8000** (`improvement_roadmap.md:135-142,45-50`).
6. **No stack of conformant, ≤2 ms shader changes closes the gap.** Stacking the
   three best untried items gives **[E] +0.4…+0.9 VMAF @6000**
   (`notes/pipeline/MASTER.md:30-38`, `notes/02_RD_TRANSFORM_QUANT.md:421-424`)
   and x264's own tool ablations are **non-additive** (individual +0.279 vs
   actual +0.558, `improvement_roadmap.md:153-159`).

**Sequence (detail in §3):** Phase 0 measurement hygiene → Phase 1 cheap
conformant decisions (exact bit cost, VIF-proxy λ, deblock offsets) → Phase 2
offline QP plan / rate legality → **scope gate** → Phase 3 frame-parallel +
dual-ref ABI + B-frames/lookahead (CABAC only if the exclusion is lifted) →
Phase 4 full reconstruction RD. Phases 1–2 preserve **132–140 fps loop /
93–103 fps process** (`improvement_roadmap.md:15-21`); Phase 3 risks dropping to
**60–68 fps at N=8 or 105–120 fps at N=16** (`notes/FRAME_PARALLEL.md:360-369`).

---

## 1. Complete x264-slow tool inventory

x264-slow is defined in `x264-vulkan/common/base.c:557-564` on top of the
`x264_param_default` block at `:383-459`. `medium` is the unchanged default
(`:553-556`); "slow" changes only five fields: `subpel_refine 7→8` (`:559`),
`frame_reference 3→5` (`:560`), `direct_mv_pred spatial→auto` (`:561`),
`trellis 1→2` (`:562`), `lookahead 40→50` (`:563`). Everything else it uses is
inherited from the defaults. The full inventory:

| # | x264-slow tool | x264 setting / source line | Where x264 implements it | Vk264 status | Measured contribution (our ablations unless noted) | Cost / feasibility at 1080p60 |
|---:|---|---|---|---|---|---|
| 1 | **B-frames** | `i_bframe=3` `:387`; B-pyramid `:391`; adaptive B `:389`; slow keeps 3 B | `encoder/slicetype.c:1551-1621` (B-adapt), `:1623-1645`, `:1745-1975` (frame-type finalisation) | **Missing / blocked**: no B path, `b_frames=0` (`quality_baseline.md:38-39`), I/P only (`improvement_roadmap.md:11-13`) | −0.411 VMAF when removed from veryslow `[L]` (`improvement_roadmap.md:155`); −0.04 dB PSNR on 240f Warzone (`quality-parity.md:45`); **−0.64 VMAF if enabled without lookahead** (`quality_baseline.md:338-341`) | **XL**: needs dual-reference ABI, host DPB reorder and slicetype analysis. Not feasible in the current single-ref, decode-order pathway. |
| 2 | **CABAC** | `b_cabac=1` `:399`, `cabac_init_idc=0` `:400`; slow does not change it | `common/cabac.h:30-52`; `common/cabac.c:30-183` (460 contexts, `low/range/queue/outstanding`, carry patches `p[-1]` at `:89`); reset per slice `encoder.c:2791-2804` | **Tried / reverted; also user-excluded**: native slice-parallel prototype failed `err_detect explode` (intra mode −1, `cabac_init_idc` overflow) and ran ~23 fps (`quality_baseline.md:168-184`) | −0.552 VMAF when removed from veryslow `[L]`; 81-pair sweep: **+1.738 @2000, +0.603 @6000, +0.350 @8000**, −3.04…−3.30 % bits (`CABAC_VS_CAVLC_SWEEP.md:22-29`); **+0.02…+0.03 on `obs_new` @6000, ~0 @8000** (`:40-45`); +0.98 dB PSNR vs CAVLC/trellis-off (`quality-parity.md:59-61`) | **XL / excluded**. Even the exact chunk-summary design needs a **2.9–5.9 ms serial prefix** (`03_ENTROPY.md:131-139`); frame-parallel CABAC reaches only 105–120 fps at N=16 with ~3.8 GiB scratch (`FRAME_PARALLEL.md:360-369,386-396`). |
| 3 | **Trellis / RDOQ** | `i_trellis=1` default `:455`, **slow→2** `:562` | `encoder/rdo.c:375-641` (CABAC trellis), `:760-895` (coefficient loop), `:897-919` ("gigantic hack"), `:920+` (`quant_trellis_cavlc`) | **Tried / reverted (confirmed blocked)**: 6 variants (`improvement_roadmap.md:73`); exact-CAVLC RD reverted (`quality_baseline.md:104`) | −0.650 VMAF when removed from veryslow `[L]`; our exact-CAVLC prototype **+0.123 dB / +0.344 VMAF** but residual **1.8→2.7 ms**; best pruned V3 **+0.035 VMAF / +0.016 dB**, **+0.726 ms**, **−15 fps**; +0.15 dB PSNR from trellis 1→2 (`quality-parity.md:61`) | **M–L but blocked in practice**: the only survivors are sub-gate bits-cost-only variants (`02_RD_TRANSFORM_QUANT.md:284-299`); do not resubmit the reverted prototypes. |
| 4 | **Full reconstruction mode RD / subme 8** | `i_subpel_refine=7` `:444`, **slow→8** `:559` | `encoder/me.c:32-50` (subme8 = `{0,0,4,10}` hpel/qpel RD iterations), `:1233-1355` (qpel-RD refine); `encoder/analyse.c:982-1120` (`intra_rd`, `intra_rd_refine`), `:2547-2621` (`mb_analyse_p_rd`), `:2622-2772` (`mb_analyse_b_rd`), `:3256-3298`; `encoder/rdo.c:162-197` (`rd_cost_mb` actually encodes the MB, CAVLC bit count at `:187-191`) | **Missing**: Vk264 uses SATD+λ bits only (`shaders/residual.comp:90-188,295-345`; `shaders/spatial.comp:105-143`) | **−0.863 VMAF** — the single largest x264 tool `[L]`; smaller ME/subpel preset cost **−0.80 dB** (`quality-parity.md:47`); −1.13 dB when all constrained together (`:49`) | **XL / infeasible as a literal port**: ≈16 re-encodes/MB, **≫2 ms** (`notes/02_RD_TRANSFORM_QUANT.md:290-291`). An IDR-gated approximation is worth ≤+0.03 VMAF overall (`01_PREDICTION.md:188-189,247-277`). |
| 5 | **8×8 adaptive DCT** | `b_transform_8x8=1` `:454`; `analyse.intra/inter` I4/I8 `:435-437` | `encoder/analyse.c:2773-2809` (`mb_analyse_transform_rd`); `encoder/rdo.c:279-313`; High-profile syntax at `encoder/cavlc.c:555,685`, `encoder/cabac.c:973` | **Tried / reverted**: 8×8 inter caused decode breakage (`quality_baseline.md:103`); fork has `dct8x8.comp` but native path off | −0.210 VMAF when removed from veryslow `[L]`; 4×4-only costs **−0.38 dB** (`quality-parity.md:48`) | **L / high risk**: needs full High-profile syntax + neighbour reconstruction; measured decode failures, no clean gain. |
| 6 | **I4×4 / I8×8 intra** | `analyse.intra = I4x4\|I8x8` `:435` | `encoder/analyse.c:485-528,864-979`; `common/predict.c:478-621` | **Tried / reverted**: I4×4 syntax + `ffmpeg -err_detect explode` failures, IDR residual **65.7→352 ms** (`quality_baseline.md:150-166`) | no gain measured `[L]`; restricted-tools ablation with partitions off −1.58 dB (`quality-parity.md:49`) | Reverted; do not retry without a full syntax-path rewrite. |
| 7 | **MB-tree** | `b_mb_tree=1` `:427` | `encoder/slicetype.c:1675-1678` (+ path code `:1288-1383`); QP offsets at `:1032-1042`; `encoder/ratecontrol.c:760-766,1994-1998` | **Missing / blocked**: no lookahead, no B/DPB (`improvement_roadmap.md:11-13`) | −0.166 VMAF when removed from veryslow `[L]`; included in the −1.58 dB restricted-tools run | **XL**: requires lookahead propagation and B/reference structure. |
| 8 | **Lookahead / slicetype decision** | `i_lookahead=40` `:417`, **slow→50** `:563`; `scenecut_threshold=40` `:388`; `bframe_adaptive=FAST` `:389` | `encoder/slicetype.c:836-1000,1186-1287` (cost + VBV), `:1384-1473` (scenecut), `:1551-1645` (B decision), `:1745-1975` (finalise) | **Missing**: `lookahead_frames=0`, periodic GOP (`quality_baseline.md:38-39`; `src/presets.hpp:64-68`) | B-frames *without* lookahead lose 0.64 VMAF `[L]`; x265 lookahead study +0.617 dB to +1.705 dB BD-PSNR `[P]` (`notes/05_RATE_CONTROL.md:100`); a hard cut costs ~596 kB ≈20× a frame budget (`quality_baseline.md:239-247`) | **M for complexity-only** (all frames are preloaded — `src/main.cpp:202-224`), **XL for B-slicetype**. Offline QP plan is the only affordable subset (§3 Phase 2). |
| 9 | **Variance AQ** | `i_aq_mode=VARIANCE` `:415`, `f_aq_strength=1.0` `:416` | `encoder/ratecontrol.c:379-401`; MB-tree interaction `:760-766` | **Tried / reverted**: `aq` field exists but measured negative (`improvement_roadmap.md:79,92`) | **−0.63 dB / −1.28 VMAF** on 300f (`quality_baseline.md:143`); **93.96→93.06** @6000 (`:100`); **disabling AQ in x264 *gains* +0.241 VMAF** (`improvement_roadmap.md:155-159`) | **Do not retry.** A mean-normalised distortion weight (not a QP move) is a distinct, untried idea (`02_RD_TRANSFORM_QUANT.md:358-377`). |
| 10 | **Psy-RD / psy-trellis** | `f_psy_rd=1.0` `:440`, `b_psy=1` `:441`, `f_psy_trellis=0` `:442`; enabled only at subme≥6 (`encoder.c:1211-1219`) | `encoder/rdo.c:95-159` (`ssd_plane` + `psy·λ·|SATD(fenc)−SATD(fdec)|`), `:821-829,993-1000` | **Tried / reverted** (negative) | often *lowers* VMAF/PSNR; latest per-title study leaves psy at defaults for that reason (`02_RD_TRANSFORM_QUANT.md:105-109`) | Rejected for a VMAF target; no cost worth paying. |
| 11 | **Weighted P prediction** | `i_weighted_pred=SMART` `:452`, `b_weighted_bipred=1` `:451` | `encoder/slicetype.c:284-500` (`x264_weights_analyse`), applied in ME `encoder/me.c:76,903-904` and recon `encoder/analyse.c:219-244` | **Missing**: no `weighted_pred_flag`, no `pred_weight_table`, unweighted `lumaPrediction()` (`01_PREDICTION.md:69-82`) | **0 VMAF on our corpora** (no fades); 5–30 % bitrate saving on fade sequences `[P]` (ICIP 2008); 0.31/0.24 % BD-rate general content `[P]` (VCIP 2023) | **M / high syntax risk**: SPS/PPS + slice header + four shader paths; prior I4/CABAC attempts died on header integration. Not a fix for the measured gap. |
| 12 | **Subpixel ME / subme refinement** | see #4; `i_me_range=16` `:443`, `me_method=HEX` `:439`, `b_chroma_me=1` `:446`, `b_mixed_references=1` `:445` | `encoder/me.c:214-275` (predictor prep), `:865-992` (hpel/qpel diamond + SATD), `:1027-1190` (bidir), `:1233-1355` (RD) | **Partial**: quarter-pel 9-point refine (`src/presets.hpp:72`; `shaders/motion.comp:114-123`), no SATD inside subpel, no chroma ME | SATD in subpel **−0.020 VMAF**, motion 1.96→2.65 ms; older-ref subpel **+0.017/−0.113 VMAF**; MV penalty ×4/×0 **−0.093/−0.020** — all reverted (`quality_baseline.md:138-141`) | Cheap variants tried and failed; the remaining gain is the #4 reconstruction RD, not another search pattern. Literature ME papers are speed-only (`01_PREDICTION.md:127-133`). |
| 13 | **References** | `i_frame_reference=3` `:384`, **slow→5** `:560`; `b_mixed_references=1` `:445` | `encoder/analyse.c:1255-1332` (ref loop, ref cost, dupe bypass) | **In Vk264**: 5 refs (`src/presets.hpp:14,71`), with shaders `older_references.comp`; older refs cover **0.76 %** of P MBs (`quality_baseline.md:145-146`) / 1.9–2.8 % in the 600f study (`quality-parity.md:69`) | −0.147 VMAF when removed from veryslow `[L]`; 5 refs improved Warzone **32.43→32.65 dB** (`quality-parity.md:69`); more refs rejected (`improvement_roadmap.md:95`) | Already at slow parity. No further gain available without B-frames/dual-ref ABI. |
| 14 | **ME method / search** | `me_method=HEX` `:439`, `me_range=16` `:443`; slow does not change them | `encoder/me.c:320-419` (DIA/HEX), `:422-616` (UMH), `:618-771` (ESA/TESA); `common/mvpred.c:519-607` | **In Vk264 (parallel analogue)**: quarter-res pyramid + rings + 7×7/3×3 refine + 9-point diamond (`01_PREDICTION.md:43-44`); collocated temporal seed (`layout.glsl:82`) | SATD finalists 4→8: **+0.020 dB / −0.005 VMAF**, spatial 1.19→1.76 ms, process 86.5→82.1 (`quality_baseline.md:138`); another sweep 93.960→93.962 (`:102`). veryslow radius 32/8 finalists/16 refs: **93.95 vs slow 93.96** (`quality_baseline.md:72-77`) | Axis exhausted. Published TZ/FMECWT results save 35–68 % ME time at +0.08–0.30 % BD-rate — speed only (`01_PREDICTION.md:219-241`). |
| 15 | **Deblocking** | `b_deblocking_filter=1` `:395`, α/β offsets 0 `:396-397` | `common/deblock.c:32-76,277-337,378-415` | **In Vk264**: full-frame 4-tile wavefront, standard α/β/BS, QP<16 early-out (`04_FILTERING_POSTPROCESS.md:20-34`); α/β offsets currently ignored (`deblock_filter.glsl:99`) | Full-frame filtering fixed the slice-boundary error jump 4.18→2.17 luma (`quality-parity.md:18`); offset sweep predicted **≤0.05 VMAF** (`04_FILTERING_POSTPROCESS.md:94,205`) | Conformant, ~0 ms. Cheap test, not a gap-closer. |
| 16 | **Chroma QP offset** | `i_chroma_qp_offset=0` `:449` | `common/set.c` chroma table; `encoder.c:1585` | **Tried / reverted**: offset −2 with PPS signalling (`quality_baseline.md:98`) | 640p PSNR **48.13→48.05**, VMAF neutral → reverted | Zero-cost, no gain available. |
| 17 | **IP ratio / PB ratio** | `f_ip_factor=1.4` `:413`, `f_pb_factor=1.3` `:414` | `encoder/ratecontrol.c` QP scaling | **Tried / reverted**: explicit `ipratio=1.40` IDR offset (`quality_baseline.md:210-213`) | **95.30→95.00 @6000**, **96.67→96.47 @8000** — ABR already budgets IDRs, extra offset starves P frames | Rejected; no further work. |
| 18 | **RC: CRF/ABR, qcomp, VBV** | `i_rc_method=CRF` `:402`, `qcompress=0.6` `:423`, `qblur=0.5` `:424`, `complexity_blur=20` `:425`, `vbv=0` default `:405-407`; `i_qp_step=4` `:412` | `encoder/ratecontrol.c:202-215,1989-2000,2457-2517,2538-2559`; `doc/ratecontrol.txt` | **Partial / blocked**: reactive `scan.comp:24-82` (no predictive `bits(q)`, no EWMA, no qcomp); smoothed ABR is an **opt-in** (`scan.comp:52-68`); `vbv=0` (`quality_baseline.md:39`) | ABR instability costs up to **+8 VMAF** on trailer and **+2.9** on 60fps game at matched rate (`quality_baseline.md:294-300,330-332`); 13 adaptive-floor variants and 12 qcomp arms all reverted (`improvement_roadmap.md:68,71`); PR #505 debt clamp **95.719→96.149 @6000** (`quality_baseline.md:239-265`) | **M host-side**: offline plan-then-encode is affordable (§3 Phase 2). VBV not needed for preloaded local files (`05_RATE_CONTROL.md:59`). |
| 19 | **Scene-cut detection** | `scenecut_threshold=40` `:388` | `encoder/slicetype.c:1384-1473` | **Missing**: periodic GOP only (`src/presets.hpp:64-68`); PR #505 repaid a 596 kB cut over many frames | Cut frame costs ~20× a frame budget; clamp recovered **+0.43 VMAF** (`quality_baseline.md:239-265`) | Cheap host-side detection fits the Phase 2 plan; do not add B-slicetype. |

**Legend of statuses:** **in** = present at slow-equivalent budget; **partial** =
present in weakened form; **tried/reverted** = measured and rejected, do not
repeat without new evidence; **blocked** = architectural/scope blocker;
**missing** = absent and not yet attempted.

---

## 2. Missing tools: measured contribution, cost, feasibility

### 2.1 The structural four (B-frames, CABAC, MB-tree, lookahead/mode-RD)

These are the tools that account for essentially all of the x264-slow advantage.
**All four are outside the current architecture.**

| Tool | Contribution `[L]` | What it would take here | Feasible at ~100 fps + H.264 conformance? |
|---|---:|---|---|
| B-frames + lookahead | −0.411 B, −0.166 MB-tree, lookahead study +0.617 dB to +1.705 dB BD-PSNR `[P]` | dual-reference ABI, host DPB reorder, slicetype analysis, B-direct prediction (`01_PREDICTION.md:84-92`) | **No** in the current single-ref, decode-order pipeline. Needs a scope decision (§4). |
| CABAC | −0.552 veryslow; sweep +0.603 @6000, +0.350 @8000; ~0 on `obs_new` | 460 contexts + arithmetic state through every bin, carry patches previous byte (`common/cabac.c:69-99`); only slice boundaries reset (`encoder.c:2791-2804`) | **User-excluded**; even if allowed, native prototype 23 fps / 117 ms IDR, and frame-parallel N=16 tops out at 105–120 fps with ~3.8 GiB scratch. |
| Full mode RD / subme 8 | −0.863 veryslow | reconstruct each candidate MB and count real CAVLC bits (`encoder/rdo.c:162-197,187-191`), ~16 re-encodes/MB | **No**: ≫2 ms per frame; the ungated prototype already cost +0.9 ms for +0.344 VMAF and was reverted. |
| Trellis/RDOQ | −0.650 veryslow | per-coefficient reconstruction-aware rounding, 5 coupled CAVLC state machines (`rdo.c:897-919`) | **Confirmed blocked** (6 reverted variants; see §1 #3). Untried cheap subsets are bounded at +0.13–0.4 VMAF. |

**Non-additivity warning.** Removing all of them from veryslow costs +0.558
VMAF in aggregate, but the individual removal costs sum to +0.279; the
differences interact (`improvement_roadmap.md:153-159`). Never add the table
numbers to project a roadmap outcome.

### 2.2 Tried and failed — closed files

| Tool | Measured result `[L]` | Why it failed / why it stays closed |
|---|---|---|
| Exact CAVLC bit-cost RD | +0.123 dB / +0.344 VMAF, residual 1.8→2.7 ms (`quality_baseline.md:104`) | Below the ≥+0.3 VMAF gate *after* speed cost; reverted. A bit-count-only variant (no reconstruction loop) is the one surviving idea (`03_ENTROPY.md:264-300`). |
| Trellis/QNS (6 variants) | best +0.035 VMAF / +0.016 dB for +0.726 ms and −15 fps; full x264-style **−0.439 VMAF**, +1.744 ms (`improvement_roadmap.md:73`) | Variants move along the existing RD curve rather than improving it; pruned decisions cannot recover the +0.344 exact-RD gain without the full CAVLC state model. |
| AQ / psy | −0.63 dB / −1.28 VMAF (`quality_baseline.md:143`); −0.90 VMAF @6000 (`:100`); disabling AQ in x264 gains +0.241 (`improvement_roadmap.md:155`) | AQ moves QP and the ABR re-spends/oscillates; psy often lowers VMAF by design (`02_RD_TRANSFORM_QUANT.md:105-109`). |
| 8×8 inter / 8×8 transform / I4×4 | entropy 0.09→7.04 ms and −0.97 VMAF (`--partitions 1`, `quality_baseline.md:142`); decode breakage (`:103,150-166`) | Syntax path and neighbour reconstruction missing; prior attempts failed `err_detect explode`. |
| chroma QP −2 | PSNR 48.13→48.05, VMAF neutral (`quality_baseline.md:98`) | No gain. |
| ipratio offset | VMAF 95.30→95.00 @6000, 96.67→96.47 @8000 (`quality_baseline.md:210-213`) | ABR already gives IDRs a separate budget (`scan.comp:33-41`). |
| SATD 4→8 / SATD-subpel / older-ref subpel / MV penalty / more refs / veryslow default | all ≤+0.02 VMAF or negative with real fps cost (`quality_baseline.md:102,138-141`; `improvement_roadmap.md:95`) | Exhausted. |
| 13 QP-floor variants / 12 qcomp arms / larger debt window / wider aim cap / two-sided GOP controller | all reverted; best floor variant cost >10 VMAF on other masters; GOP-scaled controller **60.93/63.00 vs 72.30** baseline (`improvement_roadmap.md:68,71`; `quality_baseline.md:306-325`) | The failure is a *planning* problem, not a feedback-gain problem (`05_RATE_CONTROL.md:31-41`). |
| Native CABAC | ~23 fps loop, entropy 27.05 ms, 14 µs/MB, 117 ms IDR (`quality_baseline.md:168-184`) | Serial per-slice arithmetic chain; user decision excludes it regardless. |

### 2.3 Untried, cheap, conformant — the only things worth doing now

| Candidate | Expected `[E]/[L]` | GPU/host cost | Conformance / risk |
|---|---:|---|---|
| Exact bit-cost feedback to existing RD decisions (CAVLC VLC-length LUTs, no reconstruction loop) | **+0.13…+0.26 @6000**, upper bound +0.344 (our reverted full prototype) (`03_ENTROPY.md:367-380`; `quality_baseline.md:104`) | **<0.05 ms**, a few KB of LUT (`03_ENTROPY.md:377`) | Fully conformant (decision-only). Must differ from the reverted exact-RD prototype: no per-candidate CAVLC writer. |
| VIF-proxy distortion-propagation λ (mean-normalised per-MB weight from activity/SATD/ref-reuse; **no QP change**) | **+0.2…+0.5 @6000** (`02_RD_TRANSFORM_QUANT.md:358-377`; `MASTER.md:22`) | **0.1–0.4 ms** | Conformant; explicitly distinct from AQ (which moved QP and lost 1.28 VMAF). Validate with VIF scale0 + ADM2 + VMAF-NEG. |
| Deblock α/β offset sweep | **≤+0.05** (`04_FILTERING_POSTPROCESS.md:94`) | ~0 ms | Conformant, signalled in slice header. Half-day ceiling. |
| Offline probe-calibrated per-frame QP plan | rate legality **20.93→≈6.0 Mb/s** on `obs_new`; **+2.9** game / **+8.1** trailer at 60 fps `[L]` ceilings (`quality_baseline.md:294-300,330-332`; `05_RATE_CONTROL.md:187-200`) | host **0.1–3.4 s per 300f clip**; loop unchanged | Conformant: one `uint32 plannedQp` in `FrameConfig`, QP already signalled via `slice_qp_delta`. VMAF *falls* numerically on overshooting clips because we stop spending 3.5× the target. |
| Pyramid-confidence adaptive search range | **0 quality** (published +0.08…+0.30 % BD-rate), saves **0.5–1.2 ms** (`01_PREDICTION.md:219-241`) | −0.5…−1.2 ms | Conformant; speed budget only. |

**Explicit rejections for VMAF work:** unsharp mask / CLAHE / display sharpening
(up to +34 % VMAF v0 with subjective quality *worse* —
`04_FILTERING_POSTPROCESS.md:77,97`); SAO/ALF/neural in-loop filters (non-conformant
or decoder-optional, and worth ~+0.03…+0.07 VMAF at our slope, `:93-103`);
sharing a display filter with x264 but not applying it to the baseline (not a
matched-rate claim, `:74`).

---

## 3. Sequenced roadmap (phase 0–4)

**Standing gates for every phase** (`improvement_roadmap.md:99-110`): VMAF ≥
baseline +0.3 **or** PSNR +0.3 dB at matched rate; achieved bitrate within
±5 %; encode loop ≥100 fps; process ≥85 fps; `ctest` 45/46 (only the
pre-existing `x264_cavlc_packet_parity` failure); `check.py` 13/13;
`ffmpeg -err_detect explode` clean. Masters: `build/native1080_300.yuv` (25 fps
game), `build/obs_new_src.yuv` (60 fps capture), `build/obs_capture_src.yuv`
(easy capture), plus the task-brief 1080p60 capture; rates 6000/8000,
`--gop {125|120|300} --qp 14`.

### Phase 0 — Lock the measurement (no encoder change)

**Deliverables**
1. Re-score the current streams with **VMAF v0.6.1, v1 and NEG** and record
   `vif_scale0`, `adm2`, frame-level QP and achieved kb/s; pin model/version in
   the harness or results are not comparable (`MASTER.md:40-48`;
   `04_FILTERING_POSTPROCESS.md:14,82-85`).
2. Establish the **fixed-QP codec ceiling** for the brief capture at matched
   rate (brief: QP40 = 8.55 Mb/s / 76.38 `[B]`; docs: QP28 = 7.65 Mb/s /
   92.372 on `obs_new_src`, `improvement_roadmap.md:137-142`).
3. Record the current throughput baseline: loop **132–140 fps**, process
   **93–103 fps**, stage sum **6.5–7.2 ms** (motion 2.13, deblock 2.04, spatial
   1.09, residual 0.83, entropy 0.08 — `VULKAN_PERF.md:12-17`;
   `improvement_roadmap.md:15-23`).

**Expected cumulative VMAF:** unchanged (game 96.15 @6000 / 97.11 @8000;
brief capture 76.38 fixed-QP). **fps impact:** none.
**Gate:** baselines reproducible within ±0.05 VMAF and ±5 % rate; if v1/NEG
moves the score by >1 point, re-baseline the whole roadmap before proceeding.

### Phase 1 — Cheap conformant RD/externals (no ABI change)

**Deliverables (in this order)**
1. **Exact CAVLC bit-cost LUT** in the existing group/P-skip and I16 decisions
   (`shaders/residual.comp:180-181,227,255-261,295-345`,
   `shaders/flat_intra.comp:61,73`), fed by x264 VLC lengths
   (`encoder/cavlc.c:148-193`, `common/bitstream.h:221-248`). Target +0.13…+0.26
   VMAF, <0.05 ms, bitstream decode clean (`03_ENTROPY.md:367-380`). **Do not**
   reintroduce the reverted exact-RD reconstruction loop.
2. **VIF-proxy mean-normalised λ** on per-MB decisions only — clip `[0.7,1.4]`,
   do not touch `qp` or `mbSyntax[].y` (`02_RD_TRANSFORM_QUANT.md:358-377`).
   Target +0.2…+0.5; require VIF scale0 0.52→≥0.58 with ADM2 unchanged
   (`02_RD_TRANSFORM_QUANT.md:28,350-353`).
3. **Deblock α/β offset sweep** (≤half a day) using the signalled slice-header
   offsets currently ignored (`deblock_filter.glsl:99`;
   `04_FILTERING_POSTPROCESS.md:111-134`). Accept only if VMAF ≥ +0.05.
4. Optional speed bank: pyramid-confidence adaptive search range (quality
   neutral, −0.5…−1.2 ms) to buy headroom for #2 (`01_PREDICTION.md:219-241`).

**Expected cumulative VMAF:** game @6000 **96.15 → ~96.6–97.0** (non-additive
stack estimate +0.4…+0.9 from `MASTER.md:30-38`, with the conservative end
more likely); @8000 **97.11 → ~97.3–97.6**; brief capture **76.38 → ~76.8–77.3**
(small because the gap there is not RD-only). **fps impact:** ≈0 to −0.4 ms
(stage sum ≤7.6 ms); loop stays >100 fps, process 93–103 fps.
**Decision gate:** if the stack delivers <+0.3 VMAF at matched rate, stop
quality tuning and go to Phase 2 — no other ≤2 ms conforming idea is left
(`01_PREDICTION.md:211-217`, `MASTER.md:31-38`).

### Phase 2 — Rate legality via offline QP plan (host only)

**Deliverables**
1. Host planner after the YUV preload (`src/main.cpp:202-224`, before encoder
   construction at `:226`): 16×16-downsample features (SAD to i−1/i+1/i+2,
   variance, edge energy, duplicate/cut flags), 1–3 fixed-QP probe encodes on
   a 60f strided subsample, fit `bits_i(q)`, bisect a global λ so
   Σ bits = target, clamp to the accepted floor 18 at ≥50 fps, push
   `plannedQp` through `FrameConfig` (`src/gpu.hpp:14-23`) and
   `shaders/layout.glsl:55-57`; keep the ±allocation/4 debt clamp as a ±1
   safety net (`scan.comp:48-49`, `:70-81`).
2. Duplicate-cadence QP pinning as the fallback (the brief capture is 30→60
   duplicated: ~25 % of frames at 4× `aim`, the rest ~1/20 —
   `improvement_roadmap.md:68`).
3. Scene-cut reserve from Pass 0a features (the PR #505 lesson applied *before*
   the cut, `05_RATE_CONTROL.md:213-215`).

**Expected cumulative VMAF:** at matched rate no codec gain; the gain is
**rate legality**: game 60 fps ABR 88.70 @6938 kb/s → **≈91.6 @6121 kb/s**;
trailer 72.30 @7472 → **≈80.4 @5872** (`quality_baseline.md:330-332,288-300`;
`05_RATE_CONTROL.md:198-201`). Brief capture: 8.55 Mb/s overshoot → ~6 Mb/s,
but VMAF lands at the fixed-QP ceiling (~76–77 on that capture `[B]`; 92.1–92.4
on `obs_new_src`), not at 96.98. **fps impact:** encode loop unchanged
(132–140 fps), +0.1–3.4 s wall per 300f clip.
**Decision gate:** achieved rate ±5 % on all masters **and** VMAF ≥
fixed-QP ceiling −0.3, **and** no >2 VMAF regression on the 25 fps game master.
If the plan cannot hit ±5 %, ship duplicate pinning and stop — 13 floor
variants and 12 qcomp arms already failed (`improvement_roadmap.md:68,71`).

### Scope gate (between Phase 2 and Phase 3) — **explicit decision required**

Phases 0–2 are the entire achievable roadmap **inside the current scope**.
Everything beyond requires the user to choose one:

- **Option A (B-frames):** dual-reference ABI + host DPB reorder + lookahead.
  VMAF value +0.41…+0.86 (non-additive), no CABAC. Needs decode-order →
  display-order machinery and two refs per MB.
- **Option B (CABAC):** requires reversing the user exclusion *and* either an
  exact chunk-summary design (2.9–5.9 ms serial prefix, `03_ENTROPY.md:131-139`)
  or frame-parallel N=16 (105–120 fps ceiling, ~3.8 GiB scratch,
  `FRAME_PARALLEL.md:360-369,386-396`).
- **Option C (stop):** accept ~93–94 game / ~76–80 capture as the shipping
  ceiling and declare parity out of scope.

**The 100 fps process gate conflicts with both A and B**: frame-parallel N=8
gives only 60–68 fps, N=16 a *best case* 105–120 fps with 3.8 GiB and N-frame
RC lag that breaks bitstream identity (`FRAME_PARALLEL.md:360-369,251-271`).
A relaxation to ≥60 fps process, or a different GPU, is part of the decision.

### Phase 3 — Structural (only after the scope gate)

**Deliverables, if Option A is chosen**
1. Dual-reference reconstruction ABI (`reconSlots = max(N, R+1)`,
   `src/gpu.cpp:619-629`; `shaders/layout.glsl:89-95`) and host DPB reorder.
2. Host lightweight slicetype/lookahead from the already-preloaded frames —
   **B-frames must not be enabled without it** (−0.64 VMAF otherwise).
3. N-deep ring with per-frame `RateState` + host shadow RC folded in decode
   order (`gpu.cpp:324,522-548`; `FRAME_PARALLEL.md:251-271`); frame-parallel
   gates 0–7 in `FRAME_PARALLEL.md:454-497` must all pass in order.
4. Gate 3 mandatory before any entropy work: a single conformant CABAC lane
   failed `err_detect explode` on MB 0 in the earlier attempt
   (`quality_baseline.md:177-178`; `FRAME_PARALLEL.md:474-478`).

**Expected cumulative VMAF:** game @6000 **~97.0–97.8** (B +0.41,
MB-tree +0.166, refs +0.147, non-additive; still below x264 97.99 without
mode RD); brief capture **~77–79** vs x264-slow 86.3 — the gap remains.
**fps impact:** 60–68 fps at N=8; 105–120 fps at N=16 *only* if the work-
conservation bound allows it; VRAM 1.9–3.8 GiB. **Decision gate:** ≥60 fps
process (or the relaxed agreed floor), rate ±5 %, ≥−0.3 VMAF vs serial, clean
decode across ≥3 IDRs. A negative IDR-overlap measurement is decisive — stop.

**If Option B is chosen instead:** expect +0.35…+0.60 VMAF on detailed content
but ~0 on the brief capture (`CABAC_VS_CAVLC_SWEEP.md:31-45`); the 100 fps gate
is unlikely to hold. Do not attempt WPP/row-slice CABAC: H.264 has no context
carry, row slices cost **−0.439 VMAF** at 6000 and **+5.6…+7.5 % bits** at
matched QP (`notes/WPP_H264.md:16-20,188-224,230-248`).

### Phase 4 — Full RD (research horizon, not a shipping promise)

**Deliverable:** reconstruction-domain mode RD with real CAVLC bit counts
(`encoder/rdo.c:162-197`, `analyse.c:982-1120,2547-2621`) amortised over N
in-flight frames so the ≫2 ms/MB cost is hidden. This is the only remaining
tool worth **−0.863 VMAF** (`improvement_roadmap.md:155`).
**Expected cumulative VMAF:** game @6000 maybe **98+** if B/CABAC/MB-tree also
land and the ablations prove additive enough; brief capture **~80–83** — still
short of x264-slow 86.3. **fps impact:** unknown; likely below the current
gate even with N=16. **Decision gate:** treat as a rewrite project, not an
incremental PR. If it does not clear x264−1.5 VMAF at matched rate, declare
parity unattainable with this architecture.

---

## 4. What would actually close the gap

The minimum architecture that could plausibly reach x264-slow quality is
**frame-parallel + lookahead + (B-frames or CABAC) + reconstruction RD**, plus a
plan-then-encode controller. Nothing smaller works, for measurable reasons:

| Required element | Why it is unavoidable | Minimum effort / cost |
|---|---|---|
| **Frame-level parallelism** | CABAC state is wholly per-slice/per-frame (`common/cabac.c:30-67`; `encoder.c:2785-2801`), so frames are natural lanes; a single IDR entropy lane is **117 ms** (`14 µs/MB × 8160`, `FRAME_PARALLEL.md:327-330,339-344`). | N× scratch, N× Rate slots, N× command buffers/fences, `reconSlots = max(N,R+1)`, host shadow RC. **~1.9 GiB at N=8, ~3.8 GiB at N=16** (`FRAME_PARALLEL.md:246-249,386-396`). Gates 0–7 at `:454-497`. |
| **Dual-reference ABI + DPB reorder** | B-frames and weighted bi-prediction require two reference lists and display-order output; today decode order = display order (`improvement_roadmap.md:11-13`). | XL host + shader ABI change; no isolated cost estimate exists. **Without lookahead it loses 0.64 VMAF**, so the two must ship together. |
| **Lookahead / slicetype** | x264-slow's 50-frame lookahead (`base.c:563`) drives B decisions, MB-tree propagation (`slicetype.c:1675-1678`) and scene cuts (`:1384-1473`). | Complexity-only lookahead is cheap because frames are preloaded (`05_RATE_CONTROL.md:147-170`); B-slicetype is XL. |
| **CABAC (if the exclusion is lifted)** | −0.552 VMAF / −3 % bits; but ~0 on the brief capture (`CABAC_VS_CAVLC_SWEEP.md:31-45`). | Exact chunk-summary: 460-context prefix **2.9–5.9 ms** (`03_ENTROPY.md:131-139`), vs current CAVLC 0.08–0.1 ms. Frame-parallel variant tops out at **105–120 fps at N=16** (`FRAME_PARALLEL.md:360-369`). Speculative rollback is dead: **98.4 % conflict** at 64 bins (`SPECULATIVE_HYBRID.md:108-129`). CPU–GPU hybrid is a dead end (0.25–0.7 ms serial CPU + sync, and x264-slow already does 75–105 fps on that CPU, `:232-252`). |
| **Plan-then-encode RC** | Reactive `log2(rateRatio)` control cannot represent bimodal capture demand (`05_RATE_CONTROL.md:31-41`). | Host 0.1–3.4 s per 300f clip; no shader cost. |
| **Reconstruction RD** | Largest single tool (−0.863) and responsible for current fine-detail/VIF-scale0 deficit (`quality_baseline.md:186-191`). | ≫2 ms as a literal port; only affordable amortised across N frames. |

**Effort estimate for the whole architecture:** multiple engineer-months, with
two hard feasibility gates that can kill it at any point — (a) the 100 fps
process gate versus N=8/16 throughput bounds and (b) the N-frame RC lag, which
makes serial bitstream identity impossible (`FRAME_PARALLEL.md:430-450`). It
also needs a **scope decision** about the user's CABAC exclusion and the
acceptable fps floor; neither can be decided from measurements alone.

**What is *not* a route to parity:** more shader tuning (best measured single
change is +0.035 VMAF or −0.2 ms), filters (1.84 VMAF would need ~55 % BD-rate;
best literature filters are 0.13–4 % and mostly non-conformant —
`04_FILTERING_POSTPROCESS.md:88-105,203-209`), sharpening (VIF exploit, subjectively
worse — `:77`), or changing presets/refs (93.84/93.93/93.96/93.95 for
fast/medium/slow/veryslow — `quality_baseline.md:72-77`).

---

## 5. Citation index and caveats

**Parent docs (read-only):** `docs/quality_baseline.md` (negative experiments
`:91-105,132-184,194-213`; 1080p60 audits `:273-341`; speed breakthrough
`:111-130`), `docs/improvement_roadmap.md` (state `:9-13`; rankings `:66-79`;
rejections `:89-95`; gates `:99-110`; ablation `:153-159`; capture ceiling
`:135-142`), `docs/quality-parity.md` (x264 ablations `:41-63`).

**x264 fork (read-only):** `common/base.c:383-459` (defaults),
`:489-609` (preset ladder; slow `:557-564`); `encoder/me.c:32-50` (subpel
iterations), `:320-771` (search), `:865-992,1233-1355` (refine/RD);
`encoder/analyse.c:982-1120,2547-2621,2773-2809`; `encoder/rdo.c:162-197,375-641,
760-919`; `encoder/cavlc.c:148-204,487`; `encoder/slicetype.c:284-500,1032-1078,
1384-1473,1551-1645,1675-1678,1745-1975`; `encoder/ratecontrol.c:202-215,
379-401,1989-2000,2457-2559`; `common/cabac.c:30-183`; `common/cabac.h:30-52`;
`encoder/encoder.c:203-210,1211-1219,2791-2804`.

**Vk264 source (read-only):** `src/presets.hpp:14-15,64-79`; `src/gpu.hpp:14-23`;
`src/gpu.cpp:245-251,324,519-548,611-650`; `shaders/scan.comp:24-82`;
`shaders/layout.glsl:24,55-57,82,89-95`; `shaders/residual.comp:90-188,198-345`;
`shaders/spatial.comp:54-143`; `shaders/motion.comp:41-123`;
`shaders/transform.glsl:5-22`; `shaders/deblock_filter.glsl:39-105`.

**Lab notes:** `notes/pipeline/01_PREDICTION.md`, `02_RD_TRANSFORM_QUANT.md`,
`03_ENTROPY.md`, `04_FILTERING_POSTPROCESS.md`, `05_RATE_CONTROL.md`, `MASTER.md`,
`CABAC_VS_CAVLC_SWEEP.md`, `SPECULATIVE_HYBRID.md`, `FRAME_PARALLEL.md`,
`WPP_H264.md`, `VULKAN_PERF.md`.

**Caveats:** (1) All lab VMAF is v0.6.1; v1/NEG can move scores 1–3 points and
removed VIF as a core feature, which directly affects the `vif_scale0`-driven
diagnosis (`04_FILTERING_POSTPROCESS.md:78-84`). (2) Ablation gains are not
additive. (3) Task-brief numbers (`[B]`) are not present in the parent docs and
appear to describe a harder capture than `obs_new_src.yuv`; rates/ceilings from
the two corpora must not be mixed. (4) No code was written, no build run and no
parent file changed while producing this roadmap.
