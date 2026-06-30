/*
 * Display Thread — reads cached sensor globals, 3s refresh, dual-mode
 *
 * Unified button logic (wake queue values):
 *   LEFT  (PC5, w=1) = confirm / enter / wake LCD
 *   RIGHT (PC4, w=2) = exit / back (→ boot menu in main view)
 *   UP    (PC0, w=3) = navigate up / scroll up / enter records view
 *   DOWN  (PC1, w=4) = navigate down / scroll down
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

/* ===== RFID snapshot records ===== */
static struct rfid_record_t g_records[MAX_RECORDS];
static int g_record_head = 0;       /* ring buffer write index */
static int g_record_count = 0;      /* total records stored (up to MAX_RECORDS) */
static int g_records_scroll = 0;    /* scroll offset in records view */
static int g_show_records = 0;      /* 0=main view, 1=records list view */

/* Add a new record — captures current temp/humi/GPS into the ring buffer */
static void record_add(const char *uid, float temp, float humi,
                       float lat, float lng)
{
    struct rfid_record_t *r = &g_records[g_record_head];
    rt_strncpy(r->uid, uid, sizeof(r->uid) - 1);
    r->temp = temp;
    r->humi = humi;
    r->lat  = lat;
    r->lng  = lng;
    r->tick = rt_tick_get();

    g_record_head = (g_record_head + 1) % MAX_RECORDS;
    if (g_record_count < MAX_RECORDS)
        g_record_count++;
}

/* Get record by display index (0 = newest) */
static struct rfid_record_t *record_get(int idx)
{
    if (idx < 0 || idx >= g_record_count) return (struct rfid_record_t *)0;
    /* newest is at (head-1), going backwards */
    int pos = (g_record_head - 1 - idx) % MAX_RECORDS;
    if (pos < 0) pos += MAX_RECORDS;
    return &g_records[pos];
}

/* ===== Card UID display area ===== */
static char g_card_uid[32] = {0};
static int  g_card_present = 0;
#define CARD_AREA_Y1  128
#define CARD_AREA_Y2  195

static void draw_card_uid(const char *uid_str)
{
    lcd_fill(0, CARD_AREA_Y1, 239, CARD_AREA_Y2, WHITE);
    if (uid_str && uid_str[0]) {
        lcd_fill(0, CARD_AREA_Y1,      239, CARD_AREA_Y1 + 4, BLUE);
        lcd_fill(0, CARD_AREA_Y2 - 4, 239, CARD_AREA_Y2,      BLUE);
        lcd_set_color(WHITE, BLACK);
        lcd_show_string(10, CARD_AREA_Y1 + 10, 24, "Card UID:");
        lcd_set_color(WHITE, BLUE);
        lcd_show_string(10, CARD_AREA_Y1 + 32, 24, "%s", uid_str);
    }
}

