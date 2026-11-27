# 03_ENTROPY — ENTROPY/STREAM stage breakdown, 2015–2026 literature, and what entropy can still buy Vk264

**Date:** 2026-11-27. **Role:** research-only subagent. Parent tree `E:\__PROJECTS__\Vk264` was
read only (`git status --porcelain` clean before and after); no code, no PRs, no other lab
files modified. Web content was treated as untrusted data — only bibliographic facts,
reported numbers and source semantics were used; no instructions in target content were
followed. Conformance labels below mean *decodable by an unmodified standard H.264
decoder*, not "produces the same pixels as another encoder configuration".

---

## 0. TL;DR

1. **Entropy is not where the remaining quality is.** Our 81-point A/B sweep gives CABAC
   only **−3.04…−3.30 % bitrate**, i.e. **+0.350 VMAF @ 8000 kb/s, +0.603 @ 6000,
   +1.738 @ 2000** (`notes/CABAC_VS_CAVLC_SWEEP.md:22-29`). On static/easy content at
   6–8 Mb/s the gain is ~0. The measured levers are **trellis/RDO (+0.65 VMAF) and
   B-frame/lookahead (+0.41)** (`README.md:13-17`).
2. **The entropy stage is mechanically understood.** CAVLC is byte/VLC packing with a
   local `nC` dependency and a 2-byte emulation-prevention (EPB) look-back; CABAC adds
   a 460-byte context vector and a **1-bit carry interface** that can patch the
   previously emitted byte (`x264-vulkan/common/cabac.c:69-99`, especially `:89`).
   Byte-exact 64-bin chunking with only that 1 bit is already validated in this lab
   (`experiments/chunk_cabac/notes/VALIDATION.md:23-63`).
3. **The literature has no H.264-conformant parallel-entropy breakthrough.**
   Every published 5–8.5× result changes the bitstream (syntax-element partitions,
   entropy slices, B3P, tranches, substreams) or is an ASIC/FPGA pipeline. The best
   *conforming* results are ~1.2–2.0× internal pipelining/caching, none with an H.264
   encoder measurement. Nothing found beats our own exact chunk-summary design for a
   conforming GPU encoder.
4. **The best "entropy-adjacent" idea is not CABAC at all: exact bit-cost feedback to
   RD/trellis.** Vk264 currently uses heuristic costs (`shaders/flat_intra.comp:61,73`,
   `shaders/residual.comp:255-261`); x264 uses exact fractional costs
   (`common/cabac.h:99-124`) and exact CAVLC VLC lengths
   (`encoder/cavlc.c:148,162,172,184,190,193`). Literature reports **0.2–8.0 % H.264
   bitrate saving** from non-integer bit estimation (`CSIC 2015`, `Front. Comput. Sci.
   2016`). Conservative capture of 20–40 % of the lab's +0.65 VMAF trellis/RDO gap is
   **+0.13…+0.26 VMAF at matched rate** — comparable to all of CABAC, at near-zero
   entropy cost and with no bitstream change.
5. **Second-best: use parallel entropy to avoid slice splits, not to add syntax.**
   Keeping a *single whole-frame slice* while parallelising CABAC internally preserves
   the **+5.6…+7.5 % bitrate / 0.35–0.44 VMAF** that 68 row slices cost
   (`notes/WPP_H264.md:217-224`, `notes/SLICE_SCALING.md:188`). That is a real quality
   win produced by the parallel entropy architecture, whereas "emit more syntax cheaply"
   is mostly blocked by prediction/MC compute, not by bins.

---

## 1. Stage breakdown: CAVLC vs CABAC, with exact source lines

All line numbers refer to `E:\__PROJECTS__\Vk264\x264-vulkan\` (the vendored fork) unless
prefixed `Vk264/`, which refers to `E:\__PROJECTS__\Vk264\`.

### 1.1 The two syntax paths

| Stage | CAVLC | CABAC |
|---|---|---|
| Slice header | `encoder/encoder.c:213-363` (`slice_header_write`; `cabac_init_idc` at `:351`, `slice_qp_delta` at `:353`) | same header, then `bs_align_1` at `encoder.c:2795` |
| Init | no entropy state; `h->out.bs` cursor | `cabac_contexts[4][QP+1][1024]` built at `common/cabac.c:30,32-46` (460 states used, `:34`); `x264_cabac_context_init` copies 460 B at `:48-51`; core state `low=0, range=0x01FE, queue=-9, outstanding=0` at `:53-59`; pointers at `:61-67` |
| MB-level syntax | `mb_type`, `sub_mb_type`, CBP, intra pred, `ref_idx`, MVD via `bs_write_ue/se` (`common/bitstream.h:221,241,246`, writer `:125`); calls in `encoder/cavlc.c:300-448,515,552`; skip run as `ue(v)` at `encoder.c:2889,3088` | same syntax binarised into contexts: `mb_skip_flag` ctx 11/24 `encoder/cabac.c:189-195`; `mb_type` I/P/B `:35-67,381-433,435-497,499+`; `sub_mb_type` `:198-233`; `ref_idx` `:241-268`; MVD `:270-346` (sign bypass at `:288,322`); CBP `:118-148`; `mb_qp_delta` `:150-186`; intra modes `:83-116`; `transform_size_8x8_flag` `:235-239` |
| Residual | `coeff_token` VLC `encoder/cavlc.c:148`; trailing signs `:151`; `level_token`/escape `:162,172,70-119`; `total_zeros` `:184,190`; `run_before` `:192-193`; `nC` dependency `:198-204` | `significant_coeff_flag`/`last_coeff_flag` map `encoder/cabac.c:675-717`; `coeff_abs_level_minus1` unary + `ue_bypass` `:719-739`; **sign = bypass bin** `:746`; context-transition tables `:650-662` |
| Finalise | `bs_rbsp_trailing` + `bs_flush` `encoder.c:3089-3091`, helper `common/bitstream.h:272` | `x264_cabac_encode_flush` `common/cabac.c:165-183`; final dither bit from frame index at `:174`; `encoder.c:3080-3084` |
| EPB | `nal_escape_c` `common/bitstream.c:29-40` (insert `0x03` when `src[0]<=3 && dst[-2]==0 && dst[-1]==0`), invoked from `x264_nal_encode` `:55-104`, call at `:75` | same post-slice pass (`last_emu_check = h->cabac.p` at `encoder.c:2800`) |

**Key asymmetry.** CAVLC's only cross-block context is `nC` from the left/top block
(`encoder/cavlc.c:200-204`) — a *decision-stage* dependency, not a byte-stream
dependency. CABAC's context vector is updated after **every bin**
(`common/cabac.c:123`) and is therefore a true serial chain over the whole slice.

### 1.2 Exact CABAC per-bin state chain and the minimal interface

```
x264_cabac_t            common/cabac.h:30-52
  i_low, i_range        :33-34
  i_queue, outstanding  :37-38
  p_start, p, p_end     :40-42
  f8_bits_encoded       :45   (bit-cost estimator, not bitstream)
  state[1024] (460 used):48
