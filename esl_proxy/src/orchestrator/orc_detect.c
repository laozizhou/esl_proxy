/* Thin wrapper: detect case header → callable for orchestrator.c. */

#include "tensormap.h"
#include "log.h"
#include "qwen3_14b_decoder_detect.h"

int orc_detect_call(uint64_t orch_args, int thread_id, int total_task_cnt,
    int *created_cnt)
{
    return orchestrator_detect(orch_args, thread_id, total_task_cnt, created_cnt);
}
