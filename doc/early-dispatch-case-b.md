# 早发调度 — Case B

把后继任务预先排进"前驱正在执行"的那个核的兄弟槽,使前驱一退休后继就立即启动,省掉
AICore → AICPU → AICore 这一整趟完成通知的往返。

范围:仅 `esl_proxy/src/scheduler/*`(`Makefile_scheduler` 构建的独立调度器)。algorithm 树不动。

## 1. 问题

后继当前必须等完整往返才能启动:

```
AICore 退休 P → 置 MSGQ_VLD 位 → dispatch 下一轮 read_msgq()
→ push_2_completed_queue() → painter 出队、减入度、投进 ready_queue
→ dispatch 下一轮 send_task() → AICore 启动 S
```

跨两个线程循环、至少两个 dispatch 轮次。而每个 AI Core 有 `AIC_OSTD = 2` 个槽,后继完全可以
提前排进兄弟槽,由核本地接续。

## 2. 机制

`S` = 后继,`P` = 它**唯一剩余的未完成前驱**。

```
核 c: slot0 = Q(执行中), slot1 = 空
  1. send_task 把 P 放进 slot1        (P 排在 Q 后面; 需要双 outstanding)
  2. Q 退休, slot0 空出, P 开始执行
  3. dispatch 观察到: 核 c 恰好一个槽忙, 占用者是 P
  4. 把 S 放进空出的兄弟槽
  5. P 退休 → 兄弟槽只有 S 一个候选 → 任何非抢占仲裁都会选中 S
```

第 1 步是**双 outstanding 成为前置条件**的原因:现在 `free = free_bitmap[0] & free_bitmap[1]`
要求两槽全空才选核,于是 P 永远不会被排在别的任务后面,第 3 步永不出现。

第 5 步的正确性**只依赖 AICore 非抢占**:P 退休时兄弟槽里恰好只有一个候选,任何仲裁规则都无法
把顺序调反。这正是 Case B 比 Case A 便宜的地方——Case A 还额外需要"空闲核先看 slot0"。

残留窗口:硬件释放 Q 的槽到 P 真正启动之间,两个槽可能短暂都看起来有效。在目标硬件模型下
(相位指针空闲复位为 0、之后交替)指针此刻已指向 P 所在槽,顺序成立;在"低编号优先"仲裁下,
若 S 落在更低的槽且 P 尚未启动,则不成立。见 §8。

## 3. 链式与嵌套(A ← B ← C ← D)

**双槽天然支持任意长的链,一次推进一环。** 设执行顺序 D → C → B → A:

```
D(slot0) 执行中, C(slot1) 已预排
D 退休 → C 在 slot1 启动, slot0 空出
        → B 唯一剩余前驱是 C, C 正在执行 → Case B 再次成立 → 把 B 排进 slot0
C 退休 → B 在 slot0 启动, slot1 空出 → 把 A 排进 slot1
...
```

整条链在**一个核上跑完,链内零往返**。每一环的正确性都成立,因为每次预排时兄弟槽里都只有一个
候选。相位交替恰好与"槽轮换"同步:退休 slot1 后指针指向 slot0,而那正是新预排的位置。

**只能领先一环。** `D` 执行、`C` 已排时两槽已满,`B` 无处可放,必须等 `D` 退休。这是
`AIC_OSTD = 2` 的硬限制,不是软件选择。

**链暴露了一个必须处理的缺口**:纯链上的每个任务只有一个前驱,所以它的入度**从初始化起就是 1**,
永远不会发生"降到 1"的跳变。因此提示的发布点必须有两处:

| 位置 | 触发条件 |
|---|---|
| `resolve_dep()` 递减点 | 入度 **跳变**到 1 |
| `add_successors()` 提交点 | 入度**初始化**即为 1 |

只挂递减点会静默漏掉全部纯链。本 DAG 上恰好不暴露这个问题(见 §10),但机制必须两处都挂。

## 4. 职责划分

| | painter | dispatch |
|---|---|---|
| 掌握 | 活入度、前驱列表、完成状态 | 核/槽占用、`task_id_map`、自己发过什么 |
| 负责 | 检测 almost-ready、识别 P、发布提示 | 消费提示、放置 S、去重 |

所有依赖策略留在 painter,所有槽位机制留在 dispatch。将来任何策略变更(阈值 K > 1、临界路径
优先、按时长加权)只改 painter。

**提示按 P 键入,不按 S。** dispatch 看某个核时已经握有 `(type, core, slot)`,兄弟槽就是
`slot ^ 1`,于是**不需要 `task_id → 位置` 的反查表**,也不存在陈旧映射的风险。

