/*
 * early_dispatch.h - Early-dispatch (ed) 一期基础设施
 *
 * Step 1 范围：generation-tagged record、per-task 元数据、边锁、slot_state 配套类型。
 * Hook 0/1/2 业务逻辑在 Step 4–7 接入；本头文件集中声明 ed 全局与 helper API。
 */

#ifndef ALGORITHM_EARLY_DISPATCH_H
#define ALGORITHM_EARLY_DISPATCH_H

#include <stdint.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "conf.h"
#include "queue.h"

#ifndef ED_HOOK0_CONTRIB_STATS
#define ED_HOOK0_CONTRIB_STATS 0
#endif

/* -------------------------------------------------------------------------
 * spec_state：s-task 在 ed 流水线中的投机状态（Hook 0/1/2 使用，Step 1 仅初始化）
 * ------------------------------------------------------------------------- */
typedef enum {
    ED_SPEC_NONE       = 0,  /* 未进入 ed */
    ED_SPEC_STAGING    = 1,  /* fanin 齐，等待 Hook 1 stage */
    ED_SPEC_DISPATCHED = 2,  /* 依赖已满足，已 release（可能已敲 doorbell） */
} ed_spec_state_t;

/* notify_once 调用来源，用于日志区分 Hook 1 自敲 vs Hook 2 释放 */
typedef enum {
    ED_NOTIFY_HOOK1 = 0,
    ED_NOTIFY_HOOK2 = 1,
} ed_notify_source_t;

/* -------------------------------------------------------------------------
 * generation-tagged 64-bit record 编码（设计方案 §5.13）
 *
 * 高 32 位：完整 task_id（generation tag，防 ring slot 复用 ABA）
 * 低 32 位：packed slot = (type<<24)|(slot<<16)|core
 * ------------------------------------------------------------------------- */
#define ED_RECORD_INVALID       (0xFFFFFFFFFFFFFFFFull)
#define ED_TASK_TAG_INVALID     (0xFFFFFFFFu)
#define ED_STAGED_INVALID       (0xFFFFFFFFu)

#define ED_PACK_RECORD(tag, slot_packed) \
    (((uint64_t)(tag) << 32) | (uint64_t)(slot_packed))
#define ED_RECORD_TAG(rec)      ((uint32_t)((uint64_t)(rec) >> 32))
#define ED_RECORD_SLOT(rec)     ((uint32_t)(rec))

#define ED_PACK_SLOT(core, slot, type) \
    (((uint32_t)(type) << 24) | ((uint32_t)(slot) << 16) | (uint32_t)(core))
#define ED_UNPACK_CORE(p)       ((uint16_t)((p) & 0xFFFFu))
#define ED_UNPACK_SLOT(p)       ((uint8_t)(((p) >> 16) & 0xFFu))
#define ED_UNPACK_TYPE(p)       ((uint8_t)(((p) >> 24) & 0xFFu))
#define ED_STAGED_CORE_INVALID  ((uint16_t)0xFFFFu)

/* -------------------------------------------------------------------------
 * Per-task ed 元数据（下标 task_id & RING_MASK）
 * ------------------------------------------------------------------------- */

/* Hook 0：已 dispatch 的前驱计数；target 在 commit 时固化 */
extern _Atomic uint16_t  g_dispatch_fanin[RING_SIZE];
extern uint16_t          g_dispatch_fanin_target[RING_SIZE];

/* resolve_dep：未完成前驱数；仅 unfin 1→0 时可 release */
extern _Atomic uint16_t  g_unfin_pred_cnt[RING_SIZE];

/* s-task 投机状态机 */
extern _Atomic uint8_t   g_spec_state[RING_SIZE];

/*
 * g_staged_slot_record：Hook 1 stage 后写入的 (core,slot,type) + tag；
 * Hook 2 seq_cst 读取，必须校验 tag 防 ring 复用误敲 doorbell。
 */
extern _Atomic uint64_t  g_staged_slot_record[RING_SIZE];

/*
 * g_notify_claimed：Hook 1/2 竞争唯一通知权；仅 CAS 0→1 胜者可写 doorbell。
 */
extern _Atomic uint8_t   g_notify_claimed[RING_SIZE];

