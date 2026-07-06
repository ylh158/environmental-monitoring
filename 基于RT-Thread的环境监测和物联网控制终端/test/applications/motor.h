/*
 * DC Motor Driver — DRV8833
 * Interface: AIN1, AIN2 (2 pins control 1 DC motor)
 * Hardware:  STM32F407ZGT6 + DRV8833 module
 */

#ifndef __MOTOR_H__
#define __MOTOR_H__

#include <rtthread.h>

/* Motor control modes */
#define MOTOR_STOP      0   /* coast stop: AIN1=0, AIN2=0 */
#define MOTOR_FORWARD   1   /* forward:    AIN1=1, AIN2=0 */
#define MOTOR_REVERSE   2   /* reverse:    AIN1=0, AIN2=1 */
#define MOTOR_BRAKE     3   /* brake:      AIN1=1, AIN2=1 */

/* Initialize motor GPIO pins */
void motor_init(void);

/* Set motor running state (MOTOR_STOP / FORWARD / REVERSE / BRAKE) */
void motor_set(int state);

/* Convenience helpers */
static inline void motor_forward(void) { motor_set(MOTOR_FORWARD); }
static inline void motor_reverse(void) { motor_set(MOTOR_REVERSE); }
static inline void motor_stop(void)    { motor_set(MOTOR_STOP); }
static inline void motor_brake(void)   { motor_set(MOTOR_BRAKE); }

#endif /* __MOTOR_H__ */