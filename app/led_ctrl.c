/*
 * Status LED logic — see led_ctrl.h and siu_detailed_design.md §6.1.
 */
#include "led_ctrl.h"

#include <stddef.h>

#define RGB_OFF    {0, 0, 0}
#define RGB_RED    {255, 0, 0}
#define RGB_GREEN  {0, 255, 0}
#define RGB_BLUE   {0, 0, 255}
#define RGB_WHITE  {255, 255, 255}
#define RGB_PURPLE {255, 0, 255}
#define RGB_AMBER  {255, 110, 0}
#define RGB_CYAN   {0, 255, 255}

#define SELFTEST_STEP_MS  300u
#define SELFTEST_MS       (3u * SELFTEST_STEP_MS)
#define FLASH_ON_MS       150u
#define FLASH_PERIOD_MS   300u

typedef struct {
    rgb_t   color;
    rgb_t   color2;   /* PAT_ALTERNATE only */
    uint8_t pattern;
} look_t;

/* siu_detailed_design.md §6.1.2 */
static const look_t k_state_look[UI_STATE_COUNT] = {
    [UI_AVAILABLE]        = { RGB_GREEN,  RGB_OFF, PAT_SOLID   },
    [UI_SUSPENDED_EVSE]   = { RGB_BLUE,   RGB_OFF, PAT_SOLID   },
    [UI_CHARGING]         = { RGB_GREEN,  RGB_OFF, PAT_BLINK   },
    [UI_FAULTED]          = { RGB_RED,    RGB_OFF, PAT_SOLID   },
    [UI_RESERVED]         = { RGB_PURPLE, RGB_OFF, PAT_SOLID   },
    [UI_STOPPED]          = { RGB_RED,    RGB_OFF, PAT_FLICKER },
    [UI_UPDATING]         = { RGB_CYAN,   RGB_OFF, PAT_BLINK   },
    [UI_AUTHORIZING]      = { RGB_WHITE,  RGB_OFF, PAT_BREATHE },
    [UI_SUSPENDED_EV]     = { RGB_BLUE,   RGB_OFF, PAT_FLICKER },
    [UI_PREPARING]        = { RGB_WHITE,  RGB_OFF, PAT_SOLID   },
    [UI_FINISHING]        = { RGB_WHITE,  RGB_OFF, PAT_BLINK   },
    [UI_UNAVAILABLE]      = { RGB_AMBER,  RGB_OFF, PAT_SOLID   },
    [UI_PENDING_APPROVAL] = { RGB_AMBER,  RGB_OFF, PAT_BLINK   },
};

/* siu_detailed_design.md §6.1.3 */
static const look_t k_override_look[LED_OVR_COUNT] = {
    [LED_OVR_SELFTEST]   = { RGB_OFF,  RGB_OFF,  PAT_SELFTEST  },
    [LED_OVR_FACTORY]    = { RGB_RED,  RGB_BLUE, PAT_ALTERNATE },
    [LED_OVR_BOOTLOADER] = { RGB_CYAN, RGB_OFF,  PAT_BLINK     },
    [LED_OVR_ESTOP]      = { RGB_RED,  RGB_OFF,  PAT_FLICKER   },
    [LED_OVR_OVERTEMP]   = { RGB_RED,  RGB_OFF,  PAT_SOLID     },
    [LED_OVR_NOLINK]     = { RGB_RED,  RGB_OFF,  PAT_BLINK     },
};

static struct {
    bool      override_active[LED_OVR_COUNT];
    uint32_t  selftest_start_ms;

    bool      commanded_valid;     /* a LED_SET arrived since the last hold began */
    look_t    commanded;

    bool      holding;             /* showing a cleared override until the next LED_SET */
    look_t    hold;

    bool      fb_active;
    rgb_t     fb_color;
    uint8_t   fb_count;
    uint32_t  fb_start_ms;

    look_t    shown;               /* for detecting a change of look */
    bool      shown_valid;
    uint32_t  phase_start_ms;      /* patterns start "on" when the look changes */
} s;

static bool look_equal(const look_t *a, const look_t *b)
{
    return a->pattern == b->pattern &&
           a->color.r == b->color.r && a->color.g == b->color.g && a->color.b == b->color.b &&
           a->color2.r == b->color2.r && a->color2.g == b->color2.g && a->color2.b == b->color2.b;
}

static rgb_t scale(rgb_t c, uint32_t level /* 0..256 */)
{
    rgb_t out = { (uint8_t)((c.r * level) >> 8), (uint8_t)((c.g * level) >> 8), (uint8_t)((c.b * level) >> 8) };
    return out;
}

