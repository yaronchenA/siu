/*
 * RS485 link to the CPM — USART1, interrupt-driven, half-duplex.
 *
 * The USART drives the RS485 transceiver's DE pin in hardware (DE mode), so the bus is
 * released right after the last stop bit (cpm_siu_protocol.md §1). On the bench the
 * same pins go straight to a USB-serial adapter and DE is simply unconnected.
 *
 * Bytes are buffered both ways; rs485_write() never blocks.
 */
#ifndef RS485_H
#define RS485_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RS485_BAUD 115200u

void rs485_init(void);

/* Queues bytes for sending. Returns false (and queues nothing) if they don't all fit. */
bool rs485_write(const uint8_t *data, size_t len);

/* Copies up to max received bytes into buf; returns how many. */
size_t rs485_read(uint8_t *buf, size_t max);

/* Diagnostics: bytes lost because the RX buffer was full, or the hardware overran. */
uint32_t rs485_rx_dropped(void);

#endif /* RS485_H */
