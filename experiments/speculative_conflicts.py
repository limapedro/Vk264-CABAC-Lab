#!/usr/bin/env python3
"""Standalone x264-CABAC-compatible simulator for speculative-chunk conflicts.

Reads the numeric CABAC tables from the PARENT repo read-only (never writes to
it), reimplements x264_cabac_encode_* from common/cabac.c:53-181, and counts
how often a chunk started from a *predicted* state disagrees with the true
serial state at the same bin position.

Usage:
  python experiments/speculative_conflicts.py --out results/speculative_conflicts.json

NOTE: the bin stream is synthetic but driven by x264's own state transition
tables and a realistic mix of H.264 context classes.  It is NOT a decoded
bitstream: no bin-level CABAC trace is available from ffmpeg or x264 CLI.
"""

from __future__ import annotations

import argparse
import json
import random
import re
from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_TABLES = Path(r"E:\__PROJECTS__\Vk264\x264-vulkan\common\tables.c")


# ---------------------------------------------------------------------------
# Table loading (numeric data only; no source copied into this repository)
# ---------------------------------------------------------------------------

def _numbers(text: str) -> list[int]:
    return [int(x) for x in re.findall(r"-?\d+", text)]


def load_tables(path: Path) -> dict:
    src = path.read_text(encoding="utf-8", errors="replace")

    def grab(name: str, pattern: str) -> str:
        m = re.search(pattern, src, re.S)
        if not m:
            raise RuntimeError(f"cannot find table {name} in {path}")
        return m.group(1)

    lps = _numbers(grab("range_lps", r"x264_cabac_range_lps\[64\]\[4\]\s*=\s*\{(.*?)\};"))
    trans = _numbers(grab("transition", r"x264_cabac_transition\[128\]\[2\]\s*=\s*\{(.*?)\};"))
    shift = _numbers(grab("renorm", r"x264_cabac_renorm_shift\[64\]\s*=\s*\{(.*?)\};"))
    assert len(lps) == 256 and len(trans) == 256 and len(shift) == 64
    return {
        "range_lps": [lps[i * 4:(i + 1) * 4] for i in range(64)],
        "transition": [trans[i * 2:(i + 1) * 2] for i in range(128)],
        "renorm_shift": shift,
    }


def initial_contexts(qp: int, tables: dict, slice_i: bool = True) -> list[int]:
    """Context states at slice start, from cabac.c:32-51."""
    # x264_cabac_context_init_I / PB[0]: first 460 entries used for 4:2:0.
    # Initialisation constants are loaded lazily from tables.c below.
    return _INIT_CACHE[(slice_i, qp)]


_INIT_CACHE: dict[tuple[bool, int], list[int]] = {}


def load_init_tables(path: Path) -> None:
    src = path.read_text(encoding="utf-8", errors="replace")
    specs = [
        (r"x264_cabac_context_init_I\[1024\]\[2\]\s*=\s*\{(.*?)\};", True, 0),
        (r"x264_cabac_context_init_PB\[3\]\[1024\]\[2\]\s*=\s*\{(.*?)\n\};", False, 1),
    ]
    for pattern, is_i, model in specs:
        m = re.search(pattern, src, re.S)
        if not m:
            raise RuntimeError(f"cannot find table for {pattern}")
        vals = _numbers(m.group(1))
        assert len(vals) >= (model + 1) * 1024 * 2, len(vals)
        vals = vals[model * 1024 * 2:]
        for qp in range(52):
            state = []
            for j in range(460):
                a, b = vals[j * 2], vals[j * 2 + 1]
                s = max(1, min(126, ((a * qp) >> 4) + b))
                state.append((min(s, 127 - s) << 1) | (s >> 6))
            _INIT_CACHE[(is_i, qp)] = state


# ---------------------------------------------------------------------------
# x264-compatible arithmetic coder (common/cabac.c:53-181)
# ---------------------------------------------------------------------------

