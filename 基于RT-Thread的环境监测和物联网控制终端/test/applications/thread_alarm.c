/*
 * Alarm Thread — monitors temp/humi against cloud-settable thresholds
 * Controls buzzer + motor (DRV8833) when thresholds are exceeded
 */
#include "threads.h"
#include "motor.h"
#include <board.h>

#define BUZZER_PIN  GET_PIN(B, 0)

extern rt_mq_t mq_sensor;
extern rt_mq_t mq_display_wake;

void alarm_thread_entry(void *param)
{
    struct sensor_msg s;
    int motor_running = 0;      /* track motor state to avoid repeated starts */

    rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BUZZER_PIN, PIN_LOW);

    motor_init();               /* init DRV8833, motor starts stopped */

    while (1) {
        if (rt_mq_recv(mq_sensor, &s, sizeof(s), RT_WAITING_FOREVER) == RT_EOK) {
            int in_alarm = 0;
            const char *reason = RT_NULL;

            if (s.temp > g_temp_high)      { reason = "TEMP HIGH";  in_alarm = 1; }
            else if (s.temp < g_temp_low)  { reason = "TEMP LOW";   in_alarm = 1; }
            else if (s.humi > g_humi_high) { reason = "HUMI HIGH";  in_alarm = 1; }
            else if (s.humi < g_humi_low)  { reason = "HUMI LOW";   in_alarm = 1; }

            if (in_alarm) {
                /* --- Entered alarm state --- */
                if (!motor_running) {
                    motor_running = 1;
                    motor_forward();
                    rt_kprintf("[alarm] Motor ON — %s\n", reason);
                }

                int dummy = 1;
                rt_mq_send(mq_display_wake, &dummy, sizeof(dummy));
                rt_kprintf("[alarm] %s! temp=%d.%dC humi=%d.%d%%\n",
                           reason,
                           (int)s.temp, (int)(s.temp * 10) % 10,
                           (int)s.humi, (int)(s.humi * 10) % 10);
                for (int i = 0; i < 3; i++) {
                    rt_pin_write(BUZZER_PIN, PIN_HIGH);
                    rt_thread_mdelay(200);
                    rt_pin_write(BUZZER_PIN, PIN_LOW);
                    rt_thread_mdelay(200);
                }
            } else {
                /* --- Returned to normal state --- */
                if (motor_running) {
                    motor_running = 0;
                    motor_stop();
                    rt_kprintf("[alarm] Motor OFF — threshold OK\n");
                }
            }
        }
    }
}