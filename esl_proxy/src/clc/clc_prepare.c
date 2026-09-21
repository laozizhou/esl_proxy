/*
 * clc_prepare.c - CLC (dv121) AICPU 侧的全部工作
 *
 * CLC 方案里 AICPU 不再参与运行时调度。它只在初始化时做一件事：
 *
 *     读 case 里的 DAG  ->  排序  ->  写成 GM 任务表  ->  退场
 *
 * 之后 AICore 自己从 GM 抓任务、自己解依赖。所以这个文件就是 AICPU 侧的全部逻辑。
 *
 * ---------------------------------------------------------------------------
 * 为什么排序是这里唯一重要的事
 *
 * AICore 抓取任务时不检查依赖是否就绪 —— 它先把任务拉到核上，再在核上等前驱。于是抓到
 * 一个还不能跑的任务就会占着核干等，而后面本可以立刻跑的任务只能留在 GM 里。核是按列表
 * 顺序抓的，所以列表顺序就是抓取顺序，排序直接决定这种浪费有多少。
 *
 * 排序键用 est（earliest start time，最早可开始时刻）：
 *
 *     est[i] = max over 前驱 p ( est[p] + duration[p] )，无前驱则为 0
 *
 * 也就是"假设核无限多，这个任务最早能在什么时刻开始"。按 est 升序排，列表顺序就和任务
 * 真正变得可跑的先后顺序对齐了。
 *
 * 三个性质让它成为现阶段的合理选择：
 *   1. 白送拓扑序。权重恒为正时任何一条边都有 est[后继] > est[前驱]，所以按 est 排出来
 *      必然满足"前驱在前"，不需要再单跑一遍拓扑排序。而拓扑序是正确性底线：顺序抓取 +
 *      拓扑序可以证明不死锁 —— 当前所有核持有的任务中排在列表最前面的那个，它前面的任务
 *      必然都已被抓走且都不在核上，也就是都跑完了，所以它的前驱全部就绪，它一定能跑。
 *      "权重恒为正"这条由 check_input_durations() 显式守住，不是假设。
 *   2. 成本 O(V+E)，一趟扫描。
 *   3. 层序是它的特例 —— 把所有 duration 当作 1，est 就退化成拓扑层号。所以
 *      CLC_USE_DURATION=0 就是层序，不必写两套代码。真机 profiling 回来发现 duration
 *      估得太离谱（误差 >±100%）就把开关关掉。
 *
 * 已知的局限，不要当成最优解：静态排序在执行前就定死了，运行时的任何偏差它都无法响应，
 * 所以原理上不可能保证追平能看到真实完成时间的动态调度。est 还假设核无限多，忽略了核
 * 竞争。这些是拉模型换取"AICPU 不参与运行时"的固有代价。
 * ---------------------------------------------------------------------------
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clc/clc_task.h"
#include "scheduler/conf.h"   /* RING_SIZE。case 头文件也会 include 它，这里显式写出来，
                               * 免得 CLC_MAX_TASKS 依赖一条看不见的传递引入。*/

/* 输入的 DAG。用总图版（而不是 *_subgraph.h）有两个理由：CLC 要的是一条全局列表，不是
 * 按 painter 线程切开的两组；而且总图版保留了 mix 作为独立的 type=2，subgraph 版已经把
 * 它并进 type 1 丢掉了。
 * 换 case 只改这一个宏（另一个可选项是 cases/paged_attention_total_graph.h），报告里打印
 * 的也是它，不会出现"换了 case 但报告还写着老名字"。
 * 注意此头文件带 #if PAINTER_THREAD_CNT != 1 守卫，靠 Makefile_clc 传 -D 满足。*/
#ifndef CLC_CASE_HDR
#define CLC_CASE_HDR "cases/qwen3_14b_decode_total_graph.h"
#endif
#include CLC_CASE_HDR

