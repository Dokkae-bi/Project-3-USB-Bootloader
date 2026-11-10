#include "boot_proto.h"

/* Reflected CRC-32, same polynomial and init/xorout as zlib.crc32() and the
   STM32 CRC peripheral, so the host and the device agree byte for byte. */
static const uint32_t k_crc_table[16] = {
  0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu,
  0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
  0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu,
  0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu
};

uint32_t boot_crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
  uint32_t i;
  for (i = 0; i < len; i++) {
    crc ^= data[i];
    crc = (crc >> 4) ^ k_crc_table[crc & 0x0Fu];
    crc = (crc >> 4) ^ k_crc_table[crc & 0x0Fu];
  }
  return crc;
}

uint32_t boot_crc32(const uint8_t *data, uint32_t len)
{
  return boot_crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------ ring buffer */

void boot_ring_init(boot_ring *r)
{
  r->head = 0;
  r->tail = 0;
}

int boot_ring_room(const boot_ring *r)
{
  uint16_t used = (uint16_t)((r->head - r->tail) & (BOOT_RING_SIZE - 1u));
  return (int)(BOOT_RING_SIZE - 1u - used);
}

/* Copies as much as fits and reports how many bytes were taken, so the caller
   can leave the rest for the next call instead of dropping it. */
int boot_ring_push(boot_ring *r, const uint8_t *src, uint16_t n)
{
  int room = boot_ring_room(r);
  uint16_t i;
  if (n > (uint16_t)room) {
    n = (uint16_t)room;
  }
  for (i = 0; i < n; i++) {
    r->buf[r->head] = src[i];
    r->head = (uint16_t)((r->head + 1u) & (BOOT_RING_SIZE - 1u));
  }
  return (int)n;
}

int boot_ring_pop(boot_ring *r, uint8_t *out)
{
  if (r->head == r->tail) {
    return 0;
  }
  *out = r->buf[r->tail];
  r->tail = (uint16_t)((r->tail + 1u) & (BOOT_RING_SIZE - 1u));
  return 1;
}

/* --------------------------------------------------------- frame parser */

static uint32_t le32(const uint8_t *p)
{
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void boot_proto_init(boot_proto *p)
{
  uint32_t i;
  p->state = (uint8_t)BOOT_ST_MAGIC;
  p->hdr_len = 0;
  p->magic_seen = 0;
  p->shift = 0;
  p->length = 0;
  p->crc_expected = 0;
  p->payload_rx = 0;
  for (i = 0; i < BOOT_HEADER_LEN; i++) {
    p->hdr[i] = 0;
  }
}

/* Re-arms at the first byte of the frame. Called on a rejected header, and
   also when the host gives up, so the next attempt starts clean. */
static void resync(boot_proto *p)
{
  boot_proto_init(p);
}

boot_evt boot_proto_feed(boot_proto *p, uint8_t b)
{
  switch (p->state) {
    case BOOT_ST_MAGIC:
      /* Shift the byte in and check the last four at once, so a stray byte
         before the magic does not cost a real attempt. */
      p->shift = (p->shift >> 8) | ((uint32_t)b << 24);
      if (p->magic_seen < 4) {
        p->magic_seen++;
      }
      if (p->magic_seen == 4 && p->shift == BOOT_MAGIC_WORD) {
        p->state = (uint8_t)BOOT_ST_LENGTH;
        p->hdr_len = 4;
        p->hdr[0] = 'F';
        p->hdr[1] = 'W';
        p->hdr[2] = 'U';
        p->hdr[3] = 'P';
      }
      return BOOT_EVT_NONE;

    case BOOT_ST_LENGTH:
      p->hdr[p->hdr_len++] = b;
      if (p->hdr_len < 8) {
        return BOOT_EVT_NONE;
      }
      p->length = le32(&p->hdr[4]);
      if (p->length == 0u || p->length > (uint32_t)BOOT_APP_BYTES) {
        resync(p);
        return BOOT_EVT_HEADER_BAD;
      }
      p->state = (uint8_t)BOOT_ST_CRC;
      return BOOT_EVT_NONE;

    case BOOT_ST_CRC:
      p->hdr[p->hdr_len++] = b;
      if (p->hdr_len < BOOT_HEADER_LEN) {
        return BOOT_EVT_NONE;
      }
      p->crc_expected = le32(&p->hdr[8]);
      p->payload_rx = 0;
      p->state = (uint8_t)BOOT_ST_PAYLOAD;
      return BOOT_EVT_HEADER_OK;

    case BOOT_ST_PAYLOAD:
      p->payload_rx++;
      if (p->payload_rx >= p->length) {
        return BOOT_EVT_COMPLETE;
      }
      return BOOT_EVT_PAYLOAD;

    default:
      return BOOT_EVT_NONE;
  }
}

/* -------------------------------------------------- double-word buffer */

void boot_dw_init(boot_dw *d)
{
  uint8_t i;
  for (i = 0; i < 8u; i++) {
    d->buf[i] = 0xFFu;
  }
  d->n = 0;
}

int boot_dw_push(boot_dw *d, uint8_t b)
{
  d->buf[d->n++] = b;
  if (d->n < 8u) {
    return 0;
  }
  d->n = 0;
  return 1;
}

/* Pads the tail of an image whose size is not a multiple of 8. The padding is
   never counted as image data - it just makes the last double word legal. */
int boot_dw_pad(boot_dw *d)
{
  uint8_t i;
  if (d->n == 0u) {
    return 0;
  }
  for (i = d->n; i < 8u; i++) {
    d->buf[i] = 0xFFu;
  }
  d->n = 0;
  return 1;
}

/* ------------------------------------------------------- erase planning */

int boot_erase_plan(boot_span out[2])
{
  /* Page numbers restart at 0 in bank 2, so the region is two spans, not one
     run of 480. Sizes are derived from the addresses rather than written out,
     because getting these numbers wrong is the whole point of this function. */
  out[0].bank = 1u;
  out[0].first_page = (BOOT_APP_BASE - BOOT_FLASH_BASE) / BOOT_PAGE_SIZE;
  out[0].page_count = (BOOT_BANK_SPLIT - BOOT_APP_BASE) / BOOT_PAGE_SIZE;

  out[1].bank = 2u;
  out[1].first_page = 0u;
  out[1].page_count = (BOOT_FLASH_END - BOOT_BANK_SPLIT) / BOOT_PAGE_SIZE;

  return 2;
}

/* --------------------------------------------------- image sanity check */

int boot_image_valid(uint32_t initial_sp, uint32_t reset_pc)
{
  /* Stack pointer must sit inside SRAM1, and the reset vector must land in the
     application region with the Thumb bit set. An erased region reads back as
     0xFFFFFFFF, which fails both checks, so we never jump into nothing.
     BOOT_RAM_TOP is itself legal: the linker sets _estack to one past the end
     of RAM, so a real image starts with exactly that word. */
  if (initial_sp <= BOOT_RAM_BASE || initial_sp > BOOT_RAM_TOP) {
    return 0;
  }
  if ((initial_sp & 7u) != 0u) {   /* AAPCS wants an 8-byte aligned stack */
    return 0;
  }
  if (reset_pc < BOOT_APP_BASE || reset_pc >= BOOT_FLASH_END) {
    return 0;
  }
  if ((reset_pc & 1u) == 0u) {
    return 0;
  }
  return 1;
}
