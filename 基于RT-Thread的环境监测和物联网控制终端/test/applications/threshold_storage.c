/*
 * Threshold Storage — persist cloud-set thresholds in STM32 internal Flash
 *
 * Uses the last Flash sector (Sector 11, 0x080E0000, 128 KB) to store
 * a small struct with a magic + checksum for validity detection.
 *
 * On boot:  threshold_load() checks flash; if valid, overrides defaults.
 * On cloud: threshold_save() erases sector and writes new values.
 */
#include "threshold_storage.h"
#include "threads.h"
#include <board.h>
#include <stddef.h>

/* ---- Flash layout ---- *
 * STM32F407ZG 1MB Flash, Sector 11 = 128KB @ 0x080E0000
 * We only use the first few bytes — the rest remains untouched.
 */
#define THRESHOLD_FLASH_SECTOR      FLASH_SECTOR_11
#define THRESHOLD_FLASH_ADDR        ((uint32_t)0x080E0000)
#define THRESHOLD_MAGIC             0xDEADBEEF

/* Data layout stored in flash (must be 32-bit aligned) */
typedef struct {
    uint32_t magic;         /* validity marker */
    float    temp_high;
    float    temp_low;
    float    humi_high;
    float    humi_low;
    uint32_t checksum;      /* simple integrity check */
} __attribute__((packed)) threshold_data_t;

/* Compute XOR checksum over the payload fields */
static uint32_t calc_checksum(const threshold_data_t *d)
{
    uint32_t c = 0;
    uint32_t *p = (uint32_t *)&d->temp_high;
    size_t n = (sizeof(threshold_data_t) - offsetof(threshold_data_t, temp_high)
                - sizeof(d->checksum)) / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++)
        c ^= p[i];
    return c;
}

/* ---- Public API ---- */

void threshold_save(void)
{
    threshold_data_t d;
    HAL_StatusTypeDef hal;

    d.magic     = THRESHOLD_MAGIC;
    d.temp_high = g_temp_high;
    d.temp_low  = g_temp_low;
    d.humi_high = g_humi_high;
    d.humi_low  = g_humi_low;
    d.checksum  = calc_checksum(&d);

    /* Unlock Flash */
    HAL_FLASH_Unlock();

    /* Erase the entire sector (required before write on STM32F4) */
    FLASH_Erase_Sector(THRESHOLD_FLASH_SECTOR, FLASH_VOLTAGE_RANGE_3);

    /* Program 4 × 32-bit words (magic + 4 floats) */
    uint32_t *src = (uint32_t *)&d;
    for (int i = 0; i < 5; i++) {
        hal = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                THRESHOLD_FLASH_ADDR + i * 4, src[i]);
        if (hal != HAL_OK) {
            rt_kprintf("[storage] FLASH write error at +0x%04X\n", i * 4);
            break;
        }
    }
    /* Program checksum separately */
    hal = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                            THRESHOLD_FLASH_ADDR + 5 * 4, d.checksum);
    if (hal != HAL_OK)
        rt_kprintf("[storage] FLASH checksum write error\n");

    HAL_FLASH_Lock();

    rt_kprintf("[storage] thresholds saved to flash\n");
}

void threshold_load(void)
{
    threshold_data_t *d = (threshold_data_t *)THRESHOLD_FLASH_ADDR;

    /* Check magic number */
    if (d->magic != THRESHOLD_MAGIC) {
        rt_kprintf("[storage] no valid data in flash (magic=0x%08X), using defaults\n",
                   (unsigned)d->magic);
        return;
    }

    /* Verify checksum */
    uint32_t saved_ck = d->checksum;
    /* Temporarily clear checksum field to compute checksum of other fields */
    /* Since we read from flash, we compute using a copy */
    threshold_data_t tmp;
    rt_memcpy(&tmp, d, sizeof(tmp));
    tmp.checksum = 0;
    if (calc_checksum(&tmp) != saved_ck) {
        rt_kprintf("[storage] checksum mismatch, using defaults\n");
        return;
    }

    /* All good — override defaults with saved values */
    g_temp_high = d->temp_high;
    g_temp_low  = d->temp_low;
    g_humi_high = d->humi_high;
    g_humi_low  = d->humi_low;

    rt_kprintf("[storage] thresholds restored: "
               "temp[%.1f~%.1f] humi[%.1f~%.1f]\n",
               g_temp_low, g_temp_high, g_humi_low, g_humi_high);
}