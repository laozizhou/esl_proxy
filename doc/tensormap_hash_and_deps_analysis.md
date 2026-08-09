# TensorMap 深度分析：Hash 计算与 Tensor 数据依赖检测

> 分析范围：`tensor.h`、`tensormap_core.h`、`tensormap.h`、`ring_buf.h`
> 版本：based on commit 18a55fe

---

## 目录

1. [整体架构概览](#1-整体架构概览)
2. [Tensor 数据结构](#2-tensor-数据结构)
3. [TmEntry 与 Hash Table 结构](#3-tmentry-与-hash-table-结构)
4. [Hash 计算机制深度分析](#4-hash-计算机制深度分析)
5. [Tensor 数据依赖检测机制深度分析](#5-tensor-数据依赖检测机制深度分析)
6. [生命周期与清理机制](#6-生命周期与清理机制)
7. [上层编排胶水层](#7-上层编排胶水层)
8. [性能特征与设计考量](#8-性能特征与设计考量)
9. [实际使用场景](#9-实际使用场景)

---

## 1. 整体架构概览

```
┌──────────────────────────────────────────────────────────────┐
│                    tensormap.h (编排胶水层)                     │
│  tm_in / tm_out / tm_inout  ───→  tm_pending_push             │
│  tm_submit → ①sync_cleanup → ②lookup(依赖) → ③insert(声明)     │
│                                                                  │
│  依赖：ring_buf.h (add_tensor_addr / add_predecessors)         │
├──────────────────────────────────────────────────────────────┤
│                 tensormap_core.h (核心引擎层)                    │
│  Hash Table (bucket链)  +  Overlap检测 (tm_check_overlap)     │
│  生命周期管理 (sync / cleanup_retired / lazy invalidation)     │
├──────────────────────────────────────────────────────────────┤
│                     tensor.h (数据结构层)                        │
│  Tensor (128B, 2 cache lines)  +  view() 子视图机制            │
└──────────────────────────────────────────────────────────────┘
```

**设计目标**：在编译期（orchestration阶段），通过分析 tensor 的 buffer_addr 和形状/偏移信息，自动检测数据依赖关系（producer-consumer），替代手写依赖声明。

---

## 2. Tensor 数据结构

### 2.1 内存布局（128B，两个64B Cache Line）

```c
struct Tensor {
    /* === Cache line 1 (64B) — hot path === */
    uint64_t buffer_addr;       // offset 0:  基地址
    uint64_t buffer_size;       // offset 8:  缓冲区总大小(字节)
    uint64_t owner_task_id;     // offset 16: 所有者 task ID
    uint64_t start_offset;      // offset 24: 起始元素偏移
    int32_t  version;           // offset 32: 版本号
    uint32_t ndims;             // offset 36: 维度数 (1~5)
    uint8_t  dtype;             // offset 40: 数据类型 (2=BF16, 4=FP32/INT32)
    uint8_t  manual_dep;        // offset 41: 是否手动依赖
    uint8_t  is_contiguous;     // offset 42: 是否连续存储
    uint8_t  _pad_cl1;          // offset 43: padding
    uint32_t shapes[5];         // offset 44: 各维度大小 (共20B)

    /* === Cache line 2 (64B) — warm path === */
    uint64_t extent_elem_cache; // offset 64: extent(element数)，行优先跨度
    uint32_t strides[5];        // offset 72: 各维度stride (共20B)
    uint8_t  _pad_cl2[36];      // offset 92: padding
} __attribute__((aligned(64))); // 总计 128B
```

### 2.2 关键字段语义

| 字段 | 语义 | 在依赖检测中的作用 |
|------|------|-------------------|
| `buffer_addr` | 内存基地址 | **Hash Key**，唯一标识逻辑 buffer |
| `start_offset` | 相对于 buffer_addr 的元素偏移 | 确定 tensor 在 buffer 中的起始位置 |
| `shapes[]` | 各维度大小 | 计算 tensor 的 extent |
| `strides[]` | 各维度步长（行优先） | 判断 layout 兼容性 |
| `dtype` | 元素类型大小 | 将偏移/大小统一到元素粒度 |
| `is_contiguous` | 是否连续 | 快速路径：extent = numel |
| `extent_elem_cache` | extent（元素跨度） | 非连续 tensor 的 extent |
| `version` | 版本号 | **L1 快速拒绝**：in.version > entry.version → OTHER |
| `buffer_size` | 缓冲区总大小 | 反推 ref_shape[0]（存储空间维度0上限） |

### 2.3 View 机制

```c
static inline Tensor view_at(const Tensor *t, uint32_t off0, uint32_t off1,
                             uint32_t n0, uint32_t n1) {
    Tensor v = *t;
    v.ndims = 2;
    v.start_offset += off0 * v.strides[0] + off1 * v.strides[1];
    v.shapes[0] = n0;
    v.shapes[1] = n1;
    v.is_contiguous = v.strides[1] == 1 && v.strides[0] == v.shapes[1];
    v.extent_elem_cache = (n0-1)*strides[0] + (n1-1)*strides[1];
    return v;
}
```

- **不修改 buffer_addr**：view 共享同一底层 buffer
- **调整 start_offset**：反映子视图在 buffer 中的偏移
- **仅支持 2D 子视图**：`view(t, off0, off1, n0, n1)` 宏

---

## 3. TmEntry 与 Hash Table 结构

### 3.1 TmEntry 内存布局（128B，与 Tensor Cache Line 1 字节兼容）

```c
typedef struct {
    /* === cache line 1 (64B) — 与 Tensor 前64B 逐字节一致 === */
    uint64_t base_addr;         // = Tensor.buffer_addr
    int32_t  next_in_bucket;    // 覆盖 Tensor.buffer_size 低32位
    int32_t  _pad_nb;           // 覆盖 Tensor.buffer_size 高32位
    uint64_t producer_id;      // 覆盖 Tensor.owner_task_id
    uint64_t start_offset;     // = Tensor.start_offset
    int32_t  version;           // = Tensor.version
    uint32_t ndims;            // = Tensor.ndims
    uint8_t  dtype;            // = Tensor.dtype
    uint8_t  manual_dep;       // = Tensor.manual_dep
    uint8_t  is_contiguous;    // = Tensor.is_contiguous
    uint8_t  __padding1__;
    uint32_t shapes[5];        // = Tensor.shapes

    /* === cache line 2 (64B) — hash table 链表指针 === */
    int32_t  prev_in_bucket;   // bucket 内前驱
    int32_t  next_in_task;     // 同 task slot 内后继
    int32_t  prev_in_task;     // 同 task slot 内前驱
    int32_t  bucket_index;     // 所属 bucket
    uint64_t extent_elem_cache;// = Tensor.extent_elem_cache
    uint32_t strides[5];       // = Tensor.strides
    uint32_t _line2_pad[5];
} TmEntry;
```

### 3.2 关键设计：Cache Line 1 的字节兼容

```
┌──────────────────┬─────────────────────────────┐
│   Tensor         │         TmEntry             │
│   offset 0-63    │       offset 0-63           │
├──────────────────┼─────────────────────────────┤
│ buffer_addr (8B) │ base_addr (8B)    ← 相同     │
│ buffer_size (8B) │ next_in_bucket(4)+_pad(4)   │
│ owner_task_id(8B)│ producer_id (8B)  ← 覆盖     │
│ start_offset(8B) │ start_offset (8B) ← 相同     │
│ version (4B)     │ version (4B)      ← 相同     │
│ ndims (4B)       │ ndims (4B)        ← 相同     │
│ dtype (1B)       │ dtype (1B)        ← 相同     │
│ manual_dep (1B)  │ manual_dep (1B)   ← 相同     │
│ is_contiguous(1B)│ is_contiguous(1B) ← 相同     │
│ shapes (20B)     │ shapes (20B)      ← 相同     │
└──────────────────┴─────────────────────────────┘
```

**性能优化**：`tm_copy_tensor_to_entry()` 直接用 `memcpy(e, t, 64)` 复制 Tensor 前 64B 到 TmEntry。`buffer_size` 和 `owner_task_id` 被覆盖为 `next_in_bucket`/`_pad_nb`/`producer_id`，但 `tm_link_entry()` 随后会正确设置 `producer_id` 和链表指针。

### 3.3 Hash Table 内存布局

```
┌──────────────────────────────────────────────────────────┐
│ TmHeader (含 TmConfig, offsets, 元数据)                    │
├──────────────────────────────────────────────────────────┤
│ buckets[] : int32_t[num_buckets]      ← 每个bucket指向首个entry  │
├──────────────────────────────────────────────────────────┤
│ (对齐到 128B)                                              │
├──────────────────────────────────────────────────────────┤
│ pool[]    : TmEntry[pool_size]         ← entry对象池      │
├──────────────────────────────────────────────────────────┤
│ free_list[] : int32_t[pool_size]       ← 空闲entry索引     │
├──────────────────────────────────────────────────────────┤
│ task_heads[ring][] : int32_t[task_window[ring]]  ← 每task slot的entry链头 │
└──────────────────────────────────────────────────────────┘
```

### 3.4 复合 ID 编码

```c
static inline uint64_t tm_make_id(uint32_t ring, uint32_t local) {
    return ((uint64_t)ring << 32) | local;
}
// ring: 高32位，实现中固定为0（num_rings=1）
// local: 低32位 = task_id
```

---

## 4. Hash 计算机制深度分析

### 4.1 Hash 函数

```c
static inline uint32_t tm_hash(const TmTensorMap *self, uint64_t key) {
    key *= 0x9E3779B97F4A7C15ULL;  // 黄金比例倒数 (2^64 / φ)
    return (uint32_t)(key >> (64 - __builtin_ctz(tm_hdr(self)->cfg.num_buckets)));
}
```

### 4.2 算法分解

**Step 1: 乘法哈希**
- 常量 `0x9E3779B97F4A7C15ULL` = `2^64 / φ` ≈ `0x9E3779B97F4A7C100`
- 这是 Knuth 乘法哈希法的标准常数，具有良好的雪崩效应
- 将 64 位 key 乘以该常数，高位包含充分的随机性

**Step 2: 提取高位作为 bucket index**
- `__builtin_ctz(num_buckets)` = `log2(num_buckets)`（因为 num_buckets 必须是 2 的幂）
- `key >> (64 - log2(N))` 提取乘法结果的高 `log2(N)` 位
- 例如 num_buckets=2048=2^11: ctz=11, 右移53位，取高11位

**为什么用高位而非取模**：
- 乘法哈希的高位具有更好的分布均匀性
- 右移操作比取模快
- 要求 num_buckets 必须是 2 的幂

### 4.3 Hash Key

```c
// 插入时
tm_link_entry(self, idx, t->buffer_addr, ...);
    const uint32_t b = tm_hash(self, addr);  // addr = t->buffer_addr

// 查找时
tm_lookup_tensor(self, t, ...);
    const uint32_t b = tm_hash(self, t->buffer_addr);
```

**Hash Key = `buffer_addr`（64位物理/虚拟地址）**

其他 tensor 属性（start_offset, shapes, strides, dtype）不参与 hash，在重叠检测阶段处理。

### 4.4 冲突解决：链地址法（Separate Chaining）

```c
// Bucket 链：next_in_bucket / prev_in_bucket 双向链表
e->next_in_bucket = bk[b];    // 插入到链表头部
if (bk[b] != -1) pl[bk[b]].prev_in_bucket = idx;
bk[b] = idx;

// 查找时遍历整个 bucket 链
while (cur != -1) {
    TmEntry *e = &pl[cur];
    if (tm_entry_valid(self, e) && e->base_addr == t->buffer_addr) {
        // 对每个匹配 base_addr 的 entry 进行重叠检测
    }
    cur = pl[cur].next_in_bucket;  // 安全的迭代（提前保存next）
}
```

**关键细节**：查找时遍历过程中，先保存 `next = pl[cur].next_in_bucket`，再处理当前 entry，这允许回调中安全删除当前 entry（remove in callback）。

### 4.5 Hash Table 配置参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `TMD_NUM_BUCKETS` | 2048 | bucket 数量（必须是2的幂） |
| `TMD_POOL_SIZE` | 8192 | entry 池大小 |
| `TMD_TASK_WINDOW` | 65536 | task window 大小 |
| `TM_CLEANUP_INTERVAL` | 64 | 清理间隔 |

**配置文件示例** (test中):
```c
cfg.num_buckets = 16;     // 小测试用
cfg.pool_size = 64;
cfg.num_rings = 1;
cfg.task_window[0] = 64;
```

**性能测试配置**:
```c
cfg.num_buckets = 65536;  // 大负载用
cfg.pool_size = 200000;
```

---

## 5. Tensor 数据依赖检测机制深度分析

### 5.1 三层检测架构

```
┌─────────────────────────────────────────────────────────────────┐
│                      tm_check_overlap(in, entry)                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  L0: 版本快速拒绝                                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ if (in->version > e->version) → TM_OVERLAP_OTHER        │    │
│  │ 消费者版本更高意味着生产者数据已过时，无法精确匹配       │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                    │
│                              ▼                                    │
│  L1: 元素区间检查 (element-range)                                 │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ in_begin  = in->start_offset                             │    │
│  │ in_end    = in->start_offset + extent(in)                │    │
│  │ ent_begin = e->start_offset                              │    │
│  │ ent_end   = e->start_offset + extent(e)                  │    │
│  │                                                           │    │
│  │ if (!(in_end > ent_begin && ent_end > in_begin))         │    │
│  │     → TM_OVERLAP_NONE  ← 无重叠，快速退出                │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                    │
│                              ▼                                    │
│  L2: 逐维度精确检测 (per-dimension row-major)                     │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ 适用于: dtype相同, ndims相同, strides逐维匹配的情况       │    │
│  │                                                           │    │
│  │ 步骤:                                                     │    │
│  │ 1. 校验 strides 整除性、最内维 stride=1                  │    │
│  │ 2. 反推 ref_shape[i] = stride[i-1]/stride[i]            │    │
│  │ 3. 将 start_offset 分解为多维坐标 (offset decomposition) │    │
│  │ 4. 校验坐标+shapes 不超过 ref_shape                       │    │
│  │ 5. 逐维度检测区间重叠                                     │    │
│  │ 6. 判断全覆盖 (COVERED)  vs  部分重叠 (OTHER)            │    │
│  └─────────────────────────────────────────────────────────┘    │
│                              │                                    │
│                              ▼                                    │
│  L3: 退化处理 (fallback)                                          │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │ 任何无法走 L2 精确路径的情况 → TM_OVERLAP_OTHER          │    │
│  │ (dtype不同 / ndims不同 / strides不匹配 / 坐标不对齐 等)  │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                   │
│  返回三种值:                                                      │
│  - TM_OVERLAP_NONE    = 0  (无重叠，无依赖)                      │
│  - TM_OVERLAP_COVERED = 1  (消费者完全覆盖生产者，inout可安全删除) │
│  - TM_OVERLAP_OTHER   = 2  (部分重叠或其他情况，产生依赖)          │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 版本号机制（L0 快速拒绝）

```c
if (in->version > e->version) {
    return TM_OVERLAP_OTHER;
}
```

**语义**：
- `version = 0`：默认，表示未版本化的 tensor
- `in->version > e->version`：消费者期望的版本高于生产者写入的版本
  - 这意味着生产者写入的是旧版本数据
  - 无法精确做 overlap 分析，保守返回 OTHER（当作有依赖）

**测试用例验证**：
```c
// test_version_guard
Tensor prod = tensor_1d(0xB100, 0, 64, 64);
prod.version = 0;
tm_insert_tensor(&map, &prod, 1);

Tensor probe = tensor_1d(0xB100, 0, 64, 64);
probe.version = 1;                       // 消费者版本更高
HitSink s = collect(&map, probe);
assert(s.statuses[0] == TM_OVERLAP_OTHER); // 返回 OTHER 而非 NONE
```

### 5.3 L1 元素区间快速拒绝

```c
// 计算extent
uint64_t extent_elem;
if (in->is_contiguous) {
    extent_elem = 1;
    for (uint32_t i = 0; i < in->ndims; i++) extent_elem *= in->shapes[i];
} else {
    extent_elem = in->extent_elem_cache;
}

const uint64_t in_begin  = in->start_offset;
const uint64_t in_end    = in->start_offset + extent_elem;
const uint64_t ent_begin = e->start_offset;
const uint64_t ent_end   = e->start_offset + tm_entry_extent_elem(e);

if (!(in_end > ent_begin && ent_end > in_begin))
    return TM_OVERLAP_NONE;  // 无重叠
```

**区间重叠判断标准公式**：
```
in_end > ent_begin && ent_end > in_begin
```
等价于：`max(in_begin, ent_begin) < min(in_end, ent_end)`

**测试用例**：
```c
// 相邻但不重叠 → NONE
Tensor b = tensor_1d(0x2000, 0, 128, 256);     // [0, 128)
s = collect(&map, tensor_1d(0x2000, 128, 128, 256));  // [128, 256)
assert(s.count == 0);  // 相邻不重叠

// 偏移重叠 → OTHER
Tensor c = tensor_1d(0x3000, 0, 128, 256);     // [0, 128)
s = collect(&map, tensor_1d(0x3000, 64, 128, 256));   // [64, 192)
assert(s.statuses[0] == TM_OVERLAP_OTHER);     // 部分重叠

// 不同 base_addr → NONE (hash 桶不同，根本不会匹配)
Tensor d = tensor_1d(0x4000, 0, 128, 128);
s = collect(&map, tensor_1d(0x5000, 0, 128, 128));
assert(s.count == 0);
```

### 5.4 L2 逐维度精确检测

#### 5.4.1 2D 快速路径（qwen3 / paged-attention 专用）

```c
if (in->ndims == 2u && e->ndims == 2u) {
    // ... 2D 专用检测
}
```

**2D 路径核心步骤**：

1. **stride 一致性检查**：
   ```c
   if (in->strides[0] != e->strides[0] || in->strides[1] != e->strides[1])
       return TM_OVERLAP_OTHER;
   if (e->strides[1] != 1u) return TM_OVERLAP_OTHER;
   if (e->strides[0] % e->strides[1] != 0u) return TM_OVERLAP_OTHER;
   ```

2. **推导 ref_shape（存储空间逻辑维度）**：
   ```c
   const uint32_t ref_shape1 = e->strides[0] / e->strides[1];  // dim1 大小
   const uint32_t numel_storage = in->buffer_size / in->dtype;   // 总元素数
   const uint32_t ref_shape0 = numel_storage / e->strides[0];   // dim0 大小
   ```
   - `ref_shape1` 从 stride 关系推导：`stride[0] = shape[1] * stride[1]`
   - `ref_shape0` 从总存储大小反推

3. **坐标分解**：
   ```c
   const uint32_t in_off0 = in->start_offset / stride0;
   const uint32_t in_off1 = (in->start_offset % stride0) / stride1;
   // 同理计算 ent_off0, ent_off1
   // 校验余数为0（偏移对齐）
   ```

4. **边界检查**：
   ```c
   if (in_off0 + in->shapes[0] > ref_shape0) return TM_OVERLAP_OTHER;
   if (in_off1 + in->shapes[1] > ref_shape1) return TM_OVERLAP_OTHER;
   ```

5. **逐维度重叠判定 + 覆盖判断**：
   ```c
   // dim0 重叠检测
   if (!(in_a1_0 > ent_off0 && ent_b1_0 > in_off0)) return TM_OVERLAP_NONE;
   bool input_contains_entry = (in_off0 <= ent_off0 && ent_b1_0 <= in_a1_0);
   
   // dim1 重叠检测
   if (!(in_a1_1 > ent_off1 && ent_b1_1 > in_off1)) return TM_OVERLAP_NONE;
   if (!(in_off1 <= ent_off1 && ent_b1_1 <= in_a1_1)) input_contains_entry = false;
   
   return input_contains_entry ? TM_OVERLAP_COVERED : TM_OVERLAP_OTHER;
   ```

**2D 覆盖判定**：只有当两个维度上消费者区间都**完全包含**生产者区间时，才返回 COVERED。

**2D Qwen3 测试用例**（列切片不重叠）：
```c
// gate_tile dim1 column slice — 验证列相邻切片不重叠
Tensor tile = tensor_from_base_layout(0xE0000, {16, 17408}, 2, FLOAT32);
Tensor prod0 = view(tile, 0, 0, 16, 512);    // 列 [0, 512)
Tensor cons1 = view(tile, 0, 512, 16, 512);   // 列 [512, 1024)
assert(tm_check_overlap(&cons1, &prod0) == TM_OVERLAP_NONE); // 不重叠
```

**2D Row Tile 测试用例**：
```c
// 2D row tile — 行区间重叠
Tensor prod = tensor_2d_rows(base, 96, 5120, 16, 16);  // 行 [16, 32)
// 完全匹配 → COVERED
hit = collect(&map, tensor_2d_rows(base, 96, 5120, 16, 16));
assert(hit.statuses[0] == TM_OVERLAP_COVERED);
// 不相交 → NONE
miss = collect(&map, tensor_2d_rows(base, 96, 5120, 0, 16));
assert(miss.count == 0);
// 部分重叠 → OTHER
part = collect(&map, tensor_2d_rows(base, 96, 5120, 8, 16));
assert(part.statuses[0] == TM_OVERLAP_OTHER);
```

#### 5.4.2 通用 N 维路径

对任意 ndims（1~5），通用路径执行类似逻辑：

```c
// 1. 校验 strides 整除性
for (uint32_t i = 1; i < in->ndims; i++) {
    if (e->strides[i-1] % e->strides[i] != 0u) return TM_OVERLAP_OTHER;
}

// 2. 反推 ref_shape
for (uint32_t i = 1; i < in->ndims; i++)
    ref_shapes[i] = e->strides[i-1] / e->strides[i];
ref_shapes[0] = numel_storage / e->strides[0];

// 3. 坐标分解（逐维）
for (uint32_t i = 0; i < in->ndims; i++) {
    in_offsets[i]  = in_remain / e->strides[i];
    ent_offsets[i] = ent_remain / e->strides[i];
    in_remain  %= e->strides[i];
    ent_remain %= e->strides[i];
}

// 4. 边界检查 + 逐维重叠 + 覆盖判定
```

### 5.5 三种检测结果的语义

| 结果 | 语义 | 在依赖检测中的行为 |
|------|------|-------------------|
| `TM_OVERLAP_NONE` | 无数据重叠 | **不产生依赖**，callback 仍被调用 |
| `TM_OVERLAP_COVERED` | 消费者完全覆盖生产者数据 | **产生依赖**；若 inout，**删除生产者 entry**（数据被完全覆写） |
| `TM_OVERLAP_OTHER` | 部分重叠或无法精确判定 | **产生依赖**；不删除 entry（部分读取/写入） |

### 5.6 完整依赖检测流程

```
tm_submit(tid) 执行流程:
┌──────────────────────────────────────────────────────────────────┐
│ STEP 0: 同步与清理                                                │
│                                                                   │
│ tm_sync_tensormap(                                               │
│     &g_tm_deps.map, 0,                                           │
│     g_min_uncomplete_task,   ← 全局最小未完成任务（原子读取）     │
│     tid);                                                         │
│                                                                   │
│ 效果：推进 last_alive 水印，清理已完成的 producer entry           │
│       条件：last_alive - old >= 64 或 slot 回绕                  │
│                                                                   │
├──────────────────────────────────────────────────────────────────┤
│ STEP 1: 遍历 pending IO，对 IN/INOUT 进行查找                     │
│                                                                   │
│ for (i = 0; i < pend_n; i++) {                                   │
│     if (pend[i].kind & TM_PEND_IN) {                             │
│         ctx.is_inout = (kind == TM_PEND_INOUT);                  │
│         tm_lookup_tensor(&map, pend[i].t,                        │
│                          tm_collect_on_match, &ctx);             │
│     }                                                             │
│ }                                                                 │
│                                                                   │
│ tm_lookup_tensor 内部:                                            │
│ ┌──────────────────────────────────────────────────────┐         │
│ │ 1. hash(buffer_addr) → bucket index b                │         │
│ │ 2. 遍历 bucket 链:                                   │         │
│ │    for (cur = buckets[b]; cur != -1; cur = next) {   │         │
│ │        if (!tm_entry_valid(map, e)) continue;        │         │
│ │        if (e->base_addr != t->buffer_addr) continue; │         │
│ │        overlap = tm_check_overlap(t, e);             │         │
│ │        if (overlap != NONE) callback(e, overlap);    │         │
│ │    }                                                 │         │
│ └──────────────────────────────────────────────────────┘         │
│                                                                   │
│ tm_collect_on_match 内部:                                         │
│ ┌──────────────────────────────────────────────────────┐         │
│ │ 1. 提取 producer_id 的 local 部分                    │         │
│ │ 2. 去重：如果 pred 已在列表中，跳过                  │         │
│ │ 3. 如果不是自身（consumer != producer），加入列表    │         │
│ │ 4. 如果是 inout + COVERED → tm_remove(entry)         │         │
│ └──────────────────────────────────────────────────────┘         │
│                                                                   │
├──────────────────────────────────────────────────────────────────┤
│ STEP 2: 注册依赖关系                                               │
│                                                                   │
│ if (ctx.pn > 0) {                                                 │
│     add_predecessors(tid, ctx.preds, ctx.pn, 0);                  │
│ }                                                                 │
│                                                                   │
│ add_predecessors 内部:                                            │
│ ┌──────────────────────────────────────────────────────┐         │
│ │ 1. 获取 predecessor_list[tid]                        │         │
│ │ 2. 对每个 pred:                                       │         │
│ │    if (pred < min_uncomplete_task) continue; // 跳过  │         │
│ │    *ring_tail++ = pred; // 原子递增 tail              │         │
│ │ 3. 更新 predecessor_list[tid].cnt                     │         │
│ └──────────────────────────────────────────────────────┘         │
│                                                                   │
├──────────────────────────────────────────────────────────────────┤
│ STEP 3: 遍历 pending IO，对 OUT/INOUT 进行插入                    │
│                                                                   │
│ for (i = 0; i < pend_n; i++) {                                   │
│     if (pend[i].kind & TM_PEND_OUT) {                            │
│         tm_insert_tensor(&map, pend[i].t, tid);                  │
│     }                                                             │
│ }                                                                 │
│                                                                   │
│ tm_insert_tensor 内部:                                            │
│ ┌──────────────────────────────────────────────────────┐         │
│ │ 1. tm_new_entry() → 分配 entry（free_list 或 新分配）│         │
│ │ 2. tm_copy_tensor_to_entry(t, entry) → memcpy 64B    │         │
│ │ 3. tm_link_entry(idx, buffer_addr, producer_id):     │         │
│ │    - hash(buffer_addr) → bucket                       │         │
│ │    - 插入 bucket 链头部                                │         │
│ │    - 插入 task slot 链头部                             │         │
│ └──────────────────────────────────────────────────────┘         │
│                                                                   │
├──────────────────────────────────────────────────────────────────┤
│ STEP 4: 清空 pending 队列                                         │
│                                                                   │
│ tm_pending_clear()  // pend_n = 0                                │
└──────────────────────────────────────────────────────────────────┘
```

### 5.7 关键设计决策：依赖检测方向性

```
          tm_in(tensor)               tm_out(tensor)
消费者 ─────────────────→ 查找生产者      ─────────────────→ 注册为未来生产者
(读取数据)                  (谁写过这块?)   (写入数据)            (后续消费者能找到我)

          tm_inout(tensor)
消费者 ─────────────────→ 查找生产者 + 注册为生产者
(先读后写)                 COVERED时删除旧生产者

          tm_in_ro / tm_out_ro / tm_inout_ro
                            只记录 buffer_addr 到 ring_buf
                            但不参与依赖检测（只读常量数据如 weight）
```

---

## 6. 生命周期与清理机制

### 6.1 Lazy Invalidation（惰性失效）

```c
static inline bool tm_entry_valid(const TmTensorMap *self, const TmEntry *e) {
    return (int32_t)tm_local_of(e->producer_id) >=
           tm_hdr(self)->last_alive[tm_ring_of(e->producer_id)];
}
```

- 不主动删除已完成任务的 entry
- 在查找时检查 `entry.producer_id >= last_alive`
- `producer_id < last_alive` → 生产者已完成，entry 逻辑失效（被忽略但不释放）

### 6.2 批量清理

```c
static inline void tm_cleanup_retired(TmTensorMap *self, uint32_t ring,
    int32_t old_alive, int32_t new_alive) {
    for (int32_t local = old_alive; local < new_alive; local++) {
        uint32_t slot = local & mask;
        // 遍历该 slot 的 task 链表
        int32_t cur = th[slot];
        while (cur != -1) {
            next = pl[cur].next_in_task;
            tm_free_entry(self, cur);  // 从 bucket 链移除 + 归还 free_list
            cur = next;
        }
        th[slot] = -1;
    }
}
```

**清理触发时机** (`tm_sync_tensormap`):
```c
// 条件1: 积累足够多的已完成任务（每64个）
if (last_alive - old >= (int32_t)TM_CLEANUP_INTERVAL)

// 条件2: slot 回绕（task_window 是 2 的幂，slot = local & mask）
// 当新任务的 slot 与最后一次清理的 slot 相同时
if (slot == old_slot)

// 满足任一条件即触发清理
```

**测试用例验证**：
```c
// test_sync_interval_gating: 直到累够64个才触发清理
for (uint32_t i = 0; i < 8; i++) tm_insert_tensor(...);
tm_sync_tensormap(&map, 0, 5, 1);
assert(tm_hdr(&map)->last_cleanup[0] == 0); // 未达阈值，未清理

tm_sync_tensormap(&map, 0, TM_CLEANUP_INTERVAL, TM_CLEANUP_INTERVAL);
assert(tm_hdr(&map)->last_cleanup[0] == 64);  // 触发清理
assert(tm_hdr(&map)->free_num >= 8);           // entries 被释放
```

### 6.3 内存回收

```c
static inline void tm_free_entry(TmTensorMap *self, int32_t idx) {
    // 1. 从 bucket 双向链表中摘除
    if (e->prev_in_bucket == -1)
        bk[e->bucket_index] = e->next_in_bucket;
    else
        pl[e->prev_in_bucket].next_in_bucket = e->next_in_bucket;
    if (e->next_in_bucket != -1)
        pl[e->next_in_bucket].prev_in_bucket = e->prev_in_bucket;

    // 2. 放入 free_list
    tm_free_list(self)[tm_hdr(self)->free_num++] = idx;

    // 3. 清理指针
    e->bucket_index = -1;
    e->next_in_bucket = e->prev_in_bucket = -1;
    e->next_in_task = e->prev_in_task = -1;
}
```

---

## 7. 上层编排胶水层

### 7.1 全局上下文

```c
typedef struct TmDepsState {
    TmTensorMap map;
    _Alignas(TM_ENTRY_ALIGN) uint8_t buf[TMD_BUF_BYTES];  // 内联缓冲区
    TmPendingSlot pend[TM_PENDING_MAX_IO];                 // 最多16个IO声明
    int pend_n;                                             // 当前 pending 计数
} TmDepsState;

static TmDepsState g_tm_deps;  // 全局单例
```

### 7.2 IO 声明 API

```c
// 输入（读取）：查找生产者，建立依赖
#define tm_in(tid, t)       tm_in_ptr(tid, &(t))      // kind = TM_PEND_IN
#define tm_in_ro(tid, t)    tm_in_ro_ptr(tid, &(t))   // 只读常量，不参与依赖检测

// 输出（写入）：注册为未来生产者
#define tm_out(tid, t)      tm_out_ptr(tid, &(t))     // kind = TM_PEND_OUT
#define tm_out_ro(tid, t)   tm_out_ro_ptr(tid, &(t))  // 只读常量输出

// 输入输出（先读后写）：同时查找 + 注册
#define tm_inout(tid, t)    tm_inout_ptr(tid, &(t))   // kind = TM_PEND_INOUT
#define tm_inout_ro(tid, t) tm_inout_ro_ptr(tid, &(t))// 只读常量

// 提交
#define tm_submit(tid)      tm_submit_ptr(tid)
```

所有宏都调用 `add_tensor_addr(tid, t->buffer_addr)` 将地址记录到 `ring_buf` 的 `g_basic_buf[task_id].data[]` 中。

### 7.3 与 Ring Buffer 的交互

```c
// ring_buf.h
static inline void add_tensor_addr(uint32_t task_id, uint64_t addr) {
    int idx = g_basic_buf[task_id & RING_MASK].tensor_cnt++;
    g_basic_buf[task_id & RING_MASK].data[idx] = addr;
}

static int add_predecessors(uint32_t task_id, uint32_t target[], uint32_t n, uint32_t start) {
    // 跳过已完成的 producer
    if (target[i] < min_uncomplete_task) continue;
    // 原子追加到 predecessor ring buffer
    uint32_t* idx = atomic_fetch_add(&g_predecessor_ring.tail, 1);
    *idx = target[i];
}
```

---

## 8. 性能特征与设计考量

### 8.1 时间复杂度

| 操作 | 时间复杂度 | 说明 |
|------|-----------|------|
| `tm_hash` | O(1) | 一次乘法 + 一次位移 |
| `tm_insert_tensor` | O(1) amortized | hash + 链表头插入 |
| `tm_lookup_tensor` | O(L) | L = bucket 链长度，通常很短 |
| `tm_check_overlap` | O(ndims) | ndims ≤ 5 |
| `tm_submit` | O(P × L × D) | P=输入数(≤16), L=链长, D=维度(≤5) |
| `tm_cleanup_retired` | O(K) | K=被清理的 entry 数 |

### 8.2 空间效率

- 每个 TmEntry 128B，pool_size=8192 → 1MB entry 存储
- 全局内联缓冲区 `TMD_BUF_BYTES`
- pending 队列最多 16 个槽位

### 8.3 Cache 优化

1. **Entry cache line 1 与 Tensor 字节兼容**：`tm_copy_tensor_to_entry` 用单条 `memcpy(e, t, 64)` 完成
2. **Cache line 分离**：line 1 (hot, 查找时访问) / line 2 (warm, 指针链)
3. **64B 对齐**：避免 false sharing

### 8.4 并发安全

- 全局单例 `g_tm_deps`：编排阶段单线程执行，无需锁
- `add_predecessors` 使用原子操作确保 ring buffer 并发写入安全
- `g_min_uncomplete_task` 原子读取

### 8.5 可重定位性

```c
static inline void tm_attach(TmTensorMap *self, void *base) {
    tm_bind(self, base);  // 重绑定内部指针
}
```
- Entry 间用 pool index 而非指针链接
- 支持 memcpy 到新位置后重新绑定（test 验证通过）

---

## 9. 实际使用场景

### 9.1 Qwen3 Decoder (qwen3_dynamic_tensormap.h)

```
tm_deps_init();

// Task 0: RMS Norm (输入 ext_hidden_states, 输出 normed_tile)
tm_in_ro(g_task_id, ext_hidden_states);
tm_out(g_task_id, normed_tile);
tm_submit(g_task_id); g_task_id++;

// Task 1~N: Q/K/V Projections (并行，消费 normed_tile)
for each batch tile:
    tm_in(g_task_id, normed_tile);     // ← 查找到 Task 0
    tm_in_ro(g_task_id, ext_wq);
    tm_out(g_task_id, q_piece);
    tm_submit(g_task_id); g_task_id++;

// Attention Online Softmax (inout: 读写同一个buffer)
tm_inout(g_task_id, attn_out_piece);  // ← COVERED时删除旧生产者
tm_submit(g_task_id); g_task_id++;

// ...
```

### 9.2 Paged Attention (paged_attention_unroll.h)

```
// S = Q*K^T
tm_in_ro(g_task_id, ext_block_table);
tm_out(g_task_id, sij_buf);
tm_submit(g_task_id);

// P = softmax(S)
tm_in(g_task_id, sij_buf);           // ← 查找到上一个 task
tm_out(g_task_id, pij_buf);
tm_submit(g_task_id);

// O += P*V
tm_in(g_task_id, pij_buf);           // ← 查找到上一个 task
tm_in_ro(g_task_id, ext_value_cache);
tm_out(g_task_id, oi_new);
tm_submit(g_task_id);

// Rescale & merge
tm_in(g_task_id, oi_new);            // ← 查找到上一个 task
tm_inout(g_task_id, oi);             // ← 先读后写，COVERED时删除
tm_submit(g_task_id);
```

### 9.3 依赖检测的正确性保证

```
生产者 Task A:          消费者 Task B:
tm_out(X)               tm_in(X)
tm_submit(A)            tm_submit(B)
     │                      │
     │  插入 X 到 map       │  lookup X → 找到 A → add_predecessors(B, [A])
     │                      │
     ▼                      ▼
  后续 Task C 消费 X → 也会找到 A 或 B（取决于 A 是否已清理）
```

---

## 总结

### Hash 计算特点

1. **Key**: `buffer_addr`（64位基地址）
2. **算法**: 乘法哈希（Knuth's method），常数 `0x9E3779B97F4A7C15ULL`
3. **取桶**: 取乘法结果的高 `log2(N)` 位（要求桶数为2的幂）
4. **冲突**: 双向链表链地址法，支持 O(1) 删除

### 依赖检测特点

1. **三层渐进式检测**：版本(L0) → 元素区间(L1) → 逐维度精确(L2)
2. **2D 快速路径**：针对 Qwen3/PagedAttention 常见的2D tile 操作优化
3. **三种结果**：NONE（无依赖）、COVERED（完全覆盖，可安全删除）、OTHER（部分重叠，产生依赖）
4. **版本机制**：防止跨版本错误的 overlap 判定
5. **惰性清理**：通过 `last_alive` 水印逻辑失效 + 批量清理
6. **可重定位**：使用 pool index 链接，支持内存搬迁

### 架构协作

```
Tensor (数据结构)
   │
   ├──→ tensormap_core (Hash表 + Overlap检测 + 生命周期)
   │        │
   │        └──→ tensormap (编排胶水: tm_in/out/submit)
   │                 │
   │                 └──→ ring_buf (依赖记录: add_predecessors)
   │
   └──→ cases/*.h (实际使用: Qwen3 Decoder, PagedAttention)