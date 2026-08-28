# 早发让位 ready_queue

分支：`dev/early-dispatch-cpu-free`

## 1. 动机

早发任务占用了一个计算单元、还在等自己的前驱完成的时候，如果 `ready_queue` 
里恰好冒出一个新任务、本可以立刻用这个核，会不会反而更低效——早发抢到手的是一
个"晚点才有收益"的位置，挤走的是一个"现在就能用"的机会。

这个问题不是新出现的，是这次跨类型早发全程都悬着但没人正面回答过的一条：
之前的设计（`plant_pass` 排在三次 `send_task` 之前）默认"早发先抢"，从没
认真权衡过要不要让 `ready_queue` 优先。往下追一步，发现问题不止 `plant_pass`
（Case B）这一处——`send_task` 内部三个类型顺序处理（`MIX → VECTOR →
CUBE`），跨类型 Case A 内联在 `send_task` 里，处理 VECTOR 的时候就能当场
把 CUBE 的一个空闲核抢走，这时候 CUBE 自己的 `ready_queue` 还没轮到，抢的
是"另一个类型还没来得及表态的需求"，比 Case B 那边的问题更隐蔽。

## 2. 改动逻辑

- **核心思路：把早发（不管哪种 case）都挪到三次 `send_task` 全部跑完之
  后**——这样早发只能捡"三个类型的 `ready_queue` 需求都被满足之后"剩下的
  核，不会抢在任何真实、当下就能用的需求前面。`ready_queue` 里的任务立刻
  就能开始跑，早发的 successor 拿到核也还得等前驱，拿"立刻有收益"的机会
  换"以后才有收益、而且前驱迟迟不完成还可能被无限拖着"的机会，划不来。
- **顺带发现：Case A 变得不需要单独存在了。** 一旦 `plant_pass` 排到最
  后，P 只要被 `send_task` 放上了一个原本完全空闲的核，到 `plant_pass`
  检查这一刻，这个核天然就是"恰好一个忙槽"——跟 P 已经在那躺了很多轮是同
  一种状态，`plant_pass` 分不出也不需要分。Case A 当初存在，是因为
  `plant_pass` 排在 `send_task` 前面、P 刚下发时还赶不上这一轮的扫描，得
  靠内联逻辑当场补上；顺序一换，这个理由本身就没有了。这不是这次改动的目
  的，是重排之后自然推出来的结果。
- **验证方式，分两步、不一次性下手删代码**：先只挪 `plant_pass` 的调用位
  置，Case A 原样不动，跑一遍确认没坏；再用 `EARLY_DISPATCH_CASE_A` 这个
  Makefile 开关做 A/B 对比（开 vs 关，代码不用改），确认关掉之后
  `ordering_pairs` 依然 `0 failed`、且 `plants + cross_plants` 的总数变化
  符合预期（见 §4），才真正把 Case A 的代码和开关整段删掉。

## 3. 现在覆盖的两种情况

Case A/B 合一之后，早发只剩两种情况，靠同一次扫描（`plant_pass` 检查
"P 是不是所在核上唯一的占用者"）触发，不再区分"P 刚下发"还是"P 已经跑
了很久"：

- **同类型**：占着核的 P 有一个同类型 successor S 在等它（`g_early_hint`，
  或 `MULTI_PRED` 下 CSR 列表里唯一未完成的那个）——S 直接种进 P 的兄弟
  槽，靠仲裁器的非抢占性保证顺序，不需要额外通知。
- **跨类型**：successor S 是跨类型的（`g_cross_hint`），多查一步：P 配
  对的那个核（1:1 假设下，`type^1`、同核编号）是不是完全空闲，空闲才把
  S 放上去，靠 `sim_place`/`sim_tick` 那套主动通知机制（`waiting_notify`/
  `g_cross_notify`）等 P 退休后被唤醒。

这次扫描本身排在三次 `send_task` 之后，保证两种情况都不会抢在当轮真实的
`ready_queue` 需求前面。

## 4. 具体改动（`src/scheduler/dispatch.c`、`Makefile_scheduler`）

- `dispatch()`（[dispatch.c:627](esl_proxy/src/scheduler/dispatch.c#L627)）：
  `plant_pass(tid)` 从三次 `send_task` 之前挪到之后（[:657](esl_proxy/src/scheduler/dispatch.c#L657)）。
- `send_task`（[dispatch.c:502](esl_proxy/src/scheduler/dispatch.c#L502)）：
  删掉内联的 Case A 逻辑（同类型 `hint_s`、跨类型 `cross_hint_s` 两段），
  恢复成只做正常派发；"prefer slot 0"那条注释里跟 Case A 相关的一句也删了
  （这条规则本身还在起作用，只是不再是 Case A 的专属理由）。
- `plant_pass`（[dispatch.c:428](esl_proxy/src/scheduler/dispatch.c#L428)）：
  头部注释和内部同类型分支的控制流从"查不到就 `continue`"改成 `if`/`else`
  （这个改动其实是更早一次改动——加跨类型分支时就做了，这次只是顺带把
  "Case A/B"这套已经不准确的说法从注释里去掉）。
- `Makefile_scheduler`：`EARLY_DISPATCH_CASE_A` 开关整段删除；
  `EARLY_DISPATCH` 的注释改成描述现在统一之后的机制。
- 计数器清理：`g_early_plants_a`/`g_cross_plants_a` 删除（Case A 没了，
  这两个永远是0，是死代码）；`g_early_plants_b`/`g_cross_plants_b` 改名成
  `g_early_plants`/`g_cross_plants`（"_b"这个后缀失去了对照的"_a"，留着没
  意义）。
- 报告打印格式整理：加一行 `cross_type=on/off`；四个计数器统一改成
  `same_type_hints_published`/`same_type_plants`/`cross_type_hints_published`/
  `cross_type_plants` 这套一致的命名；顺序调整为
  `latency → early_dispatch → cross_type` 打头，`skipped_duplicate` 挪到
  `cross_type_plants` 后面。

## 5. 效果验证

`make -f Makefile_scheduler run EARLY_DISPATCH_CROSS_TYPE=1 SIM_LATENCY=1
SIM_TICKS=4` 跑了几十次，`ordering_pairs` 全程 `0 failed`，正确性没受任
何影响。真正有意义的对比是"Case A 还在、还会跨类型抢核"和"Case A 已经不
存在"这两种状态下 `cross_type_plants` 的量级：

- **Case A 还在**（内联在 `send_task` 里，会在别的类型的 `ready_queue`
  还没处理之前就抢核）：跨类型 plants 总数大概在 250-300 这个区间。
- **Case A 不存在**（不管是当初用开关关掉，还是现在彻底删代码）：稳定落
  在 180-280 这个区间，整体明显往下移了一截，而且这个下降在十几次、几十
  次运行里反复重现，不是单次运行的噪声。

这个下降是**预期且正确**的：少掉的这部分，正是本该留给别的类型 `ready_queue`
的机会，之前被 Case A 提前"偷"走了。这也直接证实了负责人最初提的那个疑
虑——"早发占着核、`ready_queue` 新任务被挤"——**确实真实发生过**，不是
纸上谈兵的担心。同类型 `plants` 这次全程稳定在5-6，不受影响，因为同类型
successor 从来都是落在 P 自己核的兄弟槽，不存在"抢别的类型"这回事。
