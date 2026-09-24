/*
 * SIU debug log, carried to the CPM (or the PC emulator) in LOG_TEXT TLVs (cpm_siu_protocol.md §8.8).
 *
 * siu_log() formats one line into a small RAM buffer; link_session drains whole lines into its
 * responses while logging is enabled (CONFIG_SET key 0x10). Lines are kept until drained; when the
 * buffer is full new lines are dropped and counted, and a "[N lines dropped]" note follows.
 *
 * Formatter supports: %s %c %d %u %x %X, an optional zero-pad/width ("%02x", "%4u"), and %%.
 */
#ifndef SIU_LOG_H
#define SIU_LOG_H

#include <stddef.h>
#include <stdint.h>

#define SIU_LOG_LINE_MAX   60u    /* longer lines are truncated; must fit one response's log budget */
#define SIU_LOG_BUF_SIZE  320u

/* Optional lock for callers in different interrupt contexts (NULL on the host). */
typedef struct {
    uint32_t (*enter)(void);
    void     (*exit)(uint32_t state);
} siu_log_lock_t;

void siu_log_init(const siu_log_lock_t *lock);

void siu_log(const char *fmt, ...);

/* Copies as many complete lines as fit into out (each ending in '\n'); returns the byte count. */
size_t siu_log_drain(char *out, size_t max);

/* True if there is anything to drain. */
int siu_log_pending(void);

uint32_t siu_log_dropped(void);

#endif /* SIU_LOG_H */
