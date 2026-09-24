#include "siu_log.h"

#include <stdarg.h>
#include <stdbool.h>

static char     s_buf[SIU_LOG_BUF_SIZE];   /* FIFO of '\n'-terminated lines */
static size_t   s_head, s_tail, s_used;
static uint32_t s_dropped;                  /* lines lost since the last "[dropped]" note */
static uint32_t s_dropped_total;
static const siu_log_lock_t *s_lock;

static uint32_t lock_enter(void) { return (s_lock && s_lock->enter) ? s_lock->enter() : 0u; }
static void lock_exit(uint32_t st) { if (s_lock && s_lock->exit) { s_lock->exit(st); } }

void siu_log_init(const siu_log_lock_t *lock)
{
    s_lock = lock;
    s_head = s_tail = s_used = 0;
    s_dropped = s_dropped_total = 0;
}

/* ---- tiny formatter ------------------------------------------------------------ */

typedef struct {
    char  *p;
    size_t n, max;
} out_t;

static void put_c(out_t *o, char c)
{
    if (o->n < o->max) {
        o->p[o->n++] = c;
    }
}

static void put_num(out_t *o, uint32_t v, unsigned base, bool upper, bool neg, unsigned width, char pad)
{
    char tmp[11];
    unsigned len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        tmp[len++] = digits[v % base];
        v /= base;
    } while (v != 0u);
    unsigned total = len + (neg ? 1u : 0u);
    if (neg && pad == '0') {
        put_c(o, '-');
    }
    for (; total < width; total++) {
        put_c(o, pad);
    }
    if (neg && pad != '0') {
        put_c(o, '-');
    }
    while (len > 0u) {
        put_c(o, tmp[--len]);
    }
}

static void format(out_t *o, const char *fmt, va_list ap)
{
    for (; *fmt != '\0'; fmt++) {
        if (*fmt != '%') {
            put_c(o, *fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        unsigned width = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10u + (unsigned)(*fmt++ - '0');
        }
        if (*fmt == 'l') {
            fmt++;                          /* int and long are both 32-bit here */
        }
        switch (*fmt) {
        case 'd': {
            int32_t v = va_arg(ap, int32_t);
            put_num(o, v < 0 ? (uint32_t)(-(v + 1)) + 1u : (uint32_t)v, 10, false, v < 0, width, pad);
            break;
        }
        case 'u': put_num(o, va_arg(ap, uint32_t), 10, false, false, width, pad); break;
        case 'x': put_num(o, va_arg(ap, uint32_t), 16, false, false, width, pad); break;
        case 'X': put_num(o, va_arg(ap, uint32_t), 16, true, false, width, pad); break;
        case 'c': put_c(o, (char)va_arg(ap, int)); break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            for (s = s ? s : "(null)"; *s != '\0'; s++) {
                put_c(o, *s);
            }
            break;
        }
        case '%': put_c(o, '%'); break;
        case '\0': return;
        default: put_c(o, '%'); put_c(o, *fmt); break;
        }
    }
}

/* ---- FIFO ---------------------------------------------------------------------------- */

static bool push_line(const char *line, size_t len)   /* len includes the '\n'; call locked */
{
    if (SIU_LOG_BUF_SIZE - s_used < len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        s_buf[s_head] = line[i];
        s_head = (s_head + 1u) % SIU_LOG_BUF_SIZE;
    }
    s_used += len;
    return true;
}

void siu_log(const char *fmt, ...)
{
    char line[SIU_LOG_LINE_MAX + 1u];
    out_t o = { line, 0, SIU_LOG_LINE_MAX };
    va_list ap;
    va_start(ap, fmt);
    format(&o, fmt, ap);
    va_end(ap);
    line[o.n++] = '\n';

    uint32_t st = lock_enter();
    if (s_dropped > 0u) {                    /* report earlier losses before newer lines */
        char note[32];
        out_t n = { note, 0, sizeof note - 1u };
        put_c(&n, '[');
        put_num(&n, s_dropped, 10, false, false, 0, ' ');
        const char *t = " lines dropped]";
        for (; *t != '\0'; t++) {
            put_c(&n, *t);
        }
        note[n.n++] = '\n';
        if (push_line(note, n.n)) {
            s_dropped = 0;
        }
    }
    if (s_dropped > 0u || !push_line(line, o.n)) {
        s_dropped++;
        s_dropped_total++;
    }
    lock_exit(st);
}

size_t siu_log_drain(char *out, size_t max)
{
    uint32_t st = lock_enter();
    size_t n = 0;
    size_t pos = s_tail;
    size_t avail = s_used;
    /* take whole lines only */
    while (avail > 0u) {
        size_t len = 0;
        while (len < avail && s_buf[(pos + len) % SIU_LOG_BUF_SIZE] != '\n') {
            len++;
        }
        len++;                               /* include the '\n' */
        if (len > avail || n + len > max) {
            break;
        }
        for (size_t i = 0; i < len; i++) {
            out[n++] = s_buf[(pos + i) % SIU_LOG_BUF_SIZE];
        }
        pos = (pos + len) % SIU_LOG_BUF_SIZE;
        avail -= len;
    }
    s_tail = pos;
    s_used = avail;
    lock_exit(st);
    return n;
}

int siu_log_pending(void)
{
    return s_used > 0u;
}

uint32_t siu_log_dropped(void)
{
    return s_dropped_total;
}
