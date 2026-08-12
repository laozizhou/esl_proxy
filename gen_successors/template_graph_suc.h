#ifndef SCHEDULER_GRAPH_H
#define SCHEDULER_GRAPH_H

#include <stdint.h>

#include "scheduler/conf.h"

static uint32_t total_task_cnt = 16;

static uint32_t task_id_1[8] = {0, 1, 2, 3, 4, 5, 6, 7};
static bool type_1[8] = {0, 1, 0, 1, 0, 1, 0, 1};
static int duration_1[8] = {5, 5, 5, 5, 5, 5, 5, 5};
static int pre_cnt_1[8] = {0, 1, 1, 2, 2, 2, 2, 2};
static int pre_idx_1[8] = {0, 0, 1, 2, 4, 6, 8, 10};
static int predecessors_1[] = {0, 0, 1, 2, 1, 2, 3, 4, 3, 4, 5, 6};
static int suc_cnt_1[8] = {2, 2, 3, 3, 2, 1, 1, 0};
static int suc_idx_1[8] = {0, 2, 4, 7, 10, 12, 13, 14};
static int successors_1[] = {1, 2, 3, 4, 3, 4, 12, 5, 6, 14, 5, 6, 7, 7};

static uint32_t task_id_2[8] = {8, 9, 10, 11, 12, 13, 14, 15};
static bool type_2[8] = {0, 1, 0, 1, 0, 1, 0, 1};
static int duration_2[8] = {5, 5, 5, 5, 5, 5, 5, 5};
static int pre_cnt_2[8] = {0, 1, 1, 2, 2, 2, 2, 2};
static int pre_idx_2[8] = {0, 0, 1, 2, 4, 6, 8, 10};
static int predecessors_2[] = {8, 8, 9, 10, 9, 2, 11, 12, 3, 12, 13, 14};
static int suc_cnt_2[8] = {2, 2, 1, 1, 2, 1, 1, 0};
static int suc_idx_2[8] = {0, 2, 4, 5, 6, 8, 9, 10};
static int successors_2[] = {9, 10, 11, 12, 11, 13, 13, 14, 15, 15};

typedef struct subgraph {
    uint32_t task_cnt;
    uint32_t* task_id;
    bool* type;
    int* duration;
    int* pre_cnt;
    int* pre_idx;
    int* predecessors;
    int* suc_cnt;
    int* suc_idx;
    int* successors;
} subgraph;

static subgraph test_graph[PAINTER_THREAD_CNT] = {
    {8, task_id_1, type_1, duration_1, pre_cnt_1, pre_idx_1, predecessors_1, suc_cnt_1, suc_idx_1, successors_1},
    {8, task_id_2, type_2, duration_2, pre_cnt_2, pre_idx_2, predecessors_2, suc_cnt_2, suc_idx_2, successors_2}
};

#endif