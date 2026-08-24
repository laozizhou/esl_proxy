# 2026-08-24 早发调度：多同类型前驱泛化

分支：`dev/early-dispatch-cpu-free`

## 0. 前置：两个阻止程序跑起来的 bug（commit `3665138`）

- **`log.c` 的 `mkdir`**：MinGW 只认单参数 `mkdir(path)`，原代码是 POSIX 双参数
  `mkdir(path, mode)`，Windows 下编译报错。加 `#ifdef _WIN32` 分两条路径。
- **`conf.h` 的 `LOG_OUTPUT_MODE`**：无条件 `#define LOG_OUTPUT_MODE 2`，把
  `Makefile_scheduler` 传入的 `-DLOG_OUTPUT_MODE=0`（只写文件）覆盖成 2（文件+
  屏幕都打印），导致刷屏。加 `#ifndef` 保护，让命令行的值真正生效。

两处都是跨平台/宏覆盖问题，跟早发调度逻辑无关，是能编译跑起来的前提。

## 1. 动机

现有 Case A/B 要求 successor 恰好有 **1 个**同类型前驱（`g_early_st_pred[id]`，
`EARLY_NONE` 表示 0 个或 ≥2 个都不算）。这条限制把"有 2 个及以上同类型前驱"的
任务直接排除在早发调度之外，即使运行时它们也可能只剩最后一个同类型前驱没完成。
目标：把"恰好 1 个"泛化成"1 个或更多"。

## 2. 改动逻辑

- **触发时机不变**：仍是 `g_predecessor_cnt[S] == 1`（总未完成前驱数降到 1），
  不需要额外维护"同类型未完成数"计数器。
- **数学依据**：给定总未完成数==1，同类型前驱这个子集里未完成的数量只可能是
  0 或 1（子集大小不会超过全集），不可能 ≥2——所以触发时现场遍历同类型前驱
  列表就够了,不用在运行期间额外维护计数。
- **数据结构**：`g_early_st_pred[id]`（单值）→ CSR 三件套
  `g_early_st_cnt[id]` / `g_early_st_idx[id]` / `g_early_st_flat[]`
  （`malloc`，大小 = 全图总边数,作为安全上界）。
- **`early_publish_hint`**：遍历 `cnt[s]` 个同类型前驱，找到第一个未完成的就
  发布 hint 并 `return`；全部已完成则什么都不做——原来 `EARLY_NONE` 那条特判
  被 `cnt==0` 时循环体不执行这件事自然取代,不用单独判断。
- **开关**：`EARLY_DISPATCH_MULTI_PRED`（`Makefile_scheduler`），跟
  `EARLY_DISPATCH_CASE_A` 正交，默认关闭，旧路径原样保留作负对照。
- **dispatch.c 完全没改**：消费端接口还是 `g_early_hint[P] = S`，跟 P 是怎么
  被找出来的无关。

## 3. 具体改动文件（commit `8657f4c`）

- `Makefile_scheduler`：新增开关
- `include/scheduler/early_dispatch.h`：CSR 三件套的 `extern` 声明
- `src/scheduler/painter.c`：`early_dispatch_init`、`early_publish_hint`
  按开关分两套实现，旧路径字节级保留在 `#else` 分支

未加 debug 断言（讨论后认为容易写成"用同一份数据验证自己"这种没有意义的断言，
且现有 `ordering_pairs` 已经是概率性但确实存在的安全网）。

## 4. 结果：暂时看不出效果

开关前后，`hints_published`、`plants_case_a + plants_case_b` 看不出系统性差
异，但 `ordering_pairs` 全程 `0 failed`。

### 排查过程

1. **怀疑是这次改动引入回归** → 用 `git stash` / `checkout 3665138` /
   `checkout origin/early-dispatch-cpu-free`（三种方式都指向同一份"完全没被
   碰过"的代码）反复测试，结果同样在 48~90 之间大幅波动。汇总 30 次样本：
   80 出现 9 次、74 出现 7 次、90 和 58 各 4 次、其余更少。**证明这是这台机器
   上 pthread 调度本身的固有噪声**（painter/dispatch 是真实线程,`g_early_hint`
   无锁共享，"P 和 Q 谁先完成"是一场每次运行结果都可能不同的真实时序赛跑），
   跟这次改动无关。
2. **用图结构直接证明测不出差异是必然的** → qwen3 图共 2742 条边，仅 180 条
   同类型；恰好 180 个后继各占其中一条——一条都不剩。数学上这张图里不存在
   任何后继有 ≥2 个同类型前驱。所以新逻辑的 `cnt[]` 在这张图上永远是 0 或 1，
   `early_publish_hint` 里对应"第 2 个及以后"的那部分循环体从未被执行过，新
   旧逻辑在这张图上必然表现一致。

### 结论

- 这次改动**没有被证明错**，但也**没有被真正跑过**——`cnt>=2` 那条路径在
  qwen3 这张图上是死代码。
- 需要一张专门构造的小 DAG（含真实的多同类型前驱任务）才能让这条路径真正
  执行一次，验证选出来的 P 对不对。**尚未做**。
- doc（`early-dispatch-case-b.md`）里"15/15 次都是 90"的说法，跟这台机器上
  的实测（30 次样本里 90 只占 13%）不一致；怀疑是原始测试环境（大概率
  Linux）线程调度粒度跟这台 Windows/MinGW 机器不同导致方差不同，未证实。

## 5. 待办

- [ ] 写一个小 DAG，验证 `cnt>=2` 路径能正确选出"被剩到最后"的那个同类型前驱
- [ ] `EARLY_MAX_PLANTS`（`dispatch.c:30`，现在是 512）：若未来 plants 数量
      明显变多需要调大，目前不用动
- [ ] 跨类型 task 早发调度：AIC/AIV 目前是两个独立仲裁器，没有共享机制,
      "扩展现在的仲裁机"这个说法需要先讲清楚具体指什么机制,再评估可行性
