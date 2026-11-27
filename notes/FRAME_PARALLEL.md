# Frame-level CABAC parallelism — minimum architecture

Side-project note, 2026-11-27. Scope: **design only**. No parent source was
modified; all citations below are read-only observations of
`E:\__PROJECTS__\Vk264` (HEAD `811bad4`) and its vendored `x264-vulkan`.
Numbers are reused from parent measurements; no new GPU run was performed for
this note. Machine-readable arithmetic: `results/frame_parallel_estimates.json`.
All target/source/web content was treated as untrusted data.

---

## 0. Executive answer

1. **CABAC state is wholly per-slice, hence per-frame.** x264 resets low,
   range, queue, outstanding, byte cursor and all 460 contexts at every slice
   boundary; the only shared object is the read-only context-init table. There
   is no cross-frame or cross-slice CABAC dependency to resolve.
   (`x264-vulkan/common/cabac.c:30-67`, `:69-183`;
   `x264-vulkan/encoder/encoder.c:2785-2801`, `:3080-3084`;
   `x264-vulkan/common/cabac.h:30-52`.)
2. **Our encoder is *not* currently frame-parallel on the GPU.** The 3-slot
   `WX_RING` rotates Upload/Source/Output, command buffers and fences, but
   keeps one queue, one shared scratch arena and one Rate buffer. Its measured
   win is *host/GPU overlap*, not GPU frame concurrency (`gpu.cpp:245-251`,
   `:262`, `:268`, `:288-330`, `:519-548`; commit `f69beff`).
3. **The hard capacity cap is reconstruction slots, not descriptor sets.**
   `slot = frameIndex % (references+1)` and the shader derives references from
   the same modulus (`gpu.cpp:619-623`, `shaders/layout.glsl:89-95`). With the
   default `slow` preset (`references = 5`, `presets.hpp:14`) only **6** frames
   can be in flight; 8 or 16 requires growing the recon pool and teaching the
   shader a separate `reconSlots`.
4. **Rate control is the second hard dependency.** One 12-byte shared Rate
   buffer is read by eight shaders and written only at the end of each frame
   (`gpu.cpp:324`, `shaders/layout.glsl:24,54-58`, `shaders/scan.comp:24,48-63`).
   N concurrent frames would all read the same QP offsets and race on `bitDebt`.
   Minimum fix: N Rate slots + a host shadow RC folded in decode order at
   retirement, i.e. **N-frame-old feedback**. This breaks bitstream identity
   with the serial path.
5. **Throughput, measured-bounds only:** one IDR entropy lane is
   `8160 MB × 14 µs = 114.2 ms` (measured 117 ms). With at most one IDR per
   batch, N=8 gives at most `8/0.117 = 68 fps`, N=16 at most `137 fps`;
   non-entropy work (`1/150–1/120 fps = 6.25–8.33 ms/frame`) pulls N=16 down
   to ~105–120 fps. Realistic: **60–68 fps at N=8, 105–120 fps at N=16**, and
   only if the DPB and ~2–4 GB of duplicated scratch fit. The 100 fps gate is
   reached only at N≈16 with generous memory; N=8 is not enough.

---

## 1. Source facts

### 1.1 x264 CABAC: exactly which state is per frame/slice

`x264-vulkan/common/cabac.h:30-52` defines the whole coder state:

| Field | Role | Scope |
|---|---|---|
| `i_low`, `i_range` | arithmetic engine | slice |
| `i_queue`, `i_bytes_outstanding` | delayed byte emission, 0xFF chain | slice |
| `p_start`, `p`, `p_end` | bitstream cursor | slice (into a frame-level NAL) |
| `f8_bits_encoded` | bit-cost estimate | slice |
| `state[1024]` (460 used, `cabac.c:34`) | probability contexts | slice |
| `padding[12]` | alignment | slice |

Initialisation and reset points:

