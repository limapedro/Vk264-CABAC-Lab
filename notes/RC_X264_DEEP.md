# x264 1-pass ABR/CBR rate control, read from source — and what is portable to Vk264

**Research only.** No file in `E:\__PROJECTS__\Vk264` was modified; no PRs, no builds.
This file is the only artifact written. Web material is treated as untrusted data:
only bibliographic facts and quoted algorithm descriptions are carried forward,
no instructions from web content were followed.

## 0. Provenance and an important correction to the earlier notes

All `file:line` numbers below are from the **actual working copy** at
`E:\__PROJECTS__\Vk264\x264-vulkan`, git HEAD
`48ac002e346b314823a215d7822b3c47462ff351`, read directly. The earlier notes
(`notes/pipeline/05_RATE_CONTROL.md`, `notes/PARITY_ROADMAP.md`) cite a different
pin, `90a61ec76424778c050524f682a33f115024be96`, whose line numbers are shifted
by roughly 20–60 lines in `ratecontrol.c` and ~10 lines in `slicetype.c`. The
**formulas and constants are unchanged** across the two revisions; only the line
numbers differ. Where a prior-note line number is materially wrong (see the
`qblur`/`cplxblur` correction in §1.7 and §2.6) it is called out explicitly.

Files read in full or in the cited regions:
`encoder/ratecontrol.c` (127 kB, 3100+ lines), `encoder/ratecontrol.h`,
`encoder/slicetype.c`, `common/base.c`, `common/common.h`, `common/tables.c`,
`x264.h`, `x264.c:1801-1848`, `encoder/encoder.c:3470-3504,3845-3869`;
Vk264 side: `shaders/scan.comp`, `shaders/layout.glsl`, `shaders/*.glsl` grep,
`src/gpu.hpp`, `src/gpu.cpp`, `src/presets.hpp`, `src/main.cpp`,
`obs/obs-vk264.cpp`.

