/*
 * Cloud Thread — WiFi connect + OneNET MQTT upload + threshold command callback
 */
#include "threads.h"
#include "threshold_storage.h"
#include "motor.h"
#include <string.h>
#include <wlan_mgnt.h>
#include <onenet.h>
#include <cJSON.h>

extern rt_mq_t mq_gps;
rt_bool_t g_wifi_ok   = RT_FALSE;
rt_bool_t g_cloud_ok  = RT_FALSE;

volatile int  g_motor_state = 0;    /* 0=OFF, 1=ON */
volatile float g_temp_high = 35.0f;
volatile float g_temp_low  =  0.0f;
volatile float g_humi_high = 90.0f;
volatile float g_humi_low  = 20.0f;

/* Buffer for deferred JSON parsing in cloud thread context */
static char  g_cmd_buf[256];
static volatile int g_cmd_pending = 0;

/*
 * OneNET command callback — runs in Paho MQTT thread.
 * Keep it fast: only memcpy the payload and extract the "id" field.
 * Heavy cJSON parsing is deferred to the cloud thread main loop.
 *
 * OneNET Studio expects a response containing the same "id" as the request,
 * otherwise the platform cannot match the reply and reports "timeout".
 * Expected: {"id":"<request_id>","code":200,"msg":"success"}
 */
static void onenet_threshold_cmd_cb(uint8_t *recv_data, size_t recv_size,
                                     uint8_t **resp_data, size_t *resp_size)
{
    size_t n = recv_size < sizeof(g_cmd_buf) - 1 ? recv_size : sizeof(g_cmd_buf) - 1;
    rt_memcpy(g_cmd_buf, recv_data, n);
    g_cmd_buf[n] = '\0';
    g_cmd_pending = 1;

    rt_kprintf("[cloud] cmd: %s\n", g_cmd_buf);

    /* Extract "id" field via lightweight string search (avoid cJSON in callback) */
    char id_str[32] = {0};
    const char *p = strstr((const char *)recv_data, "\"id\"");
    if (p) {
        p += 4;                     /* skip past "id" */
        while (*p && (*p == ':' || *p == ' ')) p++;
        if (*p == '"') {
            /* string id: "12345" */
            p++;
            char *d = id_str;
            while (*p && *p != '"' && (size_t)(d - id_str) < sizeof(id_str) - 1)
                *d++ = *p++;
        } else {
            /* numeric id: 12345 */
            char *d = id_str;
            while (*p && *p != ',' && *p != '}' && *p != ' ' && (size_t)(d - id_str) < sizeof(id_str) - 1)
                *d++ = *p++;
        }
    }

    /* Build proper OneNET Studio response with matching id */
    char ack[128];
    int len;
    if (id_str[0]) {
        len = rt_snprintf(ack, sizeof(ack),
                         "{\"id\":\"%s\",\"code\":200,\"msg\":\"success\"}", id_str);
    } else {
        len = rt_snprintf(ack, sizeof(ack),
                         "{\"code\":200,\"msg\":\"success\"}");
    }

    *resp_data = (uint8_t *)ONENET_MALLOC(len + 1);
    if (*resp_data) {
        rt_memcpy(*resp_data, ack, len + 1);
        *resp_size = len;
    }
}

