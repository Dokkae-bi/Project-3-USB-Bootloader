/* Host suite for the HAL-free half of the bootloader.
   Build and run on the Mac:  cc -std=c11 -Wall -Wextra -I Core/Inc \
       tests/test_boot_proto.c Core/Src/boot_proto.c -o /tmp/t && /tmp/t */

#include <stdio.h>
#include <string.h>

#include "boot_proto.h"

static int g_checks;
static int g_fails;

static void ok(int cond, const char *what)
{
  g_checks++;
  if (!cond) {
    g_fails++;
    printf("FAIL  %s\n", what);
  }
}

static void eq_u32(uint32_t got, uint32_t want, const char *what)
{
  g_checks++;
  if (got != want) {
    g_fails++;
    printf("FAIL  %s (got 0x%08lX want 0x%08lX)\n", what,
           (unsigned long)got, (unsigned long)want);
  }
}

/* ------------------------------------------------------------------ crc */

static void test_crc(void)
{
  /* Standard check value for CRC-32/ISO-HDLC; matches zlib.crc32(b"123456789") */
  eq_u32(boot_crc32((const uint8_t *)"123456789", 9), 0xCBF43926u,
         "crc32 of 123456789");

  eq_u32(boot_crc32((const uint8_t *)"", 0), 0x00000000u, "crc32 of empty");

  eq_u32(boot_crc32((const uint8_t *)"FWUP", 4), 0x5A6F182Du,
         "crc32 of FWUP");

  /* Chunked update must equal one-shot. */
  {
    const uint8_t v[] = {0x00, 0x01, 0x7F, 0x80, 0xFE, 0xFF, 0x42};
    uint32_t a = boot_crc32(v, (uint32_t)sizeof v);
    uint32_t c = boot_crc32_update(0xFFFFFFFFu, v, 3u);
    c = boot_crc32_update(c, v + 3, (uint32_t)sizeof v - 3u) ^ 0xFFFFFFFFu;
    eq_u32(c, a, "crc32 chunked == one-shot");
  }
}

/* ----------------------------------------------------------- ring buffer */

static void test_ring(void)
{
  boot_ring r;
  uint8_t in[64];
  uint8_t out;
  int i;

  for (i = 0; i < 64; i++) {
    in[i] = (uint8_t)i;
  }

  boot_ring_init(&r);
  ok(boot_ring_room(&r) == (int)BOOT_RING_SIZE - 1, "empty ring reports full room");

  ok(boot_ring_push(&r, in, 64) == 64, "push 64");
  ok(boot_ring_room(&r) == (int)BOOT_RING_SIZE - 1 - 64, "room drops by 64");

  for (i = 0; i < 64; i++) {
    ok(boot_ring_pop(&r, &out) == 1, "pop succeeds");
    ok((int)out == i, "pop order preserved");
  }
  ok(boot_ring_pop(&r, &out) == 0, "pop on empty fails");

  /* Fill past capacity: must take only what fits, never overwrite. */
  boot_ring_init(&r);
  {
    uint8_t big[BOOT_RING_SIZE];
    int taken;
    for (i = 0; i < (int)BOOT_RING_SIZE; i++) {
      big[i] = (uint8_t)i;
    }
    taken = boot_ring_push(&r, big, (uint16_t)BOOT_RING_SIZE);
    ok(taken == (int)BOOT_RING_SIZE - 1, "oversized push is clipped to capacity");
    ok(boot_ring_room(&r) == 0, "ring is full");
    for (i = 0; i < taken; i++) {
      boot_ring_pop(&r, &out);
      /* the source bytes were written as (uint8_t)i, so they wrap every 256 */
      ok((int)out == (i & 0xFF), "clipped push keeps prefix order");
    }
  }

  /* Wrap: tail and head must both cross the end and still read in order. */
  boot_ring_init(&r);
  for (i = 0; i < 3; i++) {          /* start partway through the buffer */
    boot_ring_push(&r, (const uint8_t *)"abc", 3);
    boot_ring_pop(&r, &out);
    boot_ring_pop(&r, &out);
    boot_ring_pop(&r, &out);
  }
  {
    uint8_t big[700];
    for (i = 0; i < 700; i++) {
      big[i] = (uint8_t)(i & 0xFF);
    }
    boot_ring_push(&r, big, 700);
    for (i = 0; i < 700; i++) {
      boot_ring_pop(&r, &out);
      ok((int)out == (i & 0xFF), "wrap preserves order");
    }
  }
}

/* --------------------------------------------------------- frame parser */

/* Feeds a whole header + payload and returns the final event plus how many
   payload bytes the parser accepted. */