**不保证送达,也不需要保证。** 放不下就丢弃提示(P 已退休、兄弟槽被占、提示丢失),S 仍会通过
正常 `ready_queue` 路径到达。无重试、无超时、无不可撤销的抑制——见 §5。

## 5. 去重

被预排的 S later 还会因入度归零而进入 `ready_queue`。没有保护就会执行两次,而后果比"重复计算"
严重:

- `resolve_dep()` 会把 S 的后继列表走两遍,每个后继入度**双减**。有两个活前驱的后继会在另一个
  前驱还在执行时被放行——静默的顺序错误,正是本特性要防的事。
- `g_predecessor_cnt` 是 `uint32_t` 且判据是 `< 1`,第三次递减会下溢成约 4e9,那个后继**永不放行**
  → 无看门狗的无限自旋。
- `completed_task_cnt` 是朴素累加,重复计数会让 `g_is_done` 提前触发,运行"成功"退出。

保护:`g_dispatched[task_id]`,在 **dispatch 的唯一 send 点**检查并置位,同时覆盖早发和正常两条
路径。单写者(见 §7 第 6 条),无需原子操作。

**故意不在 painter 侧拦。** 在 painter 的 ready 门上抑制 S,会让这个标记变成"从唯一还能救回 S 的
路径上不可撤销地移除"——一旦软件预排成功而硬件没执行,S 永久丢失且无声死循环。放在 send 点跳过,
则 `ready_queue` 始终是活的兜底,不需要看门狗。

## 6. 改动清单

| 文件 | 改动 |
|---|---|
| `src/scheduler/dispatch.c` | `send_task`:`free_bitmap[t][0] \| free_bitmap[t][1]`(双 outstanding);容量 `popcount(fb[0]) + popcount(fb[1])`;保留 slot0 优先 |
| | 新增 `plant_pass()`,在 `dispatch()` 中于 `read_msgq()` **之后**、`push_2_completed_queue()` **之前**调用 |
| | send 点的 `g_dispatched[]` 检查与置位 |
| | 延迟模型(§9)替换假完成 |
| `src/scheduler/painter.c` | 递减点 + 提交点两处发布 `g_hint[P] = S`,带同类型过滤 |
| `include/scheduler/dispatch.h` | `ctrl_t` 上的计数器 |
| `Makefile_scheduler` | `-DEARLY_DISPATCH` / `-DSIM_LATENCY` |

`plant_pass()` 的位置是**承重的**:在 `read_msgq()` 之后,`free_bitmap` 才反映本轮完成;在
`push_2_completed_queue()` 之前,预排才一定 happens-before 本轮观察到的任何完成的入队。

## 7. 不变式

1. **每个任务恰好派发一次** —— send 点的 `g_dispatched[]`。
2. **有依赖边的两个任务永不并发执行** —— 同 exe_type、同核、兄弟槽、非抢占按序仲裁。
3. **P 与 S 同 exe_type。** 槽按 `(exe_type, core)` 分组,cube/vector 是独立单元,跨类型的槽位
   放置**不产生任何顺序约束**。跨类型配对在提示发布时就被拒绝。
4. **预排是机会性的。** 拒绝永远正确,`ready_queue` 是兜底。
5. **commit 先于 dispatch。** painter 只能在 `add_successors()` 提交过 S 之后才可能检测到它,
   原有不变式保持。
6. **单 die / 单 dispatch 线程。** `send_2_ready_queue()` 硬编码 `target_ctrl = 0`,所有任务经
   dispatch 线程 0,它拥有 die 0 的核 0..31。故 `g_dispatched[]` 和 `g_hint[]` 只有一个写者,
   init 处断言。**若将来真正启用多 die,这两个数组变成共享、核查找会跨线程,必须重新设计。**
7. **`task_id < RING_SIZE`。** id 为 0..863,`RING_SIZE = 2048`,且不复用,故 `g_dispatched[]`
   无需清零。断言保证。

## 8. 硬件假设

| 假设 | 可信度 | 若不成立 |
|---|---|---|
| AICore 不抢占正在执行的 kernel | 高 —— 抢占需保存 L0/L1/UB | S 可能与 P 并发 |
| P 退休时,兄弟槽的占用者被接续取走 | 高 —— 它是唯一候选 | S 饿死或乱序 |
| §2 的窄窗口不会反序 | **未验证** | S 先于 P 执行,静默错数据 |

第三条可测,上真机前应先测:从空闲核写 P→slot0、S→slot1,让 S 校验 P 写下的哨兵值,重复统计失败
率。0 说明空闲态 slot0 优先;接近 100% 说明相位指针没有复位。

`include/platform/a6.h` 里 `READ_REG`/`WRITE_REG` 是空宏、`MSGQ_VLD0..3` 未定义,所以 `REAL_CHIP`
路径当前读不到任何东西,上硬件前必须先实现。

