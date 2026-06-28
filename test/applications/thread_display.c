/*
 * Display Thread — reads cached sensor globals, 3s refresh, dual-mode
 * PC4 FALLING → mq_display_wake(2) → re-enter boot menu
 */

#include "threads.h"
#include "drv_lcd.h"
#include <board.h>

int g_lcd_mode = 0;

extern rt_mq_t mq_sensor;
extern rt_mq_t mq_rfid;
extern rt_mq_t mq_gps;
extern rt_mq_t mq_display_wake;

static int lcd_on = 0;
static rt_tick_t off_time = 0;

/* ===== Card UID display area ===== */
static char g_card_uid[32] = {0};         /* 当前显示的卡号字符串 */
static int  g_card_present = 0;           /* 是否有卡正在显示 */
#define CARD_AREA_Y1  128                  /* 卡号显示区域上边界 */
#define CARD_AREA_Y2  195                  /* 卡号显示区域下边界 */

/* 绘制卡号显示区域 */
static void draw_card_uid(const char *uid_str)
{
    /* 清空卡号显示区域 */
    lcd_fill(0, CARD_AREA_Y1, 239, CARD_AREA_Y2, WHITE);

    if (uid_str && uid_str[0]) {
        /* 上下蓝色装饰边框（加宽到4px更醒目） */
        lcd_fill(0, CARD_AREA_Y1,      239, CARD_AREA_Y1 + 4, BLUE);
        lcd_fill(0, CARD_AREA_Y2 - 4, 239, CARD_AREA_Y2,      BLUE);

        /* 标签 "Card UID:" */
        lcd_set_color(WHITE, BLACK);
        lcd_show_string(10, CARD_AREA_Y1 + 10, 24, "Card UID:");

        /* 卡号值 — 用蓝色+大字显示 */
        lcd_set_color(WHITE, BLUE);
        lcd_show_string(10, CARD_AREA_Y1 + 32, 24, "%s", uid_str);
    }
}

/* ===== Authorized UID database (identity recognition) =====
   Add card UIDs here to enable identity recognition.
   Format: { UID0, UID1, UID2, UID3, len }. */
static struct {
    uint8_t uid[5];
    uint8_t len;
    const char *name;
} g_authorized[] = {
    /* Example cards — replace with your card's UID */
    { { 0x12, 0x34, 0x56, 0x78 }, 4, "User 1" },
    { { 0xAA, 0xBB, 0xCC, 0xDD }, 4, "User 2" },
};
#define NUM_AUTHORIZED  (sizeof(g_authorized) / sizeof(g_authorized[0]))

static const char *identify_card(struct rfid_msg *r)
{
    for (int i = 0; i < (int)NUM_AUTHORIZED; i++) {
        if (r->len != g_authorized[i].len) continue;
        if (rt_memcmp(r->uid, g_authorized[i].uid, r->len) == 0)
            return g_authorized[i].name;
    }
    return (const char *)0; /* not authorized */
}

static void lcd_bl_on(void)
{
    if (!lcd_on) { LCD_BackLightSet(80); lcd_on = 1; }
    off_time = rt_tick_get() + rt_tick_from_millisecond(10000);
}
static void lcd_bl_off(void)
{
    if (lcd_on) { LCD_BackLightSet(0); lcd_on = 0; }
}

static void draw_static(void)
{
    lcd_clear(WHITE);
    lcd_fill(0, 0, 239, 35, BLUE);
    lcd_set_color(BLUE, WHITE);
    lcd_show_string(40, 6, 24, "AHT10 Monitor");
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 48, 24, "Temp:");
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 88, 24, "Humi:");
}

static void upd_temp(float t) {
    lcd_fill(100, 44, 210, 72, GRAY240);
    lcd_set_color(GRAY240, RED);
    lcd_show_string(110, 48, 24, "%d.%dC", (int)t, (int)(t * 10) % 10);
}
static void upd_humi(float h) {
    lcd_fill(100, 84, 210, 112, GRAY240);
    lcd_set_color(GRAY240, BLUE);
    lcd_show_string(110, 88, 24, "%d.%d%%", (int)h, (int)(h * 10) % 10);
}
static void upd_status(const char *m) {
    lcd_fill(0, 200, 239, 239, GRAY175);
    lcd_set_color(GRAY175, BLACK);
    lcd_show_string(10, 208, 24, "%s", m);
}

void display_thread_entry(void *param)
{
    struct gps_msg g = {0};
    char st[64];
    rt_tick_t next = 0;

    LCD_BackLightSet(80); lcd_on = 1;
    off_time = rt_tick_get() + rt_tick_from_millisecond(10000);
    draw_static();

    while (1) {
        /* wait 1s for sensor msg or wake event */
        int evt = 0;
        struct sensor_msg sm;
        if (rt_mq_recv(mq_sensor, &sm, sizeof(sm), rt_tick_from_millisecond(1000)) == RT_EOK)
            evt = 1;

        /* drain wake queue — 1=button, 2=exit-menu, 3=alarm */
        int w;
        while (rt_mq_recv(mq_display_wake, &w, sizeof(w), 0) == RT_EOK) {
            if (w == 2) { /* PC4 → re-enter menu */
                lcd_bl_on(); lcd_clear(WHITE);
                run_boot_menu();
                draw_static();
                next = 0; /* force immediate refresh */
            } else {
                lcd_bl_on();
            }
        }

        /* RFID — identity recognition */
        struct rfid_msg r;
        if (rt_mq_recv(mq_rfid, &r, sizeof(r), 0) == RT_EOK) {
            lcd_bl_on();
            g_card_present = 1;

            /* Format UID for display: AA:BB:CC:DD */
            char uid_str[32];
            char *p = uid_str;
            for (int i = 0; i < r.len; i++) {
                int n = rt_snprintf(p, sizeof(uid_str) - (p - uid_str),
                                   "%02X%s", r.uid[i], (i + 1 < r.len) ? ":" : "");
                if (n > 0) p += n;
            }
            rt_strncpy(g_card_uid, uid_str, sizeof(g_card_uid) - 1);

            const char *who = identify_card(&r);
            char status[64];
            if (who) {
                rt_snprintf(status, sizeof(status), "Hi %s!", who);
            } else {
                rt_snprintf(status, sizeof(status), "UID:%s", uid_str);
            }

            draw_card_uid(g_card_uid);
            upd_status(status);
        }

        /* 3s refresh from global cache */
        if (g_data_ready && rt_tick_get() >= next) {
            next = rt_tick_get() + rt_tick_from_millisecond(3000);
            if (g_lcd_mode == 0) lcd_bl_on();

            upd_temp(g_temp);
            upd_humi(g_humi);

            /* 如果有卡号，保持显示不被覆盖 */
            if (g_card_present) {
                draw_card_uid(g_card_uid);
            }

            if (rt_mq_recv(mq_gps, &g, sizeof(g), 0) == RT_EOK) {
                int ld = (int)g.lat,  lf = (int)((g.lat - ld) * 10000);
                int gd = (int)g.lng,  gf = (int)((g.lng - gd) * 10000);
                rt_snprintf(st, sizeof(st), "%d.%04d,%d.%04d", ld, lf, gd, gf);
            } else {
                rt_snprintf(st, sizeof(st), "Tap Card...");
            }
            upd_status(st);
        }

        /* power-save auto-off */
        if (g_lcd_mode == 1 && lcd_on && rt_tick_get() > off_time)
            lcd_bl_off();
    }
}