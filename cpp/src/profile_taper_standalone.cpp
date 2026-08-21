/// Standalone profile runner for Taper C++ — no Google Benchmark dependency.
/// Identical workload to the Criterion Rust bench, but with manual timing.
///
/// Usage:
///   ./cpp_profile_taper [sel=0.1|0.9]
///
/// Default: sel=0.1 (low selectivity = many new groups, stresses insertion path)

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include <random>

#define XXH_INLINE_ALL
#include "xxhash.h"
#include "column_marshaller.h"

// ═══════════════════════════════════════════════════════════════════
// Compile-time instrumentation (enable with -DPROFILE_INSTRUMENTATION)
// ═══════════════════════════════════════════════════════════════════

#ifdef PROFILE_INSTRUMENTATION
struct ProfileCounters {
    uint64_t new_group_count = 0;
    uint64_t existing_group_count = 0;
    uint64_t arena_alloc_count = 0;
    uint64_t arena_alloc_bytes = 0;
    uint64_t row_alloc_count = 0;
    uint64_t row_alloc_bytes = 0;
    uint64_t varchar_serialize_count = 0;
    uint64_t varchar_serialize_bytes = 0;
    uint64_t memcpy_count = 0;
    uint64_t memcpy_bytes = 0;
    uint64_t short_string_cmp_count = 0;
    uint64_t zeroed_bytes = 0;
};
static ProfileCounters g_counters;
#define PROF_INC(field) (g_counters.field++)
#define PROF_ADD(field, n) (g_counters.field += (n))
#else
#define PROF_INC(field)
#define PROF_ADD(field, n)
#endif

// ═══════════════════════════════════════════════════════════════════
// Parameters (matching Rust exactly)
// ═══════════════════════════════════════════════════════════════════

static constexpr size_t NUM_STR_COLS = 4;
static constexpr size_t NUM_INT_COLS = 0;
static constexpr size_t HT_SIZE = 16384;
static constexpr double LOAD_FACTOR = 0.50;
static constexpr size_t NUM_PROBE_ROWS = 1000000;
static constexpr size_t BATCH_SIZE = 410;
static constexpr size_t NUM_ITERS = 10;
static constexpr uint64_t SEED = 42;

// ═══════════════════════════════════════════════════════════════════
// Data generation (identical to taper_bench.cpp GenData)
// ═══════════════════════════════════════════════════════════════════

static inline uint64_t HB(const uint8_t* d, size_t l, uint64_t s) {
    return XXH3_64bits_withSeed(d, l, s);
}
static inline uint64_t HC(uint64_t s, int64_t v) {
    return XXH3_64bits_withSeed(&v, 8, s);
}

struct BenchData {
    std::vector<std::vector<std::vector<uint8_t>>> strCols;
    std::vector<std::vector<int64_t>> intCols;
    std::vector<int64_t> hashes, values;
    std::vector<std::vector<taper::VarcharSlice>> strSlices;
    size_t totalRows;
};

