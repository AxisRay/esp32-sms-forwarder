#pragma once

#include <stdbool.h>
#include <stddef.h>

// 目标硬件：ESP32C3 + ML307A

// 多任务共用模组 UART 时需在“会话”前后加锁（一次 AT 或一次 URC 处理）
void modem_lock_acquire(void);
void modem_lock_release(void);

// 硬件引脚定义
#define MODEM_TXD_PIN  3
#define MODEM_RXD_PIN  4
#define MODEM_EN_PIN   5

#define MODEM_UART_NUM       1
#define MODEM_UART_BAUD      115200
#define MODEM_UART_BUF_SIZE  1024

// 初始化UART和模组
void modem_init(void);

// 模组断电重启（EN引脚控制）
void modem_power_cycle(void);

// 硬重启模组并等待AT握手
void modem_reset(void);

// 发送AT命令并获取响应，结果写入buf，返回实际长度
int modem_send_at(const char *cmd, char *buf, size_t buf_size, unsigned long timeout_ms);

// 发送AT命令并等待OK，返回是否成功
bool modem_send_at_wait_ok(const char *cmd, unsigned long timeout_ms);

// 等待网络注册（LTE/4G CEREG）
bool modem_wait_cereg(void);

// 查询 CGACT 状态：cid 对应 PDP context，返回 true 表示该 context 已激活
bool modem_cgact_is_active(int cid);
// 激活/关闭 PDP context（用于 ping 测试前临时开启、测试后关闭）
bool modem_cgact_activate(int cid);
bool modem_cgact_deactivate(int cid);

// 从UART读取一行（非阻塞），返回是否读到完整行
bool modem_read_line(char *buf, size_t buf_size);

// 等待模组返回 > 提示符（用于 CMGS），不发送数据，仅轮询读取
bool modem_wait_prompt(unsigned long timeout_ms);

// 仅读取直至 OK/ERROR 或超时，不发送数据（用于 CMGS 发送 PDU 后等结果）
int modem_read_until_ok_or_error(char *buf, size_t buf_size, unsigned long timeout_ms);

// 清空UART接收缓冲区
void modem_flush_rx(void);

// 发送原始数据到UART
void modem_write(const char *data, size_t len);

// 发送一行（带\r\n）
void modem_writeln(const char *data);

// 4G 模组 Ping（固定模组，如 Quectel AT+CGPING），成功返回 true，message 写入 msg 缓冲区
bool modem_ping(const char *host, char *msg, size_t msg_size);
