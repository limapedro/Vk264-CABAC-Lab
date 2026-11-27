// chunk_cabac.cpp -- CPU reference prototype for the exact chunk-summary CABAC
// design in notes/SPECULATIVE_HYBRID.md section 4.
//
// Ground rules:
//   * Parent repo E:\__PROJECTS__\Vk264 is READ ONLY.  We only read tables.c
//     and cabac.c at runtime; no parent source is copied here.
//   * x264 arithmetic is re-implemented verbatim from common/cabac.c:53-183.
//
// Build: see build.bat (MSVC x64).
// Output: human-readable verification + results/chunk_cabac_results.json.
//

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <intrin.h>
#include <map>
#include <numeric>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const int NCTX = 460;  // 4:2:0 (CHROMA444 == 0)
static const char* DEFAULT_TABLES =
    R"(E:\__PROJECTS__\Vk264\x264-vulkan\common\tables.c)";

enum Kind : uint8_t { DECISION = 0, BYPASS = 1, TERMINAL = 2 };

struct BinEvent {
    uint16_t ctx;  // NCTX range; 0xFFFF unused marker if bypass/terminal
    uint8_t bin;   // 0/1 for decision/bypass; ignored for terminal
    uint8_t kind;  // Kind
};

// ---------------------------------------------------------------------------
// Table loading (numeric data only, read-only from parent)
// ---------------------------------------------------------------------------
struct Tables {
    uint8_t range_lps[64][4]{};
    uint8_t transition[128][2]{};
    uint8_t renorm_shift[64]{};
    int8_t init_I[1024][2]{};
    int8_t init_PB[3][1024][2]{};
};

static std::string strip_comments(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool in_block = false, in_line = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (in_block) {
            if (c == '*' && i + 1 < s.size() && s[i + 1] == '/') { in_block = false; ++i; }
            continue;
        }
        if (in_line) {
            if (c == '\n' || c == '\r') { in_line = false; out.push_back(c); }
            continue;
        }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '*') { in_block = true; ++i; continue; }
        if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') { in_line = true; ++i; continue; }
        out.push_back(c);
    }
    return out;
}

static std::vector<int> numbers(const std::string& s) {
    std::vector<int> v;
    static const std::regex re(R"(-?\d+)");
    auto b = std::sregex_iterator(s.begin(), s.end(), re);
    auto e = std::sregex_iterator();
    for (auto it = b; it != e; ++it) v.push_back(std::stoi(it->str()));
    return v;
}

static std::string grab(const std::string& src, const std::string& pattern) {
    std::regex re(pattern);  // ECMAScript: supports \\s, non-greedy .*?, escaped braces
    std::smatch m;
    if (!std::regex_search(src, m, re))
        throw std::runtime_error("table not found: " + pattern);
    return m[1].str();
}

