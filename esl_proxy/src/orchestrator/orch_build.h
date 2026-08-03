/*
 * orch_build.h - Minimal shim for standalone orchestrator.c build.
 *
 * Defines struct node_list and extern globals needed by the algorithm headers,
 * avoiding pulling in the full scheduler subsystem.
 */
#ifndef ORCH_BUILD_H
#define ORCH_BUILD_H

#include <stdint.h>
#include "conf.h"

/* Forward declarations needed by ring_buf.h */
struct node_list {
    uint32_t cnt;
    uint32_t node[CON_NODE_CNT];
    struct node_list *next;
};

#endif /* ORCH_BUILD_H */