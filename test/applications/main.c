/*
 * Smart Monitoring System — Main thread launcher
 * Multi-thread architecture based on RT-Thread message queues
 */

#include "threads.h"
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

/* WK_UP button — wake LCD on press */
#define BTN_WAKE   GET_PIN(C, 5)
#define BTN_EXIT   GET_PIN(C, 4)

static void btn_wake_cb(void *args)
{
    int v = 1;
    rt_mq_send(mq_display_wake, &v, sizeof(v));
}
static void btn_exit_cb(void *args)
{
    int v = 2;  /* 2 = exit to menu */
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

/* boot menu — select display mode */
void run_boot_menu(void)
{
    #define KEY_UP    GET_PIN(C, 0)
    #define KEY_DOWN  GET_PIN(C, 1)
    #define KEY_OK    GET_PIN(C, 5)

    const char *opts[] = { "> Always On", "> Power Save" };
    int sel = 0, last = -1;

    rt_pin_mode(KEY_UP,   PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(KEY_DOWN, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(KEY_OK,   PIN_MODE_INPUT_PULLUP);

    lcd_clear(WHITE);
    lcd_set_color(WHITE, BLUE);
    lcd_show_string(30, 20, 24, "Select Mode");

    while (1) {
        if (!rt_pin_read(KEY_UP) && sel > 0)   { sel--; rt_thread_mdelay(200); }
        if (!rt_pin_read(KEY_DOWN) && sel < 1) { sel++; rt_thread_mdelay(200); }

        if (sel != last) {
            lcd_set_color(WHITE, BLACK);
            lcd_show_string(40, 80,  24, sel == 0 ? "> Always On"  : "  Always On");
            lcd_show_string(40, 120, 24, sel == 1 ? "> Power Save" : "  Power Save");
            last = sel;
        }

        if (!rt_pin_read(KEY_OK)) {
            rt_thread_mdelay(300); /* debounce */
            g_lcd_mode = sel;
            break;
        }
        rt_thread_mdelay(50);
    }

    lcd_clear(WHITE);
    #undef KEY_UP
    #undef KEY_DOWN
    #undef KEY_OK
}

int main(void)
{
    rt_thread_t tid;

    /* hardware init */
    g_lcd = rt_device_find("lcd");
    if (g_lcd)
        rt_device_open(g_lcd, RT_DEVICE_OFLAG_RDWR);

    LCD_BackLightSet(80); /* backlight on for menu */
    run_boot_menu();

    /* create message queues */
    mq_sensor       = rt_mq_create("sensor", sizeof(struct sensor_msg), 4, RT_IPC_FLAG_FIFO);
    mq_rfid         = rt_mq_create("rfid",   sizeof(struct rfid_msg),   2, RT_IPC_FLAG_FIFO);
    mq_gps          = rt_mq_create("gps",    sizeof(struct gps_msg),    2, RT_IPC_FLAG_FIFO);
    mq_display_wake = rt_mq_create("wake",   sizeof(int),               4, RT_IPC_FLAG_FIFO);

    RT_ASSERT(mq_sensor && mq_rfid && mq_gps && mq_display_wake);

    /* WK_UP(PC5): wake LCD */
    rt_pin_mode(BTN_WAKE, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_WAKE, PIN_IRQ_MODE_FALLING, btn_wake_cb, RT_NULL);
    rt_pin_irq_enable(BTN_WAKE, PIN_IRQ_ENABLE);

    /* PC4: exit to mode menu */
    rt_pin_mode(BTN_EXIT, PIN_MODE_INPUT_PULLUP);
    rt_pin_attach_irq(BTN_EXIT, PIN_IRQ_MODE_FALLING, btn_exit_cb, RT_NULL);
    rt_pin_irq_enable(BTN_EXIT, PIN_IRQ_ENABLE);

    /* launch threads */
    tid = rt_thread_create("sensor",  sensor_thread_entry,  RT_NULL, STK_SENSOR,  PRI_SENSOR,  10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("rfid",    rfid_thread_entry,    RT_NULL, STK_RFID,    PRI_RFID,    10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("display", display_thread_entry, RT_NULL, STK_DISPLAY, PRI_DISPLAY, 10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("alarm",   alarm_thread_entry,   RT_NULL, STK_ALARM,   PRI_ALARM,   10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("cloud",   cloud_thread_entry,   RT_NULL, STK_CLOUD,   PRI_CLOUD,   10);
    if (tid) rt_thread_startup(tid);

    tid = rt_thread_create("gps",     gps_thread_entry,     RT_NULL, STK_GPS,     PRI_GPS,     10);
    if (tid) rt_thread_startup(tid);

    rt_kprintf("[main] 6 threads started, system running\n");

    return 0;
}
