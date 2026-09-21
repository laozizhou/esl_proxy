/*
 * clc_task.h - CLC (dv121) 任务表的条目布局
 *
 * CLC 把调度的控制权从 AICPU 翻转给 AICore：AICPU 不再逐个下发任务，而是在初始化时
 * 一次性把全部任务排好序、写进 GM，之后退场；AICore 自己从这张表里抓取任务、自己解依赖。
 *
 * 因此这个头文件是 AICPU 侧和 AICore 侧的**共享契约**，两边必须字节级一致。改动它等于
 * 改动 ABI —— 下面的 _Static_assert 就是拿来钉这件事的。不要把它拆进某个 .c 里，
 * 否则 AICore 那边只能手抄一份定义，早晚对不上。
 *
 * 依赖的表达方式（notify counter 模型）：
 *   每个 task_id 对应一个同号的硬件计数器 cnt_<task_id>。
 *     - 任务跑完  -> 给自己的 publish_notify_id 发信号
 *     - 任务想跑  -> 先确认 subscribe_notify_id[0 .. subscribe_cnt) 全部已被发过信号
 */

#ifndef CLC_TASK_H
#define CLC_TASK_H

#include <stddef.h>
#include <stdint.h>

/* 一个任务最多能订阅多少个前驱。
 * qwen3_14b_decode spmd_tier=2 实测最大前驱数为 16（分布：0个×6, 1个×342, 2个×294,
 * 3个×90, 7个×6, 9个×60, 10个×16, 16个×50）。换 workload 要重新核这个上限，
 * clc_prepare.c 在超限时会直接报错退出，不会静默截断。*/
#define CLC_MAX_SUBSCRIBE 16

/*
 * 任务类型 —— 决定"哪种核能接这个任务"，是 AICore 抓取时的第一道判断。
 *
 * 编码故意照抄 cases 下各头文件的约定（注释写的是 aiv=0, aic=1, mix=2，并且 duration 统计
 * 独立佐证：type=1 均值 53us 远重于 type=0 的 12.8us，符合 matmul vs elementwise）。
 *
 * **不要 include common/task.h 复用那边的枚举** —— 那里是 TASK_TYPE_CUBE=0 /
 * TASK_TYPE_VECTOR=1，和 case 数据正好相反。老调度器没因此出事，只是因为仿真里两种核
 * 数量对称、标签反了也看不出来；CLC 里 type 决定核能不能接活，反一位就是每个核都在抓
 * 错任务。
 */
typedef enum {
    CLC_TYPE_VECTOR = 0,   /* aiv */
    CLC_TYPE_CUBE   = 1,   /* aic */
    CLC_TYPE_MIX    = 2,   /* 需要同时占 cube + vector。AICore 侧尚未定义如何抓取，
                            * 因此**绝不会出现在 GM 表里** —— clc_prepare.c 在写表前
                            * 用 clc_fold_type() 折成 CUBE，并有断言兜底。*/
} clc_type_t;

/*
 * GM 里的一条任务记录。
 *
 * 定长布局，不是 CSR。CSR 更省（22KB vs 66KB），但拉模型里 AICore 每抓一个任务都要
 * 自己算地址：定长是一次乘法，CSR 要两次访存。取数开销在拉模型里是新增成本、由每个核
 * 的每次抓取承担，所以这里用空间换。
 *
 * 注意：**数组下标 != task_id**。按 est 排序之后，表里第 k 项的 task_id 一般不等于 k。
 * 而 counter 是按 task_id 编号的，和它在表里排第几毫无关系。
 */
typedef struct {
    uint32_t task_id;                                 /* 全局任务 id，不是表内下标 */
    uint32_t duration;                                /* 估算执行时长 (ns)，来自 case 数据 */
    uint32_t publish_notify_id;                       /* 跑完后给哪个 counter 发信号 */
    uint32_t subscribe_notify_id[CLC_MAX_SUBSCRIBE];  /* 要等哪些 counter（= 各前驱的 id）*/
    uint8_t  subscribe_cnt;                           /* 上面数组里有效的项数 */
    uint8_t  type;                                    /* clc_type_t，表里只会是 VECTOR/CUBE */
    uint8_t  reserved[2];                             /* 显式补齐，别让编译器悄悄决定 */
} clc_task_t;

/* publish_notify_id 目前恒等于 task_id，看起来冗余。仍然显式存着，是因为如果将来
 * counter 是稀缺硬件资源、需要在任务之间复用，这个等式就不成立了 —— 现在留个字段
 * 比以后改 ABI 便宜。而 type=2 不一样：那是对方**没有定义**的值，冗余可以留，
 * 未定义不能留。 */

/* 布局是 AICPU/AICore 共享的 ABI，两边编译器可能不同，所以把总大小和每个字段的偏移都钉死。
 * 只钉总大小挡不住"字段重排但大小不变"这种改动 —— 那种改动两边各编各的，不会有任何报错，
 * 直到 AICore 把 duration 当成 task_id 用。 */
_Static_assert(sizeof(clc_task_t) == 80,
               "clc_task_t 大小变了，AICPU/AICore 两侧就对不上");
_Static_assert(offsetof(clc_task_t, task_id)             ==  0, "ABI: task_id 偏移");
_Static_assert(offsetof(clc_task_t, duration)            ==  4, "ABI: duration 偏移");
_Static_assert(offsetof(clc_task_t, publish_notify_id)   ==  8, "ABI: publish 偏移");
_Static_assert(offsetof(clc_task_t, subscribe_notify_id) == 12, "ABI: subscribe 偏移");
_Static_assert(offsetof(clc_task_t, subscribe_cnt)       == 76, "ABI: subscribe_cnt 偏移");
_Static_assert(offsetof(clc_task_t, type)                == 77, "ABI: type 偏移");

_Static_assert(CLC_MAX_SUBSCRIBE <= 255,
               "subscribe_cnt 是 uint8_t");

#endif /* CLC_TASK_H */
