/*
 * DC Motor Driver — DRV8833
 *
 * Pin mapping (change these to match your wiring):
 *   AIN1 = PA0  →  DRV8833 AIN1
 *   AIN2 = PA6  →  DRV8833 AIN2
 *
 * DRV8833 truth table for one DC motor:
 *   AIN1  AIN2  →  Motor state
 *    0     0        Coast (stop)
 *    1     0        Forward
 *    0     1        Reverse
 *    1     1        Brake
 *
 * To add PWM speed control later:
 *   - Enable TIM2 (CH1 on PA0) and/or TIM3 (CH1 on PA6) in RT-Thread config
 *   - Then use rt_pwm_set() to adjust duty cycle
 */

#include "motor.h"
#include <board.h>

/* ============ Pin definitions ============ */
#define AIN1   GET_PIN(A, 0)   /* PA0 — PWM-capable (TIM2_CH1) */
#define AIN2   GET_PIN(A, 6)   /* PA6 — PWM-capable (TIM3_CH1) */

/* ============ Initialization ============ */
void motor_init(void)
{
    rt_pin_mode(AIN1, PIN_MODE_OUTPUT);
    rt_pin_mode(AIN2, PIN_MODE_OUTPUT);

    /* Ensure motor starts in stopped state */
    rt_pin_write(AIN1, PIN_LOW);
    rt_pin_write(AIN2, PIN_LOW);

    rt_kprintf("[motor] DRV8833 initialized (AIN1=PA0, AIN2=PA6)\n");
}

/* ============ Control ============ */
void motor_set(int state)
{
    switch (state)
    {
    case MOTOR_FORWARD:
        rt_pin_write(AIN1, PIN_HIGH);
        rt_pin_write(AIN2, PIN_LOW);
        break;

    case MOTOR_REVERSE:
        rt_pin_write(AIN1, PIN_LOW);
        rt_pin_write(AIN2, PIN_HIGH);
        break;

    case MOTOR_BRAKE:
        rt_pin_write(AIN1, PIN_HIGH);
        rt_pin_write(AIN2, PIN_HIGH);
        break;

    case MOTOR_STOP:
    default:
        rt_pin_write(AIN1, PIN_LOW);
        rt_pin_write(AIN2, PIN_LOW);
        break;
    }
}