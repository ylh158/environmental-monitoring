/*
 * OneNET cloud upload sample — adapted for AHT10 real sensor data
 * MSH commands: onenet_upload_cycle, onenet_upload_once
 * Based on 05_iot_cloud_onenet reference project
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include <stdlib.h>
#include <onenet.h>
#include "aht10.h"

#define DBG_ENABLE
#define DBG_COLOR
#define DBG_SECTION_NAME    "onenet.sample"
#if ONENET_DEBUG
#define DBG_LEVEL           DBG_LOG
#else
#define DBG_LEVEL           DBG_INFO
#endif
#include <rtdbg.h>

#ifdef FINSH_USING_MSH
#include <finsh.h>

#define I2C_BUS_NAME    "i2c3"

extern rt_bool_t g_wifi_ok;

/* Upload real AHT10 temperature + humidity to OneNET cyclically */
static void onenet_upload_entry(void *parameter)
{
    aht10_device_t aht = aht10_init(I2C_BUS_NAME);
    if (!aht) {
        LOG_E("AHT10 init failed");
        return;
    }
    rt_thread_mdelay(500);

    while (1)
    {
        float t = aht10_read_temperature(aht);
        float h = aht10_read_humidity(aht);

        rt_kprintf("[upload] temp=%d -> ", (int)t);
        if (onenet_mqtt_upload_digit("temp_value", (double)(int)t) < 0) {
            rt_kprintf("FAILED, stopping\n");
            break;
        }
        rt_kprintf("OK\n");

        rt_kprintf("[upload] humi=%d -> ", (int)h);
        if (onenet_mqtt_upload_digit("humidity_value", (double)(int)h) < 0) {
            rt_kprintf("FAILED, stopping\n");
            break;
        }
        rt_kprintf("OK\n");
        rt_thread_mdelay(5000);
    }
}

int onenet_upload_cycle(void)
{
    rt_thread_t tid = rt_thread_create("onenet_send",
        onenet_upload_entry, RT_NULL, 2 * 1024,
        RT_THREAD_PRIORITY_MAX / 3 - 1, 5);
    if (tid) rt_thread_startup(tid);
    return 0;
}
MSH_CMD_EXPORT(onenet_upload_cycle, start cyclic AHT10 upload to OneNET);

/* One-shot upload of current AHT10 reading */
int onenet_upload_once(void)
{
    aht10_device_t aht = aht10_init(I2C_BUS_NAME);
    if (!aht) {
        rt_kprintf("AHT10 init failed\n");
        return -1;
    }
    rt_thread_mdelay(300);

    float t = aht10_read_temperature(aht);
    float h = aht10_read_humidity(aht);

    if (onenet_mqtt_upload_digit("temp_value", (double)(int)t) < 0) {
        rt_kprintf("upload temp_value failed\n");
        return -1;
    }
    if (onenet_mqtt_upload_digit("humidity_value", (double)(int)h) < 0) {
        rt_kprintf("upload humidity_value failed\n");
        return -1;
    }

    rt_kprintf("uploaded: temp=%d.%d C, humi=%d %%\n", (int)t,(int)(t* 10) % 10 ,(int)h);
    return 0;
}
MSH_CMD_EXPORT(onenet_upload_once, upload current AHT10 reading to OneNET);

/* Publish digit value to datastream (manual) */
int onenet_publish_digit(int argc, char **argv)
{
    if (argc != 3) {
        rt_kprintf("usage: onenet_mqtt_publish_digit <ds_id> <value>\n");
        return -1;
    }
    if (onenet_mqtt_upload_digit(argv[1], atoi(argv[2])) < 0) {
        rt_kprintf("upload failed\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(onenet_publish_digit, onenet_mqtt_publish_digit,
    send digit data to OneNET cloud);

int onenet_publish_string(int argc, char **argv)
{
    if (argc != 3) {
        rt_kprintf("usage: onenet_mqtt_publish_string <ds_id> <string>\n");
        return -1;
    }
    if (onenet_mqtt_upload_string(argv[1], argv[2]) < 0) {
        rt_kprintf("upload failed\n");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(onenet_publish_string, onenet_mqtt_publish_string,
    send string data to OneNET cloud);

#endif /* FINSH_USING_MSH */
