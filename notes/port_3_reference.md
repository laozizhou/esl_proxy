# 阶段二附录：原型仓参考实现

> **这不是要照抄的代码。** 这些实现踩在原型仓自己的数据结构上（`free_bitmap[type][slot]`
> 位图、`task_id_map1/2`、单一 `send_task` 等），目标仓已知与之分叉。它的用途是：对照
> `port_2_invariants.md`，看每一条不变量在一个能跑通的实现里**具体长什么样**。
>
> 每段代码后面标注了它承载哪几条不变量。如果目标仓的形状不同，要保住的是**不变量**，不是
> 这里的代码。
>
> 说明：原型仓实际用了三个编译开关，下面已按目标仓的决定合并成单一开关
> `EARLY_DISPATCH_CPU_FREE`。

---

## 一、可以基本照抄的部分

这三块只依赖任务图和任务完成状态，跟槽位模型、下发路径完全无关，目标仓改动再大也影响不到，
适配成本最低。

### 1.1 头文件

```c
#ifndef SCHEDULER_EARLY_DISPATCH_H
#define SCHEDULER_EARLY_DISPATCH_H

#include <stdint.h>
#include "scheduler/conf.h"

#ifdef EARLY_DISPATCH_CPU_FREE

#define EARLY_NONE 0xFFFFFFFFu

/* 同类型前驱，CSR 存储：任务 id 的那一段是
 * g_early_st_flat[g_early_st_idx[id] .. + g_early_st_cnt[id]) */
extern uint32_t g_early_st_cnt[RING_SIZE];
extern uint32_t g_early_st_idx[RING_SIZE];
extern uint32_t *g_early_st_flat;

/* 跨类型前驱，布局相同。每条边必然落进两者之一，不重不漏。 */
extern uint32_t g_early_ct_cnt[RING_SIZE];
extern uint32_t g_early_ct_idx[RING_SIZE];
extern uint32_t *g_early_ct_flat;

/* 按【前驱】索引（不变量 1） */
extern uint32_t g_early_hint[RING_SIZE];
extern uint32_t g_cross_hint[RING_SIZE];

extern uint8_t  g_early_type[RING_SIZE];

/* 恰好下发一次的唯一卡点（不变量 10） */
extern uint8_t  g_early_dispatched[RING_SIZE];

extern uint32_t g_early_hints_published;
extern uint32_t g_cross_hints_published;
extern uint32_t g_early_plants;
extern uint32_t g_cross_plants;
extern uint32_t g_early_skipped_dup;

void early_dispatch_init(void);

#endif /* EARLY_DISPATCH_CPU_FREE */
#endif
```

`#ifndef ... #define ... #endif` 是标准的头文件保护，防止同一个 `.c` 通过不同路径重复包含
时声明重复。宏名按目标仓的命名习惯改。

**原型头文件顶部还有一段所有权说明，是整套无锁设计唯一被写下来的地方**（承载不变量 10c），
移植时这段假设必须重新验证：

```
 *   g_early_st_*        启动时建好，之后只读
 *   g_early_hint[]      由依赖解算侧发布，由下发侧消费（并清除）
 *   g_early_dispatched[] 只由下发侧在它唯一的下发点读写
 *
 * g_early_hint 是唯一的跨线程字段。它存的是一个普通 task id 或哨兵值，所以发布/消费撞车
 * 最多只会丢失或重复一条 hint，不会损坏它。丢了无所谓——早发本来就是机会性的，就绪队列是
 * 兜底；重复了会被 g_early_dispatched 挡住。这两个数组是单写者，**前提是所有任务都流经
 * 同一个下发线程**——而这只在「就绪队列的目标下发上下文被硬编码成 0」时成立。真正启用多
 * die / 多下发上下文之前必须重新设计。
```

### 1.2 静态分类：`early_dispatch_init()`

承载 **不变量 3**。

