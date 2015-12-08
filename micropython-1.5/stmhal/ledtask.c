#include "stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include STM32_HAL_H

void LedTask(void *pvParameters)
{
    /* GPIO structure */
    GPIO_InitTypeDef GPIO_InitStructure;

    /* Configure I/O speed, mode, output type and pull */
    GPIO_InitStructure.Pin = GPIO_PIN_5;
    GPIO_InitStructure.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStructure.Pull = GPIO_PULLUP;
    GPIO_InitStructure.Speed = GPIO_SPEED_FAST;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStructure); 

    /* Toggle LED in an infinite loop */  
    while (1) {
        HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);

        /* Insert a 1000ms delay */
        HAL_Delay(1000);
    }
}
