/// Micro-benchmark: isolate NewRow + StoreKeyOneRow (4 varchar) to compare with Rust.
/// No hash table involved — pure arena allocation + varchar serialization.
#include <benchmark/benchmark.h>
#include <vector>
#include <string>
#include <cstring>
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "column_marshaller.h"

static void BM_NewRowAndStoreKey(benchmark::State& st) {
    const size_t N = 1000000; // 1M rows
    const size_t NUM_STR = 4;

    // Generate varchar data
    std::vector<std::vector<std::vector<uint8_t>>> strCols(NUM_STR);
    for (size_t c = 0; c < NUM_STR; c++) {
        strCols[c].resize(N);
        for (size_t i = 0; i < N; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            strCols[c][i].assign(s.begin(), s.end());
        }
    }
    std::vector<std::vector<taper::VarcharSlice>> slices(NUM_STR);
    for (size_t c = 0; c < NUM_STR; c++) {
        slices[c].resize(N);
        for (size_t i = 0; i < N; i++) {
            slices[c][i].ptr = strCols[c][i].data();
            slices[c][i].len = strCols[c][i].size();
        }
    }

    std::vector<taper::ColumnDesc> cd(NUM_STR, taper::ColumnDesc::Varchar);
    std::vector<taper::ColumnInput> cols(NUM_STR);

    for (auto _ : st) {
        taper::SimpleArenaAllocator pool;
        taper::TaperColumnSerializeHandler handler(pool, 8, cd, 1024);

        for (size_t c = 0; c < NUM_STR; c++)
            cols[c] = taper::ColumnInput::MakeVarchar(slices[c].data());

        // Simulate on_init path: NewRow + StoreKeyOneRow for 1M rows
        for (size_t i = 0; i < N; i++) {
            // This is what on_init does (minus the hash table slot write)
            char* row = handler.aggRows_.NewRow();
            // StoreKeyOneRow equivalent - directly call the merged varchar path
            size_t totalSize = 0;
            for (size_t c = 0; c < NUM_STR; c++) {
                totalSize += 1 + taper::ComputeRowLenSize(slices[c][i].len) + slices[c][i].len;
            }
            uint8_t* blockStart = handler.aggRows_.ArenaAlloc(totalSize);
            uint8_t* wp = blockStart;
            for (size_t c = 0; c < NUM_STR; c++) {
                wp += taper::SerializeVarcharToBuffer(wp, slices[c][i].ptr, slices[c][i].len);
            }
            // Store pointer to merged block in row
            memcpy(row + 0, &blockStart, sizeof(blockStart)); // varchar slot at offset 0
        }
        benchmark::DoNotOptimize(handler.NumGroups());
    }
    st.SetItemsProcessed(st.iterations() * N);
}

BENCHMARK(BM_NewRowAndStoreKey)->Iterations(10);
BENCHMARK_MAIN();
