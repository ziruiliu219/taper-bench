# Row Layout & Allocation Comparison: C++ vs Rust

## Conclusion: All parameters are numerically identical.

The Rust implementation is a faithful port. Memory layout and allocation
behavior are the same. Performance differences come from codegen, not layout.

---

## RowContainer Layout (for 4str_0int, agg_state_size=8)

| Column | Type | C++ Offset | Rust Offset |
|--------|------|-----------|-------------|
| col 0 (varchar) | ptr slot (8B) | 0 | 0 |
| col 1 (varchar) | ptr slot (8B) | 8 | 8 |
| col 2 (varchar) | ptr slot (8B) | 16 | 16 |
| col 3 (varchar) | ptr slot (8B) | 24 | 24 |
| null bitmap | 1 byte (4 cols / 8) | 32 | 32 |
| agg state | i64 (8B) | 33 | 33 |
| **total row_size** | | **41 bytes** | **41 bytes** |

---

## SimpleArenaAllocator Parameters

| Parameter | C++ | Rust | Match? |
|-----------|-----|------|--------|
| min_chunk_size | 4096 | 4096 | Identical |
| growth_factor | 2 | 2 | Identical |
| linear_growth_threshold | 512 KiB | 512 KiB | Identical |
| allocation alignment | None (bump pointer) | None (bump pointer) | Identical |
| underlying allocator | malloc / free | libc::malloc / libc::free | Identical |
| chunk growth formula | max(requested, last * 2) until 512K, then linear | Same | Identical |

---

## RowContainer Parameters

| Parameter | C++ | Rust | Match? |
|-----------|-----|------|--------|
| BLOCK_ROWS | 1024 | 1024 | Identical |
| row_size formula | sum(col_sizes) + null_bytes + agg_size | Same | Identical |
| null_block_start | after last column offset | Same | Identical |
| null_bytes | (num_keys + 7) / 8 | Same | Identical |
| agg_state_offset | null_block_start + null_bytes | Same | Identical |
| new_row: zero-init | memset(block, 0, size) | write_bytes(0) | Identical |
| per-row alignment | None (packed at row_size stride) | None | Identical |

---

## Structural Differences (no performance impact on layout)

| Aspect | C++ | Rust |
|--------|-----|------|
| Arena ownership | External reference (`&pool`) | Owned inline (`pool: SimpleArenaAllocator`) |
| Block tracking | No list (only batchPtr/batchRemaining) | Vec of (ptr, count) for agg_checksum iteration |
| AllocateContinue | Present (unused in bench) | Not present |
| Reset method | Present (unused in bench) | Not present |

---

## Implication for Performance

Since all data layout and allocation parameters are identical, the measured
performance difference (20-50% on Kunpeng, 5% on M2) cannot be attributed to:
- Different row sizes
- Different memory alignment
- Different allocation chunk sizes or growth rates
- Different null bitmap placement

The difference is entirely in **code generation** — how the compiler translates
the algorithm into machine instructions.
