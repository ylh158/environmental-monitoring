/*
 * Threshold Storage — persist cloud-set thresholds in STM32 internal Flash
 */
#ifndef __THRESHOLD_STORAGE_H__
#define __THRESHOLD_STORAGE_H__

#include <rtthread.h>

/* Save current thresholds to Flash (called after cloud update) */
void threshold_save(void);

/* Load thresholds from Flash at boot; falls back to defaults if invalid */
void threshold_load(void);

#endif /* __THRESHOLD_STORAGE_H__ */