/* Parse saved command and update thresholds — called from cloud thread loop */
static void process_pending_cmd(void)
{
    if (!g_cmd_pending) return;
    g_cmd_pending = 0;

    cJSON *root = cJSON_Parse(g_cmd_buf);
    if (!root) return;

    cJSON *params = cJSON_GetObjectItem(root, "params");
    if (params) {
        cJSON *item;
        int updated = 0;

        #define TRY_UPDATE(key, var)                        \
            item = cJSON_GetObjectItem(params, key);        \
            if (item) {                                     \
                double val;                                 \
                if (cJSON_IsNumber(item)) {                 \
                    val = item->valuedouble;                \
                } else if (cJSON_IsObject(item)) {          \
                    cJSON *v = cJSON_GetObjectItem(item, "value"); \
                    if (v && cJSON_IsNumber(v)) val = v->valuedouble; \
                    else val = -9999;                       \
                } else {                                    \
                    val = -9999;                            \
                }                                           \
                if (val != -9999) {                         \
                    var = (float)val;                       \
                    rt_kprintf("[cloud] " key "=%.1f\n", var); \
                    updated = 1;                            \
                }                                           \
            }

        TRY_UPDATE("maxtemp_set",  g_temp_high);
        TRY_UPDATE("minitemp_set", g_temp_low);
        TRY_UPDATE("maxhum_set",   g_humi_high);
        TRY_UPDATE("minihum_set",  g_humi_low);

        #undef TRY_UPDATE

        /* 电机开关属性：{"motor_switch":1} 或 {"motor_switch":0} */
        cJSON *motor_item = cJSON_GetObjectItem(params, "motor_switch");
        if (motor_item) {
            int val = -1;
            if (cJSON_IsNumber(motor_item)) {
                val = motor_item->valueint;
            } else if (cJSON_IsObject(motor_item)) {
                cJSON *v = cJSON_GetObjectItem(motor_item, "value");
                if (v && cJSON_IsNumber(v)) val = v->valueint;
            }
            if (val == 1) {
                motor_forward();
                g_motor_state = 1;
                onenet_mqtt_upload_digit("motor_switch", 1);
                rt_kprintf("[cloud] motor ON (property)\n");
            } else if (val == 0) {
                motor_stop();
                g_motor_state = 0;
                onenet_mqtt_upload_digit("motor_switch", 0);
                rt_kprintf("[cloud] motor OFF (property)\n");
            }
        }

        if (updated) {
            threshold_save();   /* persist to flash for next boot */
        }
    }

    cJSON_Delete(root);
}

