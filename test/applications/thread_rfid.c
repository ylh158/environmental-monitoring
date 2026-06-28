/*
 * RFID Thread — poll RC522 for cards, publish UID to message queue
 * Uses corrected FIFO-based transceive from rc522_port.c
 */

#include "threads.h"
#include <board.h>

extern rt_mq_t mq_rfid;
extern int rc522_read_card(uint8_t *uid, uint8_t *uid_len);
extern void rc522_full_init(void);
extern void rc522_reinit_pins(void);

/* 缓存最近一次卡号，供云端线程读取 */
volatile char  g_rfid_uid[32] = {0};
volatile int   g_rfid_updated = 0;

void rfid_thread_entry(void *param)
{
    uint8_t uid[5], uid_len;
    rt_thread_mdelay(2000);

    while (1) {
        /* Poll for a card every 500ms */
        if (rc522_read_card(uid, &uid_len) == 0) {
            struct rfid_msg msg;
            rt_memset(&msg, 0, sizeof(msg));
            rt_memcpy(msg.uid, uid, uid_len);
            msg.len = uid_len;

            rt_mq_send(mq_rfid, &msg, sizeof(msg));

            /* 更新全局缓存，供云端线程上传 */
            char *p = (char *)g_rfid_uid;
            for (int i = 0; i < uid_len; i++) {
                p += rt_snprintf(p, sizeof(g_rfid_uid) - (p - (char *)g_rfid_uid),
                                "%02X%s", uid[i], (i + 1 < uid_len) ? ":" : "");
            }
            g_rfid_updated = 1;

            /* Wait for card to be removed, or debounce 3s */
            rt_thread_mdelay(3000);
        } else {
            rt_thread_mdelay(500);
        }
    }
}