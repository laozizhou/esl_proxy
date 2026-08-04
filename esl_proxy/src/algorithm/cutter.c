#include "cutter.h"
#include "log.h"
#include "ring_buf.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

task_state* g_state_buf;

void init_state_buf(void) {
    g_state_buf = malloc(sizeof(task_state) * RING_SIZE);
    for (size_t i = 0; i < RING_SIZE; i++) {
        g_state_buf[i].state = TASK_STATUS_CREATING;
        g_state_buf[i].task_id = 0;
        g_state_buf[i].successor_cnt = 0;
    }
}

extern atomic_int g_min_uncomplete_task;
extern ctrl_t g_ctrl_t[DISPATCH_THREAD_CNT];
extern _Atomic bool g_orch_is_done;
extern _Atomic bool g_is_done;

uint32_t  g_predecessor_cnt[RING_SIZE];
uint32_t g_commit_task_id = 0;
uint32_t g_completed_task_cnt = 0;
uint64_t g_add_successors_ns = 0;

static inline bool update_task_state(uint32_t cnt, uint32_t* cq_buf)
{
    if (cnt <= 0)
        return false;
    
    uint32_t task_id;
    uint32_t idx;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        int idx = task_id;
        g_state_buf[idx].state = TASK_STATUS_COMPLETED;
    }
    uint32_t i = atomic_load_explicit(&g_min_uncomplete_task, memory_order_acquire);
    uint32_t end = atomic_load_explicit(&g_task_id, memory_order_acquire);
    for (; i < end; i++) {
        if (g_state_buf[i].state != TASK_STATUS_COMPLETED) {
            break;
        }
    }
    atomic_store(&g_min_uncomplete_task, i);
    WORKER_LOGF("min_uncomplete_task,%u, completed_cnt,%u, cube_ready_cnt,%d,vector_ready_cnt,%d", \
        g_min_uncomplete_task, end, g_ctrl_t[0].ready_queue[2].cnt, g_ctrl_t[0].ready_queue[1].cnt);
}

void add_successors(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]) {
    uint32_t end = atomic_load(&g_task_id);
    uint32_t tmp = g_commit_task_id + PRE_BATCH_SIZE;
    end = tmp > end ? end : tmp;
    while ( g_commit_task_id < end)
    {
        uint32_t task_idx = g_commit_task_id;
        struct predecessor_list *ptr = &g_predecessors[task_idx];
        if (ptr->cnt <= 0) {
            // WORKER_LOGF("ready, task_id,%u, task_idx,%u, ready_cnt,%u", g_commit_task_id, task_idx, *ready_cnt);
            task_type_t type = g_basic_buf[g_commit_task_id].type;
            rq_buf[type][ready_cnt[type]] = g_commit_task_id++;
            ready_cnt[type]++;
            WORKER_LOGF("ready_cnt[%d],%d",type, ready_cnt[type]);
            continue;
        }
        uint32_t precessor_id = 0;
        uint32_t predecessor_cnt = 0;
        while (ptr->cnt > 0)
        {
            precessor_id = *(ptr->exp);
            uint32_t precessor_idx = precessor_id;
            if(g_state_buf[precessor_idx].state != TASK_STATUS_COMPLETED) {
                uint32_t successor_idx = g_successor_buf[precessor_idx].cnt++;
                g_successor_buf[precessor_idx].node[successor_idx] = g_commit_task_id;
                g_state_buf[precessor_idx].successor_cnt++;
                predecessor_cnt++;
                WORKER_LOGF("add, task_id,%u, successor_cnt,%u, successor_id, %u", precessor_id, g_successor_buf[precessor_idx].cnt, g_commit_task_id);
            }
            ptr->cnt--;
            ptr->exp++;
        }
        g_predecessor_cnt[task_idx] = predecessor_cnt;
        if (predecessor_cnt <= 0)
        {
            task_type_t type = g_basic_buf[g_commit_task_id].type;
            rq_buf[type][ready_cnt[type]] = g_commit_task_id;
            ready_cnt[type]++;
            WORKER_LOGF("ready_cnt[%d],%d",type, ready_cnt[type]);
        }
        g_commit_task_id++;
    }
}

