/*
 * Host unit tests for app/led_ctrl — behaviour from siu_detailed_design.md §6.1.
 * Built and run with `make test`.
 */
#include "led_ctrl.h"

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

static int is(rgb_t c, uint8_t r, uint8_t g, uint8_t b)
{
    return c.r == r && c.g == g && c.b == b;
}

#define GREEN(c)  is((c), 0, 255, 0)
#define RED(c)    is((c), 255, 0, 0)
#define BLUE(c)   is((c), 0, 0, 255)
#define WHITE(c)  is((c), 255, 255, 255)
#define AMBER(c)  is((c), 255, 110, 0)
#define OFF(c)    is((c), 0, 0, 0)

/* Starts at t=1000 and finishes the 900 ms self-test, so tests begin from a clean state. */
static uint32_t boot_past_selftest(void)
{
    led_ctrl_init(1000);
    (void)led_ctrl_update(1000 + 900);
    return 1000 + 900;
}

static void test_selftest_sequence(void)
{
    led_ctrl_init(5000);
    led_ctrl_set_state(UI_AVAILABLE, PAT_DEFAULT);   /* commanded during self-test */
    CHECK(RED(led_ctrl_update(5000)));
    CHECK(RED(led_ctrl_update(5299)));
    CHECK(GREEN(led_ctrl_update(5300)));
    CHECK(BLUE(led_ctrl_update(5600)));
    CHECK(BLUE(led_ctrl_update(5899)));
    /* after 900 ms the commanded status shows */
    CHECK(GREEN(led_ctrl_update(5900)));
    CHECK(GREEN(led_ctrl_update(9000)));
}

static void test_nothing_commanded_is_off(void)
{
    uint32_t t = boot_past_selftest();
    CHECK(OFF(led_ctrl_update(t + 10)));
}

static void test_state_colours(void)
{
    uint32_t t = boot_past_selftest();

    CHECK(led_ctrl_set_state(UI_AVAILABLE, 0));
    CHECK(GREEN(led_ctrl_update(t)));
    CHECK(led_ctrl_set_state(UI_SUSPENDED_EVSE, 0));
    CHECK(BLUE(led_ctrl_update(t)));
    CHECK(led_ctrl_set_state(UI_FAULTED, 0));
    CHECK(RED(led_ctrl_update(t)));
    CHECK(led_ctrl_set_state(UI_PREPARING, 0));
    CHECK(WHITE(led_ctrl_update(t)));
    CHECK(led_ctrl_set_state(UI_UNAVAILABLE, 0));
    CHECK(AMBER(led_ctrl_update(t)));
    CHECK(led_ctrl_set_state(UI_RESERVED, 0));
    CHECK(is(led_ctrl_update(t), 255, 0, 255));
}

static void test_charging_blinks_1hz_starting_on(void)
{
    uint32_t t = boot_past_selftest() + 123;   /* arbitrary start time */
    led_ctrl_set_state(UI_CHARGING, 0);
    CHECK(GREEN(led_ctrl_update(t)));
    CHECK(GREEN(led_ctrl_update(t + 499)));
    CHECK(OFF(led_ctrl_update(t + 500)));
    CHECK(OFF(led_ctrl_update(t + 999)));
    CHECK(GREEN(led_ctrl_update(t + 1000)));
}

static void test_suspended_ev_flickers_4hz(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_state(UI_SUSPENDED_EV, 0);
    CHECK(BLUE(led_ctrl_update(t)));
    CHECK(OFF(led_ctrl_update(t + 125)));
    CHECK(BLUE(led_ctrl_update(t + 250)));
}

static void test_breathe_fades(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_state(UI_AUTHORIZING, 0);
    rgb_t start = led_ctrl_update(t);
    rgb_t mid   = led_ctrl_update(t + 500);
    rgb_t low   = led_ctrl_update(t + 1000);
    rgb_t back  = led_ctrl_update(t + 2000);
    CHECK(start.r > 250 && start.g > 250 && start.b > 250);
    CHECK(mid.r > 0 && mid.r < start.r);
    CHECK(low.r < 5);
    CHECK(back.r > 250);
}

