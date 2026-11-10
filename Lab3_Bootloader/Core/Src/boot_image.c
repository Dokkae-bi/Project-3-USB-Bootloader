#include "boot_image.h"

#include "stm32l4xx.h"
#include "boot_proto.h"
#include "main.h"

/* ------------------------------------------------------------------ flash */

int boot_flash_unlock(void)
{
  /* Stale error flags from an earlier attempt would make the next erase fail
     for no visible reason, so clear them before unlocking. */
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  return HAL_FLASH_Unlock() == HAL_OK;
}

void boot_flash_lock(void)
{
  (void)HAL_FLASH_Lock();
}

/* Erases exactly the application region. Page numbers restart at 0 in bank 2,
   so this is two HAL calls, not one - see boot_erase_plan(). */
int boot_erase_app(void)
{
  boot_span spans[2];
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t page_error = 0;
  int count = boot_erase_plan(spans);
  int i;

  for (i = 0; i < count; i++) {
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = (spans[i].bank == 1u) ? FLASH_BANK_1 : FLASH_BANK_2;
    erase.Page = spans[i].first_page;
    erase.NbPages = spans[i].page_count;

    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) {
      return 0;
    }
  }
  return 1;
}

int boot_program_dw(uint32_t addr, const uint8_t *dw8)
{
  uint64_t word = 0;
  int i;

  /* Pack little-endian: dw8[0] is the least significant byte on target. */
  for (i = 7; i >= 0; i--) {
    word = (word << 8) | (uint64_t)dw8[i];
  }

  return HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr, word) == HAL_OK;
}

/* Reads the image back out of flash rather than trusting the incoming CRC
   alone - that way a bad erase or a dropped double word is caught too. */
int boot_verify_crc(uint32_t addr, uint32_t len, uint32_t expect)
{
  const uint8_t *p = (const uint8_t *)addr;
  uint32_t crc = 0xFFFFFFFFu;
  uint32_t done = 0;

  while (done < len) {
    uint32_t n = len - done;
    if (n > 256u) {
      n = 256u;
    }
    crc = boot_crc32_update(crc, &p[done], n);
    done += n;
  }
  return ((crc ^ 0xFFFFFFFFu) == expect) ? 1 : 0;
}

/* -------------------------------------------------------------- hand-off */

int boot_app_installed(void)
{
  uint32_t sp = *(volatile uint32_t *)BOOT_APP_BASE;
  uint32_t pc = *(volatile uint32_t *)(BOOT_APP_BASE + 4u);

  return boot_image_valid(sp, pc);
}

void boot_jump_to_app(void)
{
  uint32_t sp = *(volatile uint32_t *)BOOT_APP_BASE;
  uint32_t pc = *(volatile uint32_t *)(BOOT_APP_BASE + 4u);
  void (*entry)(void);
  int i;

  if (!boot_image_valid(sp, pc)) {
    return;
  }

  /* Take the bootloader's interrupts and clock out of the way before the
     application's Reset_Handler runs, or the old SysTick keeps firing into
     handlers that are no longer mapped. */
  __disable_irq();
  SysTick->CTRL = 0u;
  SysTick->LOAD = 0u;
  SysTick->VAL = 0u;
  for (i = 0; i < 8; i++) {
    NVIC->ICER[i] = 0xFFFFFFFFu;
    NVIC->ICPR[i] = 0xFFFFFFFFu;
  }

  SCB->VTOR = BOOT_APP_BASE;
  __set_MSP(sp);
  __enable_irq();

  entry = (void (*)(void))pc;
  entry();

  /* The application's Reset_Handler never returns. */
  for (;;) {
  }
}

/* -------------------------------------------------------------------- led */

void boot_led_write(int on)
{
  HAL_GPIO_WritePin(LD3_GPIO_Port, LD3_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Solid while writing, off when idle, ~5 Hz while faulted. */
void boot_status_led(uint32_t now_ms, int faulting)
{
  if (!faulting) {
    return;
  }
  boot_led_write((int)((now_ms / 100u) & 1u));
}

/* ----------------------------------------------------------------- button */

int boot_button_pressed(void)
{
  /* The switch pulls PC13 to ground while it is held; the pull-up is on the
     board, which is why MX_GPIO_Init leaves the pin as NOPULL. */
  return HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_RESET;
}