/* case 头文件把那个结构体命名为 `subgraph`，是"按 painter 线程切成两组"时代的遗留：分组版
 * 是 test_graph[2]（每组 432 个任务），总图版复用了完全相同的结构体形状和变量名，只是数组
 * 长度变成 1、那唯一一项装着全部 864 个任务 —— 这样 painter.c 里 for (sg = 0; sg <
 * PAINTER_THREAD_CNT; sg++) 的循环两种数据都能跑，代码不用改。
 *
 * CLC 用的就是那"唯一一组"，它是整张图，不是某个子图。类型名会误导，这里起个准确的别名；
 * 底下所有函数签名都用它，免得每看到一次 subgraph 都要重新想一遍。 */
typedef subgraph clc_graph_t;

/* 1 = 按 est 排序（用 duration）；0 = 把所有 duration 当作 1，退化为按拓扑层号排序。
 *
 * 注意：在 qwen3_14b_decode 上这两个取值排出来的顺序**逐字节相同**，别在这个 case 上 A/B
 * 它。原因是这个 DAG 完全分层：14 个 est 取值和 14 个拓扑层一一对应、成员完全一致（每层是
 * 同一个算子的 SPMD 切片，同时就绪、耗时相同），所以从根到任一任务的所有路径既跳数相同、
 * 加权长度也相同。开关唯一改变的是报告里那个数字（413810 ns vs 14 层）。
 * 要让这个开关真正产生差别，需要一个层内耗时不规则的 workload。*/
#ifndef CLC_USE_DURATION
#define CLC_USE_DURATION 1
#endif

/* id 空间上限。case 数据里的 task_id 是全局 id，scheduler/conf.h 的 RING_SIZE 就是这个
 * id 空间的大小，沿用它，顺便让越界能被检查抓到。
 * （严格说 RING_SIZE 是调度器环形缓冲的大小，和 CLC 的 id 空间只是恰好同源；等 GM 布局
 * 定下来后这里应该换成 CLC 自己的常量。）*/
#define CLC_MAX_TASKS RING_SIZE

/* ------------------------------------------------------------------ 内部状态 */

/* 排好序的 GM 任务表。表内下标 k 是抓取顺序，和 task_id 无关。 */
static clc_task_t g_clc_table[CLC_MAX_TASKS];
static uint32_t   g_clc_task_cnt;

/* 下面两个数组都按 task_id 索引，既不是 case 数组下标，也不是表内位置。 */
static uint64_t g_est[CLC_MAX_TASKS];
static int32_t  g_index_of_id[CLC_MAX_TASKS];  /* task_id -> case 数组下标，-1 = 不存在 */

static uint32_t g_order[CLC_MAX_TASKS];        /* 排序结果：按抓取顺序排列的 task_id */

static uint32_t g_folded_mix_cnt;              /* 被折成 CUBE 的 mix 任务数，仅供报告 */

/* 人读的输出（自检、统计）走这里；--dump 时 main() 把它指向 stderr，stdout 就只剩纯 CSV，
 * 于是 `clc_prepare --dump > table.csv` 直接是一个能用的 csv，不用再 sed 掉前面十几行。
 * 不能在文件作用域初始化成 stdout（标准 C 里它不是常量表达式），所以 main() 一进来就赋值。
 * 致命错误始终走 stderr（见 die），和这个开关无关。*/
static FILE *g_report;

static _Noreturn void die(const char *what)
{
    fprintf(stderr, "[clc_prepare] FATAL: %s\n", what);
    exit(1);
}

/* ------------------------------------------------------- 第 1 步：id -> 下标 */

/* case 数据里 predecessors[] 存的是全局 task_id，而 pre_cnt[]/pre_idx[]/duration[] 是按
 * 数组下标索引的，所以需要这张反查表才能从一个前驱 id 拿到它的 duration。 */
static void build_index(const clc_graph_t *g)
{
    for (uint32_t i = 0; i < CLC_MAX_TASKS; i++) {
        g_index_of_id[i] = -1;
    }
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        uint32_t id = g->task_id[i];
        if (id >= CLC_MAX_TASKS) {
            die("task_id 超出 RING_SIZE，需要调大 CLC_MAX_TASKS");
        }
        if (g_index_of_id[id] != -1) {
            die("case 数据里出现重复的 task_id");
        }
        g_index_of_id[id] = (int32_t)i;
    }
}

