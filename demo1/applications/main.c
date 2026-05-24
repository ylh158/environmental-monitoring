#include <rtthread.h>
#include <stdio.h>
#include <string.h>

#define DBG_TAG "main"
#define DBG_LVL DBG_LOG
#include <rtdbg.h>

// 消息队列
rt_mq_t my_mq;
//信号量
rt_sem_t uart_sem;

// 结构体
typedef struct {
    int temp;
    int humidity;
    int longitude;
    int latitude;
    int count;
} sensor_data_t;

// 安全打印
void safe_printf(const char *fmt,...)
{
    char buffer[256];
    va_list args;
    va_start(args, fmt);
    rt_vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    rt_sem_take(uart_sem, RT_WAITING_FOREVER);
    rt_kprintf("%s", buffer);
    rt_sem_release(uart_sem);
}

// 采集线程
void sensor_entry(void *parameter)
{
    int count = 0;
    while (1)
    {
        sensor_data_t data;
        data.count = count++;
        data.temp = 25 + (count % 5);    // int 类型
        data.humidity = 45 + (count % 10);
        data.longitude = 116.397f;
        data.latitude = 39.908f;

        rt_mq_send(my_mq, &data, sizeof(sensor_data_t));
        safe_printf("Sample OK: No.%d\n", data.count);
        rt_thread_mdelay(2000);
    }
}

// 接收线程
void recv_thread(void *param)
{
    sensor_data_t recv_data;
    while (1)
    {
        rt_mq_recv(my_mq, &recv_data, sizeof(sensor_data_t), RT_WAITING_FOREVER);

        safe_printf("-------------------------\n");
        safe_printf("Recv Data: %d\n", recv_data.count);
        safe_printf("Temp:      %d C\n", recv_data.temp);       // %d
        safe_printf("Humidity:  %d %% \n", recv_data.humidity);  // %d
        safe_printf("GPS:       %d, %d\n", recv_data.longitude, recv_data.latitude);
        safe_printf("-------------------------\n\n");
    }
}

//定时器回调函数
void timer_callback(void *param)
{

}
// main
int main(void)
{
    uart_sem = rt_sem_create("uart_sem",1, RT_IPC_FLAG_FIFO);

    my_mq = rt_mq_create
            ("my_queue",
            sizeof(sensor_data_t),
            5,
            RT_IPC_FLAG_FIFO);

    rt_thread_t sensor_tid = rt_thread_create(
            "sensor",
            sensor_entry,
            RT_NULL,
            1024,
            8,
            10);
    rt_thread_startup(sensor_tid);

    rt_thread_t recv_tid = rt_thread_create(
            "recv",
            recv_thread,
            RT_NULL,
            1024,
            11,
            10);

    rt_thread_startup(recv_tid);

    return RT_EOK;
}