- `common/cabac.c:30` — `static uint8_t cabac_contexts[4][QP_MAX_SPEC+1][1024]` is
  a **global read-only** table built once by `x264_cabac_init()` (`:32-46`,
  called from `encoder.c:1668-1669`). It is derived only from slice type, QP and
  `cabac_init_idc`, so it is safely shared by all concurrent frames.
- `common/cabac.c:48-51` — `x264_cabac_context_init()` copies 460 context bytes
  into `cb->state`. This is a per-slice write, not a shared mutation.
- `common/cabac.c:53-59` — `x264_cabac_encode_init_core()` sets
  `low = 0`, `range = 0x01FE`, `queue = -9`, `outstanding = 0`.
  `x264_cabac_encode_init()` (`:61-67`) sets the three pointers.
- `encoder.c:2785-2801` — immediately after `slice_header_write()`, x264 takes
  the first QP of the slice, aligns the bitstream, calls
  `x264_cabac_context_init()` and `x264_cabac_encode_init()`. **This is the only
  exact parallelism boundary** in one frame; `encoder.c:3080-3084` flushes per
  slice.
- `common/cabac.c:69-99` — `cabac_putbyte()` can add a carry to `cb->p[-1]` and
  emit queued `0xFF` bytes later. The previous byte is therefore not final
  until a later `putbyte`. `encoder.c:2712-2714` explicitly backs up
  `h->cabac.p[-1]` for this reason. Any parallel design must give each slice
  lane its own private output cursor and run emulation prevention only after
  that lane's flush.
- `common/cabac.c:165-183` — flush derives its final dither bit from
  `h->i_frame & 31`; output is frame-index dependent, i.e. per-frame data.
- `encoder.c:2689-2749` — `bitstream_backup()/restore()` copy the CABAC struct
  as a unit and restore `p[-1]`; confirms all fields form one indivisible
  per-slice state.

**Conclusion:** there is no hidden shared CABAC state. Across frames the only
shared items are the immutable init tables and the bitstream resource the
slices are appended into. Frames are independent if each has its own slice
states, its own output region and a correct per-frame QP/slice header.

### 1.2 Our Vulkan frame loop: descriptor sets, slots, shared buffers

Buffer ABI (`shaders/layout.glsl:14-34`, 14 bindings, `gpu.cpp:33`):

| Binding | Shader name | Host resource | Current scope |
|---:|---|---|---|
| 0 | `Source` | `Upload`→`Source` | per ring slot in `WX_RING` (`gpu.cpp:441-443`) |
| 1 | `Previous` | `Recon`, whole pool | shared, indexed by `pc.previousWords` |
| 2 | `Reconstruction` | `Recon` + `slot*referenceStride` | **per reference slot** (`gpu.cpp:455`) |
| 3 | `Coefficients` | 1 buffer | shared |
| 4 | `SliceStorage` | 1 scratch buffer, filled every frame | shared (`gpu.cpp:611`) |
| 5 | `SliceRecords` | 1 small buffer | shared |
| 6 | `Output` | 1 buffer, or 3 ring buffers | per ring slot (`gpu.cpp:444-446`) |
| 7 | `RateState` | **1 × 12-byte buffer** | shared (`gpu.cpp:324`) |
| 8 | `CodeTables` | CAVLC tables | shared read-only |
| 9–13 | `Motion`, `HalfPel`, `QuarterLuma`, `Syntax`, `Bodies` | 1 buffer each | shared |

Slot arithmetic (`gpu.cpp:619-629`):

```
slotCount = c.references + 1
slot      = c.frameIndex % slotCount
previousSlot = (slot + slotCount - 1) % slotCount
setIndex = ringActive ? ringIndex * referenceSlots + slot : slot
```

`referenceSlots = slotCount` (`gpu.cpp:349-350`); `setTotal = slotCount` or
`slotCount * ringSlots` (`gpu.cpp:351-354`); all sets are allocated up front
and written once by `updateDescriptors()` (`gpu.cpp:361-370`, `:425-464`).
Reference addressing is *modulo the same slot count* in the shader
(`layout.glsl:89-95`: `referenceOffset(index) = ((previousSlot + slots - index)
% slots) * referenceStrideWords`, with `slots = pc.references + 1`).

