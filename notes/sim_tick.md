# 跨类型早发调度：sim_tick 扩展

分支：`dev/early-dispatch-cpu-free`

## 1. 动机

现有 Case A/B 只支持同类型早发（P、S 共享一个核的两个槎，靠被动硬件仲裁保证顺序）。
跨类型（cube↔vector）P、S 不共享核，被动仲裁走不通，需要一种主动的"完成后通知
对方"机制。真机上项目负责人说法是"AIC/AIV 可以直接写对方的 SPR"（跟文档里的硬
性约束矛盾,尚未证实,不影响仿真器开发）；1:1 简化（cube 第 i 号 ↔ vector 第 i
号）先把协议本身跑通，1:2 真实拓扑、超时兜底都留到以后。

## 2. 改动逻辑

- **1 bit 就够，不用带身份信息**：只要保证"S 开始等待那一刻，P 保证是它所在
  单元唯一可能完成的东西"，通知就不会有歧义——非抢占仲裁器保证兄弟槽不可能
  抢在 P 前面完成。跨类型 Case A/B 的触发条件正是为了维持这个前提（P 要么刚
  发到全空闲核，要么 P 所在 core 恰好只有一个忙槽，且 S 的目的地必须全空
  闲，见下一版 painter/dispatch 改动）。**判断"P 是不是唯一活跃"不能直接
  读 `g_sim[...].started`**——那是仿真器内部状态，真机 AICPU 看不到；必须
  跟同类型 Case B（`plant_pass`）一样，只用 `free_bitmap` 异或算出"恰好一
  个忙槽"，这是可观测信号，且能证明等价于"没有东西排在它前面"（见 2.1）。
- **"写对方"不是"写自己"**：`g_cross_notify[tid][type][core]` 是"发给
  `(type,core)` 的邮箱"，不是"这个单元自己的完成状态"——退休时往对方
  `[type^1][core]` 写1，不是往自己的位置写。这样设计是因为 P 不可能知道有没
  有人在等自己，只能无条件发；接收方主动读、读完清零。
- **无条件发信号会有陈旧信号问题，靠"放置那一刻清邮箱"解决，不是"开始跑时
  清"**：一开始想的是"新任务开始跑时（`!started`）清自己邮箱"，但这样不够
  早——哪怕这个位置从没跑过东西，配对的另一种类型也可能早就无条件发过好几次
  无关的信号，邮箱里可能已经是1了；S 一上来检查就会误读成"P 已经完成"。真正
  的修法是**放置的那一刻（`sim_place` 里，`needs_notify=true` 时）就清空邮
  箱**——保证 S 从第一次检查起，邮箱只可能反映"放置之后才发生"的信号。原来
  想在"消费时"、"开始跑时"各清一次都变得多余（分析过：两处都只是对同一件
  事的滞后补救，放置时清了之后它们不再有事可做），最终只留 `sim_place` 这
  一处。
- **`sim_tick` 里 waiting_notify 的检查要放在 `started`/`remaining` 判断之
  前**：卡住的任务不倒计时、不标记 started；一轮之内最多做"发现通知→解除阻
  塞→continue"，真正开始跑要等下一轮，不挤在同一轮里处理两件事。

## 3. 具体改动（`src/scheduler/dispatch.c`，`SIM_LATENCY && !REAL_CHIP` 块）

- `sim_slot_t` 加 `uint8_t waiting_notify`
- 新增 `g_cross_notify[DISPATCH_THREAD_CNT][EXE_TYPE_CNT][AIC_CNT]`
- `sim_place` 加参数 `bool needs_notify`，唯一调用点（`place_task` 内）传
  `false`，行为不变；`needs_notify=true` 时顺带清空自己的邮箱
  （`g_cross_notify[tid][type][core] = 0`）——这是全部改动里唯一的清零点
- `sim_tick` 插入两处：卡住检查（`waiting_notify` 时查自己邮箱,`type` 不加
  `^1`,读到 1 就解除阻塞,不再清零)、退休时往对方（`type^1`）邮箱无条件置1。
  原来设想的"`started` 时清自己邮箱"这一步已去掉——见 2.1 上一条,清零提前
  到 `sim_place` 就够了,`sim_tick` 里不用再管邮箱清零

**这是第一个 commit（纯基础设施，无调用点触发新逻辑，行为应与改动前完全一
致）。** 第二个 commit 才加 `g_cross_hint`、painter 侧按类型分流、dispatch
侧的跨类型 Case A/B 判断、新开关 `EARLY_DISPATCH_CROSS_TYPE`。

## 4. 待办

- [ ] 编译验证这次改动后基线（`EARLY_DISPATCH_CASE_A=1 EARLY_DISPATCH_MULTI_PRED=1`）
      跟改动前表现一致（本地 Bash 沙箱编译环境有问题，需要在 PowerShell 里跑）
- [ ] 第二个 commit：`g_cross_hint`、painter 分流、dispatch 侧跨类型 Case A/B
- [ ] hscb / "写对方 SPR" 具体机制、能不能"读"对方——需要黄区代码或硬件同事确认
- [ ] 无界等待的超时兜底——本次不做
- [ ] 1:2 真实拓扑：同类型跨物理单元（如 V1 等 V2）、跨类型配对从"唯一"变成
      "多个候选"，通知机制都要跟着重新设计——1:1 验证通过后再考虑
- [ ] 小型专用 DAG 验证跨类型路径（同 multi_pred，qwen3 图覆盖不到这条新路径）
