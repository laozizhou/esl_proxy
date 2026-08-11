# early-dispatch 一期开发方案

> 面向"编码层"读者，讲清一期落地的代码修改点、`ED_ENABLE` 开关、以及有/无 ed 对比测试方案。
> 设计取舍与整体架构参见同目录下 `early-dispatch设计方案.md`。

---

## 1. 范围声明

- **模块范围**：只改 `esl_proxy/esl_proxy/src/algorithm/` 与 `esl_proxy/esl_proxy/include/algorithm/`；`scheduler/` 目录不动
- **激活 executor**：需要激活 `src/main.c` 里被注释的 `executor_worker` 线程（当前 dispatch 侧走 "Fake Return"，无法测出真正的 gate 收益）
- **开关**：编译期宏 `ED_ENABLE`（默认 1，通过 `make ED_ENABLE=0/1` 覆盖）
- **workload**：复用 `cases/` 现有 4 个（`qwen3_dynamic_manual_scope.h`、`qwen3_dynamic_tensormap.h`、`paged_attention_unroll.h`、`paged_attention_unroll_manual_scope.h`），不新造合成 DAG
- **不做**：SPMD 部分 stage（`count > 1` 的 s-task）、STARS 转发通道、duration-based 选核、outstanding 水位保留
- **一期硬约束**：`DISPATCH_THREAD_CNT == 1 && CUTTER_THREAD_CNT == 1`；此值定义在 `include/algorithm/conf.h`，一期不能拨大（`executor.c` 里 `msg_bitmap` 路由 `core % DISPATCH_THREAD_CNT` 与 stager 抢 slot 的 `ctrl_t` 归属尚未统一，见设计方案 §5.15 与 L14）

### 1.1 一期需要先修的既有编译错误（作为 Step 0）

现有基线代码在没有 ed 改动的情况下就**编译失败**。以下清单已用 `make`（默认 CASE=`qwen3_dynamic_manual_scope.h`）实测核对过，按报错先后顺序排列：

- **【首个 error，最先爆】`struct node_list` 不完整类型**：`include/algorithm/ring_buf.h:34-35` 里 `extern struct node_list g_successor_buf[RING_SIZE];` / `g_successor_exp_buf[HALF_RING_SIZE];` 只有前向声明，`struct node_list` 的**完整定义只在 `include/scheduler/painter.h`**（`{ uint16_t cnt; uint16_t node[CON_NODE_CNT]; struct node_list* next; }`），而算法侧不 include scheduler 头 → 报 `array has incomplete element type 'struct node_list'`。
  - **修法**：在算法侧提供该结构体的完整定义。推荐放到 `include/algorithm/task.h`（紧跟 `struct predecessor_list` 之后）或 `include/algorithm/ring_buf.h` 顶部，字段与 painter.h 保持一致：
    ```c
    struct node_list {
        uint16_t cnt;
        uint16_t node[CON_NODE_CNT];
        struct node_list* next;
    };
    ```
  - 注意 `CON_NODE_CNT`（=32）定义在 `include/algorithm/conf.h`，需确保定义处能见到该宏。
- `src/main.c:42` 使用 `EXECUTOR_THREAD_CNT` 但 `conf.h` 未定义 → `conf.h` 补 `#define EXECUTOR_THREAD_CNT 1`（实测：`error: use of undeclared identifier 'EXECUTOR_THREAD_CNT'`，**硬 error**）
- `src/main.c:59` 调用 `init_predecessors()` 但未在任何头文件里声明 → 补一份声明到 `include/algorithm/ring_buf.h`（实测：`warning: call to undeclared function 'init_predecessors'`。注意当前 Makefile 的 `CFLAGS` 带 `-Wno-error=implicit-function-declaration`，即便将来加 `-Werror` 也仍是 warning；但 C99 隐式声明会默认返回 `int`、丢失真实签名，属类型隐患，仍应补声明）
- `include/algorithm/mem_pool.h:173` `ring_min_uncompleted` 隐式声明 → 在 `ring_buf.h`/`mem_pool.h` 里补 `extern` 声明或占位实现（实测：`warning: call to undeclared function 'ring_min_uncompleted'`，同上，warning 但应补）
- `src/main.c:60` 调用 `init_ctrl_t()`：这是**链接期**错误（不是编译期——编译 `main.o` 阶段只要有声明即可过；`init_ctrl_t` 定义在 `src/scheduler/dispatch.c`，但 Makefile 不 build scheduler 目录，故链接 `bin/esl_proxy` 时才报 `undefined reference to init_ctrl_t`）→ 把 `init_ctrl_t()` 定义搬到 `src/algorithm/dispatch.c`，或在算法侧新增一个初始化函数并在 `main.c` 改调它。

> 上面 5 条里，**第 1 条（`struct node_list` 不完整类型）是原方案早期版本漏列的**——它其实是 `make` 时最先中断编译的**硬 error**，不补它，即便修完其余 4 条也过不了编译。真正会中断构建的是「node_list 不完整类型」（编译 error）与「init_ctrl_t 未定义」（链接 error）两条；`init_predecessors` / `ring_min_uncompleted` 在当前 CFLAGS 下只是 warning，但一并补齐更干净。

**Step 0 验证**：`ED_ENABLE=0` 情况下 4 个 case 都能通过 `make && ./bin/esl_proxy` 而不 `SIGSEGV`（`executor_worker` 已激活，`main.c` 里取消对应注释）。

### 1.2 一致性隐患（编码前必须知道的现状与设计的差异）

以下是「设计方案假设」与「基线代码现状」之间已核对出的 4 处偏差。它们不影响 Step 0 的 baseline 编译，但会直接影响 ed 实现的正确性与 A11/A12 的可执行性，编码时必须显式处理：

| # | 隐患 | 现状（实测） | 设计/ed 的假设 | 一期处理 |
| --- | --- | --- | --- | --- |
| C1 | **ring 索引未 `& RING_MASK`，不支持卷绕** | `add_predecessors` 用 `int slotIdx = task_id;`（`ring_buf.h:97`，注释里那行 `& RING_MASK` 被禁用）直接索引 `g_predecessors[task_id]`；`cutter.c::add_successors` 里 `task_idx = g_commit_task_id`（`cutter.c:58`）、`precessor_idx` 同样裸用，且 `g_predecessors`/`g_successor_buf`/`g_state_buf` 都只有 `RING_SIZE`(4096) 项 → 只要 id ≥ 4096 就**越界读写，必 SIGSEGV** | §5.13/§5.20/A12 全都依赖 ring 卷绕 + generation tag 正确工作 | 把 `add_predecessors`、`add_successors` 及一切按 id 索引 ring 数组的地方统一改成 `& RING_MASK`；这是 A12 stress case 能跑的**前置条件**，也是 ed 卷绕正确性的地基 |
| C2 | **边/commit 游标是 16 位；task 计数器是 32 位** | `g_task_id`、`g_min_uncomplete_task` 是 `atomic_int`（32 位），但 **`g_commit_task_id` 是 `uint16_t`**（`cutter.c:25`），`add_successors` 里 `uint16_t end = atomic_load(&g_task_id)`（`cutter.c:53`）把 32 位计数器**截断成 16 位**；边存储 `predecessor_list.exp` 是 `uint16_t*`、successor `node[]` 也是 `uint16_t`。即整条建边/消边通路在 **16 位 id 空间**里跑 | §5.13/§5.20 的 record「高 32 位 task_tag」若被理解成「独立的 32 位 generation 计数器」 | ed 的 `_Atomic uint64_t` record 本身可存 32 位值，但 **generation 的有效分辨率被边层的 16 位 id 上限约束**。好在存活窗口 ≤ `RING_SIZE`(4096) ≪ 65536，任意两个存活任务的 16 位 id 必不相同，故 16 位足以区分「同 slot 的不同代」——tag 比较用**完整 16 位 task_id 相等**即可。**不要**假设另有更宽的独立代计数器；若确需 >16 位代计数（M2 SPMD/超长跑），须把 `g_commit_task_id`、`exp`、`node[]` 一并升位，一期**不做**（见 §5.13「M3 扩展」与 §5.20 ring 复用边界） |
| C3 | **duration 是 uint16_t，且落 slot 时被 `/10000` 缩放** | `struct task_desc.duration` 为 `uint16_t`（上限 65535，`task.h:64`）；`dispatch.c:104-106` 在把任务落到 executor slot 时做 `slot.duration = (raw > 10000) ? raw/10000 : 1`（raw 已是截断后的 uint16 值），随后 `executor.c` 每次外层扫描迭代把该 slot duration `--` 一次。→ 单 block 执行至多约 `65535/10000 ≈ 6` tick；现有 case 里 `DUR_GATE_PROJ 95700` 之类**已溢出 uint16**（`-Wconstant-conversion` 告警，值被截断后再 /10000） | A11 原用 `dur=100000/200000` 制造「长任务空转」 | 放弃「超长单任务」路线；A11 改用 **SPMD `count` 放大执行时长**（executor 逐 block 递推，总时长 ≈ 6×count tick，见 §5.4 A11 用例 `ED_A11_P2_BLOCKS`）。任何依赖「任务执行很久」的构造都要走 count，不要指望 duration。**并且 A11 的 dump 采样周期必须远小于该窗口**（见 §5.4） |
| C4 | **`manager_worker` 与 ed 无关；真正要确认的是 cutter/`resolve_dep` 通路** | `manager_worker`（`src/algorithm/manager.c:15-26`）是 `mem_pool` 的 when2free FIFO 消费者，只做「等某 `taskid` 完成后释放挂账内存」；且函数体是 stub（打完日志直接 `return NULL`），`main.c:64` 的 `pthread_create` 也被注释。它**不 dec `unfin_pred_cnt`、不推进 `g_min_uncomplete_task`**——这些是 cutter/dispatch 侧 `resolve_dep` 的职责 | 早期误以为 ed Hook 2 依赖 `manager_worker` 才能推进依赖消费 | ed 一期**不依赖** `manager_worker`，它可以继续保持注释状态。Step 0 真正要确认的是「cutter 线程被激活 + `resolve_dep`（unfin `1→0`）通路活着」——§5.15 明确禁止靠 executor 空转 kick 出 RUNNABLE，只能靠 Hook 2 触发。若业务确需 `mem_pool` 挂账自动释放，另行激活 `manager_worker` 与 ed 无冲突。见 §3.2/§3.3 |

> C1、C2 是**同一根问题的两面**：本仓的 ring/依赖存储自始就是「16 位 task-id 直索引、且没做卷绕掩码」。ed 的 generation-tag 设计要落地，第一步就是把 ring 索引改成掩码化（C1，一期必做），generation 位宽扩展（C2）留到 M2。A12 的 ring stress case 正是 C1 修复后的回归测试。

---

## 2. 数据结构增量

### 2.1 [include/algorithm/conf.h](../../include/algorithm/conf.h) 新增

```c
/* 1: enable early-dispatch; 0: disable (baseline) */
#ifndef ED_ENABLE
#define ED_ENABLE 1
#endif

/* Threshold N: stage a s-task only when
 *   (all predecessors have been dispatched) AND
 *   (unfinished predecessor count <= ED_UNFIN_THRESHOLD)
 * Phase 1 default = 0xFFFF (effectively infinite; equivalent to "stage as soon as all preds dispatched").
 */
#ifndef ED_UNFIN_THRESHOLD
#define ED_UNFIN_THRESHOLD 0xFFFF
#endif
```

### 2.2 新增头文件 [include/algorithm/early_dispatch.h](../../include/algorithm/early_dispatch.h)

集中放 ed 相关类型、全局、函数原型：

```c
#ifndef ALGORITHM_EARLY_DISPATCH_H
#define ALGORITHM_EARLY_DISPATCH_H

#include <stdint.h>
#include <stdatomic.h>
#include "conf.h"
#include "queue.h"

typedef enum {
    ED_SPEC_NONE       = 0,
    ED_SPEC_STAGING    = 1,
    ED_SPEC_DISPATCHED = 2,
} ed_spec_state_t;

/* slot packed 低 32 位格式（外层 record 高 32 位保存完整 task tag）:
 *   bits [15:0]  core   (0xFFFF = INVALID/未 stage)
 *   bits [23:16] slot   (0..AIC_OSTD-1)
 *   bits [31:24] type   (0..EXE_TYPE_CNT-1)
 *
 * 使用 _Atomic uint32_t 而非 struct 的理由: 跨线程 (dispatcher 写 / cutter 读) 必须
 * 用原子操作建立 happens-before; struct 赋值 + atomic_thread_fence 在 C11 里不足以
 * 防止读到撕裂值 (data race UB), 详见设计方案 §5.13.
 */
#define ED_STAGED_INVALID       (0xFFFFFFFFu)
#define ED_PACK_SLOT(core, slot, type) \
    (((uint32_t)(type) << 24) | ((uint32_t)(slot) << 16) | (uint32_t)(core))
#define ED_UNPACK_CORE(p)  ((uint16_t)((p) & 0xFFFFu))
#define ED_UNPACK_SLOT(p)  ((uint8_t)(((p) >> 16) & 0xFFu))
#define ED_UNPACK_TYPE(p)  ((uint8_t)(((p) >> 24) & 0xFFu))
#define ED_STAGED_CORE_INVALID  ((uint16_t)0xFFFFu)

/* Per-task globals (indexed by task_id & RING_MASK). */
extern _Atomic uint16_t  g_dispatch_fanin[RING_SIZE];
extern uint16_t          g_dispatch_fanin_target[RING_SIZE];  /* = predecessor_cnt at commit */
extern _Atomic uint16_t  g_unfin_pred_cnt[RING_SIZE];
extern _Atomic uint8_t   g_spec_state[RING_SIZE];
/* 低 32 位 slot packed，高 32 位完整 task-id/generation；防 ring slot 复用 ABA。 */
extern _Atomic uint64_t  g_staged_slot_record[RING_SIZE];
extern _Atomic uint8_t   g_notify_claimed[RING_SIZE];         /* Hook 1/2 中仅 CAS 0->1 胜者可通知 */

/* Block-level progress pointer (Hook 1 stager 与 send_task 共用).
 * 一期 count==1: 取值 {0, 1};  M3 SPMD: 取值 [0, count]. */
extern _Atomic uint16_t  g_next_block_idx[RING_SIZE];

/* 瞬时位置记录带 task tag，完成时清；持久 dispatched tag 完成后不清，
 * add_successors 不能用瞬时 core map 判断“曾 dispatch”。 */
extern _Atomic uint64_t  g_task_dispatch_record[RING_SIZE];
extern _Atomic uint32_t  g_ring_task_tag[RING_SIZE];     /* 当前占用 ring slot 的完整 task-id */
extern _Atomic uint32_t  g_dispatch_tag[RING_SIZE];
extern atomic_flag       g_ed_edge_lock[RING_SIZE];

/* 每个 s-task 的 pred 快照, 供 pick_stage_core 定位 p-core 使用.
 * add_successors 在给 s 挂 successor 时同步 snapshot; 只记录 task_id, 具体 core
 * 通过 generation-tagged g_task_dispatch_record 二次查。 */
typedef struct {
    uint16_t cnt;
    uint16_t node[CON_NODE_CNT];  /* CON_NODE_CNT=32, 与 successor list 对齐 */
} ed_pred_snapshot_t;
extern ed_pred_snapshot_t g_ed_pred_snapshot[RING_SIZE];

/* Cross-thread queue of STAGING s-tasks awaiting Hook 1. */
extern queue_t g_ed_ready_queue;

/* Metrics (updated with relaxed atomics; read at end-of-run). */
extern _Atomic uint64_t g_ed_stage_cnt;           /* Hook 1 stage 成功 (executor 写完) 次数 */
extern _Atomic uint64_t g_ed_hit_cnt;             /* Hook 2 敲 doorbell 次数 */
extern _Atomic uint64_t g_ed_self_notify_cnt;    /* Hook 1 自敲 doorbell (Hook 2 抢先) 次数 */
extern _Atomic uint64_t g_ed_slot_retry_cnt;     /* Hook 1 抢 slot 失败并 re-push 次数 (方案β) */
extern _Atomic uint64_t g_ed_block_cas_fail_cnt; /* Hook 1 CAS next_block_idx 失败 + 回退 slot 次数 */
extern _Atomic uint64_t g_ed_send_skip_cnt;      /* send_task 因 next_block_idx 已满而跳过 s-task 次数 */
extern _Atomic uint64_t g_ed_late_arrival_cnt;   /* add_successors 里补齐迟到 fanin 的次数 (见设计方案 §5.12) */

#if ED_HOOK0_CONTRIB_STATS
/* A12 测试期专用: propagate_dispatch_fanin 里对每条边 +1;
 * 与 g_ed_late_arrival_cnt 相加应等于 sum(g_dispatch_fanin_target). */
extern _Atomic uint64_t g_ed_hook0_contrib_cnt;
#endif

void ed_init(void);                                       /* 初始化 metrics 与全 INVALID 表, main.c 里 init_ctrl_t() 后调用 */
uint64_t ed_task_dispatch_record_load(uint32_t task_id);
void     ed_task_dispatch_record_store(uint32_t task_id, int core, int slot, int type);
void     ed_task_dispatch_record_clear(uint32_t task_id); /* 仅清匹配 generation 的瞬时位置 */
void     ed_notify_once(uint32_t task_id, uint64_t record, ed_notify_source_t source);

#if ED_ENABLE
void propagate_dispatch_fanin(uint16_t p_id);
int  try_early_dispatch(int tid);
#endif

/* pick_nth_bit: 提取 bitmap 中第 nth 个 1-bit (0-indexed), 用于 pick_stage_core.
 * 一期用 __builtin_ctzll + 消 bit 循环实现, 复杂度 O(nth+1). */
static inline int pick_nth_bit(uint64_t bitmap, int nth) {
    for (int i = 0; i < nth; i++) bitmap &= (bitmap - 1);
    return __builtin_ctzll(bitmap);
}

#endif /* ALGORITHM_EARLY_DISPATCH_H */
```