/* ------------------------------------- 第 2 步：检查输入本身就是拓扑排列的 */

/* 下一步的 est 单趟前向递推要求：case 数组里每个任务的前驱都排在它之前。这在实践上成立
 * （编排是顺序构图的，一个任务只可能依赖已经创建出来的任务；qwen3_14b_decode 的 864 个
 * 任务已逐个验证过），但这是输入的性质而不是保证，所以显式检查 —— 不成立就报错退出，
 * 绝不静默算出一个错的 est。 */
static void check_input_topological(const clc_graph_t *g)
{
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        for (int k = 0; k < g->pre_cnt[i]; k++) {
            uint32_t pid = (uint32_t)g->predecessors[g->pre_idx[i] + k];
            if (pid >= CLC_MAX_TASKS || g_index_of_id[pid] < 0) {
                fprintf(stderr, "[clc_prepare] 任务 %u 的前驱 %u 不在 case 数据里\n",
                        g->task_id[i], pid);
                die("前驱引用了不存在的 task_id");
            }
            if ((uint32_t)g_index_of_id[pid] >= i) {
                fprintf(stderr,
                        "[clc_prepare] 任务 %u (下标 %u) 的前驱 %u 在下标 %d，不在它前面\n",
                        g->task_id[i], i, pid, g_index_of_id[pid]);
                die("case 数组的存放顺序不是拓扑序，est 的单趟递推不适用");
            }
        }
    }
}

/* 排序正确性的另一条前提：权重必须为正，否则 est 沿边不再严格递增，"按 est 排就是拓扑序"
 * 就不成立了。CLC_USE_DURATION=0 时权重是字面量 1 自然满足；=1 时取自 case 数据，必须查。
 * 无论开关如何都检查，因为 duration 还会原样写进 GM 表（duration 是 int，负值转成
 * uint32_t 会变成约 40 亿）。 */
static void check_input_durations(const clc_graph_t *g)
{
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        if (g->duration[i] <= 0) {
            fprintf(stderr, "[clc_prepare] 任务 %u 的 duration=%d，非正\n",
                    g->task_id[i], g->duration[i]);
            die("duration 必须为正：否则 est 沿边不再严格递增，排序不再保证拓扑序");
        }
    }
}

/* --------------------------------------------------------- 第 3 步：算 est */

static void compute_est(const clc_graph_t *g)
{
    memset(g_est, 0, sizeof(g_est));

    /* 靠上一步保证的性质：按 case 数组顺序走一遍，读到某个任务时它的前驱都算完了。 */
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        uint32_t id = g->task_id[i];
        uint64_t best = 0;
        for (int k = 0; k < g->pre_cnt[i]; k++) {
            uint32_t pid = (uint32_t)g->predecessors[g->pre_idx[i] + k];
#if CLC_USE_DURATION
            uint32_t pidx = (uint32_t)g_index_of_id[pid];
            uint64_t w = (uint64_t)g->duration[pidx];
#else
            uint64_t w = 1;   /* 所有权重取 1 -> est 退化成拓扑层号 */
#endif
            uint64_t cand = g_est[pid] + w;
            if (cand > best) {
                best = cand;
            }
        }
        g_est[id] = best;
    }
}

/* ----------------------------------------------------------- 第 4 步：排序 */

/* 按 (est, case 数组下标) 升序。
 *
 * 主键就足够了：对任意一条边 p -> i 有 est[i] >= est[p] + w，而 w 恒为正
 * （check_input_durations 守住），所以严格大于，p 必在 i 前面。
 *
 * 次键的作用其实是**确定性**，不是补救拓扑序：既然 check_input_durations() 保证权重为正，
 * est 沿每条边严格递增，两个 est 相等的任务之间就不可能有边，次键怎么选都不影响拓扑正确性。
 * 但 qsort 不是稳定排序，没有全序的话输出可能随 libc 实现而变 —— 次键让 cmp_by_est 成为
 * 全序，输出可复现。
 * 选 case 数组下标而不是 task_id，是因为 check_input_topological() 验证的恰好是"前驱的数组
 * 下标更小"，而 task_id 之间的大小关系从未被验证过（这份数据里 task_id[i]==i 所以两者一致，
 * 换个 case 就不一定）。万一哪天权重为正的检查被放宽，用已验证的性质当次键仍然兜得住。
 * 无论如何 verify() 会无条件复查一遍输出顺序。 */
