# 跨类型早发调度：dispatch 侧消费

分支：`dev/early-dispatch-cpu-free`

跨类型早发一共三次 commit：`sim_tick.md`（notify 基础设施）、
`cross_painter.md`（painter 侧发布 `g_cross_hint`），这是第三次也是最后
一次——dispatch 侧把 `g_cross_hint` 真正消费掉，把 S 放上去。

## 1. 动机

前两次 commit 分别搭好了"怎么等"（`sim_tick`/`sim_place` 的 notify 机
制）和"该等谁"（painter 发布的 `g_cross_hint[P] = S`），但没人真正去查
这张表、把 S 放上去。这次要把 `cross_painter.md` §2 定好的两种场景接进
`dispatch()` 已有的两个入口——跟同类型早发的接入点完全一样：P 刚被放上
空闲核（Case A）接在 `send_task` 里，P 已经在跑（Case B）接在
`plant_pass` 里。

## 2. 改动逻辑

- **`place_task` 加 `needs_notify` 参数**：现有 3 个调用点（`plant_pass`
  种同类型 S、`send_task` 放 P、`send_task` Case A 种同类型 S）全部显式
  传 `false`，行为不变；新增的 2 处跨类型调用传 `true`，一路透传给
  `sim_place`。
- **两个消费点，跟同类型结构对称，检查内容不同**：`plant_pass` 里已经
  用"恰好一个忙槽"精确定位到 P 之后，并列加一段查 `g_cross_hint[p]`；
  `send_task` 里已有的 `idle && slot==0` 块里，并列加一段查
  `g_cross_hint[task_id]`。两处都是**独立的 `if`，不是 `else`**——P 完
  全可能同时挂着一个同类型 successor 和一个跨类型 successor，两者互不
  排斥，要各自判断、各自触发。
- **目的地要单独判断，这是跟同类型的核心差异**：同类型里"P 的核空闲"
  和"S 的目的地空闲"是同一件事（共享一个核）；跨类型 P、S 不共享核，
  找到 hint 之后还要额外检查配对核（`type^1`，同一个核编号）的两个槽
  是不是都空——这是同类型完全不需要的一步。
- **消费 hint 的时机不能照抄同类型**：同类型"win or lose 都直接清空"是
  安全的，因为条件一旦成立，S 必然有地方放，唯一的失败原因是"S 是过期
  重复项"。跨类型不同：目的地这一刻不空闲不等于"过期"，P 放上去之后会
  一直是所在核上唯一的占用者（这条路径本来就要求 P 的核空闲），后续
  `plant_pass` 每一轮都会重新扫到同一个 P，还有机会补上。所以只有"S 已
  经是过期重复项"或"这次真的放成功"才清空 `g_cross_hint`，目的地不空
  闲时原样留着，等下一轮重试。
- **复用现成的正确性断言，不用新写一套**：跨类型的两次 `place_task`
  调用都照样调用 `early_record_plant(P, S)`，`early_dispatch_report`
  里已有的 `ordering_pairs` 断言（`start(S) >= retire(P)`）不区分类
  型，直接就把跨类型的 P/S 对也覆盖了。
- **编译期挡掉没有意义的组合**：跨类型的等待机制只在 `SIM_LATENCY`
  的仲裁器模型里存在——`SIM_LATENCY` 关闭时 `place_task` 走"fake
  return，下一轮无条件退休"那条路，完全不看 `needs_notify`，S 会在没
  真正等到 P 的情况下被判定完成，是静默的正确性错误。`REAL_CHIP` 同理
  排除，真机的通知机制还没接。加了一条 `#error`，`EARLY_DISPATCH_CROSS_
  TYPE` 离开 `SIM_LATENCY && !REAL_CHIP` 直接编译不过。

## 3. 具体改动（`src/scheduler/dispatch.c`、`include/scheduler/early_dispatch.h`）

- `place_task` 签名加 `bool needs_notify`，透传给 `sim_place`。
- `plant_pass`：同类型分支从"没找到就 `continue`"改成 `if`/`else`，好
  让下面并列加的跨类型分支不会被跳过；跨类型分支查 `g_cross_hint[p]`，
  检查配对核空闲，成功则 `place_task(ctrl, type^1, idx, 0, cs, true)`。
- `send_task`：`idle && slot==0` 块里并列加跨类型分支，查
  `g_cross_hint[task_id]`，检查配对核空闲，成功则
  `place_task(ctrl, type^1, core, 0, cross_hint_s, true)`。
- 新增计数器 `g_cross_plants_a`/`g_cross_plants_b`（定义在 `dispatch.c`，
  声明在 `early_dispatch.h`），`early_dispatch_report` 打印。
- `dispatch.c` 顶部加 `EARLY_DISPATCH_CROSS_TYPE` 必须搭配
  `SIM_LATENCY && !REAL_CHIP` 的 `#error` 检查。

## 4. 验证

`make -f Makefile_scheduler run EARLY_DISPATCH_CASE_A=1 EARLY_DISPATCH_
CROSS_TYPE=1 SIM_LATENCY=1 SIM_TICKS=4` 跑了 5 次，`ordering_pairs` 全部
`0 failed`，且每次都精确等于 `plants_case_a + plants_case_b +
cross_plants_a + cross_plants_b`（四类 plant 都被同一份表记录、同一条
断言检查）。`cross_plants_a+b` 大约是 `cross_hints_published` 的
45%-57%，说明发布的跨类型 hint 有接近一半真的转化成了实际早发。四类
plant 总数最高到 321，离 `EARLY_MAX_PLANTS=512` 还有余量，但这是这次
这张图跑出来的数字，不是永久保证。

同类型和跨类型可以同时发生在同一次运行里，甚至同一轮里四种 case（同类
型 A/B、跨类型 A/B）都不为零——这是设计上允许的，因为四个检查两两独
立，不是互斥关系。

## 5. 待办

- [ ] 真机（`REAL_CHIP`）的跨类型通知机制怎么接，目前完全没做
- [ ] `cross_painter.md` §4 提到的 1:2 拓扑问题，这次仍然只按 1:1 实现
- [ ] 图变大、跨类型占比变高时重新留意 `EARLY_MAX_PLANTS`