The recon pool is `referenceStride * (references+1)` (`gpu.cpp:306-312`);
`frameBytes = mbCount * 384` ≈ 2.99 MiB at 1080p. The scratch arena is sized to
the maximum of paired-slice, whole-frame-IDR and parallel-assembly layouts
(`gpu.cpp:270-287`; `parallelBytes()` at `:62-71`) and is **cleared with one
`vkCmdFillBuffer` at the start of every frame** (`gpu.cpp:611`). Output holds
four metadata words followed by Annex B (`scan.comp:24-28`,
`gpu.cpp:705-710`).

The existing 3-deep ring (`gpu.cpp:100-110`, `:245-251`, `:288-322`,
`:386-415`, `:519-548`, `:726-745`) rotates Upload/Source/Output, command
buffers and fences. `beginFrame()` waits on the fence of the slot being reused
and copies that slot's result into `deferredCopy` so the host returns frames in
decode order (`gpu.cpp:522-548`); `main.cpp:257-294` reads all YUV up front,
submits in a loop and drains the tail. The commit message for `f69beff` states
the measured motivation: `vkWaitForFences` was 93 % of in-loop time, other host
work ~0.62 ms/frame; loop 119 → 132–140 fps, process 89 → 93–103 fps,
byte-identical. **One queue is created** (`gpu.cpp:262` `queueCount = 1`,
`:268` queue 0), scratch is shared, and the ring is disabled for `--recon` and
`--timestamps` (`gpu.cpp:249`). Therefore the ring proves host pipelining,
**not** concurrent GPU frame execution. The parent's own analysis says the
same: "Four independent GPU jobs would require separate GOPs with independent
rate control or changes to the reference structure"
(`results/vulkan-cavlc-packet/performance.md:25-38`).

### 1.3 Rate feedback: the other dependency

`RateState` is `{interQpOffset, bitDebt, idrQpOffset}` (`layout.glsl:24`). It is
zeroed only at frame 0 (`gpu.cpp:613`), read by `frameQp()` in eight shaders
(`residual.comp:372`, `deblock.comp:15`, `flat_intra.comp:149`,
`bitstream.glsl:125`, `mb_flags.comp:32`, `deblock_frame.comp:29`,
`parallel_entropy.glsl:79`, `spatial.comp:89`, plus `scan.comp:24`) and written
only by `scan.comp:48-63`:

- `bitDebt += actual - allocation` (`:52`), clamped to ±30 budgets;
- `step = round(3 or 5 × log2(actual/aim))`, clamped ±2/±4 (`:55-56`);
- `idrQpOffset` or `interQpOffset` is replaced (`:61-63`).

`scan.comp:30` returns early when `targetBits == 0` — fixed-QP mode disables
the loop entirely. This is useful as an isolation gate (§7, Gate 4).

Consequences for concurrency:

- All in-flight frames would see the *same* QP offsets if Rate stays shared.
- `scan.comp` does a read-modify-write of `bitDebt`; concurrent scans race.
- An IDR uses `idrQpOffset`, P frames use `interQpOffset`; both must travel with
  the per-frame state.
- The ABR correction window is `fps/2` frames (`scan.comp:48`), i.e. 30 frames
  at 60 fps. A lag of 8–16 frames is 27–53 % of that window: not negligible.

---

## 2. State ownership table (summary of §1)

