/*
 * Link task — runs in PendSV (ARCHITECTURE.md §2), pended by the UART interrupt at the end of
 * every frame. Reassembles COBS frames from the RX bytes, hands them to link_session, and sends
 * the response — so the reply goes out within the 1 ms budget whatever the main loop is doing.
 */
#ifndef LINK_TASK_H
#define LINK_TASK_H

#include "link_session.h"

void link_task_init(const siu_identity_t *id, uint32_t now_ms);

/* Latest status, copied into every STATUS_FAST. Call from the main loop. */
void link_task_set_status(const siu_status_t *status);

/* Link timeout check. Call from the main loop. Reads the clock itself, inside the critical
 * section, so "now" can never be older than a frame time the link task just recorded. */
void link_task_tick(void);

#endif /* LINK_TASK_H */