One web source was consulted as an untrusted cross-check: Loren Merritt,
`doc/ratecontrol.txt` (https://raw.githubusercontent.com/mirror/x264/master/doc/ratecontrol.txt).
It self-declares "outdated" but its description of 1-pass ABR agrees with the
code: half-resolution SATD complexity, scaling factor chosen to give the target
bitrate over **all frames so far**, and tunable overflow compensation. No other
web claims are relied on in this document.

---

## 1. The complete 1-pass ABR algorithm, in execution order

### 1.0 Cast of characters (state that survives frames)

`struct x264_ratecontrol_t`, `ratecontrol.c:72-179`:

| Field | Line | Role in 1-pass ABR |
|---|---|---|
| `b_abr` | 75 | `rc_method != CQP && !b_stat_read` (`:753`) |
| `b_2pass` | 76 | `rc_method==ABR && b_stat_read` (`:754`) |
| `b_vbv`, `b_vbv_min_rate` | 77-78 | VBV active; CBR flag (`:737-739`) |
| `fps`, `bitrate` | 79-80 | `fps = fps_num/fps_den` or 25.0 (`:757-760`); `bitrate = i_bitrate*(1024 or 1000)` (`:770`) |
| `rate_tolerance` | 81 | `f_rate_tolerance` (`:771`), floored at 0.01 (`:801-805`) |
| `qcompress` | 82 | `f_qcompress`, **forced to 1 with MB-tree** (`:762-768`) |
| `nmb` | 83 | MB count per frame |
| `qp_constant[3]` | 84 | CQP only |
| `last_rceq` | 108 | last frame's `pow(blurred_complexity,1-qcompress)`; the normaliser |
| `cplxr_sum` | 109 | **integral** of `bits*qscale/last_rceq` over all finished frames |
| `expected_bits_sum` | 110 | 2-pass only |
| `wanted_bits_window` | 112 | **integral** of target bits over all finished frames |
| `cbr_decay` | 113 | 1.0 for ABR, <1 for CBR/VBV (`:718-719`, `:774`) |
| `short_term_cplxsum`, `short_term_cplxcount` | 114-115 | 2-frame EWMA state |
| `ip_offset`, `pb_offset` | 117-118 | `6*log2(f_ip_factor)`, `6*log2(f_pb_factor)` (`:821-822`) |
| `lmin[3]`,`lmax[3]`,`lstep`,`last_qscale_for[3]` | 137-139,132 | per-type QP clamp and slew limit |

Per-frame scratch is `ratecontrol_entry_t`, `ratecontrol.c:36-61`
(notably `tex_bits`, `mv_bits`, `blurred_complexity`, `qscale=1`, `p_count=nmb`,
`i_count=s_count=0` for the 1-pass case, all set at `:2565-2573`).

### 1.1 Initialization, exact constants — `x264_ratecontrol_new`, `:744-861`

```
:753  b_abr  = (rc_method != CQP) && !b_stat_read
:754  b_2pass= (rc_method == ABR) &&  b_stat_read
:757  fps    = fps_num/fps_den  (else 25.0)
:762  if mb_tree: f_pb_factor=1; qcompress=1
:768  else:       qcompress = f_qcompress            // default 0.6, base.c:423
:770  bitrate = i_bitrate * (avcintra ? 1024 : 1000) // kbit -> bit
:771  rate_tolerance = f_rate_tolerance              // default 1.0, base.c:404
:774  cbr_decay = 1.0
```

ABR state (`ratecontrol.c:809-819`), verbatim constants:

```
#define ABR_INIT_QP ((CRF ? f_rf_constant : 24) + QP_BD_OFFSET)   // :812
accum_p_norm       = 0.01                                          // :813
accum_p_qp         = ABR_INIT_QP * 0.01                            // :814
cplxr_sum          = 0.01 * pow(7.0e5, qcompress) * pow(nmb, 0.5)   // :816
wanted_bits_window = 1.0 * bitrate / fps                            // :817
last_non_b_pict_type = I                                            // :818
```

`QP_BD_OFFSET = 6*(BIT_DEPTH-8)` (`common/common.h:58`), i.e. **0 for 8-bit**;
`QP_MAX_SPEC = 51 + QP_BD_OFFSET`, `QP_MAX = QP_MAX_SPEC+18`
(`common/common.h:59-60`). `X264_LOOKAHEAD_QP = 12 + QP_BD_OFFSET`
(`common/common.h:63`).

Derived constants (`:821-829`):

```
ip_offset   = 6*log2(f_ip_factor)      // f_ip_factor=1.4  -> 2.913 QP units
pb_offset   = 6*log2(f_pb_factor)      // f_pb_factor=1.3  -> 2.271 QP units
lstep       = pow(2, i_qp_step/6)     // i_qp_step=4 -> 2^(2/3) = 1.5874
last_qscale = qp2qscale(26 + QP_BD_OFFSET)
```

Predictors (`:830-860`) are seeded with `coeff_min = table/2`, `coeff = table`,
`count = 1`, `decay = 0.5`, `offset = 0`, where `pred_coeff_table[3] = {1,1,1.5}`
(`:833`); row predictors use `.25/4` and `.25`; `pred_b_from_p` uses `0.5/2`
and `0.5`. Zones are parsed at `:862` (details §1.9).

Full set of relevant defaults (`common/base.c:383-427`, `x264.h:437-479`):
`i_frame_reference=3` (:384), `i_keyint_max=250` (:385), `i_bframe=3` (:387),
`i_scenecut_threshold=40` (:388), `i_qp_min=0` (:410), `i_qp_max=INT_MAX` (:411),
`i_qp_step=4` (:412), `f_ip_factor=1.4` (:413), `f_pb_factor=1.3` (:414),
`aq_mode=VAR` (:415), `aq_strength=1.0` (:416), `lookahead=40` (:417),
`f_rf_constant=23` (:409), `f_rate_tolerance=1.0` (:404),
`vbv_max_bitrate=0`/`vbv_buffer_size=0` (:405-406),
`f_qcompress=0.6` (:423), `f_qblur=0.5` (:424), `f_complexity_blur=20` (:425),
`b_mb_tree=1` (:427).

Sanity check of the initialization for 1080p60 @6000 kb/s: `nmb=8160`,
`sqrt(nmb)=90.3`, `7e5^0.6≈3212`, so `cplxr_sum≈0.01*3212*90.3≈2900`;
`wanted_bits_window=6e6/60=100000`; initial `rate_factor≈34.4`. A frame with
SATD ≈ 1e6 gives `last_rceq≈1e6^0.4≈251`, `q≈251/34.4≈7.3`,
`qp≈12+6*log2(7.3/0.85)≈30.6`. The first frame is therefore near QP 31 — no
magic initial QP is needed.

### 1.2 Q-scale ↔ QP and the R–Q model

```
qp2qscale(qp)  = 0.85 * 2^((qp - (12 + QP_BD_OFFSET))/6)   // :203-206
qscale2qp(qs)  = (12 + QP_BD_OFFSET) + 6*log2(qs/0.85)     // :207-210
```

`qscale` is "linearized quantizer = Lagrange multiplier" (`:199-202`); indeed
`common/tables.c:96` documents `lambda = pow(2, qp/6-2)` and `x264_lambda_tab`.

```
qscale2bits(rce,q) = (tex_bits + .1) * (rce->qscale/q)^1.1
                   + mv_bits * (max(rce->qscale,1)/max(q,1))^0.5
                   + misc_bits,  with q clamped to >= 0.1   // :216-223
```

This is the model used by 2-pass and VBV planning; **1-pass ABR does not call
it for the QP decision** (it calls `predict_size` instead, §1.6). It is the
model to use if we add probe calibration.

### 1.3 Complexity input: pre-analysed half-res SATD, not post-hoc bits

`x264_ratecontrol_start` is called at `encoder/encoder.c:3857` **after**
`x264_ratecontrol_zone_init` (`:3478`) and before the frame is encoded. The
complexity comes from `x264_rc_analyse_slice` (`slicetype.c:1976-2013`):

```
p1 = b = 0                         if I frame                       // :1982-1983
p1 = b = fenc->i_bframes + 1       if P frame                       // :1984-1985
p1 = (poc_ref1-poc_ref0)/2; b=(poc-poc_ref0)/2  if B                 // :1986-1990
cost = frames[b]->i_cost_est[b-p0][p1-b]                            // :1995
if mb_tree && !stat_read: cost = slicetype_frame_cost_recalculate(...)// :1998-2003
else if aq_mode:          cost = i_cost_est_aq[...]                 // :2005-2006
fdec->i_satd = cost                                                 // :2010
```

The costs were already computed earlier by `x264_slicetype_decide`
(`slicetype.c:1745-1797`); `x264_slicetype_analyse` is triggered when B-adapt,
scenecut, MB-tree or VBV-lookahead is on (`:1793-1797`), and there is an
explicit comment at `:1898`: *"calculate the frame costs ahead of time for
`x264_rc_analyse_slice` while we still have lowres"*. The lowres context uses
`a->i_qp = X264_LOOKAHEAD_QP (=12)` and `a->i_lambda = x264_lambda_tab[12]`
(`slicetype.c:45-48`), forced HEX/subpel-4 (`:50-54`), no chroma ME (`:60`).
The score is a λ-weighted 8×8 SATD/SAD inter cost with a fixed `lowres_penalty=4`
(`slicetype.c:514-545,541-542`) and a B-frame scaling by `100/(120+bias)`
(`:983-985`).

**This is the single most important structural fact:** the complexity driving
x264's controller is a QP-independent, *pre-encoded* measurement at fixed QP 12.
It cannot be perturbed by the QP x264 just chose. Our `actual = 32*total`
(`scan.comp:51`) is a QP-dependent, *post-encoded* measurement, so it is both
delayed and inside a feedback loop with the thing it controls.

### 1.4 The short-term EWMA (2-frame half-life) — `ratecontrol.c:2559-2566`

```
last_satd = x264_rc_analyse_slice(h);
short_term_cplxsum   *= 0.5;
short_term_cplxcount *= 0.5;
short_term_cplxsum   += last_satd / (CLIP_DURATION(f_duration) / BASE_FRAME_DURATION);
short_term_cplxcount += 1;
blurred_complexity    = short_term_cplxsum / short_term_cplxcount;
```

`BASE_FRAME_DURATION = 0.04/((frame_packing==5)+1)`,
`CLIP_DURATION(f) = clip3f(f, 0.01/..., 1.00/...)` (`ratecontrol.h:34-40`).
So 60 fps (`f_duration=1/60`) contributes `0.667*SATD`, 25 fps contributes
`0.4*SATD`. The normalised ratio is essential: a 60 fps clip is not charged
twice for having half-length frames. The EWMA is 1/2, 1/4, 1/8 … i.e. an
effective span of ~2 frames — x264 does **not** use a long complexity blur in
1-pass mode (see §1.7).

### 1.5 QP computation — `get_qscale` (`:2003-2035`) and `rate_estimate_qscale` (`:2400-2663`)

`get_qscale` (`:2003-2035`):

```
if mb_tree: q = pow(BASE_FRAME_DURATION / CLIP_DURATION(i_duration*timescale),
                    1 - f_qcompress)                                  // :2011
else:       q = pow(rce->blurred_complexity, 1 - qcompress)            // :2014
if !isfinite(q) || (tex_bits+mv_bits)==0: q = last_qscale_for[pict_type] // :2017-2018
else: last_rceq = q; q /= rate_factor; last_qscale = q;                // :2021-2023
if zone: q = qp2qscale(zone->i_qp) [force] or q /= zone->f_bitrate_factor // :2026-2032
```

The 1-pass ABR rate factor (`:2581`):

```
rate_factor = wanted_bits_window / cplxr_sum
q           = pow(blurred_complexity, 1 - qcompress) / rate_factor      // :2014,:2022
```

with `qcompress=0.6` → exponent **0.4**. MB-tree changes the complexity proxy
to a *duration* term (`:2011`) because MB-tree QP offsets already carry the
spatial complexity information (§1.8); with MB-tree, `qcompress` is forced to 1
so the exponent is 0 (`:765`).

The full decision chain in `rate_estimate_qscale`:

```
:2479  abr_buffer = 2 * rate_tolerance * bitrate
:2480  predicted_bits = total_bits                 // all finished frames, :2406-2409
:2481-2493  += in-flight frame_size_planned/estimated of other threads
:2581  q = get_qscale(h, &rce, wanted_bits_window/cplxr_sum, frame)
```

Then, for **ABR only** (`!b_vbv_min_rate`, i.e. not CBR — `:737-739`), the
"borrow/debt" / overflow term (`:2585-2599`):

```
time_done   = i_frame / fps            (VFR: from PTS, :2590-2591)
wanted_bits = time_done * bitrate
abr_buffer *= max(1, sqrt(time_done))  // :2595  <- growing window
overflow    = clip3f(1 + (predicted_bits - wanted_bits)/abr_buffer, 0.5, 2.0) // :2596
q          *= overflow                 // :2597
```

With defaults, `abr_buffer = 2*1.0*6e6 = 12e6` bits = 2 s of bitrate, then
grows as `sqrt(t)`. The correction is a *multiplicative qscale* bounded to
`[0.5, 2]` (≈ ±6 QP), and its denominator widens with elapsed time — the
opposite of a ratcheting EMA.

I-frame / GOP-boundary handling (`:2602-2608`, plus the P-frame accumulator at
`:1410-1420`):

```
if I && keyint_max>1 && last_non_b_pict_type != I:
    q = qp2qscale(accum_p_qp / accum_p_norm) / f_ip_factor        // :2606-2607
else if i_frame > 0 && not CRF:
    lmin = last_qscale_for[type]/lstep; lmax = last_qscale_for[type]*lstep
    if overflow > 1.1 && i_frame > 3: lmax *= lstep              // :2617-2618
    else if overflow < 0.9:           lmin /= lstep              // :2619-2620
    q = clip3f(q, lmin, lmax)                                    // :2622
else if CRF && qcompress != 1:
    q = qp2qscale(ABR_INIT_QP)/f_ip_factor                        // :2627
```

`accum_p_qp_update` (`:1410-1420`):

```
accum_p_qp   *= 0.95;  accum_p_norm *= 0.95;  accum_p_norm += 1;
accum_p_qp   += qp + (ip_offset if I else 0);
```

So an IDR's QP is predicted from a ~20-frame EWMA of *trailing P QPs* and then
made ~1.4× better quality (`/ip_factor`); it is not allowed to spike because a
scene cut happened. `:2637-2638` seeds P QP on frame 0 as `q*f_ip_factor`.

Finish (`:2629-2661`):

```
qp_novbv          = qscale2qp(q)                         // :2629
q                 = vbv_pass1(h, pict_type, q)            // :2631, §1.10
last_qscale_for[t] = last_qscale = q                      // :2634-2635
frame_size_planned = predict_size(&pred[t], q, last_satd)  // :2643
frame_size_estimated = frame_size_planned                  // :2660
return q
```

Back in `x264_ratecontrol_start` (`:1497-1538`):

```
if b_abr: q = qscale2qp(rate_estimate_qscale(h))          // :1499
if i_force_qp != X264_QP_AUTO: q = i_force_qp - 1         // :1521-1522 (qpfile)
q = clip3f(q, i_qp_min, i_qp_max)                         // :1524
qpm = f_qp_avg_rc = f_qp_avg_aq = q; accum_p_qp_update(q) // :1526-1534
```

B-frames have **no independent rate control** (`:2421-2455`): QP is a POC-
distance-weighted average of the two adjacent P-frame QPs plus `pb_offset`
(`:2438-2450`), with `pb_offset/2` if kept as a reference. This is why B-frames
do not need their own debt accumulator.

### 1.6 The long-term integral (the real "debt" mechanism) — `x264_ratecontrol_end`, `:1915-1928`

```
if sh.type != B: cplxr_sum += bits * qp2qscale(qpa_rc) / last_rceq        // :1918
else:            cplxr_sum += bits * qp2qscale(qpa_rc) / (last_rceq*f_pb_factor) // :1923
cplxr_sum          *= cbr_decay                                          // :1925
wanted_bits_window += f_duration * bitrate                               // :1926
wanted_bits_window *= cbr_decay                                          // :1927
```

`qpa_rc` is the frame's mean QP **before** AQ (`ratecontrol.h:89-90`, normalised
at `:1842`). `predict_size`/`update_predictor` (`:2109-2134`):