```c
void early_dispatch_init(void)
{
    for (uint32_t i = 0; i < RING_SIZE; i++) {
        g_early_hint[i] = EARLY_NONE;
        g_cross_hint[i] = EARLY_NONE;
    }

    /* 第一遍：填类型表。必须先做完，第二遍才能判断同类型/跨类型——顺序不能颠倒。 */
    for (int sg = 0; sg < PAINTER_THREAD_CNT; sg++) {
        for (uint32_t i = 0; i < test_graph[sg].task_cnt; i++) {
            g_early_type[test_graph[sg].task_id[i]] = (uint8_t)test_graph[sg].type[i];
        }
    }

    /* 两个 flat 数组各按「全图总边数」分配，这是安全上界：每条边只进其中一个。 */
    uint32_t total_edges = 0;
    for (int sg = 0; sg < PAINTER_THREAD_CNT; sg++) {
        for (uint32_t i = 0; i < test_graph[sg].task_cnt; i++) {
            total_edges += (uint32_t)test_graph[sg].pre_cnt[i];
        }
    }
    g_early_st_flat = malloc(sizeof(uint32_t) * total_edges);
    g_early_ct_flat = malloc(sizeof(uint32_t) * total_edges);

    /* 第二遍：一次遍历同时建好两份 CSR，两个游标各自独立推进。 */
    uint32_t st_cursor = 0, ct_cursor = 0;
    for (int sg = 0; sg < PAINTER_THREAD_CNT; sg++) {
        for (uint32_t i = 0; i < test_graph[sg].task_cnt; i++) {
            uint32_t id = test_graph[sg].task_id[i];
            uint32_t st_start = st_cursor, ct_start = ct_cursor;
            for (int k = 0; k < test_graph[sg].pre_cnt[i]; k++) {
                uint32_t q = (uint32_t)test_graph[sg].predecessors[test_graph[sg].pre_idx[i] + k];
                if (g_early_type[q] == g_early_type[id]) {
                    g_early_st_flat[st_cursor++] = q;
                } else {
                    g_early_ct_flat[ct_cursor++] = q;
                }
            }
            g_early_st_idx[id] = st_start;
            g_early_st_cnt[id] = st_cursor - st_start;
            g_early_ct_idx[id] = ct_start;
            g_early_ct_cnt[id] = ct_cursor - ct_start;
        }
    }
}
```

**适配点**：`test_graph` 的字段名（勘察 E1）；`malloc` 能否使用（勘察 G3）——不能的话改成
定长静态数组，加一句越界保护即可，不影响任何调度逻辑。

### 1.3 发布 hint：`early_publish_hint()`

承载 **不变量 4**（同类型优先、互斥、`return` 而不是两个都发）。

```c
static inline void early_publish_hint(int tid, uint32_t s)
{
    uint32_t cnt  = g_early_st_cnt[s];
    uint32_t base = g_early_st_idx[s];
    for (uint32_t k = 0; k < cnt; k++) {
        uint32_t p = g_early_st_flat[base + k];
        if (g_state_buf[tid][p].state != TASK_STATUS_COMPLETED) {
            g_early_hint[p] = s;
            g_early_hints_published++;
            WORKER_LOGF("early,hint,successor,%u,predecessor,%u", s, p);
            return;                     /* 互斥：走不到下面的跨类型循环 */
        }
    }

    uint32_t ct_cnt  = g_early_ct_cnt[s];
    uint32_t ct_base = g_early_ct_idx[s];
    for (uint32_t k = 0; k < ct_cnt; k++) {
        uint32_t p = g_early_ct_flat[ct_base + k];
        if (g_state_buf[tid][p].state != TASK_STATUS_COMPLETED) {
            g_cross_hint[p] = s;
            g_cross_hints_published++;
            WORKER_LOGF("early,cross_hint,successor,%u,predecessor,%u", s, p);
            return;
        }
    }
}
```

**为什么不需要任何计数器**：调用方保证 S 的未完成前驱总数是 1，所以任一子集里未完成的最多
1 个——现场遍历找到第一个未完成的就是答案。

**适配点**：任务完成状态怎么查（勘察 E2）。

### 1.4 两个发布点

承载 **不变量 2**。

**发布点甲**——入度递减到 1 的地方（原型在 `resolve_dep()` 里）：