> **实现细节**：
> - `g_staged_slot_record` / `g_task_dispatch_record` 必须显式初始化为 INVALID，且每次读取先校验高 32 位 task tag；不能只凭 ring index 读取旧代位置。
> - `g_dispatch_tag[idx]` 在该代任务提交时重置为 INVALID，在 Hook 0 锁内写成完整 task-id，任务完成后不清；它表达“曾 dispatch”，与瞬时位置表职责不同。
> - `g_ed_pred_snapshot` 保存完整可比较的 task-id；实际 core 查询靠 generation-tagged `g_task_dispatch_record`。

### 2.3 [include/algorithm/executor.h](../../include/algorithm/executor.h) 修改

在 `executor_t` 里新增原子 slot 发布状态；doorbell 仍保留为硬件信号模型，但只允许唯一通知者写：

```c
typedef struct executor {
    uint8_t idx;
    uint16_t tasks[AIC_OSTD];
    uint16_t block_idx[AIC_OSTD];
    uint16_t duration[AIC_OSTD];
    uint64_t base[AIC_OSTD];
    _Atomic uint8_t slot_state[AIC_OSTD]; /* EMPTY/GATED/RUNNABLE */
#if ED_ENABLE
    _Atomic uint8_t doorbell[AIC_OSTD];   /* 仅 notify_claimed CAS 胜者写 1 */
#endif
} executor_t;
```

`slot_state` 无论 `ED_ENABLE` 是否开启都存在，因为 normal dispatch 也必须遵守发布协议：payload 和 `task_id_map` 写完后 `store(RUNNABLE, release)`；executor `load(acquire)` 后才能读取。完成时 executor 先 `store(EMPTY, release)`，再 release 置 `msg_bitmap`。

### 2.4 `ctrl_t` 必须把 `msg_bitmap` / `free_bitmap` 声明为 `_Atomic uint64_t`

**背景**：`executor_worker` 激活后：
- `free_bitmap[type][slot]` 会被 dispatcher（`send_task` 派发时清 bit / `try_early_dispatch` 抢 slot / CAS 抢块失败回退）与 dispatcher 自己（`get_free_exe` 从 `msg_bitmap` 回收）**同线程写**，但被 `try_early_dispatch::pick_stage_core` **同线程读**——单线程内 OK。
- `msg_bitmap[type][slot]` 会被 **executor 线程写**（任务完成置位）、被 **dispatcher 线程读+清**（`get_free_exe` 回收）——**跨线程读写**。

**结论**：`msg_bitmap` 必须是 `_Atomic uint64_t`；消费者对每个 `(type,slot)` **恰好执行一次** `atomic_exchange(..., 0, memory_order_acquire)`，同一个返回快照同时用于读取 `task_id_map`、生成 completed queue、回收 free bit。禁止先 load/OR 回收、再从已被清空或新写入的 bitmap 另读一次。

**修改点**：[include/algorithm/dispatch.h](../../include/algorithm/dispatch.h) 的 `ctrl_t`：

```c
typedef struct ctrl {
    _Atomic uint64_t free_bitmap[TASK_TYPE_CNT][AIC_OSTD];
    _Atomic uint64_t msg_bitmap[EXE_TYPE_CNT][AIC_OSTD];
    /* 其余字段不变 */
    uint16_t task_id_map1[EXE_TYPE_CNT][AIC_CNT];
    uint16_t task_id_map2[EXE_TYPE_CNT][AIC_CNT];
    queue_t  ready_queue[TASK_TYPE_CNT];
    queue_t  completed_queue;
    queue_t  remote_completed_queue;
    uint16_t tid;
} ctrl_t;
```

**为何不用 `(_Atomic uint64_t *)&plain_uint64_t` 强制转换**：C11 §6.7.2.4 明确原子类型与非原子类型**不兼容**，`_Atomic uint64_t` 与 `uint64_t` 的表示允许不同（虽然在 x86/ARM 上通常一致，但严格 UB）。gcc/clang 会照 hw 常识编译，但一期文档不推荐依赖此 UB。

**如果坚持不改声明**（例如 `ctrl_t` 布局在其他地方被依赖）：可以用 GCC/Clang 的 `__atomic_*` builtins（例如 `__atomic_fetch_and(&free_bitmap[type][slot], mask, __ATOMIC_ACQ_REL)`）——这些函数接受非原子指针，是 well-defined 的编译器扩展，但一期为了可移植性优先改声明。

**init 影响**：`init_ctrl_t()` 里对 `free_bitmap` 与 `msg_bitmap` 的赋值改为 `atomic_store_explicit(..., memory_order_relaxed)`。原代码是普通赋值，需要一并更新。

`dispatch.c` 必须把原来的 `get_free_exe()` 与 `push_2_completed_queue()` 合并为单一消费者：

```c
static void drain_completed_snapshot(int tid) {
    uint16_t completed[EXE_TYPE_CNT * AIC_OSTD * AIC_CNT];
    uint16_t n = 0;
    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        for (int slot = 0; slot < AIC_OSTD; slot++) {
            uint64_t done = atomic_exchange_explicit(
                &g_ctrl_t[tid].msg_bitmap[type][slot], 0,
                memory_order_acquire);
            uint64_t bits = done;
            while (bits) {
                int core = __builtin_ctzll(bits);
                /* 与该快照对应的 map 只读一次；尚未发布 free bit，不会被覆盖。 */
                completed[n++] = (slot == 0)
                    ? g_ctrl_t[tid].task_id_map1[type][core]
                    : g_ctrl_t[tid].task_id_map2[type][core];
                bits &= bits - 1;
            }
            /* 生成 completed[] 后才回收；后续 send_task 才能覆写 map/payload。 */
            atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[type][slot],
                                     done, memory_order_release);
        }
    }
    batch_enqueue(&g_ctrl_t[tid].completed_queue, completed, n);
    atomic_fetch_add_explicit(&g_completed_cnt, n, memory_order_relaxed);
}
```

同一 bitmap 不得在其他函数再次 load/clear；`set_mix()` 只能基于 exchange 完成后的 `free_bitmap` 重算。

**`drain_completed_snapshot` 与 `set_mix` 的职责隔离**（对应设计方案 §5.19、§5.18）：

- `drain_completed_snapshot` 里的 `type` 循环范围是 `[0, EXE_TYPE_CNT) = [0, 2) = {CUBE, VECTOR}`；**不迭代 `TASK_TYPE_MIX`**。因为 MIX 与 VECTOR 同枚举值、共用下标 1 的物理存储，此循环写入 `free_bitmap[1]` 时事实上覆盖了 MIX 的映像——但这**只是恢复 VECTOR/MIX 共用槽位的 free bit**，还不是 MIX 语义（"CUBE 且 VECTOR 都 free 的 core"）。
- `set_mix()` 是**唯一**允许把 `free_bitmap[MIX]` 收紧为 "CUBE ∩ VECTOR" 的入口；它每轮 dispatch 都必须在 `drain_completed_snapshot` 之后、任何 `send_task` 之前调用。
- 顺序倒置的后果：若 `set_mix` 在 `drain` 之前，`set_mix` 用的是上一轮结尾的 CUBE/VECTOR 状态，本轮 executor 新回收的 free bit 没有反映到 MIX free 集合，MIX 任务会 miss 一轮；若 `send_task` 在 `set_mix` 之前，`send_task(MIX)` 读到的 `free_bitmap[1]` 是尚未收紧的 VECTOR-only 视图，可能派发到不是 CUBE-free 的 core（对纯 VECTOR/MIX 场景无害，但破坏 MIX 语义）。
- **`drain_completed_snapshot` 里禁止**再单独针对 `TASK_TYPE_MIX = 2` 下标做 `atomic_fetch_or`——那会与 VECTOR 的下标 1 循环产生重复写。三元数组的下标 2 是 dead slot（`TASK_TYPE_CNT = 3` 保留位），一期不使用。

---

## 3. 模块改动明细

### 3.1 [src/algorithm/dispatch.c](../../src/algorithm/dispatch.c)

#### 改动 A：去掉 "Fake Return"

现有代码里 `send_task` 派发后立即置 `msg_bitmap`（假装 executor 完成了），激活 `executor_worker` 后必须去掉：

```c
// Fake Return
// ctrl->msg_bitmap[type][slot] |= mask;   // <-- 删除或用 #if !ED_EXECUTOR_ACTIVATED 包住
```

（实际上 executor 激活是"独立于 ed 的前置改动"，`ED_ENABLE=0` 也要激活 executor，才能拿到"真实 executor 基线"。）

#### 改动 B：`send_task` 加 Hook 0 调用 + `next_block_idx` 去重检查

`send_task` 有两处改动：

**B1**：normal dispatch 与当前 executor 都是“一个条目执行整任务”。因此 normal 路径必须原子认领 `0 → count`，不能在 `count > 1` 时只加 1：

```c
for (int i = 0; i < cnt; i++) {
    uint16_t task_id = task_ids[i];
    uint16_t s_idx = task_id & RING_MASK;

#if ED_ENABLE
    uint16_t count = g_basic_buf[s_idx].count;
    uint16_t start = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_next_block_idx[s_idx], &start, count,
            memory_order_acq_rel, memory_order_acquire)) {
        atomic_fetch_add_explicit(&g_ed_send_skip_cnt, 1,
                                   memory_order_relaxed);
        continue;
    }
#endif

    /* skip 判定过后再计算 idx, 避免 skip 的 task 白占 free_bitmap 位 */
    uint64_t idx = (uint64_t)__builtin_ctzll(free_bitmap);
    uint64_t mask = 1ULL << idx;
    int slot = (atomic_load_explicit(&ctrl->free_bitmap[type][0],
                                      memory_order_relaxed) & mask) != 0 ? 0 : 1;

    /* 先原子认领 free bit，再写任何 slot 数据。 */
    uint64_t old_free = atomic_fetch_and_explicit(
        &ctrl->free_bitmap[type][slot], ~mask, memory_order_acq_rel);
    assert(old_free & mask);
    free_bitmap &= ~mask;

    /* slot 必须仍为 EMPTY；随后写 tasks/block_idx/duration/task_id_map。 */
    assert(atomic_load_explicit(&g_executors[type][idx].slot_state[slot],
                                memory_order_acquire) == EXE_SLOT_EMPTY);
    /* ... 写完整 payload 与 task_id_map ... */

#if ED_ENABLE
    /* record 必须在 RUNNABLE 发布前写；否则 executor 可能先完成，留下 stale record。 */
    ed_task_dispatch_record_store(task_id, (int)idx, slot, (int)type);
#endif
    /* payload 最后一步 release 发布，executor acquire 后才可读。 */
    atomic_store_explicit(&g_executors[type][idx].slot_state[slot],
                          EXE_SLOT_RUNNABLE, memory_order_release);
    sent++;

#if ED_ENABLE
    /* B3: Hook 0；位置 record 已在 RUNNABLE 前发布。 */
    propagate_dispatch_fanin(task_id);
#endif
}
```

> **实现细节**：
> - **skip 与 idx 计算的顺序**：必须先做 `next_block_idx` 检查/CAS，若 skip 则 `continue`，**不消耗 `free_bitmap`**（因为没占用 slot）。若在 idx 计算之后才 skip，会导致 `free_bitmap` 局部拷贝被推进但实际未清 bit，下一次 `ctzll` 拿错 idx。
> - `ED_ENABLE=1` 时 normal 路径对 `count==N` 做 `0→N`；Hook 1 只接受 `count==1` 并做 `0→1`。M3 前禁止把 normal claim 改回逐块递增。
> - **B2 (`ed_task_dispatch_record_store`) 必须在 Hook 0 之前**，供选核读取；late-arrival 则读取独立的持久 `g_dispatch_tag`。

**B4**：Hook 0 调用（原有描述保持）：在成功派发后调 `propagate_dispatch_fanin(task_id)`。

#### 改动 C：新增 `propagate_dispatch_fanin`

放在 dispatch.c 或新的 early_dispatch.c 里（推荐新建 `src/algorithm/early_dispatch.c` 集中管理 ed 代码）：

```c
void propagate_dispatch_fanin(uint16_t p_id) {
    uint16_t p_idx = p_id & RING_MASK;
    ed_edge_lock(p_idx);
    if (atomic_load_explicit(&g_ring_task_tag[p_idx], memory_order_acquire)
        != (uint32_t)p_id) {
        ed_edge_unlock(p_idx);
        return;  /* stale generation */
    }
    /* 持久记录“该 generation 曾 dispatch”；完成时不清。 */
    atomic_store_explicit(&g_dispatch_tag[p_idx], (uint32_t)p_id,
                          memory_order_release);
    uint16_t succ_cnt = g_successor_buf[p_idx].cnt;
    for (uint16_t k = 0; k < succ_cnt; k++) {
        uint16_t s_id  = g_successor_buf[p_idx].node[k];
        uint16_t s_idx = s_id & RING_MASK;

        /* 一期约束：只处理 count == 1 的 s-task */
        if (g_basic_buf[s_idx].count != 1) continue;

        uint16_t v = atomic_fetch_add_explicit(&g_dispatch_fanin[s_idx], 1,
                                                memory_order_relaxed) + 1;

        if (v != g_dispatch_fanin_target[s_idx]) continue;

        /* 检查 ED_UNFIN_THRESHOLD 门槛 */
        uint16_t unfin = atomic_load_explicit(&g_unfin_pred_cnt[s_idx],
                                               memory_order_acquire);
        if (unfin > ED_UNFIN_THRESHOLD) continue;

        /* CAS(NONE -> STAGING) */
        uint8_t exp = ED_SPEC_NONE;
        if (atomic_compare_exchange_strong_explicit(
                &g_spec_state[s_idx], &exp, ED_SPEC_STAGING,
                memory_order_acq_rel, memory_order_relaxed)) {
            ed_enqueue_or_abandon(s_id);
        }
    }
    ed_edge_unlock(p_idx);
}
```