```
predict_size(p,q,var) = (p->coeff*var + p->offset) / (q*p->count)         // :2111
update_predictor: range=1.5; skip if var<10;
  new_coeff = max((bits*q - offset)/var, coeff_min);
  new_coeff_clipped = clip3f(new_coeff, old_coeff/1.5, old_coeff*1.5);
  decay everything by 0.5, count++, add new terms                        // :2116-2133
```

**The key algebra.** `qscale2bits` says `bits ∝ c / qscale^1.1`. Therefore
`bits * qp2qscale(qpa_rc) ∝ c / qscale^0.1 ≈ c` (nearly QP-independent), and
dividing by `last_rceq = c^0.4` gives

```
cplxr_sum += c^0.6        (per frame, duration-normalised)
rate_factor = (cumulative target bits) / Σ c_i^0.6
q_i         = c_i^0.4 / rate_factor = c_i^0.4 * Σc^0.6 / B_total
```

This is **exactly** the 2-pass `complexity^0.6` law that Merritt attributes to
the first pass (`doc/ratecontrol.txt`: "default formula … complexity ** 0.6"),
reconstructed online. It is the mathematical reason the controller is stable
(§2.1). It also means x264's "debt" is not a per-frame accumulator of
`actual - aim`: it is an *integral of QP-normalised complexity* versus an
*integral of target bits*.

### 1.7 What `qblur` and `cplxblur` actually do — correction to earlier notes

`f_qblur=0.5` (`base.c:424`) and `f_complexity_blur=20` (`base.c:425`) appear
**only inside `init_pass2`** (`ratecontrol.c:2932-3001`, `:3066`). Grep shows no
other use in `ratecontrol.c`: they are read at `:2943-2945`, used in the
Gaussian blur loops at `:2968-3001` (`exp(-j*j/200)` weights, ±`2*cplxblur`
frames) and in the QP smoothing at `:3066` (`exp(-d*d/(qblur*qblur))`).