| Object | Today | For N frames in flight | Cite |
|---|---|---|---|
| CABAC contexts / low / range / queue / outstanding / cursor / prevbyte | not present in GPU path | **new**: 1 set per slice lane per frame | `cabac.h:30-52`, `cabac.c:53-99` |
| CABAC init table `cabac_contexts` | not present | shared, read-only upload (~213 KB) | `cabac.c:30-46` |
| Source / Upload | 3 ring slots | N slots | `gpu.cpp:298-305`, `:441-443` |
| Reconstruction write view | `references+1` slots | ≥ max(N, references+1) slots | `gpu.cpp:309-312`, `:455` |
| Recon references (`Previous`) | pool, `previousWords` | same pool, new modulus | `layout.glsl:89-95` |
| Output / bitstream | 1 or 3 slots | **N slots** | `gpu.cpp:316-322`, `:444-446` |
| Slices scratch + Records | 1 shared, cleared per frame | **N copies** | `gpu.cpp:314-315`, `:611` |
| Coefficients / Vectors / Filtered / QuarterLuma / Syntax / Bodies | 1 shared | N copies **if** real GPU overlap is required | `gpu.cpp:313`, `:326-330` |
| Rate (12 B) | 1 shared | **N slots** + host shadow | `gpu.cpp:324`, `scan.comp:48-63` |
| Command buffers / fences | 1 or 3 | N | `gpu.cpp:386-415` |
| Descriptor sets | (R+1) or (R+1)×3 | N × reconSlots (cheap) | `gpu.cpp:349-369` |
| Read-only code tables | 1 | shared | `gpu.cpp:325` |

---

## 3. Minimum multi-frame-in-flight design

### 3.1 Capacity

Let `R = references` (5 for `slow`, 16 for `veryslow`, `presets.hpp:14-15`),
`S = reconSlots`, `N = in-flight depth`.

- Recon write slots must satisfy **S ≥ N** (two concurrent frames cannot write
  one slot) and **S ≥ R+1** (the reference window must remain resident). So
  `S = max(N, R+1)`.
- Today `S = R+1`, so with `slow` the design caps at **N = 6**, with `veryslow`
  at **N = 17**. N=8 or N=16 on `slow` **requires DPB growth**.
- Growing `S` does *not* change decoder DPB signalling: `availableReferences()`
  stays `min(references, frameIndex % gop)` (`layout.glsl:89`), and the H.264
  level check uses `picture × references` (`presets.hpp:82-95`). Extra slots are
  encoder-internal reconstruction storage, not extra signalled references.
- Descriptor sets: generalise the existing formula to
  `setIndex = ringIndex * reconSlots + slot` with `ringIndex = frameIndex % N`
  and `slot = frameIndex % reconSlots`. That is `N × S` sets, e.g. 8×6=48 or
  16×17=272. Set memory is negligible; the descriptor pool must be sized
  `setTotal × 14` (`gpu.cpp:355`).
- Queue: `selectDevice()` records `queueCount` but `init()` hardcodes one queue
  (`gpu.cpp:200-208`, `:262`, `:268`). With one queue, N command buffers exist
  but the driver may serialise them; see §3.5.

### 3.2 What must be duplicated per frame

1. **CABAC state array.** Per frame, `sliceCount` state records, each = 460
   context bytes + `low/range/queue/outstanding` + byte cursor + `prevbyte`.
   At 1080p paired slices (34 slices, `gpu.cpp:588-591`) that is ~15.6 KB per
   frame; 8 frames ≈ 125 KB, 16 frames ≈ 250 KB. The IDR uses one slice but its
   context record must be sized for a whole-frame lane.
2. **Output slot** — N × (`scratchSize + 16`) host-visible buffers, as the ring
   already does (`gpu.cpp:317`).
3. **Slices scratch and Records** — N copies. `Slices` is the largest single
   item: ~53 MB for the paired-P layout and ~103 MB for the whole-frame-IDR
   layout at 1080p (`parallelBytes()`, `gpu.cpp:62-71`, `:270-287`).
4. **Rate slot** — N × 12 B, bound as binding 7 in the per-frame set.
5. **Other scratch** — Coefficients (~13.4 MiB), Bodies (~23.9 MiB), Filtered
   (~8.1 MiB), Vectors (~0.75 MiB), QuarterLuma (~0.5 MiB), Syntax (~0.06 MiB)
   per frame. These must be duplicated only if frames truly execute
   concurrently; if the queue serialises them, duplication is wasted memory.