@dataclass
class Coder:
    low: int = 0
    rng: int = 0x01FE
    queue: int = -9
    outstanding: int = 0
    pos: int = 0
    out: bytearray = field(default_factory=bytearray)

    def copy(self) -> "Coder":
        return Coder(self.low, self.rng, self.queue, self.outstanding,
                     self.pos, bytearray(self.out))

    def putbyte(self) -> int | None:
        """Returns carry bit applied to the previous byte, or None."""
        if self.queue < 0:
            return None
        out = self.low >> (self.queue + 10)
        self.low &= (0x400 << self.queue) - 1
        self.queue -= 8
        if (out & 0xFF) == 0xFF:
            self.outstanding += 1
            return None
        carry = out >> 8
        # x264 cabac.c:89 patches the previously emitted byte in place.
        if carry and self.pos > 0:
            self.out[self.pos - 1] = (self.out[self.pos - 1] + carry) & 0xFF
        while self.outstanding > 0:
            self.out.append((carry - 1) & 0xFF)
            self.pos += 1
            self.outstanding -= 1
        self.out.append(out & 0xFF)
        self.pos += 1
        return carry

    def renorm(self, tables: dict) -> int | None:
        shift = tables["renorm_shift"][self.rng >> 3]
        self.rng <<= shift
        self.low <<= shift
        self.queue += shift
        return self.putbyte()

    def decide(self, state: list[int], ctx: int, b: int, tables: dict) -> int | None:
        s = state[ctx]
        lps = tables["range_lps"][s >> 1][(self.rng >> 6) - 4]
        self.rng -= lps
        if b != (s & 1):
            self.low += self.rng
            self.rng = lps
        state[ctx] = tables["transition"][s][b]
        return self.renorm(tables)

    def bypass(self, b: int) -> int | None:
        self.low <<= 1
        self.low += b & self.rng
        self.queue += 1
        return self.putbyte()

    def terminal(self, tables: dict) -> int | None:
        self.rng -= 2
        return self.renorm(tables)


# ---------------------------------------------------------------------------
# Bin streams
# ---------------------------------------------------------------------------

# Context classes with plausible H.264 usage weights (mb_type/skip, CBP,
# chroma prediction, significant-coeff flags, last-coeff flags, coeff levels).
REALISTIC_CLASSES = [
    (list(range(0, 16)), 16),      # mb_type / mb_skip / sub_mb_type
    (list(range(64, 76)), 10),     # intra chroma / cbp
    (list(range(84, 100)), 22),    # significant coeff flags (luma)
    (list(range(100, 112)), 14),   # last significant coeff flags
    (list(range(112, 126)), 18),   # coeff level abs-minus-1
    (list(range(400, 424)), 20),   # high-frequency residual contexts
]


def make_bins(nbins: int, qp: int, tables: dict, seed: int = 264,
              bypass_frac: float = 0.18) -> list[tuple[int, int]]:
    """(context index, bin value); ctx=-1 marks a bypass bin."""
    rng = random.Random(seed)
    ctxs = initial_contexts(qp, tables, True)[:]
    pool, weights = [], []
    for contexts, w in REALISTIC_CLASSES:
        for c in contexts:
            pool.append(c)
            weights.append(w)
    bins: list[tuple[int, int]] = []
    for _ in range(nbins):
        if rng.random() < bypass_frac:
            bins.append((-1, rng.randrange(2)))
            continue
        ctx = rng.choices(pool, weights=weights, k=1)[0]
        s = ctxs[ctx]
        p_mps = max(0.02, min(0.98, (s >> 1) / 64.0))
        b = (s & 1) if rng.random() < p_mps else (s & 1) ^ 1
        ctxs[ctx] = tables["transition"][s][b]
        bins.append((ctx, b))
    return bins


# ---------------------------------------------------------------------------
# Serial ground truth
# ---------------------------------------------------------------------------

def serial_encode(bins, ctx0, tables, chunk: int):
    c = Coder()
    ctx = ctx0[:]
    snapshots = []
    stats = {
        "carry_cross_chunk": 0,
        "chunks_with_no_byte": 0,
        "chunks_with_pending_ff_at_start": 0,
        "max_outstanding": 0,
    }
    emitted_in_chunk = 0
    chunk_positions = []
    for i, (ci, b) in enumerate(bins):
        k = i // chunk
        if i % chunk == 0:
            snapshots.append((c.low, c.rng, c.queue, c.outstanding, ctx[:]))
            chunk_positions.append(c.pos)
            emitted_in_chunk = 0
            if c.outstanding > 0:
                stats["chunks_with_pending_ff_at_start"] += 1
        before_pos = c.pos
        before_outstanding = c.outstanding
        if ci < 0:
            carry = c.bypass(b)
        else:
            carry = c.decide(ctx, ci, b, tables)
        stats["max_outstanding"] = max(stats["max_outstanding"], c.outstanding)
        if c.pos > before_pos:
            if emitted_in_chunk == 0 and carry:
                # The first byte emitted by this chunk has a carry that
                # retroactively edits the last byte written by the previous
                # chunk (this is the cross-chunk carry dependency).
                stats["carry_cross_chunk"] += 1
            emitted_in_chunk += c.pos - before_pos
    chunk_positions.append(c.pos)
    stats["chunks_with_no_byte"] = sum(
        chunk_positions[k] == chunk_positions[k + 1]
        for k in range(len(snapshots))
    )
    return c, snapshots, stats