`g_successor_buf[p_idx]` 是普通内存，Hook 0 的遍历和 `add_successors` 的 append 必须在同一把 `g_ed_edge_lock[p_idx]` 下。`ed_enqueue_or_abandon` 在 enqueue 失败时立即 CAS `STAGING→NONE`；此时尚未认领 block，可以安全放弃 ed。

#### 改动 D：新增 `pick_stage_core`

策略：优先从 s-task 的前驱集合里**随机**挑一个"有空 slot"的 core；找不到则 fallback 到普通空闲 core：

```c
static __thread unsigned int s_ed_rand_seed;   /* 线程局部 PRNG seed, 一期 DISPATCH_THREAD_CNT=1 无所谓; 
                                                 * 提前用 thread_local 是为了 M4 多线程平滑迁移. */

/* returns core index (>=0) with free slot, or -1 if none */
static int pick_stage_core(int tid, uint16_t s_id, task_type_t type,
                           int *out_slot) {
    uint16_t s_idx = s_id & RING_MASK;

    /* 1) 从 g_ed_pred_snapshot[s_idx] 快照里逐个查 generation-tagged dispatch record，
     *    未 COMPLETED 且已 dispatched 的 pred 的 core 加入 pcore_bitmap. */
    uint64_t pcore_bitmap = 0;
    ed_pred_snapshot_t *snap = &g_ed_pred_snapshot[s_idx];
    for (uint16_t k = 0; k < snap->cnt; k++) {
        uint16_t p_id = snap->node[k];
        /* pred 已完成 => 该 core 已释放, 归入 fallback (空闲核) 路径 */
        if (g_state_buf[p_id & RING_MASK].state == TASK_STATUS_COMPLETED) continue;
        uint64_t rec = ed_task_dispatch_record_load(p_id);
        if (ED_RECORD_TAG(rec) != (uint32_t)p_id) continue; /* 拒绝旧 generation */
        uint32_t packed = ED_RECORD_SLOT(rec);
        uint8_t p_type = ED_UNPACK_TYPE(packed);
        /* 只考虑同 type 的核 (跨 type 抢 slot 无意义, executor 索引不同) */
        if ((task_type_t)p_type != type) continue;
        pcore_bitmap |= (1ULL << ED_UNPACK_CORE(packed));
    }

    /* 2) 求 available = pcore_bitmap ∩ (slot0 free ∨ slot1 free) */
    uint64_t free0 = atomic_load_explicit(&g_ctrl_t[tid].free_bitmap[type][0],
                                           memory_order_acquire);
    uint64_t free1 = atomic_load_explicit(&g_ctrl_t[tid].free_bitmap[type][1],
                                           memory_order_acquire);
    uint64_t free_any = free0 | free1;                 /* 核有至少一个 slot 空 */
    uint64_t available_pcore = pcore_bitmap & free_any;

    /* 3) 若非空, 随机挑一位; 否则 fallback 到空闲核 */
    uint64_t candidate = available_pcore ? available_pcore : free_any;
    if (candidate == 0) return -1;
    int popcnt = __builtin_popcountll(candidate);
    int nth = (int)(rand_r(&s_ed_rand_seed) % (unsigned)popcnt);
    int core = pick_nth_bit(candidate, nth);

    /* 4) 挑 slot: 优先 slot 0; 若 slot 0 已 busy, 用 slot 1 */
    uint64_t mask = 1ULL << core;
    *out_slot = (free0 & mask) != 0 ? 0 : 1;
    return core;
}
```

> **实现细节**：
> - `predecessor_list` (`g_predecessors[s_idx]`) 在 `cutter.c::add_successors` 里被消费（`ptr->cnt--; ptr->exp++;`），**不能**在 `pick_stage_core` 里直接读——所以设计里引入 `g_ed_pred_snapshot`，`add_successors` 挂 successor 时**同步 snapshot pred id 列表**。
> - pred → core 的反查用 `g_task_dispatch_record`（send 时 store，完成时仅在 tag 匹配时 clear）。
> - `pcore_bitmap` **必须限制在同一 `type`** 内：p-task 与 s-task 类型不同（例如 p 是 CUBE、s 是 VECTOR）时，抢 CUBE 类型 core 的 slot 与 executor 索引到 `g_executors[TASK_TYPE_VECTOR][core]` 不匹配。
> - `rand_r()` 而非 `rand()`：`rand()` 在 POSIX 上不是线程安全的（且不同实现会串扰全局 seed）。一期即便只有 1 个 dispatcher 线程，也用 `rand_r` 为将来做准备。
> - `pick_nth_bit` 见 §2.2 头文件的 static inline 实现。
> - **`free0 | free1` 而非 `free0 & free1`**：`send_task` 现有代码里用 `free0 & free1` 判"核完全空"，那是因为 dispatch 想一次性拿一个 slot；这里 stager 只要**一个** slot 空即可，所以是 OR。

#### 改动 E：新增 `try_early_dispatch`（simpler 对齐版：**先抢 slot、后 CAS 抢块**；顺序按设计方案 §5.14 修正）

严格按设计方案 5.10 的 seq_cst 双向 + 自敲序列实现，全流程**不做 spec_state / executor 数据的撤销**；抢 slot 失败走 β re-push（不动 `next_block_idx`），抢块 CAS 失败必须回退 slot 位。

```c
int try_early_dispatch(int tid) {
    uint16_t s_id;
    if (!dequeue(&g_ed_ready_queue, &s_id)) return 0;

    uint16_t s_idx = s_id & RING_MASK;

    /* ① 快速门控: Hook 2 已经抢先 DISPATCHED 且没有 stage 记录, 放弃 */
    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst)
        != ED_SPEC_STAGING) {
        return 0;  /* 已 DISPATCHED, 走 ready_queue 兜底, 不做 stage */
    }

    /* ②' 选核 + 抢 slot (先做, 见 §5.14) */
    task_type_t type = g_basic_buf[s_idx].type;
    int slot = -1;
    int core = pick_stage_core(tid, s_id, type, &slot);
    if (core < 0) {
        goto re_push_slot_busy;  /* 无可用 slot, re-push (保 STAGING, 不动 next_block_idx) */
    }

    uint64_t core_mask = 1ULL << core;
    uint64_t old_bm = atomic_fetch_and_explicit(
        &g_ctrl_t[tid].free_bitmap[type][slot],
        ~core_mask, memory_order_acq_rel);
    if ((old_bm & core_mask) == 0) {
        /* pick_stage_core 采样后到 CAS 之间, slot 被 send_task 或另一 stager 清 0 */
        goto re_push_slot_busy;
    }

    /* ③' CAS 抢块 (后做): M2 一期 count==1, 从 0 抢到 1 */
    uint16_t expected_blk = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &g_next_block_idx[s_idx], &expected_blk, 1,
            memory_order_acq_rel, memory_order_relaxed)) {
        /* send_task 或另一 stager 已经 CAS 成功 (即将 或 已经派发) */
        /* !!! 必须回退 slot 位, 否则该 (core, slot) 永久 busy !!! */
        atomic_fetch_or_explicit(&g_ctrl_t[tid].free_bitmap[type][slot],
                                  core_mask, memory_order_release);
        atomic_fetch_add_explicit(&g_ed_block_cas_fail_cnt, 1,
                                   memory_order_relaxed);
        return 0;  /* 不 re-push: s-task 已被别人拿走 */
    }

    /* ④ 写 executor 数据；抢到 free bit 后 slot 必须为 EMPTY */
    assert(atomic_load_explicit(&g_executors[type][core].slot_state[slot],
                                memory_order_acquire) == EXE_SLOT_EMPTY);
    /* 4a: 先清 doorbell 残留 */
    atomic_store_explicit(&g_executors[type][core].doorbell[slot], 0,
                          memory_order_relaxed);
    /* 4b: 写 payload/duration/tasks/task_id_map (relaxed, 单写者) */
    g_executors[type][core].tasks[slot]    = s_id;
    g_executors[type][core].block_idx[slot] = 0;
    uint32_t raw = g_basic_buf[s_idx].duration;
    g_executors[type][core].duration[slot] = (raw > 10000) ? (raw / 10000) : 1;
    if (slot == 1) g_ctrl_t[tid].task_id_map2[type][core] = s_id;
    else           g_ctrl_t[tid].task_id_map1[type][core] = s_id;
    /* g_executors[type][core].idx 不再由 stager 维护; 见 §3.2 改动 B (全 slot 扫描) */
    /* 4c: 最后 release 发布 GATED，executor acquire 后才读 payload */
    atomic_store_explicit(&g_executors[type][core].slot_state[slot],
                          EXE_SLOT_GATED, memory_order_release);

    /* ⑥ 记录 stage 位置 (seq_cst 原子写, 与 Hook 2 的 seq_cst 原子读配对) */
    uint32_t packed = ED_PACK_SLOT((uint16_t)core, (uint8_t)slot, (uint8_t)type);
    uint64_t record = ED_PACK_RECORD((uint32_t)s_id, packed);
    atomic_store_explicit(&g_staged_slot_record[s_idx], record,
                          memory_order_seq_cst);

    atomic_fetch_add_explicit(&g_ed_stage_cnt, 1, memory_order_relaxed);

    /* ⑦ 若 Hook 2 抢先，只尝试认领通知；CAS 失败者禁止写 doorbell。 */
    if (atomic_load_explicit(&g_spec_state[s_idx], memory_order_seq_cst)
        == ED_SPEC_DISPATCHED) {
        ed_notify_once(s_id, record, ED_NOTIFY_SELF);
    }
    return 1;

re_push_slot_busy:
    /* 方案 β: 保持 spec_state=STAGING, 不动 next_block_idx (还没 CAS), re-push ed_ready_queue */
    if (!enqueue(&g_ed_ready_queue, s_id)) {
        /* 尚未认领 block，可以安全放弃 ed；Hook 2 将走 normal ready_queue。 */
        uint8_t exp = ED_SPEC_STAGING;
        atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &exp, ED_SPEC_NONE,
            memory_order_acq_rel, memory_order_acquire);
        WORKER_LOGF("ed_ready_queue full, s=%u abandon ed", s_id);
    }
    atomic_fetch_add_explicit(&g_ed_slot_retry_cnt, 1, memory_order_relaxed);
    return 0;
}
```

> **关键点**：
> - **顺序修正为 ②' → ③'（先抢 slot 再 CAS 抢块）**：见设计方案 §5.14。抢 slot 失败走 β re-push；CAS 抢块失败**必须回退 slot 位**（`atomic_fetch_or`），否则该 (core, slot) 永久 busy。
> - **`g_staged_slot_record` 是 `_Atomic uint64_t`**：高 32 位 tag 防 ABA，低 32 位保存位置；Hook 1/2 用 seq_cst store/load 配对。
> - **通知恰好一次**：`ed_notify_once` 先校验 record tag，再 CAS `g_notify_claimed[s_idx] 0→1`；仅胜者写 doorbell，并 CAS `slot_state GATED→RUNNABLE`。Hook 1/2 不能直接写 doorbell。
> - **队列失败不泄漏**：block 尚未认领时将 STAGING 回退为 NONE；Hook 2 后续 normal dispatch。不得依赖收尾 kick。

#### 改动 F：`dispatch(int tid)` 主循环——四段职责边界（drain → set_mix → send_task ×3 → Hook 1 drain）

一次 `dispatch()` 里应尽量把 `ed_ready_queue` 里当前可 stage 的 s-task 都处理完，避免"一轮 dispatch 只 pop 一个"造成 stage 时机滞后。**同时**由于 `TASK_TYPE_MIX == TASK_TYPE_VECTOR == 1`（见设计方案 §5.18），`set_mix()` 必须**在 drain 之后、send_task 之前**执行，把 `free_bitmap[MIX]` 重算为 `free_bitmap[CUBE] & free_bitmap[VECTOR]`——否则会漏派发本轮新回收的 core。

```c
#define ED_DRAIN_MAX_PER_ROUND 16   /* 单次 dispatch 最多 stage 的 s-task 数, 防止 ed 循环独占 CPU */

int dispatch(int tid) {
    int total_sent = 0;
    /* Step 1: 唯一 exchange 消费点 - 从 msg_bitmap 拿 done 快照, 生成 completed queue, 恢复 free_bitmap[CUBE/VECTOR]. */
    drain_completed_snapshot(tid);
    /* Step 2: 重算 free_bitmap[MIX] = f[CUBE] & f[VECTOR]. 每轮必算; 只有本函数写它. */
    set_mix(tid);
    /* Step 3: 派发. MIX 与 VECTOR 因枚举同值共用下标 1 的 ready_queue/free_bitmap;
     *          send_task(MIX) 与 send_task(VECTOR) 消费同一队列, 第二次通常空返.
     *          保留结构上的独立调用, 方便未来 MIX/VECTOR 拆开 (M6 之后, 见 §5.18). */
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
#if ED_ENABLE
    /* Step 4: 从 ed_ready_queue 尽可能多 stage. */
    for (int k = 0; k < ED_DRAIN_MAX_PER_ROUND; k++) {
        if (try_early_dispatch(tid) == 0) break;   /* 队列空或抢块/抢 slot 都失败, 停 */
        total_sent++;
    }
#endif
    return total_sent;
}
```

**四段的职责边界不重叠**（对应设计方案 §5.19）：

| 步骤 | 唯一职责 | 禁止行为 |
| --- | --- | --- |
| Step 1 `drain_completed_snapshot(tid)` | 对每个 `(exe_type, slot)` 恰好一次 `atomic_exchange(msg_bitmap, 0, acquire)`；用**同一** done 快照读取 `task_id_map` 并 push 到 `completed_queue`；最后 `atomic_fetch_or(free_bitmap[type][slot], done, release)` 恢复 free bit | 禁止二次 load/clear msg_bitmap；禁止越权写 `free_bitmap[MIX]`（下标 1 由 set_mix 独占写入） |
| Step 2 `set_mix(tid)` | 对每个 `slot ∈ {0, 1}` 计算 `free_bitmap[MIX][slot] = free_bitmap[CUBE][slot] & free_bitmap[VECTOR][slot]`，写回下标 1 存储（与 VECTOR 共用同一物理位置） | 禁止读/写 msg_bitmap；不可省略——每轮都必须重算 |
| Step 3 `send_task(ctrl, type)` × 3 | 从 `ready_queue[type]` batch_dequeue；CAS `next_block_idx: 0 → count` 认领整任务；`atomic_fetch_and free_bitmap` 抢 free bit；写 payload / `task_id_map` / `ed_task_dispatch_record`；`slot_state=RUNNABLE, release` 发布；`propagate_dispatch_fanin` (Hook 0) | 禁止在 slot_state 发布前调 propagate（否则 pick_stage_core 读到未生效的 record）；skip 掉的 task 不得消耗 free_bitmap 位（§3.1 B1） |
| Step 4 `try_early_dispatch(tid)` × N | 从 `g_ed_ready_queue` pop；先抢 slot、后 CAS 抢块；写 executor 数据；`slot_state=GATED, release`；`g_staged_slot_record, seq_cst`；stager 自敲兜底 | 禁止改动 spec_state（除 §5.10 CAS）；抢块 CAS 失败必须回退 free bit（§5.14） |

**关于 `TASK_TYPE_MIX == TASK_TYPE_VECTOR` 的详细语义、为何保留 send_task(MIX) 独立调用**：见设计方案 §5.18。