```c
    g_predecessor_cnt[succ_id & RING_MASK]--;
    if (g_predecessor_cnt[succ_id & RING_MASK] == 1) {
        early_publish_hint(tid, succ_id);          /* 刚变成「准就绪」 */
    }
    if (g_predecessor_cnt[succ_id & RING_MASK] < 1) {
        /* ...原有的入就绪队列逻辑... */
    }
```

（`== 1` 和 `< 1` 互斥，谁先谁后、用不用 `else if` 都不影响正确性——真正必须是「并列的 `if`
而不是 `else`」的地方在 2.3 的两个 hint 检查，见不变量 13。）

**发布点乙**——任务提交时初始入度就是 1 的地方（原型在 `add_successors()` 里）：

```c
    g_predecessor_cnt[id] = predecessor_cnt;
    if (predecessor_cnt <= 0) {
        /* ...原有的直接入就绪队列逻辑... */
    }
    else if (predecessor_cnt == 1) {
        /* 提交时入度就是 1，永远不会「跳变到 1」，这是唯一的发布机会 */
        early_publish_hint(tid, id);
    }
```

**注意 `predecessor_cnt` 的口径**：原型这里算的是「有几个前驱**还没完成**」（运行时活跃入
度），不是图里的静态前驱个数——静态个数为 0 的情况在更早的地方已经提前返回了。挂到目标仓时
如果错挂成静态个数，不会报错，只会静默丢掉纯链式那部分覆盖率。

**适配点**：勘察 E4 / E6。这是**最容易漏掉**的一处——只挂发布点甲的话，纯链式任务全部拿不到
早发机会，而且不会有任何报错，只是收益凭空少一大块。

---

## 二、必须按目标仓形状重新落地的部分

### 2.1 单一放置点 + 恰好下发一次

承载 **不变量 10**。原型把原本内联在下发函数里的写槽代码抽成了一个函数，好处是早发路径和
正常路径共用同一个出口，`g_early_dispatched` 只需要在一个地方置位。

```c
static inline void place_task(ctrl_t *ctrl, int type, int core, int slot, uint32_t task_id,
                              bool needs_notify)
{
    uint64_t mask = (uint64_t)0x1 << core;

    if (slot == 1) {
        ctrl->task_id_map2[type][core] = task_id;
        *ctrl->aicore_spr_2[type][core] = task_id;   /* needs_notify 要编码进这里，见下 */
    } else {
        ctrl->task_id_map1[type][core] = task_id;
        *ctrl->aicore_spr_1[type][core] = task_id;
    }

    ctrl->free_bitmap[type][slot] &= ~mask;
    g_early_dispatched[task_id] = 1;                 /* ← 不变量 10 的唯一置位点 */
}
```

`needs_notify == true` 表示「这是跨类型早发的后继，它必须在计算单元侧先等配对单元的通知」。
这个标记怎么随任务写进下发字，取决于下发字里还有哪些空闲位——**属于人工 TODO，不要猜**。

**★ 适配重点**：目标仓已知把 slot 的下发拆开了（勘察 A3），可能存在**多个**放置点。
**每一个都必须置位 `g_early_dispatched`**，漏掉任何一个，那条路径就是不变量 10 的缺口。
最稳妥的做法是先把它们收敛成一个共用出口，再加这一行。

### 2.2 正常下发路径的重复拦截

承载 **不变量 10** 的另一半。