6. **Command buffers + fences** — N, created with
   `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT` as the ring already does
   (`gpu.cpp:389`, `:396-410`).

Worst-case per-frame allocation (IDR-sized Slices + mirror Output + Bodies +
Coefficients + Filtered + one recon slot) is ~246 MiB; N=8 ≈ 1.9 GiB, N=16 ≈
3.8 GiB (`results/frame_parallel_estimates.json`). On a 6 GB GTX 1660 SUPER
this fits, but it is a first-class feasibility gate, not an afterthought.

### 3.3 Rate feedback ordered with an N-frame delay

Minimum host-side protocol:

1. At submit of frame `f`, read the host shadow RC (the state after the last
   *retired* frame) and seed `Rate[f % N]` with its two QP offsets and debt.
   Those values are the feedback from roughly frame `f-N`, hence the N-frame
   lag. No cross-frame GPU access to Rate remains.
2. `scan.comp` writes only its own slot; no other shader changes. Because all
   stages of frame `f` bind the same per-frame set, `frameQp()` is consistent
   across residual, deblocking, slice header (`bitstream.glsl:125`) and scan.
3. At retirement of frame `f` — the existing `beginFrame()` fence wait at
   `gpu.cpp:526-537` — read that slot's `bitDebt`/offsets **in decode order**,
   fold into the host shadow, then let later submissions seed from it. This is
   exactly the deferred-result pattern already used for Output, extended from
   3 to N slots. Output writing in `main.cpp:274-280` already happens on the
   returned (deferred) frame, so decode-order delivery is preserved.
4. The lag is bounded: with the ABR window of `fps/2` frames
   (`scan.comp:48`) and N ≤ 16, the controller reacts to 8–16-frame-old
   measurements. QP trajectory will differ from serial; bitstream identity is
   **not** achievable with this scheme. Gate on bitrate/VMAF, not on SHA.

Alternative (larger change, not minimum): move the whole RC decision to the
host with a bit predictor; then no GPU `scan.comp` feedback is needed at all,
but a new predictor has to be validated.

### 3.4 Reconstruction references stay correct

- Frame `f` writes recon into slot `f % S` (binding 2, `gpu.cpp:455`) and reads
  frame `f-1` through `pc.previousWords` plus older refs through
  `referenceOffset(index)`, `index ∈ [1, availableReferences())`
  (`layout.glsl:89-95`).
- A slot may be reused by frame `f+N`; all potential readers of `f`'s recon are
  `f+1 … f+R`. With `S ≥ max(N, R+1)`, `f+N` is the earliest frame that reuses
  the slot, so all readers are earlier submissions. Retire the fence for slot
  `i` before overwriting it — the existing `beginFrame()` wait
  (`gpu.cpp:526-528`) does exactly this when generalised to N.
- An IDR's recon must exist before the next P-frame's motion stage reads it.
  Same-queue decode-order submission plus the existing prologue barrier
  (`gpu.cpp:607-608`, `:636-638`) is the current mechanism; for a true
  multi-queue variant this needs an explicit event/semaphore chain from the
  IDR's deblock completion to the following frames' `Previous` reads.
- IDR residual already uses diagonal waves (`gpu.cpp:551-571`), so its
  reconstruction is not itself a 117 ms serial stage; the 117 ms is the
  *entropy* lane, which the following P frames do **not** depend on. This is
  the key reason frame-level parallelism can hide the IDR cost: P frames need
  the IDR's reconstructed pixels, not its compressed bytes.

### 3.5 Execution model — choose explicitly

