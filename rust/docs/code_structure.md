# rust-hash 代码结构

## 目录结构

```
src/
├── lib.rs                  # crate 入口, 导出所有模块
├── taper_hashmap.rs        # TaperHashMap: hash table 核心
├── chunk.rs                # Chunk + SlotValue: hash table 的物理存储单元
├── bitmask.rs              # BitMask: SWAR tag 匹配
├── row_container.rs        # RowContainer: 行存储 + arena 分配
├── column_marshaller.rs    # TaperColumnSerializeHandler: 5-step emplace pipeline
└── batch_compare.rs        # 批量 key 比较 (NEON / scalar)

benches/
└── hashmap_bench.rs        # Criterion benchmark 入口
```

---

## 模块关系图

```
hashmap_bench.rs
  └── TaperColumnSerializeHandler::emplace_table_with_decode()
        ├── TaperHashMap::emplace_batch_full()     [taper_hashmap.rs]
        │     ├── BitMask::match_tag()             [bitmask.rs]
        │     ├── BitMask::match_empty()           [bitmask.rs]
        │     └── Chunk / SlotValue                [chunk.rs]
        ├── store_key_one_row_from_decode()        [column_marshaller.rs]
        │     └── RowContainer::new_row()          [row_container.rs]
        │     └── RowContainer::arena_alloc()      [row_container.rs]
        ├── get_unequals_num_with_decode()         [column_marshaller.rs]
        │     ├── batch_compare_decoded_i64()      [batch_compare.rs]
        │     ├── batch_compare_varchar_decoded_cached() [batch_compare.rs]
        │     └── build_merged_varchar_cache()     [batch_compare.rs]
        ├── compare_keys_with_decode()             [column_marshaller.rs]
        └── TaperHashMap::emplace()                [taper_hashmap.rs]
```

---

## 各文件 & 函数说明

### `taper_hashmap.rs` — Hash Table 核心

| 函数 | 作用 | 对应 C++ |
|---|---|---|
| `TaperHashMap::new()` | 创建空表（1 chunk） | `TaperFlatHashTable(1)` |
| `with_capacity(n)` | 创建 n 个 chunk 的表 | `TaperFlatHashTable(n)` |
| `with_slot_capacity(min_slots)` | 创建至少 min_slots 个 slot 的表 | 无直接对应 |
| `emplace(hash, key_cmp, init, update)` | 单行精确插入（Step 5 用） | `table->Emplace(...)` |
| `emplace_batch_full(hashes, filter, init, update)` | 批量插入（Step 2），碰撞迭代+expand | `table->EmplaceBatch(...)` |
| `emplace_batch_simd(...)` | SIMD 加速批量插入（实验性） | 无 |
| `expand()` | 容量翻倍 + rehash 所有条目 | `ExpandCapacityIteratively()` |
| `chunk_pos(hash)` | hash → chunk index (mask) | `GetChunkPos(h)` |
| `rehash_pos(batch, pos)` | 碰撞后线性探测下一个 chunk | `GetRehashPos(batch, pos)` |
| `prefetch_chunk(pos)` | L1 prefetch 指定 chunk | `Prefetch(pos)` |
| `alloc_chunks(n)` | `aligned_alloc` + memset 0x80 | `AllocChunks(n)` |

### `chunk.rs` — Hash Table 物理存储

| 结构/函数 | 作用 | 对应 C++ |
|---|---|---|
| `Chunk` (128 bytes, `#[repr(align(128))]`) | 一个 chunk: tags + keys + values | `TaperHashTableChunk` |
| `SlotValue` | 6 字节 value slot (存 row 指针) | chunk value buf |
| `SlotValue::set_ptr(ptr)` | 写入 48-bit 指针 | `SetRowPtr(buf, ptr)` |
| `SlotValue::get_ptr()` | 读出 48-bit 指针 | `GetRowPtr(buf)` |

### `bitmask.rs` — SWAR Tag 匹配

