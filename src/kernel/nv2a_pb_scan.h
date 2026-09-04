#ifndef NV2A_PB_SCAN_H
#define NV2A_PB_SCAN_H
#include <stdint.h>

/* Set before starting GPU/ack threads. A validated pusher owns execution;
 * the legacy scanner may still survey with RECOMP_PB_SCAN, but must neither
 * execute methods a second time nor read/report the owner's mutable state. */
void nv2a_pb_scan_set_external_executor(int external);
void nv2a_pb_scan(uint32_t start_va, uint32_t end_va);
void nv2a_pb_scan_report(void);
#endif