**They do not exist in 1-pass ABR.** The earlier note
`05_RATE_CONTROL.md:278-285` (Rank 3) proposes `cplxblur=20` for our 1-pass
planner; that proposal borrows a 2-pass mechanism. If we want the equivalent
smoothing in 1-pass we must implement it ourselves — the closest native thing
is the 2-frame EWMA at `:2560-2566`. This correction does not invalidate the
idea (Gaussian ±20-frame blur of a *preloaded* complexity sequence is strictly
better than a 2-frame EWMA for our bimodal capture); it just must be attributed
to 2-pass, not to 1-pass ABR.

### 1.8 MB-tree offsets — `slicetype.c:1029-1089`, `ratecontrol.c:762-766`

When MB-tree is on, `qcompress=1` and `f_pb_factor=1` (`ratecontrol.c:762-766`),
so frame-level qscale depends only on duration (`:2011`). Quality is instead
distributed per MB by the propagation of distortion into future references
(`slicetype.c:1051-1085`, called at `:1501` and `:1678`):

```
strength = 5.0 * (1 - f_qcompress)                            // :1038
fps_factor = round(clip_dur(avg_dur)/clip_dur(frame->dur)*256/MBTREE_PRECISION) // :1031
intra_cost = (i_intra_cost[i] * i_inv_qscale_factor[i] + 128) >> 8             // :1041
propagate  = (i_propagate_cost[i] * fps_factor + 128) >> 8                    // :1044
log2_ratio = log2(intra+propagate) - log2(intra) + weightdelta                 // :1045
f_qp_offset[i] = f_qp_offset_aq[i] - strength * log2_ratio                    // :1046
```

`weightdelta = 1 - f_weighted_cost_delta[ref0_distance-1]` if positive (`:1032-1034`).
Meaning: an MB that many future frames depend on gets a negative offset (lower
QP). This is a per-MB signal; Vk264 has no per-MB QP syntax and no B/DPB, so it
is **not portable** (consistent with `PARITY_ROADMAP.md:90`).

AQ, by contrast, is a variance-based per-MB offset
(`ratecontrol.c:304-403`, `:394-395`: `qp_adj = strength*(log2(max(energy,1)) - (14.427 + 2*(BIT_DEPTH-8)))`)
— also unportable and already measured negative in this lab
(`PARITY_ROADMAP.md:92`).

### 1.9 Scene cuts, GOP boundaries, qpfile and zones

- **Scenecut** `slicetype.c:1384-1428`: computes half-res intra cost
  `i_cost_est[0][0]` and inter cost `i_cost_est[p1-p0][0]` (`:1394-1395`), then
  `res = pcost >= (1-f_bias)*icost` (`:1417`). Threshold
  `f_thresh_max = scenecut_threshold/100 = 0.4`, `f_thresh_min = 0.25*max`
  (`:1398-1400`), `f_bias` ramps with GOP position (`:1403-1415`). Called at
  `:1522-1528`; a hit sets `frames[1]->i_type = X264_TYPE_I`. Flash suppression
  looks ±1 future frame (`:1430-1468`). Final I→IDR promotion at
  `slicetype.c:1522-1528,1830-1865`; keyint_max forces IDR at `:1831-1843`.
  So x264 **knows about the cut before encoding the cut frame** and the
  I-frame QP law (§1.5) spreads its cost over the following P frames.
- **qpfile** `x264.c:1801-1848`: per-frame `framenum type QP`; sets
  `pic->i_qpplus1 = qp+1` (`:1846-1847`); consumed by
  `x264_ratecontrol_start` as `i_force_qp` and applied at
  `ratecontrol.c:1521-1522` **after** all calculation. It overrides ABR entirely
  for that frame but does not reset the integrators.
- **zones** `ratecontrol.c:1219-1343`: `--zones a,b,q=N` (force QP,
  `:1225-1226`) or `a,b,b=factor` (bitrate multiplier, `:1227-1228`), arbitrary
  param overrides (`:1243-1256`); default zone `[0,INT_MAX]`, factor 1
  (`:1313-1317`); `get_zone` scans from last to first (`:1333-1343`). Applied in
  `get_qscale` at `:2026-2032` (`q /= f_bitrate_factor` or forced QP), and in
  CQP at `:1513-1519`; reconfiguration at frame boundaries via
  `x264_ratecontrol_zone_init` (`:1422-1429`, called from `encoder.c:3478`).

### 1.10 VBV / CBR (present in x264, absent in our configuration)

Set up in `x264_ratecontrol_init_reconfigurable` (`ratecontrol.c:632-742`):
`buffer_rate = vbv_max_bitrate/fps` (`:713`), `buffer_size = vbv_buffer_size*1000`
(`:663,715`), `single_frame_vbv = buffer_rate*1.1 > buffer_size` (`:716`),
and for CBR

```
cbr_decay = 1 - (buffer_rate/buffer_size) * 0.5 *
            max(0, 1.5 - buffer_rate*fps/bitrate)                 // :718-719
b_vbv_min_rate = !b_2pass && ABR && vbv_max_bitrate <= bitrate    // :737-739
```

With `b_vbv_min_rate` true, the ABR overflow term is **disabled**
(`:2585`) and `cbr_decay<1` turns the integrators into a VBV-sized sliding
window (`:1925-1927`). `vbv_pass1` (`:2266-2397`) then uses lookahead to walk
planned future frames (`:2302-2309`), targeting 50–80 % buffer fill (`:2311-2327`),
with a reactive fallback for `lookahead=0` (`:2331-2359`), B/mini-GOP reserve
(`:2364-2390`), and finally `clip_qscale` (`:2242-2263`, `:2396`). Per-row VBV
adjustment happens in `x264_ratecontrol_mb` (`:1590-1747`, e.g. `:1662-1689`).

Vk264 has `vbv=0` and no HRD path; none of this is needed for local files, and
OBS output here also has no VBV contract. **Do not port VBV.** The one VBV idea
worth stealing is the *lookahead buffer walk*, but applied to our bit budget,
not to an HRD model (§4).

---

## 2. Why x264 is stable on bimodal content where our EMA ratchets

### 2.1 The driver is QP-independent; ours is QP-dependent

Our controller (`shaders/scan.comp:48-86`) measures
`actual = 32*total` *after* CABAC/CAVLC (`scan.comp:51`), forms
`rateRatio = actual/aim` (`:77`) and steps QP by
`clamp(round(3*log2(rateRatio)), -2, +2)` (`:79`). Because `actual` itself falls
when QP rises, this is a closed loop with gain and one-frame delay. On 30→60
duplicated capture the true demand alternates ≈4× / ≈1/20; `log2` alternates
≈+2 / ≈−4; the ±2 step cannot span it, so the loop limit-cycles and the
`bitDebt` accumulator (`:75`, clamped to ±30 budgets) keeps pushing QP toward a
wall. The opt-in `smoothAbr` path (`:52-74`) replaces this with an EMA of
`actual` but *still feeds a QP-dependent signal*, hence the measured ratchet to
QP 51 documented in its own comment (`:65-70`).

x264 breaks that loop in three independent places:

1. **Complexity is measured before encoding, at fixed QP 12**
   (`slicetype.c:45-48,514-545,1898`; `ratecontrol.c:2559`). Changing QP cannot
   change the driver.
