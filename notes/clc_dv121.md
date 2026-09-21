# CLC（dv121）：AICPU 侧任务表生成

分支：`dev/clc-dv121`（从 `dev/early-dispatch-cpu-free` 分出，基线 `5a8ab55`）

CLC 是 dv121 上一种新调度方案的代号，这是它 AICPU 侧的草稿。方案本身由同事提出，尚有若干
细节未定，本分支只实现已经确定的那部分：**读 case 的 DAG、排序、生成任务表**。写 GM 留空。

---

## 1. CLC 是什么：调度权从 AICPU 翻给 AICore

| | 现在（`src/scheduler/`） | CLC |
|---|---|---|
| 谁解依赖 | AICPU（painter 线程） | AICore 自己 |
| 谁决定哪个核跑哪个任务 | AICPU（dispatch 查空闲位图） | AICore 自己去抓 |
| AICPU 干什么 | 常驻循环：收完成、减入度、挑核、写 SPR | 一次性：读 case → 排序 → 写 GM → 退场 |
| 通信 | AICPU 写 AICore 的 SPR，读 MSGQ 完成位 | GM 里一张共享任务表 |

依赖用 notify counter 表达：每个 `task_id` 对应一个同号的硬件计数器 `cnt_<task_id>`。任务跑完
给自己的计数器发信号（publish），想跑之前先确认所有前驱的计数器都已被发过信号（subscribe）。

**关键前提（来自同事的方案）**：AICore 抓任务时**不检查依赖是否就绪**，先把任务拉到核上再在
核上等前驱。这条决定了下面第 3 节的一切。

---

## 2. 改了哪些文件

| 文件 | 状态 | 内容 |
|---|---|---|
| `esl_proxy/include/clc/clc_task.h` | 新增 | `clc_task_t`（80 字节）、`clc_type_t`、ABI 断言 |
| `esl_proxy/src/clc/clc_prepare.c` | 新增 | AICPU 侧全部逻辑 |
| `Makefile_clc` | 新增 | 独立构建，产出 `bin/clc_prepare` |
| `esl_proxy/include/scheduler/conf.h` | 改 7 行 | 给 `PAINTER_THREAD_CNT` 加 `#ifndef` 保护 |

前三个是自包含的一棵新树，和 `algorithm/`、`scheduler/` 两棵零耦合——将来整体搬走或删掉都不
影响现有代码。

`conf.h` 那处改动是必需的：`cases/*_total_graph.h` 带 `#if PAINTER_THREAD_CNT != 1` 守卫，而
`conf.h` 无条件 `#define ... 2`，命令行的 `-D` 覆盖不掉。同样的手法在 `LOG_OUTPUT_MODE` 上已有
先例（见 `multi_pred.md`）。已实测：现有 scheduler 构建照旧拿到 2，编译退出码 0。

---

## 3. 为什么排序是 AICPU 唯一要做好的事

AICore 按列表顺序抓，所以**列表顺序就是抓取顺序**。抓到一个还不能跑的任务，核就占着干等，而
后面本可以立刻跑的任务只能留在 GM 里——这是队头阻塞。

用真实 qwen3 DAG 仿真（64 cube 核 + 64 vector 核，每核 1 槽），对照组是理想的推模型：

```
排序方式                    makespan
task_id 顺序（编排原始序）   858,710 ns     慢 65%
按 est / 按层               519,460 ns     追平理想推模型
关键路径下界                413,810 ns
```

两种排序**都是合法拓扑序、都不会死锁**，差别纯粹是性能。`task_id` 顺序的问题在于层号锯齿：
vector 列表里 `3,5,7,3,5,7…` 重复上百次，核抓到层 7 的任务卡住，层 3 的在后面排队。

**结论：拓扑序是正确性底线（编排输出天然满足，白送），排序质量是这份工作的全部价值。**

---

## 4. 排序键：est（最早可开始时刻）

```
est[i] = max over 前驱 p ( est[p] + duration[p] )，无前驱则为 0
```

即"假设核无限多，这个任务最早能在什么时刻开始"。按 est 升序排，列表顺序就和任务真正变得可跑
的先后顺序对齐。

选它的三个理由：

1. **白送拓扑序**。权重恒为正时每条边都有 `est[后继] > est[前驱]`，按 est 排必然前驱在前，
   不用再单跑一遍拓扑排序。权重为正由 `check_input_durations()` 守住。
2. **不死锁可证**。顺序抓取 + 拓扑序：当前所有核持有的任务中，排在列表最前面的那个，它前面的
   任务必然都已被抓走且都不在核上（否则就有更靠前的在核上），也就是都跑完了，所以它的前驱全
   部就绪，一定能跑。总有任务能跑 ⇒ 不会全体卡死。
3. **层序是它的特例**。所有 duration 当作 1，est 就退化成拓扑层号。`CLC_USE_DURATION=0` 即层
   序，不必写两套代码——真机 profiling 回来若发现 duration 估计误差超过 ±100%，关掉开关即可。

