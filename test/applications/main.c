#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "aht10.h"
#include "drv_lcd.h"

#define I2C_BUS_NAME    "i2c3"

#define LED_PA7     GET_PIN(A, 7)
#define LED_PF2     GET_PIN(F, 2)

static aht10_device_t g_aht = RT_NULL;
static rt_device_t    g_lcd = RT_NULL;

static void lcd_show_ui(float temp, float humi)
{
    lcd_clear(WHITE);

    // 标题栏
    lcd_fill(0, 0, 239, 38, BLUE);
    lcd_set_color(BLUE, WHITE);
    lcd_show_string(40, 8, 24, "AHT10 Monitor");

    // 温度行
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 65, 24, "Temp:");
    lcd_fill(100, 60, 210, 90, GRAY240);
    lcd_set_color(GRAY240, RED);
    lcd_show_string(110, 65, 24, "%d.%dC", (int)temp, (int)(temp * 10) % 10);
    // 湿度行
    lcd_set_color(WHITE, BLACK);
    lcd_show_string(20, 125, 24, "Humi:");
    lcd_fill(100, 120, 210, 150, GRAY240);
    lcd_set_color(GRAY240, BLUE);
    lcd_show_string(110, 125, 24,"%d.%d%%", (int)humi, (int)(humi * 10) % 10);
}

int main(void)
{
    // 关灯
    rt_pin_mode(LED_PA7, PIN_MODE_OUTPUT);
    rt_pin_mode(LED_PF2, PIN_MODE_OUTPUT);
    rt_pin_write(LED_PA7, PIN_LOW);
    rt_pin_write(LED_PF2, PIN_LOW);

    // 初始化 AHT10
    g_aht = aht10_init(I2C_BUS_NAME);
    rt_thread_mdelay(500);

    // 初始化 LCD
    g_lcd = rt_device_find("lcd");
    if (g_lcd)
    {
        rt_device_open(g_lcd, RT_DEVICE_OFLAG_RDWR);
        lcd_clear(WHITE);
    }

    while (1)
    {
        if (g_aht)
        {
            float t = aht10_read_temperature(g_aht);
            float h = aht10_read_humidity(g_aht);

            rt_kprintf("temp:%d.%dC\n", (int)t, (int)(t * 10) % 10);
            rt_kprintf("humi:%d.%d%%\n", (int)h, (int)(h * 10) % 10);

            lcd_show_ui(t, h);
        }
        rt_thread_mdelay(2000);
    }
}
