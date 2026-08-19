# cpp-taper-bench 内存布局与管理结构

## 整体架构

```
TaperColumnSerializeHandler
├── TaperFlatHashTable (hash table, 存 hash → row ptr 映射)
├── RowContainer (行存储, 固定宽度行 + 变长 varchar arena)
└── SimpleArenaAllocator (统一底层内存池)
```

---

## 1. TaperFlatHashTable 内存布局

### Chunk 结构 (128 字节, cache line 对齐)

```
┌─────────────────────────────────────────────────────────────────┐
│  TaperHashTableChunk  (alignas(128), 128 bytes total)           │
├─────────┬───────────────────────────────┬───────────────────────┤
│  Tags   │         Keys                  │       Values          │
│  8 B    │     8 slots × 8 B = 64 B     │   8 slots × 6 B = 48 B│
├─────────┼───────────────────────────────┼───────────────────────┤
│ [T0|T1| │ [K0    |K1    |K2    |K3    | │ [V0   |V1   |V2   |  │
│  T2|T3| │  K4    |K5    |K6    |K7    ] │  V3   |V4   |V5   |  │
│  T4|T5| │                               │  V6   |V7   ]        │
│  T6|T7] │                               │                       │
└─────────┴───────────────────────────────┴───────────────────────┘
  8 + 64 + 48 = 120 bytes used, 8 bytes padding
```

- **Tag (1B)**: hash 的高 7 位 + 1 bit 标记 (0x80 = empty)
- **Key (8B)**: 完整的 int64 hash 值
- **Value (6B)**: RowContainer 行指针的低 48 位 (ROW_PTR_SIZE=6)

### Hash Table 整体

```
chunks_[] = [ Chunk0 | Chunk1 | Chunk2 | ... | ChunkN ]
             128B     128B     128B           128B

定位: chunkIdx = hash & lastChunkIdx_   (power-of-2 mask)
slot 查找: SWAR tag match on Tags[0..7]
```

---

## 2. RowContainer 内存布局

### 行 (Row) 结构

```
┌──────────────────────────────────────────────────────────────┐
│  Fixed Row (fixedRowSize_ bytes)                             │
├────────────┬────────────┬─────────┬──────────┬──────────────┤
│  Col0 data │  Col1 data │  ...    │ Null bits│  AggState    │
│  (8B ptr   │  (8B int64 │         │ (N bytes)│  (8B int64)  │
│   or 8B    │   or 8B    │         │          │              │
│   int64)   │   ptr)     │         │          │              │
└────────────┴────────────┴─────────┴──────────┴──────────────┘
```

**4str_0int 场景 (4 个 varchar 列):**
```
Row layout:
┌────────────────┬──────────┬──────────┐
│ VarcharSlot ptr│ Null bits│ AggState │
│  (8 bytes)     │ (N bytes)│ (8 bytes)│
└───────┬────────┴──────────┴──────────┘
        │
        ▼  (指向 arena 里的 merged varchar block)
┌──────────────────────────────────────────────┐
│ [col0: rowLenSize|len|data]                  │
│ [col1: rowLenSize|len|data]                  │
│ [col2: rowLenSize|len|data]                  │
│ [col3: rowLenSize|len|data]                  │
└──────────────────────────────────────────────┘
```

**Merged varchar**: 当 varchar 列 > 1 时, 所有 varchar 数据序列化在一个连续 arena block 里,
行里只存一个指针 (slot column = varcharColIndices[0] 的 offset)。

### 行批量分配

```
pool_.Allocate(kBatchSize * fixedRowSize_)
→ 一次性分配 kBatchSize 行的连续内存
→ memset 清零
→ batchPtr_ 逐行推进
```

---

## 3. SimpleArenaAllocator 内存管理

### Chunk 增长策略

```
分配请求 → GetNextSize() 决定 chunk 大小:

  chunks[0]: max(requested, 4KB)          ← minChunkSize
  chunks[1]: max(requested, 8KB)          ← × growthFactor(2)
  chunks[2]: max(requested, 16KB)
  chunks[3]: max(requested, 32KB)
  ...
  chunks[N]: max(requested, 512KB)        ← 达到 linearGrowthThreshold
  chunks[N+1]: 512KB                      ← 线性增长
  chunks[N+2]: 512KB
```

### Bump Pointer 分配

