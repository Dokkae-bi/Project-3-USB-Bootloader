/* Update protocol and flash geometry. No HAL, no registers - so this compiles
   for the host and gets unit tested before it ever touches the board. */

#ifndef BOOT_PROTO_H
#define BOOT_PROTO_H

#include <stdint.h>

#define BOOT_FLASH_BASE    0x08000000u
#define BOOT_APP_BASE      0x08010000u
#define BOOT_FLASH_END     0x08100000u   /* one past the last byte */
#define BOOT_BANK_SPLIT    0x08080000u   /* bank 1 ends at 0x0807FFFF */
#define BOOT_PAGE_SIZE     2048u

#define BOOT_APP_BYTES     (BOOT_FLASH_END - BOOT_APP_BASE)   /* 983040 */

#define BOOT_RAM_BASE      0x20000000u   /* 256 KB SRAM1 region */
#define BOOT_RAM_TOP       0x20040000u   /* one past the last byte; a legal initial SP */

#define BOOT_HEADER_LEN    12u
#define BOOT_MAGIC_WORD    0x50555746u   /* "FWUP" in little-endian order */

/* ---------------------------------------------------------------- crc32 */

uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len);
uint32_t boot_crc32(const uint8_t *data, uint32_t len);

/* ------------------------------------------------------------ ring buffer */

#define BOOT_RING_SIZE     2048u

typedef struct {
  uint8_t  buf[BOOT_RING_SIZE];
  uint16_t head;   /* next write index */
  uint16_t tail;   /* next read index */
} boot_ring;

void boot_ring_init(boot_ring *r);
int  boot_ring_room(const boot_ring *r);
int  boot_ring_push(boot_ring *r, const uint8_t *src, uint16_t n);
int  boot_ring_pop(boot_ring *r, uint8_t *out);

/* --------------------------------------------------------- frame parser */

typedef enum {
  BOOT_EVT_NONE = 0,
  BOOT_EVT_HEADER_OK,    /* all 12 header bytes valid; length and crc known */
  BOOT_EVT_HEADER_BAD,   /* rejected; parser has re-armed for a new magic */
  BOOT_EVT_PAYLOAD,      /* one payload byte accepted */
  BOOT_EVT_COMPLETE      /* last payload byte accepted */
} boot_evt;

enum { BOOT_ST_MAGIC = 0, BOOT_ST_LENGTH, BOOT_ST_CRC, BOOT_ST_PAYLOAD };

typedef struct {
  uint8_t  state;
  uint8_t  hdr[BOOT_HEADER_LEN];
  uint8_t  hdr_len;
  uint8_t  magic_seen;
  uint32_t shift;
  uint32_t length;
  uint32_t crc_expected;
  uint32_t payload_rx;
} boot_proto;

void     boot_proto_init(boot_proto *p);
boot_evt boot_proto_feed(boot_proto *p, uint8_t b);

/* -------------------------------------------------- double-word buffer */

typedef struct {
  uint8_t buf[8];
  uint8_t n;
} boot_dw;

void boot_dw_init(boot_dw *d);
int  boot_dw_push(boot_dw *d, uint8_t b);   /* 1 = full, program it */
int  boot_dw_pad(boot_dw *d);               /* 1 = partial word padded with 0xFF */

/* ------------------------------------------------------- erase planning */

typedef struct {
  uint32_t bank;
  uint32_t first_page;
  uint32_t page_count;
} boot_span;

/* Splits the application region into the bank-relative page spans the HAL
   needs. Returns the number of spans written (always 2 for this device). */
int boot_erase_plan(boot_span out[2]);

/* --------------------------------------------------- image sanity check */

int boot_image_valid(uint32_t initial_sp, uint32_t reset_pc);

#endif /* BOOT_PROTO_H */
