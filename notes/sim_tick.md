# 2026-08-24 跨类型早发调度设计（cube↔vector）

分支：`dev/early-dispatch-cpu-free`，在 esl_proxy 仿真层先验证协议逻辑，之后再搬到黄区改 kernel.h。

## 0. 换机器后先做这几件事（不然编译不过）

1. **`a6.h` 是本地文件，`.gitignore` 排除了它，git 上没有，换机器要重新建**：
   路径 `esl_proxy/include/platform/a6.h`，内容：
   ```c
   #define NPU_END_FLAG 0
   #define HAND_SHAKE_VAL 1
   #define LOAW_ADDR_MASK 2

   #define AICORE_MASK_PER_DIE 0xFFFFFFFFFFFFFFFFULL

   #define AICPU_MSGQ_BASE 0x10A000000
   #define AICPU_OFFSET 0x400000
   #define AICPU_SMT_OFFSET 0x10000
   #define AICPU_MSGQ_OFFSET 0x200
   #define AICPU_CLUSTER_OFFSET 0x400000

   #define AICORE_SPR_BASE 0x100005500
   #define AICORE_OFFSET 0x400000
   #define AICORE_CUBE_OFFSET 0x0
   #define AICORE_VECTOR_OFFSET 0x100000
   #define AICORE_SPR_OFFSET 0x10
   #define AICORE_DIE_OFFSET 0x1000000000

   #define READ_REG(dst, src)
   #define WRITE_REG(dst, src)
   ```
2. **Windows 上要把 MSYS2 加进 PATH 才有 `make`**（如果是新机器/新终端窗口）：
   ```powershell
   $env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
   ```
   （前提是那台机器装了 MSYS2；没装的话要先装，装完这两个目录应该就存在）
3. **验证环境能跑通（跑一下改动前应该有的基线结果）**：
   ```powershell
   cd esl_proxy   # 仓库最外层，Makefile_scheduler 所在目录
   make -f Makefile_scheduler clean
   make -f Makefile_scheduler run EARLY_DISPATCH_CASE_A=1 EARLY_DISPATCH_MULTI_PRED=1 SIM_LATENCY=1 SIM_TICKS=4
   ```
   看到 `[early-dispatch] ordering_pairs = N checked, 0 failed` 就说明环境没问题（`hints_published`/`plants` 的具体数字每次都会不一样，这是正常的，见 `multi_pred.md` 里的详细解释——不用因为数字对不上而怀疑环境坏了）。

## 1. 背景与动机

现有 Case A/B 只支持**同类型**早发（P、S 必须是同一种执行类型，靠共享一个核的两个槎、被动硬件仲裁实现顺序保证）。目标是扩展到**跨类型**（P、S 分属 cube/vector），此时 P、S 不再共享同一个核，被动仲裁这条路走不通，需要一种主动的"完成后通知对方"机制。

真机上的对应关系（来自跟硬件同事的沟通,尚未完全证实）：
- 下发任务：AICPU 写 SPR（跟现有一致）
- 任务完成通知：走的是 **hscb**，不是 SPR——发送和完成走的是两条不同的硬件通路
- AIC/AIV 互相通知：项目负责人确认"可以直接写对方的东西"，具体机制未知
- 跨类型同步的真实硬件候选是 **FFTS**（`CrossCoreSetFlag`/`CrossCoreWaitFlag`），但只在同一个 AICore 集群（1 AIC + 2 AIV）内confirmed 有效，不确定能否跨集群

## 2. `sim_tick` 现状回顾

### 数据结构

```c
typedef struct {
    uint32_t task_id;
    uint16_t remaining;   // 还剩几个 tick
    uint8_t occupied;     // 槽是否被占用
    uint8_t started;      // 是否已经被仲裁器选中过、真正开始跑
} sim_slot_t;

static sim_slot_t g_sim[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_sim_next[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];  // 相位指针
```

### `sim_tick` 每轮逻辑（对每个 type/core 组合）

1. 看相位指针指向哪个槽；该槽占用就跑它，否则看兄弟槽，都空则本轮跳过（指针复位到0）。
2. 第一次被选中 → 标记 `started=1`，记录 `g_sim_start`。
3. `remaining` 减到 0 → 退休：记 `g_sim_retire`，清空槽，置位 `msg_bitmap`，指针翻到兄弟槽；若翻过去发现两槽全空，指针立刻强制复位到 0（这条是 Case A 依赖的硬件性质）。

