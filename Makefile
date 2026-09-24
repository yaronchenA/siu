# SIU firmware — STM32F030R8 (bench board: 32F0308DISCOVERY)
#
#   make          build firmware (build/siu.elf, build/siu.bin)
#   make flash    program the board over its ST-LINK/V2
#   make test     build and run the host unit tests
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
	board/board_f0308disco.c \
	drivers/rgb_led.c \
	drivers/rs485.c \
	app/led_ctrl.c \
	$(ST)/cmsis_device_f0/Source/system_stm32f0xx.c

ASM_SRCS := $(ST)/cmsis_device_f0/Source/startup_stm32f030x8.s

# Vendor headers via -isystem: their warnings aren't ours to fix.
INCLUDES := \
	-Icommon -Iapp -Idrivers -Iboard \
	-isystem $(ST)/cmsis_core/Include \
	-isystem $(ST)/cmsis_device_f0/Include \
	-isystem $(ST)/stm32f0xx_ll/Inc

DEFS     := -DSTM32F030x8 -DUSE_FULL_LL_DRIVER
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

.PHONY: all clean flash test

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

HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Icommon -Iapp
TESTS := test_led_ctrl

$(BUILD)/host/test_led_ctrl: tests/test_led_ctrl.c app/led_ctrl.c app/led_ctrl.h common/rgb.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_led_ctrl.c app/led_ctrl.c -o $@

test: $(TESTS:%=$(BUILD)/host/%)
	@for t in $^; do echo "== $$t"; $$t || exit 1; done

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)
