/*
 * Host unit tests for app/siu_log — formatter, whole-line draining, overflow handling.
 */
#include "siu_log.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

static char g_out[512];

static const char *drain_all(void)
{
    size_t n = siu_log_drain(g_out, sizeof g_out - 1);
    g_out[n] = '\0';
    return g_out;
}

static void test_formatter(void)
{
    siu_log_init(NULL);
    siu_log("plain");
    siu_log("u=%u d=%d neg=%d x=%x X=%X", 42u, 7, -123, 0xbeefu, 0xbeefu);
    siu_log("pad [%02x] [%4u] [%04d] [%3d]", 0x5u, 7u, -42, -5);
    siu_log("s=%s c=%c pct=%% big=%u", "abc", 'Z', 4294967295u);
    siu_log("min=%d", (int)(-2147483647 - 1));
    CHECK(strcmp(drain_all(),
                 "plain\n"
                 "u=42 d=7 neg=-123 x=beef X=BEEF\n"
                 "pad [05] [   7] [-042] [ -5]\n"
                 "s=abc c=Z pct=% big=4294967295\n"
                 "min=-2147483648\n") == 0);
    CHECK(!siu_log_pending());
}

static void test_drain_takes_whole_lines_only(void)
{
    siu_log_init(NULL);
    siu_log("first line");       /* 11 bytes with \n */
    siu_log("second line");      /* 12 bytes */
    char buf[20];
    size_t n = siu_log_drain(buf, sizeof buf);
    CHECK(n == 11 && memcmp(buf, "first line\n", 11) == 0);
    n = siu_log_drain(buf, 5);   /* too small for the next line: nothing taken */
    CHECK(n == 0 && siu_log_pending());
    n = siu_log_drain(buf, sizeof buf);
    CHECK(n == 12 && memcmp(buf, "second line\n", 12) == 0);
}

static void test_long_line_truncated(void)
{
    siu_log_init(NULL);
    char big[200];
    memset(big, 'a', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    siu_log("%s", big);
    const char *out = drain_all();
    CHECK(strlen(out) == SIU_LOG_LINE_MAX + 1u && out[SIU_LOG_LINE_MAX] == '\n');
}

static void test_overflow_drops_and_reports(void)
{
    siu_log_init(NULL);
    int written = 0;
    for (int i = 0; i < 100; i++) {                 /* far more than fits */
        siu_log("line %02d........", i);            /* 16 bytes each */
        written++;
    }
    CHECK(siu_log_dropped() > 0);
    uint32_t dropped = siu_log_dropped();
    const char *out = drain_all();
    CHECK(strncmp(out, "line 00", 7) == 0);          /* oldest lines are kept */

    siu_log("after");                                /* the drop note comes first, then new lines */
    out = drain_all();
    char expect[64];
    snprintf(expect, sizeof expect, "[%u lines dropped]\nafter\n", (unsigned)dropped);
    CHECK(strcmp(out, expect) == 0);
    (void)written;
}

static void test_wraparound(void)
{
    siu_log_init(NULL);
    char buf[64];
    for (int round = 0; round < 50; round++) {      /* push the ring indexes around many times */
        siu_log("round %d", round);
        size_t n = siu_log_drain(buf, sizeof buf - 1);
        buf[n] = '\0';
        char expect[32];
        snprintf(expect, sizeof expect, "round %d\n", round);
        CHECK(strcmp(buf, expect) == 0);
    }
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "formatter",                  test_formatter },
        { "drain_takes_whole_lines_only", test_drain_takes_whole_lines_only },
        { "long_line_truncated",        test_long_line_truncated },
        { "overflow_drops_and_reports", test_overflow_drops_and_reports },
        { "wraparound",                 test_wraparound },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