def verify_chunk_merge(bins, snapshots, tables, chunk: int) -> dict:
    """Encode each 64-bin chunk in isolation and merge with a 1-bit carry.

    This demonstrates that the ONLY byte-stream dependency between chunks is a
    single carry bit that patches the previous byte; all other arithmetic state
    can be checkpointed as (low, range, queue, outstanding, contexts).
    """
    merged = bytearray()
    carry_cross = 0
    for k, (low, rng, queue, outstanding, true_ctx) in enumerate(snapshots):
        ctx = true_ctx[:]
        c = Coder(low, rng, queue, outstanding, 0, bytearray())
        local_carry_out = None
        for ci, b in bins[k * chunk:min((k + 1) * chunk, len(bins))]:
            before = c.pos
            if ci < 0:
                carry = c.bypass(b)
            else:
                carry = c.decide(ctx, ci, b, tables)
            if carry is not None and local_carry_out is None and c.pos > before:
                local_carry_out = carry
        if local_carry_out and merged:
            merged[-1] = (merged[-1] + 1) & 0xFF
            carry_cross += 1
        merged.extend(c.out)
    serial, _, _ = serial_encode(bins, snapshots[0][4], tables, chunk)
    return {
        "chunk_bins": chunk,
        "merged_bytes": len(merged),
        "serial_bytes": len(serial.out),
        "bit_exact": bytes(merged) == bytes(serial.out),
        "cross_chunk_carry_edits": carry_cross,
    }


# ---------------------------------------------------------------------------
# Speculation models
# ---------------------------------------------------------------------------

def count_conflicts(bins, snapshots, chunk: int, ctx0, mode: str, checkpoint: int = 8):
    """Count chunk starts where predicted state differs from the true state."""
    ctx_conflicts = arith_conflicts = 0
    touched_total = 0
    for k, (low, rng, queue, outstanding, true_ctx) in enumerate(snapshots):
        start = k * chunk
        end = min(start + chunk, len(bins))
        touched = {ci for ci, _ in bins[start:end] if ci >= 0}
        touched_total += len(touched)
        if mode == "naive_init":
            pred = ctx0
        elif mode == "stale":
            pred = snapshots[max(0, k - checkpoint)][4]
        else:
            raise ValueError(mode)
        if any(pred[ci] != true_ctx[ci] for ci in touched):
            ctx_conflicts += 1
        # Arithmetic-state mismatch: low/range/queue/outstanding must all match
        # for the chunk's emitted bytes to be reusable.  A carry error is a
        # 1-bit retroactive edit to the last byte of the previous chunk.
        if mode == "naive_init":
            pred_arith = (0, 0x01FE, -9, 0)
        else:
            pl, pr, pq, po, _ = snapshots[max(0, k - checkpoint)]
            pred_arith = (pl, pr, pq, po)
        if pred_arith != (low, rng, queue, outstanding):
            arith_conflicts += 1
    return {
        "mode": mode,
        "checkpoint_chunks": None if mode == "naive_init" else checkpoint,
        "chunks": len(snapshots),
        "context_conflicts": ctx_conflicts,
        "arith_conflicts": arith_conflicts,
        "context_conflict_rate": round(ctx_conflicts / len(snapshots), 4),
        "arith_conflict_rate": round(arith_conflicts / len(snapshots), 4),
        "mean_distinct_contexts_per_chunk": round(touched_total / len(snapshots), 2),
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tables", type=Path, default=DEFAULT_TABLES)
    ap.add_argument("--bins", type=int, default=4096)
    ap.add_argument("--chunk", type=int, default=64)
    ap.add_argument("--qp", type=int, default=26)
    ap.add_argument("--seed", type=int, default=264)
    ap.add_argument("--out", type=Path,
                    default=Path(__file__).resolve().parents[1] / "results" / "speculative_conflicts.json")
    args = ap.parse_args()

    load_init_tables(args.tables)
    tables = load_tables(args.tables)
    bins = make_bins(args.bins, args.qp, tables, args.seed)
    ctx0 = initial_contexts(args.qp, tables, True)

    report = {
        "source": {
            "tables": str(args.tables),
            "bin_stream": "synthetic x264-state-driven (no bin-level decoder available)",
            "bins": args.bins,
            "qp": args.qp,
            "seed": args.seed,
        },
        "experiments": [],
    }
    for chunk in (args.chunk, 8, 16, 32, 128):
        if chunk in [e["chunk_bins"] for e in report["experiments"]]:
            continue
        coder, snapshots, stats = serial_encode(bins, ctx0, tables, chunk)
        entry = {
            "chunk_bins": chunk,
            "serial": {
                "bytes": len(coder.out),
                "outstanding_at_end": coder.outstanding,
                "sha256_of_bytes": __import__("hashlib").sha256(coder.out).hexdigest(),
            },
            "carry_and_byte_stats": stats,
            "chunk_merge_verification": verify_chunk_merge(bins, snapshots, tables, chunk),
            "conflicts": [],
        }
        for mode, cp in (("naive_init", 0), ("stale", 1), ("stale", 4), ("stale", 16)):
            entry["conflicts"].append(count_conflicts(
                bins, snapshots, chunk, ctx0, mode, cp))
        report["experiments"].append(entry)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