```c
    /* 候选核 = 任一槽空（不变量 16）。取任务的上界必须跟这个条件配套：
     * 下面的循环放完一个任务就把整个核从位图里去掉，所以上界是「或」之后的 popcount，
     * 不是两个 popcount 相加——多要的话会拿到放不下的任务，然后对空位图做位扫描，未定义。 */
    uint64_t free_bitmap = ctrl->free_bitmap[type][0] | ctrl->free_bitmap[type][1];
    int cnt = __builtin_popcountll(free_bitmap);
    /* ...batch_dequeue(ready_queue, task_ids, &cnt)... */

    for (int i = 0; i < cnt; i++) {
        uint32_t task_id = task_ids[i];

        if (g_early_dispatched[task_id]) {
            g_early_skipped_dup++;
            WORKER_LOGF("early,skip_dup,task_id,%u,type,%d", task_id, type);
            continue;               /* 注意：不消耗这个核的名额 */
        }

        uint64_t idx  = (uint64_t)__builtin_ctzll(free_bitmap);
        uint64_t mask = (uint64_t)0x1 << idx;
        /* ★⚠ 优先低编号槽。原型注释明写这是 load-bearing 而非风格问题：空闲核上硬件先看
         * 低槽，软件按这个顺序填，排在驻留任务后面的任务才会落在高槽。这是不变量 7 的软件
         * 半边——另一半是硬件的仲裁行为，未验证。 */
        int slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
        int core = (int)idx;

        place_task(ctrl, type, core, slot, task_id, false);
        sent++;
        free_bitmap &= ~mask;
    }
```

**适配重点**：
- 目标仓如果有多条正常下发路径，**每一条**都要有这个重复拦截。
- 「候选核的条件」和「一轮能取多少任务的上界」是绑死的一对，改一个就要改另一个
  （不变量 16 的配套要求）。
- 那条「优先低编号槽」的规则不是排版偏好，它和硬件的取任务顺序必须一致，见不变量 7。

### 2.3 早发扫描：`plant_pass()`

这是主体，承载 **不变量 6、7、8、11、12、13**。

```c
static int plant_pass(int tid)
{
    ctrl_t *ctrl = &g_ctrl_t[tid];
    int planted = 0;

    for (int type = 0; type < EXE_TYPE_CNT; type++) {
        /* 异或 = 两个槽恰好一忙一闲 = 这个核唯一活跃（不变量 6）。
         * 只用占用位图，不碰任何任务内部状态（不变量 5）。 */
        uint64_t cand = ctrl->free_bitmap[type][0] ^ ctrl->free_bitmap[type][1];
        while (cand) {
            uint64_t idx  = (uint64_t)__builtin_ctzll(cand);
            uint64_t mask = (uint64_t)0x1 << idx;
            cand &= cand - 1;                       /* 清掉最低位的 1，取下一个候选核 */

            int free_slot = (ctrl->free_bitmap[type][0] & mask) != 0 ? 0 : 1;
            uint32_t p = free_slot == 1 ? ctrl->task_id_map1[type][idx]
                                        : ctrl->task_id_map2[type][idx];

            /* ---- 同类型（不变量 7、11）---- */
            uint32_t s = g_early_hint[p];
            if (s != EARLY_NONE) {
                g_early_hint[p] = EARLY_NONE;       /* 无条件消费：不变量 11 */
                if (g_early_dispatched[s]) {
                    g_early_skipped_dup++;
                } else {
                    place_task(ctrl, type, (int)idx, free_slot, s, false);
                    g_early_plants++;
                    planted++;
                    WORKER_LOGF("early,plant,successor,%u,predecessor,%u,core,%d,slot,%d",
                                s, p, (int)idx, free_slot);
                }
            }

            /* ---- 跨类型（不变量 8、9、12）；并列的 if，不是 else：不变量 13 ---- */
            uint32_t cs = g_cross_hint[p];
            if (cs != EARLY_NONE) {
                if (g_early_dispatched[cs]) {
                    g_cross_hint[p] = EARLY_NONE;   /* 已过期，消费 */
                    g_early_skipped_dup++;
                } else {
                    int ctype = type ^ 1;                       /* 不变量 9 的配对假设 */
                    bool dest_idle = (ctrl->free_bitmap[ctype][0] & mask) != 0
                                   && (ctrl->free_bitmap[ctype][1] & mask) != 0;
                    if (dest_idle) {                            /* 不变量 8：必须完全空闲 */
                        g_cross_hint[p] = EARLY_NONE;           /* 成功，消费 */
                        place_task(ctrl, ctype, (int)idx, 0, cs, true);
                        g_cross_plants++;
                        planted++;
                        WORKER_LOGF("early,cross_plant,successor,%u,predecessor,%u,core,%d,"
                                    "type,%d", cs, p, (int)idx, ctype);
                    }
                    /* 目标忙 → 什么都不做，hint 留着下一轮重试：不变量 12 */
                }
            }
        }
    }
    return planted;
}
```