2. **Actual bits are normalised before entering the integrator**:
   `bits*qscale/last_rceq ≈ c^0.6` (`ratecontrol.c:1918,2021`). This removes the
   QP dependence from the feedback signal itself. A 4× frame genuinely adds 4×
   to `cplxr_sum`; it does not masquerade as a controller error.
3. **The controller is an integral controller over all elapsed frames**
   (`rate_factor = wanted_bits_window/cplxr_sum`, `:2581`), not a derivative
   step on the last frame. A single expensive frame is amortised over the whole
   history instead of causing an emergency QP move.

### 2.2 The five concrete stabilisers (ranked by importance)

| # | Mechanism | Exact lines | Effect on bimodality |
|---|---|---|---|
| 1 | QP-normalised integral (`cplxr_sum`, `wanted_bits_window`) | `:1915-1928`, `:2581` | A 4× frame and a 1/20 frame both contribute their *true* complexity^0.6; no per-frame ratchet. The sequence total is enforced by construction. |
| 2 | `qcomp` exponent 0.4 (`pow(blurred,1-0.6)`) | `:2014`, `base.c:423` | Compresses a 20× complexity swing to `20^0.4≈3.3×` in qscale (≈8 QP), before the integral even acts. With `qcomp=1` (our "constant QP" analogy) this term is 1 — the mode that *does* oscillate. Our 12 failed fixed-exponent arms omitted the integral that gives this exponent meaning. |
| 3 | Growing overflow window `sqrt(time_done)` and clip `[0.5,2]` | `:2479`, `:2592-2597` | Early correction is strong, later correction is deliberately weak; the denominator grows so a late burst cannot ratchet QP. Contrast our fixed `fps/2` window (`scan.comp:48`). |
| 4 | Error-gated asymmetric slew | `:2615-2622`, `lstep=2^(4/6)` (`:828`) | QP can move at most ×1.587 per frame by default; the window widens in the *error-correcting direction* only when `overflow>1.1` (and frame>3) or `<0.9`. Sustained oscillation does not keep widening it. Contrast our unconditional ±2 QP step (`scan.comp:79`). |
| 5 | Lookahead + I-frame-from-P-history | `slicetype.c:1793-1797,1898`; `ratecontrol.c:1410-1420,2602-2608` | The IDR (the 20× outlier) is priced from a 20-frame P-QP EWMA divided by `ip_factor`, with the cut known in advance. Our IDR is strictly periodic (`presets.hpp:68`) and is charged after the fact (`scan.comp:33-41`). |

Secondary: B-frames being QP-slaved to adjacent P frames (`:2421-2455`) means
they add no independent control variable; AQ/MB-tree are per-MB and do not enter
the frame-level integrator except through `qpa_rc` normalisation.

### 2.3 What this implies for us

The failure is **not** a gain-tuning problem. Thirteen QP-floor variants, twelve
`qcomp` arms, a larger debt window and asymmetric clamps were all reverted
(`PARITY_ROADMAP.md:141`). All of them changed constants inside a controller
whose *input* was still post-encode, QP-dependent bits. Porting mechanisms 1–4
above — even without lookahead — is qualitatively different from any of those
experiments.

---

## 3. Mechanism-by-mechanism map to Vk264

### 3.1 What exists today

| x264 mechanism | Vk264 today | Evidence |
|---|---|---|
| QP decision before encode | `frameQp() = clamp(initialQp + (IDR?rc.idrQpOffset:rc.interQpOffset),0,51)` | `layout.glsl:55-59`; consumed by `bitstream.glsl:125`, `parallel_entropy.glsl:79`, `residual.comp:376`, `deblock.comp:15`, `spatial.comp:90` |
| Rate state | 12-byte GPU buffer, 3 ints `{interQpOffset, bitDebt, idrQpOffset}`, zeroed at frame 0, never read by host | `layout.glsl:25`, `gpu.cpp:324,613` |
| Bit measurement | `total = Σ slice words`; `outputWords[0]=4*total`; host gets `EncodedFrame.bytes`; shader uses `actual=32*total` | `scan.comp:8-28,51`; `gpu.hpp:26-30` |
| Reactive controller | `bitDebt += actual-allocation` clamped ±30 budgets; `correction=clamp(bitDebt/(fps/2),±allocation/4)`; `aim=...`; `step=clamp(round(3*log2(actual/aim)),-2,+2)` | `scan.comp:48-50,75-86` |
| IDR reserve | `idrExtra = budget*min(5,(gop-1)/8)`, redistributed over the GOP | `scan.comp:33-41` |
| QP floor / ceiling | floor 18 at ≥50 fps; opt-in `smoothAbr` ceiling 32 at ≥50 fps | `scan.comp:69,84` |
| Complexity input | none. `lookahead_frames=0`, `b_frames=0` | `main.cpp:200`; `presets.hpp:63-68` |
| Frame-type / IDR | strictly `frameIndex % gop == 0`; no scenecut | `layout.glsl:48`, `presets.hpp:68` |
| Target | `targetBits = bitsPerFrame(kbps)` = per-frame integer budget; `bitrate = targetBits*fps` | `presets.hpp:27-30`, `main.cpp:141`, `obs-vk264.cpp:153` |
| OBS constraints | frame-at-a-time encode, no preload; `initialQp` capped to 14 when ABR | `obs-vk264.cpp:98-128,149-168` |
| CLI constraints | **all source frames preloaded** before `EncoderGpu` construction | `main.cpp:207-231` |

### 3.2 What CLI can compute that OBS cannot

CLI has the full source in RAM (`main.cpp:209`, `EncoderGpu` at `:231`), so it
can afford:

- **Pass 0a complexity**: 16×16/8×8 luma thumbnail per frame, SAD to i−1, i+1,
  i+2 and a few integer MV offsets; variance/edge energy; duplicate flag;
  scene-cut flag. This is the direct analogue of x264's half-res lowres cost
  (`slicetype.c:514-545`) and is QP-independent.
- **Full Gaussian blur ±20 frames** of the complexity sequence — the 2-pass
  `cplxblur` law (`ratecontrol.c:2976-3000`) made available to 1-pass by the
  preload. This is the proper fix for the 30→60 cadence; it is *not* something
  x264's 1-pass does natively (§1.7).
- **Closed-form global rate factor**: `Σ c_i^0.6` over the entire clip, so
  `RF = B_total / Σc^0.6` exactly, with no convergence transient.
- **Probe calibration** of `bits_i(q)` using `EncodedFrame.bytes`
  (`gpu.hpp:28`) on a subsample, fitted to `qscale2bits`
  (`ratecontrol.c:216-223`), giving a real `bits(q)` inverse.
- **Advance IDR reserve and scene-cut QP pinning** from future SAD spikes.
- **Per-frame QP plan** delivered through one new `uint32 plannedQp` in
  `FrameConfig` (`gpu.hpp:14-24`) read by `layout.glsl:55-59` — no bitstream or
  OBS ABI change; QP is already signalled per slice.