/*
 * g_next_block_idx：块级进度指针；normal send_task 与 Hook 1 stager 共用 CAS 认领。
 * 一期 count==1 时取值 {0,1}。
 */
extern _Atomic uint16_t  g_next_block_idx[RING_SIZE];

/*
 * g_task_dispatch_record：瞬时 dispatch 位置（完成时按 tag 清除）；
 * pick_stage_core 查前驱 core 时使用。
 */
extern _Atomic uint64_t  g_task_dispatch_record[RING_SIZE];

/*
 * g_ring_task_tag：当前占用该 ring slot 的完整 task_id；
 * Hook 0 / add_successors 第二趟校验 stale generation。
 */
extern _Atomic uint32_t  g_ring_task_tag[RING_SIZE];

/*
 * g_dispatch_tag：持久标记「该代 task 曾 dispatch」；完成时不清，
 * add_successors late-arrival 靠它判断 pred 是否已 dispatch。
 */
extern _Atomic uint32_t  g_dispatch_tag[RING_SIZE];

/* per-pred 边锁：线性化 Hook 0 遍历与 add_successors append */
extern atomic_flag       g_ed_edge_lock[RING_SIZE];

/* add_successors 快照的前驱 task_id 列表，供 pick_stage_core 查 dispatch record */
typedef struct {
    uint16_t cnt;
    uint16_t node[CON_NODE_CNT];
} ed_pred_snapshot_t;
extern ed_pred_snapshot_t g_ed_pred_snapshot[RING_SIZE];

/* Hook 1 消费者队列：STAGING 状态的 s-task 等待 try_early_dispatch */
extern queue_t g_ed_ready_queue;

/* -------------------------------------------------------------------------
 * ed metrics（运行结束时 main 打印；relaxed 更新即可）
 * ------------------------------------------------------------------------- */
extern _Atomic uint64_t g_ed_stage_cnt;
extern _Atomic uint64_t g_ed_hit_cnt;
extern _Atomic uint64_t g_ed_self_notify_cnt;
extern _Atomic uint64_t g_ed_slot_retry_cnt;
extern _Atomic uint64_t g_ed_block_cas_fail_cnt;
extern _Atomic uint64_t g_ed_send_skip_cnt;
extern _Atomic uint64_t g_ed_late_arrival_cnt;

#if ED_HOOK0_CONTRIB_STATS
extern _Atomic uint64_t g_ed_hook0_contrib_cnt;
#endif

/* -------------------------------------------------------------------------
 * 生命周期 API
 * ------------------------------------------------------------------------- */

/* 启动时调用（init_ctrl_t 之后）：全表 INVALID + metrics 清零 */
void ed_init(void);

/*
 * ring slot 换代时重置该 task 的全部 ed 元数据（add_successors 第一趟结束后调用）。
 * predecessor_cnt 来自 survivors 扫描，固化为 g_dispatch_fanin_target。
 */
void ed_init_task_meta(uint32_t full_task_id, uint16_t predecessor_cnt);

void ed_edge_lock(uint16_t task_idx);
void ed_edge_unlock(uint16_t task_idx);

/* generation-tagged dispatch 位置 helper */
uint64_t ed_task_dispatch_record_load(uint32_t task_id);
void     ed_task_dispatch_record_store(uint32_t task_id, int core, int slot, int type);
void     ed_task_dispatch_record_clear(uint32_t task_id);

/* tag 校验：record 高 32 位须与 task_id 相等，否则视为 stale/无效 */
bool ed_record_tag_matches(uint64_t record, uint32_t task_id);

/* Step 6 接入；Step 1 提供空 stub */
void ed_notify_once(uint32_t task_id, uint64_t record, ed_notify_source_t source);

#if ED_ENABLE
void propagate_dispatch_fanin(uint16_t p_id);
int  try_early_dispatch(int tid);
#endif

/* 提取 bitmap 中第 nth 个置位（0-indexed），供 pick_stage_core 随机选核 */
static inline int pick_nth_bit(uint64_t bitmap, int nth)
{
    for (int i = 0; i < nth; i++) {
        bitmap &= (bitmap - 1);
    }
    return __builtin_ctzll(bitmap);
}

#endif /* ALGORITHM_EARLY_DISPATCH_H */