static BenchData GenData(size_t nKeys, double sel) {
    std::mt19937_64 rng(SEED);
    BenchData d;
    d.strCols.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].resize(nKeys);
        for (size_t i = 0; i < nKeys; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            d.strCols[c][i].assign(s.begin(), s.end());
        }
    }
    // No int cols for 4str_0int
    std::vector<int64_t> bh(nKeys);
    for (size_t i = 0; i < nKeys; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            h = HB(d.strCols[c][i].data(), d.strCols[c][i].size(), h);
        bh[i] = static_cast<int64_t>(h);
    }
    std::vector<int64_t> bv(nKeys);
    for (size_t i = 0; i < nKeys; i++) bv[i] = i % 1000;

    size_t nH = static_cast<size_t>(NUM_PROBE_ROWS * sel);
    size_t nM = NUM_PROBE_ROWS - nH;

    std::vector<std::vector<std::vector<uint8_t>>> ps(NUM_STR_COLS);
    for (auto& v : ps) v.reserve(NUM_PROBE_ROWS);
    std::vector<int64_t> ph;
    ph.reserve(NUM_PROBE_ROWS);

    for (size_t i = 0; i < nH; i++) {
        size_t idx = rng() % nKeys;
        for (size_t c = 0; c < NUM_STR_COLS; c++)
            ps[c].push_back(d.strCols[c][idx]);
        ph.push_back(bh[idx]);
    }
    for (size_t i = 0; i < nM; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NUM_STR_COLS; c++) {
            auto s = "miss_" + std::to_string(i) + "_" + std::to_string(c);
            std::vector<uint8_t> b(s.begin(), s.end());
            h = HB(b.data(), b.size(), h);
            ps[c].push_back(std::move(b));
        }
        ph.push_back(static_cast<int64_t>(h));
    }

    // Shuffle
    std::vector<size_t> ord(NUM_PROBE_ROWS);
    std::iota(ord.begin(), ord.end(), 0);
    for (size_t i = NUM_PROBE_ROWS - 1; i > 0; i--)
        std::swap(ord[i], ord[rng() % (i + 1)]);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        auto tmp = std::move(ps[c]);
        ps[c].resize(NUM_PROBE_ROWS);
        for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ps[c][i] = std::move(tmp[ord[i]]);
    }
    { auto tmp = ph; for (size_t i = 0; i < NUM_PROBE_ROWS; i++) ph[i] = tmp[ord[i]]; }
    std::vector<int64_t> pv(NUM_PROBE_ROWS);
    for (size_t i = 0; i < NUM_PROBE_ROWS; i++) pv[i] = i % 1000;

    d.totalRows = nKeys + NUM_PROBE_ROWS;
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strCols[c].reserve(d.totalRows);
        for (auto& v : ps[c]) d.strCols[c].push_back(std::move(v));
    }
    d.hashes = bh;
    d.hashes.insert(d.hashes.end(), ph.begin(), ph.end());
    d.values = bv;
    d.values.insert(d.values.end(), pv.begin(), pv.end());

    // Build VarcharSlice arrays
    d.strSlices.resize(NUM_STR_COLS);
    for (size_t c = 0; c < NUM_STR_COLS; c++) {
        d.strSlices[c].resize(d.totalRows);
        for (size_t i = 0; i < d.totalRows; i++) {
            d.strSlices[c][i].ptr = d.strCols[c][i].data();
            d.strSlices[c][i].len = d.strCols[c][i].size();
        }
    }
    return d;
}

// ═══════════════════════════════════════════════════════════════════
// Core workload (noinline for perf visibility)
// ═══════════════════════════════════════════════════════════════════

__attribute__((noinline))
static size_t RunWorkload(const BenchData& d, size_t numChunks) {
    taper::SimpleArenaAllocator pool;
    std::vector<taper::ColumnDesc> cd;
    for (size_t c = 0; c < NUM_STR_COLS; c++) cd.push_back(taper::ColumnDesc::Varchar);
    for (size_t c = 0; c < NUM_INT_COLS; c++) cd.push_back(taper::ColumnDesc::Int64);

    taper::TaperColumnSerializeHandler t(pool, 8, cd, numChunks);

    size_t totalRows = d.totalRows;
    size_t numBatches = (totalRows + BATCH_SIZE - 1) / BATCH_SIZE;
    std::vector<taper::ColumnInput> cols(NUM_STR_COLS + NUM_INT_COLS);

    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, totalRows);
        int32_t batchLen = static_cast<int32_t>(end - start);

        for (size_t c = 0; c < NUM_STR_COLS; c++)
            cols[c] = taper::ColumnInput::MakeVarchar(d.strSlices[c].data() + start);
        for (size_t c = 0; c < NUM_INT_COLS; c++)
            cols[NUM_STR_COLS + c] = taper::ColumnInput::MakeInt64(d.intCols[c].data() + start);

        t.EmplaceTableWithDecode(d.hashes.data() + start, batchLen, cols, d.values.data() + start);
    }

    return t.NumGroups();
}

// ═══════════════════════════════════════════════════════════════════
// Main
// ═══════════════════════════════════════════════════════════════════

