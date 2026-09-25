/* Minimal firmware-update port for host tests that don't exercise flash (e.g. test_link_session). */
#include "fw_update.h"

#include <string.h>

static uint8_t s_flash[1024];

bool fw_port_erase_staging(void) { memset(s_flash, 0xFF, sizeof s_flash); return true; }
bool fw_port_write(uint32_t offset, const uint8_t *data, size_t len)
{
    if (offset + len > sizeof s_flash) {
        return false;
    }
    memcpy(&s_flash[offset], data, len);
    return true;
}
const uint8_t *fw_port_staging(void) { return s_flash; }
bool fw_port_link_idle(void) { return true; }
void fw_port_activate_and_reset(void) {}
void fw_port_after_stall(void) {}
uint32_t fw_port_lock(void) { return 0; }
void fw_port_unlock(uint32_t state) { (void)state; }