static int cmp_by_est(const void *a, const void *b)
{
    uint32_t ia = *(const uint32_t *)a;
    uint32_t ib = *(const uint32_t *)b;

    if (g_est[ia] != g_est[ib]) {
        return g_est[ia] < g_est[ib] ? -1 : 1;
    }
    if (g_index_of_id[ia] != g_index_of_id[ib]) {
        return g_index_of_id[ia] < g_index_of_id[ib] ? -1 : 1;
    }
    return 0;
}

static void sort_tasks(const clc_graph_t *g)
{
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        g_order[i] = g->task_id[i];
    }
    qsort(g_order, g->task_cnt, sizeof(g_order[0]), cmp_by_est);
}

/* ------------------------------------------------------- 第 5 步：填 GM 表 */

/*
 * mix 的去向 —— 改 mix 策略只需要改这一个函数。
 *
 * 同事提的 CLC 方案里，AICore 抓取时只区分 cube / vector，没有定义 mix 任务由谁 claim
 * （一个 mix 任务要同时占一个 cube 和一个 vector，拉模型里没有全局视角，两个核怎么会合
 * 是个未解的协议问题）。所以 type=2 绝不能进 GM：AICore 读到一个看不懂的 type，那 60 个
 * mix 任务就没人抓，而直接或间接依赖它们的有 288 个 —— 合计 348/864（40%）会一起卡死。
 * 宁可在这里折叠，也不要上机挂死。
 *
 * 折进 cube 而不是 vector 是按正确性选的：out_proj 主体是 matmul，而且它的 duration
 * (40750) 贴近 cube 均值 (53361)、远离 vector 均值 (12811)。性能上两种折法实测差 <1%
 * （这个 DAG 被依赖卡住，不被吞吐卡住），所以不用拿性能来纠结。
 *
 * 保持纯函数（计数放在调用处）：verify() 会再调一次做交叉核对，有副作用就会重复计数。
 */
static uint8_t clc_fold_type(uint8_t raw)
{
    return (raw == (uint8_t)CLC_TYPE_MIX) ? (uint8_t)CLC_TYPE_CUBE : raw;
}

static void fill_table(const clc_graph_t *g)
{
    memset(g_clc_table, 0, sizeof(g_clc_table));
    g_folded_mix_cnt = 0;

    for (uint32_t k = 0; k < g->task_cnt; k++) {
        uint32_t id = g_order[k];
        uint32_t i  = (uint32_t)g_index_of_id[id];
        clc_task_t *t = &g_clc_table[k];
        uint8_t raw_type = (uint8_t)g->type[i];

        if (raw_type == (uint8_t)CLC_TYPE_MIX) {
            g_folded_mix_cnt++;
        }

        t->task_id           = id;
        t->duration          = (uint32_t)g->duration[i];
        t->publish_notify_id = id;                       /* 当前约定：counter 与 id 同号 */
        t->type              = clc_fold_type(raw_type);

        int pc = g->pre_cnt[i];
        if (pc < 0 || pc > CLC_MAX_SUBSCRIBE) {
            fprintf(stderr,
                    "[clc_prepare] 任务 %u 有 %d 个前驱，超过 CLC_MAX_SUBSCRIBE=%d\n",
                    id, pc, CLC_MAX_SUBSCRIBE);
            die("订阅列表放不下，需要调大 CLC_MAX_SUBSCRIBE 或改用 CSR 布局");
        }
        t->subscribe_cnt = (uint8_t)pc;
        for (int m = 0; m < pc; m++) {
            t->subscribe_notify_id[m] = (uint32_t)g->predecessors[g->pre_idx[i] + m];
        }
    }
    g_clc_task_cnt = g->task_cnt;
}