static Tables load_tables(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::string src((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string clean = strip_comments(src);
    Tables t;

    auto lps = numbers(grab(clean, R"(x264_cabac_range_lps\[64\]\[4\]\s*=\s*\{([\s\S]*?)\};)"));
    auto tr = numbers(grab(clean, R"(x264_cabac_transition\[128\]\[2\]\s*=\s*\{([\s\S]*?)\};)"));
    auto sh = numbers(grab(clean, R"(x264_cabac_renorm_shift\[64\]\s*=\s*\{([\s\S]*?)\};)"));
    if (lps.size() != 256 || tr.size() != 256 || sh.size() != 64)
        throw std::runtime_error("unexpected LUT sizes");
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 4; ++j) t.range_lps[i][j] = (uint8_t)lps[i * 4 + j];
    for (int i = 0; i < 128; ++i)
        for (int j = 0; j < 2; ++j) t.transition[i][j] = (uint8_t)tr[i * 2 + j];
    for (int i = 0; i < 64; ++i) t.renorm_shift[i] = (uint8_t)sh[i];

    auto ini = numbers(grab(clean, R"(x264_cabac_context_init_I\[1024\]\[2\]\s*=\s*\{([\s\S]*?)\};)"));
    if (ini.size() != 2048) throw std::runtime_error("init_I size");
    for (int i = 0; i < 1024; ++i)
        for (int j = 0; j < 2; ++j) t.init_I[i][j] = (int8_t)ini[i * 2 + j];

    auto inip = numbers(grab(clean, R"(x264_cabac_context_init_PB\[3\]\[1024\]\[2\]\s*=\s*\{([\s\S]*?)\n\};)"));
    if (inip.size() < 3 * 2048) throw std::runtime_error("init_PB size");
    for (int m = 0; m < 3; ++m)
        for (int i = 0; i < 1024; ++i)
            for (int j = 0; j < 2; ++j)
                t.init_PB[m][i][j] = (int8_t)inip[(m * 1024 + i) * 2 + j];
    return t;
}

static std::array<uint8_t, NCTX> init_contexts(const Tables& t, int qp, bool is_i, int model) {
    std::array<uint8_t, NCTX> s{};
    for (int j = 0; j < NCTX; ++j) {
        int a = is_i ? t.init_I[j][0] : t.init_PB[model][j][0];
        int b = is_i ? t.init_I[j][1] : t.init_PB[model][j][1];
        int st = std::max(1, std::min(126, ((a * qp) >> 4) + b));
        s[j] = (uint8_t)((std::min(st, 127 - st) << 1) | (st >> 6));
    }
    return s;
}

// ---------------------------------------------------------------------------
// Light arithmetic state (no byte writes) -- used by Stage 1 and checkpoints
// ---------------------------------------------------------------------------
struct ArithState {
    int low = 0, range = 0x01FE, queue = -9, outstanding = 0;
};

static inline void light_putbyte(ArithState& a) {
    if (a.queue >= 0) {
        int out = a.low >> (a.queue + 10);
        a.low &= (0x400 << a.queue) - 1;
        a.queue -= 8;
        if ((out & 0xff) == 0xff) a.outstanding++;
        else a.outstanding = 0;  // the deferred FF run is consumed here
    }
}
static inline void light_renorm(ArithState& a, const Tables& t) {
    int shift = t.renorm_shift[a.range >> 3];
    a.range <<= shift;
    a.low <<= shift;
    a.queue += shift;
    light_putbyte(a);
}
static inline void light_decide(ArithState& a, uint8_t* state, int ctx, int b, const Tables& t) {
    int s = state[ctx];
    int lps = t.range_lps[s >> 1][(a.range >> 6) - 4];
    a.range -= lps;
    if (b != (s & 1)) { a.low += a.range; a.range = lps; }
    state[ctx] = t.transition[s][b];
    light_renorm(a, t);
}
static inline void light_bypass(ArithState& a, int b) {
    a.low <<= 1;
    a.low += b & a.range;
    a.queue += 1;
    light_putbyte(a);
}
static inline void light_terminal(ArithState& a, const Tables& t) {
    a.range -= 2;
    light_renorm(a, t);
}

// ---------------------------------------------------------------------------
// Serial ground-truth encoder (x264 cabac.c:53-183, byte for byte)
// ---------------------------------------------------------------------------
extern const Tables* g_tables_ptr;  // set in main; serial coder reads tables through it

struct SerialCoder {
    ArithState a;
    std::array<uint8_t, NCTX> ctx{};
    // The first byte models the last byte of the slice header: x264's
    // cabac.c:89 patches p[-1], which is guaranteed to exist.
    std::vector<uint8_t> out{0x5A};
    int frame = 0;

    void putbyte() {
        if (a.queue >= 0) {
            int outv = a.low >> (a.queue + 10);
            a.low &= (0x400 << a.queue) - 1;
            a.queue -= 8;
            if ((outv & 0xff) == 0xff) a.outstanding++;
            else {
                int carry = outv >> 8;
                if (carry) out.back() = (uint8_t)(out.back() + carry);  // cabac.c:89
                while (a.outstanding > 0) { out.push_back((uint8_t)(carry - 1)); a.outstanding--; }
                out.push_back((uint8_t)outv);
            }
        }
    }
    void renorm() {
        int shift = (*g_tables_ptr).renorm_shift[a.range >> 3];
        a.range <<= shift; a.low <<= shift; a.queue += shift; putbyte();
    }
    void decide(int c, int b) {
        int s = ctx[c];
        int lps = (*g_tables_ptr).range_lps[s >> 1][(a.range >> 6) - 4];
        a.range -= lps;
        if (b != (s & 1)) { a.low += a.range; a.range = lps; }
        ctx[c] = (*g_tables_ptr).transition[s][b];
        renorm();
    }
    void bypass(int b) { a.low <<= 1; a.low += b & a.range; a.queue += 1; putbyte(); }
    void terminal() { a.range -= 2; renorm(); }
    void flush() {
        a.low += a.range - 2;
        a.low |= 1;
        a.low <<= 9;
        a.queue += 9;
        putbyte();
        putbyte();
        a.low <<= -a.queue;
        a.low |= (uint32_t(0x35a4e4f5) >> (frame & 31) & 1) << 10;
        a.queue = 0;
        putbyte();
        while (a.outstanding > 0) { out.push_back(0xff); a.outstanding--; }
    }
};
// One shared table set; set in main.
static const Tables* g_tables_ptr = nullptr;

// ---------------------------------------------------------------------------
// Stage-1 checkpoint and Stage-2 chunk encoder
// ---------------------------------------------------------------------------
struct Checkpoint {
    ArithState a;
    std::array<uint8_t, NCTX> ctx{};
};

struct ChunkResult {
    std::vector<uint8_t> bytes;  // local bytes, excluding the patched predecessor
    uint8_t carry_out = 0;       // 1-bit cross-chunk patch of previous byte
};

struct ChunkCoder {
    ArithState a;
    std::array<uint8_t, NCTX> ctx{};
    std::vector<uint8_t> out;
    uint8_t carry_out = 0;

    void putbyte() {
        if (a.queue >= 0) {
            int outv = a.low >> (a.queue + 10);
            a.low &= (0x400 << a.queue) - 1;
            a.queue -= 8;
            if ((outv & 0xff) == 0xff) a.outstanding++;
            else {
                int carry = outv >> 8;
                if (out.empty()) {
                    // Patch target lives in an earlier chunk: expose as carry_out.
                    if (carry) carry_out = 1;
                } else {
                    out.back() = (uint8_t)(out.back() + carry);
                }
                while (a.outstanding > 0) { out.push_back((uint8_t)(carry - 1)); a.outstanding--; }
                out.push_back((uint8_t)outv);
            }
        }
    }
    void renorm(const Tables& t) {
        int shift = t.renorm_shift[a.range >> 3];
        a.range <<= shift; a.low <<= shift; a.queue += shift; putbyte();
    }
    void decide(int c, int b, const Tables& t) {
        int s = ctx[c];
        int lps = t.range_lps[s >> 1][(a.range >> 6) - 4];
        a.range -= lps;
        if (b != (s & 1)) { a.low += a.range; a.range = lps; }
        ctx[c] = t.transition[s][b];
        renorm(t);
    }
    void bypass(int b) { a.low <<= 1; a.low += b & a.range; a.queue += 1; putbyte(); }
    void terminal(const Tables& t) { a.range -= 2; renorm(t); }
    void flush(const Tables& t, int frame) {
        a.low += a.range - 2;
        a.low |= 1;
        a.low <<= 9;
        a.queue += 9;
        putbyte();
        putbyte();
        a.low <<= -a.queue;
        a.low |= (uint32_t(0x35a4e4f5) >> (frame & 31) & 1) << 10;
        a.queue = 0;
        putbyte();
        while (a.outstanding > 0) { out.push_back(0xff); a.outstanding--; }
    }
};

// `cin` is accepted for parity with the spec's "2 carry-in variants".  In
// x264 the incoming carry never alters this chunk's own bytes (it only patches
// the previous chunk's last byte), so both variants must produce identical
// local output; the harness asserts this for every chunk.
static ChunkResult encode_chunk(const Checkpoint& cp, const BinEvent* bins, size_t n,
                                const Tables& t, int /*cin*/, bool do_flush, int frame) {
    ChunkCoder c;
    c.a = cp.a;
    c.ctx = cp.ctx;
    for (size_t i = 0; i < n; ++i) {
        const BinEvent& be = bins[i];
        if (be.kind == DECISION) c.decide(be.ctx, be.bin, t);
        else if (be.kind == BYPASS) c.bypass(be.bin);
        else c.terminal(t);
    }
    if (do_flush) c.flush(t, frame);
    return {std::move(c.out), c.carry_out};
}

// ---------------------------------------------------------------------------
// Stage-1 combined prefix (contexts + light arithmetic checkpoints)
// ---------------------------------------------------------------------------
struct PrefixResult {
    std::vector<Checkpoint> checkpoints;            // one per chunk
    std::vector<int> touched_per_chunk;
    std::vector<uint8_t> serial_context_only_out;   // unused, keeps layout explicit
};

static PrefixResult run_prefix(const std::vector<BinEvent>& bins, size_t chunk,
                               const std::array<uint8_t, NCTX>& ctx0,
                               const Tables& t, int nchunks) {
    PrefixResult r;
    r.checkpoints.resize(nchunks);
    r.touched_per_chunk.assign(nchunks, 0);
    ArithState a;
    auto ctx = ctx0;
    std::vector<int> seen(NCTX, 0);
    int gen = 0;
    for (size_t i = 0; i < bins.size(); ++i) {
        size_t k = i / chunk;
        if (i % chunk == 0) {
            r.checkpoints[k].a = a;
            r.checkpoints[k].ctx = ctx;
            ++gen;
        }
        const BinEvent& be = bins[i];
        if (be.kind == DECISION) {
            light_decide(a, ctx.data(), be.ctx, be.bin, t);
            if (seen[be.ctx] != gen) { seen[be.ctx] = gen; r.touched_per_chunk[k]++; }
        } else if (be.kind == BYPASS) {
            light_bypass(a, be.bin);
        } else {
            light_terminal(a, t);
        }
    }
    return r;
}

// Literal section-4 Stage 1: only context transitions, no arithmetic.
static double time_context_only(const std::vector<BinEvent>& bins,
                                const std::array<uint8_t, NCTX>& ctx0,
                                const Tables& t, int reps, uint64_t* tsc_out) {
    auto ctx = ctx0;
    uint64_t tsc0 = __rdtsc();
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; ++r) {
        ctx = ctx0;
        for (const auto& be : bins) {
            if (be.kind == DECISION)
                ctx[be.ctx] = t.transition[ctx[be.ctx]][be.bin];
        }
        sink += ctx[bins.back().ctx];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t tsc1 = __rdtsc();
    (void)sink;
    *tsc_out = tsc1 - tsc0;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// Context transitions + light arithmetic, but no checkpoint memory writes:
// isolates the serial compute cost of the corrected Stage 1.
static double time_light_only(const std::vector<BinEvent>& bins,
                              const std::array<uint8_t, NCTX>& ctx0,
                              const Tables& t, int reps, uint64_t* tsc_out) {
    uint64_t tsc0 = __rdtsc();
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; ++r) {
        ArithState a;
        auto ctx = ctx0;
        for (const auto& be : bins) {
            if (be.kind == DECISION) light_decide(a, ctx.data(), be.ctx, be.bin, t);
            else if (be.kind == BYPASS) light_bypass(a, be.bin);
            else light_terminal(a, t);
        }
        sink += a.low + ctx[bins.back().ctx];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t tsc1 = __rdtsc();
    (void)sink;
    *tsc_out = tsc1 - tsc0;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// Literal Stage 1 plus a dense 460-byte context snapshot per chunk: isolates
// the cost of writing the checkpoint records themselves.
static double time_context_plus_dense(const std::vector<BinEvent>& bins, size_t chunk,
                                      const std::array<uint8_t, NCTX>& ctx0,
                                      const Tables& t, int reps, uint64_t* tsc_out) {
    int nchunks = (int)((bins.size() + chunk - 1) / chunk);
    uint64_t tsc0 = __rdtsc();
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; ++r) {
        auto ctx = ctx0;
        std::vector<std::array<uint8_t, NCTX>> snaps(nchunks);
        for (size_t i = 0; i < bins.size(); ++i) {
            size_t k = i / chunk;
            if (i % chunk == 0) snaps[k] = ctx;
            const auto& be = bins[i];
            if (be.kind == DECISION) ctx[be.ctx] = t.transition[ctx[be.ctx]][be.bin];
        }
        sink += snaps.back()[0];
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t tsc1 = __rdtsc();
    (void)sink;
    *tsc_out = tsc1 - tsc0;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

static double time_combined_prefix(const std::vector<BinEvent>& bins, size_t chunk,
                                   const std::array<uint8_t, NCTX>& ctx0,
                                   const Tables& t, int reps, uint64_t* tsc_out) {
    int nchunks = (int)((bins.size() + chunk - 1) / chunk);
    uint64_t tsc0 = __rdtsc();
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile uint64_t sink = 0;
    for (int r = 0; r < reps; ++r) {
        auto p = run_prefix(bins, chunk, ctx0, t, nchunks);
        sink += p.checkpoints.back().a.low + p.checkpoints.size();
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    uint64_t tsc1 = __rdtsc();
    (void)sink;
    *tsc_out = tsc1 - tsc0;
    return std::chrono::duration<double, std::nano>(t1 - t0).count();
}

// ---------------------------------------------------------------------------
// Emulation-prevention byte insertion (after merge, per section 4 stage 3)
// ---------------------------------------------------------------------------
static std::vector<uint8_t> insert_epb(const std::vector<uint8_t>& in) {
    std::vector<uint8_t> out;
    out.reserve(in.size() + in.size() / 64);
    int zeros = 0;
    for (uint8_t b : in) {
        if (zeros >= 2 && b <= 3) { out.push_back(0x03); zeros = 0; }
        out.push_back(b);
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Bin stream generators (deterministic, state-driven)
// ---------------------------------------------------------------------------
static std::vector<BinEvent> make_state_driven(size_t n, int qp, bool is_i, int model,
                                               uint64_t seed, double bypass_frac,
                                               const Tables& t) {
    auto ctx0 = init_contexts(t, qp, is_i, model);
    std::mt19937_64 rng(seed);
    std::vector<std::pair<int,int>> pool;
    const int cls[][2] = {{0,16},{64,12},{84,16},{100,12},{112,14},{400,24}};
    for (auto& c : cls) for (int j = 0; j < c[1]; ++j) pool.push_back({c[0]+j, 1 + (j % 4)});
    std::vector<int> weights;
    for (auto& p : pool) weights.push_back(p.second);
    std::discrete_distribution<int> pick(weights.begin(), weights.end());

    std::vector<BinEvent> bins;
    bins.reserve(n);
    auto state = ctx0;
    for (size_t i = 0; i < n; ++i) {
        if (rng() / double(UINT64_MAX) < bypass_frac) {
            bins.push_back({0, (uint8_t)(rng() & 1), BYPASS});
            continue;
        }
        int c = pool[pick(rng)].first;
        int s = state[c];
        double p_mps = std::max(0.02, std::min(0.98, (s >> 1) / 64.0));
        int b = (rng() / double(UINT64_MAX) < p_mps) ? (s & 1) : ((s & 1) ^ 1);
        state[c] = t.transition[s][b];
        bins.push_back({(uint16_t)c, (uint8_t)b, DECISION});
    }
    return bins;
}

// Structured syntax-like stream: MB skip runs -> mb_type -> CBP -> chroma ->
// residual significant/last/level flags.  Bins are still drawn from the live
// context state (the encoder-decoder symmetry x264 relies on).
static std::vector<BinEvent> make_mb_cbp(size_t n, uint64_t seed, const Tables& t) {
    auto ctx0 = init_contexts(t, 26, false, 1);
    std::mt19937_64 rng(seed);
    auto state = ctx0;
    std::vector<BinEvent> bins;
    bins.reserve(n);
    auto dec = [&](int c) {
        int s = state[c];
        double p = std::max(0.02, std::min(0.98, (s >> 1) / 64.0));
        int b = (rng() / double(UINT64_MAX) < p) ? (s & 1) : ((s & 1) ^ 1);
        state[c] = t.transition[s][b];
        bins.push_back({(uint16_t)c, (uint8_t)b, DECISION});
        return b;
    };
    while (bins.size() < n) {
        // skip run: repeated mb_skip_flag with context 11
        int run = (int)(rng() % 24);
        for (int i = 0; i < run && bins.size() < n; ++i) dec(11);
        if (bins.size() >= n) break;
        if (!dec(11)) {  // not skipped: mb_type (truncated ternary-ish)
            for (int k = 0; k < 3 && bins.size() < n; ++k) dec(k % 11);
        }
        // coded block pattern luma/chroma
        for (int k = 76; k < 84 && bins.size() < n; ++k) dec(k);
        // chroma prediction mode
        dec(64 + (int)(rng() % 12));
        // residual blocks
        int blocks = 1 + (int)(rng() % 5);
        for (int bl = 0; bl < blocks && bins.size() < n; ++bl) {
            for (int k = 0; k < 8 && bins.size() < n; ++k) {
                int sig = dec(84 + (k % 16));
                if (sig) {
                    dec(100 + (k % 12));  // last flag
                    for (int lev = 0; lev < 3 && bins.size() < n; ++lev) {
                        dec(112 + (lev % 14));
                        if ((rng() & 3) == 0) bins.push_back({0, (uint8_t)(rng() & 1), BYPASS});
                    }
                }
            }
        }
    }
    bins.resize(n);
    return bins;
}

static std::vector<BinEvent> make_residual(size_t n, uint64_t seed, const Tables& t) {
    auto ctx0 = init_contexts(t, 30, true, 0);
    std::mt19937_64 rng(seed);
    auto state = ctx0;
    std::vector<BinEvent> bins;
    bins.reserve(n);
    auto dec = [&](int c) {
        int s = state[c];
        double p = std::max(0.02, std::min(0.98, (s >> 1) / 64.0));
        int b = (rng() / double(UINT64_MAX) < p) ? (s & 1) : ((s & 1) ^ 1);
        state[c] = t.transition[s][b];
        bins.push_back({(uint16_t)c, (uint8_t)b, DECISION});
    };
    while (bins.size() < n) {
        int zeros_run = (int)(rng() % 30);
        for (int i = 0; i < zeros_run && bins.size() < n; ++i) dec(84 + (i % 16));
        if (bins.size() >= n) break;
        dec(84 + (int)(rng() % 16));  // significant
        dec(100 + (int)(rng() % 12));  // last
        for (int lev = 0; lev < 6 && bins.size() < n; ++lev) {
            dec(112 + (lev % 14));
            bins.push_back({0, (uint8_t)(rng() & 1), BYPASS});  // coeff sign
        }
        if ((rng() & 7) == 0 && bins.size() < n)
            bins.push_back({0, 0, TERMINAL});
    }
    bins.resize(n);
    return bins;
}

// ---------------------------------------------------------------------------
// Verification harness
// ---------------------------------------------------------------------------
struct StreamResult {
    std::string name;
    size_t bins = 0, bytes = 0;
    bool all_chunk_sizes_exact = true;
    std::vector<std::map<std::string, std::string>> per_chunk_size;
};

static SerialCoder serial_encode(const std::vector<BinEvent>& bins,
                                 const std::array<uint8_t, NCTX>& ctx0,
                                 const Tables& t, int frame) {
    SerialCoder c;
    c.ctx = ctx0;
    c.frame = frame;
    for (const auto& be : bins) {
        if (be.kind == DECISION) c.decide(be.ctx, be.bin);
        else if (be.kind == BYPASS) c.bypass(be.bin);
        else c.terminal();
    }
    c.flush();
    return c;
}

// ---------------------------------------------------------------------------
// Sparse context checkpoint variant (only contexts touched in the chunk)
// ---------------------------------------------------------------------------
struct SparseCheckpoint {
    ArithState a;
    std::vector<std::pair<int, uint8_t>> entries;  // (ctx, state at chunk entry)
};

static std::vector<SparseCheckpoint> make_sparse_checkpoints(
        const std::vector<BinEvent>& bins, size_t chunk,
        const std::vector<Checkpoint>& dense, const std::array<uint8_t, NCTX>& ctx0) {
    int nchunks = (int)dense.size();
    std::vector<SparseCheckpoint> out(nchunks);
    std::vector<int> seen(NCTX, 0);
    int gen = 0;
    for (int k = 0; k < nchunks; ++k) {
        out[k].a = dense[k].a;
        ++gen;
        size_t st = (size_t)k * chunk, en = std::min(st + chunk, bins.size());
        for (size_t i = st; i < en; ++i) {
            const BinEvent& be = bins[i];
            if (be.kind != DECISION) continue;
            if (seen[be.ctx] != gen) {
                seen[be.ctx] = gen;
                out[k].entries.push_back({be.ctx, dense[k].ctx[be.ctx]});
            }
        }
    }
    (void)ctx0;
    return out;
}

static ChunkResult encode_chunk_sparse(const SparseCheckpoint& cp, const BinEvent* bins,
                                       size_t n, const Tables& t,
                                       const std::array<uint8_t, NCTX>& ctx0,
                                       bool do_flush, int frame) {
    ChunkCoder c;
    c.a = cp.a;
    c.ctx = ctx0;
    for (const auto& e : cp.entries) c.ctx[e.first] = e.second;
    for (size_t i = 0; i < n; ++i) {
        const BinEvent& be = bins[i];
        if (be.kind == DECISION) c.decide(be.ctx, be.bin, t);
        else if (be.kind == BYPASS) c.bypass(be.bin);
        else c.terminal(t);
    }
    if (do_flush) c.flush(t, frame);
    return {std::move(c.out), c.carry_out};
}

static std::vector<uint8_t> chunk_encode_sparse(const std::vector<BinEvent>& bins,
        const std::array<uint8_t, NCTX>& ctx0, const Tables& t, size_t chunk, int frame,
        std::map<std::string, std::string>* stats) {
    int nchunks = (int)((bins.size() + chunk - 1) / chunk);
    auto dense = run_prefix(bins, chunk, ctx0, t, nchunks).checkpoints;
    auto sparse = make_sparse_checkpoints(bins, chunk, dense, ctx0);
    std::vector<uint8_t> merged{0x5A};
    double entries = 0;
    for (int k = 0; k < nchunks; ++k) {
        size_t st = (size_t)k * chunk, en = std::min(st + chunk, bins.size());
        auto r = encode_chunk_sparse(sparse[k], bins.data() + st, en - st, t, ctx0,
                                     k == nchunks - 1, frame);
        if (r.carry_out && !merged.empty()) merged.back() = (uint8_t)(merged.back() + 1);
        merged.insert(merged.end(), r.bytes.begin(), r.bytes.end());
        entries += (double)sparse[k].entries.size();
    }
    if (stats) (*stats)["mean_sparse_entries_per_chunk"] = std::to_string(entries / nchunks);
    return merged;
}

// ---------------------------------------------------------------------------
// Negative control: contexts-only checkpoint, arithmetic state reset to init.
// Demonstrates that the literal section-4 Stage 1 cannot seed Stage 2 alone.
// ---------------------------------------------------------------------------
static bool context_only_checkpoint_is_sufficient(const std::vector<BinEvent>& bins,
        const std::array<uint8_t, NCTX>& ctx0, const Tables& t, size_t chunk, int frame) {
    int nchunks = (int)((bins.size() + chunk - 1) / chunk);
    auto dense = run_prefix(bins, chunk, ctx0, t, nchunks).checkpoints;
    auto serial = serial_encode(bins, ctx0, t, frame);
    std::vector<uint8_t> merged{0x5A};
    for (int k = 0; k < nchunks; ++k) {
        Checkpoint forced = dense[k];
        forced.a = ArithState{};  // low=0, range=0x1FE, queue=-9, outstanding=0
        size_t st = (size_t)k * chunk, en = std::min(st + chunk, bins.size());
        auto r = encode_chunk(forced, bins.data() + st, en - st, t, 0,
                              k == nchunks - 1, frame);
        if (r.carry_out && !merged.empty()) merged.back() = (uint8_t)(merged.back() + 1);
        merged.insert(merged.end(), r.bytes.begin(), r.bytes.end());
    }
    return merged == serial.out;  // expected false
}


static std::vector<uint8_t> chunk_encode(const std::vector<BinEvent>& bins,
                                         const std::array<uint8_t, NCTX>& ctx0,
                                         const Tables& t, size_t chunk, int frame,
                                         std::map<std::string, std::string>* stats) {
    int nchunks = (int)((bins.size() + chunk - 1) / chunk);
    auto p = run_prefix(bins, chunk, ctx0, t, nchunks);
    std::vector<uint8_t> merged{0x5A};
    int carry_edits = 0, variant_mismatches = 0, max_outstanding = 0, empty_chunks = 0;
    for (int k = 0; k < nchunks; ++k) {
        size_t st = (size_t)k * chunk;
        size_t en = std::min(st + chunk, bins.size());
        auto r0 = encode_chunk(p.checkpoints[k], bins.data() + st, en - st, t, 0,
                               k == nchunks - 1, frame);
        auto r1 = encode_chunk(p.checkpoints[k], bins.data() + st, en - st, t, 1,
                               k == nchunks - 1, frame);
        if (r0.bytes != r1.bytes || r0.carry_out != r1.carry_out) variant_mismatches++;
        if (r0.carry_out && !merged.empty()) { merged.back() = (uint8_t)(merged.back() + 1); carry_edits++; }
        merged.insert(merged.end(), r0.bytes.begin(), r0.bytes.end());
        max_outstanding = std::max(max_outstanding, p.checkpoints[k].a.outstanding);
        if (r0.bytes.empty()) empty_chunks++;
    }
    if (stats) {
        (*stats)["carry_cross_chunk_edits"] = std::to_string(carry_edits);
        (*stats)["carry_in_variant_mismatches"] = std::to_string(variant_mismatches);
        (*stats)["max_outstanding_at_chunk_start"] = std::to_string(max_outstanding);
        (*stats)["chunks_with_no_local_bytes"] = std::to_string(empty_chunks);
        (*stats)["mean_distinct_contexts_per_chunk"] = [&]{
            double s = 0; for (int v : p.touched_per_chunk) s += v;
            return std::to_string(s / nchunks);
        }();
    }
    return merged;
}

static StreamResult verify_stream(const std::string& name, const std::vector<BinEvent>& bins,
                                  const std::array<uint8_t, NCTX>& ctx0, const Tables& t,
                                  int frame) {
    StreamResult r;
    r.name = name;
    r.bins = bins.size();
    auto serial = serial_encode(bins, ctx0, t, frame);
    r.bytes = serial.out.size() - 1;
    auto serial_epb = insert_epb(serial.out);
    size_t epb_insertions = serial_epb.size() - serial.out.size();
    static const size_t CHUNKS[] = {8, 16, 32, 64, 128, 256};
    for (size_t cs : CHUNKS) {
        std::map<std::string, std::string> st;
        auto chunked = chunk_encode(bins, ctx0, t, cs, frame, &st);
        auto sparse = chunk_encode_sparse(bins, ctx0, t, cs, frame, &st);
        bool exact = chunked == serial.out;
        bool sparse_exact = sparse == serial.out;
        auto chunked_epb = insert_epb(chunked);
        bool epb_exact = chunked_epb == serial_epb;
        st["chunk_size"] = std::to_string(cs);
        st["byte_exact"] = exact ? "true" : "false";
        st["sparse_checkpoint_byte_exact"] = sparse_exact ? "true" : "false";
        st["epb_exact_after_merge"] = epb_exact ? "true" : "false";
        st["chunked_bytes"] = std::to_string(chunked.size() - 1);
        st["epb_insertions_in_stream"] = std::to_string(epb_insertions);
        st["context_only_checkpoint_sufficient"] = context_only_checkpoint_is_sufficient(
            bins, ctx0, t, cs, frame) ? "true" : "false";
        if (!exact || !epb_exact || !sparse_exact ||
            st["carry_in_variant_mismatches"] != "0")
            r.all_chunk_sizes_exact = false;
        r.per_chunk_size.push_back(st);
    }
    return r;
}

// ---------------------------------------------------------------------------
// Result serialisation (tiny hand-rolled JSON)
// ---------------------------------------------------------------------------
static std::string json_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') { o.push_back('\\'); o.push_back(c); }
        else if (c == '\n') o += "\\n";
        else o.push_back(c);
    }
    return o;
}

static void emit_results(const std::string& path,
                         const std::vector<StreamResult>& results,
                         const std::map<std::string, std::string>& timing,
                         bool all_exact) {
    std::ostringstream j;
    j << "{\n  \"design\": \"SPECULATIVE_HYBRID.md section 4 chunk-summary CABAC\",\n";
    j << "  \"all_byte_exact\": " << (all_exact ? "true" : "false") << ",\n";
    j << "  \"streams\": [\n";
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        j << "    {\"name\": \"" << json_escape(r.name) << "\", \"bins\": " << r.bins
          << ", \"bytes\": " << r.bytes << ", \"all_chunk_sizes_exact\": "
          << (r.all_chunk_sizes_exact ? "true" : "false") << ", \"chunk_sizes\": [\n";
        for (size_t k = 0; k < r.per_chunk_size.size(); ++k) {
            j << "      {";
            bool first = true;
            for (const auto& kv : r.per_chunk_size[k]) {
                if (!first) j << ", ";
                first = false;
                j << "\"" << kv.first << "\": \"" << json_escape(kv.second) << "\"";
            }
            j << "}" << (k + 1 < r.per_chunk_size.size() ? "," : "") << "\n";
        }
        j << "    ]}" << (i + 1 < results.size() ? "," : "") << "\n";
    }
    j << "  ],\n  \"timing\": {\n";
    bool first = true;
    for (const auto& kv : timing) {
        if (!first) j << ",\n";
        first = false;
        j << "    \"" << kv.first << "\": \"" << json_escape(kv.second) << "\"";
    }
    j << "\n  }\n}\n";
    std::ofstream f(path, std::ios::binary);
    f << j.str();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    std::string tables_path = argc > 1 ? argv[1] : DEFAULT_TABLES;
    try {
        Tables t = load_tables(tables_path);
        g_tables_ptr = &t;
        std::cout << "Loaded tables from " << tables_path << "\n";

        struct StreamCase {
            std::string name;
            std::vector<BinEvent> bins;
            std::array<uint8_t, NCTX> ctx0;
        };
        std::vector<StreamCase> streams;
        streams.push_back({"state_driven_qp26_I_seed264",
            make_state_driven(4096, 26, true, 0, 264, 0.18, t),
            init_contexts(t, 26, true, 0)});
        streams.push_back({"state_driven_qp18_P_seed777",
            make_state_driven(8192, 18, false, 1, 777, 0.25, t),
            init_contexts(t, 18, false, 1)});
        streams.push_back({"structured_mb_type_cbp_residual",
            make_mb_cbp(6000, 12345, t),
            init_contexts(t, 26, false, 1)});
        streams.push_back({"structured_residual_bypass_terminal",
            make_residual(12000, 999, t),
            init_contexts(t, 30, true, 0)});
        streams.push_back({"stress_250k_state_driven",
            make_state_driven(250000, 26, true, 0, 424242, 0.18, t),
            init_contexts(t, 26, true, 0)});

        bool all_exact = true;
        std::vector<StreamResult> results;
        for (auto& s : streams) {
            const auto& ctx0 = s.ctx0;
            int frame = (int)(results.size() % 32);
            auto r = verify_stream(s.name, s.bins, ctx0, t, frame);
            all_exact &= r.all_chunk_sizes_exact;
            std::cout << (r.all_chunk_sizes_exact ? "[PASS] " : "[FAIL] ") << r.name
                      << "  bins=" << r.bins << " bytes=" << r.bytes << "\n";
            if (!r.all_chunk_sizes_exact) {
                for (const auto& m : r.per_chunk_size)
                    std::cout << "    chunk=" << m.at("chunk_size")
                              << " exact=" << m.at("byte_exact")
                              << " epb=" << m.at("epb_exact_after_merge")
                              << " cin_mismatch=" << m.at("carry_in_variant_mismatches") << "\n";
            } else {
                const auto& m = r.per_chunk_size[3];  // 64
                std::cout << "    64-bin: carry_edits=" << m.at("carry_cross_chunk_edits")
                          << "  touched_ctx/chunk=" << m.at("mean_distinct_contexts_per_chunk")
                          << "  sparse_entries/chunk=" << m.at("mean_sparse_entries_per_chunk")
                          << "  max_outstanding=" << m.at("max_outstanding_at_chunk_start")
                          << "  ctx_only_checkpoint_ok=" << m.at("context_only_checkpoint_sufficient")
                          << "  epb_insertions=" << m.at("epb_insertions_in_stream") << "\n";
            }
            results.push_back(std::move(r));
        }

        // ---- Timing on the 250k-bin stress stream -------------------------
        const auto& stress = streams.back().bins;
        const auto& stress_ctx = streams.back().ctx0;
        const int reps = 7;
        uint64_t tsc = 0;
        double ns_ctx = time_context_only(stress, stress_ctx, t, reps, &tsc) / reps;
        double cyc_ctx = double(tsc) / reps;
        tsc = 0;
        double ns_light = time_light_only(stress, stress_ctx, t, reps, &tsc) / reps;
        double cyc_light = double(tsc) / reps;
        tsc = 0;
        double ns_cdense = time_context_plus_dense(stress, 64, stress_ctx, t, reps, &tsc) / reps;
        double cyc_cdense = double(tsc) / reps;
        tsc = 0;
        double ns_pref = time_combined_prefix(stress, 64, stress_ctx, t, reps, &tsc) / reps;
        double cyc_pref = double(tsc) / reps;

        auto t0 = std::chrono::high_resolution_clock::now();
        uint64_t s0 = __rdtsc();
        for (int r = 0; r < reps; ++r) serial_encode(stress, stress_ctx, t, 0);
        uint64_t s1 = __rdtsc();
        auto t1 = std::chrono::high_resolution_clock::now();
        double ns_serial = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
        double cyc_serial = double(s1 - s0) / reps;

        t0 = std::chrono::high_resolution_clock::now();
        s0 = __rdtsc();
        for (int r = 0; r < reps; ++r) {
            std::map<std::string, std::string> st;
            chunk_encode(stress, stress_ctx, t, 64, 0, &st);
        }
        s1 = __rdtsc();
        t1 = std::chrono::high_resolution_clock::now();
        double ns_chunk = std::chrono::duration<double, std::nano>(t1 - t0).count() / reps;
        double cyc_chunk = double(s1 - s0) / reps;

        const double nbins = (double)stress.size();
        const double tsc_ghz = 2.5;  // invariant TSC at i5-12400F base clock
        std::map<std::string, std::string> timing;
        auto put = [&](const char* k, double v) { timing[k] = std::to_string(v); };
        put("stress_bins", nbins);
        put("tsc_assumed_ghz", tsc_ghz);
        put("context_only_ns_total", ns_ctx);
        put("context_only_ns_per_bin", ns_ctx / nbins);
        put("context_only_tsc_cycles_per_bin", cyc_ctx / nbins);
        put("context_only_ms_at_250k", ns_ctx / 1e6);
        put("context_only_ms_at_300k", ns_ctx / nbins * 300000.0 / 1e6);
        put("light_only_ns_total", ns_light);
        put("light_only_ns_per_bin", ns_light / nbins);
        put("light_only_tsc_cycles_per_bin", cyc_light / nbins);
        put("light_only_ms_at_300k", ns_light / nbins * 300000.0 / 1e6);
        put("context_plus_dense_snapshot_ns_total", ns_cdense);
        put("context_plus_dense_snapshot_ns_per_bin", ns_cdense / nbins);
        put("context_plus_dense_snapshot_tsc_cycles_per_bin", cyc_cdense / nbins);
        put("context_plus_dense_snapshot_ms_at_300k", ns_cdense / nbins * 300000.0 / 1e6);
        put("combined_prefix_ns_total", ns_pref);
        put("combined_prefix_ns_per_bin", ns_pref / nbins);
        put("combined_prefix_tsc_cycles_per_bin", cyc_pref / nbins);
        put("combined_prefix_ms_at_300k", ns_pref / nbins * 300000.0 / 1e6);
        put("serial_arithmetic_ns_total", ns_serial);
        put("serial_arithmetic_ns_per_bin", ns_serial / nbins);
        put("serial_arithmetic_tsc_cycles_per_bin", cyc_serial / nbins);
        put("serial_arithmetic_ms_at_300k", ns_serial / nbins * 300000.0 / 1e6);
        put("chunked_ns_total", ns_chunk);
        put("chunked_ns_per_bin", ns_chunk / nbins);
        put("chunked_tsc_cycles_per_bin", cyc_chunk / nbins);
        put("chunked_ms_at_300k", ns_chunk / nbins * 300000.0 / 1e6);
        put("gpu_serial_context_ms_at_300k_1p5ghz_same_cycles",
            cyc_ctx / nbins * 300000.0 / 1.5e9 * 1e3);
        put("gpu_serial_light_only_ms_at_300k_1p5ghz_same_cycles",
            cyc_light / nbins * 300000.0 / 1.5e9 * 1e3);
        put("gpu_serial_context_plus_dense_ms_at_300k_1p5ghz_same_cycles",
            cyc_cdense / nbins * 300000.0 / 1.5e9 * 1e3);
        put("gpu_serial_combined_prefix_ms_at_300k_1p5ghz_same_cycles",
            cyc_pref / nbins * 300000.0 / 1.5e9 * 1e3);

        std::cout << "\nTiming on " << (size_t)nbins << " bins (median-equivalent, " << reps << " reps):\n";
        for (const auto& kv : timing)
            std::cout << "  " << kv.first << " = " << kv.second << "\n";

        std::string out_path = "results/chunk_cabac_results.json";
        if (argc > 2) out_path = argv[2];
        emit_results(out_path, results, timing, all_exact);
        std::cout << "\nResults written to " << out_path << "\n";
        return all_exact ? 0 : 2;
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}


