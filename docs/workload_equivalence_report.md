# C++ vs Rust Benchmark Workload Equivalence Report

## Summary

The two benchmarks are **semantically equivalent** in algorithm, data volume, and output,
but have **3 timing-scope discrepancies** that systematically favor C++ over Rust:

| # | Discrepancy | Favors | Impact Estimate |
|---|---|---|---|
| 1 | Rust allocates `Vec<Vec<&[u8]>>` per batch inside timing; C++ uses pre-computed `VarcharSlice*` | **C++** | ~2-5% for low sel |
| 2 | Rust allocates `Vec<ColumnInput>` per batch inside timing; C++ pre-allocates once | **C++** | ~1-2% |
| 3 | C++ hashes stored as `int64_t`; Rust as `u64` | Neutral | 0% (bitwise ops identical) |

**Important**: Discrepancies 1 and 2 mean Rust is doing MORE work per iteration than C++,
yet Rust is still faster. The true algorithmic performance gap is even larger than measured.

## Detailed Comparison

### A. Data Generation (Outside Timing)

| Aspect | C++ | Rust | Match? |
|--------|-----|------|--------|
| RNG | std::mt19937_64(42) | Mt19937GenRand64::new(42) | Same algo, same seed |
| String format | "key_{i}_c{c}" | "key_{i}_c{c}" | Identical |
| Int formula | i*(97+c*31)+1 | i*(97+c*31)+1 | Identical |
| Hash function | XXH3_64bits_withSeed | xxh3_64_with_seed | Same algorithm |
| Hash chain order | str cols first, then int cols | Same | Identical |
| num_keys | ht_size * load_factor | Same | Identical |
| num_probe_rows | 1,000,000 | 1,000,000 | Identical |
| Selectivity | floor(nProbe * sel) hits, rest misses | Same | Identical |
| Shuffle | Fisher-Yates with RNG | Same | Same algo |
| Final layout | build ++ shuffled_probe | Same | Identical |

### B. Timing Scope

| What's INSIDE timing | C++ | Rust |
|---------------------|-----|------|
| Arena/pool creation | YES | YES |
| Hash table creation | YES | YES |
| ColumnDesc vec build | YES (tiny) | YES (tiny) |
| Batch loop | YES | YES |
| VarcharSlice construction | **NO** (pre-computed in GenData) | **YES** (per-batch Vec alloc) |
| ColumnInput vec per batch | **NO** (pre-allocated, just assigns) | **YES** (new Vec each batch) |
| Process all rows | YES | YES |
| Read num_groups | YES | YES |

### C. Hash Table Setup

| Parameter | C++ | Rust | Match? |
|-----------|-----|------|--------|
| BATCH_SIZE | 410 | 410 | Identical |
| distinct_keys | numKeys + numMisses | Same | Identical |
| min_slots | distinct_keys / 0.85, max 8 | Same | Identical |
| num_chunks | smallest power-of-2 where chunks*8 >= min_slots | Same | Identical |

### D. Per-Iteration Work

Both create a fresh hash table + arena from scratch each iteration.
Both process total_rows = num_keys + 1,000,000 rows.
Both output num_groups via DoNotOptimize/black_box.

### E. Conclusion

The workloads are algorithm-equivalent. Rust actually does slightly MORE per-iteration work
(per-batch Vec allocations) yet is still faster. This makes the C++ performance gap even
more concerning — the true gap when controlling for this discrepancy would be slightly larger.

**Recommendation**: Do NOT "fix" this by adding pre-computation to Rust. The correct
approach is to identify why C++ generates worse machine code for the shared algorithm.
