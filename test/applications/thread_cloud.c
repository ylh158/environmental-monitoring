/*
 * Cloud Thread — WiFi connect + OneNET MQTT upload
 */
#include "threads.h"
#include <wlan_mgnt.h>
#include <onenet.h>

#define WIFI_SSID       "vivo"
#define WIFI_PASSWORD   "11050712"

extern rt_mq_t mq_gps;
rt_bool_t g_wifi_ok = RT_FALSE;

void cloud_thread_entry(void *param)
{
    rt_err_t ret;

    rt_thread_mdelay(5000);

    rt_kprintf("[cloud] WiFi %s ... ", WIFI_SSID);
    ret = rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD);
    if (ret != RT_EOK) {
        rt_kprintf("FAIL(%d)\n", ret);
        return;
    }
    while (!rt_wlan_is_ready())
        rt_thread_mdelay(1000);

    rt_kprintf("OK, OneNET init...\n");
    onenet_mqtt_init();
    rt_thread_mdelay(3000);
    rt_kprintf("[cloud] ready\n");
    g_wifi_ok = RT_TRUE;

    struct gps_msg g;

    while (1) {
        if (g_data_ready) {
            rt_kprintf("[cloud] upload: temp:%d.%dC humi:%d.%d%%\n",
                       (int)g_temp, (int)(g_temp * 10) % 10,
                       (int)g_humi, (int)(g_humi * 10) % 10);

            onenet_mqtt_upload_digit("temp_value", (double)(int)g_temp);
            rt_thread_mdelay(200);
            onenet_mqtt_upload_digit("humidity_value", (double)(int)g_humi);
            rt_thread_mdelay(200);
        }

        if (rt_mq_recv(mq_gps, &g, sizeof(g), 0) == RT_EOK) {
            char loc[64];
            rt_snprintf(loc, sizeof(loc), "%d.%04d,%d.%04d",
                        (int)g.lat, (int)((g.lat - (int)g.lat) * 10000),
                        (int)g.lng, (int)((g.lng - (int)g.lng) * 10000));
            onenet_mqtt_upload_string("location", loc);
            rt_thread_mdelay(200);
        }

        if (g_rfid_updated) {
            g_rfid_updated = 0;
            onenet_mqtt_upload_string("rfid_uid", (const char *)g_rfid_uid);
            rt_thread_mdelay(200);
        }

        rt_thread_mdelay(15000);
    }
}
