/* Flash erasing/programming, image verification, and the jump into the
   application. This is the half that needs the HAL; everything it computes
   with (page spans, CRC, image sanity) lives in boot_proto. */

#ifndef BOOT_IMAGE_H
#define BOOT_IMAGE_H

#include <stdint.h>

int  boot_flash_unlock(void);
void boot_flash_lock(void);

int  boot_erase_app(void);
int  boot_program_dw(uint32_t addr, const uint8_t *dw8);
int  boot_verify_crc(uint32_t addr, uint32_t len, uint32_t expect);

int  boot_app_installed(void);
void boot_jump_to_app(void);

void boot_led_write(int on);
void boot_status_led(uint32_t now_ms, int faulting);

int  boot_button_pressed(void);

#endif /* BOOT_IMAGE_H */
