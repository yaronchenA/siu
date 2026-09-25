# SIU Firmware Architecture

Firmware for the **Socket Interface Unit (SIU)** — the per-outlet unit the driver touches: Type 2 socket, control pilot (CP) / proximity pilot (PP), connector lock, RGB status LED, buzzer, RFID reader, AC-presence and temperature sensing. It talks to its parent **CPM** over RS485 using the CPM↔SIU protocol.

Design references (in the `pine/design` document set): `siu_detailed_design.md` (hardware, LED behaviour §6.1, lock logic §5), `cpm_siu_protocol.md` (the wire protocol), `hardware_design.md` §4 (SIU requirements), `manufacturing_procedures.md` §6 (production test).

## 1. Constraints that shape the design

| Constraint | Consequence |
|---|---|
| STM32F030R8: Cortex-M0, 48 MHz, **8 KB RAM**, 64 KB flash | Bare metal, no RTOS, no heap, static allocation only |
| Protocol: SIU must answer every poll within **≤ 1 ms** (cpm_siu_protocol.md §6.1) | Frame handling runs in its own interrupt context, independent of the main loop |
| Safety-related outputs (CP, lock) | Safe state set first at reset; independent watchdog; local safety actions don't wait for the CPM |
| Must be developed and tested before the CPM firmware exists | Pure-logic modules unit-tested on the PC; a Python CPM emulator drives the SIU over a USB-UART adapter |
| Bench board today (32F0308DISCOVERY), our own PCB later | All pin/clock choices isolated in one board file |

## 2. Execution model

| Context | Runs | Rule |
|---|---|---|
| **Interrupts** (highest priority) | UART bytes, CP timer/ADC sampling, 1 ms SysTick | Minimal work: move data, set flags |
| **Link handler** (PendSV — lowest-priority exception) | Pended by the UART ISR when a complete frame (`0x00` delimiter) arrives: decode, apply commands, build and send the response | Guarantees the ≤ 1 ms turnaround no matter what the main loop is doing |
| **Main loop** | Everything slow: RFID transactions, lock state machine, LED patterns, telemetry, flash writes | Every module is a non-blocking `xxx_poll()` — no busy-waits, no delays |

Data shared between the link handler and the main loop is kept in small structs, copied with interrupts briefly masked.

## 3. Layers

```
┌──────────────────────────────────────────────────────────────┐
│ app/      link_session · cmd_dispatch · events · safety_mon  │  pure logic —
│           cp_ctrl · lock_ctrl · led_ctrl · telemetry · ident │  unit-tested on the PC
├──────────────────────────────────────────────────────────────┤
│ common/protocol/   COBS · CRC16 · TLV codec · type defs      │  shared with the CPM
├──────────────────────────────────────────────────────────────┤
│ drivers/  rs485 · cp_pwm_adc · pp · ntc · ac_sense · hbridge │  hardware access
│           rgb_led · buzzer · rc522 · gpio_inputs · cfg_flash │
├──────────────────────────────────────────────────────────────┤
│ board/    board_f0308disco.c  (later: board_siu_rev1.c)      │  pins, clocks, timers
├──────────────────────────────────────────────────────────────┤
│ third_party/st   CMSIS device headers + LL drivers           │  ST, vendored
└──────────────────────────────────────────────────────────────┘
```

Rules:
- **`app/` never touches registers or includes ST headers.** It gets time and inputs as function arguments and returns what to do — so it compiles unchanged on the PC for tests.
- **ST LL drivers, not the HAL.** LL is a thin, readable layer over the registers; the HAL is too large for this part and hides timing.
- **One `board_*.c` per board.** Moving to our own PCB means adding a board file, not editing drivers.

## 4. Modules

| Module | Implements | Status |
|---|---|---|
| `app/led_ctrl` | Status → colour/pattern, SIU-local overrides by priority, feedback flashes (siu_detailed_design.md §6.1) | **Done** (host-tested) |
| `drivers/rgb_led` | Colour balance, brightness, gamma → PWM | **Done** |
| `app/lock_ctrl` + `drivers/hbridge` | Lock until switch closes (1500 ms timeout), unlock to 1000 ms, manual-release mismatch (§5) | Next |
| `common/protocol` | COBS, CRC-16/CCITT-FALSE, frame + TLV parse/build | **Done** (host-tested vs. spec §10 bytes) |
| `drivers/rs485` | USART1, interrupt-driven RX/TX rings, hardware DE on PA12 | **Done** (bench-verified) |
| `app/link_session` + `src/link_task` | Session state machine, `SEQ` response cache, link timeout; frames handled in PendSV | **Done** (host-tested, bench-verified) |
| `app/cmd_dispatch` | Command TLVs: `LED_SET`, `LED_RAW`, `AUTH_FEEDBACK`, `CONFIG_SET/GET`, `CP_SET`; `REQ_ID` duplicate filter + `RESULT` for actions | **Partial** (grows per peripheral) |
| `app/siu_config` | Settings from `CONFIG_SET` (LED brightness, log enable); RAM only until `cfg_flash` | **Partial** |
| `app/siu_log` | Debug log lines → `LOG_TEXT` TLVs; own small formatter; whole lines, bounded per response | **Done** (host + HIL tested) |
| `app/events` | 8-entry event queue with ACK | Planned |
| `app/cp_ctrl` + `drivers/cp_pwm_adc` | CP PWM/modes, A–F detection, diode check | Needs the ±12 V front-end |
| `app/safety_mon` | Over-temp / link loss → CP state F; unlock gates | Planned |
| `app/fw_update` + `src/fw_port.c` + `drivers/flash` | Firmware update receiver (FW_BEGIN/CHUNK/END/ACTIVATE), flash erase/program | **Done** (host + HIL tested, real installs) |
| `boot/boot_main.c` | Bootloader: validate slots, install activated image, recover from an interrupted install, start app | **Done** |
| `common/crc32`, `common/fw_image` | CRC-32 (zlib) and image header/trailer validation, shared by app, bootloader and tools | **Done** |
| `drivers/rc522`, `ntc`, `ac_sense`, `buzzer`, `cfg_flash` | Peripherals | Planned |