**关键不变式**：仲裁器严格非抢占、按顺序——一旦承诺跑某个槽，中途绝不会切换去看兄弟槽，直到该槽真正退休。这条不变式是本次跨类型设计正确性的根基。

## 3. 跨类型早发调度的设计思路

### 3.1 拓扑简化

真机是 1 AIC : 2 AIV。第一版先简化成 **1:1**（cube 第 i 号 ↔ vector 第 i 号算同一个"集群"），协议逻辑本身不因 1:1 还是 1:2 而改变，只是配对关系更复杂；1:1 先把协议跑通。

### 3.2 通知机制：1 bit 就够，不需要携带身份信息

结论：只要保证 **S 开始等待的那一刻，P 保证是它所在单元当前唯一可能完成的东西**，一个裸的 0/1 通知位就足够，不会有歧义——因为仲裁器非抢占，P 所在单元上不可能有别的任务抢在 P 前面完成。

### 3.3 正确性条件 = 跨类型版本的 Case A / Case B

| | 条件 | 检查方式 |
|---|---|---|
| **跨类型 Case A** | P 即将被首次发到一个完全空闲的核，S 要去的邻居单元也完全空闲 | `free_bitmap` 双方都全 1 |
| **跨类型 Case B** | P 已经在跑（`occupied=1 && started=1`），P 的兄弟槽状态无所谓；S 要去的邻居单元完全空闲 | 查 `g_sim[...P...].started` |

Case B 里"P 的兄弟槽无所谓"成立的原因：只要 P 是当前仲裁器正在服务的槽（`started=1`），兄弟槽上不管坐着什么，都不可能抢在 P 前面完成（非抢占不变式）。

**S 的落点两种情况都要求"完全空闲"，不做"排队进别人槽"的版本**——原因见下一节机会成本分析。Case B 需要每轮持续扫描（类似 `plant_pass`），不能只在 P 刚发下去那一刻查一次，否则会漏掉"P 当时还在排队、后来才轮到它开始跑"这种过渡情况。

### 3.4 机会成本分析（why S 只能落在完全空闲的核）

如果允许 S 排队进一个有别的不相关任务 X 的槽：
- X 比 P 先完成：早发调度生效，收益如预期。
- X 比 P 后完成：早发调度白做，但也没有额外损失。
- **更严重的问题**：如果 S 落在一个空闲核的槽0，同一核的槽1 后来被 ready_queue 塞进一个真正 ready 的新任务 Y——由于仲裁器"只看指针指的槽，不达标就不看兄弟槽"，只要 S 还占着槽0（哪怕只是卡在等通知、并未真正倒计时），**Y 会被完全饿死，直到 S 解除阻塞并跑完退休为止**。这比同类型 cpu-relay 当年的问题更严重，因为跨类型的等待是**无界**的（不像同类型 Case B 那样,等待时长受本核上 P 剩余 tick 数约束）。

zhangcb 的看法：这套机制设计的前提场景通常是 AICore 富余、AICPU 才是瓶颈，所以 Y 这种撞车情况实际发生概率低，收益仍然更大。这个判断成立但有前提（吞吐受限的负载下不成立，比如 qwen3 这张图自己都标注"吞吐受限 2.19 倍"），且最坏情况的代价无上限——建议后续给等待加超时兜底，本次先不做。

## 4. 具体修改点

### 4.1 第一个 commit：`sim_tick` 基础设施（纯新增，无调用点，行为不变）

改动的是 `esl_proxy/src/scheduler/dispatch.c` 里 `SIM_LATENCY && !REAL_CHIP` 那一整块（现在大概在 164-256 行附近，具体行号可能已经变了，认代码内容不认行号）。

