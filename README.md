# SIU firmware

Firmware for the **Socket Interface Unit** of the Pine modular EV charging hub — the per-outlet unit with the Type 2 socket, lock, status LED, RFID reader, and control-pilot interface. Runs on an STM32F030R8; developed on a **32F0308DISCOVERY** bench board.

See [ARCHITECTURE.md](ARCHITECTURE.md) for the design, module list, and pin map.

## Status

| Area | State |
|---|---|
| Build system, ST CMSIS + LL, 48 MHz clock, 1 ms tick | Done |
| Status LED logic (`app/led_ctrl`) — all states, overrides, feedback | Done, host-tested |
| RGB LED driver (TIM3 PWM) | Done |
| RS485 link driver (USART1, interrupt-driven, hardware DE) | Done, verified on the bench via USB-serial adapter |
| CPM↔SIU protocol + Python CPM emulator | Next |
| Lock control | Planned |

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

## Demo (current firmware)

After reset the LED runs its self-test (red → green → blue), then shows **Available** (green).

- **Short press** of the blue user button: step to the next status — Available, SuspendedEVSE, Charging, Faulted, Reserved, Stopped, Updating, Authorizing, SuspendedEV, Preparing, Finishing, Unavailable, Pending approval.
- **Long press** (≥ 0.6 s): step through card-accepted flashes, card-rejected flashes, stop button on/off, no-link on/off, factory mode on/off.

The board only has a green (LD3) and a blue (LD4) LED. For red, and for mixed colours like amber and white, connect an LED from **PC6** through ~470 Ω to GND.

## Link test (current firmware)

The SIU sends `SIU alive, uptime N s` once a second on the CPM link UART and echoes every line it receives. With a 3.3 V USB-serial adapter wired to PA9/PA10/GND (see ARCHITECTURE.md §8):

```sh
python3 -m venv .venv && .venv/bin/pip install -r tools/requirements.txt   # once
.venv/bin/python tools/link_test.py --port /dev/cu.usbserial-0001
```

## Layout

```
app/          pure logic (host-testable)       board/     board support (pins, clocks)
drivers/      hardware drivers                 common/    shared types
src/main.c    init + main loop                 tests/     host unit tests
ld/           linker script                    third_party/st/   ST CMSIS + LL (vendored)
```
