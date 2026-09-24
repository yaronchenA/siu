#include "rs485.h"

#include "board.h"
#include "stm32f0xx.h"
#include "stm32f0xx_ll_bus.h"
#include "stm32f0xx_ll_usart.h"

/* Power-of-two sizes so the ring index wraps with a mask. 256 bytes each covers one
 * maximum-size protocol frame (248 bytes on the wire, cpm_siu_protocol.md §2.2). */
#define RX_SIZE 256u
#define TX_SIZE 256u

static volatile uint8_t  s_rx[RX_SIZE];
static volatile uint16_t s_rx_head, s_rx_tail;   /* ISR writes head, main reads tail */
static volatile uint8_t  s_tx[TX_SIZE];
static volatile uint16_t s_tx_head, s_tx_tail;   /* main writes head, ISR reads tail */
static volatile uint32_t s_rx_dropped;

void rs485_init(void)
{
    board_link_uart_pins_init();
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_USART1);

    LL_USART_Disable(USART1);
    LL_USART_SetBaudRate(USART1, BOARD_LINK_UART_CLOCK_HZ, LL_USART_OVERSAMPLING_16, RS485_BAUD);
    LL_USART_SetDataWidth(USART1, LL_USART_DATAWIDTH_8B);
    LL_USART_SetParity(USART1, LL_USART_PARITY_NONE);
    LL_USART_SetStopBitsLength(USART1, LL_USART_STOPBITS_1);
    LL_USART_SetTransferDirection(USART1, LL_USART_DIRECTION_TX_RX);

    /* Hardware RS485 driver enable on PA12: high while transmitting, no extra delays. */
    LL_USART_EnableDEMode(USART1);
    LL_USART_SetDESignalPolarity(USART1, LL_USART_DE_POLARITY_HIGH);
    LL_USART_SetDEAssertionTime(USART1, 0);
    LL_USART_SetDEDeassertionTime(USART1, 0);

    LL_USART_Enable(USART1);
    LL_USART_EnableIT_RXNE(USART1);

    NVIC_SetPriority(USART1_IRQn, 1);   /* above SysTick; the link must never lose bytes */
    NVIC_EnableIRQ(USART1_IRQn);
}

void USART1_IRQHandler(void)
{
    if (LL_USART_IsActiveFlag_ORE(USART1)) {
        LL_USART_ClearFlag_ORE(USART1);
        s_rx_dropped++;
    }
    if (LL_USART_IsActiveFlag_RXNE(USART1)) {
        uint8_t b = LL_USART_ReceiveData8(USART1);
        uint16_t next = (uint16_t)((s_rx_head + 1u) & (RX_SIZE - 1u));
        if (next == s_rx_tail) {
            s_rx_dropped++;
        } else {
            s_rx[s_rx_head] = b;
            s_rx_head = next;
        }
    }
    if (LL_USART_IsEnabledIT_TXE(USART1) && LL_USART_IsActiveFlag_TXE(USART1)) {
        if (s_tx_tail == s_tx_head) {
            LL_USART_DisableIT_TXE(USART1);
        } else {
            LL_USART_TransmitData8(USART1, s_tx[s_tx_tail]);
            s_tx_tail = (uint16_t)((s_tx_tail + 1u) & (TX_SIZE - 1u));
        }
    }
}

bool rs485_write(const uint8_t *data, size_t len)
{
    uint16_t head = s_tx_head;
    uint16_t free_space = (uint16_t)((s_tx_tail - head - 1u) & (TX_SIZE - 1u));
    if (len > free_space) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        s_tx[head] = data[i];
        head = (uint16_t)((head + 1u) & (TX_SIZE - 1u));
    }
    s_tx_head = head;
    LL_USART_EnableIT_TXE(USART1);
    return true;
}

size_t rs485_read(uint8_t *buf, size_t max)
{
    size_t n = 0;
    while (n < max && s_rx_tail != s_rx_head) {
        buf[n++] = s_rx[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) & (RX_SIZE - 1u));
    }
    return n;
}

uint32_t rs485_rx_dropped(void)
{
    return s_rx_dropped;
}
