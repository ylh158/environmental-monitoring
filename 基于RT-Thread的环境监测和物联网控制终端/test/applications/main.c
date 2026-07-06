/*
 * Smart Monitoring System — Main thread launcher
 * Multi-thread architecture based on RT-Thread message queues
 *
 * Unified button mapping:
 *   UP   (PC0) = ↑ navigate up
 *   DOWN (PC1) = ↓ navigate down
 *   LEFT (PC5) = ✓ confirm / enter / wake LCD
 *   RIGHT(PC4) = ← exit / back (→ boot menu)
 */

#include "threads.h"
#include "threshold_storage.h"
#include "motor.h"
#include <board.h>
#include <wlan_mgnt.h>
#include <onenet.h>
#include "aht10.h"
#include "drv_lcd.h"

#define I2C_BUS_NAME    "i2c3"

/* ---- Message queues ---- */
rt_mq_t mq_sensor       = RT_NULL;
rt_mq_t mq_rfid         = RT_NULL;
rt_mq_t mq_gps          = RT_NULL;
rt_mq_t mq_display_wake = RT_NULL;

static rt_device_t g_lcd = RT_NULL;

/* Unified button mapping:
 *   UP   (PC0)  = ↑ navigate up
 *   DOWN (PC1)  = ↓ navigate down
 *   LEFT (PC5)  = ✓ confirm / enter
 *   RIGHT(PC4)  = ← exit / back
 */
#define BTN_UP     GET_PIN(C, 0)
#define BTN_DOWN   GET_PIN(C, 1)
#define BTN_LEFT   GET_PIN(C, 5)
#define BTN_RIGHT  GET_PIN(C, 4)

static void btn_left_cb(void *args)
{
    int v = 1;  /* confirm / enter */
    rt_mq_send(mq_display_wake, &v, sizeof(v));
}
static void btn_right_cb(void *args)
{
    int v = 2;  /* exit / back */
    rt_mq_send(mq_display_wake, &v, sizeof(v));
}
static void btn_up_cb(void *args)
{
    int v = 3;  /* up / scroll up */
    rt_mq_send(mq_display_wake, &v, sizeof(v));
}
static void btn_down_cb(void *args)
{
    int v = 4;  /* down / scroll down */
    rt_mq_send(mq_display_wake, &v, sizeof(v));
}

/* ---- Thread stack/priority ---- */
#define STK_SENSOR   1024
#define STK_RFID     1024
#define STK_DISPLAY  2048
#define STK_ALARM     512
#define STK_CLOUD    3072
#define STK_GPS      1024

#define PRI_SENSOR   12
#define PRI_RFID     14
#define PRI_DISPLAY  16
#define PRI_ALARM    13
#define PRI_CLOUD    15
#define PRI_GPS      15

extern int g_lcd_mode;  /* defined in thread_display.c */

/* forward declarations */
static void show_quick_status(void);
static void show_fan_control(void);