/* --------------------------------------------------------- 第 6 步：自检 */

/* 输出的表要满足五条。(1)(2) 是正确性（违反就会挂死或乱序），(3)(4) 是和 AICore 的契约，
 * (5) 拿表和源数据交叉核对。返回失败条数，0 表示全过。 */
static int verify(const clc_graph_t *g)
{
    static int32_t pos_of_id[CLC_MAX_TASKS];
    int fail = 0;

    for (uint32_t i = 0; i < CLC_MAX_TASKS; i++) {
        pos_of_id[i] = -1;
    }

    /* (1) 每个任务恰好出现一次 */
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        uint32_t id = g_clc_table[k].task_id;
        if (id >= CLC_MAX_TASKS) {
            fprintf(g_report, "  FAIL 表内位置 %u 的 task_id %u 越界\n", k, id);
            fail++;
            continue;
        }
        if (pos_of_id[id] >= 0) {
            fprintf(g_report, "  FAIL task_id %u 出现了不止一次（位置 %d 和 %u）\n",
                   id, pos_of_id[id], k);
            fail++;
        }
        pos_of_id[id] = (int32_t)k;
    }
    for (uint32_t i = 0; i < g->task_cnt; i++) {
        if (pos_of_id[g->task_id[i]] < 0) {
            fprintf(g_report, "  FAIL task_id %u 在输出表里缺失\n", g->task_id[i]);
            fail++;
        }
    }

    /* (2) 输出顺序是合法拓扑序 —— 这是不死锁的前提，最重要的一条 */
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        const clc_task_t *t = &g_clc_table[k];
        for (uint8_t m = 0; m < t->subscribe_cnt; m++) {
            uint32_t pid = t->subscribe_notify_id[m];
            if (pid >= CLC_MAX_TASKS || pos_of_id[pid] < 0) {
                fprintf(g_report, "  FAIL 任务 %u 订阅了不存在的 counter %u\n", t->task_id, pid);
                fail++;
            } else if ((uint32_t)pos_of_id[pid] >= k) {
                fprintf(g_report, "  FAIL 任务 %u 在位置 %u，其前驱 %u 却在位置 %d（不是拓扑序）\n",
                       t->task_id, k, pid, pos_of_id[pid]);
                fail++;
            }
        }
    }

    /* (3) GM 表里只能出现 AICore 定义过的 type。谁把 clc_fold_type() 改坏了，这里拦住。*/
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        uint8_t ty = g_clc_table[k].type;
        if (ty != (uint8_t)CLC_TYPE_VECTOR && ty != (uint8_t)CLC_TYPE_CUBE) {
            fprintf(g_report, "  FAIL 任务 %u 的 type=%u 未定义，AICore 不会抓它\n",
                   g_clc_table[k].task_id, ty);
            fail++;
        }
    }

    /* (4) 当前约定 publish counter 与 task_id 同号 */
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        if (g_clc_table[k].publish_notify_id != g_clc_table[k].task_id) {
            fprintf(g_report, "  FAIL 任务 %u 的 publish_notify_id=%u，与 task_id 不同号\n",
                   g_clc_table[k].task_id, g_clc_table[k].publish_notify_id);
            fail++;
        }
    }

    /* (5) 表内每一项确实来自它 task_id 对应的那条源数据。
     *
     * 上面四条只拿表和表自己比对，所以 fill_table() 如果取错了下标（比如误写
     * g->duration[k] 而不是 g->duration[i]）它们全都会通过 —— 而 task_id / case 数组下标
     * / 表内位置这三个索引空间搞混，正是这个文件最容易犯的错。这一条拿源数据交叉核对，
     * 把"我手工核对过下标"变成机器每次都查。 */
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        const clc_task_t *t = &g_clc_table[k];
        if (t->task_id >= CLC_MAX_TASKS || g_index_of_id[t->task_id] < 0) {
            continue;   /* 表里出现了源数据里没有的 task_id。实际不可能（g_order 就是从
                         * g->task_id 建出来的），而且 (1) 只报越界/重复/缺失三种、未必覆盖
                         * 它 —— 但这里没有源数据可比对，只能跳过。*/
        }
        uint32_t i = (uint32_t)g_index_of_id[t->task_id];

        if (t->duration != (uint32_t)g->duration[i]) {
            fprintf(g_report, "  FAIL 任务 %u 的 duration=%u，源数据是 %d\n",
                   t->task_id, t->duration, g->duration[i]);
            fail++;
        }
        if (t->type != clc_fold_type((uint8_t)g->type[i])) {
            fprintf(g_report, "  FAIL 任务 %u 的 type=%u，源数据折叠后应为 %u\n",
                   t->task_id, t->type, clc_fold_type((uint8_t)g->type[i]));
            fail++;
        }
        if ((int)t->subscribe_cnt != g->pre_cnt[i]) {
            fprintf(g_report, "  FAIL 任务 %u 的 subscribe_cnt=%u，源数据 pre_cnt 是 %d\n",
                   t->task_id, t->subscribe_cnt, g->pre_cnt[i]);
            fail++;
            continue;   /* 条数都对不上，逐项比没有意义 */
        }
        for (uint8_t m = 0; m < t->subscribe_cnt; m++) {
            uint32_t want = (uint32_t)g->predecessors[g->pre_idx[i] + m];
            if (t->subscribe_notify_id[m] != want) {
                fprintf(g_report, "  FAIL 任务 %u 的第 %u 个订阅是 %u，源数据是 %u\n",
                       t->task_id, m, t->subscribe_notify_id[m], want);
                fail++;
            }
        }
    }

    return fail;
}