## 5. Safety and robustness
- **Safe state first:** the very first thing after reset (before clocks, before anything else) puts CP in state F and leaves the lock untouched.
- **Independent watchdog (IWDG)**, fed from the main loop only when every module has checked in — a stuck module resets the chip.
- **No dynamic memory.** RAM budget target: protocol ≈ 1 KB, stacks ≈ 1 KB, RFID ≈ 0.5 KB, rest ≈ 1 KB → ≈ 3.5 of 8 KB.
- **Configuration in flash** in two alternating pages, so a power loss during a write can't corrupt it.
- **Bootloader + firmware update** (§9): the Cortex-M0 has no VTOR, so the app copies its vector table to the start of SRAM and remaps SRAM to address 0 (`SYSCFG_CFGR1.MEM_MODE`) as the very first thing in `main()`.

## 6. Testing
- `make test` — builds and runs the host unit tests (`tests/`) with the Mac's C compiler.
- `tools/cpm_emulator.py` — drives the SIU over a USB-UART adapter using the real protocol; later reused as the production test fixture driver.
- `tools/test_siu_proto.py` — checks the Python codec against the same spec bytes as the C tests (run by `make test`).
- `make hil` — pytest suite in `tests/hil/` that checks the real firmware against the protocol spec over the USB-serial adapter.

## 7. Directory layout

```
siu/
├── ARCHITECTURE.md     this file
├── README.md           build, flash, test
├── Makefile
├── boot/               bootloader
├── ld/                 linker scripts (app.ld, boot.ld)
├── board/              board.h + one board_*.c per board
├── drivers/            hardware drivers (use board + ST LL)
├── app/                pure logic, host-testable
├── src/main.c          init + main loop
├── tests/              host unit tests
└── third_party/st/     vendored ST CMSIS + LL (see third_party/README.md)
```

## 8. Bench board pin map (32F0308DISCOVERY)

| Function | Pin | Notes |
|---|---|---|
| LED red | PC6 (TIM3_CH1) | **External** LED + ~470 Ω to GND — the board has no red LED |
| LED green | PC9 (TIM3_CH4) | On-board LD3 |
| LED blue | PC8 (TIM3_CH3) | On-board LD4 |
| User button | PA0 | On-board B1, active high — demo input |
| Lock H-bridge IN1 / IN2 | PB4 / PB5 | Planned (DRV8871) |
| Lock switch | PB12 | Planned, pull-up, closed = locked |
| CP PWM / CP sense | PA8 (TIM1_CH1) / PA1 (ADC_IN1) | Planned |
| PP sense / NTC | PA4 (ADC_IN4) / PB0 (ADC_IN8) | Planned |
| 12 V, CP +12 V, CP −12 V rail monitors | PB1 / PC0 / PC1 (ADC_IN9–11) | Planned |
| RC522 RFID | PA5 SCK, PA6 MISO, PA7 MOSI (SPI1), PB6 CS, PB7 RST | Planned |
| Buzzer | PB8 (TIM16_CH1) | Planned |
| E-stop tap / tamper | PC2 / PC3 | Planned |
| AC sense L1, L2, L3, N–PE | PC10–PC13 (active low) | Planned |
| RS485 / protocol UART | PA9 TX / PA10 RX, DE PA12 | USART1, 115200 8N1 — bench: USB-serial adapter (TXD→PA10, RXD←PA9, GND) |
| SWD | PA13 / PA14 | Debug — don't reuse |

Full wiring with external components: `pine/design/bench_wiring.md`. Verify against the board user manual (UM1658) before wiring new functions.

## 9. Flash layout and firmware update

| Region | Address | Size | Contents |
|---|---|---|---|
| Bootloader | `0x08000000` | 8 KB | `boot/` — ~2.4 KB used |
| App slot | `0x08002000` | 26 KB | the running firmware — ~11 KB used |
| Staging slot | `0x08008800` | 26 KB | an update downloads here; kept afterwards as the backup copy |
| Reserved | `0x0800F000` | 4 KB | configuration storage (later) |

RAM: `0x20000000`–`0xBF` app vector table (copied at start-up), `0x200000C0` boot-info word (bootloader tells the app "I just installed you"), normal use from `0x20000100`.

An **image** (`build/siu.img`) = app binary + 32-byte header at `0xC0` (magic, hardware model, size, version, build ID) + CRC-32 trailer — made by `tools/mkimage.py`. The bootloader starts the app only if header, hardware model and CRC are valid; it installs the staged image if it was activated and differs from the app, or if the app is invalid and staging is valid (interrupted install). With no valid image at all it blinks red fast (recovery needs SWD).

Flash erase/program stalls the CPU (and so the UART) — the app does it only in the main loop, only after the last response has left the UART, and restarts the link timer afterwards. `make flash` erases the staging slot so an old staged image can't replace a freshly flashed app.
