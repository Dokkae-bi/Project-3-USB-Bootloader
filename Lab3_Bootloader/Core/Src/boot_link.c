#include "boot_link.h"

#include "boot_proto.h"
#include "usbd_cdc_if.h"

#define BOOT_LINK_MPS   64u    /* USB full-speed max packet size */

static boot_ring s_ring;
static uint8_t   s_paused;

void boot_link_init(void)
{
  boot_ring_init(&s_ring);
  s_paused = 0;
}

/* Whole packets only. The endpoint is re-armed only while the ring still has a
   full max-packet of room, so a packet can never arrive with nowhere to go.
   When it does run out we simply stop re-arming: the host then sees NAKs and
   its write() blocks, which is the flow control this protocol depends on. */
void boot_link_on_rx(const uint8_t *buf, uint32_t len)
{
  boot_ring_push(&s_ring, buf, (uint16_t)len);

  if (boot_ring_room(&s_ring) < (int)BOOT_LINK_MPS) {
    s_paused = 1;
    return;
  }
  boot_cdc_rearm();
}

int boot_link_pop(uint8_t *b)
{
  return boot_ring_pop(&s_ring, b);
}

void boot_link_service(void)
{
  if (s_paused && boot_ring_room(&s_ring) >= (int)BOOT_LINK_MPS) {
    s_paused = 0;
    boot_cdc_rearm();
  }
}

int boot_link_send(uint8_t b)
{
  /* Static so the byte stays valid until the IN transfer completes; the status
     bytes are sent far apart, so one buffer is enough. */
  static uint8_t s_tx;

  s_tx = b;
  if (CDC_Transmit_FS(&s_tx, 1u) == USBD_OK) {
    return 1;
  }
  return 0;
}
