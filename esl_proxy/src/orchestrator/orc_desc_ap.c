/* Thin wrapper: includes only the desc_ap case header and exposes a
 * callable function for use by orchestrator.c. */

#include "tensormap.h"
#include "log.h"
#include "qwen3_14b_decoder_desc_ap.h"

int orc_desc_call(uint64_t orch_args, int thread_id, int *created_cnt)
{
    return orchestrator_desc_ap(orch_args, thread_id, created_cnt);
}