**改动前**（现状，作为对照）：
```c
typedef struct {
    uint32_t task_id;
    uint16_t remaining;
    uint8_t occupied;
    uint8_t started;
} sim_slot_t;

static sim_slot_t g_sim[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_sim_next[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];
static uint64_t g_sim_now[DISPATCH_THREAD_CNT];

static inline void sim_place(int tid, int type, int core, int slot, uint32_t task_id)
{
    sim_slot_t *s = &g_sim[tid][type][core][slot];
    s->task_id = task_id;
    s->remaining = SIM_TICKS;
    s->occupied = 1;
    s->started = 0;
}

static void sim_tick(int tid)
{
    g_sim_now[tid]++;
    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        for (int core = 0; core < AIC_CNT_PER_THREAD; core++) {
            sim_slot_t *s = g_sim[tid][type][core];
            uint8_t n = g_sim_next[tid][type][core];
            int run = s[n].occupied ? n : (s[n ^ 1].occupied ? (n ^ 1) : -1);
            if (run < 0) {
                g_sim_next[tid][type][core] = 0;
                continue;
            }
            if (!s[run].started) {
                s[run].started = 1;
                g_sim_start[s[run].task_id] = g_sim_now[tid];
            }
            if (--s[run].remaining == 0) {
                g_sim_retire[s[run].task_id] = g_sim_now[tid];
                s[run].occupied = 0;
                s[run].started = 0;
                g_ctrl_t[tid].msg_bitmap[type][run] |= (uint64_t)0x1 << core;
                g_sim_next[tid][type][core] = run ^ 1;
                if (!s[0].occupied && !s[1].occupied) {
                    g_sim_next[tid][type][core] = 0;
                }
            }
        }
    }
}
```

**改动后**（草稿，1:1 配对：cube 第 `core` 号 ↔ vector 第 `core` 号，`EXE_TYPE_CNT==2` 时用 `type ^ 1` 直接算出对方类型）：

```c
typedef struct {
    uint32_t task_id;
    uint16_t remaining;
    uint8_t occupied;
    uint8_t started;
    uint8_t waiting_notify;   // 新增：1 = 占着槽但先别跑，等跨类型的通知
} sim_slot_t;

static sim_slot_t g_sim[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT][AIC_OSTD];
static uint8_t g_sim_next[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];
static uint64_t g_sim_now[DISPATCH_THREAD_CNT];

// 新增：跨类型完成通知。[type][core] 置1 = "type类型的core号核刚完成，通知配对的另一种类型"
// 1:1 简化：cube 的 core 号就是它配对的 vector 的 core 号，type^1 直接算出对方类型（EXE_TYPE_CNT==2 时成立）
static uint8_t g_cross_notify[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT];

// needs_notify: 是否需要先卡住等跨类型通知。现有调用点全部传 false，行为不变。
static inline void sim_place(int tid, int type, int core, int slot, uint32_t task_id, bool needs_notify)
{
    sim_slot_t *s = &g_sim[tid][type][core][slot];
    s->task_id = task_id;
    s->remaining = SIM_TICKS;
    s->occupied = 1;
    s->started = 0;
    s->waiting_notify = needs_notify ? 1 : 0;
}

static void sim_tick(int tid)
{
    g_sim_now[tid]++;
    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        for (int core = 0; core < AIC_CNT_PER_THREAD; core++) {
            sim_slot_t *s = g_sim[tid][type][core];
            uint8_t n = g_sim_next[tid][type][core];
            int run = s[n].occupied ? n : (s[n ^ 1].occupied ? (n ^ 1) : -1);
            if (run < 0) {
                g_sim_next[tid][type][core] = 0;
                continue;
            }

            // 新增插入点①：卡在等通知，就别倒计时，也别标 started
            if (s[run].waiting_notify) {
                if (g_cross_notify[tid][type ^ 1][core]) {
                    g_cross_notify[tid][type ^ 1][core] = 0;   // 消费掉通知
                    s[run].waiting_notify = 0;                  // 解除阻塞，下一轮开始正常跑
                }
                continue;   // 这一轮不倒计时（不管是刚解除还是还在等）
            }

            if (!s[run].started) {
                s[run].started = 1;
                g_sim_start[s[run].task_id] = g_sim_now[tid];
            }
            if (--s[run].remaining == 0) {
                g_sim_retire[s[run].task_id] = g_sim_now[tid];
                s[run].occupied = 0;
                s[run].started = 0;
                g_ctrl_t[tid].msg_bitmap[type][run] |= (uint64_t)0x1 << core;

                // 新增插入点②：退休时，如果有跨类型的人在等自己，顺手通知
                // g_cross_hint 是第二个 commit 才加的数组，这里先占位，第一个 commit 里这段判断恒为假
                if (g_cross_hint[s[run].task_id] != EARLY_NONE) {
                    g_cross_notify[tid][type][core] = 1;
                }

                g_sim_next[tid][type][core] = run ^ 1;
                if (!s[0].occupied && !s[1].occupied) {
                    g_sim_next[tid][type][core] = 0;
                }
            }
        }
    }
}
```