static boot_evt feed_frame(boot_proto *p, uint32_t len, uint32_t crc,
                           uint32_t payload_bytes, int *accepted)
{
  uint8_t hdr[BOOT_HEADER_LEN];
  boot_evt ev = BOOT_EVT_NONE;
  uint32_t i;

  hdr[0] = 'F'; hdr[1] = 'W'; hdr[2] = 'U'; hdr[3] = 'P';
  hdr[4] = (uint8_t)(len);       hdr[5] = (uint8_t)(len >> 8);
  hdr[6] = (uint8_t)(len >> 16); hdr[7] = (uint8_t)(len >> 24);
  hdr[8] = (uint8_t)(crc);       hdr[9] = (uint8_t)(crc >> 8);
  hdr[10] = (uint8_t)(crc >> 16); hdr[11] = (uint8_t)(crc >> 24);

  boot_proto_init(p);
  *accepted = 0;
  for (i = 0; i < BOOT_HEADER_LEN; i++) {
    ev = boot_proto_feed(p, hdr[i]);
  }
  for (i = 0; i < payload_bytes; i++) {
    ev = boot_proto_feed(p, 0xA5);
    if (ev == BOOT_EVT_PAYLOAD || ev == BOOT_EVT_COMPLETE) {
      (*accepted)++;
    }
  }
  return ev;
}

static void test_parser(void)
{
  boot_proto p;
  int accepted;
  boot_evt ev;

  /* Clean frame. */
  ev = feed_frame(&p, 100u, 0xDEADBEEFu, 100u, &accepted);
  ok(ev == BOOT_EVT_COMPLETE, "full frame completes");
  ok(accepted == 100, "all payload bytes accepted");
  eq_u32(p.length, 100u, "length parsed");
  eq_u32(p.crc_expected, 0xDEADBEEFu, "crc parsed");

  /* One byte short never completes. */
  ev = feed_frame(&p, 100u, 0u, 99u, &accepted);
  ok(ev == BOOT_EVT_PAYLOAD, "short frame does not complete");
  ok(accepted == 99, "short frame counts 99");

  /* Wrong magic: header must be rejected, and the parser must re-arm so the
     NEXT good frame still works. */
  boot_proto_init(&p);
  {
    const char junk[] = "XXXXgarbage";
    size_t i;
    boot_evt e = BOOT_EVT_NONE;
    for (i = 0; i < sizeof junk - 1; i++) {
      e = boot_proto_feed(&p, (uint8_t)junk[i]);
    }
    ok(e == BOOT_EVT_NONE, "junk before magic is simply ignored");
    ok(p.state == BOOT_ST_MAGIC, "parser still waiting for magic");
  }

  /* A bad magic that LOOKS like the start of one must resync, not wedge. */
  boot_proto_init(&p);
  boot_proto_feed(&p, 'F');
  boot_proto_feed(&p, 'W');
  boot_proto_feed(&p, 'U');
  boot_proto_feed(&p, 'X');           /* breaks the magic here */
  ok(p.state == BOOT_ST_MAGIC, "broken magic stays in magic state");

  ev = feed_frame(&p, 8u, 0x11223344u, 8u, &accepted);
  ok(ev == BOOT_EVT_COMPLETE, "good frame accepted after junk magic");
  ok(accepted == 8, "good frame payload counted");

  /* Zero length is rejected outright. */
  boot_proto_init(&p);
  {
    const uint8_t bad[] = {'F','W','U','P', 0,0,0,0, 0,0,0,0};
    size_t i;
    int saw_bad = 0;
    for (i = 0; i < sizeof bad; i++) {
      if (boot_proto_feed(&p, bad[i]) == BOOT_EVT_HEADER_BAD) {
        saw_bad = 1;
      }
    }
    ok(saw_bad, "zero length rejected");
    ok(p.state == BOOT_ST_MAGIC, "zero length re-arms");
  }

  /* One byte over the region is rejected. */
  boot_proto_init(&p);
  {
    uint32_t over = (uint32_t)BOOT_APP_BYTES + 1u;
    uint8_t bad[BOOT_HEADER_LEN];
    size_t i;
    int saw_bad = 0;
    bad[0]='F'; bad[1]='W'; bad[2]='U'; bad[3]='P';
    bad[4]=(uint8_t)over; bad[5]=(uint8_t)(over>>8);
    bad[6]=(uint8_t)(over>>16); bad[7]=(uint8_t)(over>>24);
    bad[8]=bad[9]=bad[10]=bad[11]=0;
    for (i = 0; i < BOOT_HEADER_LEN; i++) {
      if (boot_proto_feed(&p, bad[i]) == BOOT_EVT_HEADER_BAD) {
        saw_bad = 1;
      }
    }
    ok(saw_bad, "oversized length rejected");
  }

  /* Exactly the region size is allowed. */
  ev = feed_frame(&p, (uint32_t)BOOT_APP_BYTES, 0u, 0u, &accepted);
  ok(ev == BOOT_EVT_HEADER_OK, "length == region size accepted");
}