**Model A (minimum, recommended first try).** Keep the single queue, submit N
command buffers in decode order, but duplicate all scratch as in §3.2 so
overlap is legal. Correctness is easy (queue order + per-slot fences); whether
the GPU actually overlaps stages is an empirical question. The existing ring
shows the host benefits even when the GPU does not; measure with timestamps
before investing further. Note `--timestamps` currently disables the ring
(`gpu.cpp:249`), so a timestamp-compatible variant must be added for this
measurement.

**Model B (if A shows no overlap).** Either (a) create `N` compute queues from
the selected family — `selectDevice()` already finds `queueCount`
(`gpu.cpp:194-210`), but `init()` requests only one (`gpu.cpp:262`) — and chain
them with semaphores for the recon dependencies and fences for RC retirement;
or (b) interleave N frames' dispatch chains inside one command buffer with
per-frame descriptor rebinds and only intra-frame barriers. (b) avoids
multi-queue limitations but makes the command buffer large and the barrier
bookkeeping delicate.

---

## 4. Throughput — measured numbers, not wishful thinking

### 4.1 Measured inputs

- 1920×1080 → `mbWidth = 120`, `mbHeight = 68`, **8160 MB**
  (`presets.hpp:56-57`, `gpu.hpp:9-12`).
- CABAC prototype: **14 µs/MB** serial per slice lane; 320×180/240 MB cost
  ~3.45 ms; **~117 ms** for a 1080p whole-frame IDR; P slices ~5–7 ms/frame;
  1080p/20f probe entropy 27.05 ms/frame, loop **23 fps** vs 113 baseline
  (`docs/quality_baseline.md:168-184`).
- CAVLC loop after parallel assembly: **100–112 fps** at 1080p, entropy
  0.09–0.11 ms/frame; earlier notes quote 150–160 fps on easier content
  (`quality_baseline.md:117-127`, `improvement_roadmap.md:18,52`).
- Ring: loop 119 → 132–140 fps, process 89 → 93–103 fps (commit `f69beff`).
- Default GOP is 5 s: 125 frames at 25 fps, 300 at 60 fps (`presets.hpp:64-68`).

### 4.2 Two bounds

**(a) IDR-lane latency bound.** One IDR entropy lane = `8160 × 14 µs =
114.24 ms`, matching the measured 117 ms within 2.4 %. With `N ≪ gop` there is
at most one IDR in any N-frame batch, so a batch completes no sooner than that
lane:

| N | IDR-lane bound | Comment |
|---:|---:|---|
| 8 | `8 / 0.117 = 68.4 fps` | 272 CABAC lanes; ~16 per SM on 22 SMs — fits |
| 16 | `16 / 0.117 = 136.8 fps` | 544 lanes; roughly two waves |

**(b) Work-conservation bound.** Frame parallelism hides *latency*, it does
not remove *work*. Per frame, CABAC is still 114 ms of single-lane work, i.e.
~5–7 ms of wall time when spread over 34 slice lanes on this GPU — the same
number the prototype measured. N frames need `34N` lanes, so the amortised
entropy cost per frame stays ~5–7 ms. Adding the non-entropy floor
`1/150…1/120 = 6.25…8.33 ms/frame` gives a hard ceiling of roughly
**65–87 fps**, unless CABAC occupies SMs that would otherwise be idle.

Combining (a) and (b), including the non-entropy batch cost
(`N × 6.25…8.33 ms` = 50–67 ms at N=8, 100–133 ms at N=16):

| N | Bound that dominates | Plausible loop fps |
|---:|---|---|
| 8 | IDR lane (117 ms) vs 50–67 ms other work | **60–68 fps** |
| 16 | IDR lane vs 100–133 ms other work | **105–120 fps** |

N=16 reaches the 100 fps loop gate only at the optimistic end of the
non-entropy range and only if the recon pool and ~3.8 GiB of scratch are
provided. N=8 does **not** reach 100 fps. These are ceilings, not promises:
host RC folding (~0.62 ms/frame measured for all other host work, `f69beff`)
and extra descriptor/barrier work will eat into them.

### 4.3 The IDR bottleneck and multiple IDRs

