/* Minimal test application for the USB bootloader lab.
   This replaces the whole CubeMX output, so regenerating from the .ioc wipes it. */

#include "main.h"

/* 0 = slow blink (original firmware), 1 = fast blink (updated firmware) */
#define FAST_BLINK 0

#if FAST_BLINK
#define BLINK_DELAY_MS 100
#else
#define BLINK_DELAY_MS 500
#endif

void SystemClock_Config(void);
static void MX_GPIO_Init(void);

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();

  while (1)
  {
    HAL_GPIO_TogglePin(LD2_GPIO_Port, LD2_Pin);
    HAL_Delay(BLINK_DELAY_MS);
  }
}

/* Left empty on purpose: the app runs on the reset-default MSI clock, and
   HAL_Delay() counts whatever clock is actually running. */
void SystemClock_Config(void)
{
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* LED is LD2 (blue) on PB7. The handout's GPIOD enable is wrong for this board. */
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LD2_GPIO_Port, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}