int main(int argc, char** argv) {
    double sel = 0.1;
    if (argc > 1) {
        sel = std::atof(argv[1]);
        if (sel <= 0.0 || sel > 1.0) { fprintf(stderr, "sel must be in (0,1]\n"); return 1; }
    }

    size_t numKeys = static_cast<size_t>(HT_SIZE * LOAD_FACTOR);
    size_t numMisses = NUM_PROBE_ROWS - static_cast<size_t>(NUM_PROBE_ROWS * sel);
    size_t distinctKeys = numKeys + numMisses;
    size_t minSlots = std::max(static_cast<size_t>(distinctKeys / 0.85), size_t(8));
    size_t numChunks = 1;
    while (numChunks * 8 < minSlots) numChunks *= 2;

    fprintf(stderr, "=== C++ Profile Runner ===\n");
    fprintf(stderr, "Config: 4str_0int, ht=%zu, lf=%.2f, sel=%.1f\n", HT_SIZE, LOAD_FACTOR, sel);
    fprintf(stderr, "numKeys=%zu, numProbe=%zu, totalRows=%zu\n", numKeys, NUM_PROBE_ROWS, numKeys + NUM_PROBE_ROWS);
    fprintf(stderr, "distinctKeys=%zu, numChunks=%zu, capacity=%zu\n", distinctKeys, numChunks, numChunks * 8);
    fprintf(stderr, "Iterations: %zu (+ 1 warmup)\n", NUM_ITERS);
    fprintf(stderr, "Generating data...\n");

    BenchData data = GenData(numKeys, sel);
    fprintf(stderr, "Data generated. totalRows=%zu\n\n", data.totalRows);

    // Warmup (pre-fault pages, warm caches)
    fprintf(stderr, "Warmup...\n");
    volatile size_t warmup_groups = RunWorkload(data, numChunks);
    (void)warmup_groups;
    fprintf(stderr, "Warmup done (groups=%zu)\n\n", (size_t)warmup_groups);

#ifdef PROFILE_INSTRUMENTATION
    // Reset counters after warmup
    memset(&g_counters, 0, sizeof(g_counters));
#endif

    // Timed iterations
    fprintf(stderr, "Running %zu iterations...\n", NUM_ITERS);
    auto t0 = std::chrono::high_resolution_clock::now();

    volatile size_t groups = 0;
    for (size_t iter = 0; iter < NUM_ITERS; iter++) {
        groups = RunWorkload(data, numChunks);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed_ns = std::chrono::duration<double, std::nano>(t1 - t0).count();
    double per_iter_ns = elapsed_ns / NUM_ITERS;
    double per_iter_ms = per_iter_ns / 1e6;
    double items_per_sec = static_cast<double>(data.totalRows) / (per_iter_ns / 1e9);

    printf("=== Results ===\n");
    printf("Total elapsed:     %.3f ms\n", elapsed_ns / 1e6);
    printf("Per iteration:     %.3f ms\n", per_iter_ms);
    printf("ns/iteration:      %.0f\n", per_iter_ns);
    printf("Items/sec:         %.3f M/s\n", items_per_sec / 1e6);
    printf("Groups:            %zu\n", (size_t)groups);
    printf("Checksum (groups): %zu\n", (size_t)groups); // prevent DCE

#ifdef PROFILE_INSTRUMENTATION
    printf("\n=== Instrumentation ===\n");
    printf("new_group_count:         %lu\n", g_counters.new_group_count);
    printf("existing_group_count:    %lu\n", g_counters.existing_group_count);
    printf("arena_alloc_count:       %lu\n", g_counters.arena_alloc_count);
    printf("arena_alloc_bytes:       %lu\n", g_counters.arena_alloc_bytes);
    printf("row_alloc_count:         %lu\n", g_counters.row_alloc_count);
    printf("row_alloc_bytes:         %lu\n", g_counters.row_alloc_bytes);
    printf("varchar_serialize_count: %lu\n", g_counters.varchar_serialize_count);
    printf("varchar_serialize_bytes: %lu\n", g_counters.varchar_serialize_bytes);
    printf("memcpy_count:            %lu\n", g_counters.memcpy_count);
    printf("memcpy_bytes:            %lu\n", g_counters.memcpy_bytes);
    printf("short_string_cmp_count:  %lu\n", g_counters.short_string_cmp_count);
    printf("zeroed_bytes:            %lu\n", g_counters.zeroed_bytes);
#endif

    return 0;
}
