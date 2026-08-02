/* Thin wrapper: includes only the alloc case header and exposes a
 * callable function for use by orchestrator.c. */

#include "log.h"
#include "qwen3_14b_decoder_alloc.h"

void orc_alloc_call(uint64_t orch_args)
{
    orchestrator_alloc(orch_args);
}