**关于 drain / set_mix 顺序的正确性证明**：见设计方案 §5.19。核心是 `set_mix` 基于当前 CUBE / VECTOR free 集合求交集，若 drain 后置则本轮 executor 完成的任务对应 free bit 尚未恢复，`set_mix` 会算出偏小的 MIX free 集合，本可派发的 MIX 任务会 miss 一轮。

**关于收尾循环**：`dispatch_worker` 的两段 while（`!g_orch_is_done` 与 `completed_cnt < task_id`）都只是继续调用 `dispatch(tid)` 并等待真实 completion；禁止按空转轮数扫描并 kick STAGING，因为 `next_block_idx==count` 不代表依赖已完成。见设计方案 §5.15。

### 3.2 [src/algorithm/executor.c](../../src/algorithm/executor.c)

#### 改动 A：`executor_init` 初始化 slot_state / doorbell

```c
void executor_init(void) {
    for (int t = 0; t < EXE_TYPE_CNT; t++) {
        for (int c = 0; c < AIC_CNT; c++) {
            g_executors[t][c].idx = AIC_OSTD;
            for (int i = 0; i < AIC_OSTD; i++) {
                g_executors[t][c].tasks[i]     = 0;
                g_executors[t][c].block_idx[i] = 0;
                g_executors[t][c].duration[i]  = 0;
                g_executors[t][c].base[i]      = 0;
                atomic_init(&g_executors[t][c].slot_state[i], EXE_SLOT_EMPTY);
#if ED_ENABLE
                atomic_init(&g_executors[t][c].doorbell[i], 0);
#endif
            }
        }
    }
}
```

#### 改动 B：`executor_worker` 主循环改为"全 slot 扫描 + gate 检查 + 保留 SPMD 分支"

> **⚠ 基线漂移警告**（见设计方案 L9）：现有 `executor_worker` 每 iteration 只 tick `.idx` 指向的一个 slot（"每核串行 2 slot"模型）；一期改为**全 slot 扫描后每 iter 都 tick**（"每核并行 2 slot"模型）。这会让 `ED_ENABLE=0` 基线相对旧 fake-return 版本加速 ≈2x，ed 相对收益被稀释。
>
> **测试口径**：本文档 §5 所有对比都以"激活 executor 后的 `ED_ENABLE=0`"为基线，**不与旧 fake-return 版本对比**。若希望保留原语义（每核 tick 一个 slot），可以在扫描时维护一个"round-robin slot 指针"，只 tick 该指针指向的 slot 的 `duration`，但仍扫全部 slot 的 gate——一期先接受基线漂移，M2 再评估。

**必须保留 SPMD 分支**：现有 `executor_worker` 里 `block_count > 1` 时**逐 block 递推 duration**（本 block 归 0 → `block_idx++` → 重置 duration 跑下一 block → 直到 `block_idx >= block_count` 才置 `msg_bitmap`）。一期 normal dispatch 用 `next_block_idx: 0 → count` 整任务认领（§3.1 B1），executor 仍按"一个派发条目执行整任务"语义，因此 SPMD 分支必须保留，不能像早期草案那样直接把 duration 归零就置 msg——那会让 `count > 1` 的任务在第一 block 结束时被误判完成。

合并后 executor_worker 的**最终一期版本**（对应设计方案 §5.17）：

```c
static inline void ed_complete_slot(int type, int core, int slot, uint16_t task_id_done) {
    executor_t *e = &g_executors[type][core];
    e->block_idx[slot] = 0;
#if ED_ENABLE
    /* generation 校验后再清瞬时位置; ring 已被下一代覆写时会跳过 clear (§5.13). */
    ed_task_dispatch_record_clear(task_id_done);
#endif
    /* 发布顺序 (禁止倒序):
     *   1. slot_state = EMPTY, release  -> 之后 send_task/try_early_dispatch 才允许挑到本 slot
     *   2. msg_bitmap |= bit, release  -> dispatcher 拿到 exchange 快照后才回收 free_bit
     * 若倒序, dispatcher 可能在两个 store 之间读到 msg_bitmap bit=1 但 slot_state 仍 RUNNABLE. */
    atomic_store_explicit(&e->slot_state[slot], EXE_SLOT_EMPTY,
                          memory_order_release);
    atomic_fetch_or_explicit(
        &g_ctrl_t[core % DISPATCH_THREAD_CNT].msg_bitmap[type][slot],
        ((uint64_t)0x1 << core), memory_order_release);
}

void *executor_worker(void *arg) {
    (void)arg;
    int total_write_cnt = 0;
    while (!atomic_load(&g_is_done)) {
        for (int type = 0; type < EXE_TYPE_CNT; type++) {
            for (int core = 0; core < AIC_CNT; core++) {
                executor_t *e = &g_executors[type][core];
                for (int slot = 0; slot < AIC_OSTD; slot++) {
                    uint8_t state = atomic_load_explicit(
                        &e->slot_state[slot], memory_order_acquire);
                    /* EMPTY: 无任务; GATED: 等 doorbell/notify_once 升 RUNNABLE. 都不 tick. */
                    if (state != EXE_SLOT_RUNNABLE) continue;

#if ED_ENABLE
                    /* gate 通过后清 doorbell (下一次 slot 复用前不留残留).
                     * 一期 slot_state 才是唯一 gate 判据; doorbell 仅是硬件信号的模型占位. */
                    atomic_store_explicit(&e->doorbell[slot], 0,
                                          memory_order_relaxed);
#endif
                    uint16_t task_id     = e->tasks[slot];
                    uint32_t block_count = g_basic_buf[task_id & RING_MASK].count;

                    if (block_count > 1) {
                        /* ---------- SPMD 分支 (保留) ---------- */
                        if (e->duration[slot] > 0) {
                            e->duration[slot]--;
                            continue;                 /* 本 block 未完, 保持 RUNNABLE 下一轮再来 */
                        }
                        /* 本 block 归 0, 推进到下一 block */
                        uint16_t nb = ++e->block_idx[slot];
                        if (nb < block_count) {
                            uint32_t raw =
                                g_basic_buf[task_id & RING_MASK].duration;
                            e->duration[slot] =
                                (raw > 10000) ? (uint16_t)(raw / 10000) : 1;
                            continue;                 /* 下一 block 起跑, slot 仍 RUNNABLE */
                        }
                        /* 整任务完成 */
                        total_write_cnt++;
                        WORKER_LOGF("total,%d,core,%d,type,%d,blocks,%u",
                                    total_write_cnt, core, type, block_count);
                        ed_complete_slot(type, core, slot, task_id);
                    } else {
                        /* ---------- 单 block 分支 ---------- */
                        if (e->duration[slot] > 0) {
                            e->duration[slot]--;
                        }
                        if (e->duration[slot] == 0) {
                            total_write_cnt++;
                            WORKER_LOGF("total,%d,core,%d,type,%d",
                                        total_write_cnt, core, type);
                            ed_complete_slot(type, core, slot, task_id);
                        }
                    }
                }
            }
        }
    }
    WORKER_LOGF("finished, total_write_cnt=%d", total_write_cnt);
    return NULL;
}
```

**关键要点**：

1. **`slot_state != EXE_SLOT_RUNNABLE` 时不 tick**：`EMPTY` 是无任务；`GATED` 是等 gate 通过（等 `notify_once` 里 CAS `GATED→RUNNABLE`）。这两种情况下 duration 不递减、msg_bitmap 也不置——避免 stager 已 stage 但 Hook 2/自敲还没跑时任务被误当作已完成。
2. **SPMD 分支的 slot 一旦从 `GATED` / normal-init 升为 `RUNNABLE`**，就一直保持 RUNNABLE 直到最后一个 block 完成；中间 block 的推进不涉及 `slot_state` 变更、不涉及 doorbell 重写。M3 引入部分 stage 时才需要 per-block 通知，一期不用。
3. **`ed_complete_slot` 的发布顺序**：`slot_state = EMPTY` **必须**在 `msg_bitmap |= bit` 之前。dispatcher 在 `drain_completed_snapshot` 里拿到 msg bit 后**可能立刻**回收 free_bit 并调 send_task；send_task 会 assert `slot_state == EMPTY`（见 §3.1 B1 里的 assert）。倒序会破坏这个不变式。
4. **`ed_task_dispatch_record_clear` 按 tag 校验**：任务完成时的 clear 必须先 CAS 校验 `tag == task_id_done`；若已被 ring 换代覆写为 (task_id_done + RING_SIZE)，则跳过 clear——防止误清新代任务的位置记录。见 §5.13。
5. **`executor_t::idx` 字段废弃**：一期改为全 slot 扫描后，`.idx` 不再作为主索引，只用作 log/debug；`executor_init` 里保留 `idx = AIC_OSTD` 的默认初值即可。

### 3.3 [src/algorithm/cutter.c](../../src/algorithm/cutter.c)

#### 改动 A：`add_successors` 改为"两趟"结构，**所有分支**都要 init ed 元数据（含 §5.12 late-arrival 补齐）

**关键**：`add_successors` 目前有 3 条通路：
1. `ptr->cnt <= 0`：**根任务**（提交前压根没写 predecessor list），直接入 ready_queue；
2. `ptr->cnt > 0` 但所有 pred 都已 COMPLETED（`predecessor_cnt == 0`）：入 ready_queue；
3. `ptr->cnt > 0` 且有 pending pred（`predecessor_cnt > 0`）：留在 predecessor_cnt 分支，等 Hook 2。

**因为环形 ring 会复用**（RING_SIZE=4096，任务数 > 4096 时同一 ring slot 会被复用），3 条通路都必须**重置** ed 元数据；否则残留状态（例如 `next_block_idx=1`、`spec_state=DISPATCHED`）会污染新任务。

**两趟结构总览**（对应设计方案 §5.12.1）：

- **第一趟**：只做本地扫描——遍历 `g_predecessors[s_idx]`，对每个 pred 检查 `g_state_buf[p_idx].state`，把未 COMPLETED 的 pred 收集到栈上 `survivors[]` 缓冲（同时算 `predecessor_cnt`）。**不进任何锁，不写 g_successor_buf，不写 g_dispatch_fanin**。第一趟结束后立刻用 `ed_init_task_meta(s_full, predecessor_cnt)` 把 s 自身的 ed 元数据（含 `g_dispatch_fanin_target`）一次性固化。
- **第二趟**：对 `survivors[]` 的每个 pred p，独立进 `g_ed_edge_lock[p_idx]`，在锁内 append 到 `g_successor_buf[p_idx]`，锁外读 `g_dispatch_tag[p_idx]` 判断 p 本代是否已 dispatch，若是则补一次 `g_dispatch_fanin[s_idx] +=1`。

**为什么必须"先 init s、后 append"**：若先 append 再 init，Hook 0 for p 可能在 append 与 init 之间读到未初始化的 `g_dispatch_fanin_target[s]`（可能是上一代的残值），导致 `maybe_enter_staging` 提前触发（若上一代 target < 本代 target）或永远错过（若上一代 target > 本代 target）。

**统一的 init helper**（放在 `early_dispatch.c`）：

```c
static inline void ed_init_task_meta(uint32_t full_task_id, uint16_t predecessor_cnt) {
#if ED_ENABLE
    uint16_t task_idx = full_task_id & RING_MASK;
    /* ring 换代与 outgoing edge 清理必须和 Hook 0 / add_successors 第二趟共用一把 s 的边锁。 */
    ed_edge_lock(task_idx);
    atomic_store_explicit(&g_ring_task_tag[task_idx], full_task_id,
                          memory_order_release);
    atomic_store_explicit(&g_dispatch_tag[task_idx], ED_TASK_TAG_INVALID,
                          memory_order_relaxed);
    g_successor_buf[task_idx].cnt = 0;
    g_state_buf[task_idx].successor_cnt = 0;
    ed_edge_unlock(task_idx);
    g_dispatch_fanin_target[task_idx] = predecessor_cnt;
    atomic_store_explicit(&g_dispatch_fanin[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_unfin_pred_cnt[task_idx], predecessor_cnt, memory_order_relaxed);
    atomic_store_explicit(&g_spec_state[task_idx], ED_SPEC_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_next_block_idx[task_idx], 0, memory_order_relaxed);
    atomic_store_explicit(&g_staged_slot_record[task_idx], ED_RECORD_INVALID, memory_order_relaxed);
    atomic_store_explicit(&g_notify_claimed[task_idx], 0, memory_order_relaxed);
    g_ed_pred_snapshot[task_idx].cnt = 0;
#endif
}
```

**maybe_enter_staging helper**（第二趟里 late-arrival 命中时或"pred 已换代情形③"时调用）：

```c
static inline void ed_maybe_enter_staging(uint32_t s_full, uint16_t s_idx, uint16_t v) {
#if ED_ENABLE
    if (v != g_dispatch_fanin_target[s_idx]) return;
    if (g_basic_buf[s_idx].count != 1) return;
    uint16_t unfin = atomic_load_explicit(&g_unfin_pred_cnt[s_idx],
                                           memory_order_acquire);
    if (unfin > ED_UNFIN_THRESHOLD) return;
    uint8_t exp = ED_SPEC_NONE;
    if (atomic_compare_exchange_strong_explicit(
            &g_spec_state[s_idx], &exp, ED_SPEC_STAGING,
            memory_order_acq_rel, memory_order_relaxed)) {
        ed_enqueue_or_abandon(s_full);
    }
#endif
}
```

**完整两趟 `add_successors` 伪代码**（替换现有 `src/algorithm/cutter.c::add_successors` 的整个循环体）：