```
┌─────────────────────────────────────────────┐
│  Chunk (e.g. 4KB)                           │
│  [used used used used | ← availBuf_ → free] │
│                       ↑                     │
│                  next alloc here             │
└─────────────────────────────────────────────┘

Allocate(size):
  if availBytes_ >= size:
    ret = availBuf_; availBuf_ += size; availBytes_ -= size;
  else:
    AllocateChunk(GetNextSize(size));  // malloc 新 chunk
    ret = availBuf_; ...
```

### AllocateContinue 搬迁

```
场景: 序列化 merged varchar 时需要连续内存

AllocateContinue(size, start):
  if start == nullptr:
    → 普通 Allocate (新序列开始)
  else if 当前 chunk 剩余空间够:
    → 继续追加 (bump pointer)
  else:
    → AllocateChunk(新 chunk)
    → memcpy(旧数据 → 新 chunk 开头)  ← 搬迁！
    → start 更新为新位置
    → 继续追加
```

---

## 4. 数据流: EmplaceTableWithDecode

```
输入: hashes[], columns[], aggValues[]
         │
         ▼
┌─ Step 2: EmplaceBatch ──────────────────────────────┐
│  hash → chunkIdx → tag match → slot                 │
│  new slot: NewRow() → SetRowPtr(valBuf, row)        │
│  existing slot: groups[i] = GetRowPtr(valBuf)       │
│                 workingUpdateIndices.push(i)         │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─ Step 3: Store keys for new groups ─────────────────┐
│  BatchStoreMergedVarcharColumns:                     │
│    arena_alloc(totalSize) → serialize 4 varchar      │
│    row[slot_offset] = blockStart ptr                 │
│  StoreValue<int64_t>(row, offset, aggValues[i])     │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─ Step 4: GetUnequalsNumWithDecode ──────────────────┐
│  Build mergedVarcharCache_[]                         │
│  Per column:                                         │
│    Int64 → SveBatchCompareInt64NoNull (SVE)          │
│    Varchar → BatchCompareVarcharDecodedNoNull        │
│              → CompareVarcharFromRow → memcmp        │
│  Result: unequal indices swapped to front            │
└─────────────────────────────────────────────────────┘
         │
         ▼
┌─ Step 5: Re-emplace collisions ─────────────────────┐
│  for each unequal:                                   │
│    table->Emplace(hash, keyCmp, init, update)        │
└─────────────────────────────────────────────────────┘
```

---

## 5. 与 OmniOperator 的差异

| 方面 | OmniOperator | cpp-taper-bench | 影响 |
|------|-------------|-----------------|------|
| **Arena 分配器** | `Chunk::NewChunk(allocator, size)` 通过 Allocator 抽象 | 直接 `malloc(size)` | 无性能差异 |
| **RowContainer free list** | 有 `firstFreeRow` 链表复用 | 无 | bench 不做行删除, 不影响 |
| **行初始化** | 每行 `memset(row, 0, fixedRowSize)` | 整 batch `memset(batchPtr_, 0, sz)` | bench 稍快 (一次大 memset) |
| **allocations 追踪** | `allocations.push_back(row)` 每行记录 | 无 | bench 不需要 ListRows |
| **Hash table pool** | hash table chunk 从同一个 arena 分配 | hash table chunk 独立 `calloc` | 内存来源不同, 不影响热路径 |
| **DecodedVector 抽象** | 有 (支持 Flat/Dict/Const 三种 encoding) | 无 (直接裸指针输入, 等同于 Flat) | bench 是纯 flat, 逻辑等价 |
| **SVE int64 compare** | `SveBatchCompareNoNullDecoded<int64_t>` | `SveBatchCompareInt64NoNull` | 完全一致 |
| **Varchar compare** | `memcmp` | `memcmp` | 完全一致 |
| **colToVarcharPos_** | 有 (O(1) 查找) | 有 (O(1) 查找) | 完全一致 |
| **mergedVarcharCache_** | 有 | 有 | 完全一致 |

### 结论

cpp-taper-bench 的内存布局和管理逻辑与 OmniOperator 完全对齐。
差异仅在 OmniOperator 多了一些 bench 不需要的功能 (free list, allocations 追踪, DecodedVector 多态),
对 `EmplaceTableWithDecode` 热路径的行为没有影响。
