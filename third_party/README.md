# Third-party code

Only the files this firmware needs are vendored here, unmodified, so the build doesn't depend on network access or an STM32Cube installation.

| Directory | Source | Version | Commit | License |
|---|---|---|---|---|
| `st/cmsis_core/Include` | [STMicroelectronics/cmsis_core](https://github.com/STMicroelectronics/cmsis_core) (Arm CMSIS-Core) — `cmsis_compiler.h`, `cmsis_gcc.h`, `cmsis_version.h`, `core_cm0.h` only | v5.9.0 | `1a2f783` | Apache-2.0 (`st/cmsis_core/LICENSE.md`) |
| `st/cmsis_device_f0` | [STMicroelectronics/cmsis_device_f0](https://github.com/STMicroelectronics/cmsis_device_f0) — STM32F030x8 headers, `system_stm32f0xx.c`, GCC startup | v2.3.7 | `3973d99` | Apache-2.0 (`st/cmsis_device_f0/LICENSE.md`) |
| `st/stm32f0xx_ll` | [STMicroelectronics/stm32f0xx_hal_driver](https://github.com/STMicroelectronics/stm32f0xx_hal_driver) — LL (low-layer) drivers only, no HAL; files for peripherals the F030 lacks (COMP, CRS, DAC, USB) removed | v1.7.8 | `115eb1d` | BSD-3-Clause (`st/stm32f0xx_ll/LICENSE.md`) |

To update: clone the repos at the new tag, copy the same files over, update this table, and rebuild + retest.