void send_2_ready_queue(uint32_t ready_cnt[], uint32_t rq_buf[][RQ_BATCH_SIZE]) {
    for (uint32_t j = 0; j < 2; j++) {
        int target_ctrl = 0;
        queue_t *rq = &g_ctrl_t[target_ctrl].ready_queue[j];
        if (ready_cnt[j] > 0)
        {
            WORKER_LOGF("batch_enqueue,%d,cnt,%u,first,%d",j, ready_cnt[j], rq_buf[j][0]);
            batch_enqueue(rq, rq_buf[j], ready_cnt[j]);
        }
    }
}

void resolve_dep(uint32_t cnt, uint32_t* cq_buf, uint32_t rq_buf[][RQ_BATCH_SIZE], uint32_t* ready_cnt) {
    uint32_t task_id;
    uint32_t succ_id;
    uint32_t idx;
    uint32_t succ_cnt;
    for (uint32_t j = 0; j < cnt; j++) {
        task_id = cq_buf[j];
        idx = task_id & RING_MASK;
        task_state st = g_state_buf[idx];
        succ_cnt = (uint32_t)st.successor_cnt;
        WORKER_LOGF("completed,task_id,%u,type,%u, successor_cnt,%u", task_id, g_basic_buf[idx].type, succ_cnt);
        for (uint32_t k = 0; k < succ_cnt; k++) {
            succ_id = g_successor_buf[idx].node[k];
            g_predecessor_cnt[succ_id & RING_MASK]--;
            WORKER_LOGF("cutter, task_id,%u, successor_id,%u, predecessor_cnt,%u", task_id, succ_id, g_predecessor_cnt[succ_id & RING_MASK]);
            if (g_predecessor_cnt[succ_id & RING_MASK] < 1) {
                task_type_t type = g_basic_buf[succ_id].type;
                rq_buf[type][ready_cnt[type]] = succ_id;
                ready_cnt[type]++;
                WORKER_LOGF("ready_cnt[%d],%d",type, ready_cnt[type]);
            }
        }
    }
}

void deal_completed_queue() {
    for (int i = 0; i < DISPATCH_THREAD_CNT; i++) {
        uint32_t cq_buf[CQ_BATCH_SIZE];
        uint32_t rq_buf[2][RQ_BATCH_SIZE];
        uint32_t ready_cnt[2] = {0, 0};
        queue_t *cq = &g_ctrl_t[i].completed_queue;
        uint32_t cnt = CQ_BATCH_SIZE;
        batch_dequeue(cq, cq_buf, &cnt);
        g_completed_task_cnt += cnt;
        // for (size_t i = 0; i < cnt; i++)
        // {
        //     WORKER_LOGF("cutter, completed_task_id,%d ", cq_buf[i]);
        // }
        update_task_state(cnt, cq_buf);
        uint64_t as_start_ns = get_time_ns();
        add_successors(ready_cnt, rq_buf);
        g_add_successors_ns += get_time_ns() - as_start_ns;
        resolve_dep(cnt, cq_buf, rq_buf, ready_cnt);
        send_2_ready_queue(ready_cnt, rq_buf);
    }
}

