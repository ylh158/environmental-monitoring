/*
 * Smart Monitoring System — Thread Definitions & Message Queues
 */

#ifndef __THREADS_H__
#define __THREADS_H__

#include <rtthread.h>

/* ---- Message types ---- */
struct sensor_msg {
    float temp;
    float humi;
};

struct rfid_msg {
    uint8_t uid[5];
    uint8_t len;
};

struct gps_msg {
    float lat;
    float lng;
};

/* ---- Message queues (extern) ---- */
extern rt_mq_t mq_sensor;
extern rt_mq_t mq_rfid;
extern rt_mq_t mq_gps;
extern rt_mq_t mq_display_wake;

/* ---- Adjustable thresholds (settable from OneNET cloud) ---- */
extern volatile float g_temp_high;
extern volatile float g_temp_low;
extern volatile float g_humi_high;
extern volatile float g_humi_low;

/* ---- Cached sensor data (zero-copy for display) ---- */
extern volatile float g_temp;
extern volatile float g_humi;
extern volatile int   g_data_ready;

/* ---- Cached RFID data (for cloud upload) ---- */
extern volatile char  g_rfid_uid[32];   /* 最近一次识别的卡号 */
extern volatile int   g_rfid_updated;   /* 是否有新卡号待上传 */

/* ---- WiFi credentials ---- */
#define WIFI_SSID       "vivo"
#define WIFI_PASSWORD   "11050712"

/* ---- RFID bound records ---- */
#define MAX_RECORDS    20

struct rfid_record_t {
    char    uid[32];       /* card UID string (e.g. "AA:BB:CC:DD") */
    float   temp;          /* temperature at scan time */
    float   humi;          /* humidity at scan time */
    float   lat;           /* GPS latitude at scan time */
    float   lng;           /* GPS longitude at scan time */
    uint32_t tick;         /* system tick when recorded */
};

/* ---- Connection flags (defined in thread_cloud.c) ---- */
extern rt_bool_t g_wifi_ok;
extern rt_bool_t g_cloud_ok;

/* ---- Motor state (defined in thread_cloud.c) ---- */
extern volatile int g_motor_state;  /* 0=OFF, 1=ON */

/* ---- Thread entries ---- */
void sensor_thread_entry(void *param);
void rfid_thread_entry(void *param);
void display_thread_entry(void *param);
void alarm_thread_entry(void *param);
void cloud_thread_entry(void *param);
void gps_thread_entry(void *param);
void run_boot_menu(void);

/* ---- LCD helper ---- */
void lcd_show_ui(float temp, float humi, const char *msg);

/* ---- RC522 extern ---- */
extern int rc522_read_card(uint8_t *uid, uint8_t *uid_len);

#endif