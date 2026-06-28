/*
 * GPS Thread — simulated positioning (logistics route)
 */
#include "threads.h"

extern rt_mq_t mq_gps;

void gps_thread_entry(void *param)
{
    struct gps_msg g;
    /* starting point — Shenzhen */
    g.lat = 22.5431f;
    g.lng = 113.9298f;

    while (1) {
        g.lat += 0.0005f;  /* ~55m north per tick */
        g.lng += 0.0003f;  /* ~30m east per tick */

        rt_mq_send(mq_gps, &g, sizeof(g));
        rt_thread_mdelay(5000);
    }
}
