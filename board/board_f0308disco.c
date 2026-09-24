/*
 * Board support: 32F0308DISCOVERY (STM32F030R8T6) — the SIU bench board.
 *
 * Pin map (see ARCHITECTURE.md §8):
 *   PC6  TIM3_CH1  LED red   — external LED + ~470 R to GND (board has no red LED)
 *   PC9  TIM3_CH4  LED green — on-board LD3
 *   PC8  TIM3_CH3  LED blue  — on-board LD4
 *   PA0            user button B1 (active high, external pull-down on the board)
 *   PA9  USART1_TX link to CPM (bench: USB-serial adapter RXD)
 *   PA10 USART1_RX link to CPM (bench: USB-serial adapter TXD)
 *   PA12 USART1_DE RS485 driver enable (unconnected until a transceiver is fitted)
 */
#include "board.h"

#include "proto.h"
#include "stm32f0xx.h"
#include "stm32f0xx_ll_adc.h"
#include "stm32f0xx_ll_bus.h"
#include "stm32f0xx_ll_gpio.h"
#include "stm32f0xx_ll_rcc.h"
#include "stm32f0xx_ll_system.h"
#include "stm32f0xx_ll_tim.h"

#define SYSCLK_HZ 48000000u

/* The on-board green/blue LEDs are much brighter than a typical external red one;
 * placeholder values, tune by eye on the bench (and per LED part on the real PCB). */
const uint16_t board_led_balance[3] = { 256u, 160u, 200u };

static volatile uint32_t s_ms;

void SysTick_Handler(void)
{
    s_ms++;
}

uint32_t board_millis(void)
{
    return s_ms;
}

static void safe_state(void)
{
    /* First thing after reset: put safety-related outputs in their safe state.
     * - CP: to state F (-12 V) — no CP hardware on the bench board yet.
     * - Lock: deliberately untouched (never unlock at reset, hardware_design.md §4.4).
     * Nothing to do on this board yet; kept as the single place this happens. */
}

static void clock_init(void)
{
    /* 48 MHz from HSI: HSI/2 = 4 MHz x 12. Flash needs 1 wait state above 24 MHz.
     * (Switch to HSE when the CP timing needs crystal accuracy — prototype_plan.md §2.2.) */
    LL_FLASH_SetLatency(LL_FLASH_LATENCY_1);
    LL_FLASH_EnablePrefetch();

    LL_RCC_HSI_Enable();
    while (!LL_RCC_HSI_IsReady()) {
    }
    LL_RCC_PLL_ConfigDomain_SYS(LL_RCC_PLLSOURCE_HSI_DIV_2, LL_RCC_PLL_MUL_12);
    LL_RCC_PLL_Enable();
    while (!LL_RCC_PLL_IsReady()) {
    }
    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_PLL);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_PLL) {
    }

    SystemCoreClock = SYSCLK_HZ;
    SysTick_Config(SYSCLK_HZ / 1000u);
}

static void led_pwm_init(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOC);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_TIM3);

    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_6, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_8, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetPinMode(GPIOC, LL_GPIO_PIN_9, LL_GPIO_MODE_ALTERNATE);
    LL_GPIO_SetAFPin_0_7(GPIOC, LL_GPIO_PIN_6, LL_GPIO_AF_0);
    LL_GPIO_SetAFPin_8_15(GPIOC, LL_GPIO_PIN_8, LL_GPIO_AF_0);
    LL_GPIO_SetAFPin_8_15(GPIOC, LL_GPIO_PIN_9, LL_GPIO_AF_0);

    /* 48 MHz / 48 = 1 MHz tick, 1000 ticks = 1 kHz PWM — no visible flicker. */
    LL_TIM_SetPrescaler(TIM3, (SYSCLK_HZ / 1000000u) - 1u);
    LL_TIM_SetAutoReload(TIM3, BOARD_LED_PWM_MAX);
    LL_TIM_EnableARRPreload(TIM3);

    static const uint32_t channels[] = { LL_TIM_CHANNEL_CH1, LL_TIM_CHANNEL_CH3, LL_TIM_CHANNEL_CH4 };
    for (unsigned i = 0; i < sizeof channels / sizeof channels[0]; i++) {
        LL_TIM_OC_SetMode(TIM3, channels[i], LL_TIM_OCMODE_PWM1);
        LL_TIM_OC_EnablePreload(TIM3, channels[i]);
    }
    LL_TIM_OC_SetCompareCH1(TIM3, 0);
    LL_TIM_OC_SetCompareCH3(TIM3, 0);
    LL_TIM_OC_SetCompareCH4(TIM3, 0);
    LL_TIM_CC_EnableChannel(TIM3, LL_TIM_CHANNEL_CH1 | LL_TIM_CHANNEL_CH3 | LL_TIM_CHANNEL_CH4);

    LL_TIM_GenerateEvent_UPDATE(TIM3);
    LL_TIM_EnableCounter(TIM3);
}