static rgb_t eval_look(const look_t *l, uint32_t t /* ms since the look started */)
{
    static const rgb_t off = RGB_OFF;

    switch (l->pattern) {
    case PAT_SOLID:
        return l->color;
    case PAT_BLINK:
        return (t % 1000u) < 500u ? l->color : off;
    case PAT_FLICKER:
        return (t % 250u) < 125u ? l->color : off;
    case PAT_BREATHE: {
        /* Triangle 0..1000..0 over 2 s, squared for a perceptually smooth fade.
         * Starts at full brightness so the change is visible immediately. */
        uint32_t p = t % 2000u;
        uint32_t tri = p < 1000u ? 1000u - p : p - 1000u;
        return scale(l->color, (tri * tri) / 3907u);
    }
    case PAT_ALTERNATE:
        return (t % 1000u) < 500u ? l->color : l->color2;
    case PAT_SELFTEST: {
        static const rgb_t seq[3] = { RGB_RED, RGB_GREEN, RGB_BLUE };
        uint32_t step = t / SELFTEST_STEP_MS;
        return step < 3u ? seq[step] : off;
    }
    default:
        return off;
    }
}

void led_ctrl_init(uint32_t now_ms)
{
    for (size_t i = 0; i < LED_OVR_COUNT; i++) {
        s.override_active[i] = false;
    }
    s.override_active[LED_OVR_SELFTEST] = true;
    s.selftest_start_ms = now_ms;
    s.commanded_valid = false;
    s.holding = false;
    s.fb_active = false;
    s.shown_valid = false;
}

bool led_ctrl_set_state(uint8_t ui_state, uint8_t pattern)
{
    if (ui_state >= UI_STATE_COUNT || pattern >= PAT_PROTOCOL_COUNT) {
        return false;
    }
    s.commanded = k_state_look[ui_state];
    if (pattern != PAT_DEFAULT) {
        s.commanded.pattern = pattern;
    }
    s.commanded_valid = true;
    s.holding = false;
    return true;
}

void led_ctrl_set_override(led_override_t ovr, bool active)
{
    if ((unsigned)ovr >= LED_OVR_COUNT || s.override_active[ovr] == active) {
        return;
    }
    s.override_active[ovr] = active;

    if (active) {
        s.fb_active = false;   /* a fault or stop cancels any feedback animation */
    } else if (ovr == LED_OVR_ESTOP || ovr == LED_OVR_OVERTEMP || ovr == LED_OVR_NOLINK) {
        /* Don't fall back to a possibly stale commanded status: hold until the CPM re-sends. */
        s.hold = k_override_look[ovr];
        s.holding = true;
        s.commanded_valid = false;
    }
}

static int active_override(void)
{
    for (int i = 0; i < LED_OVR_COUNT; i++) {
        if (s.override_active[i]) {
            return i;
        }
    }
    return -1;
}

void led_ctrl_feedback(led_feedback_t fb, uint32_t now_ms)
{
    static const rgb_t green = RGB_GREEN, red = RGB_RED;

    if (active_override() >= 0 || s.holding || !s.commanded_valid) {
        return;
    }
    switch (fb) {
    case LED_FB_ACCEPTED:     s.fb_color = green; s.fb_count = 2; break;
    case LED_FB_REJECTED:     s.fb_color = red;   s.fb_count = 3; break;
    case LED_FB_CARD_IGNORED: s.fb_color = red;   s.fb_count = 1; break;
    default: return;
    }
    s.fb_start_ms = now_ms;
    s.fb_active = true;
}

rgb_t led_ctrl_update(uint32_t now_ms)
{
    static const rgb_t off = RGB_OFF;

    if (s.override_active[LED_OVR_SELFTEST] && (now_ms - s.selftest_start_ms) >= SELFTEST_MS) {
        s.override_active[LED_OVR_SELFTEST] = false;
    }

    /* Feedback flashes take over the commanded status while they run. */
    if (s.fb_active) {
        uint32_t t = now_ms - s.fb_start_ms;
        if (t < (uint32_t)s.fb_count * FLASH_PERIOD_MS) {
            s.shown_valid = false;   /* restart the status pattern afterwards */
            return (t % FLASH_PERIOD_MS) < FLASH_ON_MS ? s.fb_color : off;
        }
        s.fb_active = false;
    }

    const look_t *look = NULL;
    int ovr = active_override();
    if (ovr >= 0) {
        look = &k_override_look[ovr];
    } else if (s.holding) {
        look = &s.hold;
    } else if (s.commanded_valid) {
        look = &s.commanded;
    }
    if (look == NULL) {
        s.shown_valid = false;
        return off;
    }

    if (!s.shown_valid || !look_equal(look, &s.shown)) {
        s.shown = *look;
        s.shown_valid = true;
        s.phase_start_ms = (ovr == LED_OVR_SELFTEST) ? s.selftest_start_ms : now_ms;
    }
    return eval_look(look, now_ms - s.phase_start_ms);
}
