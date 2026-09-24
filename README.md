# SIU firmware

Firmware for the **Socket Interface Unit** of the Pine modular EV charging hub — the per-outlet unit with the Type 2 socket, lock, status LED, RFID reader, and control-pilot interface. Runs on an STM32F030R8; developed on a **32F0308DISCOVERY** bench board.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the design, module list, and pin map.

## Status

| Area | State |
|---|---|
| Build system, ST CMSIS + LL, 48 MHz clock, 1 ms tick | Done |
| Status LED logic (`app/led_ctrl`) — all states, overrides, feedback | Done, host-tested |
| RGB LED driver (TIM3 PWM) | Done |
| RS485 link driver (USART1, interrupt-driven, hardware DE) | Done, bench-verified |
| Protocol codec (`common/protocol`: COBS, CRC-16, frames/TLVs) | Done, host-tested against the spec's reference bytes |
| Link session: handshake, sessions, duplicate cache, link timeout | Done, host-tested + bench-verified with the CPM emulator |
| LED commands: `LED_SET`, `LED_RAW` (service), `AUTH_FEEDBACK` flashes, brightness (`CONFIG_SET/GET` key 0x01) | Done, host-tested + bench-verified |
| Action duplicate filter (`REQ_ID` history, `RESULT`) | Done |
| `CP_SET` | Validated and stored — no CP hardware yet |
| Lock control | Next |
| Events, RFID, CP/PP, telemetry, config storage | Planned |

## Toolchain

- **Arm GNU Toolchain** (`arm-none-eabi-gcc`) — `brew install --cask gcc-arm-embedded`, or unpack the official Arm release anywhere and add its `bin/` to `PATH`.
- **OpenOCD** — `brew install open-ocd` (flashes through the board's on-board ST-LINK/V2).
- A host C compiler (`cc`) for the unit tests.

## Build, test, flash

```sh
make          # firmware → build/siu.elf, build/siu.bin
make test     # host unit tests
make flash    # program the board
```

The Discovery board's ST-LINK/V2 has no USB drive and no virtual COM port — flash with `make flash`, and use a separate 3.3 V USB-UART adapter for a debug console. The Makefile selects this board's ST-LINK by USB ID, so a CPM Nucleo can stay plugged in at the same time.

## Bench setup

- 3.3 V USB-serial adapter on the CPM link UART: adapter TXD → PA10, RXD ← PA9, GND ↔ GND (ARCHITECTURE.md §8).
- Optional: an LED from **PC6** through ~470 Ω to GND for the red channel — the board only has green (LD3) and blue (LD4), so red, amber, white and purple need it.
- Python tools, once: `python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt`

## Driving the SIU from the PC (CPM emulator)

```sh
.venv/bin/python tools/cpm_emulator.py --port /dev/cu.usbserial-0001
```

It performs the HELLO → SESSION_START handshake, prints the SIU's identity, then polls every 20 ms like a real CPM. Type commands while it runs:

| Command | Effect |
|---|---|
| `led charging` / `led 3` / `led available 2` | `LED_SET` by name or number, optional pattern |
| `raw 255 110 0` | `LED_RAW` with the SERVICE flag — exact colour until the next `led` |
| `auth accepted` / `rejected` / `pending` / `expired` | `AUTH_FEEDBACK` — green or red feedback flashes |
| `bright 40` / `getbright` | LED brightness via `CONFIG_SET` / `CONFIG_GET` |
| `cp f` / `cp 12v` / `cp pwm 26.7` | `CP_SET` (stored; no CP hardware yet) |
| `ident` | `IDENT_GET` — SIU resends its identity |
| `bad` | Sends an unknown TLV — SIU answers `ERROR UNKNOWN_TLV` |
| `dup` | Sends the same SEQ twice — SIU must answer from its cache |
| `stop` / `go` | Pause / resume polling — after 200 ms the SIU shows "no link" (red slow blink) |
| `stats`, `quit` | |

After reset the LED runs its self-test (red → green → blue), then blinks red slowly (no link) until the emulator connects.

## Layout

```
app/          pure logic (host-testable)       board/     board support (pins, clocks)
drivers/      hardware drivers                 common/    shared types
src/main.c    init + main loop                 tests/     host unit tests
ld/           linker script                    third_party/st/   ST CMSIS + LL (vendored)
```