/* ------------------------------------------------------- double words */

static void test_dw(void)
{
  boot_dw d;
  int i;
  int fired = 0;

  boot_dw_init(&d);
  for (i = 0; i < 7; i++) {
    fired += boot_dw_push(&d, (uint8_t)i);
  }
  ok(fired == 0, "seven bytes do not complete a double word");
  ok(d.n == 7, "seven bytes buffered");
  ok(boot_dw_push(&d, 7) == 1, "eighth byte completes a double word");
  for (i = 0; i < 8; i++) {
    ok(d.buf[i] == (uint8_t)i, "double word bytes in order");
  }
  ok(d.n == 0, "buffer resets after a double word");

  /* Partial tail is padded with 0xFF and reported once. */
  boot_dw_init(&d);
  boot_dw_push(&d, 0x11);
  boot_dw_push(&d, 0x22);
  boot_dw_push(&d, 0x33);
  ok(boot_dw_pad(&d) == 1, "partial word reports padding needed");
  ok(d.buf[0] == 0x11 && d.buf[1] == 0x22 && d.buf[2] == 0x33, "image bytes untouched");
  for (i = 3; i < 8; i++) {
    ok(d.buf[i] == 0xFF, "padding byte is 0xFF");
  }

  /* An exact multiple needs no padding. */
  boot_dw_init(&d);
  for (i = 0; i < 8; i++) {
    boot_dw_push(&d, 0);
  }
  ok(boot_dw_pad(&d) == 0, "aligned image needs no padding");
}

/* ------------------------------------------------------- erase planning */

static void test_erase_plan(void)
{
  boot_span s[2];
  int n = boot_erase_plan(s);
  uint32_t total;

  ok(n == 2, "application region is two bank spans");

  eq_u32(s[0].bank, 1u, "first span is bank 1");
  eq_u32(s[0].first_page, 32u, "bank 1 starts at page 32");
  eq_u32(s[0].page_count, 224u, "bank 1 holds 224 pages");

  eq_u32(s[1].bank, 2u, "second span is bank 2");
  eq_u32(s[1].first_page, 0u, "bank 2 restarts at page 0");
  eq_u32(s[1].page_count, 256u, "bank 2 holds 256 pages");

  total = s[0].page_count + s[1].page_count;
  eq_u32(total, 480u, "480 pages total");

  /* No page may reach into the bootloader: the HAL rejects page numbers >= 256,
     and bank 1 page 32 is the first page after 64 KB. */
  ok(s[0].first_page >= 32u, "bank 1 span starts after the bootloader region");
  ok(s[0].first_page + s[0].page_count <= 256u, "bank 1 span fits inside bank 1");
  ok(s[1].first_page + s[1].page_count <= 256u, "bank 2 span fits inside bank 2");

  /* The spans must cover exactly the application region. */
  eq_u32(total * BOOT_PAGE_SIZE, (uint32_t)BOOT_APP_BYTES,
         "span byte total equals the application region");
}

/* ------------------------------------------------------ image validity */

static void test_image_valid(void)
{
  /* 0x20040000 is what the linker actually puts there: _estack is one past the
     end of the 256 KB SRAM1 region. Confirmed against the app's .isr_vector. */
  ok(boot_image_valid(0x20040000u, 0x080109D9u) == 1, "top-of-RAM stack accepted");
  ok(boot_image_valid(0x2003FFE8u, 0x080109D9u) == 1, "mid-stack accepted");
  ok(boot_image_valid(0x20040008u, 0x080109D9u) == 0, "stack past top of RAM rejected");
  ok(boot_image_valid(0x20000000u, 0x080109D9u) == 0, "stack at RAM base rejected");
  ok(boot_image_valid(0x20040004u, 0x080109D9u) == 0, "misaligned stack rejected");

  ok(boot_image_valid(0xFFFFFFFFu, 0xFFFFFFFFu) == 0, "erased flash rejected");
  ok(boot_image_valid(0x20040000u, 0xFFFFFFFFu) == 0, "erased reset vector rejected");
  ok(boot_image_valid(0xFFFFFFFFu, 0x080109D9u) == 0, "erased stack pointer rejected");

  ok(boot_image_valid(0x08010000u, 0x080109D9u) == 0, "flash address as stack rejected");
  ok(boot_image_valid(0x20040000u, 0x0800FFFFu) == 0, "reset vector below app rejected");
  ok(boot_image_valid(0x20040000u, 0x08100000u) == 0, "reset vector past flash rejected");
  ok(boot_image_valid(0x20040000u, 0x080109D8u) == 0, "even reset vector rejected");
}

int main(void)
{
  test_crc();
  test_ring();
  test_parser();
  test_dw();
  test_erase_plan();
  test_image_valid();

  printf("\n%d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
