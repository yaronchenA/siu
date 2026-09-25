# SIU firmware — STM32F030R8 (bench board: 32F0308DISCOVERY)
#
#   make          build bootloader + application + update image
#   make flash    program bootloader + application over the ST-LINK/V2 (and clear the staging slot)
#   make test     build and run the host unit tests
#   make hil      hardware-in-the-loop tests against a connected SIU (PORT=...)
#   make clean
#
# Outputs (build/):
#   siu_boot.elf / .bin   bootloader, 0x08000000
#   siu_app.elf  / .bin   application, 0x08002000
#   siu.img               application image (header + CRC) — flashed to the app slot, and the file
#                         a firmware update sends (tools/fw_update.py)

BUILD    := build
ST       := third_party/st

CC       := arm-none-eabi-gcc
OBJCOPY  := arm-none-eabi-objcopy
SIZE     := arm-none-eabi-size
HOST_CC  := cc
PYTHON   := $(if $(wildcard .venv/bin/python),.venv/bin/python,python3)

# ---- sources --------------------------------------------------------------------------

ST_SRCS  := $(ST)/cmsis_device_f0/Source/system_stm32f0xx.c
ASM_SRCS := $(ST)/cmsis_device_f0/Source/startup_stm32f030x8.s

APP_SRCS := \
	src/main.c \
	src/link_task.c \
	src/fw_port.c \
	src/app_header.c \
	board/board_f0308disco.c \
	drivers/rgb_led.c \
	drivers/rs485.c \
	drivers/flash.c \
	app/led_ctrl.c \
	app/link_session.c \
	app/cmd_dispatch.c \
	app/siu_config.c \
	app/siu_log.c \
	app/fw_update.c \
	common/crc32.c \
	common/fw_image.c \
	common/protocol/cobs.c \
	common/protocol/crc16.c \
	common/protocol/frame.c \
	$(ST_SRCS)

BOOT_SRCS := \
	boot/boot_main.c \
	board/board_f0308disco.c \
	drivers/flash.c \
	common/crc32.c \
	common/fw_image.c \
	$(ST_SRCS)

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
LDFLAGS  := $(CPUFLAGS) --specs=nano.specs -Wl,--gc-sections -Wl,--print-memory-usage \
            -Wl,--no-warn-rwx-segments

APP_OBJS  := $(APP_SRCS:%.c=$(BUILD)/app/%.o) $(ASM_SRCS:%.s=$(BUILD)/app/%.o)
BOOT_OBJS := $(BOOT_SRCS:%.c=$(BUILD)/boot/%.o) $(ASM_SRCS:%.s=$(BUILD)/boot/%.o)

.PHONY: all clean flash test hil

all: $(BUILD)/siu_boot.bin $(BUILD)/siu.img
	@$(SIZE) $(BUILD)/siu_boot.elf $(BUILD)/siu_app.elf

# (separate rules: a two-target pattern rule would tell make one command builds both objects)
$(BUILD)/app/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/boot/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/app/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(BUILD)/boot/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) $(CPUFLAGS) -c $< -o $@

$(BUILD)/siu_app.elf: $(APP_OBJS) ld/app.ld
	$(CC) $(LDFLAGS) -Tld/app.ld -Wl,-Map=$(BUILD)/siu_app.map $(APP_OBJS) -o $@

$(BUILD)/siu_boot.elf: $(BOOT_OBJS) ld/boot.ld
	$(CC) $(LDFLAGS) -Tld/boot.ld -Wl,-Map=$(BUILD)/siu_boot.map $(BOOT_OBJS) -o $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/siu.img: $(BUILD)/siu_app.bin tools/mkimage.py
	$(PYTHON) tools/mkimage.py $< $@

# ---- flashing ------------------------------------------------------------------------------

# Pin OpenOCD to the Discovery board's ST-LINK/V2 (PID 3748) so the CPM Nucleo
# (ST-LINK/V2-1, 374b/3752) can stay plugged in too.
STLINK_VID_PID := 0x0483 0x3748

# The staging slot is erased too: otherwise the bootloader could find an older, activated image
# there and "update" the freshly flashed app back to it.
flash: $(BUILD)/siu_boot.bin $(BUILD)/siu.img
	openocd -f board/st_nucleo_f0.cfg -c "hla_vid_pid $(STLINK_VID_PID)" \
		-c "init" -c "reset halt" \
		-c "flash write_image erase $(BUILD)/siu_boot.bin 0x08000000" \
		-c "flash write_image erase $(BUILD)/siu.img 0x08002000" \
		-c "flash erase_address 0x08008800 26624" \
		-c "verify_image $(BUILD)/siu_boot.bin 0x08000000" \
		-c "verify_image $(BUILD)/siu.img 0x08002000" \
		-c "reset run" -c "shutdown"

# ---- host unit tests -----------------------------------------------------------------------

HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Icommon -Icommon/protocol -Iapp
PROTO_SRCS  := common/protocol/cobs.c common/protocol/crc16.c common/protocol/frame.c
SESSION_SRCS := app/link_session.c app/cmd_dispatch.c app/siu_config.c app/siu_log.c app/led_ctrl.c \
                app/fw_update.c common/crc32.c common/fw_image.c tests/fake_fw_port.c $(PROTO_SRCS)
HOST_DEPS   := $(wildcard app/*.h common/*.h common/protocol/*.h)
TESTS       := test_led_ctrl test_protocol test_link_session test_siu_log test_fw_update

$(BUILD)/host/test_led_ctrl: tests/test_led_ctrl.c app/led_ctrl.c $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_led_ctrl.c app/led_ctrl.c -o $@

$(BUILD)/host/test_protocol: tests/test_protocol.c $(PROTO_SRCS) $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_protocol.c $(PROTO_SRCS) -o $@

$(BUILD)/host/test_link_session: tests/test_link_session.c $(SESSION_SRCS) $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_link_session.c $(SESSION_SRCS) -o $@

$(BUILD)/host/test_siu_log: tests/test_siu_log.c app/siu_log.c $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_siu_log.c app/siu_log.c -o $@

$(BUILD)/host/test_fw_update: tests/test_fw_update.c app/fw_update.c app/siu_log.c common/crc32.c common/fw_image.c $(HOST_DEPS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) tests/test_fw_update.c app/fw_update.c app/siu_log.c common/crc32.c common/fw_image.c -o $@

test: $(TESTS:%=$(BUILD)/host/%)
	@for t in $^; do echo "== $$t"; $$t || exit 1; done
	@if [ -x .venv/bin/python ]; then echo "== tools/test_siu_proto.py"; .venv/bin/python tools/test_siu_proto.py; \
	 else echo "(skipping Python tests: create .venv, see README)"; fi

# ---- hardware-in-the-loop tests --------------------------------------------------------------

PORT ?= /dev/cu.usbserial-0001

hil:
	.venv/bin/python -m pytest tests/hil -v -p no:cacheprovider --port $(PORT)

clean:
	rm -rf $(BUILD)

-include $(APP_OBJS:.o=.d) $(BOOT_OBJS:.o=.d)