## 9. 验证

默认的假完成构建**无法**触发 Case B:`send_task()` 在派发的同一轮就置 `msg_bitmap`,P 驻留不足
一轮,第 3 步永不发生。**延迟模型因此是前置条件,不是便利设施。**

模型(在 `dispatch.c` 内,`SIM_LATENCY` 开关):每个 `(exe_type, core)` 一个跨双槽的按序仲裁器——
取相位指针所指的槽,若空则取兄弟槽,递减其 tick,归零才置 `msg_bitmap[type][slot]`,然后翻转相位;
两槽皆空时相位复位为 0。在 `dispatch()` 顶部、`read_msgq()` 之前**由所属 dispatch 线程同步调用**
—— `msg_bitmap` 是普通 `uint64_t`,`get_completed()` 用非原子读改写清位,独立的模拟线程会静默丢
完成。

检查项:

1. `completed_task_cnt == 864` **恰好**。偏多=重复派发,偏少=丢任务。
2. `plants_ok > 0` —— 证明路径**真的执行过**而非静默回落。报告 0 次预排的运行等于没测特性。
3. 每个被预排的 `(P, S)`:`start_tick(S) >= retire_tick(P)`。这是顺序判据,在模拟器里断言,
   因为它同时掌握两个时间戳。
4. 计数器由 `main()` 在 join 之后用**裸 `printf`** 输出 —— `WORKER_LOGF` 会被 `SCHEDULER_LOG=0`
   静默关掉,而那正是文档里的性能测量模式,否则"特性触发了"和"性能数据"永远来自不同的运行。
5. 负对照:`EARLY_DISPATCH=0` 强制所有预排拒绝,任务数与完成集合必须完全一致。

**makespan 不是验收指标。** 总工作量 29,045,940 cycles / 32 个可达核 = 907,685,而临界路径
413,810 —— 本 workload **吞吐受限 2.19 倍**,给少数任务省掉往返对 makespan 影响约为零。

## 10. 已知上限

**本 DAG 的机会天花板是 6 / 864 = 0.69%。** 依据 `include/cases/qwen3_14b_decode_subgraph.h` 实测:

```
864 任务, 2742 条边, 但只有 180 条是同类型(不变式 3 拒绝其余)
180 个后继恰好有一个同类型前驱 —— 全部是 type 0
  90 个可证明永不触发: 该前驱是同一后继另一个前驱的传递祖先, 故永不可能是"最后一个未完成"
  90 个存活, 但只汇聚到 6 个不同的前驱 {10:16, 21:16, 32:16, 43:16, 54:16, 65:10}
P 占一个槽、一个核只有一个兄弟槽 ⇒ 每个 P 最多带一个后继 ⇒ 天花板 6
```

6 个 hub 是 `qk_norm`(`duration 13190`),后继是 `rope_kv_cache`(`duration 9480`)。`rope` 依赖
`qk_norm`(vector,同类型)和 `v_proj`(cube,跨类型),经同类型过滤后恰好只剩一个合格前驱。
**整张 DAG 只有这一种形状。**

**且没有链。** 同类型子图的最长路径只有 2 个节点;那 90 个 `rope` 后继自己**没有任何同类型后继**;
342 个单前驱任务中同类型的是 **0** 个。所以 §3 的链式流水在本 workload 上一次都不会发生——但机制
必须支持它,否则换一张有长同类型链的 DAG 就会静默漏掉全部机会。

所以这是**机制建设,不是本 workload 的吞吐优化**。天花板是本 DAG 在不变式 3 下的性质,不是这个
想法的性质。正确性验证应当用专门构造的微型 DAG(含长同类型链),而不是依赖这 6 个偶然机会。

## 11. 本特性暴露于的既有缺陷

不是本次引入,但它们会把本特性的 bug 变成静默失败:

- `resolve_dep()`(`painter.c`)无幂等保护,任何重复完成都会双减后继入度。不变式 1 是唯一防线。
- `g_predecessor_cnt` 是 `uint32_t` 而判据是 `< 1`,多余的递减会下溢而不是报错。
- `completed_task_cnt` 朴素累加、无去重,重复派发会让运行提前"成功"退出。
- `update_task_state()` 声明为 `bool` 但存在无返回值的路径;两处调用都丢弃返回值。
- `batch_enqueue()` 返回值被忽略。仅因 864 < `QUEUE_DEPTH` 1024 才安全。
- `ctrl_t.aicore_spr_1/2` 是普通 `uint64_t*` 无 `volatile`,`REAL_CHIP` 下编译器可能重排或下沉
  本特性依赖的槽位写入。
