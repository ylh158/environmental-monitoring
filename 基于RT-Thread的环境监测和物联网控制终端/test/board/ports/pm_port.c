/*
 * Power Management port for STM32F407_SPARK
 * STOP mode entry via HAL, wake via EXTI (RC522 IRQ)
 */

#include <rtthread.h>
#include <board.h>

#ifdef RT_USING_PM

extern void rc522_full_init(void);

static void stm32_sleep(struct rt_pm *pm, rt_uint8_t mode)
{
    switch (mode)
    {
    case PM_SLEEP_MODE_DEEP:
        /* Keep debug interface alive during STOP mode (avoids bricking) */
        DBGMCU->CR |= DBGMCU_CR_DBG_STOP;

        /* Disable systick before entering STOP */
        SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;

        /* Enter STOP mode with WFI (wake on EXTI) */
        HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);

        /* ---- Woken up by EXTI interrupt ---- */
        /* Restore system clock (HSE+PLL stopped in STOP) */
        SystemClock_Config();

        /* Re-enable systick */
        SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;

        /* Re-init RC522 after STOP wakeup (chip may have lost state) */
        rc522_full_init();
        break;

    default:
        HAL_PWR_EnterSLEEPMode(PWR_MAINREGULATOR_ON, PWR_SLEEPENTRY_WFI);
        break;
    }
}

static void stm32_run(struct rt_pm *pm, rt_uint8_t mode)
{
}

static struct rt_pm_ops stm32_pm_ops =
{
    .sleep = stm32_sleep,
    .run   = stm32_run,
    .timer_start = RT_NULL,
    .timer_stop  = RT_NULL,
    .timer_get_tick = RT_NULL,
};

static int stm32_pm_init(void)
{
    /* Register PM sleep ops with the framework */
    rt_system_pm_init(&stm32_pm_ops, 0, RT_NULL);

    /* Allow deep sleep (STOP mode) */
    rt_pm_module_request(0, PM_SLEEP_MODE_DEEP);

    rt_kprintf("[pm] STOP mode ready\n");
    return 0;
}
INIT_APP_EXPORT(stm32_pm_init);

#endif /* RT_USING_PM */