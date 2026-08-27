# 跨类型早发调度：painter 侧 hint 发布

分支：`dev/early-dispatch-cpu-free`

## 1. 回顾：同类型早发的两种情况

- **Case B**（`plant_pass`）：核上恰好一个忙槽，即 P 在运行、另一个槽是空
  的，把 S 种进那个空槽。
- **Case A**：核完全空闲（两个槽都空），P、S 一起放上去——P 落槽0、S 落
  槽1。

## 2. 跨类型的两种场景

- **场景1**：P 独自在自己的核上运行，S 的目的地核（同一 AI Core 里另一种
  类型）完全空闲，把 S 放上去。
- **场景2**：P、S 各自的核都完全空闲，分别放上去。

跟同类型不同，这两种场景都不需要规定 S（或 P）必须落在哪个槽——P、S 不
共享核，槽号只是"占了哪个位置"，不影响任何判断。

## 3. 覆盖与不覆盖的场景

### 3.1 覆盖不到：P 在运行、另一个槽也排了任务

P 所在核两个槽都忙（P 在跑，另一个槽排了 P1）时，让 S 早发本质上跟场景1
一样安全——非抢占保证 P1 不可能抢在 P 前面退休。但目前覆盖不到，因为
**两个槽都忙时，`free_bitmap` 分不出谁是正在跑的 P、谁是排队的 P1**（分
槽记录忙闲没问题，缺的是"谁先谁后"这层信息）。

这层信息属于仲裁器内部状态，dispatch 不该直接读。但可以间接推导：dispatch
自己就是把两个任务先后放进两个槽的那个人，只要记一笔"这个槽是第几个被放
上去的"，结合仲裁器有序、非抢占这条性质，就能推出"两个忙槽里先放上去、且
还没在 `msg_bitmap` 里看到完成信号的那个，就是正在跑的那个"。这份放置顺
序的记录目前还不存在，留作以后扩大覆盖率的方向。

### 3.2 故意不覆盖：P 前面还有 P0 在跑

P 所在核上 P0 在跑、P 排在后面等（两个槽都忙）时，不能拿 P 当触发对象。
邮箱按 `(type, core)` 无条件发，不认任务身份——这个 `(type,core)` 上一旦
有任务退休，邮箱就会置1。P0 排在 P 前面会先退休，若拿 P 触发，P0 退休时
S 就会把这次 post 误当成"P 已经完成"提前解除阻塞，而 P 其实可能还没开始
跑。这是正确性 bug，比 3.1 更严重。

根源和 3.1 相同：dispatch 分不清"两个忙槽"里谁在跑、谁在排队，所以干脆把
两个槽都忙的核整体排除在触发候选之外，不去猜。

### 3.3 故意不覆盖：S 前面还有 S0 在排队

S 的目的地核不是完全空闲，已经有任务 S0 占着时，也不做跨类型早发。S 会被
双重卡住——先等仲裁器轮到自己（等 S0 退休），再等 cross notify（等 P 完
成）——收益比只等 P 更小。这是场景1/2 要求 S 的目的地核必须完全空闲的原
因。

### 3.4 组合缺陷：早发的早发

S 被跨类型早发放到目的地核后，核上"恰好 S 一个占用者"，这个状态跟 S 是不
是真的在跑（`waiting_notify`）无关——`plant_pass` 只看 `free_bitmap`/
`task_id_map`，不知道 `waiting_notify` 这回事。如果 S 恰好是某个同类型任
务 S1 的唯一同类型前驱，`plant_pass` 会照常把 S1 种进 S 的兄弟槽。

但 S 此时其实卡在等 P，S1 的等待时间就变成"S 等 P 的时间 + S 真正执行的
时间"，比只等 S 执行时间长得多。仲裁顺序依然正确（S1 不会先于 S 跑），只
是等待被放大了。

要过滤这种情况，得让 `plant_pass` 读 `waiting_notify`，等于让仿真器具备
真机没有的透视能力（AICPU 本来就分不清"卡住"和"真在跑"），所以做不到。
这是跨类型和同类型两个机制拼在一起才暴露的组合情况，本轮先接受。

## 4. 留给以后：1:2 拓扑

以上都基于 1:1 简化（cube i ↔ vector i 严格一对一）。这个前提下跨类型关
系只能"一来一回"，链不会拉长——S 想再当"P"触发下一层，配对目标只能是 P
自己那个核，而 P 没退休时那个核不可能空闲，链条自然断掉（3.4 的叠加风险
只会通过同类型 `plant_pass` 发生，不会通过跨类型机制本身叠加）。

1:2 拓扑（1 个 cube 对 2 个 vector）下，配对关系不再唯一，需要重新分析两
点：