void cloud_thread_entry(void *param)
{
    rt_err_t ret;

    rt_thread_mdelay(5000);

    /* 循环重试WiFi连接，直到成功 */
    while (1) {
        rt_kprintf("[cloud] WiFi %s ... ", WIFI_SSID);
        ret = rt_wlan_connect(WIFI_SSID, WIFI_PASSWORD);
        if (ret == RT_EOK) {
            while (!rt_wlan_is_ready())
                rt_thread_mdelay(1000);
            break;
        }
        rt_kprintf("FAIL(%d), retry in 5s\n", ret);
        rt_thread_mdelay(5000);
    }

    rt_kprintf("OK\n");
    g_wifi_ok = RT_TRUE;

    rt_kprintf("[cloud] OneNET init...\n");
    onenet_mqtt_init();

    /* Register command callback */
    onenet_set_cmd_rsp_cb(onenet_threshold_cmd_cb);

    rt_thread_mdelay(3000);
    rt_kprintf("[cloud] ready\n");
    g_cloud_ok = RT_TRUE;

    struct gps_msg g;
    int periodic_tick = 0;
    int gps_tick = 0;        /* 5秒GPS轨迹上传计数器 */
    const char *prev_alarm = RT_NULL;   /* 上次报警状态 */
    int prev_motor = -1;                /* 上次电机状态 */

    while (1) {
        /* Process any pending cloud command */
        process_pending_cmd();

        /* 电机状态变化（本地按键改变）时同步到云端 */
        if (g_motor_state != prev_motor) {
            prev_motor = g_motor_state;
            onenet_mqtt_upload_digit("motor_switch", g_motor_state);
            rt_thread_mdelay(200);
            rt_kprintf("[cloud] motor_switch -> %d\n", g_motor_state);
        }

        /* 检查温湿度阈值 — 状态变化时上传报警 */
        if (g_data_ready) {
            const char *cur_alarm = RT_NULL;
            if      (g_temp > g_temp_high) cur_alarm = "TEMP_HIGH";
            else if (g_temp < g_temp_low)  cur_alarm = "TEMP_LOW";
            else if (g_humi > g_humi_high) cur_alarm = "HUMI_HIGH";
            else if (g_humi < g_humi_low)  cur_alarm = "HUMI_LOW";

            if (cur_alarm != prev_alarm) {
                prev_alarm = cur_alarm;
                const char *msg = cur_alarm ? cur_alarm : "NORMAL";
                onenet_mqtt_upload_string("alarm_status", msg);
                rt_thread_mdelay(200);
                rt_kprintf("[cloud] alarm: %s\n", msg);
            }
        }

        /* 每15秒周期上传温湿度 */
        if (++periodic_tick >= 15) {
            periodic_tick = 0;
            if (g_data_ready) {
                rt_kprintf("[cloud] upload: temp:%d.%dC humi:%d.%d%%\n",
                           (int)g_temp, (int)(g_temp * 10) % 10,
                           (int)g_humi, (int)(g_humi * 10) % 10);
                onenet_mqtt_upload_digit("temp_value", (int)g_temp);
                rt_thread_mdelay(200);
                onenet_mqtt_upload_digit("humidity_value", (int)g_humi);
                rt_thread_mdelay(200);
            }
        }

        /* 接收GPS新数据（非阻塞） */
        if (rt_mq_recv(mq_gps, &g, sizeof(g), 0) == RT_EOK) {
            rt_kprintf("[cloud] GPS updated\n");
        }

        /* 每5秒上传一次当前位置，用于云端轨迹显示 */
        if (++gps_tick >= 5) {
            gps_tick = 0;
            char loc[64];
            rt_snprintf(loc, sizeof(loc), "%d.%04d,%d.%04d",
                        (int)g.lat, (int)((g.lat - (int)g.lat) * 10000),
                        (int)g.lng, (int)((g.lng - (int)g.lng) * 10000));
            onenet_mqtt_upload_string("location", loc);
            rt_thread_mdelay(200);
            rt_kprintf("[cloud] trajectory: %s\n", loc);
        }

        /* RFID刷卡即时上传 */
        if (g_rfid_updated) {
            g_rfid_updated = 0;

            /* 单独上传RFID卡号 */
            onenet_mqtt_upload_string("rfid_uid", (const char *)g_rfid_uid);
            rt_thread_mdelay(200);

            /* 单独上传温湿度（字符串格式，与LCD一致） */
            char temp_str[16], humi_str[16];
            rt_snprintf(temp_str, sizeof(temp_str), "%d.%d",
                       (int)g_temp, (int)(g_temp * 10) % 10);
            rt_snprintf(humi_str, sizeof(humi_str), "%d.%d",
                       (int)g_humi, (int)(g_humi * 10) % 10);
            onenet_mqtt_upload_string("rfid_temp", temp_str);
            rt_thread_mdelay(200);
            onenet_mqtt_upload_string("rfid_humi", humi_str);
            rt_thread_mdelay(200);

            /* 打包绑定数据为JSON */
            char loc[64];
            rt_snprintf(loc, sizeof(loc), "%d.%04d,%d.%04d",
                        (int)g.lat, (int)((g.lat - (int)g.lat) * 10000),
                        (int)g.lng, (int)((g.lng - (int)g.lng) * 10000));

            char json[256];
            rt_snprintf(json, sizeof(json),
                       "{\"uid\":\"%s\",\"temp\":\"%s\",\"humi\":\"%s\",\"location\":\"%s\"}",
                       (const char *)g_rfid_uid, temp_str, humi_str, loc);
            onenet_mqtt_upload_string("rfid_record", json);
            rt_thread_mdelay(200);

            rt_kprintf("[cloud] RFID record: %s\n", json);
        }

        rt_thread_mdelay(1000);  /* 1秒循环，RFID响应更及时 */
    }
}