- The 117 ms is one entropy *lane*, not a whole-frame dependency graph. P
  frames need the IDR's reconstruction (available after the residual/deblock
  waves), not its compressed output. Overlapping that lane with following P
  frames is the entire point of frame-level parallelism here.
- **Multiple IDRs in flight is not a practical lever.** With GOP 125–300 and
  N = 8–16, a batch contains at most one IDR. Two IDRs require `N ≥ gop`
  (≥125 frames), which is far beyond the recon/scratch budget and the RC lag
  tolerance. Do not design for it.
- Without frame overlap the IDR forms a 117 ms bubble each GOP: at GOP 125 and
  ~12 ms other work, `125/(117 + 124×12) ≈ 78 fps`. With perfect overlap the
  bubble disappears but total work remains; the gain is ~8–12 %, consistent
  with the bounds above — not a 5× win.

### 4.4 Memory feasibility

`results/frame_parallel_estimates.json` gives the byte-by-byte arithmetic.
Per-frame worst case ~246 MiB (IDR-sized `Slices` + its Output mirror + Bodies
+ Coefficients + Filtered + recon slot); 8 frames ≈ 1.9 GiB, 16 frames ≈ 3.8
GiB. Coefficients and Bodies alone are ~37 MiB per frame. This is feasible on
6 GB but leaves little room for the driver and the existing shared buffers;
size `Slices` per ring slot for the largest frame type that can occupy it or
accept a lower N.

---

## 5. Minimum host / DPB changes

1. `FrameConfig` (`gpu.hpp:14-23`, push-constant ABI `layout.glsl:2-12`): add
   `reconSlots` and `inFlight`; keep `references` as the reference window.
   Update `referenceOffset()` (`layout.glsl:91-95`) to use `pc.reconSlots`, and
   keep `availableReferences()` unchanged. Update the
   `sizeof(FrameConfig) == 29 * sizeof(uint32_t)` assertion (`gpu.cpp:45`).
2. Generalise `ringSlots` 3 → N (`gpu.cpp:101`), allocate N command buffers,
   fences and per-slot Upload/Source/Output plus per-slot scratch and Rate.
   Extend `updateDescriptors()` (`gpu.cpp:425-464`) so bindings 0, 3–7 and
   9–13 select the slot buffer while bindings 1–2 keep the recon pool/slot view.
3. Recon pool to `referenceStride * S` and re-check
   `maxStorageBufferRange` (`gpu.cpp:309-312`). Keep the level check driven by
   `references`, not by S (`presets.hpp:82-95`).
4. Add per-frame CABAC state buffers and the read-only `cabac_contexts` upload
   (`cabac.c:30-46`); new binding(s) in the shared layout, initialised per
   slice from slice type/QP/init-idc as in `encoder.c:2798`. Give each slice
   lane a private byte cursor and run emulation prevention only after its
   flush, mirroring `slice_epb[0..2]` (`gpu.cpp:379-382`, `:665-670`).
5. Host shadow RC + deferred retirement (extend `gpu.cpp:522-548`); seed each
   frame's Rate slot at submit, fold at retire.
6. Insert cross-submission ordering for IDR recon → next frame reads; consider
   multiple queues only after Model A is measured.
7. Add a timestamp-capable in-flight mode, since `gpu.cpp:249` currently
   disables the ring whenever timestamps are requested.

No B-frames, lookahead or display reorder are required: frames are submitted
and returned in decode order; the deferred-return pattern already provides
that.

---

## 6. Biggest risks

**Risk to the objective (biggest):** work conservation. At 14 µs/MB the
entropy work is ~114 ms of single-lane time per frame regardless of how many
frames are in flight. Frame parallelisation can hide the 117 ms IDR outlier
and fill idle SMs, but the combined non-entropy + entropy work bound is
~65–87 fps. N=16 may clear 100 fps only under optimistic assumptions; N=8 will
not. This is a measured bound, not a guess, and it agrees with the parent's
own "not viable under 100 fps gate" conclusion
(`improvement_roadmap.md:77`).

