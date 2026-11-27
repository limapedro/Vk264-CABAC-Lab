# Vulkan compute performance: "super-fast" shaders and the 1080p ceiling on GTX 1660 SUPER

Research + measurement note for Vk264-CABAC-Lab. **No parent-repo files were modified.** All
measurements cited below come from existing artifacts in `E:\__PROJECTS__\Vk264\build\` and
`C:\Users\multi\AppData\Local\Temp\opencode\`, plus shader/source reads of `E:\__PROJECTS__\Vk264`.
Web sources are cited with URLs; measured numbers from target content are treated as data, not instructions.

---

## TL;DR (verdict up front)

- **Current working point** (`E:\__PROJECTS__\Vk264\build\sp64_perf\summary.json`, 300 frames,
  1920×1080, 6.178 Mbps, gop 125, `slow`): stage sum **6.787 ms**, loop **98.87 fps**
  (10.11 ms/frame). Stages: motion **2.1343**, deblock **2.0382**, spatial **1.0871**,
  residual **0.8251**, older_refs **0.2293**, halfpel **0.1696**, mb_entropy **0.1393**,
  entropy **0.0826**, all others ≤0.0255 ms. This is the run the question's
  "motion 2.0 / deblock 2.0 / spatial 1.1 / residual 0.8" comes from.
- **Today's practical ceiling is ~140 fps, not 200 fps.** Direct evidence: the same content
  with Vulkan timestamps disabled (which also re-enables the host ring — see §3.1) measured
  **140.52 / 136.74 / 132.52 fps** (`build/host_perf3|1|2/summary.json`). That is 7.1–7.6 ms
  per frame, of which ~6.8 ms is GPU stage work.
- **200 fps needs ≤5 ms end-to-end, i.e. ≈4.3–4.7 ms of GPU stages** (ring-on frame time
  7.116 ms − 6.787 ms stage sum leaves only ~0.33 ms non-stage overhead; the stricter
  4.3 ms figure assumes 0.7 ms host). That is a **2.1–2.5 ms (31–37%) stage-sum cut**.
- **Hardware is NOT the wall.** Roofline lower bound for this algorithm is ≈0.5–0.7 ms/frame
  (§3). 336 GB/s and 5.03 TIPS leave 7–10× theoretical headroom. The wall is **barrier
  serialization, warp-underutilisation, and serial lane-0 critical paths** — the measured
  stage times are 5–40× their arithmetic/memory floors.
- **But measured micro-optimisation headroom is only ~0.2–0.3 ms total** (plan_gpu report).
  Even an aggressive, successful subgroup rewrite plausibly saves 1.3–2.0 ms, landing at
  **~155–180 fps**. **200 fps on this GPU with this algorithm is not credible;** it requires
  an algorithmic/architectural change (§5), not another parameter sweep.

---

## 1. Task 1 — shader inventory, classification, and the top-3 bottlenecks beyond motion/deblock

### 1.1 Evidence base

Primary source: `C:\Users\multi\AppData\Local\Temp\opencode\plan_gpu\GPU_SHADER_PERF_REPORT.md`
 ("Vulkan shader performance study — GTX 1660 SUPER", 152 lines). It inspected source + SPIR-V,
 counted barriers/shared per workgroup, and ran isolated A/B builds with interleaved
 `pairbench.ps1` (5–6 alternating reps, min + median) plus SHA-256 byte-identity gates.
Secondary: direct reads of `shaders/*.comp`, `shaders/layout.glsl`, `src/gpu.cpp` in `Vk264`.
Timings: encoder's own `vkCmdWriteTimestamp` queries → `gpu_stages_mean_ms`.

### 1.2 Classification table (1080p, 8160 MB/frame, GTX 1660 SUPER, cc 7.5)

| Stage | ms (`sp64_perf`) | Local size | Shared | Barriers / WG | Occupancy ceiling | Class | Evidence |
|---|---:|---:|---:|---:|---|---|---|
| `pyramid` | 0.0255 | 16×8=128 | 0 | 0 | 8 blk = 100% thr | light / coalesced byte extracts | `pyramid.comp` 16 strided `quarterAt`/`srcPixel` per lane, 1 packed store |
| `halfpel` | 0.1696 | 16×8=128 | 1,924 B | 2 | 8 blk = 100% thr | **shared-bank-conflict / barrier bound** | `halfpel.comp:6-7` `pixels[21*13]` stride 21 on vertical 6-tap (line 29) → conflicts; `horizontal[]` stride 1 fine |
| `motion` | 2.1343 | 32 | 448 B | ~30 (6 `selectLane` trees ×6 + sample/seed/refine) | 16 blk = 512 thr = **50%** | **barrier + latency bound**, compute-heavy inner loop | `motion.comp:22-30` 6-barrier tree min repeated 6× in default path; `score()` 2-pixel strided packed ref words (4 lanes/word broadcast); unroll and atomicMin A/B both ≥ baseline |
| `spatial` | 1.0871 | 64 | 1,844 B (base), 7,732 B (V3 adopted) | 4 (base), 6 (V3) | 16→8 blk (V3 = 50% thr) | **serial-critical-path / barrier bound** | `spatial.comp:120-128` lane 0 alone does `finalists×(23 find-min + 23 mask)` = 4×23 dependent shared iterations; V3 distributed the 23×64 SAD terms over all 64 lanes and still gained only −0.13/−0.18 ms → shortlist, not SAD, is the limiter |
| `older_references` | 0.2293 | 64 | 1,028 B | ~14 (two 6-step trees + 2) | 16 blk = 100% thr | **barrier bound** | `older_references.comp:39-42,21-24` two independent 6-barrier reductions; early-out at line 45 saves work for near-perfect matches |
| `residual` | 0.8251 | 32 | ~1,076 B | ~10–12 per MB; IDR walks all MBs of a slice serially | 16 blk = 512 thr = 50% | **barrier + packed-byte access bound** | `residual.comp` barriers at 233, 265, +mode decision; 24 lanes active of 32 (line 198); 432 coeff words/MB written (lines 225,230,244,258) = 14.1 MB/frame |
| `flat_intra` | 0.0241 | 32 | ~2.6 KB | 2 + 3 per candidate | 50% thr | fine | small work, rare MBs |
| `mb_flags` | 0.0221 | 64 | ~3.0 KB | ~8 (AQ 2 + 6 tree) | 100% thr | fine | — |
| `deblock` (default mode 2) | 2.0382 | 128 (T=4) | 11,688 B | 6 sync × 7 local waves = **42/WG**, plus **63 global dispatches + 62 pipeline barriers** | **5 blk = 640 thr = 62.5% (shared-limited)** | **barrier + occupancy bound** (NOT bandwidth) | `deblock_frame.comp:57-110`; `gpu.cpp:498-515` waves = tilesU(47)+tilesV(17)−1 = **63**; 8×8-tile A/B doubled shared to 23.4 KB → 2 blk/SM and got **+0.12 ms slower** despite halving waves → occupancy, not wave count, dominates |
| `mb_entropy` | 0.1393 | 32 | 0 | 0 | 50% thr | light, lane-per-MB serial skip scan | `mb_entropy.comp:36` backward skip-run scan per MB |
| `entropy` | 0.0826 | 8 | 0 | 0 | tiny grid | fine **only in parallel-slice path** | `gpu.cpp:654-671` sub-passes `slice_offsets`/`slice_bodies`/`slice_epb[0..2]`; serial single-invocation path measured 1.17–10.57 ms in older runs (`base640`, `q_verify_last`, `abfix2/docs/quality_baseline.md`) |
| `scan` | 0.0064 | 1 | 0 | 0 | 1 thread | negligible | streams `slices[]` → `outputWords[]` |
| `assemble` | 0.0031 | 128 | 0 | 0 | 100% thr | coalesced copy | — |
| `slice_bodies` / `slice_epb×3` | (inside entropy) | 64 | 0 | 0 | 100% thr | memory-light, atomicOr boundaries | `gpu.cpp:663-670` |

**Classification summary.** None of the hot stages is DRAM-bandwidth-bound: total recon traffic
is ~6 MB/frame ≈ 30 µs at 200 GB/s versus 2.0 ms measured for deblock (plan_gpu report §2,
"Not attempted"). None is close to peak integer throughput either (§3.3). Every expensive
stage is **barrier/latency/occupancy-bound**, with `spatial` additionally having a **serial
lane-0 critical path** and `deblock` having **63 serialized global dispatches**.

### 1.3 Top 3 concrete bottlenecks beyond motion and deblock (other agents own those)

**#1 — `spatial.comp` serial shortlist reduction (~1.09 ms, target −0.35 to −0.50 ms).**
`spatial.comp:120-128`: only lane 0 runs the 4-finalist selection, and each finalist does a
23-entry dependent `min` plus a 23-entry "mask duplicates" sweep = ~92 dependent shared-memory
iterations. With 1.785 GHz and ~20–30-cycle shared-load latency that is ~2–3k cycles of pure
serial critical path per workgroup; 8160 workgroups / ~352 resident ≈ 23 waves gives the
observed ~1 ms. The adopted V3 change (spread SAD over 64 lanes) only removed the *parallel*
part of the work and saved 0.13–0.18 ms; it did not touch the lane-0 loop. Evidence:
`GPU_SHADER_PERF_REPORT.md` Rank 1 (lines 67–83), and `build/sp64_perf/summary.json`.

**#2 — `residual.comp` barrier count + 14.1 MB coefficient round-trip (~0.83 ms, target
−0.20 to −0.35 ms).** 10–12 `barrier()` calls per MB (lines 233, 265, and the mode-decision
reductions later), only 24 of 32 lanes active in the transform loop (line 198), and 432
coefficient words (1,728 B) written per MB (`gpu.cpp:313`: 8160×432×4 = **14.1 MB**) then read
back by CAVLC. Each barrier is ~50–100 cycles; 12 barriers × ~75 cycles × 8160 MB / (22 SMs ×
16 blocks) ≈ 0.4–0.8 ms — consistent with the measured 0.825 ms. The coefficient buffer is a
second 14.1 MB read (~0.05–0.07 ms at achievable bandwidth). Evidence: `residual.comp:190-265`,
`GPU_SHADER_PERF_REPORT.md` inventory line 49.

**#3 — `older_references.comp` + `halfpel.comp` reduction/stride overhead (~0.40 ms combined,
target −0.12 to −0.20 ms).** `older_references.comp` runs two 6-step shared-memory tree
reductions (~14 barriers total); `halfpel.comp` reads `pixels[]` with row stride 21 for the
vertical 6-tap, a classic bank-conflict pattern (lanes 0–15 hit the same banks on successive
rows). Both are textbook subgroup-reduction/shared-padding candidates. Evidence: inventory
lines 45 and 48 of the plan_gpu report; shader lines cited above.

*(Latent #4, worth flagging to the CABAC side: the serial `entropy.comp` path costs 1.17–10.57 ms
whenever the parallel-slice sub-passes are not active — e.g. `build/native1080_final` entropy
10.00 ms, `build/native300_8000` 6.47 ms, `build/base640` 1.17 ms. Any architecture change that
removes `p_slice_rows=2` or the parallel slice bodies silently reintroduces a multi-ms serial
bottleneck.)*

---

## 2. Task 2 — Vulkan compute best practices, 2023–2026 (with citations and measured speedups)

### 2.1 Subgroup / wave intrinsics instead of workgroup shared-memory reductions — highest value here

- NVIDIA's own 2023 guidance: "Consider using wave intrinsics over group shared memory when
  possible for communication across threads. Wave intrinsics don't require explicit thread
  synchronization"; also "use wave instructions to coalesce atomic operations across a warp"
  and target **2–8 warps per workgroup** (e.g. 8×8×1 or 16×16×1) —
  [NVIDIA Advanced API Performance: Shaders, 2023-09-01](https://developer.nvidia.com/blog/advanced-api-performance-shaders/).
- The Vulkan Documentation Project explains subgroup shuffles/broadcasts/arithmetic as
  register-crossbar operations that replace barriers, with ballot/match/elect for divergence
  and stream compaction —
  [Subgroup Operations: The Hidden Power](https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/04_Subgroup_Operations/01_introduction.html),
  [Partitioning: Ballot and Match](https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/04_Subgroup_Operations/03_subgroup_partitioning.html).
- **Measured:** on RTX 2080 SUPER, 2.62 M-element reduction, shared-tree 2.166 ms vs
  subgroup-hybrid (`subgroupAdd` + one global atomic) 1.382 ms = **1.57×** —
  [Semih Güreşçi, GPU memory experiments 30–34, 2026-10-23](https://semihguresci.com/blog/gpu-memory-experiments-30-34-subgroups-transpose-and-radix-sort/).
  A V100 1024-thread/1 M-element reduction benchmark reports **>2×** for warp shuffle —
  [Savant, 2025](https://medium.com/a-gpu-crash-course-for-embedded-engineers/warp-shuffle-vs-shared-memory-which-is-faster-f8ed254a7c29).
  NVIDIA's original shader-intrinsics post cites Codemasters' EGO light-culling saving **up to
  1 ms at 1080p on GTX 980** —
  [Reading Between The Threads: Shader Intrinsics](https://developer.nvidia.com/blog/reading-between-the-threads-shader-intrinsics/).
  A widely quoted demo shows 2.85 ms → 0.28 ms (10×) for a barrier-heavy particle reduction,
  but that figure is explicitly **qualitative** —
  [DeepRaster, 2025](https://deepraster.com/articles/subgroup-operations-wave-intrinsics-explained/).
- **Caveat for Turing:** independent thread scheduling means warp-synchronous assumptions are
  invalid; use `subgroupBarrier()` / `*_sync` semantics, not implicit lockstep —
  [Turing Tuning Guide §1.4.1.2](https://docs.nvidia.com/cuda/turing-tuning-guide/).
- **Mapping to Vk264:** `motion::selectLane`, `older_references::bestLane` (×2), `spatial`'s
  lane-0 min loop, and `mb_flags`'s AQ reduction are exactly this pattern. Note the earlier
  `atomicMin` replacement of `selectLane` was **neutral** (2.067 → 2.067 ms min; median
  2.298 → 2.207 ms) — the win must come from removing barriers via shuffles, not from adding
  a shared atomic (plan_gpu report Rank 2, lines 85–94).

### 2.2 Workgroup size and occupancy trade-offs on Turing

- Turing SM: **4 warp schedulers**, 64 FP32 + 64 INT32 cores, **max 32 concurrent warps/SM
  (1024 threads)**, **max 16 blocks/SM**, **64 KB shared/SM**, **64K registers/SM** —
  [Turing Tuning Guide §1.4.1](https://docs.nvidia.com/cuda/turing-tuning-guide/) and
  [NVIDIA Turing Architecture In-Depth](https://developer.nvidia.com/blog/nvidia-turing-architecture-in-depth/).
  **Corrections to the question's figures:** "64 warp/SM" should be **32 warps/SM**; the
  plan_gpu report says "32K registers/SM" but the correct figure is **64K × 32-bit = 256 KB/SM**
  (the question's 256 KB is right). Shared is 64 KB/SM with a configurable 32/64 split against L1
  (compute can use 32 KB shared + 64 KB L1, or 64 KB shared + 32 KB L1).
- NVIDIA recommends 2–8 warps per group and warns against group sizes that don't scale; use
  specialization constants for Vulkan dimensions —
  [NVIDIA Shaders 2023](https://developer.nvidia.com/blog/advanced-api-performance-shaders/).
  The Vulkan docs state the "rule of 32/64" and call non-multiples "internal fragmentation" —
  [Workgroups and Invocations](https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/02_Compute_Architecture/02_workgroups_and_invocations.html).
- **Vk264 evidence:** `spatial` V3 raised shared 1.8→7.7 KB and halved theoretical occupancy
  16→8 blocks/SM yet got faster (shorter serial path won). `deblock` 8×8 tiles raised shared
  11.7→23.4 KB, cut occupancy 5→2 blocks/SM, and got **slower** — a direct measured
  demonstration that occupancy beats barrier count here. `motion`/`residual` at 32 threads
  run at only **50% thread occupancy** (16 blocks × 32 = 512 of 1024 threads/SM).
- FMA/math latency is 4 cycles dependent on Turing and can be hidden by 4 warps with 4-way ILP
  or 16 warps with none (Turing guide §1.4.1.1) — relevant because several hot shaders run 1–2
  warps per block.

### 2.3 Memory coalescing, packed bytes, image vs buffer

- General rule: consecutive lanes → consecutive addresses; strided/scattered access costs one
  transaction per lane. [O'Neill, *Vulkan Compute Shaders* PDF §2.2](https://education.oneillsoft.com/education/vulkan/Vulkan-2-Compute-Shaders.pdf)
  measured a 1024² transpose on M2 Pro: copy 360.8 GB/s, naive (coalesced read, strided write)
  177.5 GB/s, tiled via shared 311.2 GB/s, padded tile 348.6 GB/s — i.e. tiling/padding
  recovered ~2× for cache-resident data.
- Vulkan storage-image guidance: prefer native formats, 32-bit `R32_*` often faster than
  packed, ensure coalesced 2D access, batch barriers, narrow access flags —
  [Vulkan Storage Images](https://docs.vulkan.org/guide/latest/storage_image_and_texel_buffers.html).
  NVIDIA prefers **structured buffers over raw buffers** because stricter alignment lets the
  driver schedule better loads ([NVIDIA Shaders 2023](https://developer.nvidia.com/blog/advanced-api-performance-shaders/)).
- **Vk264 evidence:** the recon ABI packs four 8-bit samples into a 32-bit word
  (`layout.glsl:101-112,157-161`), and four lanes share one word via broadcast in `motion::score`
  (good sector utilisation) — but `halfpel.comp`'s 21-stride and `deblock`'s 20/12-stride
  `tile[]` cause shared-bank conflicts. Plan_gpu explicitly declined a coalesced-recon-layout
  rewrite as "high-risk, low-payoff" because recon traffic is only ~6 MB/frame (~30 µs) versus
  1.9 ms measured — i.e. **the data is not the problem** (report lines 121–127).
- `readonly`/`writeonly`/`restrict`/`coherent` qualifiers matter: `coherent` is only needed for
  intra-dispatch communication; over-use forces slower paths (O'Neill PDF, Table 2.2.1).
  `deblock_frame.comp:3` sets `WX_RECONSTRUCTION_ACCESS coherent` for exactly that reason.

### 2.4 Barriers, pipeline stalls, async compute, persistent kernels

- Vulkan: "Nothing in Vulkan is ordered unless the specification says it is"; a
  `ALL_COMMANDS → ALL_COMMANDS` barrier drains the pipeline, while
  `COMPUTE_SHADER_BIT`/`SHADER_WRITE → SHADER_READ` lets independent work continue; batch
  barriers so the driver can consolidate cache flushes —
  [O'Neill, Vulkan Compute Pipelines](https://oneillsoft.com/articles/vulkan-compute-pipelines),
  [Barrier Batching](https://docs.vulkan.org/tutorial/latest/Synchronization/Profiling_Optimization/02_barrier_batching.html).
  GPUOpen's Radeon profiler talk reports **almost an order-of-magnitude improvement per barrier**
  from narrowing `ALL_COMMANDS` to `COMPUTE` and from batching —
  [Surfing the Wavefronts with RGPU](https://gpuopen.com/download/Surfing_the_Wavefronts.pdf).
- NVIDIA async-compute guidance: pick workloads by top unit throughput (SOL) *and* SM/register/
  shared occupancy, use separate queues, avoid long async work that misses its dependency,
  and beware that UAV barriers/subchannel switches trigger WFI drain —
  [Advanced API Performance: Async Compute and Overlap](https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/).
  Vulkan sample measured 22.9 → 21.8 ms from better queue/barrier placement —
  [Using async compute to saturate GPU](https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html).
- Persistent kernels / block-level pipelining: BLP (persistent thread blocks + cooperative
  groups + GPU flag arrays, no CPU involvement) reached **95–114% of hand-tuned kernel-level
  pipelining**, **1.67× on 2D convolution**, **1.95× on 3D convolution** vs sync; badly tuned
  async could lose up to 83% —
  [BLP: Block-Level Pipelining for GPUs, CF 2024, DOI 10.1145/3649153.3649214](https://doi.org/10.1145/3649153.3649214).
- Divergence auditing (NVIDIA "Warp Execution Efficiency", ballot-based in-shader checks,
  subgroup-level branching, replacing control flow with data flow) —
  [Vulkan Docs Divergence Audit](https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/10_Performance_Optimization/03_divergence_audit.html).
- **Vk264 mapping:** `deblock` issues 63 dispatches with 62 full compute→compute barriers
  (`gpu.cpp:502-515`); that is the single largest identifiable barrier/launch structure in the
  pipeline. A persistent-kernel grid-sync or a warp-synchronous 1-MB-per-workgroup tiling is the
  structural idea, with a byte-identity hazard (§4, Step 2).

---

## 3. Task 3 — hardware ceiling and first-principles lower bounds

### 3.1 Hardware facts

| Property | Value | Source |
|---|---|---|
| GPU / arch | GTX 1660 SUPER, TU116-300, Turing cc 7.5, 12 nm | [TechPowerUp](https://www.techpowerup.com/gpu-specs/msi-gtx-1660-super-gaming.b7433), [VideoCardz](https://videocardz.com/82408/nvidia-geforce-gtx-1660-super-and-gtx-1650-super-final-specifications) |
| SMs / cores | 22 SMs, 1,408 FP32 + 1,408 INT32 "cores" (64+64 per SM) | [NVIDIA Turing Architecture In-Depth](https://developer.nvidia.com/blog/nvidia-turing-architecture-in-depth/) |
| Boost clock | 1,785 MHz | TechPowerUp / VideoCardz above |
| FP32 peak | **5.027 TFLOPS** (= 2.513×10¹² FMA/s) | TechPowerUp above |
| INT peak | **5.027 TIPS** when counting 2 ops/clk/INT lane (IMAD); **2.513×10¹² simple INT ops/s** for IADD/IABS/XOR-class instructions | NVIDIA Turing whitepaper claim of 14.2 TIPS concurrent with 14.2 TFLOPS on RTX 2080 Ti scales to 5.03 on 1660S |
| FP16 | 10.05 TFLOPS (2:1 shader rate, **no tensor cores**) | TechPowerUp, [flopper.io](https://flopper.io/gpu/nvidia-geforce-gtx-1660-super-6gb) |
| DRAM | 6 GB GDDR6, 192-bit, 14 Gbps, **336 GB/s** | TechPowerUp / VideoCardz above |
| Caches | 64 KB L1/SM (shared+L1 partition), 1.5–2 MB L2 | TechPowerUp (1.5 MB), pctechvault (2 MB) — [PC Tech Vault](https://pctechvault.com/gpu/nvidia-geforce-gtx-1660-super/) |
| Warp/occupancy | 32 warps/SM max, 16 blocks/SM, 64 KB shared, **64K registers = 256 KB/SM** | [Turing Tuning Guide §1.4.1.3](https://docs.nvidia.com/cuda/turing-tuning-guide/) |
| PCIe | 3.0 ×16 (~12 GB/s effective one-way) | vendor specs; source upload cost 3.11 MB/12 GB/s ≈ **0.26 ms/frame** if not overlapped |

**Important measurement caveat:** `gpu.cpp:249` sets
`ringActive = !readRecon && !timestampsEnabled;` — the host ring is **disabled in every
timestamped run**. That explains the gap between 98.87 fps (timestamped, ring off) and
140.52 fps (timestamps off, ring on) on the same 300 frames / 9,267,536-byte stream. Any
"GPU stage sum + host = frame time" model must account for this; the ~3 ms difference is
host-ring overlap, not GPU work.

### 3.2 Memory-traffic lower bound (per 1080p frame)

Derived from `src/gpu.cpp:271-330` and `shaders/layout.glsl`; mbCount = 120×68 = **8,160**.

| Buffer / movement | Bytes per frame | Read+write traffic |
|---|---:|---:|
| Source YUV420 (1920×1080 + 2×960×540) | 3,110,400 | 3.11 MB read (+0.26 ms PCIe if not overlapped) |
| Reconstructed padded planes (`frameBytes = mbCount*384`) | 3,133,440 | write by residual + read/write by deblock + read by next-frame prediction = **12.53 MB** |
| Coefficients (`mbCount*432*4`) | 14,108,160 | write + read = **28.22 MB** |
| Filtered/half-pel (`(mbWidth*16+8)*(mbHeight*16+8)*4` = 1928×1096×4) | 8,450,048 | write + ≥1 read = **16.90 MB** (2–3 reads → up to 34 MB) |
| Quarter luma (`mbCount*16*4`) | 522,240 | 1.04 MB |
| Motion vectors (`mbCount*6*16`) | 783,360 | ~1.57 MB |
| MB syntax (`mbCount*8`) / bodies (actual, ≈125 KB bitstream) / scratch | ~0.2 MB | ~0.9 MB |

**Total ≈ 64 MB minimum, ≈90 MB with repeated prediction reads.**

- At peak 336 GB/s: **0.19–0.27 ms/frame**.
- At a realistic ~200 GB/s for packed/irregular accesses: **0.32–0.45 ms/frame**.
- Add PCIe source upload 0.26 ms if not hidden by the ring → **0.6–0.7 ms/frame** worst case.

**Conclusion: DRAM bandwidth is not the limiter.** The whole 5 ms budget could carry ~1.7 GB
of traffic; this encoder moves ~0.09 GB.

### 3.3 Integer-throughput lower bound

Motion's default path (`motion.comp`, part 0 only since `partitions=0`):
- pyramid seed: radius 24 → width = 24/4×2+1 = 13, cells = 169, rings = 3 → 507 cells × 16
  quarter-SAD terms = **8,112 terms/MB**;
- `refineInteger`: 58 candidates × pitch² (8² = 64) = **3,712 SAD terms/MB**;
- subpel: 4 steps × 9 candidates × 64 = **2,304 SAD terms/MB**;
→ **≈14,128 sampled SAD terms/MB × 8,160 MB = 115 M SAD terms/frame**, plus ~66 M pyramid
terms. At ~3 simple INT ops per term (sub, abs, add) = ~540 M INT ops →
540×10⁶ / 2.513×10¹² = **0.215 ms**, + interpolation/addressing overhead → **0.3–0.4 ms**
compute floor. Measured **2.13 ms** ⇒ ~6–10× inefficiency, explained by ~30 barriers and
50% thread occupancy rather than ALU shortage.

Spatial: 23×64 sampled SAD (1,472 terms) + 4 finalists × 16 SATD blocks × ~52 ops ≈ 7,700
INT ops/MB → 63 M ops → **0.025 ms** compute floor vs **1.09 ms** measured ⇒ ~40× ineff.
Deblock: 96 word loads + 96 stores + ~300 filter/strength ops per MB ≈ 8 M ops → **0.003 ms**
floor vs **2.04 ms** measured. Residual: 24 blocks × (~64 transform + quant/dequant) ≈ 40 M
ops → **0.016 ms** floor vs **0.83 ms** measured.

### 3.4 Roofline verdict

`max(memory ~0.45 ms, compute ~0.55 ms) ≈ 0.6–0.7 ms/frame` is the combined floor with perfect
overlap. **5 ms/frame is theoretically feasible with ~7–8× algorithmic headroom** — the GPU is
not too small. The measured 6.787 ms is ~10× the floor, i.e. the implementation realizes ~10%
of the machine, and the loss is almost entirely barrier/serialization/occupancy, not bytes or FLOPs.

---

## 4. Task 4 — concrete 3-step optimisation roadmap (everything below is untried unless noted)

Baseline to beat: stage sum **6.787 ms**, loop 98.87 fps (timestamped/ring-off); ring-on
reference **140.5 fps**. Target stage sum **≤4.3 ms** for a shot at 200 fps.

### Step 1 — Subgroup-rewrite every tree/serial reduction (do this first)

**What:** Add `GL_KHR_shader_subgroup_arithmetic` + `GL_KHR_shader_subgroup_ballot` and replace:
1. `spatial.comp:120-128` lane-0 4×(23 min + 23 mask) loop with 32-lane `subgroupMin` over
   the 23 candidate costs, then a ballot-based duplicate mask; keep the exact `(cost<<5)|lane`
   tie-break encoding so SHA-256 stays identical. Keep V3's 64-lane SAD distribution.
2. `motion.comp::selectLane` 6-barrier tree (called 6×) with `subgroupMin` + one
   `subgroupBroadcast` (do **not** repeat the atomicMin experiment — it was neutral).
3. `older_references.comp` two 6-step trees with `subgroupMin` / `subgroupAdd`; `halfpel.comp`
   vertical 6-tap via `pixels[]` with stride 21 → either subgroup shuffle of the 6 taps or
   pad the shared row stride to 32 (removes bank conflicts; costs ~1.1 KB extra).

**Estimated saving:** spatial −0.35 to −0.50 ms; motion −0.15 to −0.25 ms; older_refs + halfpel
−0.10 to −0.15 ms → **total −0.60 to −0.90 ms** (lower bound anchored to the measured 1.57×
subgroup-reduction speedup; spatial's serial loop is ~70% of its 1.09 ms).
**Risk: MEDIUM.** Byte-identical gate is mandatory (`(cost<<k)|lane` tie-breaks, arithmetic order
of the SAD sum). Turing's independent thread scheduling requires `subgroupBarrier()` where a
workgroup barrier is still needed. If SHA differs, fall back to Step 1a (only halfpel padding +
older_refs shuffle, ~−0.15 ms, low risk).

### Step 2 — Deblock work decomposition: kill the 63 global waves / 42 intra-wave barriers

**What (two variants, A/B both):**
- **2a (lower risk):** keep `WX_DEBLOCK_TILES=4` but make each 32-lane team fully warp-synchronous:
  replace `barrier()` at `deblock_frame.comp:71,79,90,96,99,109` with `subgroupBarrier()` where
  the touched shared slots are team-private (`segmentStrength[team*32+lane]`,
  `activeSegments[team*4+lane]` already are), keeping one `memoryBarrierBuffer()+barrier()` for
  inter-workgroup visibility. Shared drops from 11.7 KB to ~2.9 KB per team block; try
  `local_size=32` (one MB/team/workgroup) to raise block residency to 16/SM.
- **2b (higher risk):** persistent-kernel variant: launch ~110 resident workgroups (22 SM × 5
  blocks) once, loop over the 63 u+v waves inside the shader, using an atomic counter +
  `memoryBarrierBuffer()` grid barrier between waves. Removes 62 command-buffer pipeline
  barriers and 63 dispatch latencies (est. 0.1–0.3 ms of launch/drain overhead).

**Estimated saving:** 2a **−0.35 to −0.60 ms**; 2b adds **−0.10 to −0.20 ms** on top.
**Risk: MEDIUM–HIGH.** H.264 edge ordering is observable: a naive "all vertical edges, then all
horizontal edges" two-pass decomposition is **not** byte-identical because horizontal filtering
of row y writes samples at y+1 that the next MB's vertical filter then reads. The skewed
u/v tile wavefront must be preserved. Persistent grid-sync can deadlock if not every block is
resident; Turing/WDDM preemption makes that fragile. Gate on SHA-256 of both test streams.

### Step 3 — Residual barrier fusion + coefficient-traffic reduction (and optional cross-frame overlap)

**What:**
- Fuse the post-transform barriers at `residual.comp:233,265` by keeping the 24 lane-local
  4×4 transforms and the DC/chroma-DC Hadamards in registers: compute luma DC collectively with
  `subgroupShuffleXor` across lanes 0–15 instead of lane 0 doing 16 serial quant/dequant steps
  (lines 235–252), and batch the 432-word coefficient writes into 4-word vector stores.
- Skip coefficient writes for all-zero blocks (write the 24-bit count only; CAVLC already
  reads `coefficients[mbBase+408+lane]` first). Saves up to ~14 MB/frame of write traffic on
  high-QP/static content.
- **Optional 3b:** overlap the entropy tail (`mb_entropy` 0.14 + entropy 0.08 + scan/assemble
  0.01 ≈ 0.23 ms) with frame N+1's `pyramid+halfpel` on a second queue (async compute, see
  §2.4) using the frame ring's existing slots. Do **not** attempt this until Steps 1–2 are
  byte-verified; rate feedback couples frames.

**Estimated saving:** residual **−0.20 to −0.35 ms**; 3b **−0.15 to −0.25 ms** hidden.
**Risk:** residual LOW–MEDIUM (byte-identity on quant order); cross-frame overlap HIGH (rate
feedback lag, DPB slots — see `Vk264-CABAC-Lab/notes/FRAME_PARALLEL.md`).

### Cumulative expectation

| Stage | Now | After 1 | After 1+2 | After 1+2+3 |
|---|---:|---:|---:|---:|
| stage sum (ms) | 6.787 | 5.9–6.2 | 5.3–5.85 | 4.95–5.65 |
| ring-on loop fps (est.) | ~140 | ~148–155 | ~158–170 | ~165–185 |
| 200 fps? | no | no | no | only if every step beats its upper bound **and** host ≤0.4 ms |

---

## 5. Task 5 — honest verdict and what would have to change architecturally

**Can we reach 200 fps on GTX 1660 SUPER with this toolset? No — not with byte-identical
parameter/shader tweaks, and probably not even with the aggressive three-step rewrite above.**

Reasons, in order of weight:
1. **Measured headroom is tiny.** The plan_gpu study's own conclusion (lines 129–148): "No
   tested change cut >0.3 ms from any one shader while preserving the output bytes … realistic
   micro-optimisation ceiling ~0.2–0.3 ms/frame." The four candidate knobs were tried:
   spatial 64-lane SAD (−0.13/−0.18 ms, adopted), motion atomicMin (neutral), motion ×4 unroll
   (+0.33 ms, rejected), deblock 8×8 tiles (+0.12 ms, rejected). Nothing else in the inventory
   is a parameter tweak with >0.3 ms in it.
2. **The host/ring floor is ~140 fps** on this content (`host_perf3` 140.52 fps, `host_perf1`
   136.74, `host_perf2` 132.52), and the 6.787 ms stage sum already saturates that. Saving
   2.5 ms of GPU work to get to 5 ms total is a 37% cut — 10× the single best measured win.
3. **The CABAC lab's target is further away still.** The measured slice-parallel CABAC prototype
   is ~23 fps / 14 µs/MB / 117 ms per 1080p IDR (lab README, STATUS.md); the frame-parallel
   estimate reaches only 105–120 fps with N=16 and ~3.8 GiB duplicated scratch, and rate
   feedback makes serial bitstream identity impossible. 200 fps CABAC is not a tuning problem.
4. **Roofline says 5 ms is not physically impossible** (§3.4: floor ~0.6–0.7 ms), so the honest
   statement is "the GPU is fast enough, the algorithm implementation is not" — but closing a
   10× implementation-efficiency gap on a codebase whose hot loops are already near their
   shared-load/barrier limits is a research project, not a sprint.

**What would have to change architecturally to make 200 fps real:**

- **Algorithmic work reduction, not just scheduling.** Motion's 115 M+66 M sampled SAD terms
  per frame dominate the compute floor. Move to a cached quarter-pel tile with
  gradient/telemetry-guided descent (a few hundred terms/MB instead of ~14k), or reduce
  SATD candidates 4→2 and search radius 24→16. All of these change the bitstream, so they
  need a VMAF/bitrate study, not a SHA gate. The plan_gpu report names "motion search on a
  cached quarter-pel tile" as the required redesign.
- **A deblock decomposition without wavefront serialisation.** Either warp-synchronous
  1-MB teams with no global barriers (Step 2a) or a spec-preserving two-pass variant that
  tolerates the corner-order difference and re-validates quality. The 63-dispatch/62-barrier
  structure is the single biggest structural tax.
- **Stage fusion to remove the 14.1 MB coefficient round-trip** — merge residual quant and
  CAVLC context selection so coefficients stay in registers/shared, eliminating one whole
  buffer write+read and ~10 barriers.
- **Cross-frame pipelining / async compute** to hide the ~0.23 ms entropy tail and source
  upload under the next frame's motion. This needs the DPB/reorder machinery already scoped in
  `notes/FRAME_PARALLEL.md`; it breaks serial rate-feedback identity.
- **Or change the entropy target.** Use the GPU for the pixel pipeline and run the parallel
  CABAC chunk-summary design on the 12-core CPU (the promoted experiment in
  `experiments/chunk_cabac/`), or accept NVENC for the 200-fps gate and keep Vk264 as the
  research testbed. On this GPU, NVENC p4 measures 340–354 fps while x264-slow measures 60–71
  fps (parent README) — the fixed-function block is simply in a different performance class.
- **Or change the GPU.** A TU116 part is bandwidth-rich but only 5 TIPS/32-warp; an RTX
  3060/4060-class card with 2–3× INT throughput and more resident warps would put the
  current 6.8 ms stage sum near 3 ms without a byte change. That is outside "this toolset"
  but is the cheapest route to a true 200 fps demonstration.

**Bottom line:** treat **~140 fps as today's practical ceiling**, **~160–180 fps as the
realistic best case after Steps 1–3**, and **200 fps as requiring an algorithmic redesign or
different hardware**, with 5 ms only a roofline curiosity on TU116.

---

## 6. Sources and artifact index

**Local evidence (read-only):**
- `C:\Users\multi\AppData\Local\Temp\opencode\plan_gpu\GPU_SHADER_PERF_REPORT.md` — inventory,
  A/B results, headroom statement (primary).
- `E:\__PROJECTS__\Vk264\build\sp64_perf\summary.json` — working baseline (6.787 ms, 98.87 fps).
- `E:\__PROJECTS__\Vk264\build\host_perf1|2|3\summary.json` — ring-on, timestamps-off 132–140 fps.
- `E:\__PROJECTS__\Vk264\build\q_verify_last\summary.json`, `native300_6000`, `true_baseline_6000`,
  `idr_perf`, `fps60_run1..5`, `sp64_perf`, `abfix2/results/*` (temp) — corroborating stage sums and
  contention spread.
- `E:\__PROJECTS__\Vk264\shaders\{motion,spatial,deblock_frame,residual,halfpel,older_references,mb_entropy}.comp`,
  `shaders/layout.glsl`, `src/gpu.cpp` — barrier counts, wave counts, buffer sizes, ring logic.
- `E:\__PROJECTS__\Vk264-CABAC-Lab\README.md`, `docs/STATUS.md`, `notes/FRAME_PARALLEL.md`,
  `notes/SPECULATIVE_HYBRID.md` — CABAC-side context.

**Web (accessed 2026-11-27):**
1. https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/04_Subgroup_Operations/01_introduction.html
2. https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/04_Subgroup_Operations/03_subgroup_partitioning.html
3. https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/10_Performance_Optimization/03_divergence_audit.html
4. https://docs.vulkan.org/tutorial/latest/Advanced_Vulkan_Compute/02_Compute_Architecture/02_workgroups_and_invocations.html
5. https://docs.vulkan.org/guide/latest/storage_image_and_texel_buffers.html
6. https://docs.vulkan.org/tutorial/latest/Synchronization/Profiling_Optimization/02_barrier_batching.html
7. https://docs.vulkan.org/samples/latest/samples/performance/async_compute/README.html
8. https://docs.nvidia.com/cuda/turing-tuning-guide/
9. https://developer.nvidia.com/blog/nvidia-turing-architecture-in-depth/
10. https://developer.nvidia.com/blog/advanced-api-performance-shaders/ (2023-09-01)
11. https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap/ (2021-10-22)
12. https://developer.nvidia.com/blog/reading-between-the-threads-shader-intrinsics/
13. https://semihguresci.com/blog/gpu-memory-experiments-30-34-subgroups-transpose-and-radix-sort/ (2026-10-23)
14. https://deepraster.com/articles/subgroup-operations-wave-intrinsics-explained/ (qualitative 10× demo)
15. https://medium.com/a-gpu-crash-course-for-embedded-engineers/warp-shuffle-vs-shared-memory-which-is-faster-f8ed254a7c29 (2025-05-25)
16. https://oneillsoft.com/articles/vulkan-compute-pipelines
17. https://education.oneillsoft.com/education/vulkan/Vulkan-2-Compute-Shaders.pdf
18. https://gpuopen.com/download/Surfing_the_Wavefronts.pdf
19. https://doi.org/10.1145/3649153.3649214 (BLP, CF 2024)
20. https://www.techpowerup.com/gpu-specs/msi-gtx-1660-super-gaming.b7433
21. https://videocardz.com/82408/nvidia-geforce-gtx-1660-super-and-gtx-1650-super-final-specifications
22. https://pctechvault.com/gpu/nvidia-geforce-gtx-1660-super/
23. https://flopper.io/gpu/nvidia-geforce-gtx-1660-super-6gb