static void button_init(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);
    LL_GPIO_SetPinMode(GPIOA, LL_GPIO_PIN_0, LL_GPIO_MODE_INPUT);
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_0, LL_GPIO_PULL_NO);
}

void board_init(void)
{
    safe_state();
    clock_init();
    led_pwm_init();
    button_init();
}

bool board_button_pressed(void)
{
    return LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_0) != 0u;
}

void board_uid(uint8_t out[12])
{
    const volatile uint8_t *uid = (const volatile uint8_t *)UID_BASE;
    for (unsigned i = 0; i < 12u; i++) {
        out[i] = uid[i];
    }
}

uint8_t board_reset_reason(void)
{
    uint8_t reason;
    /* Power-on sets the pin-reset flag too, so check power-on first. */
    if (LL_RCC_IsActiveFlag_PORRST()) {
        reason = RESET_POWER_ON;
    } else if (LL_RCC_IsActiveFlag_IWDGRST() || LL_RCC_IsActiveFlag_WWDGRST()) {
        reason = RESET_WATCHDOG;
    } else if (LL_RCC_IsActiveFlag_SFTRST()) {
        reason = RESET_SOFTWARE;
    } else {
        reason = RESET_PIN;
    }
    LL_RCC_ClearResetFlags();
    return reason;
}

uint32_t board_random32(void)
{
    /* The lowest bit of fast, back-to-back temperature-sensor conversions is noise.
     * The ADC is left disabled afterwards; the CP driver will set it up for its own use. */
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
    LL_ADC_SetClock(ADC1, LL_ADC_CLOCK_SYNC_PCLK_DIV4);
    LL_ADC_StartCalibration(ADC1);
    while (LL_ADC_IsCalibrationOnGoing(ADC1)) {
    }
    for (volatile unsigned i = 0; i < 64u; i++) {   /* errata: ADEN can't be set right after calibration */
    }
    LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_PATH_INTERNAL_TEMPSENSOR);
    LL_ADC_REG_SetSequencerChannels(ADC1, LL_ADC_CHANNEL_TEMPSENSOR);
    LL_ADC_SetSamplingTimeCommonChannels(ADC1, LL_ADC_SAMPLINGTIME_1CYCLE_5);
    LL_ADC_Enable(ADC1);
    while (!LL_ADC_IsActiveFlag_ADRDY(ADC1)) {
    }

    uint8_t uid[12];
    board_uid(uid);
    uint32_t x = 2166136261u;                   /* FNV-1a over the UID ... */
    for (unsigned i = 0; i < sizeof uid; i++) {
        x = (x ^ uid[i]) * 16777619u;
    }
    for (unsigned i = 0; i < 64u; i++) {        /* ... and 64 noise samples */
        LL_ADC_REG_StartConversion(ADC1);
        while (!LL_ADC_IsActiveFlag_EOC(ADC1)) {
        }
        x = (x ^ LL_ADC_REG_ReadConversionData12(ADC1)) * 16777619u;
    }

    LL_ADC_Disable(ADC1);
    while (LL_ADC_IsEnabled(ADC1)) {
    }
    LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(ADC1), LL_ADC_PATH_INTERNAL_NONE);
    return x;
}

uint32_t board_critical_enter(void)
{
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    return primask;
}

void board_critical_exit(uint32_t state)
{
    __set_PRIMASK(state);
}

void board_link_task_pend(void)
{
    SCB->ICSR = SCB_ICSR_PENDSVSET_Msk;
}

void board_link_uart_pins_init(void)
{
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_GPIOA);

    static const uint32_t pins[] = { LL_GPIO_PIN_9, LL_GPIO_PIN_10, LL_GPIO_PIN_12 };
    for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
        LL_GPIO_SetPinMode(GPIOA, pins[i], LL_GPIO_MODE_ALTERNATE);
        LL_GPIO_SetAFPin_8_15(GPIOA, pins[i], LL_GPIO_AF_1);   /* USART1 TX / RX / DE */
        LL_GPIO_SetPinSpeed(GPIOA, pins[i], LL_GPIO_SPEED_FREQ_HIGH);
    }
    LL_GPIO_SetPinPull(GPIOA, LL_GPIO_PIN_10, LL_GPIO_PULL_UP);  /* idle-high RX if unplugged */
}

void board_led_pwm_write(uint16_t r, uint16_t g, uint16_t b)
{
    LL_TIM_OC_SetCompareCH1(TIM3, r);   /* PC6 red   */
    LL_TIM_OC_SetCompareCH4(TIM3, g);   /* PC9 green */
    LL_TIM_OC_SetCompareCH3(TIM3, b);   /* PC8 blue  */
}