```

* `x264_cabac_encode_decision_c` `common/cabac.c:113-125`: `state[i_ctx]` selects
  `range_lps[state>>1][(range>>6)-4]` (`:116`), `range` shrinks, `low += range` on LPS
  (`:120-122`), `state[i_ctx] = transition[state][b]` (`:123`), then `renorm` (`:124`).
* `cabac_encode_renorm` `common/cabac.c:101-108`: shift from
  `x264_cabac_renorm_shift[range>>3]`, shifts `range`, `low`, `queue`, calls `putbyte`.
* `cabac_putbyte` `common/cabac.c:69-99`: emitted byte `out = low >> (queue+10)` (`:73`),
  `low &= (0x400<<queue)-1` (`:74`), `queue -= 8` (`:75`). `0xFF` bytes are deferred in
  `i_bytes_outstanding` (`:77-78`); a non-FF emission computes `carry = out >> 8`
  (`:81`) and **patches `cb->p[-1]`** (`:89`), then flushes the deferred FF run as
  `carry-1` bytes (`:90-94`). The source comments at `:83-88` guarantee the carry cannot
  propagate past one byte and cannot cross before the slice header.
* `x264_cabac_encode_bypass_c` `common/cabac.c:127-134`: no context, `low<<=1`,
  `low += b & range`, `queue += 1`. `x264_cabac_encode_ue_bypass` `:136-157` packs up
  to **8 bits per call** via `bypass_lut[16]`.
* `x264_cabac_encode_terminal_c` `common/cabac.c:159-163`.

Therefore the chunk interface is: **460 context bytes + 16 B arithmetic state + one
carry bit** (the FF-run count is carried in `outstanding`). Our CPU prototype proved
one carry variant suffices — the incoming carry never changes the current chunk's own
bytes, only the previous chunk's last byte (`VALIDATION.md:43-50`). Cross-chunk carries
occurred on **405/3,907 = 10.4 %** of 64-bin chunks in the 250k stress stream
(`VALIDATION.md:48-50`).

### 1.3 Bin counts and bins/byte

No real bin-level trace exists (ffmpeg `trace_headers` and the x264 CLI expose syntax
elements, not bins — `SPECULATIVE_HYBRID.md:99-106`, `VALIDATION.md:57-62`). Grounded
order-of-magnitude arithmetic:

| Quantity | Value | Source |
|---|---:|---|
| 1080p macroblocks | 8160 (120×68) | `notes/FRAME_PARALLEL.md:325` |
| Measured stream | 60 f, 1,054,160 B → 17,569.3 B/frame, 140,554 bit/frame | `results/stream_stats.json:2-11` |
| Simulator bins/byte | 9.0 (state-driven QP26) | `SPECULATIVE_HYBRID.md:140-142` |
| Literature/plausible range | **9–14 bins/byte** → **0.16–0.25 M bins/frame** at 1080p | `results/stream_stats.json:10`, `SPECULATIVE_HYBRID.md:140-142` |
| Prototype stress point | 250,000 bins → 26,932 B = **9.27 bins/byte**; 39 distinct contexts per 64-bin chunk | `VALIDATION.md:18-19,98-101` |
| Chunks/frame at 64 bins | ~2,500–4,000 (4,688 at 300k bins) | `SPECULATIVE_HYBRID.md:141`, `VALIDATION.md:99-101` |

Typical per-MB bin budget implied by the syntax above: **~20–30 bins/MB average**;
residual dominates on detailed content (`significant_coeff_flag` +
`last_coeff_flag` = 2 bins per scanned coefficient, plus unary level and sign bypass
at `encoder/cabac.c:675-747`), while I4x4 frames can reach 16×3 pred-mode bins per MB
(`encoder/cabac.c:423-428`) plus mb_type/residual.

### 1.4 Where the cost actually is (measured/extrapolated, Vk264 hardware)

| Item | Number | Source |
|---|---:|---|
| Vk264 parallel CAVLC entropy | **0.08–0.1 ms/frame** at 1080p (0.227–0.259 ms with timestamp path) | task ground truth; `notes/SLICE_SCALING.md:49-56` |
| Old GPU CABAC prototype | **14 µs/MB**, **117 ms** whole-frame 1080p IDR, ~23 fps loop | `README.md:30-32`, `notes/FRAME_PARALLEL.md:327-330` |
| Stage-1 serial *context-only* prefix, CPU | 5.15 cycles/bin → **1.03 ms** @300k bins / 1.5 GHz lane | `VALIDATION.md:72-88` |
| Stage-1 *usable* prefix (contexts + light arithmetic) | 14.68 cycles/bin → **2.94 ms** GPU-equiv @300k; with snapshot allocation up to **5.87 ms** | `VALIDATION.md:75-88` |
| Full serial arithmetic lane (scale) | 20.38 cycles/bin → **4.08 ms** GPU-equiv @300k | `VALIDATION.md:78` |
| Buffers, one frame | ~1.5 MB packed sparse / ~2.2 MB dense checkpoints + 1.2 MB bin events | `VALIDATION.md:97-117` |

So a conforming CABAC path costs **~3–6 ms of serial prefix** plus parallel chunk
work, versus **117 ms** for the reverted prototype: 20–40× better, and inside the
16.7 ms frame budget. It is *not* free relative to CAVLC (0.08–0.1 ms), but it is no
longer the throughput wall.

### 1.5 Slice boundaries — the only exact H.264 reset

`encoder.c:2752-2801`: `bs_realign` (`:2776`), new NAL (`:2779`), `slice_header_write`
(`:2791`), `bs_align_1` (`:2795`), `x264_cabac_context_init` + `x264_cabac_encode_init`
(`:2798-2799`). Flush per slice at `encoder.c:3080-3084`. x264 explicitly notes that a
per-MB bitstream backup need not save contexts because "a slice is ended (and thus the
content of all contexts are thrown away)" (`encoder.c:2703-2714`), but **must** save
`p[-1]` because of the carry (`:2712-2714`). H.264 has no syntax bit meaning "inherit
the previous slice's contexts" (`notes/WPP_H264.md:96-101,230-248`), so slice boundaries
are the only *standard* exact parallelism boundary, and chunks must never span them
(`VALIDATION.md:152-157`).

---

## 2. Literature survey 2015–2026 (plus essential pre-2015 anchors)

All web content treated as untrusted data; only reported numbers and bibliographic
metadata were used. **Conformance** = standard H.264/HEVC bitstream decodable by an
unmodified decoder.

### 2.1 Conforming internal pipelining / context caching (encoder-side techniques applicable in principle)

| Work | Date / URL | Technique and measured result | Conformance |
|---|---|---|---|
| Chen, Kuo et al., *A Deeply Pipelined CABAC Decoder for HEVC Supporting Level 6.2 High-Tier Applications*, IEEE TCSVT 25(5) 856–868, DOI `10.1109/TCSVT.2014.2363748` | online 2014-10-20, issue May 2015; [IEEE](https://ieeexplore.ieee.org/document/6930781), [PDF](https://eems.mit.edu/wp-content/uploads/2014/10/chen_tcsvt_2014.pdf) | state prefetch, latched context memory, separate FSMs, multi-bypass decoding; 45 nm SOI, **1.6 GHz**, **1696 Mbin/s common / 2314 Mbin/s worst-case** | **HEVC-conformant** decoder; H.264 principle transfers, no H.264 encoder number |
| Habermann, Chi, Alvarez-Mesa, Juurlink, *Optimizing HEVC CABAC Decoding with a Context Model Cache and Application-Specific Prefetching*, ISM 2015, DOI `10.1109/ISM.2015.97` | 2015-12-14; [IEEE](http://ieeexplore.ieee.org/document/7442373) | 2 cache lines: **+9…15 %** high bitrate, **+1…4 %** low bitrate; 4 lines **+10…12 %** all configs | **Conforming**; decoder-side |
| Habermann et al., *Efficient Wavefront Parallel Processing for HEVC CABAC Decoding*, PDP 2020, DOI `10.1109/PDP50117.2020.00058` | 2020-03-11; [IEEE](https://ieeexplore.ieee.org/document/9092425) | finer-grained dependency checks, same thread count; **up to 1.83×** CABAC decode, very low overhead | **HEVC-conformant**; H.264 has no WPP syntax (`notes/WPP_H264.md:123-142`) |
| Habermann, *High-throughput HEVC CABAC decoding*, PhD thesis TU Berlin, DOI `10.14279/depositonce-10748` | defense 2020-11-02, deposited 2020-12-09; [DepositOnce](https://depositonce.tu-berlin.de/items/34d76ebd-6693-46ec-8020-9f10b2d80d33) | summary table (FHD Class B): **FG-WPP 1.83× / 0 % HW / 0 % bitstream / conformant**; **Cache 1.17× / 7.5 % HW / 0 % bitstream / conformant**; **BBR 2.04× / 0 % bitstream / conformant**; B3P 8.50× / 61.9 % HW / <0.7 % bitstream / **non-conformant**; BBP 2.45× / <0.2 % bitstream / **non-conformant**. Context-cache 2×8 models → **+17 % clock**, <2 % miss, up to **+16.7 %** perf | mixed; see conformance column |
| Sony, *Multi-threaded CABAC decoding*, US 12,041,252 B2 (granted 2024-07-16) and continuation US 12,764,315 B2 (granted 2026-10-13); filed 2021-06-07 | [US12041252](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12041252), [US12764315](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12764315) | decode loop on thread 1 emits decoded bin string at an end state; inverse binarisation / syntax parsing moved to thread 2 (optionally GPU). Decouples the tight loop **without touching the bitstream**. **No measured speedup published** | **Conformant** decoder-side; concept (split bin parse from context updates) is relevant to our Stage 0/1 split |
| Cisco, *Entropy coding for video processing applications*, US 8,422,552 B2 (priority 2007-03-29, granted 2013-04-16) | [Google Patents](https://patents.google.com/patent/US8422552B2/en) | splits encode-decision / renormalisation / PutBit into FIFOs with a precomputed `S` to break the L/R feedback; claims H.264 compliant, **no quantitative speedup** | **H.264-conformant, bitstream-identical** (pre-2015 anchor; cited in `notes/WPP_H264.md:151`) |

**Finding for us:** conforming parallel CABAC is a *pipeline* problem, not a bitstream
problem. All these results are ≤2×, and none is measured on a GPU encoder. Our exact
chunk-summary design is a strictly stronger conforming intra-slice split (theoretically
~4k-way) because Stage 1 computes true contexts/arithmetic state instead of predicting
them — which is exactly why our 98.4 % conflict result is not contradicted by this
literature (`SPECULATIVE_HYBRID.md:108-137`).

### 2.2 Syntax-element partitioning (SEP) and bitstream partitioning — **not H.264-conformant**

| Work | Date / URL | Measured result | Conformance |
|---|---|---|---|
| Sze & Chandrakasan, *A high throughput CABAC algorithm using syntax element partitioning*, ICIP 2009 | 2009-11-13; [PDF](https://eems.mit.edu/wp-content/uploads/2023/08/sze_icip2009_paper.pdf) | 5 partitions (MBINFO/PRED/CBP/SIGMAP/COEFF), start-code prefix per partition; **~2.7×** throughput, **0.06–0.37 % BD-rate**, **1.5× area**; 3 AVC slices for same speed cost **0.87–1.71 % BD-rate**; combining with 4 entropy slices → **6×** | **Non-compliant H.264** (new partition bitstream) |
| Sze & Chandrakasan, *Joint Algorithm-Architecture Optimization of CABAC*, J. Signal Process. Syst. 69:239–252 | published 2012-05-26; [PDF](https://eems.mit.edu/wp-content/uploads/2014/10/sze_jsps_2012.pdf) | MP-CABAC + subinterval reordering: **2.7–32.8× bins/cycle**, **0.25–6.84 %** coding loss, critical path −14…22 %, ~10× throughput at **0.82–2.57 % BD-rate**; at ~10× speed, MP-CABAC cuts BD-rate penalty 1.2×/3.0×/4.1× vs ordered entropy slices/entropy slices/H.264 slices | **Explicitly non-standard compliant** (paper says so); aims at HEVC |
| Sze & Chandrakasan, ISSCC 2011 paper 7.2 / JSSC 47(1) Jan 2012 | 2011-02 / 2011-12; [slides](https://eems.mit.edu/wp-content/uploads/2016/06/ISSCC2011_V07_02.pdf), [JSSC PDF](https://eems.mit.edu/wp-content/uploads/2014/10/sze_jssc_2012.pdf) | 16 IES × 5 SEP = **80 arithmetic decoders**, **14–3026 Mbins/s**, 10.5 pJ/bin min energy | **Non-compliant H.264** (IES+SEP bitstream) |
| Habermann et al., *Syntax Element Partitioning for high-throughput HEVC CABAC Decoding*, ICASSP 2017, DOI `10.1109/ICASSP.2017.7952368` | 2017-03; [postprint](https://d-nb.info/1161006958/34) | 8 partitions (control/luma/chroma × cc/bc); **up to 5.4×** with negligible overhead; **16–47 B per slice**; >5 % overhead at very low bitrate | **Non-conformant** (modified bitstream, SPS/slice flag) |
| Habermann et al., *A Bin-Based Bitstream Partitioning Approach …*, IPDPS 2019, DOI `10.1109/IPDPS.2019.00112` | 2019-05; [IEEE](https://www.computer.org/csdl/proceedings-article/ipdps/2019/124600b053/1cYhOQVkqSk) | 8 partitions; **up to 8.5×** CABAC decode, **3.94 Gbins/s**, **9.2 % extra cell area** (thesis: 61.9 % HW), **<1 % bitstream overhead at high rates** (thesis: <0.7 % for FHD) | **Non-conformant**; same reconstructed video only with modified decoder |
| GE Video Compression, *Coding concept allowing parallel processing…*, US 10,887,625 B2 (2021-01-05) and US 2021/0044835 A1 (2021-02-11); priority 2012-01-20 | [US10887625](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10887625), [US20210044835](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20210044835) | split slices/WPP substreams into *tranches* with continuing CABAC adaptation; `no_cabac_reset_flag` in slice header; lowers transport latency, no coding-efficiency loss *within the modified syntax* | **Non-conformant to H.264** (HEVC-era syntax) |
| Huawei, *Parallel entropy coding*, US 2026/0149465 A1, published 2026-05-28 (PCT/RU2021/000173) | [US20260149465](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20260149465) | encode each channel into an individual substream with signalled lengths + trailing bits; parallel multi-core encode | **Non-conformant** (custom substream bitstream) |

**Why this matters:** SEP is conceptually *the same trick as Vk264's current CAVLC
component-based parallel path* (luma DC / luma AC / chroma DC / chroma AC split), and it
works — but only because the Vk264 path finally reassembles one ordinary bitstream.
SEP/CABAC versions cannot be reassembled losslessly: each partition has its own contexts
and arithmetic state, so a standard decoder desynchronises. Do not pursue for Vk264.

### 2.3 Bin-level pipelining, context-adaptive bypass, multi-bin engines

| Work | Date / URL | Measured result | Conformance |
|---|---|---|---|
| Low-power binary arithmetic encoder in H.264, Ben Hamida et al., IJACSA 8(7) 2017 | 2017-08-03; [PDF](https://thesai.org/Downloads/Volume8No7/Paper_56-Low_Power_Hardware_Design_of_Binary_Arithmetic_Encoder.pdf) | separate regular/bypass engines, clock gating; **268 MHz** on Virtex-5; cites 2–4 bins/cycle MB-level pipelines; 30–40 cycles/bin on DSPs | **Conforming H.264** hardware architecture |
| Mamidi et al., *Design and implementation of parallel bypass bin processing for CABAC encoder*, Advances in Electrical and Electronic Engineering 19(3) 243–257, DOI `10.15598/aeee.v19i3.4010` | 2021; [DSpace](https://dspace.vsb.cz/items/c24f6c8d-7291-48fd-a2a9-e38d391d6269) | **6-core bypass path, 6 bypass bins/cycle, 1.26 Gbin/s at 210 MHz**, 28 nm Artix-7 prototype / 65 nm ASIC; HEVC+VVC | **Conforming** bitstream (internal bypass grouping) |
| Chen, Sun, Zhang, Zhou, *A Highly Pipelined and Highly Parallel VLSI Architecture of CABAC Encoder for UHDTV Applications*, Sensors 23(9) 4293, DOI `10.3390/s23094293` | published 2023-04-26; [MDPI](https://www.mdpi.com/1424-8220/23/9/4293), [PDF](https://pdfs.semanticscholar.org/843f/d3d10507f64e92031e59241d460d66666c8b.pdf) | prediction-based context prefetch removes **up to 45.66 %** of regular-bin stall time; MCMU saves **29.25 %** model-update time (QP22); 4 regular + variable bypass bins; **2191 Mbin/s at 513 MHz**; validated against HM-16.7 on 18 sequences | **Conforming** HEVC hardware (test vectors pass) |
| Lin et al., *Hardware Acceleration Design of HEVC Entropy Encoding Syntax Elements Based on FPGA*, 华南理工学报 2023(8) | 2023-08-25; [article](https://www.sciopen.com/article/10.12141/j.issn.1000-565X.220350) | 90 nm, 20,900 gates @200 MHz, **CTU encoding time −38.2 % on average** | **Conforming** (hardware preprocessing) |
| Wang et al., *Parallel pipeline implementation of H.265/HEVC entropy decoding*, 电子技术应用 49(10) 59–64, DOI `10.16157/j.issn.0258-7998.223285` | 2023-10-23; [article](http://chinaaet.com/article/3000161922) | grouped syntax-element scheduling + pipeline, **1.84 bins/clock**; authors explicitly note *encoder needs no special configuration* | **Conforming** hardware |
| Sze & Budagavi, *Simplifying binary arithmetic coding engine*, US 10,349,070 B2 granted 2019-07-09 (continuations US 2019/0273933, US 2021/0120260) | [US10349070](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10349070), [US20210120260](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20210120260) | multiple *single-probability* bin encoders in parallel, 4-entry rLPS tables based on H.264 states | **Conformance ambiguous**: exact-state implementation can be conforming; quantised states change the bitstream |
| Sze & Budagavi, *Reducing context coded and bypass coded bins to improve CABAC throughput*, US 2022/0109841 A1 (2022-04-07) / US 2025/0168347 A1 (2025-05-22) | [US20220109841](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20220109841), [US20250168347](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20250168347) | new binarisation for `mb_qp_delta` and coefficient remainder; max codeword ≤32 bits to bound worst-case bypass bins | **Non-conformant to H.264/HEVC** (new binarisation) |
| Intel, *Method and system of high throughput arithmetic entropy coding*, US 10,554,977 B2 granted 2020-02-04 | [US10554977](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10554977) | non-framewidth raster / entropy blocks; parallel coding of multiple rows/blocks; contexts limited to the block | **Non-conformant** (changes context formation and scan order) |

**Practical takeaway:** the only item here directly reusable in a Vulkan shader is
x264's own `x264_cabac_encode_ue_bypass` (`common/cabac.c:136-157`), which already packs
up to 8 bypass bins per call — see §3.3.

### 2.4 GPU entropy implementations

| Work | Date / URL | Measured result | Conformance |
|---|---|---|---|
| Su et al., *Efficient Parallel Video Processing Techniques on GPU*, ScientificWorldJournal 2014:716020 (PMC 2016 index) | 2014-03-16; [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC3976889/) | component-based parallel **CAVLC** (Luma_DC/Luma_AC/Chroma_DC/Chroma_AC) + lag packing; whole encoder **~20×** vs serial, 1080p30, PSNR loss 0.14–0.77 dB; CAVLC speed is bandwidth-bound | **Conforming** if packing restores decode order; confirms our CAVLC design direction |
| Ren et al., *Software parallel CAVLC encoder based on stream processing*, ESTIMedia 2009 | [hgpu summary](https://hgpu.org/?p=2696) | **3.03× (STORM) / 2.08× (GPU)** vs serial CAVLC | Conforming |
| CAVLCU, *an efficient GPU-based implementation of CAVLC*, J. Supercomputing | 2021-11-29; [Springer](https://link.springer.com/article/10.1007/s11227-021-04183-8) | **2.5–5.4×** faster than the only prior GPU CAVLC implementation (Su et al.) | **Conforming H.264** |
| de Souza et al., *GHEVC: An Efficient HEVC Decoder for GPUs*, IEEE TMM 19(3) 459–474, DOI `10.1109/TMM.2016.2625261` | online 2016-11-03, issue Mar 2017; [research portal](https://researchportal.ulisboa.pt/en/publications/ghevc-an-efficient-hevc-decoder-for-graphics-processing-units) | whole GPU pipeline **except entropy kept on CPU** because irregular; **145 fps 4K / 605 fps 1080p** (Titan X, RA); heterogeneous CPU+GPU variant with CPU entropy: **150 fps 4K**, 1.8–2.2× vs CPU-only | **HEVC-conformant**; negative evidence for moving entropy onto GPU as a tight loop, positive evidence for our *chunked, regular* GPU design |
| Skulkarni, *Implementation of CABAC on KiloCore Processor Arrays*, UC Davis thesis | 2021-03-16; [thesis](http://vcl.ece.ucdavis.edu/pubs/theses/2021-2.skulkarni/Sharmila_Thesis_Final.pdf) | 64 cores, 3.52 mm², **37 Mbin/s**, 943 mW — 1–2 orders below ASICs; task-parallel stages | Conforming H.264 Main profile; throughput too low to copy |

### 2.5 Exact / non-integer bit estimation for RD (this is where the value is)

| Work | Date / URL | Measured result | Conformance |
|---|---|---|---|
| Kei Im Sio, Ghandi, Hou Chan Ka, *Accurate, Non-integer Bit Estimation for H.265/HEVC and H.264/AVC Rate-Distortion Optimization*, CSIC 2015, DOI `10.2991/csic-15.2015.72` | 2015-07; [Atlantis](https://www.atlantis-press.com/proceedings/csic-15/24970) | **0.2–13.0 % bitrate saving HEVC, 0.2–8.0 % H.264/AVC** from fractional bit estimates in RDO | **Conforming** (decision-only change) |
| Kei Im Sio & Ghandi, *Improved rate-distortion optimized video coding using non-integer bit estimation and multiple Lambda search*, Front. Comput. Sci. 10(1) 157–166, DOI `10.1007/s11704-015-5066-1` | 2016-01-06; [journal](https://journal.hep.com.cn/fcs/EN/10.1007/s11704-015-5066-1) | fractional bits + per-stage lambda search → **up to 7.0 % bitrate saving H.264/AVC** | **Conforming** |
| Li & Ren, *Fast CABAC Rate Estimation for HEVC Mode Decision*, BigMM 2015, DOI `10.1109/BigMM.2015.14` | 2015-04; [IEEE](https://www.computer.org/csdl/proceedings-article/bigmm/2015/8688a171/12OmNxVDuU7) | entropy-based rate predictor skips actual CABAC in RD: **−62 % CABAC time in mode decision**, negligible RD loss | **Conforming** (estimator) |
| *Fast entropy-based CABAC rate estimation for mode decision in HEVC*, SpringerPlus 2016 | 2016-06-17; [article](https://link.springer.com/article/10.1186/s40064-016-2377-0), [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC4912508/) | compares ΔR, BD-rate and estimator speed vs Won et al. 2012; same decision-only direction | **Conforming** |

Note x264 itself already implements this: `common/cabac.h:99-124` accumulates
`x264_cabac_entropy[i_state^b]` into `f8_bits_encoded` without writing the bitstream,
and `encoder/cabac.c:282-306` switches between real encoding (`RDO_SKIP_BS=0`) and
cost-only mode. Our "novel" idea is to give Vk264 the equivalent, not to invent it.

### 2.6 Non-standard but decodable variants (rANS / neural / VVC multi-estimator)

| Work | Date / URL | Measured result | Conformance |
|---|---|---|---|
| Lin, Arunruangsirilert, Sun, Katto, *Recoil: Parallel rANS Decoding with Decoder-Adaptive Scalability*, ICPP 2023, arXiv:2306.12141 | 2023-06; [arXiv](https://arxiv.org/abs/2306.12141) | store intermediate renormalised rANS states as metadata → arbitrary entry points; throughput comparable to conventional partitioning | **Different codec**; same checkpoint idea as our Stage 1, not H.264 |
| Said, Le, Farhadzadeh, *Bitstream Organization for Parallel Entropy Coding on Neural Network-based Video Codecs*, arXiv:2312.00921 | 2023-12-01; [arXiv](https://arxiv.org/html/2312.00921v1) | entry-point index + bidirectional streams + joint termination; overhead **<1 % when streams >95 B**, **<0.1 % when >1200 B**; termination 4.56→1.78 bits/stream | **Neural codecs only**, not H.264 |
| *Using Four Hypothesis Probability Estimators for CABAC in VVC*, ACM TOMM | 2023-01-23; [ACM](https://dl.acm.org/doi/full/10.1145/3531015) | parallel estimators with per-context adaptation rates `r0∈{2,3,4,5}, r1∈{6,7,8,9}`; VTM-13.0 experiments | **VVC-conformant**, not H.264 |
| Im et al., *Dynamic estimator selection for double-bit-range estimation in VVC CABAC entropy coding*, IET Image Process. | 2024-02; [Wiley](https://ietresearch.onlinelibrary.wiley.com/doi/full/10.1049/ipr2.12980) | parallel probability subdomains/estimators; reports VVC gains | **VVC-conformant**, not H.264 |
| Xu et al., *Low-Latency H.264/AVC Encoder Hardware Design with Wavefront Parallelism on FPGA*, ICMEW 2025, DOI `10.1109/ICMEW68306.2025.11152256` | 2025-06-30; [Semantic Scholar](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/icmew68306.2025.11152256) | dual-core MB-line wavefront; **125 fps 1080p**, **up to 49.4 % latency reduction**, no RD loss | **H.264-conformant**, but **Baseline/CAVLC** — no CABAC context carry (see `notes/WPP_H264.md:150`) |

### 2.7 What the literature does *not* offer

* No published **H.264-conformant CABAC *encoder*** with a measured >2× intra-slice
  speedup. The 5–8.5× results all require a modified bitstream or are decoders/ASICs.
* No published exact speculative context checkpoint/rollback scheme for conforming
  H.264 CABAC. Sze explicitly notes bin-level speculation gives only "low hundreds of
  Mbins/s" and raises power and critical path; that matches our **98.4 % conflict**
  finding (`SPECULATIVE_HYBRID.md:164-173,108-137`).
* GPU implementations uniformly keep entropy off the GPU (`GHEVC`, §2.4) or restrict it
  to CAVLC. Our chunk-summary design is *not* contradicted by this: it replaces the
  tight loop with a regular prefix + many independent 64-bin warps.

---

## 3. Novel ideas beyond what we tried (and their quantification)

### 3.1 Idea A — exact bit-cost feedback to RD/trellis, using the entropy LUTs we already have

**Observation.** Vk264's mode decisions use hand-written bit proxies:

* `Vk264/shaders/flat_intra.comp:61`: `levelCost(level) = 0 : 5 + 2*findMSB(|level|)`;
  `:73` `bitsCost = 8`; `:85,:100,:128` accumulate it; `:140-141` charges
  `bitPrice = ...` against distortion.
* `Vk264/shaders/residual.comp:255-261`: chroma-DC `bits += 3 + 2*findMSB(|v|)`;
  `:180-181` mode cost uses fixed 1/5/3 bits; `:315-331` λ-weighted costs.

x264's actual CAVLC lengths are table lookups: `coeff_token[nC][total-1][trailing]`
(`encoder/cavlc.c:148`), `level_token[suffix][val]` (`:162,172`), `total_zeros`
(`:184,190`) and `run_before` (`:192-193`); Exp-Golomb length is
`2*floor(log2(val+1))+1` (`common/bitstream.h:221-248`). CABAC has exact fractional
costs at `common/cabac.h:101-124`.

**Idea.** Upload the four small CAVLC VLC-length tables as read-only SSBOs and add one
shader helper returning the *exact* number of bits for a candidate block, then use it in
the existing RD comparisons. No bitstream change, no new entropy stage, no extra pass:
the coefficients are already scanned in the same shaders. For CABAC, accumulate
`x264_cabac_entropy[state^b]` in Stage 1's light arithmetic pass
(`VALIDATION.md:75-88` already does the arithmetic) and expose per-MB prefix sums to
RD on a second pass.

**Quantified benefit.** Literature: 0.2–8.0 % H.264 bitrate saving from non-integer bit
estimation (`CSIC 2015`), up to 7 % with λ retuning (`Front. Comput. Sci. 2016`). Lab:
trellis/RDO ablation is worth **+0.65 VMAF** (`README.md:17`), mode RD +0.86
(`CABAC_VS_CAVLC_SWEEP.md:58-59`). Capturing 20–40 % of that gap ⇒ **+0.13…+0.26 VMAF**
at matched rate (≈0.6–2.4 % bits). Upper bound if the estimator plus λ search fully
closes the gap: +0.65 VMAF, but that is not supported by our measurements.
**Cost:** a few LUT loads per coded block; estimate **<0.05 ms/frame** (no measurable
change to the 0.08–0.1 ms CAVLC stage). **Risk: low.**

**Honesty note:** this is not new algorithmically (x264 has done it for years), but it
is new for Vk264 and it is *outside* the tried entropy workstream. Do not confuse it
with "prefix sums for RD": the prefix sums only help if a second pass is acceptable;
for CAVLC the exact lengths are available immediately, so that is the first target.

### 3.2 Idea B — "emit more syntax cheaply": quantify the real ceiling

The entropy cost of extra syntax is linear and small, but it is not free:

| Scenario | Bins/frame | Marginal Stage-1 prefix cost (14.68 cyc/bin @1.5 GHz) |
|---|---:|---:|
| Baseline | 300k | 2.94 ms |
| +33 % bins (more partitions/ref flags) | 400k | 3.92 ms (+0.98) |
| 2× bins | 600k | 5.87 ms (+2.94) |

For **CAVLC** the same scaling is ~0.08→0.16 ms/frame: essentially free. So the
bottleneck is *not* bin capacity. The extra syntax that could matter and its true cost:

* **P sub-partitions (16×8, 8×16, 8×8):** Vk264 sets `config.partitions = 0`
  (`Vk264/src/presets.hpp:73`), so only 16×16 P MBs exist. Adding two 16×8/8×16
  candidates doubles motion/SATD work per P MB — that dominates, not the ~2–4 extra
  bins/MB. Parent's combined restricted-tools ablation is 38.89→37.31 dB (1.58 dB)
  with `partitions=none` among five other changes
  (`Vk264/docs/quality-parity.md:43-51`); no isolated partition number exists, so an
  honest isolated estimate is **~0.1–0.3 dB (~0.05–0.2 VMAF)**, unmeasured.
* **More reference indices:** already 5 refs in `slow` (`presets.hpp:14`, `:71`);
  older refs chosen for only 1.9–2.8 % of P MBs (`quality-parity.md:69`). No entropy
  issue; the blocker is the dual-reference ABI for B frames
  (`README.md:16`; Vk264 has no B-frame path at all).
* **8×8 transform / I4x4:** High-profile tools; parent measures 4×4-only vs full as
  **38.51→38.89 dB (+0.38 dB)** at 6 Mb/s (`quality-parity.md:48`). Again transform
  and neighbour-reconstruction work, not bins.

**Conclusion:** spending entropy capacity on more syntax is a *bad trade* unless the
decision compute is already paid for. It cannot be justified by "bins are free" because
the extra RD/MC work costs far more than the ~1 ms of prefix time.

### 3.3 Idea C — bypass-bin grouping and free bit accounting in Stage 1

H.264 CABAC bypass bins ~**18 %** of the stream (`SPECULATIVE_HYBRID.md:99-102`). x264
already groups up to 8 bits per `x264_cabac_encode_ue_bypass` call
(`common/cabac.c:136-157`). Using that for sign/level runs removes up to 7/8 of those
bins from Stage 1: ≈**15.8 % fewer bins** ⇒ **−0.46…−0.93 ms** from the 2.9–5.9 ms
prefix. While walking those bins, accumulate `x264_cabac_entropy[state^b]`
(`common/cabac.h:101-124`) to get exact per-MB bit costs at no extra pass. Conformant,
low risk, modest reward — a micro-optimisation, not a breakthrough.

### 3.4 Idea D — parallel entropy buys quality by *not* splitting the slice

Row slices cost **−0.439 VMAF** at 6000 kb/s and **+5.6…+7.5 % bits at matched QP**
(`notes/WPP_H264.md:190,217-224`); 1→68 slices cost **−0.348 VMAF at +4.0 % bits**
(`notes/SLICE_SCALING.md:181-190`). A chunk-summary CABAC design keeps **one** slice per
frame and still gets ~4k parallel lanes, so it *avoids* that entire loss while adding
CABAC's own +0.35…+1.7 VMAF. Combined credible range: **+0.5…+2.1 VMAF** if the
3–6 ms prefix estimate holds and fps stays ≥60. This is the only way entropy work
buys quality beyond the ~3 % rate saving.

### 3.5 Idea E — exact VBV/MB-tree feedback from prefix sums

Stage 1's cumulative bit position gives exact bits at every MB boundary (x264's
`x264_cabac_pos`, `common/cabac.h:96`, and row-VBV backups at `encoder.c:2822`). This
could give exact per-row bit accounting for Vk264's ABR, whose known failure is
21 Mb/s output for a 6 Mb/s target on duplicated capture (`notes/SLICE_SCALING.md:113-120`).
But the frame-level RC lag is a bigger issue than bit-count accuracy
(`notes/FRAME_PARALLEL.md:154-178,251-271`), so this is a **minor** enabler, not a fix.

---

## 4. Ranked top 3 with concrete Vk264 sketches and cost estimates

### Rank 1 — Exact bit-cost model for CAVLC RD/trellis (no CABAC required)

* **Sketch.** (1) Export x264 VLC-length tables (`encoder/cavlc.c:36-63,121-195` and
  Exp-Golomb lengths from `common/bitstream.h:221-248`) into a read-only SSBO at init.
  (2) Add `int vlcBits(total, trailing, suffix, totalZeros, runBefore)` to
  `shaders/bitstream.glsl`, mirroring `cavlc.c:148-193`. (3) Replace
  `flat_intra.comp:61,73,85,100,128` and `residual.comp:255-261` heuristics with exact
  lengths; keep the existing λ-weighted comparison (`:140-141`, `:315-331`).
  (4) A/B at 2000/6000/8000 kb/s on the 9 sweep masters; gate on VMAF and bitrate
  error, not bitstream SHA (decisions change by design; entropy output stays legal).
* **Cost.** ~1–2 shader edits + one LUT upload; **<0.05 ms/frame** estimated; no new
  buffer beyond a few KB. 1–2 days of work. **Expected +0.13…+0.26 VMAF** (0.6–2.4 %
  bits), upper bound +0.65 (lab trellis ablation). **H.264-conformant.**
* **Why #1:** largest expected VMAF per unit engineering, works with the shipping
  CAVLC path, and does not depend on solving CABAC.

### Rank 2 — Single-slice exact chunk-summary CABAC (already validated on CPU)

* **Sketch.** Exactly `SPECULATIVE_HYBRID.md:182-215` with the `VALIDATION.md:168-175`
  correction: Stage 1 must compute **contexts + `low/range/queue/outstanding`** (not
  contexts alone); one carry variant suffices (`VALIDATION.md:43-47`); EPB **after**
  merge (`:146-151`); slice markers and per-frame dither bit at flush (`:152-157`).
  Data layout from `VALIDATION.md:97-132`; byte-exactness gates from `VALIDATION.md:205-216`.
* **Cost.** Prefix **2.9–5.9 ms/frame** GPU-equivalent projection (measured CPU
  14.68–29.33 cyc/bin at 300k bins), plus parallel chunk/merge/EPB estimated <1–2 ms;
  **1.5–2.3 MB/frame** scratch. Versus 117 ms IDR prototype: >20×. **Risk medium-high**
  (Stage 1 gather correctness), mitigable by the existing golden vectors.
* **Benefit.** CABAC's own +0.35…+1.7 VMAF **plus** avoiding the 5.6–7.5 % bitrate /
  0.35–0.44 VMAF row-slice penalty by keeping one whole-frame slice ⇒ **+0.5…+2.1 VMAF**
  if ≥60 fps is held. **H.264-conformant, bitstream-identical to serial CABAC.**

### Rank 3 — Bypass grouping + Stage-1 bit accounting as a cheap accelerator for Rank 2

* **Sketch.** In Stage 0, route sign and coefficient-remainder bins through
  `x264_cabac_encode_ue_bypass` semantics (`common/cabac.c:136-157`) so one event
  represents up to 8 bypass bins; Stage 1 accumulates
  `x264_cabac_entropy[state^b]` (`common/cabac.h:101-124`) per chunk and writes a
  cumulative bit-cost prefix alongside the arithmetic checkpoint; expose per-MB costs
  to the RD stage for the CABAC variant of Rank 1.
* **Cost.** ~15.8 % bin reduction (18 % bypass × 7/8) ⇒ **−0.46…−0.93 ms** from the
  prefix; one extra 4-byte counter per chunk (~19 KB/frame at 4,688 chunks). **Low risk**;
  byte-exactness gate already exists. **H.264-conformant.**
* **Benefit.** Makes Rank 2 cheaper and gives exact CABAC RD costs; by itself **<0.3 VMAF**.

**Not ranked (recorded as closed):** speculative checkpoint/rollback (98.4 % conflict,
`SPECULATIVE_HYBRID.md:108-137`); CPU–GPU offload (0.7 ms/frame CPU + sync, `:232-252`);
WPP row-context carry (non-conformant, `notes/WPP_H264.md:230-248`); SEP/B3P/tranches
(non-conformant, §2.2); frame-parallel N=16 (needs ~3.8 GiB scratch and N-frame RC lag,
`notes/FRAME_PARALLEL.md:386-396,251-271`) — long-term architecture, not next step.

---

## 5. Honest assessment: is entropy a solved/dead area for us?

**For quality: yes, largely.** Our own data says CABAC is worth ~3 % bitrate and
~0.35 VMAF at 8 Mb/s, ~0 at easy content. No parallel entropy variant can exceed the
entropy limit of a single conforming stream without changing the bitstream. The
2015–2026 literature confirms this: every large speedup trades coding efficiency or
standard conformance, and the conforming ones are ≤2× internal pipelines that do not
apply directly to a Vulkan encoder.

**For throughput: no — there is one unclaimed win.** The exact chunk-summary design is
byte-exact and turns a 117 ms serial lane into a ~3–6 ms serial prefix plus parallel
work. That is an *engineering* task with a clear gate, not a research breakthrough.

**For "other value": the useful reframe is bit-cost accuracy, not bin throughput.**
The biggest entropy-adjacent VMAF still on the table is exact syntax cost in RD/trellis
(Rank 1), which works without CABAC and costs almost nothing. If the goal is to beat
x264, spend effort there and use parallel CABAC (Rank 2) mainly as a way to keep
single-slice quality while raising throughput.

**Decision rule from the lab still applies** (`docs/EXPERIMENT_PLAN.md:42-45`): a path
is promising only if H.264-conformant and ≥100 fps loop, or explicitly accepted as an
opt-in quality preset. Rank 1 passes regardless of fps; Rank 2 must be measured against
the 16.7 ms budget and the 23 fps baseline before being promoted.

---

## References (all accessed 2026-11-27)

Lab notes and measurements:
- `README.md:9-32`; `notes/CABAC_VS_CAVLC_SWEEP.md:22-29,47-61`; `notes/CPU_CABAC_COST.md:15-37`;
  `notes/SPECULATIVE_HYBRID.md:36-173,175-287`; `notes/FRAME_PARALLEL.md:321-396`;
  `notes/WPP_H264.md:146-161,188-224`; `notes/SLICE_SCALING.md:44-56,181-225`;
  `notes/LEARNED_DECISIONS_STATUS.md`; `docs/EXPERIMENT_PLAN.md:42-45`;
  `experiments/chunk_cabac/notes/VALIDATION.md:23-63,66-93,97-132,136-216`;
  `results/stream_stats.json`.

x264 fork source (read only): `E:\__PROJECTS__\Vk264\x264-vulkan\{common\cabac.c,common\cabac.h,
common\bitstream.c,common\bitstream.h,encoder\cabac.c,encoder\cavlc.c,encoder\encoder.c}`.
Vk264 source (read only): `E:\__PROJECTS__\Vk264\{src\presets.hpp,shaders\flat_intra.comp,
shaders\residual.comp,shaders\mb_entropy.comp,docs\quality-parity.md}`.

Web sources are linked inline in §2 with dates; primary ones:
[CSIC 2015](https://www.atlantis-press.com/proceedings/csic-15/24970),
[Front. Comput. Sci. 2016](https://journal.hep.com.cn/fcs/EN/10.1007/s11704-015-5066-1),
[BigMM 2015](https://www.computer.org/csdl/proceedings-article/bigmm/2015/8688a171/12OmNxVDuU7),
[ISM 2015](http://ieeexplore.ieee.org/document/7442373),
[ICASSP 2017 SEP](https://d-nb.info/1161006958/34),
[IPDPS 2019 B3P](https://www.computer.org/csdl/proceedings-article/ipdps/2019/124600b053/1cYhOQVkqSk),
[PDP 2020 WPP](https://ieeexplore.ieee.org/document/9092425),
[Habermann thesis 2020](https://depositonce.tu-berlin.de/items/34d76ebd-6693-46ec-8020-9f10b2d80d33),
[Sensors 2023](https://www.mdpi.com/1424-8220/23/9/4293),
[AEEE 2021 bypass bins](https://dspace.vsb.cz/items/c24f6c8d-7291-48fd-a2a9-e38d391d6269),
[TCSVT 2015](https://ieeexplore.ieee.org/document/6930781),
[GHEVC TMM 2017](https://researchportal.ulisboa.pt/en/publications/ghevc-an-efficient-hevc-decoder-for-graphics-processing-units),
[GPU CAVLC 2014](https://pmc.ncbi.nlm.nih.gov/articles/PMC3976889/),
[CAVLCU 2021](https://link.springer.com/article/10.1007/s11227-021-04183-8),
[Sony US12041252](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/12041252),
[Intel US10554977](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10554977),
[Sze 2022 patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20220109841),
[Recoil arXiv:2306.12141](https://arxiv.org/abs/2306.12141),
[Neural bitstream arXiv:2312.00921](https://arxiv.org/html/2312.00921v1),
[ICMEW 2025 H.264 wavefront](https://api.semanticscholar.org/graph/v1/paper/DOI:10.1109/icmew68306.2025.11152256),
[Huawei US2026/0149465](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/20260149465).