**★ 适配重点，按重要性排序**：

1. **「唯一活跃」怎么判**（不变量 6）——原型的 `free_bitmap[0] ^ free_bitmap[1]` 强依赖
   「一个核两个槽、状态存在同一组位图里」。目标仓拆开之后要找到等价表达。这是全篇最关键的
   一处适配。
2. **从位置反查 task id**（勘察 B1）——原型靠 `task_id_map1/2`。
3. **★⚠ 同类型 S 放哪**（不变量 7）——注意原型**不是**「放到 P 后面那个槽」，而是「放到当时
   恰好空着的那个槽」，那可能是比 P **编号更低**的槽。这在原型上安全只因为模拟器的仲裁器会在
   核变空的那一刻把选择权复位到低槽；换一种同样合理的仲裁器（永远低编号优先），**S 就会先于
   P 执行**。这是全篇风险最高、且真机上从未验证过的一条，务必先读不变量 7 的实测方案。
   原表述保留供对照：目标仓的槽位语义如果不是「排队」，这一条
   要重新论证。
4. **跨类型配对**（不变量 9）——`type ^ 1` 和同编号是原型的假设，勘察 F2 确认。
5. **位图遍历技巧**——`__builtin_ctzll` 取最低位的 1、`cand &= cand - 1` 清掉它，是标准的
   「遍历位图中所有置位」写法（勘察 G5 确认可用）。如果目标仓的空闲状态不是位图，这套写法
   整个不适用，换成对应的遍历方式即可。

### 2.4 调用顺序

承载 **不变量 14、15**。

```c
    read_msgq(tid);                                   /* 读完成信号 */
    push_2_completed_queue(tid);                      /* 翻译成 task id 并送走 —— 不变量 15 */
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_MIX);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_VECTOR);
    total_sent += send_task(&g_ctrl_t[tid], TASK_TYPE_CUBE);
    plant_pass(tid);                                  /* 全部正常下发之后 —— 不变量 14 */
```

**适配重点**：勘察 D1/D2。目标仓一轮里的下发调用可能不止三次、也可能不是按类型划分的——
不变量 14 要求的是「在**全部**正常下发之后」，不是「在第三次调用之后」。

### 2.5 停用跨类型 AND 空闲状态的操作

承载 **不变量 17**。原型仓里对应的是一个 `set_mix()` 函数，移植过程中停用了对它的调用（函数
本体保留，只是不再被调用）。

**适配重点**：勘察 H2。目标仓如果有等价物且被调用，必须停用。

---

## 三、原型仓里存在但**不要**移植的东西

| 东西 | 为什么不移植 |
|---|---|
| `sim_tick` / `sim_place` / `sim_slot_t` / `g_sim*` | 大部分是在模拟硬件本来就有的行为（槽数组、执行倒计时），真机上这些就是硬件。**但其中的仲裁相位指针是个例外**——它模拟的是「一个单元上多个任务谁先执行」，原设计文档把它列为**未验证的硬件假设**，不能当成免费前提，见不变量 7 |
| `g_cross_notify` / `waiting_notify` | 模拟器里的通知邮箱。真机上对应的是 kernel.h 侧的实现，由人工完成 |
| `SIM_LATENCY` / `SIM_TICKS` / 相关条件编译 | 模拟器专用开关 |
| `early_dispatch_report()` 及其 `printf` | 真机内核代码一般不该有 printf；调试用目标仓自己的机制 |
| `g_early_pair` / `early_record_plant()` | 顺序断言工具，依赖模拟器才有的精确时间戳，搬不了 |

计数器（`g_early_plants` / `g_cross_plants` / `g_early_skipped_dup` /
`g_early_hints_published` / `g_cross_hints_published`）和 `WORKER_LOGF` 调用**要保留**——
出问题时这是唯一能看出「早发到底有没有生效、生效了多少次」的东西。