void *cutter_worker(void *arg)
{
    int tid = (int)(intptr_t)arg;
    uint64_t start_ns = get_time_ns();
    uint64_t first_task_ns = 0;
    uint64_t orch_done_ns = 0;
    uint64_t idle_loop_iters = 0;
    uint64_t idle_busy_iters = 0;
    uint64_t active_loop_iters = 0;
    uint64_t active_busy_iters = 0;
    uint64_t drain_loop_iters = 0;
    uint64_t drain_busy_iters = 0;
    init_state_buf();
    while (!atomic_load(&g_is_done)) {
        /* This loop's own exit condition is g_is_done (dispatch's finish
         * signal, set only after dispatch's own drain loop completes), which
         * is later than g_orch_is_done -- so it spans all three timing
         * phases (idle/active/drain), not just active. Bucket each iteration
         * by which phase was still in progress at the START of the
         * iteration, matching the idle_ns/active_ns/drain_ns boundaries
         * below; the "drain" bucket here is combined with the second loop's
         * counters since both fall in the same drain_ns window. */
        bool before_first_task = (first_task_ns == 0);
        if (before_first_task && atomic_load(&g_task_id) > 0) {
            first_task_ns = get_time_ns();
        }
        bool before_orch_done = (orch_done_ns == 0);
        if (before_orch_done && atomic_load(&g_orch_is_done)) {
            orch_done_ns = get_time_ns();
        }
        uint32_t commit_before = g_commit_task_id;
        uint32_t completed_before = g_completed_task_cnt;
        deal_completed_queue();
        bool busy = (g_commit_task_id != commit_before || g_completed_task_cnt != completed_before);
        if (before_first_task) {
            idle_loop_iters++;
            if (busy) idle_busy_iters++;
        } else if (before_orch_done) {
            active_loop_iters++;
            if (busy) active_busy_iters++;
        } else {
            drain_loop_iters++;
            if (busy) drain_busy_iters++;
        }
    }
    if (orch_done_ns == 0) {
        orch_done_ns = get_time_ns();
    }
    if (first_task_ns == 0) {
        first_task_ns = orch_done_ns;
    }

    while(g_commit_task_id < atomic_load(&g_task_id)){
        uint32_t commit_before = g_commit_task_id;
        uint32_t completed_before = g_completed_task_cnt;
        deal_completed_queue();
        drain_loop_iters++;
        if (g_commit_task_id != commit_before || g_completed_task_cnt != completed_before) {
            drain_busy_iters++;
        }
    }
    uint64_t end_ns = get_time_ns();
    uint64_t elapsed_ns = end_ns - start_ns;

    WORKER_LOGF("cutter, commit_tasks_cnt,%d,completed_task_cnt,%d ", g_commit_task_id, g_completed_task_cnt);
    MAIN_LOGF("[cutter] task_cnt = %u", g_completed_task_cnt);
    MAIN_LOGF("[cutter] duration = %llu ns", (unsigned long long)elapsed_ns);
    MAIN_LOGF("[cutter] task_tp = %f MTasks/s", (float)(g_completed_task_cnt * 1000.0 / elapsed_ns));
    MAIN_LOGF("[cutter] idle_ns = %llu ns", (unsigned long long)(first_task_ns - start_ns));
    MAIN_LOGF("[cutter] active_ns = %llu ns", (unsigned long long)(orch_done_ns - first_task_ns));
    MAIN_LOGF("[cutter] drain_ns = %llu ns", (unsigned long long)(end_ns - orch_done_ns));
    MAIN_LOGF("[cutter] idle_loop_iters = %llu", (unsigned long long)idle_loop_iters);
    MAIN_LOGF("[cutter] idle_busy_iters = %llu", (unsigned long long)idle_busy_iters);
    MAIN_LOGF("[cutter] active_loop_iters = %llu", (unsigned long long)active_loop_iters);
    MAIN_LOGF("[cutter] active_busy_iters = %llu", (unsigned long long)active_busy_iters);
    MAIN_LOGF("[cutter] drain_loop_iters = %llu", (unsigned long long)drain_loop_iters);
    MAIN_LOGF("[cutter] drain_busy_iters = %llu", (unsigned long long)drain_busy_iters);
    MAIN_LOGF("[cutter] add_successors_ns = %llu ns", (unsigned long long)g_add_successors_ns);
    MAIN_LOGF("[cutter] add_successors_share = %f", (float)(g_add_successors_ns / (double)elapsed_ns));
    return NULL;
}