**已知局限**，不要当成最优解：静态排序在执行前定死，运行时偏差无法响应，原理上不可能保证追平
能看到真实完成时间的动态调度；est 还假设核无限多，忽略核竞争。这是拉模型换取"AICPU 不参与运
行时"的固有代价。

**一个必须记住的事实**：在 qwen3_14b_decode 上，`CLC_USE_DURATION` 取 0 和取 1 排出来的顺序
**逐字节相同**。这个 DAG 完全分层（每层是同一算子的 SPMD 切片，同时就绪、耗时相同），14 个
est 取值和 14 个拓扑层成员一一对应。所以**别在这个 case 上 A/B 这个开关**，它测不出差别；要
验证需要一个层内耗时不规则的 workload。

---

## 5. GM 表的布局

```
偏移   大小   字段
  0     4    task_id                      全局 id，不是表内下标
  4     4    duration
  8     4    publish_notify_id            跑完给哪个 counter 发信号
 12    64    subscribe_notify_id[16]      要等哪些 counter（= 各前驱 id）
 76     1    subscribe_cnt                上面数组里有效的项数
 77     1    type                         0=vector, 1=cube
 78     2    reserved[2]                  对齐填充
──────────
        80    x 864 = 69,120 字节
```

**相对同事原结构只多了 `subscribe_cnt`**。原结构里订阅列表是变长的，这里用定长 16 槽，AICore
必须知道前几个有效。备选是末尾放哨兵，但那样 16 前驱的任务要占 17 槽、还得边读边比；存个数是
1 字节且 O(1)。

**为什么定长不用 CSR**：CSR 更省（22 KB vs 66 KB），但拉模型里 AICore 每抓一个任务都要自己算
地址——定长是一次乘法，CSR 要两次访存。取数开销由每个核的每次抓取承担，值得用空间换。

**16 这个上限**来自实测：qwen3_14b_decode 最大前驱数正好 16（分布 0个×6, 1个×342, 2个×294,
3个×90, 7个×6, 9个×60, 10个×16, 16个×50）。换 workload 要重新核，超限会直接报错退出，不会静默
截断。

**两个容易踩的坑**：

- **表内下标 ≠ task_id**。排序后第 k 项的 `task_id` 一般不等于 k，而 counter 是按 task_id 编
  号的。`pos` 不存进 GM——它就是数组下标。
- **type 编码和 `common/task.h` 相反**。case 数据是 `aiv=0, aic=1`，`common/task.h` 是
  `TASK_TYPE_CUBE=0, TASK_TYPE_VECTOR=1`。老调度器没出事只因为仿真里两种核对称。`clc_task.h`
  故意不 include 那个头，自己定义一套照抄 case 数据的编码。

---

## 6. mix 怎么处理

case 数据里 mix 是独立的 `type=2`（60 个 out_proj 任务，duration 全是 40750）。同事的方案没有
定义 mix 任务由谁 claim——它要同时占一个 cube 和一个 vector，拉模型里没有全局视角。

**规则：GM 里可以有冗余字段，但不能有对方看不懂的值。** 所以：

- `type=2` **绝不进 GM**。写进去 AICore 读到看不懂的值，那 60 个任务没人抓，而直接或间接依赖
  它们的有 288 个——合计 **348/864（40%）一起卡死**。
- 写表前由 `clc_fold_type()` 折成 cube。折进 cube 而非 vector 是按正确性选的（out_proj 主体是
  matmul，duration 40750 贴近 cube 均值 53361、远离 vector 均值 12811）；实测两种折法 makespan
  差 <1%，性能上无所谓。
- 反过来 `publish_notify_id` 虽然恒等于 `task_id`、冗余，但保留——如果将来 counter 稀缺需要复
  用，这个等式就不成立了，留字段比改 ABI 便宜。

**等同事答复后，改 `clc_fold_type()` 一个函数即可**，源码里 `CLC_TYPE_MIX` 的定义保留着，60 个
mix 任务在任何时候都还能认出来，折叠是可逆的。

---

## 7. 自检

`verify()` 五项，任何一项失败 → 打印失败详情、退出码 1、**并且跳过写 GM**：

1. 每个任务恰好出现一次（无重复、无缺失）
2. **输出顺序是合法拓扑序** —— 不死锁的前提，最重要的一条
3. GM 表里只有 AICore 定义过的 type（0 或 1）
4. `publish_notify_id == task_id`（当前约定）
5. **表内每一项确实来自它 task_id 对应的那条源数据** —— 前四项只拿表和表自己比对，取错下标它们
   全都会通过；而 task_id / 数组下标 / 表内位置三个索引空间搞混，正是这个文件最容易犯的错

另有三项输入校验（`build_index` / `check_input_topological` / `check_input_durations`）：
重复 id、id 越界、悬空前驱、前驱不在前面、duration 非正，都是直接报错退出。

