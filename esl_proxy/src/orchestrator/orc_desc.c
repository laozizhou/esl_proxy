/* Thin wrapper: includes only the desc case header and exposes a
 * callable function for use by orchestrator.c. */

#include "tensormap.h"
#include "log.h"
#include "qwen3_14b_decoder_desc.h"

int orc_desc_call(uint64_t orch_args, int thread_id)
{
    return orchestrator_desc(orch_args, thread_id);
}
