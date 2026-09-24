# SIU firmware — STM32F030R8 (bench board: 32F0308DISCOVERY)
#
#   make          build firmware (build/siu.elf, build/siu.bin)
#   make flash    program the board over its ST-LINK/V2
#   make test     build and run the host unit tests
#   make hil      hardware-in-the-loop protocol tests against a connected SIU (PORT=...)
#   make clean

TARGET   := siu
BUILD    := build
ST       := third_party/st

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size
HOST_CC  := cc

# ---- firmware ---------------------------------------------------------------

C_SRCS := \
	src/main.c \
	src/link_task.c \
	board/board_f0308disco.c \
	drivers/rgb_led.c \
	drivers/rs485.c \
	app/led_ctrl.c \
	app/link_session.c \
	app/cmd_dispatch.c \
	app/siu_config.c \
	app/siu_log.c \
	common/protocol/cobs.c \
	common/protocol/crc16.c \
	common/protocol/frame.c \
	$(ST)/cmsis_device_f0/Source/system_stm32f0xx.c

ASM_SRCS := $(ST)/cmsis_device_f0/Source/startup_stm32f030x8.s

# Vendor headers via -isystem: their warnings aren't ours to fix.
INCLUDES := \
	-Icommon -Icommon/protocol -Iapp -Idrivers -Iboard -Isrc \
	-isystem $(ST)/cmsis_core/Include \
	-isystem $(ST)/cmsis_device_f0/Include \
	-isystem $(ST)/stm32f0xx_ll/Inc

GIT_HASH := $(shell git rev-parse --short=8 HEAD 2>/dev/null || echo 0)
DEFS     := -DSTM32F030x8 -DUSE_FULL_LL_DRIVER -DBUILD_ID=0x$(GIT_HASH)u
CPUFLAGS := -mcpu=cortex-m0 -mthumb
CFLAGS   := $(CPUFLAGS) $(DEFS) $(INCLUDES) -Os -g3 -std=c11 -Wall -Wextra \
            -ffunction-sections -fdata-sections -MMD -MP
LDSCRIPT := ld/stm32f030r8.ld
LDFLAGS  := $(CPUFLAGS) -T$(LDSCRIPT) --specs=nano.specs \
            -Wl,--gc-sections -Wl,-Map=$(BUILD)/$(TARGET).map -Wl,--print-memory-usage \
            -Wl,--no-warn-rwx-segments

OBJS := $(C_SRCS:%.c=$(BUILD)/%.o) $(ASM_SRCS:%.s=$(BUILD)/%.o)

# Pin OpenOCD to the Discovery board's ST-LINK/V2 (PID 3748) so the CPM Nucleo
# (ST-LINK/V2-1, 374b/3752) can stay plugged in too.
STLINK_VID_PID := 0x0483 0x3748

.PHONY: all clean flash test hil

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

flash: $(BUILD)/$(TARGET).elf
	openocd -f board/st_nucleo_f0.cfg -c "hla_vid_pid $(STLINK_VID_PID)" -c "program $< verify reset exit"

# ---- host unit tests ----------------------------------------------------------

HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Icommon -Icommon/protocol -Iapp
PROTO_SRCS  := common/protocol/cobs.c common/protocol/crc16.c common/protocol/frame.c
HOST_DEPS   := $(wildcard app/*.h common/*.h common/protocol/*.h)
TESTS       := test_led_ctrl test_protocol test_link_session test_siu_log

$(BUILD)/host/test_led_ctrl: tests/test_led_ctrl.c app/led_ctrl.c $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_led_ctrl.c app/led_ctrl.c -o $@

$(BUILD)/host/test_protocol: tests/test_protocol.c $(PROTO_SRCS) $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_protocol.c $(PROTO_SRCS) -o $@

$(BUILD)/host/test_link_session: tests/test_link_session.c app/link_session.c app/cmd_dispatch.c app/siu_config.c app/siu_log.c app/led_ctrl.c $(PROTO_SRCS) $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_link_session.c app/link_session.c app/cmd_dispatch.c app/siu_config.c app/siu_log.c app/led_ctrl.c $(PROTO_SRCS) -o $@

$(BUILD)/host/test_siu_log: tests/test_siu_log.c app/siu_log.c $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_siu_log.c app/siu_log.c -o $@

test: $(TESTS:%=$(BUILD)/host/%)
	@for t in $^; do echo "== $$t"; $$t || exit 1; done
	@if [ -x .venv/bin/python ]; then echo "== tools/test_siu_proto.py"; .venv/bin/python tools/test_siu_proto.py; \
	 else echo "(skipping Python tests: create .venv, see README)"; fi

# ---- hardware-in-the-loop tests ------------------------------------------------------

PORT ?= /dev/cu.usbserial-0001

hil:
	.venv/bin/python -m pytest tests/hil -v -p no:cacheprovider --port $(PORT)

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