/* boot menu — select display mode */
void run_boot_menu(void)
{
    #define KEY_UP    GET_PIN(C, 0)
    #define KEY_DOWN  GET_PIN(C, 1)
    #define KEY_LEFT  GET_PIN(C, 5)

    int sel = 0, last = -1;

    rt_pin_mode(KEY_UP,   PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(KEY_DOWN, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(KEY_LEFT, PIN_MODE_INPUT_PULLUP);

    lcd_clear(WHITE);
    lcd_set_color(WHITE, BLUE);
    lcd_show_string(30, 20, 24, "Select Mode");

    while (1) {
        if (!rt_pin_read(KEY_LEFT) && sel > 0)   { sel--; rt_thread_mdelay(200); }
        if (!rt_pin_read(KEY_DOWN) && sel < 3) { sel++; rt_thread_mdelay(200); }

        if (sel != last) {
            lcd_set_color(WHITE, BLACK);
            lcd_show_string(40, 60,  24, sel == 0 ? "> Always On"      : "  Always On");
            lcd_show_string(40, 90,  24, sel == 1 ? "> Power Save"     : "  Power Save");
            lcd_show_string(40, 120, 24, sel == 2 ? "> Connect Status" : "  Connect Status");
            lcd_show_string(40, 150, 24, sel == 3 ? "> Fan Control"    : "  Fan Control");
            last = sel;
        }

        if (!rt_pin_read(KEY_UP)) {
            rt_thread_mdelay(300);
            if (sel == 2) {
                show_quick_status();
                lcd_clear(WHITE);
                lcd_set_color(WHITE, BLUE);
                lcd_show_string(30, 20, 24, "Select Mode");
                last = -1;
                continue;
            }
            if (sel == 3) {
                show_fan_control();
                lcd_clear(WHITE);
                lcd_set_color(WHITE, BLUE);
                lcd_show_string(30, 20, 24, "Select Mode");
                last = -1;
                continue;
            }
            g_lcd_mode = sel;
            break;
        }
        rt_thread_mdelay(50);
    }

    lcd_clear(WHITE);
    #undef KEY_UP
    #undef KEY_DOWN
    #undef KEY_LEFT
}

/* 快速状态查看 — 只显示当前连接状态，按RIGHT返回菜单 */
static void show_quick_status(void)
{
    #define KEY_RIGHT_QS   GET_PIN(C, 4)

    lcd_clear(WHITE);
    lcd_fill(0, 0, 239, 35, BLUE);
    lcd_set_color(BLUE, WHITE);
    lcd_show_string(30, 6, 24, "Connection Status");

    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 60, 20, "SSID: %s", WIFI_SSID);

    lcd_set_color(WHITE, g_wifi_ok  ? GREEN : RED);
    lcd_show_string(20, 100, 24, "WiFi:  %s", g_wifi_ok  ? "Connected" : "Disconnected");

    lcd_set_color(WHITE, g_cloud_ok ? GREEN : RED);
    lcd_show_string(20, 140, 24, "Cloud: %s", g_cloud_ok ? "Connected" : "Disconnected");

    lcd_set_color(WHITE, BLACK);
    lcd_show_string(30, 200, 16, "Press RIGHT to exit");

    rt_pin_mode(KEY_RIGHT_QS, PIN_MODE_INPUT_PULLUP);
    while (1) {
        if (!rt_pin_read(KEY_RIGHT_QS)) {
            rt_thread_mdelay(300);
            break;
        }
        rt_thread_mdelay(50);
    }
    #undef KEY_RIGHT_QS
}

/* 风扇控制界面 — 物理UP(PC5)=开, 物理DOWN=关, RIGHT=退出 */
static void show_fan_control(void)
{
    lcd_clear(WHITE);
    lcd_fill(0, 0, 239, 35, BLUE);
    lcd_set_color(BLUE, WHITE);
    lcd_show_string(40, 6, 24, "Fan Control");

    lcd_set_color(WHITE, BLACK);
    lcd_show_string(30, 65, 24, "Current: ");
    lcd_set_color(WHITE, g_motor_state ? GREEN : RED);
    lcd_show_string(160, 65, 24, "%s", g_motor_state ? "ON" : "OFF");

    lcd_set_color(WHITE, BLACK);
    lcd_show_string(30, 110, 24, "UP   = Turn ON");
    lcd_show_string(30, 145, 24, "DOWN = Turn OFF");
    lcd_show_string(30, 210, 16, "Press RIGHT to exit");

    /* 等待所有按键释放，避免菜单确认键误触发 */
    while (!rt_pin_read(BTN_LEFT) || !rt_pin_read(BTN_DOWN) ||
           !rt_pin_read(BTN_UP) || !rt_pin_read(BTN_RIGHT))
        rt_thread_mdelay(20);

    int last_state = g_motor_state;         /* 跟踪云端远程改变 */

    while (1) {
        /* 检测云端远程改变的电机状态并刷新LCD */
        if (g_motor_state != last_state) {
            last_state = g_motor_state;
            lcd_fill(160, 55, 235, 85, WHITE);
            lcd_set_color(WHITE, g_motor_state ? GREEN : RED);
            lcd_show_string(160, 65, 24, "%s", g_motor_state ? "ON" : "OFF");
        }
        if (!rt_pin_read(BTN_LEFT)) {       /* 物理UP键(PC5) = 开 */
            motor_forward();
            g_motor_state = 1;
            lcd_fill(160, 55, 235, 85, WHITE);
            lcd_set_color(WHITE, GREEN);
            lcd_show_string(160, 65, 24, "ON");
            rt_thread_mdelay(300);
        }
        if (!rt_pin_read(BTN_DOWN)) {       /* 物理DOWN键(PC1) = 关 */
            motor_stop();
            g_motor_state = 0;
            lcd_fill(160, 55, 235, 85, WHITE);
            lcd_set_color(WHITE, RED);
            lcd_show_string(160, 65, 24, "OFF");
            rt_thread_mdelay(300);
        }
        if (!rt_pin_read(BTN_RIGHT)) {      /* RIGHT = 退出 */
            rt_thread_mdelay(300);
            break;
        }
        rt_thread_mdelay(50);
    }}

/* Connection status screen — shown after mode selection, before main UI */
static void show_connect_status(void)
{
    rt_tick_t deadline;

    lcd_fill(0, 0, 239, 35, BLUE);
    lcd_set_color(BLUE, WHITE);
    lcd_show_string(20, 6, 24, "System Connecting...");

    /* WiFi SSID */
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 60, 16, "SSID:");
    lcd_set_color(WHITE, BLUE);
    lcd_show_string(70, 60, 16, "%s", WIFI_SSID);

    /* Wait for WiFi */
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 110, 24, "WiFi: Connecting...");
    deadline = rt_tick_get() + rt_tick_from_millisecond(15000);
    while (!g_wifi_ok && rt_tick_get() < deadline)
        rt_thread_mdelay(500);

    lcd_fill(0, 104, 239, 134, WHITE);
    if (g_wifi_ok) {
        lcd_set_color(WHITE, GREEN);
        lcd_show_string(20, 110, 24, "WiFi: OK");

        /* Wait for Cloud */
        lcd_set_color(WHITE, BLACK);
        lcd_show_string(20, 150, 24, "Cloud: Connecting...");
        deadline = rt_tick_get() + rt_tick_from_millisecond(10000);
        while (!g_cloud_ok && rt_tick_get() < deadline)
            rt_thread_mdelay(500);

        lcd_fill(0, 144, 239, 174, WHITE);
        if (g_cloud_ok) {
            lcd_set_color(WHITE, GREEN);
            lcd_show_string(20, 150, 24, "Cloud: OK");
        } else {
            lcd_set_color(WHITE, RED);
            lcd_show_string(20, 150, 24, "Cloud: FAIL");
        }
    } else {
        lcd_set_color(WHITE, RED);
        lcd_show_string(20, 110, 24, "WiFi: FAIL");
    }

    rt_thread_mdelay(2000);
    lcd_clear(WHITE);
}

