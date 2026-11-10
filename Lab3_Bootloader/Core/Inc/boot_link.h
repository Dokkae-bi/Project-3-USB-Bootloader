/* USB <-> ring buffer hand-off for the bootloader. */

#ifndef BOOT_LINK_H
#define BOOT_LINK_H

#include <stdint.h>

void boot_link_init(void);

/* Called from CDC_Receive_FS with one complete USB packet. */
void boot_link_on_rx(const uint8_t *buf, uint32_t len);

int  boot_link_pop(uint8_t *b);

/* Re-arms the OUT endpoint once the ring has room again. Poll from the loop. */
void boot_link_service(void);

/* Sends one status byte. Returns 0 if the IN endpoint was still busy. */
int  boot_link_send(uint8_t b);

/* Lives in USB_DEVICE/App/usbd_cdc_if.c, which owns the receive buffer. */
void boot_cdc_rearm(void);

#endif /* BOOT_LINK_H */