OBS has only the current frame in host memory and the previous `EncodedFrame`
(result bytes). It cannot look ahead, cannot know `Σc^0.6`, cannot run probes,
cannot reserve bits for a future cut. Therefore:

> **OBS should be a faithful, host-side port of x264's 1-pass ABR** (§1.3–1.6):
> current-frame QP-independent thumbnail complexity + 2-frame EWMA + integral
> rate factor + `qcomp` exponent + growing overflow window + error-gated slew.
> **CLI should use the same state machine but with the global Σ and Gaussian
> blur substituted for the EWMA and integral transient** — i.e. the planned
> variant. Both are subsets of the same controller; the differential is only
> the complexity lookahead/Σ availability.

### 3.3 New state / buffers required

Recommended: **implement the controller on the host, not in the shader.**
`EncodedFrame.bytes` is already host-visible (`gpu.hpp:28`), and the current
frame is in host memory before upload, so no GPU readback, no new storage
buffer and no new barrier are needed. `scan.comp` becomes a pure word-count and
QP-signalling pass again; its RC block (`:30-86`) is bypassed when the host
supplies `plannedQp`.

Host state to add (one small struct per encoder instance):

```
int64_t cplxr_sum;          // Q16.16 or pair of int32
int64_t wanted_bits_window; // integer bits
int32_t short_sum, short_count;   // EWMA
int32_t last_rceq;          // Q16.16 = pow(blurred,0.4)
int32_t accum_p_qp, accum_p_norm; // Q16.16
int32_t last_qscale[2];    // I/P, Q16.16
int64_t predicted_bits;    // cumulative actual bits + in-flight estimate
int32_t frame_index;
```