```c
void add_successors(uint16_t ready_cnt[], uint16_t rq_buf[][RQ_BATCH_SIZE]) {
    uint16_t end = atomic_load(&g_task_id);
    uint16_t tmp = g_commit_task_id + PRE_BATCH_SIZE;
    end = tmp > end ? end : tmp;

    while (g_commit_task_id < end) {
        /* NOTE: g_commit_task_id 一期是 uint16_t，未超 RING_SIZE 时等于 task_id；
         * 支持 RING 卷绕时应改为 uint32_t 完整 task-id + `& RING_MASK` 得 ring idx。 */
        uint32_t s_full = g_commit_task_id;
        uint16_t s_idx  = s_full & RING_MASK;
        struct predecessor_list *ptr = &g_predecessors[s_idx];
        task_type_t s_type = g_basic_buf[s_idx].type;

        /* ---------- 通路 A: 根任务, 无 predecessor ---------- */
        if (ptr->cnt <= 0) {
            ed_init_task_meta(s_full, /*predecessor_cnt=*/0);
            rq_buf[s_type][ready_cnt[s_type]++] = s_full;
            g_commit_task_id++;
            continue;
        }

        /* ---------- 第一趟: 只做本地扫描, 不进锁, 不写 g_successor_buf ---------- */
        uint16_t original_cnt = ptr->cnt;
        uint16_t predecessor_cnt = 0;
        struct { uint32_t p_full; uint16_t p_idx; } survivors[CON_NODE_CNT];
        for (uint16_t k = 0; k < original_cnt; k++) {
            uint32_t p_full = ptr->exp[k];
            uint16_t p_idx  = p_full & RING_MASK;
            if (g_state_buf[p_idx].state == TASK_STATUS_COMPLETED) continue;
            survivors[predecessor_cnt].p_full = p_full;
            survivors[predecessor_cnt].p_idx  = p_idx;
            predecessor_cnt++;
        }

        /* ---------- 关键屏障: 固化 s 的 ed 元数据; target 必须先于任何 append ---------- */
        ed_init_task_meta(s_full, predecessor_cnt);
        for (uint16_t k = 0; k < predecessor_cnt; k++) {
            g_ed_pred_snapshot[s_idx].node[k] = (uint16_t)survivors[k].p_full;
        }
        g_ed_pred_snapshot[s_idx].cnt = predecessor_cnt;
        g_predecessor_cnt[s_idx] = predecessor_cnt;

        /* ---------- 通路 B: 存活 pred 数 == 0, 走 normal ready_queue ---------- */
        if (predecessor_cnt == 0) {
            rq_buf[s_type][ready_cnt[s_type]++] = s_full;
            ptr->exp += original_cnt;  ptr->cnt = 0;
            g_commit_task_id++;
            continue;
        }

        /* ---------- 第二趟: 对每条存活 pred, 独立进锁 append + late-arrival 补齐 ---------- */
        for (uint16_t k = 0; k < predecessor_cnt; k++) {
            uint32_t p_full = survivors[k].p_full;
            uint16_t p_idx  = survivors[k].p_idx;

            ed_edge_lock(p_idx);
            if (atomic_load_explicit(&g_ring_task_tag[p_idx],
                                     memory_order_acquire) != p_full) {
                ed_edge_unlock(p_idx);
                /* §5.12.3 情形③: 第一趟到第二趟之间, p 完成并被 ring 换代.
                 * 相当于 p 已经 FIN 且不会为这条边发 Hook 2 减 unfin;
                 * 也不会为这条边发 Hook 0 加 dispatch_fanin.
                 * 需要人工把两个字段都各"消耗一次":
                 *   - g_unfin_pred_cnt --1  (等价于本应有的 Hook 2)
                 *   - g_dispatch_fanin +1  (等价于本应有的 Hook 0)
                 * 然后进入 maybe_enter_staging 尝试. */
                atomic_fetch_sub_explicit(&g_unfin_pred_cnt[s_idx], 1,
                                          memory_order_acq_rel);
                uint16_t v = atomic_fetch_add_explicit(&g_dispatch_fanin[s_idx],
                                                        1, memory_order_acq_rel) + 1;
                atomic_fetch_add_explicit(&g_ed_late_arrival_cnt, 1,
                                          memory_order_relaxed);
                ed_maybe_enter_staging(s_full, s_idx, v);
                continue;
            }

            /* 锁内 append: 现在才把 s 挂到 p 的 successor list */
            uint16_t k_pos = g_successor_buf[p_idx].cnt;
            g_successor_buf[p_idx].node[k_pos] = (uint16_t)s_full;
            g_successor_buf[p_idx].cnt = k_pos + 1;
            g_state_buf[p_idx].successor_cnt++;

            /* 锁内读 dispatch_tag: 决定"append 是否在 Hook 0 之后" */
            bool dispatched_this_gen =
                atomic_load_explicit(&g_dispatch_tag[p_idx],
                                     memory_order_acquire) == p_full;
            ed_edge_unlock(p_idx);

            if (!dispatched_this_gen) continue;   /* Hook 0 尚未跑本代 p, 它将来会看到 append 并 +1 */

            /* late-arrival: 补一次 dispatch_fanin */
            uint16_t v = atomic_fetch_add_explicit(&g_dispatch_fanin[s_idx],
                                                    1, memory_order_acq_rel) + 1;
            atomic_fetch_add_explicit(&g_ed_late_arrival_cnt, 1,
                                      memory_order_relaxed);
            ed_maybe_enter_staging(s_full, s_idx, v);
        }

        ptr->exp += original_cnt;  ptr->cnt = 0;   /* 回收 predecessor_list */
        g_commit_task_id++;
    }
}
```

> **实现细节**：
> - **两趟结构的意义**：第一趟只读、不加锁，可以线性遍历所有 pred 并算 target；第二趟才独立进每条边的锁 append。ed_init_task_meta 在两趟之间，是"锚定 target 的屏障"，确保 Hook 0 遍历本代 s 时读到的 target 就是最终值。
> - **不再有原 while 循环直接写 `g_successor_buf`**：已迁移到第二趟锁内 append。原 cutter 代码里 `g_successor_buf[precessor_idx].node[successor_idx] = g_commit_task_id;` 一行**必须删掉**，否则会双计。
> - **`g_dispatch_tag` 生命周期**：设计方案 §5.20 表格里 T3 写入，T0 覆写，**T6 不清**。add_successors 靠"本代 tag == p_full"判定"曾 dispatch"，比读瞬时 `g_task_dispatch_record` 更可靠（后者在 T6 已被清）。
> - **`ed_edge_lock` 实现**：一期用 `atomic_flag`（`__c11_atomic_flag_test_and_set` spin），因为持锁时间极短（几十条指令）。锁数量 = `RING_SIZE = 4096`，各占 1 字节，共 4KB，不需要动态分配。
> - **CON_NODE_CNT = 32 上限**：即 `survivors[]` 的 stack 上限。若 workload 里某 s-task pred 数 > 32，需要在 M2 前扩到 64（qwen3 / paged_attention 里未见 > 32，一期够用）。
> - **一期只处理 `count == 1`**：其他 s-task 不进 ed 流程；`ed_maybe_enter_staging` 内部已 check。

#### 改动 B：`resolve_dep` 里加 Hook 2 分岔（simpler 对齐版：统一 push + 敲 doorbell）

现有代码在 `g_predecessor_cnt[succ_id & RING_MASK] < 1` 时直接入 ready_queue，改为按设计方案 5.10 的 Hook 2 序列：**先 CAS spec_state → 若曾进过 ed 流程就读 g_staged_slot 敲 doorbell → 无论 stage 与否统一 push ready_queue**（避免重复由 send_task 靠 `next_block_idx` 去重）：

```c
for (uint16_t k = 0; k < succ_cnt; k++) {
    succ_id = g_successor_buf[idx].node[k];
    uint16_t s_idx = succ_id & RING_MASK;
    g_predecessor_cnt[s_idx]--;
#if ED_ENABLE
    uint16_t old_unfin = atomic_fetch_sub_explicit(
        &g_unfin_pred_cnt[s_idx], 1, memory_order_acq_rel);
    assert(old_unfin > 0);
#endif
    if (g_predecessor_cnt[s_idx] < 1) {
#if ED_ENABLE
        /* release 权限只属于观察到 1->0 的唯一 FIN；不得按空转时间推断。 */
        assert(old_unfin == 1);
#endif
        task_type_t type = g_basic_buf[succ_id].type;
#if ED_ENABLE
        /* ① CAS(NONE 或 STAGING -> DISPATCHED), 用 seq_cst 与 Hook 1 ⑦ 配对 */
        uint8_t old = atomic_exchange_explicit(
            &g_spec_state[s_idx], ED_SPEC_DISPATCHED,
            memory_order_seq_cst);

        if (old == ED_SPEC_STAGING) {
            /* ② 曾进过 ed 流程 (Hook 0 或 late-arrival 补齐时 push 过队列), 
             *    用 seq_cst atomic_load 读打包 slot 记录. 
             *    !!! 不要用 atomic_thread_fence + non-atomic struct 读: 
             *        C11 里那是 UB, 见设计方案 §5.13. */
            uint64_t record = atomic_load_explicit(
                &g_staged_slot_record[s_idx], memory_order_seq_cst);
            if (ED_RECORD_TAG(record) == (uint32_t)succ_id &&
                ED_UNPACK_CORE(ED_RECORD_SLOT(record)) != ED_STAGED_CORE_INVALID) {
                ed_notify_once(succ_id, record, ED_NOTIFY_HOOK2);
            }
            /* 若 core == INVALID (Hook 1 还没完成 stage): 不敲, 靠 stager ⑦ 自敲兜底 */
        } else if (old == ED_SPEC_DISPATCHED) {
            /* 单 cutter 线程 + predecessor_cnt 单调递减 => 不应发生; 出现即 bug */
            WORKER_LOGF("[ed] BUG: Hook 2 fired twice for s=%u", succ_id);
        }
        /* old == ED_SPEC_NONE: 从未走过 ed (fanin 未齐 或 SPMD/count 约束); 不用敲 */
#endif
        /* ③ 无论 ed 走没走, 统一 push ready_queue;
         *    send_task 会靠 next_block_idx 去重跳过 stager 已抢的块 */
        rq_buf[type][ready_cnt[type]++] = succ_id;
    }
}
```

> **关键点**：
> - 用 `atomic_exchange` 一次搞定 `NONE → DISPATCHED` 或 `STAGING → DISPATCHED`，比两次 CAS 简洁。
> - `g_staged_slot_record[s_idx]` 同时校验完整 task tag 和 core 有效性，拒绝 ring 复用后的旧记录。
> - `ed_notify_once` 先 CAS `notify_claimed 0→1`。只有胜者写一次 doorbell 并把 `slot_state` 从 GATED 发布为 RUNNABLE；Hook 2 与 stager 不再各自直接 store doorbell。
> - **不再有"跳过 ready_queue"分支**：所有 s-task 一律 push；send_task 侧靠 `next_block_idx == count` 检查跳过 stager 已抢的块。
> - `old_unfin == 1` 是 Hook 2 的必要前置条件和测试断言；任何 drain/超时路径都没有 release 权限。

### 3.4 [src/main.c](../../src/main.c) 激活 executor

取消注释现有的 executor 相关行：

```c
executor_init();
for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
    pthread_create(&executor_threads[i], NULL, executor_worker, (void *)(intptr_t)i);
}
/* ... */
for (int i = 0; i < EXECUTOR_THREAD_CNT; i++) {
    pthread_join(executor_threads[i], NULL);
}
```

同时在 orchestration 结束后打印 ed metrics 并做泄漏扫描 & 一致性断言：

```c
#if ED_ENABLE
uint64_t stage_cnt          = atomic_load(&g_ed_stage_cnt);
uint64_t hit_cnt            = atomic_load(&g_ed_hit_cnt);
uint64_t self_notify_cnt    = atomic_load(&g_ed_self_notify_cnt);
uint64_t slot_retry_cnt     = atomic_load(&g_ed_slot_retry_cnt);
uint64_t block_cas_fail_cnt = atomic_load(&g_ed_block_cas_fail_cnt);
uint64_t send_skip_cnt      = atomic_load(&g_ed_send_skip_cnt);
uint64_t late_arrival_cnt   = atomic_load(&g_ed_late_arrival_cnt);

MAIN_LOGF("[ed] stage_cnt        = %llu", (unsigned long long)stage_cnt);
MAIN_LOGF("[ed] hit_cnt          = %llu", (unsigned long long)hit_cnt);
MAIN_LOGF("[ed] self_notify_cnt  = %llu", (unsigned long long)self_notify_cnt);
MAIN_LOGF("[ed] slot_retry_cnt   = %llu", (unsigned long long)slot_retry_cnt);
MAIN_LOGF("[ed] block_cas_fail   = %llu", (unsigned long long)block_cas_fail_cnt);
MAIN_LOGF("[ed] send_skip_cnt    = %llu", (unsigned long long)send_skip_cnt);
MAIN_LOGF("[ed] late_arrival_cnt = %llu", (unsigned long long)late_arrival_cnt);

if (stage_cnt > 0) {
    /* 恰好一次通知：两个来源的计数之和必须等于成功 stage 数。 */
    MAIN_LOGF("[ed] doorbell_ratio  = %.2f%% (必须为 100%%)",
              100.0 * (hit_cnt + self_notify_cnt) / stage_cnt);
    /* send_skip 应 == stage_cnt (每次成功 stage 都对应一次 send_task 跳过);
     * 若 send_skip > stage_cnt: send_task 与 stager CAS 撞车导致 send_task 也 skip 
     *                            (block_cas_fail 增而 stage_cnt 未增), 由 CAS 撞车统计. */
    MAIN_LOGF("[ed] skip_check      = send_skip_cnt(%llu) vs stage_cnt(%llu)",
              (unsigned long long)send_skip_cnt, (unsigned long long)stage_cnt);
}

/* 泄漏扫描: 结束时不能有 s-task 停在 STAGING;
 * 扫描范围以 g_task_id 为上界 (RING 环回后每个 slot 是最后一代任务), 
 * 用 g_basic_buf[i].count 判定 slot 是否被使用过. */
uint32_t leaked_staging = 0;
uint32_t block_leaked   = 0;
uint32_t slot_leaked    = 0;
uint16_t last_gen_count = (uint16_t)((atomic_load(&g_task_id) >= RING_SIZE)
                                     ? RING_SIZE : atomic_load(&g_task_id));
for (uint16_t i = 0; i < last_gen_count; i++) {
    if (g_basic_buf[i].count == 0) continue;      /* 未使用的 slot 跳过 */
    uint8_t st = atomic_load(&g_spec_state[i]);
    if (st == ED_SPEC_STAGING) leaked_staging++;
    uint16_t nbi = atomic_load(&g_next_block_idx[i]);
    if (nbi != g_basic_buf[i].count) block_leaked++;   /* 未派发或部分派发 */
}
/* 扫描 executor slot_state，结束时应全 EMPTY。 */
for (int t = 0; t < EXE_TYPE_CNT; t++) {
    for (int c = 0; c < AIC_CNT; c++) {
        for (int s = 0; s < AIC_OSTD; s++) {
            if (atomic_load(&g_executors[t][c].slot_state[s]) != EXE_SLOT_EMPTY) slot_leaked++;
        }
    }
}
MAIN_LOGF("[ed] leaked_staging  = %u", leaked_staging);
MAIN_LOGF("[ed] block_leaked    = %u", block_leaked);
MAIN_LOGF("[ed] slot_leaked     = %u", slot_leaked);
#endif
```

### 3.5 [Makefile](../../Makefile) 增加 ED_ENABLE 参数 + 注册新增 case

#### 3.5.0 【必做】把新增 case 加进 `ALL_CASES` 白名单

现有 Makefile 对 `CASE` 有**白名单校验**（实测）：

```make
ALL_CASES := \
	qwen3_dynamic_manual_scope.h \
	qwen3_dynamic_tensormap.h \
	paged_attention_unroll.h \
	paged_attention_unroll_manual_scope.h

CASE ?= qwen3_dynamic_manual_scope.h

ifneq ($(filter $(CASE),$(ALL_CASES)),$(CASE))
$(error Unknown CASE='$(CASE)'. Valid: $(ALL_CASES))
endif
```

因此 §5.4 的 A11/A12 探针 case、以及被 A12 场景1 引用的 `qwen3_dynamic_tensormap_ot1.h`（**当前不在白名单**）都必须先加进 `ALL_CASES`，否则 `make CASE=ed_a11_probe.h` / `make CASE=qwen3_dynamic_tensormap_ot1.h` 会直接 `$(error Unknown CASE=...)` 中断。修改为：

```make
ALL_CASES := \
	qwen3_dynamic_manual_scope.h \
	qwen3_dynamic_tensormap.h \
	qwen3_dynamic_tensormap_ot1.h \
	paged_attention_unroll.h \
	paged_attention_unroll_manual_scope.h \
	ed_a11_probe.h \
	ed_a12_ring_stress.h
```

> 若不想把测试专用 case 混进主白名单，可另设 `ED_TEST_CASES` 变量并在校验行改成 `$(filter $(CASE),$(ALL_CASES) $(ED_TEST_CASES))`；A10~A12 脚本在跑测试 case 前 `export` 该变量即可。二选一，一期推荐直接并入 `ALL_CASES` 简单省事。

#### 3.5.1 加 ED_ENABLE / ED_UNFIN_THRESHOLD 参数

在 `MAIN_LOG` 那段之后加：

```make
ifneq ($(ED_ENABLE),)
CFLAGS   += -DED_ENABLE=$(ED_ENABLE)
endif

ifneq ($(ED_UNFIN_THRESHOLD),)
CFLAGS   += -DED_UNFIN_THRESHOLD=$(ED_UNFIN_THRESHOLD)
endif
```