/* ===== Authorized UID database ===== */
static struct {
    uint8_t uid[5];
    uint8_t len;
    const char *name;
} g_authorized[] = {
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
    return (const char *)0;
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

/* ===== Records list view ===== */
#define RECORDS_PER_PAGE  2

static void draw_records_view(void)
{
    lcd_clear(WHITE);

    /* Header */
    lcd_fill(0, 0, 239, 35, BLUE);
    lcd_set_color(BLUE, WHITE);
    int n = g_record_count > MAX_RECORDS ? MAX_RECORDS : g_record_count;
    lcd_show_string(20, 6, 24, "RFID Records (%d)", n);

    if (n == 0) {
        lcd_set_color(WHITE, BLACK);
        lcd_show_string(30, 100, 24, "No records yet");
        lcd_set_color(WHITE, GRAY);
        lcd_show_string(30, 140, 24, "Tap a card");
        return;
    }

    /* Clamp scroll */
    int max_scroll = n - RECORDS_PER_PAGE;
    if (max_scroll < 0) max_scroll = 0;
    if (g_records_scroll > max_scroll) g_records_scroll = max_scroll;

    /* Draw records — 24px font throughout */
    int y = 40;
    for (int i = g_records_scroll; i < n && i < g_records_scroll + RECORDS_PER_PAGE; i++) {
        struct rfid_record_t *r = record_get(i);
        if (!r) break;

        int num = n - i; /* display number (1-based, newest=1) */

        /* Line 1: Record number + Card UID */
        lcd_set_color(WHITE, BLACK);
        lcd_show_string(5, y, 24, "#%d  %s", num, r->uid);

        /* Line 2: Temperature and Humidity (integer conversion) */
        int t_int  = (int)r->temp;
        int t_dec  = (int)(r->temp * 10) % 10;
        int h_int  = (int)r->humi;
        int h_dec  = (int)(r->humi * 10) % 10;
        lcd_set_color(WHITE, BLUE);
        lcd_show_string(5, y + 28, 24, "  T:%d.%dC  H:%d.%d%%",
                        t_int, t_dec, h_int, h_dec);

        /* Line 3: GPS coordinates */
        int ld = (int)r->lat,  lf = (int)((r->lat - ld) * 10000);
        int gd = (int)r->lng,  gf = (int)((r->lng - gd) * 10000);
        lcd_set_color(WHITE, GRAY);
        lcd_show_string(5, y + 56, 24, "  %d.%04d, %d.%04d", ld, lf, gd, gf);

        /* Separator line between records */
        lcd_fill(10, y + 84, 229, y + 86, GRAY240);

        y += 90; /* per-record height */
    }

    /* Bottom hint */
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(30, 220, 16, "UP/DN:scroll  RGT:back");
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
        struct sensor_msg sm;
        if (rt_mq_recv(mq_sensor, &sm, sizeof(sm), rt_tick_from_millisecond(1000)) == RT_EOK) {
            /* sensor data received */
        }

        /* ===== unified button logic ===== */
        int w;
        while (rt_mq_recv(mq_display_wake, &w, sizeof(w), 0) == RT_EOK) {
            if (w == 1) {
                /* LEFT (PC5) = 上翻（记录页内） */
                lcd_bl_on();
                if (g_show_records) {
                    if (g_records_scroll > 0) {
                        g_records_scroll--;
                        draw_records_view();
                    }
                }
            } else if (w == 2) {
                /* RIGHT (PC4) = exit / back */
                lcd_bl_on();
                if (g_show_records) {
                    g_show_records = 0;
                    draw_static();
                    next = 0;
                } else {
                    lcd_clear(WHITE);
                    run_boot_menu();
                    /* Drain stale interrupts queued during polling */
                    while (rt_mq_recv(mq_display_wake, &w, sizeof(w), 0) == RT_EOK) {}
                    draw_static();
                    next = 0;
                }
            } else if (w == 3) {
                /* UP (PC0) = 进入记录列表（仅主界面有效） */
                lcd_bl_on();
                if (!g_show_records) {
                    g_show_records = 1;
                    g_records_scroll = 0;
                    draw_records_view();
                }
            } else if (w == 4) {
                /* DOWN (PC1) = navigate down */
                if (g_show_records) {
                    lcd_bl_on();
                    int n = g_record_count > MAX_RECORDS ? MAX_RECORDS : g_record_count;
                    int max_scroll = n - RECORDS_PER_PAGE;
                    if (max_scroll < 0) max_scroll = 0;
                    if (g_records_scroll < max_scroll) {
                        g_records_scroll++;
                        draw_records_view();
                    }
                }
            }
        }

        /* ===== RFID — process in ALL views to capture new records ===== */
        int got_new_card = 0;
        struct rfid_msg r;
        if (rt_mq_recv(mq_rfid, &r, sizeof(r), 0) == RT_EOK) {
            lcd_bl_on();
            g_card_present = 1;

            char uid_str[32];
            char *p = uid_str;
            for (int i = 0; i < r.len; i++) {
                int n = rt_snprintf(p, sizeof(uid_str) - (p - uid_str),
                                   "%02X%s", r.uid[i], (i + 1 < r.len) ? ":" : "");
                if (n > 0) p += n;
            }
            rt_strncpy(g_card_uid, uid_str, sizeof(g_card_uid) - 1);

            /* Capture current GPS (non-blocking) */
            struct gps_msg gps_now;
            if (rt_mq_recv(mq_gps, &gps_now, sizeof(gps_now), 0) != RT_EOK) {
                gps_now = g;
            }

            /* Create a snapshot record */
            record_add(uid_str, g_temp, g_humi, gps_now.lat, gps_now.lng);
            got_new_card = 1;

            /* Update main view display (only visible when not in records view) */
            if (!g_show_records) {
                const char *who = identify_card(&r);
                char status[64];
                if (who)
                    rt_snprintf(status, sizeof(status), "Hi %s!", who);
                else
                    rt_snprintf(status, sizeof(status), "UID:%s", uid_str);
                draw_card_uid(g_card_uid);
                upd_status(status);
            }
        }

        /* ===== If in records view: redraw on new card, then skip ===== */
        if (g_show_records) {
            if (got_new_card) {
                draw_records_view();
            }
            if (g_lcd_mode == 1 && lcd_on && rt_tick_get() > off_time)
                lcd_bl_off();
            continue;
        }

        /* ===== Main view: 3s refresh ===== */
        if (g_data_ready && rt_tick_get() >= next) {
            next = rt_tick_get() + rt_tick_from_millisecond(3000);
            if (g_lcd_mode == 0) lcd_bl_on();

            upd_temp(g_temp);
            upd_humi(g_humi);

            if (g_card_present)
                draw_card_uid(g_card_uid);

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