/* --------------------------------------------------------- 第 7 步：写 GM */

/*
 * 这里是整个文件唯一与硬件耦合的地方，目前故意空着 —— GM 的基址、映射方式、写入手段
 * （memcpy? DMA? 逐字段写寄存器?）都还没有定义。
 *
 * 定义下来之后，这个函数应该就是把 g_clc_table 的前 g_clc_task_cnt 项原样搬过去：
 * clc_task_t 的布局已由 _Static_assert 钉死，AICore 侧 include 同一个头文件，两边字节级
 * 一致，不需要任何序列化。
 *
 * 只在自检全过时才调用（见 main）：自检失败意味着这张表已知不是拓扑序，写上去就是把
 * "这个排序本来要防的那种死锁"直接送进硬件。
 *
 * 还没定的两件事会影响这里的形态：
 *   - 表头放哪儿（任务总数、起始偏移）？AICore 得先知道表有多长。
 *   - 抓取游标是一个全局的，还是按 type 各一个？如果是后者，需要额外给出每种 type 的任务
 *     序列。好消息是不必重排：把这张表按 type 过滤出来的子序列，天然保持原有的相对顺序，
 *     仍然是各自的合法拓扑序。
 */
static void clc_write_to_gm(void)
{
    fprintf(g_report, "[clc_prepare] 写 GM：未实现（GM 布局待定，见 clc_write_to_gm() 注释）\n");
}

/* ----------------------------------------------------------------- 报告 */

static void report(void)
{
    uint32_t per_type[3] = {0, 0, 0};
    uint32_t max_sub = 0;
    uint64_t span = 0;   /* CLC_USE_DURATION=1 时是关键路径长度，=0 时是层数 */

    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        const clc_task_t *t = &g_clc_table[k];
        if (t->type < 3) {
            per_type[t->type]++;
        }
        if (t->subscribe_cnt > max_sub) {
            max_sub = t->subscribe_cnt;
        }
        /* 必须跟着排序键走。CLC_USE_DURATION=0 时 g_est 装的是层号，加上真实 duration
         * 会得到一个既不是层数也不是时间的数字。 */
#if CLC_USE_DURATION
        uint64_t finish = g_est[t->task_id] + t->duration;
#else
        uint64_t finish = g_est[t->task_id] + 1;   /* 层号从 0 起，+1 才是层数 */
#endif
        if (finish > span) {
            span = finish;
        }
    }

    fprintf(g_report, "\n[clc_prepare] case              = %s\n", CLC_CASE_HDR);
    fprintf(g_report, "[clc_prepare] 任务数            = %u\n", g_clc_task_cnt);