**关键**：切换 `ED_ENABLE=0/1` 不仅 `main.o` 要重建，`dispatch.o`/`cutter.o`/`executor.o`/`early_dispatch.o` 都需要重建（这些文件里有 `#if ED_ENABLE` 分支）。现有 `ORCH_STAMP` 只作用于 `main.o`，需要扩展依赖：

```make
# ORCH_STAMP 影响的对象扩展到所有含 #if ED_ENABLE 的文件
ORCH_CONFIG := case=$(CASE) tier=$(QWEN3_SPMD_TIER) log=$(MAIN_LOG) ed=$(ED_ENABLE) ed_thr=$(ED_UNFIN_THRESHOLD)

$(OBJ_DIR)/main.o \
$(OBJ_DIR)/algorithm/dispatch.o \
$(OBJ_DIR)/algorithm/cutter.o \
$(OBJ_DIR)/algorithm/executor.o \
$(OBJ_DIR)/algorithm/early_dispatch.o : $(ORCH_STAMP)
```

或者简单起见，`ED_ENABLE` 变化时强制 `make clean`：脚本 `scripts/ed_bench.sh` 里每次 case 循环都 `make -s clean` 已经隐式满足。文档 §5.3 的脚本已经按 clean 后重建的方式实现，测试路径安全，但**开发调试路径**（不 clean 就切 `ED_ENABLE`）需要显式加上面的依赖关系，否则会构建出"部分文件按 ED=0 编译 + 部分文件按 ED=1 编译"的错乱二进制。

### 3.6 新增源文件 [src/algorithm/early_dispatch.c](../../src/algorithm/early_dispatch.c)

推荐把 `propagate_dispatch_fanin`、`try_early_dispatch`、`pick_stage_core`、`ed_notify_once`、per-pred edge lock、`ed_init`、generation-tagged record helper、`ed_init_task_meta` 以及所有 ed 全局变量集中到这个新文件。

本文件**不得**实现按超时/空转轮数 kick 的 drain。唯一允许的收尾扫描是只读断言；只有 Hook 2 观察到 `g_unfin_pred_cnt` 从 1 变 0 才有 release 权限。

同步在 [Makefile](../../Makefile) 的 `SRCS` 里加一行：

```make
SRCS := \
	src/main.c \
	src/algorithm/executor.c \
	src/algorithm/dispatch.c \
	src/algorithm/cutter.c \
	src/algorithm/early_dispatch.c \
	src/algorithm/manager.c \
	src/algorithm/log.c \
	src/algorithm/shm.c
```

---

## 4. `ED_ENABLE` 开关设计

### 4.1 生效方式

- **编译期宏**：`conf.h` 提供默认值 `#define ED_ENABLE 1`；[Makefile](../../Makefile) 通过 `-DED_ENABLE=$(ED_ENABLE)` 覆盖
- **用法**：
  ```bash
  make CASE=qwen3_dynamic_manual_scope.h ED_ENABLE=0 run   # 基线
  make CASE=qwen3_dynamic_manual_scope.h ED_ENABLE=1 run   # 开 ed
  ```
- **代码风格**：所有 ed 逻辑都用 `#if ED_ENABLE ... #endif` 包住；`ED_ENABLE=0` 时编译产物**只保留"激活的 executor_worker"这一改动**，业务逻辑等价原 baseline

### 4.2 为什么不做运行时开关

- 一期不追求"同一二进制内动态切换"，因为 metrics 收集只在一次运行内有意义
- 运行时开关会导致 `#if` 变成 `if`，dispatch/executor 热路径每 loop 都要判断，反而引入固定开销
- 若后续需要"同二进制多次跑对比"，可再补一个 `g_ed_enabled` runtime 变量（M2 后再决定）

---

## 5. 测试方案

### 5.1 测试矩阵

| 维度 | 取值 |
| --- | --- |
| CASE | 4 个：`qwen3_dynamic_manual_scope.h`、`qwen3_dynamic_tensormap.h`、`paged_attention_unroll.h`、`paged_attention_unroll_manual_scope.h` |
| ED_ENABLE | 0（baseline）、1（ed 开） |
| QWEN3_SPMD_TIER | 仅 qwen3 case 时用默认 `2`（可选扩展到全 tier） |
| 重复次数 | 每组 5 次，取中位数 |

共 **4 × 2 × 5 = 40 次** 跑，人工/脚本可控。

### 5.2 采集指标

**基础指标（`ED_ENABLE=0/1` 都有）**：
- `orchestration.task_cnt` / `orchestration.subtask_cnt`（编排产物，用于正确性对齐）
- `completed_task_cnt`（run 结束时 cutter 统计的完成数）
- `orchestration.elapsed_time`（编排耗时）
- `dispatch.elapsed_ns` / `dispatch.task_tp (MTasks/s)`（调度端到端）

**ed 特有（`ED_ENABLE=1` 才有）**：
- `ed.stage_cnt`（Hook 1 stage 成功次数，即 executor 数据写入成功次数）
- `ed.hit_cnt`（Hook 2 敲 doorbell 次数）
- `ed.self_notify_cnt`（Hook 1 stager 自敲 doorbell 次数，Hook 2 抢先场景）
- `ed.slot_retry_cnt`（Hook 1 抢 slot 失败并 re-push 到 ed_ready_queue 的次数）
- `ed.send_skip_cnt`（send_task 因 next_block_idx 已满被 stager 抢占而跳过的次数，理论应 == stage_cnt）
- `ed.leaked_staging`（用例结束时 spec_state 残留 STAGING 数量，应 = 0）
- `ed.block_leaked`（用例结束时 next_block_idx 处于中间态数量，应 = 0）

**推导指标**：
- `ed.doorbell_ratio` = `(hit_cnt + self_notify_cnt) / stage_cnt`（stage 完的 s-task 中通过 doorbell 越 gate 的比例）
- `ed.self_notify_ratio` = `self_notify_cnt / (hit_cnt + self_notify_cnt)`（Hook 2 抢先的比例，可用来观察并发密度）

**推导指标**：
- 端到端加速比 = `elapsed_ns(ED=0) / elapsed_ns(ED=1)`
- 吞吐提升率 = `(tp(ED=1) - tp(ED=0)) / tp(ED=0)`

### 5.3 测试脚本

新增 [scripts/ed_bench.sh](../../scripts/ed_bench.sh)：

```bash
#!/usr/bin/env bash
# scripts/ed_bench.sh - early-dispatch 有无对比 benchmark
set -euo pipefail

CASES=(
    qwen3_dynamic_manual_scope.h
    qwen3_dynamic_tensormap.h
    paged_attention_unroll.h
    paged_attention_unroll_manual_scope.h
)
REPEAT=${REPEAT:-5}
LOG_DIR="log/ed_bench_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$LOG_DIR"

for case_h in "${CASES[@]}"; do
    for ed in 0 1; do
        for rep in $(seq 1 "$REPEAT"); do
            log_file="$LOG_DIR/${case_h%.h}_ed${ed}_r${rep}.log"
            echo "[bench] case=$case_h ed=$ed rep=$rep -> $log_file"
            make -s clean >/dev/null
            make -s CASE="$case_h" ED_ENABLE="$ed" >/dev/null
            ./bin/esl_proxy > "$log_file" 2>&1
        done
    done
done

# 汇总: 用 awk grep 出关键指标到 CSV
python3 scripts/ed_bench_summary.py "$LOG_DIR" > "$LOG_DIR/summary.csv"
cat "$LOG_DIR/summary.csv"
```

配套 [scripts/ed_bench_summary.py](../../scripts/ed_bench_summary.py)（伪代码）：
- 遍历 `$LOG_DIR/*.log`
- grep `\[orchestration\]`、`\[scheduler\]`、`\[ed\]` 行
- 每 case × ed 组合取 5 次中位数
- 输出 CSV：`case,ed_enable,elapsed_ns_median,task_tp_median,stage_cnt,hit_cnt,self_notify_cnt,doorbell_ratio,slot_retry_cnt,send_skip_cnt`

### 5.4 正确性断言

脚本 `ed_bench_summary.py` 内置以下检查，任一失败即 fail：

- **A1** `completed_task_cnt == task_cnt`（两种模式下都要满足）
- **A2** `ED_ENABLE=1` 且运行结束时 `ed.hit_cnt + ed.self_notify_cnt == ed.stage_cnt`（每个成功 stage 恰好一个通知胜者）
- **A3** 对每个成功 stage 的 task，`notify_claimed == 1`；Hook 1/2 任一 CAS 失败分支的 doorbell 写次数必须为 0
- **A4** `ED_ENABLE=0/1` 下 `task_cnt` 完全相同（保证 workload 一致）
- **A5** 用例结束时 `[ed] leaked_staging == 0`（无 `spec_state` 残留 STAGING；残留说明 Hook 2 未触发或流程泄漏；扫描范围是 `i < min(g_task_id, RING_SIZE) && g_basic_buf[i].count > 0`——排除未使用的 ring slot）
- **A6** 用例结束时 `[ed] block_leaked == 0`（所有活跃任务 `next_block_idx == count`；normal dispatch 必须直接 `0→count`，Hook 1 仅 count==1）
- **A7** `abs(ed.send_skip_cnt - ed.stage_cnt) <= ed.block_cas_fail_cnt`（每次成功 stage 至少对应一次 send_task 跳过；send_task 与 stager CAS 撞车时 stager 可能失败但 send_task 也可能失败，`block_cas_fail_cnt` 记录 stager 侧回退次数——两侧统计差不超过 CAS 撞车次数）
- **A8** 用例结束时 `[ed] slot_leaked == 0`（所有 executor `slot_state == EMPTY`）
- **A9** bitmap 消费计数满足：每个完成 bit 只属于一次 `atomic_exchange` 快照，`completed_queue` 中 task-id 与该快照对应 map 一致，无重复回收

#### A10~A12 详细断言与可复现构造

**设计方案 §8.4 已给出实验构造与判据的原始版本，本节把它们落到具体 shell 命令与脚本判据。**

##### A10 Hook 1 与 Hook 2 两种先后次序都各覆盖

**断言判据**：

| 判据 ID | 断言 | 数据源 |
| --- | --- | --- |
| A10.1 | `ed.hit_cnt >= 1`（至少 1 次 Hook 2 敲） | `[ed] hit_cnt` 打印 |
| A10.2 | `ed.self_notify_cnt >= 1`（至少 1 次 Hook 1 自敲） | `[ed] self_notify_cnt` 打印 |
| A10.3 | `ed.hit_cnt + ed.self_notify_cnt == ed.stage_cnt`（每次 stage 恰好一个通知者） | 与 A2 重复 |
| A10.4 | 单个 s-task 的 doorbell 写次数恒为 1（新增 `WORKER_LOGF("notify_write, s=%u, source=%s")`；脚本聚合验证） | log 抽查 |
| A10.5 | ring slot 复用后旧代 s-task 的 doorbell/state 不被新代读到（新增 `WORKER_LOGF("slot_free, task=%u, tag=%u")`；tag 与 task 完整一致） | log 抽查 |

**可复现构造**：

```bash
# scripts/ed_a10_probe.sh
set -euo pipefail
LOG_DIR="log/a10_$(date +%Y%m%d_%H%M%S)"; mkdir -p "$LOG_DIR"
CASES=(qwen3_dynamic_manual_scope.h paged_attention_unroll.h)
for c in "${CASES[@]}"; do
  for r in 1 2 3 4 5; do
    make -s clean >/dev/null
    make -s CASE="$c" ED_ENABLE=1 WORKER_LOG=1 >/dev/null
    ./bin/esl_proxy > "$LOG_DIR/${c%.h}_r${r}.log" 2>&1
  done
done

python3 - <<'PY' "$LOG_DIR"
import re, sys, glob
log_dir = sys.argv[1]
for f in glob.glob(f"{log_dir}/*.log"):
    hit = int(re.search(r"\[ed\] hit_cnt\s*=\s*(\d+)", open(f).read()).group(1))
    self_ = int(re.search(r"\[ed\] self_notify_cnt\s*=\s*(\d+)", open(f).read()).group(1))
    stage = int(re.search(r"\[ed\] stage_cnt\s*=\s*(\d+)", open(f).read()).group(1))
    assert hit + self_ == stage, f"A10.3 fail: {f} hit={hit} self={self_} stage={stage}"
    # 单文件不要求都触发两个分支; 但汇总 5 次至少各有 1 次
# 汇总
files = glob.glob(f"{log_dir}/*.log")
any_hit  = any(int(re.search(r"\[ed\] hit_cnt\s*=\s*(\d+)", open(f).read()).group(1)) > 0 for f in files)
any_self = any(int(re.search(r"\[ed\] self_notify_cnt\s*=\s*(\d+)", open(f).read()).group(1)) > 0 for f in files)
assert any_hit,  "A10.1 fail: 5 次均无 Hook 2 敲, 需在 try_early_dispatch ④c 前手动 sched_yield 强制 Hook 2 抢先"
assert any_self, "A10.2 fail: 5 次均无 Hook 1 自敲, 说明并发密度低; 需在 resolve_dep xchg 前手动 sched_yield 让 Hook 1 落后"
# doorbell writes 计数
mismatch = []
for f in files:
    from collections import Counter
    cnt = Counter()
    for line in open(f):
        m = re.search(r"notify_write, s=(\d+)", line)
        if m: cnt[m.group(1)] += 1
    bad = [(t, n) for t, n in cnt.items() if n != 1]
    if bad: mismatch.append((f, bad))
assert not mismatch, f"A10.4 fail: {mismatch}"
print("A10 PASS")
PY
```

若 A10.1 或 A10.2 之一在 5 次跑内都未触发，需在 `try_early_dispatch` 步骤 ④c 前或 `resolve_dep` xchg 前**临时**加 `sched_yield()`（仅调试用，不进主干）强制覆盖对应分支；覆盖后立即回滚 yield 补丁。

##### A11 合法 STAGING + `g_unfin_pred_cnt > 0` 空转，任务不得进 RUNNABLE

**断言判据**：

| 判据 ID | 断言 | 数据源 |
| --- | --- | --- |
| A11.1 | 空转期间 `spec_state[S] == STAGING`（未被 kick） | `WORKER_LOGF("slot_state_dump, s=%u, unfin=%u, spec=%u, state=%u, doorbell=%u")` 每 1e4 tick 一次 |
| A11.2 | 空转期间 `slot_state[core_S][slot_S] == GATED`（未升 RUNNABLE） | 同上 |
| A11.3 | 空转期间 `doorbell[core_S][slot_S] == 0`（未误敲） | 同上 |
| A11.4 | 最后一个 pred FIN 后**同一 cutter tick** 内 `slot_state == RUNNABLE` | 同上 |

**可复现构造**——新增 `cases/ed_a11_probe.h`。⚠️ 必须用本仓真实建图 API（对照 `cases/qwen3_dynamic_manual_scope.h`）：`new_task(g_task_id, type, count, duration)`（4 参、无返回值）+ `set_task_type` + `add_predecessors(task_id, target[], n, start)`，用全局 `atomic_int g_task_id` 递增来「提交/发布」任务（本仓 manual-scope 风格没有 `submit()`/`commit()`；`g_task_id++` 即让 cutter 可见）。**原方案里的 `new_task(TASK_TYPE_..,1,dur)` 返回 id + `commit()` 这套 API 在本仓不存在**。