int main(void)
{
    rt_thread_t tid;

    /* hardware init */
    g_lcd = rt_device_find("lcd");
    if (g_lcd)
        rt_device_open(g_lcd, RT_DEVICE_OFLAG_RDWR);

    LCD_BackLightSet(80);
    run_boot_menu();

    /* Load thresholds saved in flash (override code defaults if valid) */
    threshold_load();

    /* create message queues */
    mq_sensor       = rt_mq_create("sensor", sizeof(struct sensor_msg), 4, RT_IPC_FLAG_FIFO);
    mq_rfid         = rt_mq_create("rfid",   sizeof(struct rfid_msg),   2, RT_IPC_FLAG_FIFO);
    mq_gps          = rt_mq_create("gps",    sizeof(struct gps_msg),    2, RT_IPC_FLAG_FIFO);
    mq_display_wake = rt_mq_create("wake",   sizeof(int),               4, RT_IPC_FLAG_FIFO);

    RT_ASSERT(mq_sensor && mq_rfid && mq_gps && mq_display_wake);

    /* LEFT (PC5): confirm / enter / wake LCD */
    rt_pin_mode(BTN_LEFT, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_LEFT, PIN_IRQ_MODE_FALLING, btn_left_cb, RT_NULL);
    rt_pin_irq_enable(BTN_LEFT, PIN_IRQ_ENABLE);

    /* RIGHT (PC4): exit / back */
    rt_pin_mode(BTN_RIGHT, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_RIGHT, PIN_IRQ_MODE_FALLING, btn_right_cb, RT_NULL);
    rt_pin_irq_enable(BTN_RIGHT, PIN_IRQ_ENABLE);

    /* UP (PC0): navigate up */
    rt_pin_mode(BTN_UP, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_UP, PIN_IRQ_MODE_FALLING, btn_up_cb, RT_NULL);
    rt_pin_irq_enable(BTN_UP, PIN_IRQ_ENABLE);

    /* DOWN (PC1): navigate down */
    rt_pin_mode(BTN_DOWN, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_DOWN, PIN_IRQ_MODE_FALLING, btn_down_cb, RT_NULL);
    rt_pin_irq_enable(BTN_DOWN, PIN_IRQ_ENABLE);

    /* Launch non-display threads first */
    tid = rt_thread_create("sensor",  sensor_thread_entry,  RT_NULL, STK_SENSOR,  PRI_SENSOR,  10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("rfid",    rfid_thread_entry,    RT_NULL, STK_RFID,    PRI_RFID,    10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("alarm",   alarm_thread_entry,   RT_NULL, STK_ALARM,   PRI_ALARM,   10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("cloud",   cloud_thread_entry,   RT_NULL, STK_CLOUD,   PRI_CLOUD,   10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("gps",     gps_thread_entry,     RT_NULL, STK_GPS,     PRI_GPS,     10);
    if (tid) rt_thread_startup(tid);

    /* Show WiFi/Cloud connection status */
    show_connect_status();

    /* Launch display thread last — starts with clean screen */
    tid = rt_thread_create("display", display_thread_entry, RT_NULL, STK_DISPLAY, PRI_DISPLAY, 10);
    if (tid) rt_thread_startup(tid);

    rt_kprintf("[main] 6 threads started\n");

    return 0;
}