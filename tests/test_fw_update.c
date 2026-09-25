/*
 * Host unit tests for common/crc32, common/fw_image and app/fw_update (with a RAM "flash").
 */
#include "crc32.h"
#include "fw_image.h"
#include "fw_layout.h"
#include "fw_update.h"
#include "proto.h"

#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                      \
    } while (0)

#define HW 0xB001u

/* ---- fake port ------------------------------------------------------------------------------ */

static uint8_t g_flash[FW_SLOT_SIZE] __attribute__((aligned(4)));
static bool g_fail_erase, g_fail_write, g_link_idle = true;
static int g_erases, g_writes, g_resets;

bool fw_port_erase_staging(void)
{
    g_erases++;
    memset(g_flash, 0xFF, sizeof g_flash);
    return !g_fail_erase;
}

bool fw_port_write(uint32_t offset, const uint8_t *data, size_t len)
{
    g_writes++;
    if (g_fail_write || offset + len > sizeof g_flash) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (g_flash[offset + i] != 0xFF) {
            return false;                     /* programming non-erased flash */
        }
    }
    memcpy(&g_flash[offset], data, len);
    return true;
}

const uint8_t *fw_port_staging(void) { return g_flash; }
bool fw_port_link_idle(void) { return g_link_idle; }
void fw_port_activate_and_reset(void) { g_resets++; }
void fw_port_after_stall(void) {}
uint32_t fw_port_lock(void) { return 0; }
void fw_port_unlock(uint32_t st) { (void)st; }

/* ---- a test image ------------------------------------------------------------------------- */

static uint8_t g_img[4096 + 4] __attribute__((aligned(4)));
static uint32_t g_img_len;

static uint32_t make_image(uint32_t body_len, uint16_t hw)
{
    memset(g_img, 0, sizeof g_img);
    for (uint32_t i = 0; i < body_len; i++) {
        g_img[i] = (uint8_t)(i * 7u + 3u);
    }
    fw_image_header_t h = { FW_IMAGE_MAGIC, FW_IMAGE_HDR_VER, hw, body_len, 0, 5, 0, 0, 0x1234u, { 0 } };
    memcpy(&g_img[FW_HEADER_OFFSET], &h, sizeof h);
    uint32_t crc = crc32(g_img, body_len);
    memcpy(&g_img[body_len], &crc, 4);
    g_img_len = body_len + 4u;
    return g_img_len;
}