| 函数 | 作用 | 对应 C++ |
|---|---|---|
| `BitMask::match_tag(tags, target)` | 在 8 个 tag 中找匹配 target 的 slot | `PHBitMask::MatchTag(...)` |
| `BitMask::match_empty(tags)` | 在 8 个 tag 中找空 slot (0x80) | `PHBitMask::MatchEmpty(...)` |
| `BitMask::lowest()` | 返回最低匹配 slot 索引 | `operator*()` |
| `BitMask::advance()` | 清除最低匹配，返回剩余 | `operator++()` |
| Iterator impl | 遍历所有匹配 slot | for 循环 PHBitMask |

### `row_container.rs` — 行存储

| 函数 | 作用 | 对应 C++ |
|---|---|---|
| `RowContainer::new(key_sizes, agg_size)` | 计算行布局(offsets, null bits, agg offset) | `RowContainer(keySizes, ...)` |
| `new_row()` | 分配一行（从 block 切或新建 block） | `NewRow()` |
| `arena_alloc(size)` | 从 varchar arena 分配连续内存 | `ArenaAlloc(size)` / `pool_.Allocate(...)` |
| `is_null_at(row, byte, mask)` | 读 null bit | `IsNullAt(...)` |
| `set_null_at(row, byte, mask)` | 设 null bit | `SetNullAt(...)` |
| `clear_null_at(row, byte, mask)` | 清 null bit | `ClearNullAt(...)` |
| `read_value<T>(row, offset)` | 从行读固定宽度值 | `ReadValue<T>(...)` |
| `store_value<T>(row, offset, val)` | 向行写固定宽度值 | `StoreValue<T>(...)` |
| `reserve(additional)` | 预分配 blocks（减少运行时分配） | 无直接对应 |

**Arena 实现：** 64KB 固定大小 block，纯 bump pointer。满了就 push 新 block。
无 `AllocateContinue` 搬迁逻辑（merged varchar 预先算好总大小，一次性 alloc）。

### `column_marshaller.rs` — 5-Step Emplace Pipeline

#### 静态工具函数

| 函数 | 作用 | 对应 C++ |
|---|---|---|
| `compute_row_len_size(len)` | 字符串长度 → 需要几字节存长度 (1/2/4) | `ComputeRowLenSize(len)` |
| `serialize_varchar_to_buffer(ptr, data)` | 序列化一个 varchar 到 arena | `SerializeVarcharToBuffer(...)` |
| `null_variable_type_serializer(ptr)` | 写 null 标记 `[0]` | `NullVariableTypeSerializer(...)` |
| `compute_varchar_serialized_size(ptr)` | 从 arena 指针算出条目总大小 | `ComputeVarCharSerializedSize(...)` |
| `compare_varchar_from_row(arena, input)` | 比较 arena 序列化数据 vs 输入 (memcmp) | `CompareVarcharFromRow(...)` |
| `read_varchar_ptr(row, offset)` | 从行读 varchar 指针 | `*reinterpret_cast<char**>(row+offset)` |
| `read_varchar_from_arena(arena)` | 解析 arena 格式返回 `&[u8]` | 无独立函数 |
| `variable_type_serializer(rc, row, col, data)` | 单列 varchar → arena + 写指针到行 | `VariableTypeSerializer(...)` |
| `store_merged_varchar_columns(...)` | 多列 varchar → 一个连续 arena block | `BatchStoreMergedVarcharColumns(...)` |
| `get_all_merged_varchar_ptrs(...)` | 从 merged block 解析每列的指针 | `GetAllMergedVarcharPtrs(...)` |
| `store_key_one_row_from_decode(...)` | 存一行所有 key 列 (Step 3/5 用) | `StoreKeyOneRowFromDecode(...)` |
| `get_unequals_num_with_decode(...)` (独立函数) | 批量比较, 返回不等行数 | `GetUnequalsNumWithDecode(...)` |
| `compare_keys_with_decode(...)` | 单行全列精确比较 | `CompareKeysWithDecode(...)` |

#### `TaperColumnSerializeHandler` 结构体

