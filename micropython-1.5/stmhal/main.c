#include "stdio.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "semphr.h"
#include STM32_HAL_H
#include "py/mpconfig.h"

#define MICROPYTHON_TASK_PRIORITY       (2)
#define MICROPYTHON_TASK_STACK_SIZE     ((6 * 1024) + 512)

#define LED_TASK_PRIORITY               (2)
#define LED_TASK_STACK_SIZE             (configMINIMAL_STACK_SIZE)

void SystemClock_Config(void);
extern void LedTask(void *pvParameters);
extern void MicroPythonTask(void *pvParameters);

extern void MP_PendSV_Handler(void);
extern void MP_SysTick_Handler(void);

void vApplicationIdleHook(void)
{

}

void vApplicationTickHook( void )
{
    MP_SysTick_Handler();
}

void vApplicationMallocFailedHook( void )
{
	/* vApplicationMallocFailedHook() will only be called if
	configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
	function that will get called if a call to pvPortMalloc() fails.
	pvPortMalloc() is called internally by the kernel whenever a task, queue,
	timer or semaphore is created.  It is also called by various parts of the
	demo application.  If heap_1.c or heap_2.c are used, then the size of the
	heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
	FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
	to query the size of free heap space that remains (although it does not
	provide information on how the remaining heap might be fragmented). */
	taskDISABLE_INTERRUPTS();
	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected. */
	taskDISABLE_INTERRUPTS();
	for( ;; );
}

int main(void)
{
    /* STM32F4xx HAL library initialization:
         - Configure the Flash prefetch, instruction and Data caches
         - Configure the Systick to generate an interrupt each 1 msec
         - Set NVIC Group Priority to 4
         - Global MSP (MCU Support Package) initialization
       */
    HAL_Init();

    // set the system clock to be HSE
    SystemClock_Config();

    // enable GPIO clocks
    __GPIOA_CLK_ENABLE();
    __GPIOB_CLK_ENABLE();
    __GPIOC_CLK_ENABLE();
    __GPIOD_CLK_ENABLE();

    #if defined(__HAL_RCC_DTCMRAMEN_CLK_ENABLE)
    // The STM32F746 doesn't really have CCM memory, but it does have DTCM,
    // which behaves more or less like normal SRAM.
    __HAL_RCC_DTCMRAMEN_CLK_ENABLE();
    #else
    // enable the CCM RAM
    __CCMDATARAMEN_CLK_ENABLE();
    #endif

    #if defined(MICROPY_BOARD_EARLY_INIT)
    MICROPY_BOARD_EARLY_INIT();
    #endif

    /* Create the LED task */
    xTaskCreate(LedTask, "LedTask", LED_TASK_STACK_SIZE, (void *) NULL, LED_TASK_PRIORITY, NULL);

    /* Create MicroPython task */
    xTaskCreate(MicroPythonTask, "MicroPython", MICROPYTHON_TASK_STACK_SIZE, (void *) NULL, MICROPYTHON_TASK_PRIORITY, NULL);


	/* Start the scheduler. */
	vTaskStartScheduler();

	/* If all is well, the scheduler will now be running, and the following line
	will never be reached.  If the following line does execute, then there was
	insufficient FreeRTOS heap memory available for the idle and/or timer tasks
	to be created.  See the memory management section on the FreeRTOS web site
	for more details. */
	for( ;; );
}