**Risk to correctness (biggest):** CABAC carry plus RC lag. Carry retroactively
edits `p[-1]` and defers `0xFF` bytes (`cabac.c:69-99`), so a lane's output
slice must be private and finalised in the right order. Separately, the
N-frame RC lag necessarily changes the QP trajectory, so the result cannot be
bit-identical to serial; any quality/rate regression is attributable to that
lag and must be measured before the design is accepted.

Secondary risks: one-queue serialisation giving zero GPU speedup (the existing
ring is host-only); 3.8 GiB scratch pressure at N=16; descriptor-pool growth to
N×S sets; timestamp instrumentation being incompatible with the current ring.

---

## 7. Implementation plan with feasibility gates

**Gate 0 — static capacity (no code).** Confirm `S = max(N, R+1)`, descriptor
count `N×S`, and scratch bytes from `results/frame_parallel_estimates.json`.
*Pass if* N=8/16 fits within 6 GB with ≥20 % headroom; otherwise reduce N or
size `Slices` per slot type.

**Gate 1 — host-only N-deep ring, no CABAC.** Generalise `ringSlots` 3→N with
duplicated Output/Source/Upload and per-frame Rate + host shadow; keep CAVLC.
Measure loop and process fps plus bitrate/VMAF. *Pass if* no throughput
regression vs the 3-slot ring and rate error stays within ±5 %. This isolates
RC lag. If it fails, frame parallelism is not viable with the current closed
loop and should be abandoned before CABAC work begins.

**Gate 2 — GPU overlap measurement.** Duplicate the rest of the scratch and
add a timestamp-capable in-flight mode (§5.7). Compare stage intervals of
frames `f` and `f+1`. *Pass if* intervals overlap materially. If not, try
Model B (multiple queues or interleaved dispatch). If still not, stop: no
speedup is available on this device.

**Gate 3 — single CABAC lane correctness.** Implement one slice lane with x264
tables, carry and emulation prevention; validate byte-identical output on the
320×180 fixtures (240 MB) and with `ffmpeg -err_detect explode`. *Pass if*
exact; the earlier prototype failed here (`quality_baseline.md:177-178`), so
this gate is mandatory before any parallelism work.

**Gate 4 — state isolation.** Add per-frame CABAC state buffers, N=2, run with
`targetBits = 0` so `scan.comp:30` disables RC. Two in-flight frames must
produce exactly the same two bitstreams as two serial runs. *Pass if*
byte-identical — this proves frame independence without RC confounder.

**Gate 5 — RC integration.** Enable ABR with N-frame lag; measure 1080p/300f at
6000/8000 kb/s against serial. *Pass if* within ±5 % bitrate and ≥ −0.3 VMAF;
otherwise redesign RC (host predictor) or lower N.

**Gate 6 — IDR case.** Run a GOP containing an IDR followed by ≥N P frames;
measure IDR entropy interval overlap and end-to-end loop fps. *Target*
60–68 fps at N=8, 105–120 fps at N=16 (§4.2). Record the actual number even if
it misses the 100 fps gate; a negative result here is decisive.

**Gate 7 — capacity/robustness.** Peak VRAM, descriptor count, fence retirement
order, and a decode check across ≥3 IDRs. *Pass if* no OOM, no stale reference,
clean decode.

---

## 8. What I did not verify

- Whether this Vulkan driver actually overlaps compute dispatches from separate
  submissions on one queue (Gate 2 exists for this).
- The prototype's exact workgroup shape (threads vs. active lane), which
  decides SM occupancy; 14 µs/MB was used as given.
- Quality delta from N-frame RC lag; no experiment was run for this note.
- Whether `cabac_init_idc` and slice-header bits were ever made conformant in
  the reverted prototype; the parent records that they were not
  (`quality_baseline.md:177-178`).