审查时做过 13 个故障注入，全部被拦住；其中 `g->duration[k]` 写成 `[i]` 和 `pre_idx[k]` 写成
`[i]` 这两个**只有第 5 项能抓到**。

---

## 8. 构建与运行

这台机器 PATH 里没有编译器，但 MSYS2 装在 `C:\msys64`：

```bash
export PATH="/c/msys64/ucrt64/bin:/c/msys64/usr/bin:$PATH"   # gcc 16.1.0 + make
cd <repo>
make -f Makefile_clc run    # 编译 + 运行，报告同时存到 log/clc_run.txt
make -f Makefile_clc dump   # 另外把整张表导到 log/clc_table.csv
make -f Makefile_clc clean
```

开关：

```bash
make -f Makefile_clc CLC_USE_DURATION=0 run                  # 层序（见第 4 节）
make -f Makefile_clc CASE=paged_attention_total_graph.h run  # 换 case（须是 *_total_graph.h）
```

**输出位置**：`log/clc_run.txt`（人读的报告）、`log/clc_table.csv`（864 行任务表）。`log/` 和
`bin/` 都在 `.gitignore` 里。`--dump` 时报告走 stderr、CSV 走 stdout，所以
`clc_prepare --dump > x.csv` 直接就是一个干净的 CSV。

**MSYS2 的两个坑**（都已在 Makefile 里处理，记下来免得再踩）：

- **`/usr/bin/make` 会把 `OS`、`TMP`、`TEMP`、`USERPROFILE` 从 recipe 环境里抹掉**（`$(origin)`
  报 undefined）。所以不能用 `$(OS)` 判平台，改用仓库已有的 `uname -s` + `findstring NT`。
- 同一个抹除导致 **gcc 找不到临时目录，退回 `C:\WINDOWS\` 然后报
  `Cannot create temporary file ... Permission denied`（exit 127）**。`TMPDIR` 没用（Windows 版
  gcc 走 Win32 `GetTempPath()`），在外层 shell 设 `TMP` 也没用（会被抹掉），只能在 Makefile 里
  `export TMP`。**`Makefile_scheduler` 也有这个问题，尚未修。**

---

## 9. 实测结果

```
[clc_prepare] 任务数            = 864
[clc_prepare] 排序键            = est（最早可开始时刻，使用 duration）
[clc_prepare] 关键路径          = 413810 ns
[clc_prepare] vector / cube     = 402 / 462          （402 原生 cube + 60 折叠的 mix）
[clc_prepare] 折成 cube 的 mix  = 60
[clc_prepare] 最大订阅数        = 16（上限 16）
[clc_prepare] 表占用            = 69120 字节（864 项 x 80）
```

`CLC_USE_DURATION=0` → 层数 14。`CASE=paged_attention_total_graph.h` → 1920 任务、165620 ns、
960/960、0 个 mix、最大订阅 1。

干净构建**零警告**（在 `-Wall -Wextra -pedantic` 之外，`-Wshadow`/`-Wundef`/
`-Wformat-signedness`/`-Wconversion` 也都是 0），退出码 0，五项自检全过。

输出经两次独立交叉验证：一次用 Python 重算（单趟前向递推），一次由审查 agent 用 Kahn 算法重
推，两边 CSV 逐字节相同。关键路径 413810 有实证链条：
`0→1→10→66→67→68→69→70→516→526→528→529→554→555`，14 个任务 duration 之和正好是它。

---

## 10. 还没定的事

**卡在同事那边的**（都不影响本分支已写的部分）：

1. **AICore 抓到不能跑的任务会不会退回/跳过？** 优先级最高——如果能跳，队头阻塞自行消失，排序
   的重要性大幅下降。
2. **每核能同时持有几个任务（ostd）？** 仿真里 1 槽 vs 2 槽差 1.65x vs 1.39x。
3. **抓取游标是一个全局的，还是按 type 各一个？** 若是后者不必重排——这张表按 type 过滤出来的
   子序列天然保持相对顺序，仍是各自的合法拓扑序。
4. **counter 数量够不够？** 864 个任务就要 864 个。若需复用，`publish_notify_id` 就不再等于
   `task_id`（字段已留好），而且会多一条约束：counter 回收前所有订阅者必须已读——那时排序就从
   性能问题变成正确性问题了。
5. **mix 的最终处置**（第 6 节）。

**本分支自己的 TODO**：

- `clc_write_to_gm()` 是空桩。GM 基址、映射方式、搬运手段、表头（AICore 怎么知道表有多长）都
  未定。结构体布局已由 `_Static_assert` 钉死，两侧 include 同一个头文件，所以定下来之后大概率
  就是一次 `memcpy`，不需要序列化。
- `CLC_MAX_TASKS` 借用了 `RING_SIZE`（2048）。paged_attention 是 1920，只剩 6% 余量。GM 布局定
  了之后应换成 CLC 自己的常量。
- 真机 profiling 拿到后，核一下 `duration` 的估计误差，决定 `CLC_USE_DURATION` 取 0 还是 1。