| 字段 | 说明 |
|---|---|
| `map: TaperHashMap` | hash table |
| `rc: RowContainer` | 行存储 |
| `col_descs` | 列类型描述 |
| `col_offsets` | 每列在行中的 byte offset |
| `agg_offset` | agg state 在行中的 offset |
| `varchar_col_indices` | varchar 列的索引列表 |
| `varchar_slot_col_idx` | merged varchar 指针存放的列索引 |
| `use_merged` | 是否使用 merged varchar 模式 |
| `varchar_col_descs` | 每个 varchar 列的 (null_byte, null_mask) |
| `varchar_slot_col_offset` | merged varchar 指针的行内 offset |
| `groups: Vec<*const u8>` | 复用缓冲: 行指针数组 |
| `update_indices: Vec<u32>` | 复用缓冲: 碰撞行索引 |
| `merged_cache: Vec<*const u8>` | 复用缓冲: merged varchar 指针 cache |

| 方法 | 作用 |
|---|---|
| `new(columns, agg_size, capacity)` | 初始化 handler (计算布局, 建表) |
| `emplace_table_with_decode(hashes, columns, agg_values)` | 完整 5-step pipeline |
| `get_unequals_num_with_decode(indices, count, columns)` | Step 4: 批量比较 (方法版, `#[inline(never)]`) |

#### `emplace_table_with_decode` 内部流程

```
Step 1: 确保 hash table 容量 >= n
Step 2: emplace_batch_full → 新行调 store_key_one_row_from_decode
Step 3: (已在 Step 2 的 on_init 里完成)
Step 4: get_unequals_num_with_decode → batch_compare_*
Step 5: 对 unequal 行逐个 emplace (精确 key compare)
Step 6: 对 equal 行累加 agg
```

### `batch_compare.rs` — 批量比较

| 函数 | 作用 | SIMD | 对应 C++ |
|---|---|---|---|
| `batch_compare_decoded_i64_neon(...)` | 比较 i64 列, 2行/iter | NEON `vceqq_s64` | `SveBatchCompareInt64NoNull` (SVE) |
| `batch_compare_decoded_i64_scalar(...)` | 标量 fallback | 无 | 同上 scalar 分支 |
| `batch_compare_decoded_i64(...)` | 调度: aarch64→NEON, 其他→scalar | — | — |
| `batch_compare_decoded_i32_neon(...)` | 比较 i32 列, 4行/iter | NEON `vceqq_s32` | 无 (bench 只有 i64) |
| `batch_compare_decoded_i32_scalar(...)` | 标量 fallback | 无 | — |
| `batch_compare_decoded_i32(...)` | 调度 | — | — |
| `batch_compare_varchar_decoded(...)` | 比较单 varchar 列 (读 row ptr) | 无 | `BatchCompareVarcharDecodedNoNull` |
| `batch_compare_varchar_decoded_cached(...)` | 比较 varchar 列 (用 merged cache) | 无 | 同上 (cache 路径) |
| `build_merged_varchar_cache(...)` | 预填 merged varchar 指针 cache | 无 | `GetUnequalsNumWithDecode` 前半段 |

---

## 与 cpp-hash 的对应关系

| rust-hash 文件 | cpp-hash 文件 |
|---|---|
| `taper_hashmap.rs` | `include/taper_hashtable.h` |
| `chunk.rs` + `bitmask.rs` | `taper_hashtable.h` (内嵌) |
| `row_container.rs` | `include/row_container.h` + `include/simple_arena_allocator.h` |
| `column_marshaller.rs` | `include/column_marshaller.h` |
| `batch_compare.rs` | `include/column_marshaller.h` (内嵌在 `GetUnequalsNumWithDecode` 里) |
| `benches/hashmap_bench.rs` | `src/taper_bench.cpp` |

---

## 关键差异 vs cpp-hash

| 方面 | rust-hash | cpp-hash |
|---|---|---|
| Arena | 64KB 固定 block, 纯 bump, 无搬迁 | SimpleArenaAllocator: 4KB起步, 指数增长, 有 AllocateContinue 搬迁 |
| int64 compare SIMD | NEON `vceqq_s64` (2行/iter) | SVE gather-load (变长 lanes, ~8行/iter on 鲲鹏) |
| varchar compare | `slice ==` (memcmp) | `memcmp` |
| 内存分配器 | glibc malloc (默认) | glibc malloc (默认), OmniOperator 用 jemalloc |
| buffer 复用 | struct 成员, 跨调用保留容量 | 每次 EmplaceTableWithDecode 内 `resize` (但容量已够则不重分配) |
| 编译器 | LLVM 17 (rustc) | GCC 11 |