/* What FW_BEGIN announces: the image's own CRC, i.e. its trailer. */
static uint32_t img_crc(void)
{
    return crc32(g_img, g_img_len - 4u);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t begin(uint32_t size, uint32_t crc, bool charging, uint8_t *detail)
{
    uint8_t v[12] = { 1 };
    put_u32(&v[1], size);
    put_u32(&v[5], crc);
    v[9] = 0; v[10] = 5; v[11] = 0;
    return fw_update_begin(v, sizeof v, charging, detail);
}

static uint8_t chunk(uint32_t offset, const uint8_t *data, uint8_t n, uint8_t *detail)
{
    uint8_t v[5 + FWU_CHUNK_MAX] = { 2 };
    put_u32(&v[1], offset);
    memcpy(&v[5], data, n);
    return fw_update_chunk(v, (uint8_t)(5u + n), detail);
}

static void reset_fakes(void)
{
    g_fail_erase = g_fail_write = false;
    g_link_idle = true;
    g_erases = g_writes = g_resets = 0;
    fw_update_init(HW);
}

/* Sends the whole image in 200-byte chunks, running the main loop after each. */
static bool send_all(void)
{
    uint8_t d;
    for (uint32_t off = 0; off < g_img_len;) {
        uint8_t n = (uint8_t)((g_img_len - off) > FWU_CHUNK_MAX ? FWU_CHUNK_MAX : (g_img_len - off));
        if (chunk(off, &g_img[off], n, &d) != PROTO_RESULT_OK) {
            return false;
        }
        fw_update_poll();
        off += n;
    }
    return true;
}

/* ---- tests ------------------------------------------------------------------------------- */

static void test_crc32_check_value(void)
{
    CHECK(crc32((const uint8_t *)"123456789", 9) == 0xCBF43926u);
    uint32_t c = crc32_update(0, (const uint8_t *)"1234", 4);
    CHECK(crc32_update(c, (const uint8_t *)"56789", 5) == 0xCBF43926u);    /* incremental */
}

static void test_image_check(void)
{
    make_image(1000, HW);
    const fw_image_header_t *h;
    uint32_t crc;
    CHECK(fw_image_check(g_img, g_img_len, HW, &h, &crc) == FW_IMG_OK && h->image_size == 1000 && h->fw_minor == 5);
    CHECK(fw_image_check(g_img, g_img_len, 0xB002, NULL, NULL) == FW_IMG_BAD_HW_MODEL);
    CHECK(fw_image_check(g_img, 999, HW, NULL, NULL) == FW_IMG_BAD_SIZE);       /* trailer cut off */
    g_img[500] ^= 1;
    CHECK(fw_image_check(g_img, g_img_len, HW, NULL, NULL) == FW_IMG_BAD_CRC);
    g_img[FW_HEADER_OFFSET] ^= 1;
    CHECK(fw_image_check(g_img, g_img_len, HW, NULL, NULL) == FW_IMG_BAD_MAGIC);
}

static void test_full_update(void)
{
    reset_fakes();
    make_image(3000, HW);
    uint8_t d, st[6];
    CHECK(!fw_update_status(st));                                   /* idle: no FW_STATUS */

    CHECK(begin(g_img_len, img_crc(), false, &d) == PROTO_RESULT_IN_PROGRESS);
    CHECK(fw_update_status(st) && st[0] == FWU_ERASING);
    CHECK(chunk(0, g_img, 200, &d) == PROTO_RESULT_BUSY);           /* still erasing */
    fw_update_poll();
    CHECK(g_erases == 1 && fw_update_state() == FWU_RECEIVING);

    CHECK(send_all());
    CHECK(memcmp(g_flash, g_img, g_img_len) == 0);
    CHECK(fw_update_status(st) && st[1] == (uint8_t)g_img_len && st[2] == (uint8_t)(g_img_len >> 8));

    CHECK(fw_update_end(&d) == PROTO_RESULT_IN_PROGRESS);
    fw_update_poll();
    CHECK(fw_update_state() == FWU_VERIFIED);
    CHECK(fw_update_end(&d) == PROTO_RESULT_OK);                    /* repeated FW_END: already verified */

    CHECK(fw_update_activate(false, &d) == PROTO_RESULT_OK);
    fw_update_poll();
    CHECK(g_resets == 1);
}

static void test_waits_for_link_idle(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d;
    g_link_idle = false;                                            /* response still going out */
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();
    CHECK(g_erases == 0);                                           /* doesn't stall mid-response */
    g_link_idle = true;
    fw_update_poll();
    CHECK(g_erases == 1);
}

static void test_chunk_rules(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d;
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();

    CHECK(chunk(200, g_img, 200, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_OFFSET);   /* skipped ahead */
    CHECK(chunk(0, g_img, 199, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_CHUNK);      /* odd length */
    CHECK(chunk(0, g_img, 200, &d) == PROTO_RESULT_OK);
    CHECK(chunk(200, g_img + 200, 200, &d) == PROTO_RESULT_BUSY);   /* previous not written yet */
    fw_update_poll();
    CHECK(chunk(200, g_img + 200, 200, &d) == PROTO_RESULT_OK);
    fw_update_poll();
    CHECK(chunk(0, g_img, 200, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_OFFSET);     /* going back */
    CHECK(fw_update_end(&d) == PROTO_RESULT_REJECTED && d == FWU_REJ_OFFSET);            /* incomplete */

    uint8_t st[6];
    fw_update_status(st);
    uint32_t next = st[1] | (st[2] << 8);
    CHECK(next == 400);                                             /* FW_STATUS says where to continue */
}

static void test_bad_crc_and_hw_model(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d, st[6];
    begin(g_img_len, img_crc() ^ 1u, false, &d);      /* wrong whole-file CRC */
    fw_update_poll();
    send_all();
    fw_update_end(&d);
    fw_update_poll();
    CHECK(fw_update_state() == FWU_ERROR && fw_update_status(st) && st[5] == FWU_ERR_CRC);
    CHECK(fw_update_activate(false, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_STATE);

    make_image(1000, 0xB002);                                       /* built for other hardware */
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();
    send_all();
    fw_update_end(&d);
    fw_update_poll();
    CHECK(fw_update_state() == FWU_ERROR && fw_update_status(st) && st[5] == FWU_ERR_HW_MODEL);
}

static void test_begin_rejections(void)
{
    reset_fakes();
    uint8_t d;
    CHECK(begin(4000, 0, true, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_CHARGING);
    CHECK(begin(FW_IMAGE_MAX + 4u, 0, false, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_SIZE);
    CHECK(begin(4002, 0, false, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_SIZE);      /* not multiple of 4 */
    CHECK(begin(100, 0, false, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_SIZE);       /* smaller than a header */
    CHECK(fw_update_state() == FWU_IDLE);
}

static void test_flash_failures(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d, st[6];
    g_fail_erase = true;
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();
    CHECK(fw_update_state() == FWU_ERROR && fw_update_status(st) && st[5] == FWU_ERR_ERASE);

    g_fail_erase = false;
    g_fail_write = true;
    begin(g_img_len, img_crc(), false, &d);           /* a new FW_BEGIN recovers */
    fw_update_poll();
    CHECK(fw_update_state() == FWU_RECEIVING);
    chunk(0, g_img, 200, &d);
    fw_update_poll();
    CHECK(fw_update_state() == FWU_ERROR && fw_update_status(st) && st[5] == FWU_ERR_WRITE);
}

static void test_abort_and_stale_results(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d;
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();
    chunk(0, g_img, 200, &d);
    fw_update_abort();                                              /* link lost */
    fw_update_poll();
    CHECK(fw_update_state() == FWU_IDLE && g_writes == 0);          /* buffered chunk never written */
    CHECK(chunk(0, g_img, 200, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_STATE);
    CHECK(fw_update_activate(false, &d) == PROTO_RESULT_REJECTED);
}

static void test_activate_rules(void)
{
    reset_fakes();
    make_image(1000, HW);
    uint8_t d;
    begin(g_img_len, img_crc(), false, &d);
    fw_update_poll();
    send_all();
    fw_update_end(&d);
    fw_update_poll();
    CHECK(fw_update_activate(true, &d) == PROTO_RESULT_REJECTED && d == FWU_REJ_CHARGING);
    g_link_idle = false;
    CHECK(fw_update_activate(false, &d) == PROTO_RESULT_OK);
    fw_update_poll();
    CHECK(g_resets == 0);                                           /* waits until the RESULT has gone out */
    g_link_idle = true;
    fw_update_poll();
    CHECK(g_resets == 1);
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "crc32_check_value",       test_crc32_check_value },
        { "image_check",             test_image_check },
        { "full_update",             test_full_update },
        { "waits_for_link_idle",     test_waits_for_link_idle },
        { "chunk_rules",             test_chunk_rules },
        { "bad_crc_and_hw_model",    test_bad_crc_and_hw_model },
        { "begin_rejections",        test_begin_rejections },
        { "flash_failures",          test_flash_failures },
        { "abort_and_stale_results", test_abort_and_stale_results },
        { "activate_rules",          test_activate_rules },
    };
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_failures;
        tests[i].fn();
        printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", tests[i].name);
    }
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