- 可能出现 vector1 → cube → vector2 这样的交错链，会不会引入新的叠加风
  险。
- 同类型早发多了一个选项：V1 忙、V2 完全空闲时，把 successor 发到 V2、而
  不是排进 V1 的兄弟槽。但这不等于"直接跑"——S 不管落在哪个物理单元，都
  要等 P 真正退休才能开始算。排进 V1 兄弟槽之所以能白等，是因为 V1 上仲
  裁器的相位指针退休后自动轮到兄弟槽，不需要额外机制；发到 V2 则是两个独
  立仲裁器，需要一套跟跨类型同样的主动通知机制才能等到 P 退休。两条路径
  下 S 实际开始跑的时间点大概率相近，"发到 V2"未必更优，值不值得做需要
  重新设计和分析。

## 5. 具体改动（init + painter，跨类型 hint 发布）

只做 painter 侧"发布跨类型 hint"这一半，dispatch 侧怎么消费还没做。

- `Makefile_scheduler`：新增 `EARLY_DISPATCH_CROSS_TYPE=1`，强制带上
  `EARLY_DISPATCH_MULTI_PRED`——跨类型的查找复用 multi_pred 的 CSR 遍历模
  式，不在旧的单值分支里重复实现。
- `include/scheduler/early_dispatch.h`：新增 `g_early_ct_cnt`/`idx`/`flat`
  （跨类型前驱的 CSR，跟 `g_early_st_*` 平行）、`g_cross_hint`（跟
  `g_early_hint` 同样按前驱 P 寻址）、`g_cross_hints_published` 计数器。
- `src/scheduler/painter.c`：
  - 全局变量：跟头文件声明对应的定义。
  - `early_dispatch_init`：构建 `g_early_st_flat` 的同一个循环里加
    `else` 分支，把类型不同的前驱写进 `g_early_ct_flat`（独立游标），一次
    遍历同时建好两份 CSR；`g_cross_hint[i]` 在初始化循环里跟 `g_early_hint[i]`
    一起清成 `EARLY_NONE`。
  - `early_publish_hint`（`EARLY_DISPATCH_MULTI_PRED` 分支）：同类型循环
    找不到未完成前驱时，不再直接返回，改为接着走 `g_early_ct_flat`，找到
    未完成的就发布 `g_cross_hint[p] = s`。
- `src/scheduler/dispatch.c`：`early_dispatch_report` 里加一行打印
  `cross_hints_published`，方便不改 dispatch 消费逻辑就能先看 painter 这
  半是否符合预期（比如数值能不能跟图里跨类型边的数量对上）。

## 6. 验证结果

`make -f Makefile_scheduler run EARLY_DISPATCH_CASE_A=1 SIM_LATENCY=1
SIM_TICKS=4 EARLY_DISPATCH_CROSS_TYPE=1` 跑了 4 次：

```text
hints_published    = 58 / 58 / 80 / 80
plants_case_b      = 0（4 次都是，跟这次改动无关，同类型 Case A 已经吃掉
                        了大部分机会，Case B 轮不上）
plants_case_a      = 6 / 5 / 5 / 5
cross_hints_published = 612 / 581 / 595 / 582
ordering_pairs     = 0 failed（4 次都是，只覆盖同类型 P/S 对——跨类型这半
                     dispatch 还没消费，不影响调度行为）
```

`cross_hints_published` 在 580-612 之间，量级比 `hints_published` 大一个
数量级。对 `qwen3_14b_decode_subgraph.h` 原始数组做静态统计（不依赖运行
时时序）验证了这个量级：

- 总边 2742 = 同类型 180 + 跨类型 2562。
- 864 个任务：6 个零前驱、342 个恰好 1 个前驱（纯链式）、516 个多前驱。
- **342 个纯链式任务，唯一的那个前驱全部是跨类型，一个同类型都没有**。这
  些任务的 hint 在提交时（`add_successors`，入度从一开始就是1）确定性发
  布，每次运行稳定贡献 342 条 `cross_hints_published`，不受线程调度影响。
- 剩下 239-270 来自 516 个多前驱任务，运行时竞争决定"谁是最后一个"，是
  hints_published（58-80，同类型 180 条边全部落在这 516 个任务里，链式
  任务一条同类型都没有）同一种噪声来源。

结论：跨类型 hint 数量比同类型多一个数量级，不是异常，是这张图"cube/
vector 严格交替"的结构决定的——同类型早发能覆盖的场景本来就窄，跨类型才
是这张图里早发机会的大头。但这只是 painter 侧"发现机会"的数量，不等于
最终收益：dispatch 侧还没接上，§3 的覆盖范围本来就收得很窄，实际能吃到
多少要接上 dispatch 之后才知道。
