/// Verify: run 4str_0int ht=65536 lf=0.5 sel=0.1, print NumGroups.
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <numeric>
#include <random>
#define XXH_INLINE_ALL
#include "xxhash.h"
#include "column_marshaller.h"

static inline uint64_t HB(const uint8_t* d, size_t l, uint64_t s) { return XXH3_64bits_withSeed(d,l,s); }

int main() {
    constexpr size_t NS = 4, HT = 65536, NP = 1000000;
    constexpr double LF = 0.5, SEL = 0.1;
    size_t NK = static_cast<size_t>(HT * LF);

    std::mt19937_64 rng(42);

    // Build keys
    std::vector<std::vector<std::vector<uint8_t>>> strCols(NS);
    for (size_t c = 0; c < NS; c++) {
        strCols[c].resize(NK);
        for (size_t i = 0; i < NK; i++) {
            auto s = "key_" + std::to_string(i) + "_c" + std::to_string(c);
            strCols[c][i].assign(s.begin(), s.end());
        }
    }
    std::vector<int64_t> buildHashes(NK);
    for (size_t i = 0; i < NK; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NS; c++) h = HB(strCols[c][i].data(), strCols[c][i].size(), h);
        buildHashes[i] = static_cast<int64_t>(h);
    }
    std::vector<int64_t> buildValues(NK);
    for (size_t i = 0; i < NK; i++) buildValues[i] = i % 1000;

    // Probe keys
    size_t nH = static_cast<size_t>(NP * SEL), nM = NP - nH;
    std::vector<std::vector<std::vector<uint8_t>>> probeStr(NS);
    for (auto& v : probeStr) v.reserve(NP);
    std::vector<int64_t> probeHashes; probeHashes.reserve(NP);

    for (size_t i = 0; i < nH; i++) {
        size_t idx = rng() % NK;
        for (size_t c = 0; c < NS; c++) probeStr[c].push_back(strCols[c][idx]);
        probeHashes.push_back(buildHashes[idx]);
    }
    for (size_t i = 0; i < nM; i++) {
        uint64_t h = 0;
        for (size_t c = 0; c < NS; c++) {
            auto s = "miss_" + std::to_string(i) + "_" + std::to_string(c);
            std::vector<uint8_t> b(s.begin(), s.end());
            h = HB(b.data(), b.size(), h);
            probeStr[c].push_back(std::move(b));
        }
        probeHashes.push_back(static_cast<int64_t>(h));
    }

    // Shuffle
    std::vector<size_t> ord(NP); std::iota(ord.begin(), ord.end(), 0);
    for (size_t i = NP - 1; i > 0; i--) std::swap(ord[i], ord[rng() % (i + 1)]);
    std::vector<std::vector<std::vector<uint8_t>>> shuffledStr(NS);
    for (size_t c = 0; c < NS; c++) { shuffledStr[c].resize(NP); for (size_t i = 0; i < NP; i++) shuffledStr[c][i] = std::move(probeStr[c][ord[i]]); }
    std::vector<int64_t> shuffledHashes(NP); for (size_t i = 0; i < NP; i++) shuffledHashes[i] = probeHashes[ord[i]];
    std::vector<int64_t> probeValues(NP); for (size_t i = 0; i < NP; i++) probeValues[i] = i % 1000;

    // Combine
    size_t TR = NK + NP;
    for (size_t c = 0; c < NS; c++) for (auto& v : shuffledStr[c]) strCols[c].push_back(std::move(v));
    std::vector<int64_t> allHashes = buildHashes; allHashes.insert(allHashes.end(), shuffledHashes.begin(), shuffledHashes.end());
    std::vector<int64_t> allValues = buildValues; allValues.insert(allValues.end(), probeValues.begin(), probeValues.end());

    // Build VarcharSlice arrays
    std::vector<std::vector<taper::VarcharSlice>> slices(NS);
    for (size_t c = 0; c < NS; c++) {
        slices[c].resize(TR);
        for (size_t i = 0; i < TR; i++) { slices[c][i].ptr = strCols[c][i].data(); slices[c][i].len = strCols[c][i].size(); }
    }

    // Run taper
    size_t distinctKeys = NK + nM;
    size_t minSlots = std::max(static_cast<size_t>(distinctKeys / 0.85), size_t(8));
    size_t numChunks = 1; while (numChunks * 8 < minSlots) numChunks *= 2;

    taper::SimpleArenaAllocator pool;
    std::vector<taper::ColumnDesc> cd(NS, taper::ColumnDesc::Varchar);
    taper::TaperColumnSerializeHandler t(pool, 8, cd, numChunks);

    constexpr size_t BATCH_SIZE = 410;
    size_t numBatches = (TR + BATCH_SIZE - 1) / BATCH_SIZE;
    std::vector<taper::ColumnInput> cols(NS);

    for (size_t batch = 0; batch < numBatches; batch++) {
        size_t start = batch * BATCH_SIZE;
        size_t end = std::min(start + BATCH_SIZE, TR);
        int32_t batchLen = static_cast<int32_t>(end - start);
        for (size_t c = 0; c < NS; c++) cols[c] = taper::ColumnInput::MakeVarchar(slices[c].data() + start);
        t.EmplaceTableWithDecode(allHashes.data() + start, batchLen, cols, allValues.data() + start);
    }

    printf("C++:  NumGroups = %zu\n", t.NumGroups());
}