#if CLC_USE_DURATION
    fprintf(g_report, "[clc_prepare] 排序键            = est（最早可开始时刻，使用 duration）\n");
    fprintf(g_report, "[clc_prepare] 关键路径          = %" PRIu64 " ns（任何调度都快不过它）\n", span);
#else
    fprintf(g_report, "[clc_prepare] 排序键            = 拓扑层号（所有 duration 视为 1）\n");
    fprintf(g_report, "[clc_prepare] 层数              = %" PRIu64 "\n", span);
#endif
    fprintf(g_report, "[clc_prepare] vector / cube     = %u / %u\n", per_type[0], per_type[1]);
    fprintf(g_report, "[clc_prepare] 折成 cube 的 mix  = %u\n", g_folded_mix_cnt);
    fprintf(g_report, "[clc_prepare] 最大订阅数        = %u（上限 %d）\n", max_sub, CLC_MAX_SUBSCRIBE);
    fprintf(g_report, "[clc_prepare] 表占用            = %zu 字节（%u 项 x %zu）\n",
           (size_t)g_clc_task_cnt * sizeof(clc_task_t),
           g_clc_task_cnt, sizeof(clc_task_t));
}

static void dump_csv(void)
{
#if CLC_USE_DURATION
    printf("pos,task_id,type,duration,est,publish,sub_cnt,subscribe\n");
#else
    printf("pos,task_id,type,duration,level,publish,sub_cnt,subscribe\n");
#endif
    for (uint32_t k = 0; k < g_clc_task_cnt; k++) {
        const clc_task_t *t = &g_clc_table[k];
        printf("%u,%u,%u,%u,%" PRIu64 ",%u,%u,",
               k, t->task_id, t->type, t->duration,
               g_est[t->task_id], t->publish_notify_id, t->subscribe_cnt);
        putchar('"');
        for (uint8_t m = 0; m < t->subscribe_cnt; m++) {
            printf("%s%u", m ? " " : "", t->subscribe_notify_id[m]);
        }
        putchar('"');
        putchar('\n');
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
    int want_dump = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump") == 0) {
            want_dump = 1;
        }
    }

    g_report = want_dump ? stderr : stdout;

    const clc_graph_t *g = &test_graph[0];

    if (g->task_cnt == 0) {
        die("case 里一个任务都没有");
    }
    if (total_task_cnt < 0 || g->task_cnt != (uint32_t)total_task_cnt) {
        fprintf(stderr,
                "[clc_prepare] test_graph[0].task_cnt=%u 与 total_task_cnt=%d 不符\n",
                g->task_cnt, total_task_cnt);
        die("case 头文件内部不自洽");
    }
    if (g->task_cnt > CLC_MAX_TASKS) {
        die("任务数超过 CLC_MAX_TASKS");
    }

    build_index(g);
    check_input_topological(g);
    check_input_durations(g);
    compute_est(g);
    sort_tasks(g);
    fill_table(g);

    fprintf(g_report, "[clc_prepare] 自检：\n");
    int fail = verify(g);
    if (fail == 0) {
        fprintf(g_report, "  全部通过（唯一性 / 拓扑序 / type 合法 / publish 同号 / 与源数据一致）\n");
    } else {
        fprintf(g_report, "  %d 项失败\n", fail);
    }

    report();

    if (fail == 0) {
        clc_write_to_gm();
    } else {
        fprintf(g_report, "[clc_prepare] 自检未通过，跳过写 GM\n");
    }

    /* 没有分隔空行：--dump 时上面的报告已经走了 stderr，stdout 的第一个字节就是 CSV 表头。 */
    if (want_dump) {
        dump_csv();
    }
    return fail == 0 ? 0 : 1;
}