```c
// cases/ed_a11_probe.h
// A11 探针 case: 验证 §5.15 "合法 STAGING + g_unfin_pred_cnt>0 空转时, s-task 不得进 RUNNABLE".
//
//   P1 (先 FIN) --\
//                  >--> S (count=1, pred={P1,P2})
//   P2 (后 FIN) --/
//
// P1、P2 都 dispatch 后 S 的 dispatch_fanin 齐 -> spec_state=STAGING;
// 但只要 P1/P2 有一个未 FIN, S 的 g_unfin_pred_cnt>0, 必须始终 GATED.
#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_completed_cnt;

int g_subtask_cnt = 0;   /* 每个被编译的 case 各自定义 (见 manual_scope.h:45) */

/* ⚠️ duration 是 uint16_t; dispatch 落 slot 时做 (raw>10000)?raw/10000:1 缩放
 * (dispatch.c:104-106, 见 §1.2 C3), 单 block 最多约 6 tick。所以无法用「超长单
 * 任务」制造长空转窗口——原方案 dur=100000/200000 既溢出 uint16_t 又被 /10000,
 * 反而更短。这里改用 P2 的 SPMD count 放大执行时长: P2 逐 block 递推, 总时长
 * ≈ 6 * ED_A11_P2_BLOCKS tick, 从而拉长 "P1 已 FIN、P2 未 FIN" 的可观测窗口。
 * 该窗口必须远大于 executor 的 dump 采样周期 ED_A11_DUMP_PERIOD (见下), 否则采不到样本。*/
#ifndef ED_A11_P2_BLOCKS
#define ED_A11_P2_BLOCKS 8192   /* ≈ 6*8192 ≈ 49152 tick 窗口, 相对 dump 周期 500 有约 ~98 次采样 */
#endif

static inline void set_task_type(uint16_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    (void)orch_args;
    uint16_t preds[2];

    /* P1: 单 block, 最短执行 (~1 tick), 先 FIN */
    new_task(g_task_id, TASK_TYPE_CUBE, 1, 1);
    set_task_type(g_task_id, TASK_TYPE_CUBE);
    const uint16_t p1 = g_task_id;
    g_task_id++;

    /* P2: SPMD 多 block, 明显晚于 P1 FIN (duration<65535 避免 uint16_t 溢出) */
    new_task(g_task_id, TASK_TYPE_CUBE, ED_A11_P2_BLOCKS, 60000);
    set_task_type(g_task_id, TASK_TYPE_CUBE);
    const uint16_t p2 = g_task_id;
    g_task_id++;

    /* S: count=1, 依赖 {P1, P2} */
    new_task(g_task_id, TASK_TYPE_VECTOR, 1, 1);
    set_task_type(g_task_id, TASK_TYPE_VECTOR);
    const uint16_t s = g_task_id;
    preds[0] = p1;
    preds[1] = p2;
    add_predecessors(g_task_id, preds, 2, 0);
    MAIN_LOGF("[a11] probe_s=%u", s);   /* 稳定锚点, 供脚本提取目标 s-task */
    g_task_id++;

    g_completed_cnt++;   /* 与其它 case 一致, 标记本 orchestration 完成 */
}
```

> **注**：`add_predecessors` 会跳过 `target[i] < g_min_uncomplete_task` 的前驱（已完成的 pred 不再计入）。orchestration 在极短几条指令内建完 P1/P2/S，P1/P2 几乎不可能在此窗口内被 executor 取走并完成，故 S 建边时两 pred 仍未完成、`add_predecessors` 会记满 2 条边——A11 的前提成立。若在极端调度下偶发 pred 被提前跳过，`[a11] probe_s` 之后无「unfin>0」样本，脚本判 A11.0 未覆盖并重跑即可。

新增 executor 里的 dump 打点（仅 A11 编译时开启）：

```c
#if ED_A11_PROBE
/* ⚠️ 原方案写成 (iterations & 0x2710)==0 是 bug: 0x2710=10000, 按位与并非「每 1e4 次」。
 * 用取模才是真正的周期采样。周期取小 (默认 500), 保证 P2 执行窗口 (≈6*ED_A11_P2_BLOCKS
 * tick) 内能被采样到多次; 若把 P2 block 数调小, 需同步把周期调更小。 */
#ifndef ED_A11_DUMP_PERIOD
#define ED_A11_DUMP_PERIOD 500
#endif
if ((iterations % ED_A11_DUMP_PERIOD) == 0) {
    for (int t = 0; t < EXE_TYPE_CNT; t++)
    for (int c = 0; c < AIC_CNT; c++)
    for (int sl = 0; sl < AIC_OSTD; sl++) {
        uint16_t task = g_executors[t][c].tasks[sl];
        uint16_t s_idx = task & RING_MASK;
        WORKER_LOGF("slot_state_dump, s=%u, unfin=%u, spec=%u, state=%u, doorbell=%u",
                    task,
                    atomic_load(&g_unfin_pred_cnt[s_idx]),
                    atomic_load(&g_spec_state[s_idx]),
                    atomic_load(&g_executors[t][c].slot_state[sl]),
                    atomic_load(&g_executors[t][c].doorbell[sl]));
    }
}
#endif
```

**判据脚本**：⚠️ 用 `EXTRA_CFLAGS=` 传探针宏，**不要**用 `CFLAGS+=`——命令行 `CFLAGS+=` 会覆盖 Makefile 里的 `CFLAGS :=`（丢掉 `-I include/algorithm -DORCH_CASE=...` 等）导致构建失败；Makefile 已内置 `EXTRA_CFLAGS` 追加钩子（第 62-64 行）。

```bash
# scripts/ed_a11_probe.sh
make -s clean && make -s CASE=ed_a11_probe.h ED_ENABLE=1 WORKER_LOG=1 EXTRA_CFLAGS=-DED_A11_PROBE=1 >/dev/null
./bin/esl_proxy > log/a11.log 2>&1

s_id="$(awk -F'probe_s=' '/\[a11\] probe_s=/{print $2; exit}' log/a11.log | awk '{print $1}')"
test -n "$s_id" || { echo "FAIL A11.0 missing [a11] probe_s"; exit 1; }
# 注意: 用 POSIX 可移植 awk 解析 (split on ", "/"="), 不用 gawk 专属的 3 参 match()。
# macOS 自带 awk(BWK) 不支持 `match($0, re, arr)`, 否则脚本在 darwin 上直接报错。
awk -v s_id="$s_id" -F'[ ,]+' '
$0 ~ ("slot_state_dump, s=" s_id ",") {
    seen=1
    for (i = 1; i <= NF; i++) {
        n = index($i, "=")
        if (n == 0) continue
        k = substr($i, 1, n-1); v = substr($i, n+1) + 0
        if      (k == "unfin")    unfin = v
        else if (k == "spec")     spec  = v
        else if (k == "state")    state = v
        else if (k == "doorbell") db    = v
    }
    if (unfin>0 && spec!=1)    { print "FAIL A11.1", $0; f++ }
    if (unfin>0 && state!=1)   { print "FAIL A11.2", $0; f++ }
    if (unfin>0 && db!=0)      { print "FAIL A11.3", $0; f++ }
    if (unfin==0 && state!=2)  { print "FAIL A11.4", $0; f++ }
}
END {
    if (!seen) { print "FAIL A11.0 no slot_state_dump for s=" s_id; f++ }
    exit (f?1:0)
}' log/a11.log
```

##### A12 Hook 0 与 `add_successors` 并发，每边 fanin 恰好一次

**断言判据**：

| 判据 ID | 断言 | 数据源 |
| --- | --- | --- |
| A12.1 | 结束时对每个非根 s：`g_dispatch_fanin[s] == g_dispatch_fanin_target[s]` | 新增结束扫描打印 `[ed] fanin_check, s=%u, cur=%u, tgt=%u` |
| A12.2 | 每条 (s, p) 边贡献者只属于 Hook 0 或 late-arrival 之一 | 通过 A12.3 间接验证 |
| A12.3 | `g_ed_late_arrival_cnt + g_ed_hook0_contrib_cnt == sum(g_dispatch_fanin_target)` | 新增计数点 |
| A12.4 | ring 卷绕后旧 tag 读取不触发 doorbell write | log 抽查 |

**新增 Hook 0 计数**（仅 A12 编译期开启）：

```c
#if ED_HOOK0_CONTRIB_STATS
extern _Atomic uint64_t g_ed_hook0_contrib_cnt;
/* 在 propagate_dispatch_fanin 里对每条边 +1: */
atomic_fetch_add_explicit(&g_ed_hook0_contrib_cnt, 1, memory_order_relaxed);
#endif
```

**结束扫描 fanin 一致性**（`src/main.c` 加入）：

```c
#if ED_ENABLE
uint16_t last = (uint16_t)((atomic_load(&g_task_id) >= RING_SIZE) ? RING_SIZE : atomic_load(&g_task_id));
uint64_t sum_target = 0;
for (uint16_t i = 0; i < last; i++) {
    if (g_basic_buf[i].count == 0) continue;
    uint16_t cur = atomic_load(&g_dispatch_fanin[i]);
    uint16_t tgt = g_dispatch_fanin_target[i];
    sum_target += tgt;
    if (cur != tgt) MAIN_LOGF("[ed] fanin_check, s=%u, cur=%u, tgt=%u  MISMATCH", i, cur, tgt);
}
MAIN_LOGF("[ed] sum_fanin_target = %llu", (unsigned long long)sum_target);
#if ED_HOOK0_CONTRIB_STATS
MAIN_LOGF("[ed] hook0_contrib_cnt = %llu", (unsigned long long)atomic_load(&g_ed_hook0_contrib_cnt));
#endif
#endif
```

**可复现构造**——新增 `cases/ed_a12_ring_stress.h`（真实 API，与 A11 同风格）：

```c
// cases/ed_a12_ring_stress.h
// A12 ring 卷绕 stress: 建 > 2*RING_SIZE 个任务, 强制 slot (task_id & RING_MASK)
// 复用, 制造旧代 tag 被下一代覆写的 stale_tag 事件, 验证 doorbell/state 不跨代误读。
//
// ⚠️ 前置依赖 (见 §1.2): 现状 add_predecessors 用裸 task_id 索引
// g_predecessors[task_id]（未 & RING_MASK, ring_buf.h:97), task_id>=RING_SIZE 会越界;
// add_successors 同理。跑本 case 前必须先完成 §1.2 的 "ring 索引全部 & RING_MASK"
// 改造, 否则会 SIGSEGV。这也是本 case 存在的意义: 它是 ring 卷绕加固的回归测试。
#include <stddef.h>
#include <stdint.h>

#include "dispatch.h"
#include "mem_pool.h"
#include "ring_buf.h"

extern atomic_int g_completed_cnt;

int g_subtask_cnt = 0;

#ifndef ED_A12_TASK_CNT
#define ED_A12_TASK_CNT 10000   /* > 2*RING_SIZE(=4096), 保证 ring 至少卷绕两轮 */
#endif

static inline void set_task_type(uint16_t task_id, task_type_t type) {
    g_basic_buf[task_id & RING_MASK].type = type;
}

void aicpu_orchestration_entry(const uint64_t orch_args) {
    (void)orch_args;
    uint16_t pred[1];
    uint16_t prev = 0xFFFF;   /* 哨兵: 首个任务无前驱 */

    for (int i = 0; i < ED_A12_TASK_CNT; i++) {
        task_type_t ty = (i & 1) ? TASK_TYPE_VECTOR : TASK_TYPE_CUBE;
        new_task(g_task_id, ty, 1, 2);
        set_task_type(g_task_id, ty);
        if (prev != 0xFFFF) {
            pred[0] = prev;
            add_predecessors(g_task_id, pred, 1, 0);   /* 链式: i 依赖 i-1 */
        }
        prev = g_task_id;
        g_task_id++;
    }
    g_completed_cnt++;
}
```

> 链式依赖天然制造「s 晚于 p dispatch」的 late-arrival，且 `new_task` 内部的 `while ((task_id - g_min_uncomplete_task) >= RING_SIZE) spin_wait();` 会在 ring 写满时阻塞，等待前面任务完成腾出 slot，从而反复触发 slot 复用（stale_tag）。`ED_A12_TASK_CNT` 可调大以提高卷绕轮数。

**判据脚本**：⚠️ 同 A11，探针宏用 `EXTRA_CFLAGS=` 传入，不要用 `CFLAGS+=`。

```bash
# scripts/ed_a12_probe.sh
set -euo pipefail

# 场景 1: qwen3 tensormap (常见 late-arrival). 注意 ot1 需已加进 Makefile ALL_CASES (§3.5.0)。
make -s clean && make -s CASE=qwen3_dynamic_tensormap_ot1.h ED_ENABLE=1 \
    EXTRA_CFLAGS="-DED_HOOK0_CONTRIB_STATS=1" >/dev/null
./bin/esl_proxy > log/a12_qwen3.log 2>&1

python3 - log/a12_qwen3.log <<'PY'
import re, sys
txt = open(sys.argv[1]).read()
la  = int(re.search(r"late_arrival_cnt\s*=\s*(\d+)", txt).group(1))
h0  = int(re.search(r"hook0_contrib_cnt\s*=\s*(\d+)", txt).group(1))
tgt = int(re.search(r"sum_fanin_target\s*=\s*(\d+)", txt).group(1))
mismatches = re.findall(r"fanin_check.*MISMATCH", txt)
assert not mismatches, f"A12.1 fail: {mismatches}"
assert la + h0 == tgt, f"A12.3 fail: late={la} hook0={h0} tgt={tgt}"
print("A12.1/A12.3 PASS")
PY

# 场景 2: ring 卷绕 (>4096 tasks). 先跑基线 case; 若未触发 stale_tag 自动回退 stress case.
make -s clean && make -s CASE=paged_attention_unroll_manual_scope.h ED_ENABLE=1 >/dev/null
./bin/esl_proxy > log/a12_ring.log 2>&1
if ! awk '/stale_tag/{seen=1} END{exit(seen?0:1)}' log/a12_ring.log; then
  echo "[A12] baseline case 未触发 ring 卷绕，回退到 stress case"
  make -s clean && make -s CASE=ed_a12_ring_stress.h ED_ENABLE=1 >/dev/null
  ./bin/esl_proxy > log/a12_ring.log 2>&1
fi
awk '
/stale_tag/ {seen=1}
/notify_write.*tag_mismatch/ {print "FAIL A12.4", $0; f++}
END {
    if (!seen) { print "FAIL A12.4 no stale_tag evidence"; f++; }
    exit (f?1:0);
}' log/a12_ring.log
echo "A12 PASS"
```

若基线 case（如 `paged_attention_unroll_manual_scope.h`）不足 4096 任务，脚本会自动回退到 `cases/ed_a12_ring_stress.h`。该 stress case 必须保证产生至少 1 条 `stale_tag`，否则视为 A12.4 未覆盖。

**总结**：A10~A12 全部依赖以下**新增打点/计数**，需要在实现阶段一并加入：

| 打点/计数 | 位置 | 用途 |
| --- | --- | --- |
| `WORKER_LOGF("notify_write, s=%u, source=%s")` | `ed_notify_once` CAS 胜者路径 | A10.4 |
| `WORKER_LOGF("slot_free, task=%u, tag=%u")` | `ed_complete_slot` | A10.5 |
| `WORKER_LOGF("slot_state_dump, ...")` | `executor_worker` 每 1e4 iter，仅 `ED_A11_PROBE` | A11.1~A11.4 |
| `_Atomic uint64_t g_ed_hook0_contrib_cnt` | `propagate_dispatch_fanin` 内每边 +1，仅 `ED_HOOK0_CONTRIB_STATS` | A12.3 |
| `MAIN_LOGF("[ed] fanin_check, ...")` | `main.c` 结束扫描 | A12.1 |

### 5.5 手动 sanity check（一次性）

开 `WORKER_LOG=1`，抽查任一 s-task 的完整状态转移链路：

