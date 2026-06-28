/*
 * Sensor Thread — periodic AHT10 reading, publishes to message queue
 */
#include "threads.h"
#include "aht10.h"

#define I2C_BUS_NAME "i2c3"

extern rt_mq_t mq_sensor;
volatile float g_temp = 0;
volatile float g_humi = 0;
volatile int   g_data_ready = 0;

void sensor_thread_entry(void *param)
{
    aht10_device_t aht = aht10_init(I2C_BUS_NAME);
    if (!aht) {
        rt_kprintf("[sensor] AHT10 init failed\n");
        return;
    }
    rt_thread_mdelay(500);

    while (1) {
        struct sensor_msg msg;
        msg.temp = aht10_read_temperature(aht);
        msg.humi = aht10_read_humidity(aht);

        g_temp = msg.temp;
        g_humi = msg.humi;
        g_data_ready = 1;
        rt_mq_send(mq_sensor, &msg, sizeof(msg));

        rt_thread_mdelay(2000);
    }
}