**这个 commit 单独提交时**，`g_cross_hint` 还不存在（第二个 commit 才加），所以第一次提交这一版时，插入点②那一段先不加，或者先注释掉/用 `#if 0` 包起来——**保证第一个 commit 里 `sim_place` 的调用点全部改成传 `false`、`waiting_notify` 永远是 0，行为跟改动前完全一致**，可以先单独验证没有回归，再做第二个 commit 把 `g_cross_hint` 和插入点②真正接上。

`sim_place` 加了参数之后，记得把所有现有调用点（`place_task` 里那一处）也加上 `, false`，否则编译不过。

### 4.2 第二个 commit：painter/dispatch 的跨类型 hint 逻辑

- **`g_cross_hint[RING_SIZE]`**（`early_dispatch.h` + `painter.c`）：独立数组，跟现有 `g_early_hint` 分开存、分开处理，不复用/不改动 multi_pred 已经写好并验证过的代码。`init` 时也要跟 `g_early_hint` 一样,全部置 `EARLY_NONE`。
- **painter 侧**（`painter.c` 的 `early_publish_hint`）：复用现有 indegree==1 触发时机（`resolve_dep`/`add_successors` 调用点不用改）。在“找到存活者 P”之后，原来直接判断是不是同类型然后 `return`；现在要改成：
  ```c
  if (g_early_type[survivor] == g_early_type[s]) {
      g_early_hint[survivor] = s;        // 同类型，走现有 Case A/B
  } else {
      g_cross_hint[survivor] = s;        // 跨类型，走新机制
  }
  ```
  具体“找存活者”那段循环逻辑（walk `g_early_st_flat` 找第一个未完成的）**要不要连跨类型前驱也一起收进候选列表**，这是 multi_pred 那份笔记里也讨论过的分岔点——**这次先不去改 multi_pred 现成的同类型专用结构**，跨类型这条新逻辑用一份独立的、覆盖全部前驱（不分类型）的新 CSR 结构去找存活者，两份结构并存,不共用。
- **dispatch 侧**（`dispatch.c`）：这是全新的检查，不是改 `send_task`/`plant_pass` 里现有的 Case A/B 代码,是在旁边加两段新逻辑，结构上模仿现有 Case A/B 但检查条件按 3.3 节的表来:
  - 跨类型 Case A：模仿 `send_task` 里 `EARLY_DISPATCH_CASE_A` 那段的位置（P 刚要被 `place_task` 放下去之前），但判断条件换成"P 目标核全空闲 **且** S 要去的邻居单元也全空闲"，命中就对 P 正常 `place_task`，对 S 调 `sim_place(..., needs_notify=true)`。
  - 跨类型 Case B：模仿 `plant_pass` 的结构（每轮持续扫描,不是一次性检查），但扫描条件换成"某个槽 `occupied==1 && started==1`"（不是现有 Case B 用的 `free_bitmap` 异或），查这个任务有没有 `g_cross_hint`，有且邻居单元全空闲就对 S 调 `sim_place(..., needs_notify=true)`。
- **开关**：新增 `EARLY_DISPATCH_CROSS_TYPE`（`Makefile_scheduler`，仿照 `EARLY_DISPATCH_CASE_A`/`EARLY_DISPATCH_MULTI_PRED` 那两段 `ifeq` 的写法），跟它们正交、默认关闭。

## 5. 待办 / 未决问题

- [ ] hscb 具体是什么机制、覆盖范围（同集群内还是能跨集群）——需要黄区代码或硬件同事确认
- [ ] 无界等待的超时兜底机制——本次先不做，记在这里
- [ ] 1:1 简化后续要不要改成真实的 1:2 拓扑
- [ ] 小型专用 DAG 验证跨类型路径（同 multi_pred 一样，qwen3 图不一定能覆盖到这条新路径）