GPU-side changes: **one push constant** `plannedQp` (0 = "use existing shader
RC"), consumed by `layout.glsl:55-59`. `RateState` (`layout.glsl:25`) can stay
as-is for the legacy path; if the RC is moved fully host-side it can be removed
with it. No new descriptor, no new pipeline, no OBS ABI change
(`obs/obs-vk264.cpp:100-109` needs no new field for ABR mode).

If instead we wanted to keep RC on the GPU, `RateState` would have to grow from
12 bytes to ~64 bytes (two 64-bit integrators, EWMA state, `last_rceq`,
`accum_p_*`, `last_qscale[2]`, `plannedQp`) and `scan.comp` would need access to
the *current* frame's complexity before `frameQp()` is used by other shaders —
which it does not have today (it runs at the end, stage order `gpu.cpp:44`).
This is why the host port is the lower-risk route.

### 3.4 What is deliberately **not** ported

- MB-tree (`slicetype.c:1029-1089`) and AQ (`ratecontrol.c:304-403`): need
  per-MB QP / B-pyramid; both measured negative here (`PARITY_ROADMAP.md:90,92`).
- VBV/CBR (`ratecontrol.c:632-742,2266-2397,1590-1747`): no VBV contract in our
  CLI or OBS path.
- `qblur`/`cplxblur` as *x264 1-pass* features: they are 2-pass-only (§1.7).
  We may still implement the Gaussian blur as our own CLI-side extension.
- B-frame QP law (`ratecontrol.c:2421-2455`): we have no B-frames.

---

## 4. Concrete controller pseudocode for Vk264

Two entry points share one core. All math is integer/fixed-point; `Q=16` means
a value scaled by 65536. No fixed QP ceiling/floor is used — the only bounds
are the legal `[0,51]` (and an optional practical `[10,48]` *derived from the
rate solve*, not a constant cap on controller output).

### 4.0 Shared helpers (integer-friendly)

```
QP_BD   = 0                              // 8-bit; common/common.h:58
BASE_DUR = 4/100                         // ratecontrol.h:34, in seconds
CLIP_DUR(d) = clamp(d, 1/100, 1)         // ratecontrol.h:38-40

// qscale in Q16; qp2qscale(qp) = 0.85*2^((qp-12-BD)/6)   [ratecontrol.c:203-206]
qp2qscale_Q16(qp):
    e = (qp - 12 - QP_BD) * 10923        // 65536/6
    return 55706 * exp2_fixed(e) >> 16   // 0.85*65536 = 55706

// qscale2qp(qs) = 12+BD + 6*log2(qs/0.85) [ratecontrol.c:207-210]
qscale2qp_Q16(qs):
    if qs <= 0: qs = 1
    return 12 + QP_BD + round(6 * log2_fixed((qs<<16)/55706))

// last_rceq_Q16 = pow(blurred, 1-qcompress) = pow(blurred,0.4)  [:2014]
//   computed once per frame from a 256-entry log2/exp2 LUT (Q6 log domain):
pow04_Q16(c): return exp2_fixed( (log2_fixed(c)*2) / 5 )   // 0.4 = 2/5

// duration-normalised complexity: c_norm = satd * BASE_DUR / duration  [:2562]
norm_satd(satd, dur): return satd * 4 * 100 / max(1, round(dur*1000)) ... // exact rationals with fps_num/fps_den
```

Use a single 512-entry `log2` LUT indexed by the top byte of the mantissa and a
matching `exp2` LUT; error <0.5 QP step, ample for RC. All accumulators that can
exceed 32 bits (`cplxr_sum`, `wanted_bits_window`, `predicted_bits`) are `int64`.

### 4.1 Complexity feature (both paths) — the analogue of `x264_rc_analyse_slice`

```
thumbnail(frame, 16x16 luma) -> T[i]
c_satd(i) = lam * min(
      SAD(T[i],  T[i-1],  dx,dy in {(0,0),(±1,0),(0,±1)}),
      SAD(T[i],  T[i+1],  same)        [CLI only],
      intra_SAD(T[i])                 // DC + 3 directional)
```

`lam` is a fixed constant (x264 uses `x264_lambda_tab[12]≈1`; any fixed weight
works because the integrator absorbs scale). Critically **not** derived from
the frame's QP. Duplicate frames yield `c ≈ 0`, exactly like x264's lowres
`i_satd` on a duplicated capture.

### 4.2 OBS path — online x264-1pass port

```
// ---- once, at init (x264_ratecontrol_new :809-819) ----
budget      = bitsPerFrame(config, kbps)                  // presets.hpp:27-30
bitrate     = budget * fps
cplxr_sum   = 0.01 * pow(7e5,0.6) * sqrt(mb_count)       // :816  (use integer LUT)
wanted_bits_window = bitrate / fps                        // :817
short_sum = 0; short_count = 0
accum_p_qp = 24*65536*0.01; accum_p_norm = 0.01*65536    // ABR_INIT_QP=24 (:812)
last_qscale[I]=qp2qscale(24); last_qscale[P]=qp2qscale(24)
last_rceq = pow04_Q16(1)
lstep_Q16 = 104053                                       // 2^(4/6)*65536 (:828)
ip_factor = 1.4; pb_factor = 1.3
rate_tol  = 1.0                                           // base.c:404

// ---- per frame, BEFORE encode ----
dur        = 1/fps                                        // CLIP_DUR :ratecontrol.h:40
c_norm     = norm_satd(c_satd(i), dur)                    // :2562
short_sum   = short_sum/2 + c_norm                        // :2560-2561
short_count = short_count/2 + 1
blurred     = short_sum / short_count                     // :2566
last_rceq   = pow04_Q16(blurred)                          // :2021
rf          = wanted_bits_window / cplxr_sum              // :2581  (Q16 division)
q           = last_rceq / rf                              // :2014,:2022

// overflow / borrow-debt (:2479, :2592-2597)
t_done      = frame_index / fps
wanted      = t_done * bitrate
abr_buf     = 2 * rate_tol * bitrate * max(1, isqrt(t_done))   // :2479,:2595
overflow    = clip( 1 + (predicted_bits - wanted)/abr_buf, 1/2, 2 ) // :2596
q          *= overflow

// I-frame law (:2602-2608 + :1410-1420)
if is_IDR and gop>1 and last_non_b != I:
    q = qp2qscale_Q16(accum_p_qp/accum_p_norm) / 1.4      // :2606-2607
else if frame_index > 0:
    lo = last_qscale[type]/lstep_Q16; hi = last_qscale[type]*lstep_Q16
    if overflow > 1.1 and frame_index > 3: hi *= lstep_Q16 // :2617-2618
    if overflow < 0.9:                       lo /= lstep_Q16 // :2619-2620
    q = clamp(q, lo, hi)                                   // :2622

qp = clamp(qscale2qp_Q16(q), 0, 51)                        // :1524 (no extra cap)
push qp as pc.plannedQp; encode; read EncodedFrame.bytes

// ---- AFTER encode (:1915-1928, :1410-1420) ----
bits = 8 * EncodedFrame.bytes
cplxr_sum += bits * qp2qscale_Q16(qp_avg) / last_rceq     // :1918 (I/P)
wanted_bits_window += dur * bitrate                        // :1926
predicted_bits += bits
accum_p_qp   = (accum_p_qp*19)/20 + qp + (ip_offset if I else 0) // 0.95 :1413-1419
accum_p_norm = (accum_p_norm*19)/20 + 1
last_qscale[type] = q; last_non_b = type if type != B
```

Note there is **no `bitDebt` clamp** in this design: the integral *is* the debt,
and it is QP-normalised. This is the substantive difference from
`scan.comp:75`.

### 4.3 CLI path — preloaded, with Gaussian blur and closed-form rate factor

```
// Pass 0a: compute c[i] for all N frames (with i+1,i+2 lookahead)
// Pass 0b (optional but recommended): 1-3 fixed-QP probe encodes, fit
//          bits_i(q) = a_i * (0.85*2^((q-12)/6))^-1.1 * c_i^0.6 + hdr_i
//          using x264's qscale2bits (:216-223) as the shape.
// Pass 0c: Gaussian blur of c[i] over +-20 frames with exp(-j*j/200)
//          weights and the (1-(i_count/nmb)^2) taper — the 2-pass law
//          at ratecontrol.c:2968-3000, here made available to 1-pass.
S = sum_i pow06_Q16(cblur[i])                              // c^0.6
RF = B_total / S                                           // closed form of :2581
for i in 0..N-1:
    q_i   = pow04_Q16(cblur[i]) / RF                       // :2014,:2022
    qp_i  = qscale2qp_Q16(q_i)
    // keep x264's slew as a final guard, but do NOT clamp to a fixed
    // ceiling/floor:
    qp_i  = clamp(qp_i, last_qp - 12, last_qp + 12)        // generous, rate-derived
    qp_i  = clamp(qp_i, 10, 48)                             // legal/practical only
    if duplicate[i]: qp_i = qp_[twin]                       // cadence pinning
    if scene_cut[i] or i%gop==0: reserve via S redistribution, not a step
    plannedQp[i] = qp_i
// Encode with the OBS-style host integrator above running in parallel as a
// ±1 safety drift correction; skip the log2(rateRatio) step entirely.
```

The `±12` slew is deliberately 6× wider than x264's `lstep` because the plan is
already globally consistent; it exists only to absorb model error from Pass 0b.
`[10,48]` is a *sanity* range derived from what 6000–8000 kb/s at 1080p60 needs,
not a controller ceiling: the algorithm will naturally return QP≈38–44 on
static capture and QP≈24–30 on game content, with no floor-18 hack
(`PARITY_ROADMAP.md:101,141`) and no smooth-ABR ceiling-32 hack
(`scan.comp:69-70`).

### 4.4 Expected behaviour by content

| Case | Complexity signature | Controller response | Expected rate / QP |
|---|---|---|---|
| Static / duplicated 30→60 capture | ~25 % frames at 4× `aim`, rest ~1/20 (roadmap:68) | Blurred `c` smooths the cadence; `Σc^0.6` absorbs both classes; duplicates pinned to twin QP | 6000 kb/s ±5 %, QP ~38–44 on duplicates, ~30–34 on the real frames; **no** 20.93 Mb/s overshoot and **no** ratchet to 51 |
| Easy capture | `c` near zero | `q` large → QP high; integrator stops spending | ~6 Mb/s instead of 23.75 Mb/s (PR #516 behaviour, but achieved by the rate solve rather than a floor) |
| Hard 60 fps game @6000/8000 | large `c`, moderate variation | QP ~24–30, ±1 drift only | ~6121 kb/s, VMAF ≈91.6 (fixed-QP ceiling +2.9) |
| 60 fps trailer @6000 | high spatial detail | QP ~26–32 | ~5872 kb/s, VMAF ≈80.4 (ceiling +8.1) |
| Scene cut / IDR | one frame ~20× budget | Pre-reserved against `Σc^0.6`; I QP from P-history/1.4, not a reactive step | Cut costs ~1 GOP's reserve, repaid over ≤125 frames with ≤1 QP/frame drift |

### 4.5 Measurement gates

Standing gates (unchanged from `PARITY_ROADMAP.md:165-168`):
achieved rate **±5 %**; VMAF ≥ fixed-QP ceiling −0.3 at matched rate; encode
loop ≥100 fps; process ≥85 fps; `ctest` 45/46 (only the known
`x264_cavlc_packet_parity` failure); `check.py` 13/13; `ffmpeg -err_detect
explode` clean.

New gates specific to this controller (all cheap to log from `EncodedFrame`):

1. **No ratchet**: for every consecutive non-IDR pair, `|QP_i−QP_{i−1}| ≤ 6`
   (OBS) or `≤12` (CLI plan), and no monotone run of >8 steps in one direction
   while `predicted_bits < wanted_bits − abr_buffer`.
2. **Integrator convergence**: running rate error over 1 s / 2 s / 5 s windows
   must shrink monotonically after the first 2 s (the `sqrt(time_done)` window
   at `:2595` predicts this).
3. **Duplicate-pair QP equality**: on `obs_new_src.yuv`, paired 30→60 frames
   have identical QP (Pass 0a flag) and a bit ratio ≤1.5×.
4. **No hard-clamp activity**: count of frames where QP equals 0, 18, 32, 48 or
   51 must be 0 except genuine IDRs; if the `[10,48]` sanity range is hit
   constantly, the rate model is mis-scaled, not the cap too tight.
5. **QP independent of previous QP on IDR**: IDR QP must track
   `accum_p_qp/accum_p_norm / 1.4` (`:2606-2607`) within ±3, proving the cut is
   not driving a feedback spike.
6. **OBS/CLI parity**: same source, same kbps → achieved rate within 2 % between
   the online OBS path and the planned CLI path (if OBS diverges, lookahead is
   masking a bug in the integral).

**Decision rule.** If the CLI plan hits ±5 % but OBS does not, keep the plan and
ship duplicate pinning as the OBS fallback
(`05_RATE_CONTROL.md:255-272`). If neither hits ±5 %, the fault is in the
`bits(q)` model (`ratecontrol.c:216-223`), not in the feedback loop — add a probe
encode. Do not reintroduce a QP floor, ceiling or wider `bitDebt` clamp: all
13/12/× variants are already closed (`PARITY_ROADMAP.md:141`).

---

## 5. Exact citation index

All line numbers are from HEAD `48ac002e` as read locally.

**x264 rate control (`encoder/ratecontrol.c`)**
- `:36-61` `ratecontrol_entry_t`; `:63-70` `predictor_t`; `:72-179` `x264_ratecontrol_t`
- `:199-202` qscale == lambda terminology; `:203-206` `qp2qscale`;
  `:207-210` `qscale2qp`; `:216-223` `qscale2bits`
- `:304-403` variance AQ (`:394-395` the `14.427+2*(bitdepth-8)` law)
- `:632-742` VBV/CBR init (`:713-719`, `:737-739`)
- `:744-861` `x264_ratecontrol_new` (`:753-754`, `:757-768`, `:770-774`,
  `:809-819` ABR constants, `:821-829` offsets/lstep, `:833-860` predictors)
- `:1219-1343` zones; `:1345-1355` summary (`final ratefactor` law)
- `:1410-1420` `accum_p_qp_update` (0.95 decay); `:1422-1429` zone_init
- `:1432-1538` `x264_ratecontrol_start` (`:1499`, `:1521-1524`, `:1534`)
- `:1590-1747` per-row VBV `x264_ratecontrol_mb`
- `:1829-1994` `x264_ratecontrol_end` (`:1915-1928` the integral)
- `:2003-2035` `get_qscale` (`:2011`, `:2014`, `:2021-2023`, `:2026-2032`)
- `:2037-2107` `get_diff_limited_q` (2-pass I/B QP law, `:2047-2068`)
- `:2109-2134` `predict_size` / `update_predictor`
- `:2242-2263` `clip_qscale`; `:2266-2397` `vbv_pass1`
- `:2400-2663` `rate_estimate_qscale` (`:2406-2409` total_bits,
  `:2421-2455` B law, `:2479` abr_buffer, `:2481-2493` in-flight,
  `:2495-2543` 2-pass branch, `:2545-2600` **1-pass ABR**,
  `:2559-2566` EWMA, `:2581` rate_factor, `:2595-2597` overflow,
  `:2602-2608` IDR law, `:2615-2622` asymmetric slew, `:2629-2643` finish)
- `:2765-2781` thread state copy; `:2932-3100` `init_pass2` (the only user of
  `qblur`/`cplxblur`)

**x264 slicetype (`encoder/slicetype.c`)**
- `:45-61` `lowres_context_init` (`X264_LOOKAHEAD_QP`, lambda, HEX/subpel-4)
- `:514-545` `slicetype_mb_cost` (8×8 lowres SATD, `lowres_penalty=4`)
- `:836-995` `slicetype_frame_cost` (`:983-985` B scaling)
- `:1029-1049` `macroblock_tree_finish` (`strength=5*(1-qcompress)`, `:1038-1046`)
- `:1051-1089` `macroblock_tree_propagate`; `:1091+` `macroblock_tree`;
  call sites `:1501`, `:1678`
- `:1384-1428` `scenecut_internal` (`:1394-1395,1398-1417`);
  `:1430-1468` flash suppression; `:1473+` `x264_slicetype_analyse`;
  `:1522-1528` cut→I
- `:1745-1869` `x264_slicetype_decide` (`:1793-1797` trigger,
  `:1830-1865` GOP/IDR finalisation)
- `:1898` comment: costs precomputed for `x264_rc_analyse_slice`
- `:1976-2013` `x264_rc_analyse_slice` (`:1995`, `:2008-2010`)

**Other x264**
- `encoder/encoder.c:3478` zone_init; `:3857` rc_start; `:3978` rc_end
- `x264.c:1801-1848` qpfile (`:1846-1847` `i_qpplus1=qp+1`)
- `common/base.c:383-427` defaults; `:402` CRF default; `:423-425` qcomp/qblur/cplxblur
- `common/common.h:58-64` `QP_BD_OFFSET`, `QP_MAX`, `X264_LOOKAHEAD_QP`
- `common/tables.c:96-110` lambda table; `x264.h:437-479` RC param docs,
  `:211-212` `X264_RC_ABR`/`X264_QP_AUTO`

**Vk264 (read-only)**
- `shaders/scan.comp:8-28` word count, `:30-41` IDR reserve, `:48-50` debt/aim,
  `:51` actual bits, `:52-74` opt-in smooth EMA, `:75-86` reactive step/floor
- `shaders/layout.glsl:25` `RateState`; `:48` idr(); `:55-59` `frameQp()`
- `shaders/bitstream.glsl:125`, `parallel_entropy.glsl:79`, `residual.comp:376`,
  `deblock.comp:15`, `spatial.comp:90` — consumers of `frameQp()`
- `src/gpu.hpp:14-24` `FrameConfig`, `:26-30` `EncodedFrame.bytes`
- `src/gpu.cpp:44` stage order; `:324` 12-byte Rate buffer; `:613` frame-0 reset
- `src/presets.hpp:27-30` `bitsPerFrame`; `:63` QP 30; `:68` 5 s GOP
- `src/main.cpp:141` `--bitrate`; `:207-231` full-source preload
- `obs/obs-vk264.cpp:98-128` encoder struct/no preload; `:149-168` config,
  `initialQp` cap 14, `smoothAbr` flag

**Prior lab notes (read-only context):** `notes/pipeline/05_RATE_CONTROL.md`,
`notes/PARITY_ROADMAP.md`. Their source line numbers refer to commit
`90a61ec`; the formulas agree, with the `qblur`/`cplxblur` attribution
corrected in §1.7.
