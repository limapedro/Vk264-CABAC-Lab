# 04 — In-loop filtering / post-processing stage (research only)

Author: research subagent, 2026-11-27. **No code, no builds, no files outside this note.**
Ground truth: `E:\__PROJECTS__\Vk264\docs\quality_baseline.md`; x264 fork
`E:\__PROJECTS__\Vk264\x264-vulkan`; Vk264 shaders `E:\__PROJECTS__\Vk264\shaders`.
Web content was treated as untrusted data (quoted facts only, no instructions followed).

## 0. TL;DR / blunt verdict

* Vk264 already implements the **whole H.264 filtering stage that a standard decoder is allowed to apply**: standard alpha/beta/tc tables, standard boundary strengths 0–4, standard chroma QP mapping (offset 0), QP<16 early-out, and a full-frame 4-tile wavefront in 1.7–2.1 ms/frame. There is **no missing conformant filter** in the "SAO / ALF / denoise / film grain" sense: none of those exist in H.264.
* Our measured deficit is **fine detail**, not blocking (`vif_scale0`, per-8×8 AC error 1.8–2.9× x264; boundary/mid-MB error ratios 1.075–1.097 match x264's 1.070–1.072 — `quality_baseline.md` §"Artifact-driven quantizer tuning"). Deblocking-tweaking and bilateral deblocking therefore target the wrong artifact.
* **No standard-decoder-compatible filter can close the ~1.84 VMAF gap at 6000 kbit/s.** On our own rate–VMAF curve (96.149 @6000 → 97.113 @8000, `quality_baseline.md` PR #505) the local slope is **3.35 VMAF per ln(rate)**, so 1% BD-rate saving is worth only **≈ +0.033 VMAF**; a filter winning 2% BD-rate in VVC literature would give ≈ **+0.07 VMAF** here. Closing 1.84 VMAF would need ≈ **55% BD-rate** improvement — a filter cannot do that; that gap is the missing B-frames/CABAC/trellis/MB-tree/8×8-inter structural path.
* **The only way to get a visually big VMAF number from filtering is a non-conformant or decoder-optional post-filter**, and much of that number is VIF-scale0 exploitation (sharpening), not recovered detail. Published evidence: unsharp-masking-style preprocessing raises VMAF by up to 34% on average (single videos up to 85%) while subjective quality is unchanged or *worse*; original preferred in 58.3% of pairwise comparisons (Siniukov et al., arXiv:2107.04510, 2021). Netflix's own VMAF v1 (libvmaf v3.2.0, 2026-06) **removed VIF as a core feature** and enabled NEG by default precisely because of this (`netflixtechblog.com/vmaf-v1-good-is-not-good-enough-60d7e4244ea8`, 2026-06-19).
* Our harness uses `libvmaf` with no `version=` option (`E:\__PROJECTS__\Vk264\harness\benchmark.py:52`) and the JSON contains `integer_vif_scale0` (verified in `results/slice_scale/native25_rows2/wx264-slow.vmaf.json`) → **we are measuring VMAF v0**, the version most vulnerable to sharpening. Any filtering experiment must be re-scored with `version=vmaf_v1` / NEG and a source-identity control before claiming genuine quality.

---

## 1. Stage breakdown — reference (x264) vs Vk264

| Sub-stage | What x264/spec does | Source cite | Vk264 counterpart | Gap / note |
|---|---|---|---|---|
| **Deblock placement** | In-loop, mandatory for decoder; rows filtered during reconstruction; `disable_deblocking_filter_idc` only for slices | `x264-vulkan/common/deblock.c:378` `x264_frame_deblock_row`; `encoder/encoder.c:205-210`, `2417-2458` | `shaders/deblock_frame.comp` (mode 2), `shaders/deblock.comp` (slice mode) | Equivalent; Vk264 mode 2 uses 4 tiles × 32 lanes, skewed `u=x+y,v=y` wavefront (`deblock_frame.comp:8-11,39-57`) |
| **Strength BS 0–4** | BS=2 if nnz on either side; BS=1 if ref/MV differ (|ΔMV|≥4 or `mvy_limit`); BS=0 otherwise; intra edges 3/4 | `common/deblock.c:277-304` `deblock_strength_c`; `common/macroblock.c:1441-1448` | `shaders/deblock_filter.glsl:39-51` `strength()`: intra → 4 for edge 0 else 3; `flatIntra` override; nnz → 2; ref mismatch / |ΔMV|≥4 → 1 | Equivalent. Vk264 has no B-frames so no `ref[1]` term (correct for its bitstream) |
| **alpha/beta/tc tables** | `i_alpha_table`, `i_beta_table`, `i_tc0_table[52+36][4]`; indexed by `qp+alpha_c0_offset`, `qp+beta_offset` (offsets = `param*2` signalled in slice header) | `common/deblock.c:32-76`, `306-337` | `shaders/deblock_tables.glsl:4-14` packed 52-entry table (OpenH264, SHA `5d65bd58…`); `deblock_filter.glsl:54-88` | **Vk264 ignores alpha/beta offsets entirely** (`thresholds[0]=deblockThresholds[qp]`, `deblock_filter.glsl:99`, `deblock_frame.comp:35-36`). Zero-GPU-cost lever available. |
| **QP early-out** | `qp_thresh = 15 - min(a,b) - max(0,chroma_qp_index_offset)`; skip when `qp<=qp_thresh` | `common/deblock.c:381-383`, `:415` | `if (qp < 16 && !aq) return;` (`deblock.comp:17`, `deblock_frame.comp:31`) | Matches x264 at offsets 0. x264 additionally does `first_edge_only` for 16×16 P-skip/zero-CBP (`deblock.c:415`); Vk264 has coarser equivalent: `activeSegments` mask skips all-zero-MB loads/filters (`deblock_frame.comp:74-82`) — with 56% P-skip this matters |
| **Filter arithmetic** | 8-bit luma strong/weak, chroma weak; clipping to [0,255]; tc0+smoothing | `common/deblock.c:79-220`, `deblock_edge* :306-337` | `deblock_filter.glsl:54-105`, exact `/8`,`/4` strong and clamp weak forms | Bit-exact-looking; no known divergence |
| **Chroma QP** | `h->chroma_qp_table = i_chroma_qp_table + 12 + chroma_qp_index_offset` (`encoder.c:1585`); psy-RD/psy-trellis implicitly lower chroma QP offset by 1–2 (`encoder.c:1220-1231`) | `common/set.c` chroma table | `shaders/transform.glsl:5-7` `chromaMap[]`, offset 0 | Previously tested chroma offset −2: 640p PSNR 48.13→48.05, VMAF neutral → reverted (`quality_baseline.md:98`). No gain available. |
| **AQ-aware thresholds** | Per-MB QP via AQ; thresholds use averaged `(qp+other+1)>>1` | `common/deblock.c:414,464,516,553` | `prepareEdgeThreshold()` `deblock_filter.glsl:6-14`, gated by `pictureAq()` | Vk264 default `aq=0`; `--aq 1` measured −1.28 VMAF (`quality_baseline.md:143`) — do not revisit |
| **SAO** | Does not exist in H.264 (introduced in HEVC) | ITU H.264 editions; FFmpeg `sei.h` has no SAO SEI | none | Only HEVC/VVC/AV1 decode SAO in-loop. H.264 can only signal *optional display* post-filters (SEI 20/22) — see §2 |
| **ALF / Wiener / bilateral** | No H.264 in-loop ALF. VVC ALF is Wiener-based, 7×7/5×5 diamond, gradient classification, signalled coefficients | Saha et al., *J. Real-Time Image Process.* 20, art.43, 2023-04-13, `link.springer.com/article/10.1007/s11554-023-01300-z` | none | Needs custom decoder or optional post-filter SEI |
| **Denoise / film grain** | No in-loop denoise in x264. `--tune grain` = `aq-strength 0.5, --no-dct-decimate, deadzone-inter/intra 6, deblock -2:-2, ipratio/pbratio 1.1, psy-rd :0.25, qcomp 0.8` | `x264-vulkan/x264.c:648-653` | none; `flat_intra` gate + AC divisor 3 (PR #501) | Grain SEI type 19 exists in H.264 (FRExt 2006) but synthesis is **decoder-optional**; Netflix recommends disabling FGS when scoring VMAF (§2.5) |
| **Post-filter signalling** | H.264 Amd.2 (2007) added **post-filter hint SEI**; H.264 v15 (2024) added NNPFC/NNPFA SEI via H.274; v16 (2026-06) extended FGC and NNPFA | ITU H.264 rec page, `itu.int/ITU-T/recommendations/rec.aspx?rec=H.264`; FFmpeg `sei.h:49-53`: FGC=19, deblock-display-preference=20, post-filter-hint=22 | none | Bitstream-conformant to signal; **effect only if decoder chooses to apply it** |

**Speed context**: Vk264 deblock measured **1.8576 / 1.8476 ms** at 300f 1080p (`results/ag_satd8_300_6000/summary.json:87,141`), **1.7182 ms** at 8000 k (`results/ag_base300_8000/summary.json:87`), **2.0894 ms** with AQ on (`results/ag_probe_aq/summary.json:88`), and 4.11 ms in one older/wavefront-off run (`results/ag_base300_6000/summary.json:86`). Matches the stated 1.9–2.2 ms mode-2 figure. Headroom to 100 fps loop is ~1–2 ms; an added full-frame pass must stay ≤ ~2 ms and must not become serial.

---

## 2. Literature survey 2020–2026 (gains, dates, conformance)

Conformance legend: **[C]** normative effect on every conformant decoder; **[S]** conformant bitstream but optional decoder behaviour (effect not guaranteed); **[X]** needs custom decoder / new profile / changed reference frames.

### 2.1 Deblocking optimisations and tuning
* **Reusing the H.264/AVC deblocking filter for efficient spatio-temporal prediction** — Seiler & Kaup, arXiv:2207.01210 (submitted 2022-07-04; original ICASSP 2011), `https://arxiv.org/abs/2207.01210`. **Up to 7.2% average rate reduction** at very low complexity because the existing deblock engine is reused as a prediction filter. **[X]** — both encoder and decoder must derive the same prediction; not conformant to current H.264 decoders. Interesting idea for Vk264's 1.8 ms deblock engine, but would require new syntax/ABI.
* **Foundational GPU deblocking** (pre-2020, for context only): "A efficient parallel deblocking filter based on GPU: Implementation and optimization", IEEE `document/6032906` (2012), and "Parallel deblocking filtering in H.264/AVC using multiple CPUs and GPUs", ACM `10.1145/2393347.2396370` (2012). These exploit the same left/top dependency reorganisation Vk264 already uses. Vk264's 1.7–2.1 ms is already at the "fast" end; no 2020–2026 paper shows a large *new* gain for the standard H.264 deblock itself.
* **VVC ALF GPU parallelisation** — Saha et al., 2023-04-13, `link.springer.com/article/10.1007/s11554-023-01300-z`: **2× ALF speedup** on Jetson AGX Xavier (RA: 16.6 s → 8.1 s CPU-only → CPU+GPU; LD: 18.9 → 8.8 s), bit-exact output. Relevant as a *technique* (one kernel for whole frame, copy each border pixel once, 128-thread blocks, pinned memory), not as an H.264 feature.
* **Optimized ALF in VVC** — Meng et al., arXiv:2607.05737v2, 2026-07-08, `https://arxiv.org/html/2607.05737v2`: picture-buffer accesses **152 → 1**, **22–25% ALF time saving**, BD-rate change ≤0.02% Y (RA). Shows that even highly optimised in-loop filters give essentially zero quality gain when the codec already has SAO+ALF; transferable lesson: filter optimisations buy speed, not VMAF.

### 2.2 SAO / SAO-like filters "in H.264-compatible ways"
* SAO is an HEVC/VVC in-loop tool (edge offsets / band offsets per CTU) and **does not exist in H.264**. A true SAO-like in-loop filter changes the reference frames → **[X]** unless both sides implement it.
* **Revisiting the SAO post-filter of VVC with neural networks** — Bordes et al., arXiv:2207.05134 (2022-07-11; PCS 2021), `https://arxiv.org/abs/2207.05134`: replaces SAO's a-priori classification with small NNs, **≥2.3% BD-rate gain in VVC Random Access**, low complexity. **[X]** in-loop; **[S]** if applied after decoding via SEI.
* **Post Sample Adaptive Offset for Video Coding** — Fraunhofer, `publica.fraunhofer.de/entities/publication/d9a84e96-…` (VVC study, 2020s). SAO applied as an optional *post* filter (not in-loop) avoids touching reference frames: **[S]** in principle, but still needs decoder support to be visible.
* H.264's only normative hooks for extra processing are **SEI type 20 (deblocking filter display preference)** and **type 22 (post-filter hint)**, added in Amd.2/2007; FFmpeg `libavcodec/sei.h:50-53` confirms the payload numbers. **[S]**: a decoder may ignore both. There is no SEI that makes a standard decoder apply SAO, ALF or a Wiener filter.
* "Bilateral vs block-Wiener for blocking-artifact reduction" (IEEE `document/7813895`, 2017, pre-window) reports **bilateral > Wiener** on SSIM/PSNR for compressed blocks. Our blocking measurement says this class of filter is mis-targeted for Vk264.

### 2.3 Adaptive / Wiener / bilateral filters
* VVC GALF/CCALF are Wiener-based with gradient classification; VVC ALF alone is reported as **>4% coding improvement** at high hardware cost (IEEE `document/10136842`, 2023), and ALF is 5–12% (HGPP) / 12–24% (EGPP) of total decode time (Saha 2023). **[S/X]**: in-loop needs custom decoder; post-decode Wiener needs SEI hint + willing decoder.
* **Video Coding with Cross-Component Sample Offset (CCSO)** — Gao et al., arXiv:2406.01795, 2024-06-03, `https://arxiv.org/abs/2406.01795`: multiplication-free LUT map from neighbouring luma to luma/chroma offsets; adopted in an AOM experimental codec, **up to 3.5% PSNR and 1.8% VMAF BD-rate** under RA, plus subjective gains. **[X]** in-loop (changes references).
* **Overview of cross-component in-loop filters** — arXiv:2607.12186, 2026-07, `https://arxiv.org/pdf/2607.12186`: survey of DBF/SAO/ALF→CC-ALF/CCSO evolution; useful taxonomy, no new Vk264-applicable number.

### 2.4 Neural in-loop / post filters (2020–2026)
| Work | Date / URL | Measured gain | Complexity | Conformance |
|---|---|---|---|---|
| **Video Compression with CNN-based Post Processing** (Zhang, Ma, Feng, Bull) | arXiv:2009.07583, 2020-09-16 (v2 2021-01-14), `https://arxiv.org/abs/2009.07583` | **−4.0% / −5.8% BD-rate (PSNR)** vs VVC/AV1; with perceptual loss **−13.9% / −10.5% BD-rate (VMAF)** | not given in abstract | **[S]** PP only; bitstream unchanged, decoder must run model |
| **MFRNet** (Ma, Zhang, Bull) | arXiv:2007.07099v2, 2020-12-11, `https://arxiv.org/html/2007.07099v2` | HEVC HM16.20: **−14.1% BD-rate PSNR / −21.0% BD-VMAF PP**, −10.2%/−16.0% ILF. VVC VTM7: **−6.7%/−7.1% PP**, −4.6%/−5.1% ILF. 1080p class B VVC PP: −6.3% PSNR / −6.6% VMAF BD-rate | **72.6× VTM decode time** (PP), 2.4× VTM encode (ILF); 96×96 4:4:4 patches | **[S]** PP; **[X]** ILF |
| **In-Loop Filtering via Trained Look-Up Tables** (Li et al.) | arXiv:2407.10926, 2024-07-15, `https://arxiv.org/abs/2407.10926` | VTM-11.0: **0.13/0.34/0.51% (AI)** and **0.10/0.27/0.39% (RA)** BD-rate for ultrafast/veryfast/fast | **101%/102–104%/108% time**, **0.13–0.93 kMAC/px**, **164–1148 KB** model | **[X]** in-loop; trivially portable to a display post-filter **[S]** |
| **LUT-ILF++** (Li et al.) | arXiv:2509.09494, 2025-09-11, `https://arxiv.org/abs/2509.09494` | **0.82%/2.97%/1.63% (AI)**, **0.85%/4.11%/2.06% (RA)** BD-rate (Y/U/V presumably) | lower time/storage than DNN ILF | **[X]** / optional **[S]** |
| **WCDANN: lightweight CNN PP for VVC** | IEEE `document/10201857` (2023/2024), `https://ieeexplore.ieee.org/abstract/document/10201857` | depthwise-separable conv + attention; reported BD-BR gains (exact numbers behind paywall; table on Semantic Scholar shows VTM-11-NNVC RA comparison) | designed to be lightweight | **[S]** PP |
| **CASR: content-adaptive SR post-filter + LoRA** | arXiv:2609.37328, 2026-09-29, `https://arxiv.org/abs/2609.37328`; MMSP 2026 Best Student Paper | vs VTM21: **−10.93% Y / −15.39% U / −24.41% V BD-rate (RA)**, −13.43% Y (AI), r=4 best; VSEI-compliant model signalling | LoRA weights compressed with MPEG NNR; inference cost not published in abstract | **[S]** post-filter, but encodes at reduced resolution — not applicable to fixed 1080p source |
| **Standards-based neural-network post-filters (NNPF)** | ACM `10.1145/3638036.3640809` (2023/2024), `https://dl.acm.org/doi/10.1145/3638036.3640809`; H.274 V3 2023-09 | Defines NNPFC/NNPFA SEI + activation/deactivation | — | **[S]**: signalling is normative, application is optional |
| **Real-time NNPF for 4K60 VVC on FPGA** | VCIP 2024, Tokyo, 2024-12-08…11, IEEE `document/10849820`, doi `10.1109/VCIP63160.2024.10849820` | 4K60 real-time; "minor bitrate increase" to carry model weights | FPGA implementation | **[S]**; shows custom hardware is needed even for small NNPFs |
| **Prediction-aware / model-selection CNN for VVC** | arXiv:2105.05658 / 2105.03338, 2021-05 | +1.52% BD-BR over default CNN QE; +1.3% from prediction info, +0.5% from model selection | one shared QP-map model | **[S]** PP / **[X]** ILF |
| **Multi-density attention loop filter** | arXiv:2104.12865, 2021-04-08 | claims **10.18% bit-rate reduction** vs VVC (subjective/detail preservation) | heavy | **[X]** |

**Key discriminator**: PP/ILF gains measured as BD-rate at *fixed PSNR/VMAF* say nothing about applying a filter to one encoder only. If Vk264 alone is post-filtered, the bitrate is unchanged and the VMAF number rises, but x264 at the same bitrate was not given the same treatment — that is a metric comparison, not a coding-efficiency win.

### 2.5 VMAF-driven sharpening vs genuine detail; film grain
* **Hacking VMAF and VMAF NEG** — Siniukov, Antsiferova, Kulikov, Vatolin, arXiv:2107.04510v2, 2021-08-16, `https://arxiv.org/html/2107.04510v2` (ACM `10.1145/3508259.3508272`). 618 FullHD videos. Reported VMAF "gain" (paper-defined relative gain) **up to 218.8% for CLAHE**, median 50.2%, third quartile 86.8%; **unsharp masking avg +34.07%**, gamma+unsharp +34.62% (max 85.22% on "Ocean"); **VMAF-NEG up to +23.6%**, avg +5.93% for gamma+unsharp. Subjective test (1,584 responses, 114 participants): **original preferred in 58.3%** of comparisons; "gamma correction and unsharp masking provided the worst visual quality" while VMAF rose ~46% on average. Earlier work cited in that paper: histogram equalization / unsharp masking can raise VMAF by **5–6%** ("Hacking VMAF with Video Color and Contrast Distortion", 2019).
* **VMAF v1** — Netflix TechBlog 2026-06-19, `https://netflixtechblog.com/vmaf-v1-good-is-not-good-enough-60d7e4244ea8`; libvmaf **v3.2.0** initial public VMAF v1 release (`github.com/Netflix/vmaf/releases/tag/v3.2.0`, released 2026-06-20). Changes: **VIF removed as a core feature** ("computationally complex and did not meaningfully improve accuracy after updating the other features"), **NEG enabled by default**, AIM added, CAMBI banding feature, chroma SpEED-QA, motion capped and 60 fps underprediction reduced. Directly relevant: our `vif_scale0`-dominated deficit is measured with the metric version Netflix itself deprecated for codec comparisons.
* **Film grain**: H.264 FGC SEI payload **19** exists since FRExt/Amd.1 2006; FFmpeg `sei.h:49` confirms. ITU-T H Suppl. 21 (2025-01) "Film grain synthesis technology for video applications" `itu.int/rec/T-REC-H.Sup21-202501-I`; ITU H.264 v16 (2026-06) adds VSEI interface for extended FGC. But synthesis is optional decoder behaviour. Netflix issue **#1192** (2023-07-01, maintainer reply 2023-07-27) — `https://github.com/Netflix/vmaf/issues/1192`: "synthesized grain interferes with VMAF scores, and we recommend to disable it when computing VMAF"; AV1 FGS "does not generally improve objective quality metrics because of the mismatch in positions of individual grains". Netflix AV1@Scale (TechBlog **2025-07-02**, `https://netflixtechblog.com/av1-scale-film-grain-synthesis-the-awakening-ee09cfdff40b`) reports **36% average bitrate reduction at ≥1080p over ~300 titles** (only 10% below 1080p), one heavy-grain example **8274 → 2804 kbit/s (−66%)**, and explicitly states "we lack a dedicated quality model for film grain synthesis" because pixelwise PSNR/VMAF penalise it. This is a *subjective* win invisible to VMAF. IBC 2023 paper "A subjective study of film grain synthesis for the preservation of creative intent" (Sapra, Zeng, Yeganeh; `ibc.org/.../22561`, 2023-09-16) finds current FGS suboptimal and calls for a better objective metric.
* **x264's own grain recipe** (`x264.c:648-653`) uses *less* deblocking (−2:−2), smaller deadzones (6/6), no DCT decimate and weaker AQ — i.e. encode grain as residual rather than filter it. Our game/trailer content is not grain-limited; adding synthetic grain would *lower* VMAF while possibly looking better.

### 2.6 Measurement caveat for Vk264
`harness/benchmark.py:52` calls `libvmaf=log_fmt=json:log_path=...:n_threads=4:shortest=1:repeatlast=0` with no `version=`. The emitted features (`integer_vif_scale0`, `integer_adm2`, …) confirm the **v0.6.1 model**. Before accepting any filtering change, re-run with:
`libvmaf=version=vmaf_v1:...` and `libvmaf=version=vmaf_v0.6.1neg:...` (or current NEG model name), and add a **source-identity control** (apply the same filter to the source and score filtered-source vs source; any VMAF ≠ 100 proves enhancement gain rather than error reduction).

---

## 3. Candidate scorecard at 1080p60 / 6000 kbit/s

Conversion used: local slope **3.35 VMAF per ln(bitrate)** from our own 96.149@6000 → 97.113@8000; 1% BD-rate ≈ **+0.033 VMAF**. "Direct VMAF" means the filter is applied to Vk264 display output only, without giving x264 the same filter — that is **not** a matched-rate coding gain.

| Candidate | Expected **matched-rate** VMAF gain | Expected **direct** VMAF gain (Vk264 only) | GPU cost | Conformance risk | Verdict |
|---|---:|---:|---:|---|---|
| Deblock alpha/beta offset sweep (signalled in slice header) | **+0.00…+0.05** (lab says blocking is not the deficit; error ratios already match x264) | same | **0 ms** | **None [C]** — x264 `encoder.c:209-210`, FFmpeg/x264 signalled | Free test, but do not expect to close gap |
| Chroma QP offset / x264-style psy offset | **0** (measured −2: PSNR down, VMAF neutral) | 0 | 0 ms | None [C]; previously reverted | Reject |
| H.264 post-filter hint SEI + optional display bilateral/Wiener | 0 (encoder rate unchanged) | **+0.1…+0.5**, mostly VIF scale0 | **0.3–1.0 ms** | **Medium [S]** — SEI type 22 is conformant, but effect is optional; x264 comparison becomes unfair | Only if we also filter x264 with the same display filter |
| Unsharp mask / "detail enhance" post-filter | **0** (no bits recovered) | **+1…+4** on VMAF v0 (VIF scale0), likely **−0.5…−2** on VMAF v1/NEG; subjective flat or worse (Hacking VMAF) | **0.2–0.5 ms** | **High [S]** metric hack / creative-intent violation | **Reject** as quality work; keep only as a labelled diagnostic |
| LUT-ILF / CCSO-style learned post-filter (trained on our QP/range) | +0.01…+0.07 (literature 0.13–2.06% BD-rate → 0.004–0.07 VMAF at our slope) | **+0.3…+1.5** | **0.8–2.0 ms** (0.13–0.93 kMAC/px ≈ 16–115 GMAC/s at 124 Mpx/s) | **High [X]** in-loop; **[S]** post-filter via NNPFC/NNPFA SEI but decoder support required | Promising speed/quality per watt, too small to close structural gap |
| Full CNN post-filter (MFRNet-class) | literature −7.1% BD-VMAF VVC → +0.24 VMAF at our slope | +1…+3 | **≫2 ms** (MFRNet PP = 72.6× decoder time; WCDANN lightweight but still GPU-heavy) | **[S]** signalling with NNPF SEI; needs model transport | **Infeasible** on GTX 1660 SUPER at 100 fps |
| SAO / ALF / neural **in-loop** | up to 2–4% BD-rate (SAO-NN ≥2.3%; VVC ALF >4%) → +0.07…+0.13 VMAF | n/a | 1.5–3 ms | **Fatal [X]** — changes reference frames; standard H.264 decoders produce different pictures | Reject unless new profile/custom decoder is in scope |
| Reuse deblock engine as spatio-temporal prediction filter (Seiler & Kaup) | up to 7.2% rate → **+0.23 VMAF** at our slope | n/a | ~0 ms extra (reuses deblock) | **Fatal [X]** new prediction tool | Interesting research, not a drop-in |
| Film-grain synthesis / preservation | **negative VMAF** (grain treated as noise); AV1 FGS 36% bitrate saving is subjective | **−0.5…−3 VMAF** | 0.3–1 ms | **[S]** FGC SEI 19; decoder optional | Only for a creative-intent product requirement, not for VMAF |
| Encoder-side structural work (B-frames, CABAC, trellis/RDO, MB-tree) | **+1…+15 VMAF** (fixed-QP ceiling still 4–15 VMAF below x264 on 1080p60) | n/a | already budgeted in other pipeline notes | **[C]** | **The actual answer** — outside this stage |

---

## 4. Top 3 ranked candidates with shader sketches

Ranking is by (legal × cost ÷ expected gain), not by headline VMAF. The first is conformant and free; the second is the only plausible *visual* post-filter inside our 1–2 ms budget; the third is the highest-ceiling option but explicitly non-conformant.

### #1 — Signalled deblock alpha/beta offset tuning (conformant, 0 ms)
Wire the existing slice-header offsets into the packed threshold lookup instead of ignoring them; also adopt x264's `qp_thresh = 15 - min(a,b) - max(0,chromaOffset)` early-out. Host bakes a 52-entry packed table per offset pair (same bit layout as `deblock_tables.glsl`: alpha bits 0–7, beta bits 8–12, tc0 bs1/2/3 at bits 13/18/23).

```glsl
// NEW push constants (mirrored into slice header as alpha_c0_offset = a*2, beta_offset = b*2)
layout(push_constant) uniform FilterCfg { int alphaC0; int betaOff; int chromaQpOff; };

uint packThreshold(int qpA, int qpB) {          // host-baked SSBO: 52*52 or precomputed [a][b][qp]
    int qa = clamp(qpA + alphaC0, 0, 51);
    int qb = clamp(qpB + betaOff, 0, 51);
    return alphaBetaTcTable[qa][qb];            // same packed format as deblockThresholds[]
}

void main() {
    int qp = frameQp();
    // x264 common/deblock.c:383 — identical early-out with offsets applied
    if (qp + min(alphaC0, betaOff) <= 15 && !pictureAq()) return;
    thresholds[0] = packThreshold(qp, qp);
    thresholds[1] = packThreshold(chromaQp(qp) + chromaQpOff,
                                  chromaQp(qp) + chromaQpOff);
    ...
}
```
Test grid: (a,b) ∈ {(−2,−2), (−1,−1), (0,0), (1,1), (2,2), (−2,0), (0,−2)} on the 300f 1080p game clip at 6000/8000. Expected |ΔVMAF| ≤ 0.05; stream bytes change only in slice headers; decoder conformance preserved. **Do not spend more than a half-day on this.**

### #2 — Optional display-side bilateral + mild detail-recovery pass (SEI type 22, ~0.5–1 ms)
A separate compute pass writing a *display* buffer, never back into `reconstructed[]` used for prediction. This keeps the bitstream and all reference frames bit-identical; signal the suggested filter via **post-filter hint SEI (payload 22)**; standard decoders that ignore it still decode correctly.

```glsl
// deblocked recon -> display; 16x16 workgroups; one invocation per luma pixel
layout(local_size_x=16, local_size_y=16) in;
layout(binding=0) readonly  buffer Src { uint8_t y[]; };
layout(binding=1) writeonly buffer Dst { uint8_t outy[]; };
layout(push_constant) uniform P { int stride; int sigmaR; int sigmaS; int sharp; };

float rangeW(int d) {                 // 17-entry LUT instead of exp()
    int i = clamp(abs(d) * (256 / sigmaR), 0, 16);
    return bilut[i];                  // exp(-i^2/(2*256)) precomputed
}
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    int c = int(y[p.y*stride + p.x]);
    float ws = 0.0, acc = 0.0;
    for (int j = -1; j <= 1; ++j)
      for (int i = -1; i <= 1; ++i) {
        int v = int(y[(p.y+j)*stride + p.x+i]);
        float w = rangeW(v - c) * spatialLUT[(j+1)*3 + i+1];
        acc += w * v; ws += w;
      }
    float smooth = acc / ws;
    // Genuine recovery: restore edge contrast only where the deblocked pixel is
    // already closer to the source prediction. sharp>0 is the VIF-exploiting part.
    float val = mix(smooth, float(c), 0.6) + sharp * (float(c) - smooth);
    outy[p.y*stride + p.x] = uint8_t(clamp(val, 0.0, 255.0));
}
```
Budget: 2.07 Mpx/frame, 9 taps, LUT weights, ~20 ops/px → ~42 MOp/frame at 60 fps = 2.5 GOp/s; memory ~8 MB/frame (3×3 reads reuse L1) → well under 1 ms on GTX 1660 SUPER, plus one dispatch. Chroma is 2×2 subsampled; add a 5-tap chroma variant only if Cb/Cr VMAF (SpEED-QA in v1) matters. **Acceptance gate**: report VMAF v0 *and* v1/NEG, run the source-identity control, and apply the identical display filter to x264 before claiming matched-rate gain. Set `sharp = 0` for any result intended as genuine quality.

### #3 — LUT-based neural in-loop/post filter (CCSO / LUT-ILF++ style, 0.8–2 ms, needs custom decoder for in-loop)
This is the only learned approach with a credible speed story (0.13–0.93 kMAC/px; 164–1148 KB per model). Train a small net on (deblocked recon, source) pairs at our QP range, restrict its receptive field to 8 neighbours, enumerate all inputs, bake to a LUT. Deploy first as a *display* filter (SEI NNPFC/NNPFA if we want standards-shaped signalling); promote to in-loop only if a custom decoder is acceptable.

```glsl
// 8-neighbour 2-bit quantised index => 65536-entry int16 LUT (128 KB per plane)
layout(binding=0) readonly buffer Recon { uint8_t y[]; };
layout(binding=1) readonly buffer Lut  { int16_t deltaY[]; int16_t deltaC[]; };
layout(binding=2) writeonly buffer Disp { uint8_t outy[],outu[],outv[]; };
layout(push_constant) uniform P { int stride, cstride; };

uint pack8(ivec2 p) {
    static const ivec2 off[8] = ivec2[8](ivec2(-1,-1),ivec2(0,-1),ivec2(1,-1),
                                        ivec2(-1, 0),             ivec2(1, 0),
                                        ivec2(-1, 1),ivec2(0, 1),ivec2(1, 1));
    uint idx = 0;
    for (int k = 0; k < 8; ++k)
        idx = (idx << 2) | uint(clamp(int(y[(p.y+off[k].y)*stride + p.x+off[k].x]) >> 6, 0, 3));
    return idx;
}
void main() {
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    uint idx = pack8(p);
    int  v = int(y[p.y*stride + p.x]);
    outy[p.y*stride + p.x] = uint8_t(clamp(v + int(deltaY[idx]), 0, 255));
    if ((p.x & 1) == 0 && (p.y & 1) == 0) {           // cross-component term, CCSO style
        ivec2 c = p >> 1;
        outu[c.y*cstride + c.x] = uint8_t(clamp(int(u[c.y*cstride+c.x]) + int(deltaC[idx]), 0, 255));
    }
}
```
Expected: literature equivalent is **0.13–2.06% BD-rate** → **+0.004…+0.07 VMAF** at our slope; a display-only deployment can still lift VMAF v0 by ~0.3–1.5 (partly VIF). In-loop requires that the exact same LUT run on the decoder before motion compensation → **[X]**; do not claim "standard compatible" for that variant.

---

## 5. Decision guidance

1. **Do not chase filters for the 1.84 VMAF gap.** The arithmetic is decisive: 1.84 VMAF ≈ 55% BD-rate; the best in-loop filter literature (SAO-NN ≥2.3%, ALF >4%, CCSO 1.8% VMAF BD-rate) is an order of magnitude too small, and most of it is **[X]**.
2. **Re-score with VMAF v1/NEG first.** Our `vif_scale0`-dominated diagnosis is a property of VMAF v0; Netflix removed VIF from v1. If the deficit shrinks under v1, the "fine detail" story changes and sharpening-type experiments should be stopped immediately.
3. **Conformant wins in this stage are exhausted**: deblock tables/strength/chroma-QP/early-out already match x264; alpha/beta offsets are untested but the lab's own error-ratio measurement predicts ≤0.05 VMAF.
4. **If a visual post-filter is wanted for the product**, budget ≤1 ms, keep it display-only, signal it with SEI 22 or NNPFC/NNPFA, apply the same filter to the x264 baseline before any matched-rate claim, and report VMAF v0 **and** v1/NEG plus the source-identity control. Unsharp masking is a metric hack, not quality recovery.
5. **Film grain** is a creative-intent feature that VMAF explicitly mishandles (Netflix recommends disabling FGS when scoring); pursue only if a subjective requirement exists, never as a VMAF play.
6. **The real remaining levers are structural and live outside this stage** (B-frames, CABAC, trellis/RDO/MB-tree, 8×8 inter), consistent with `quality_baseline.md` §1080p60 audit.

## Sources (all accessed 2026-11-27; web text treated as untrusted data)

Local: `docs/quality_baseline.md`; `x264-vulkan/common/deblock.c`; `x264-vulkan/encoder/encoder.c`; `x264-vulkan/x264.c`; `shaders/deblock.comp`, `shaders/deblock_frame.comp`, `shaders/deblock_filter.glsl`, `shaders/deblock_tables.glsl`, `shaders/transform.glsl`; `harness/benchmark.py`; `results/ag_satd8_300_6000/summary.json`, `results/ag_base300_8000/summary.json`, `results/ag_probe_aq/summary.json`, `results/slice_scale/native25_rows2/wx264-slow.vmaf.json`.

Web:
1. https://arxiv.org/abs/2207.01210 (2022-07-04, ICASSP 2011 original) — 7.2% rate
2. https://link.springer.com/article/10.1007/s11554-023-01300-z (2023-04-13) — ALF GPU 2×
3. https://arxiv.org/html/2607.05737v2 (2026-07-08) — ALF 152→1 buffer passes, 22–25% time
4. https://arxiv.org/abs/2207.05134 (2022-07-11, PCS 2021) — NN SAO ≥2.3% BD-rate
5. https://arxiv.org/abs/2406.01795 (2024-06-03) — CCSO 3.5% PSNR / 1.8% VMAF BD-rate
6. https://arxiv.org/abs/2009.07583 (2020-09-16) — CNN PP −4.0/−5.8% PSNR, −13.9/−10.5% VMAF BD-rate
7. https://arxiv.org/html/2007.07099v2 (2020-12-11) — MFRNet gains and 72.6× decode cost
8. https://arxiv.org/abs/2407.10926 (2024-07-15) — LUT ILF 0.13–0.51% BD-rate, 101–108% time
9. https://arxiv.org/abs/2509.09494 (2025-09-11) — LUT-ILF++ 0.82–4.11% BD-rate
10. https://ieeexplore.ieee.org/abstract/document/10201857 — WCDANN lightweight CNN PP
11. https://arxiv.org/abs/2609.37328 (2026-09-29, MMSP 2026) — CASR −10.93% Y BD-rate, VSEI-compliant
12. https://dl.acm.org/doi/10.1145/3638036.3640809 — standards-based NNPF overview
13. https://ieeexplore.ieee.org/document/10849820 (VCIP 2024) — real-time 4K60 NNPF on FPGA
14. https://arxiv.org/html/2107.04510v2 (2021-08-16) — VMAF hacking numbers and subjective study
15. https://netflixtechblog.com/vmaf-v1-good-is-not-good-enough-60d7e4244ea8 (2026-06-19) — VIF removed, NEG default
16. https://github.com/Netflix/vmaf/releases/tag/v3.2.0 (2026-06-20) — VMAF v1 public release
17. https://github.com/Netflix/vmaf/issues/1192 (2023-07-01/27) — grain synthesis interferes with VMAF
18. https://www.itu.int/ITU-T/recommendations/rec.aspx?rec=H.264 — H.264 v15 (2024) NNPF SEI, v16 (2026-06) FGC/VSEI extensions
19. https://ffmpeg.org/doxygen/trunk/sei_8h_source.html — SEI type 19 FGC, 20 deblock display preference, 22 post-filter hint
20. https://itu.int/rec/T-REC-H.Sup21-202501-I — H Suppl.21 film grain synthesis (2025-01)
21. https://www.ibc.org/accelerating-innovation/reports/a-subjective-study-of-film-grain-synthesis-for-the-preservation-of-creative-intent/22561 (2023-09-16)
22. https://www.researchgate.net/publication/271482959_A_combined_SAO_and_de-blocking_filter_architecture_for_HEVC_video_decoder; https://publica.fraunhofer.de/entities/publication/d9a84e96-… — SAO background
23. https://ieeexplore.ieee.org/document/7813895 — bilateral vs block-Wiener for blocking artifacts
24. https://netflixtechblog.com/av1-scale-film-grain-synthesis-the-awakening-ee09cfdff40b (2025-07-02) — 36% average bitrate reduction with AV1 FGS; VMAF penalises grain