```
[cutter] add,task_id=T,pre_cnt=P,succ_id=S              <- 加边
[dispatch] send,task_id=T,core=C,slot=X                 <- p 派发
[ed] propagate_fanin: s=S, fanin=P/P, CAS NONE->STAGING <- Hook 0
[ed] enqueue ed_ready_queue: s=S
[ed] try_ed: pop s=S, CAS next_block_idx 0->1, stage core=Cs slot=Xs
[ed] executor publish: core=Cs slot=Xs state=GATED
[ed] self-notify check: spec_state=STAGING, 不自敲               <- Hook 1 ⑦
[cutter] resolve_dep: s=S, unfin=0
[ed] release: old_unfin=1, xchg STAGING->DISPATCHED, notify CAS 0->1, state RUNNABLE
[dispatch] send_task s=S: next_block_idx=1, skip                <- 去重
[executor] gate release: core=Cs slot=Xs
[executor] completed: core=Cs slot=Xs
[cutter] completed_cnt += 1
```

**同时需要抽查一次"stager 自敲"路径**（触发 Hook 2 先于 Hook 1 ⑥ 完成）：

```
[ed] try_ed: pop s=S, CAS next_block_idx 0->1, stage core=Cs slot=Xs (未写完 staged_slot)
[cutter] resolve_dep: s=S, unfin=0                        <- Hook 2 抢跑
[ed] release: old_unfin=1, xchg STAGING->DISPATCHED, record INVALID, 不认领
[ed] self-notify: record 发布后 CAS notify 0->1, 唯一写 doorbell/state RUNNABLE
[dispatch] send_task s=S: next_block_idx=1, skip                <- 去重
[executor] gate release: core=Cs slot=Xs
```

抽查 3-5 个 s-task 覆盖：单前驱、多前驱、fanin 齐但 unfin 未到 0（如果 `ED_UNFIN_THRESHOLD` 收紧过）。

### 5.6 性能对比预期

由于本仓 `send_task` 侧原来是 fake return、真实 executor 未激活，"激活 executor 后的 `ED_ENABLE=0` 基线"和"原 fake return 版本"的绝对数会有差异。测试目的是**证明 ed 在同基线（executor 已激活）下带来正收益**，而不是跟老 fake return 比。

预期结果（供参考，非硬性阈值）：
- `ed.doorbell_ratio` = `(hit_cnt + self_notify_cnt) / stage_cnt` 必须等于 100%
- `ed.self_notify_ratio` 与 doorbell_ratio 相除后应远小于 1（自敲是补位路径，大量出现说明 Hook 2 频繁抢先，可能 stage 时机太靠后）
- `tp(ED=1) / tp(ED=0)` 应 >= 1.0，且不出现明显倒退

---

## 6. 开发顺序（增量交付）

按下面顺序开发，每一步都可独立编译/运行验证，不会破坏基线：

| Step | 内容 | 工期 | 验收 |
| --- | --- | --- | --- |
| **Step 0** | 修 pre-existing 编译错误：`EXECUTOR_THREAD_CNT` 加到 `conf.h`、`init_predecessors` 补声明、`init_ctrl_t` 迁到算法侧（见 §1.1） | 0.5 天 | 未加任何 ed 逻辑时 `make` 通过，`./bin/esl_proxy` 能正常退出（当前 fake return 语义） |
| **Step 1** | 加 `slot_state`、`notify_claimed`、generation-tagged staged/dispatch record、持久 dispatch tag 与 per-pred edge lock；初始化所有代际字段 | 0.5 天 | `ED_ENABLE=0/1` 编译；ring 复用测试拒绝旧 tag |
| **Step 2** | 激活 executor，并先闭合 normal 发布/回收：payload→release RUNNABLE、executor acquire、EMPTY→msg release、唯一 exchange 快照消费 | 1 天 | `ED_ENABLE=0` 下 4 case 完成；bitmap/task-id/slot_state 断言通过 |
| **Step 3** | normal `send_task` 加 `next_block_idx 0→count` 整任务认领 | 0.5 天 | count==1 与 count>1 均只执行一次，结束 nbi==count |
| **Step 4** | 加锁内 Hook 0 与 add_successors late-arrival 线性化协议 | 1 天 | 并发边测试每边恰好计数一次 |
| **Step 5** | 加 Hook 2（`resolve_dep` xchg spec_state + push ready_queue，暂时不敲 doorbell 因为还没 stage） | 0.5 天 | 4 case 全通过，`spec_state` 最终全 DISPATCHED 或 NONE |
| **Step 6** | 加 Hook 1 stage；Hook 1/2 统一调用 `ed_notify_once`，队列失败回退 NONE | 1.5 天 | `hit+self==stage`，无重复 doorbell，未完成依赖绝不 RUNNABLE |
| **Step 7** | executor 接入 GATED/RUNNABLE gate 状态 | 1 天 | 两种通知先后 race 与 slot ABA 用例通过 |
| **Step 8** | 加 benchmark 与 A1-A12 | 0.5 天 | 4 case × 2 ed × 5 rep 全通过 |
| **Step 9**（可选） | 收紧 `ED_UNFIN_THRESHOLD`（1、2、3）试跑，观察 doorbell_ratio/tp 曲线 | 0.5 天 | 得出经验最优 N |

**总工期：~6.5 天**（不含 Step 9）。

> **开发顺序的关键**：先闭合 normal slot 发布/bitmap 回收，再加 ed；先落实 normal `0→count`，再允许 count==1 stager 竞争。Hook 1/2 上线时必须同时经过统一 `ed_notify_once`，不存在允许双写的过渡版本。

---

## 7. 风险与预案

| 编号 | 风险 | 影响 | 预案 |
| --- | --- | --- | --- |
| **R1** | bitmap 跨线程且消费拆成多次读/清 | bit 丢失或 task-id map 与快照错配 | 改原子类型；每 bitmap 唯一 `atomic_exchange(0, acquire)`，同一快照生成 completion 后再回收 free bit |
| **R2** | executor_worker 全 slot 扫描：`AIC_CNT * EXE_TYPE_CNT * AIC_OSTD = 240` 次 atomic_load/轮，可能拖慢 executor | 端到端吞吐下降 | 一期先接受；观察后如需优化，可加"分段扫描"（每次只扫一小段）或 spin hint |
| **R3** | Hook 0 successor 遍历开销：每 dispatch 一个 task 都要遍历 successor list | dispatch loop 变慢 | 加"target 已达则跳过"快路径：`if (g_dispatch_fanin[s_idx] >= g_dispatch_fanin_target[s_idx]) continue;` |
| **R4** | block 未认领时 ed queue enqueue/re-push 失败 | STAGING 无消费者 | CAS `STAGING→NONE` 放弃 ed；收尾只读断言，禁止 kick |
| **R5** | `predecessor_list` 被 `cutter::add_successors` 消费后不可再用 | `pick_stage_core` 拿不到 p-core 集合 | 第一遍消费时快照完整 pred task-id 到 `g_ed_pred_snapshot`，选核时查 generation-tagged dispatch record |
| **R6** | ed_ready_queue 满 | 候选无法被 Hook 1 消费 | 尚未 claim block 时回退 STAGING→NONE；Hook 2 normal fallback |
| **R7** | ed 命中的 s-task 在 executor 端不写 msg_bitmap（不清 `free_bitmap`）| dispatch 下一轮以为 slot 还在 busy | 检查：ed hit 后 executor 越 gate → 递减 duration → 归 0 时写 msg_bitmap（同基线路径），slot 会被正常释放。**不需要额外处理** |
| **R8** | Hook 2 与 stager 重复写布尔 doorbell | executor 清零后迟到写污染下一代 slot | per-task `notify_claimed` CAS；仅胜者写一次并发布 RUNNABLE |
| **R9** | send_task 与 Hook 1 stager 之间 CAS `next_block_idx` 撞车 | 双方都可能 CAS 失败 | 双方 CAS 失败时都走 skip 路径（不再 stage / 不再派发）；stager 侧 CAS 失败必须**回退 slot 位**（`atomic_fetch_or free_bitmap`），否则永久 busy——见 §3.1 E 修正后的 ③' 分支 |
| **R10** | Hook 1 re-push 循环（不断 pop、抢 slot 失败、re-push）| CPU 空转 | 一期先接受（简单实现）；若观察到 slot_retry_cnt 异常大，可在 re-push 前 yield 一次或加抖动等待 |
| **R11** | Hook 0 与 successor append 并发 | 非原子边表 data race，漏计/双计 | per-pred 锁线性化 append、dispatch tag 发布和遍历；tag 带 generation 且完成后不清 |
| **R12** | staged/core record ring 复用 ABA | 敲到新任务 slot | `_Atomic uint64_t` record 同时携带完整 task tag 与 packed slot，读取必须匹配 tag |
| **R13** | 合法 STAGING 被空转 drain 提前释放 | 未完成依赖任务执行 | 删除 drain；仅 `g_unfin_pred_cnt` 的 1→0 线程可 release |
| **R14** | executor_worker 从"单 idx 单 tick"改为"全 slot 每轮 tick" | 每核吞吐翻倍，`ED_ENABLE=0` 基线相对旧 fake-return 加速 ≈2x，ed 相对收益被稀释 | 测试口径以"激活 executor 后 `ED_ENABLE=0`"为基线，不与旧 fake-return 对比；见 §3.2 B 警告框 |
| **R15** | `rand()` 非线程安全 | 一期 `DISPATCH_THREAD_CNT=1` 无害；多线程时 UB / 抖动 | 一期锁死 `DISPATCH_THREAD_CNT=1`；用 `rand_r(&per_thread_seed)` 与 `__thread` 局部 seed 提前铺路——见 §3.1 D |
| **R16** | count>1 仍使用逐块 `K→K+1`，但 executor 执行整任务 | 重复/漏执行 | 一期 normal 固定 `0→count`，ed 只收 count==1；M3 同步改 executor/完成/剩余块协议 |
| **R17** | `EXECUTOR_THREAD_CNT` 未定义 / `init_ctrl_t` 未链接（既有编译错误）| 未 ed 改动就编译不过 | Step 0 前置修：`conf.h` 加 `#define EXECUTOR_THREAD_CNT 1`；`init_ctrl_t` 从 `src/scheduler/dispatch.c` 迁到 `src/algorithm/dispatch.c` 或用新的 `init_algorithm_ctrl_t`；参考 §1.1 |
| **R18** | `next_block_idx == 0 && count == 0` 时 send_task 会 skip 未使用 slot | ring 环回残留 count=0 的空槽被 send_task 误处理 | `send_task` 里 `if (start >= count)` 检查 `count > 0` 前提；或调用前保证 `count >= 1`。一期 `new_task` 强制 `count >= 1`，`add_successors` 只提交已 `new_task` 的任务，可容忍；文档留作 M2 加固点 |

---

## 附录 A：需要新增/修改的文件清单

| 类别 | 文件 | 操作 |
| --- | --- | --- |
| 新增 | `include/algorithm/early_dispatch.h` | generation-tagged records、`notify_claimed`、dispatch tag、edge lock、`ed_notify_once` 与队列 API |
| 新增 | `src/algorithm/early_dispatch.c` | Hook 0/1、选核、唯一通知、edge lock/record helper 与初始化；不含 leak drain |
| 新增 | `cases/ed_a11_probe.h` | A11 探针 case（§5.4），最小 P1/P2/S 依赖，验证空转期间 S 保持 GATED；须加进 `ALL_CASES` |
| 新增 | `cases/ed_a12_ring_stress.h` | A12 ring 卷绕 stress case（§5.4），建 `>2*RING_SIZE` 任务链强制 slot 复用；须加进 `ALL_CASES` |
| 新增 | `scripts/ed_bench.sh` | 全新 |
| 新增 | `scripts/ed_bench_summary.py` | 全新（含 A1-A12 断言） |
| 修改 | `include/algorithm/conf.h` | 加 `ED_ENABLE` / `ED_UNFIN_THRESHOLD` 宏、`EXECUTOR_THREAD_CNT`（Step 0 补 pre-existing 编译错误） |
| 修改 | `include/algorithm/task.h`（或 `ring_buf.h`） | **Step 0**：补 `struct node_list` 完整定义（字段同 `scheduler/painter.h`），修 `ring_buf.h:34-35` incomplete type（§1.1 首个 error） |
| 修改 | `include/algorithm/executor.h` | 加 `_Atomic uint8_t slot_state[AIC_OSTD]` / doorbell |
| 修改 | `include/algorithm/dispatch.h` | `ctrl_t::free_bitmap` / `msg_bitmap` 声明改为 `_Atomic uint64_t`（§2.4） |
| 修改 | `include/algorithm/ring_buf.h` | 加 `init_predecessors` 声明（Step 0 修 pre-existing 隐式声明 warning）；`add_predecessors` slot 索引改 `& RING_MASK`（§1.2 一致性隐患，A12 前置） |
| 修改 | `src/algorithm/dispatch.c` | 唯一 bitmap exchange 消费、normal `0→count`、release RUNNABLE、dispatch record + Hook 0 |
| 修改 | `src/algorithm/cutter.c` | Hook 2 仅在 unfin 1→0 时调用 `ed_notify_once`；锁内 append/late-arrival；`add_successors` 索引 `& RING_MASK`（§1.2） |
| 修改 | `src/algorithm/executor.c` | acquire slot_state，全 slot 扫描；完成 EMPTY→msg release |
| 修改 | `src/main.c` | 激活 executor 线程、`init_ctrl_t()` 之后 `ed_init()`、结束时打印 ed metrics + 泄漏扫描（含 `slot_leaked`）；如果 `EXECUTOR_THREAD_CNT` 通过 conf.h 定义则不用改 main.c 里的数组声明 |
| 修改 | `Makefile` | **加 `ed_a11_probe.h` / `ed_a12_ring_stress.h` / `qwen3_dynamic_tensormap_ot1.h` 到 `ALL_CASES` 白名单（§3.5.0）**、加 `ED_ENABLE` / `ED_UNFIN_THRESHOLD` 参数、加 `early_dispatch.c` 到 `SRCS`、`ORCH_CONFIG` 加 `ed=$(ED_ENABLE) ed_thr=$(ED_UNFIN_THRESHOLD)`、扩展 `ORCH_STAMP` 依赖到所有含 `#if ED_ENABLE` 的 .o |

---

## 附录 B：与 simpler 报告的落点对照

| simpler 函数 | 本仓落点 |
| --- | --- |
| `propagate_dispatch_fanin` (`pto_scheduler.h:940-974`) | `src/algorithm/early_dispatch.c::propagate_dispatch_fanin`，由 `dispatch.c::send_task` 调用 |
| `stage_consumer_blocks` (`scheduler_dispatch.cpp:638-695`) | `src/algorithm/early_dispatch.c::try_early_dispatch` 里的 stage 段 |
| `try_speculative_early_dispatch` (`scheduler_dispatch.cpp:703-755`) | `src/algorithm/early_dispatch.c::try_early_dispatch`（drain queue），由 `dispatch.c::dispatch` 末尾调用 |
| `try_speculative_release` (`pto_scheduler.h:990-1031`) | 内联在 `resolve_dep` 的 unfin `1→0` 分支；seq_cst xchg 后经 `ed_notify_once` 竞争唯一通知，再统一 push ready_queue |
| `next_block_idx` CAS in stager (`scheduler_dispatch.cpp:661-664`) | `src/algorithm/early_dispatch.c::try_early_dispatch` 步骤 ② 的 `CAS(g_next_block_idx, 0, 1)` |
| stager 补通知 (`scheduler_dispatch.cpp:686-694`) | 步骤 ⑦ 调 `ed_notify_once`，仅 notify CAS 胜者写 |
| `next_block_idx` claim in normal dispatch | `send_task` CAS `0→count` 整任务认领 |
| `ring_one_doorbell` | 仅 `ed_notify_once` 内部写一次 |
| Gate 循环 | executor acquire 读取 `slot_state` 的 GATED/RUNNABLE 分支 |
