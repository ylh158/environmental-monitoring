/*
 * Alarm Thread — monitors temperature, drives LED + buzzer on threshold
 */
#include "threads.h"
#include <board.h>

#define BUZZER_PIN  GET_PIN(B, 0)

extern rt_mq_t mq_sensor;
extern rt_mq_t mq_display_wake;

void alarm_thread_entry(void *param)
{
    struct sensor_msg s;

    rt_pin_mode(BUZZER_PIN, PIN_MODE_OUTPUT);
    rt_pin_write(BUZZER_PIN, PIN_LOW);

    while (1) {
        if (rt_mq_recv(mq_sensor, &s, sizeof(s), RT_WAITING_FOREVER) == RT_EOK) {
            if (s.temp > TEMP_THRESHOLD) {
                int dummy = 1;
                rt_mq_send(mq_display_wake, &dummy, sizeof(dummy));
                rt_kprintf("[alarm] OVER TEMP! %d.%dC\n",
                           (int)s.temp, (int)(s.temp * 10) % 10);
                /* beep 3 times */
                for (int i = 0; i < 3; i++) {
                    rt_pin_write(BUZZER_PIN, PIN_HIGH);
                    rt_thread_mdelay(200);
                    rt_pin_write(BUZZER_PIN, PIN_LOW);
                    rt_thread_mdelay(200);
                }
            }
        }
    }
}