static void test_pattern_override_and_validation(void)
{
    uint32_t t = boot_past_selftest();
    CHECK(led_ctrl_set_state(UI_AVAILABLE, PAT_BLINK));   /* green, but blinking */
    CHECK(GREEN(led_ctrl_update(t)));
    CHECK(OFF(led_ctrl_update(t + 600)));

    /* out-of-range values are rejected and change nothing */
    CHECK(!led_ctrl_set_state(UI_STATE_COUNT, 0));
    CHECK(!led_ctrl_set_state(UI_FAULTED, PAT_PROTOCOL_COUNT));
    CHECK(!led_ctrl_set_state(UI_FAULTED, PAT_ALTERNATE));
    CHECK(GREEN(led_ctrl_update(t + 1000)));
}

static void test_override_priority_and_hold(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_state(UI_CHARGING, 0);

    led_ctrl_set_override(LED_OVR_NOLINK, true);
    led_ctrl_set_override(LED_OVR_ESTOP, true);
    CHECK(RED(led_ctrl_update(t)));          /* E-stop flicker wins: on ... */
    CHECK(OFF(led_ctrl_update(t + 125)));    /* ... off at 125 ms (4 Hz) */

    led_ctrl_set_override(LED_OVR_ESTOP, false);
    t += 1000;
    CHECK(RED(led_ctrl_update(t)));          /* no-link slow blink now */
    CHECK(RED(led_ctrl_update(t + 400)));
    CHECK(OFF(led_ctrl_update(t + 600)));

    /* no-link clears: keep showing it (hold), don't jump back to stale "charging" */
    led_ctrl_set_override(LED_OVR_NOLINK, false);
    t += 1000;
    CHECK(RED(led_ctrl_update(t)));
    CHECK(OFF(led_ctrl_update(t + 600)));

    /* fresh LED_SET ends the hold */
    led_ctrl_set_state(UI_AVAILABLE, 0);
    CHECK(GREEN(led_ctrl_update(t + 1000)));
}

static void test_factory_mode_alternates(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_override(LED_OVR_FACTORY, true);
    CHECK(RED(led_ctrl_update(t)));
    CHECK(BLUE(led_ctrl_update(t + 500)));
    CHECK(RED(led_ctrl_update(t + 1000)));
    led_ctrl_set_override(LED_OVR_FACTORY, false);   /* not a "hold" override */
    CHECK(OFF(led_ctrl_update(t + 1100)));          /* nothing commanded yet */
}

static void test_feedback_flashes(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_state(UI_PREPARING, 0);   /* white solid */

    led_ctrl_feedback(LED_FB_ACCEPTED, t);
    CHECK(GREEN(led_ctrl_update(t)));
    CHECK(OFF(led_ctrl_update(t + 150)));
    CHECK(GREEN(led_ctrl_update(t + 300)));
    CHECK(OFF(led_ctrl_update(t + 450)));
    CHECK(WHITE(led_ctrl_update(t + 600)));      /* back to the status after 2 flashes */

    t += 1000;
    led_ctrl_feedback(LED_FB_REJECTED, t);
    CHECK(RED(led_ctrl_update(t + 600)));        /* 3rd flash */
    CHECK(WHITE(led_ctrl_update(t + 900)));
}

static void test_feedback_never_over_fault(void)
{
    uint32_t t = boot_past_selftest();
    led_ctrl_set_state(UI_PREPARING, 0);
    led_ctrl_set_override(LED_OVR_OVERTEMP, true);
    led_ctrl_feedback(LED_FB_ACCEPTED, t);
    CHECK(RED(led_ctrl_update(t)));
    CHECK(RED(led_ctrl_update(t + 150)));        /* solid red, no flash gaps */

    /* an override arriving mid-feedback cancels the feedback */
    led_ctrl_set_override(LED_OVR_OVERTEMP, false);
    led_ctrl_set_state(UI_PREPARING, 0);
    led_ctrl_feedback(LED_FB_ACCEPTED, t + 1000);
    led_ctrl_set_override(LED_OVR_ESTOP, true);
    CHECK(RED(led_ctrl_update(t + 1000)));
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "selftest_sequence",               test_selftest_sequence },
        { "nothing_commanded_is_off",        test_nothing_commanded_is_off },
        { "state_colours",                   test_state_colours },
        { "charging_blinks_1hz_starting_on", test_charging_blinks_1hz_starting_on },
        { "suspended_ev_flickers_4hz",       test_suspended_ev_flickers_4hz },
        { "breathe_fades",                   test_breathe_fades },
        { "pattern_override_and_validation", test_pattern_override_and_validation },
        { "override_priority_and_hold",      test_override_priority_and_hold },
        { "factory_mode_alternates",         test_factory_mode_alternates },
        { "feedback_flashes",                test_feedback_flashes },
        { "feedback_never_over_fault",       test_feedback_never_over_fault },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
