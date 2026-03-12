// 目标硬件：ESP32C3 + ML307A
#include "modem.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "modem";

// UART 互斥：多任务（sms_poll、sms_notify、httpd、sms_send_task、scheduler）共用模组时串行化
static SemaphoreHandle_t s_modem_mutex = NULL;

static void modem_lock(void)
{
    if (s_modem_mutex) xSemaphoreTakeRecursive(s_modem_mutex, portMAX_DELAY);
}

static void modem_unlock(void)
{
    if (s_modem_mutex) xSemaphoreGiveRecursive(s_modem_mutex);
}

void modem_lock_acquire(void) { modem_lock(); }
void modem_lock_release(void) { modem_unlock(); }

// 行缓冲区（用于非阻塞逐行读取）
static char s_line_buf[512];
static int  s_line_pos = 0;

void modem_init(void)
{
    if (s_modem_mutex == NULL) {
        s_modem_mutex = xSemaphoreCreateRecursiveMutex();
    }
    uart_config_t uart_cfg = {
        .baud_rate  = MODEM_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    uart_driver_install(MODEM_UART_NUM, MODEM_UART_BUF_SIZE * 2, 0, 0, NULL, 0);
    uart_param_config(MODEM_UART_NUM, &uart_cfg);
    uart_set_pin(MODEM_UART_NUM, MODEM_TXD_PIN, MODEM_RXD_PIN,
                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    // EN引脚配置
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << MODEM_EN_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(MODEM_EN_PIN, 1);

    ESP_LOGI(TAG, "UART%d 初始化完成 (TX=%d, RX=%d)", MODEM_UART_NUM, MODEM_TXD_PIN, MODEM_RXD_PIN);
}

void modem_power_cycle(void)
{
    ESP_LOGI(TAG, "EN拉低：关闭模组");
    gpio_set_level(MODEM_EN_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1200));

    ESP_LOGI(TAG, "EN拉高：开启模组");
    gpio_set_level(MODEM_EN_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(6000));
}

void modem_reset(void)
{
    ESP_LOGI(TAG, "硬重启模组（EN断电重启）...");
    modem_power_cycle();
    modem_flush_rx();

    bool ok = false;
    for (int i = 0; i < 10; i++) {
        if (modem_send_at_wait_ok("AT", 1000)) {
            ok = true;
            break;
        }
        ESP_LOGW(TAG, "AT未响应，继续等模组启动...");
    }
    if (ok) ESP_LOGI(TAG, "模组AT恢复正常");
    else    ESP_LOGE(TAG, "模组AT仍未响应（检查EN接线/供电/波特率）");
}

void modem_flush_rx(void)
{
    modem_lock();
    uart_flush_input(MODEM_UART_NUM);
    s_line_pos = 0;
    modem_unlock();
}

void modem_write(const char *data, size_t len)
{
    modem_lock();
    ESP_LOGD(TAG, "UART TX %d 字节", (int)len);
    uart_write_bytes(MODEM_UART_NUM, data, len);
    modem_unlock();
}

void modem_writeln(const char *data)
{
    modem_lock();
    ESP_LOGD(TAG, "AT> %s", data);
    uart_write_bytes(MODEM_UART_NUM, data, strlen(data));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);
    modem_unlock();
}

int modem_send_at(const char *cmd, char *buf, size_t buf_size, unsigned long timeout_ms)
{
    modem_lock();
    ESP_LOGI(TAG, "AT 发送: %s", cmd);
    uart_flush_input(MODEM_UART_NUM);
    s_line_pos = 0;
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    int pos = 0;
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    int yield_count = 0;

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        /* 长时间等待时定期让出 CPU，避免 IDLE 无法运行触发看门狗 */
        if (++yield_count >= 50) {
            yield_count = 0;
            vTaskDelay(pdMS_TO_TICKS(0));
        }
        uint8_t byte;
        int len = uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(10));
        if (len > 0 && pos < (int)(buf_size - 1)) {
            buf[pos++] = (char)byte;
            buf[pos] = '\0';
            if (strstr(buf, "OK") || strstr(buf, "ERROR")) {
                vTaskDelay(pdMS_TO_TICKS(50));
                // 读完剩余数据
                while (pos < (int)(buf_size - 1)) {
                    len = uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
                    if (len <= 0) break;
                    buf[pos++] = (char)byte;
                }
                buf[pos] = '\0';
                // 简要打印 AT 响应首行，去掉前导换行，避免刷屏
                const char *line = buf;
                while (*line == '\r' || *line == '\n') line++;
                const char *nl = strpbrk(line, "\r\n");
                char first_line[80];
                size_t copy_len = nl ? (size_t)(nl - line) : strlen(line);
                if (copy_len >= sizeof(first_line)) copy_len = sizeof(first_line) - 1;
                memcpy(first_line, line, copy_len);
                first_line[copy_len] = '\0';
                ESP_LOGI(TAG, "AT< %s", first_line);
                modem_unlock();
                return pos;
            }
        }
    }
    buf[pos] = '\0';
    ESP_LOGD(TAG, "AT< (超时，收到 %d 字节): %s", pos, buf);
    modem_unlock();
    return pos;
}

bool modem_send_at_wait_ok(const char *cmd, unsigned long timeout_ms)
{
    char resp[256];
    modem_send_at(cmd, resp, sizeof(resp), timeout_ms);
    return strstr(resp, "OK") != NULL;
}

bool modem_wait_cereg(void)
{
    char resp[128];
    modem_send_at("AT+CEREG?", resp, sizeof(resp), 2000);
    // +CEREG: <n>,<stat> 其中stat=1或5表示已注册
    char *p = strstr(resp, "+CEREG:");
    if (!p) return false;
    if (strstr(p, ",1") || strstr(p, ",5")) return true;
    return false;
}

// 查询 PDP context 激活状态。cid 通常为 1。返回 true 表示已激活，false 表示未激活或查询失败
bool modem_cgact_is_active(int cid)
{
    char resp[256];
    int n = modem_send_at("AT+CGACT?", resp, sizeof(resp), 3000);
    if (n <= 0 || strstr(resp, "ERROR") != NULL) return false;
    // 响应格式: +CGACT: <cid>,<state> 或多行。state 0=未激活 1=已激活
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "+CGACT: %d,1", cid);
    return strstr(resp, pattern) != NULL;
}

// 激活 PDP context（用于 ping 等数据业务前），cid 通常为 1
bool modem_cgact_activate(int cid)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CGACT=1,%d", cid);
    return modem_send_at_wait_ok(cmd, 15000);
}

// 关闭 PDP context（测试完成后关闭数据连接省流量），cid 通常为 1
bool modem_cgact_deactivate(int cid)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CGACT=0,%d", cid);
    return modem_send_at_wait_ok(cmd, 5000);
}

bool modem_read_line(char *buf, size_t buf_size)
{
    modem_lock();
    uint8_t byte;
    while (uart_read_bytes(MODEM_UART_NUM, &byte, 1, 0) > 0) {
        // 如需底层抓包，可临时改为 ESP_LOGD 打印原始字节
        if (byte == '\n') {
            s_line_buf[s_line_pos] = '\0';
            if (s_line_pos > 0 && s_line_buf[s_line_pos - 1] == '\r') {
                s_line_buf[s_line_pos - 1] = '\0';
            }
            strncpy(buf, s_line_buf, buf_size - 1);
            buf[buf_size - 1] = '\0';
            int len = s_line_pos;
            s_line_pos = 0;
            if (len > 0 || strlen(buf) > 0) {
                ESP_LOGD(TAG, "URC< %s", buf);
            }
            modem_unlock();
            return len > 0 || strlen(buf) > 0;
        } else {
            if (s_line_pos < (int)sizeof(s_line_buf) - 1) {
                s_line_buf[s_line_pos++] = (char)byte;
            } else {
                s_line_pos = 0; // 超长保护
            }
        }
    }
    modem_unlock();
    return false;
}

// 仅轮询读取直到出现 '>'，不写入 s_line_buf，避免与 modem_read_line 争用
bool modem_wait_prompt(unsigned long timeout_ms)
{
    modem_lock();
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    uint8_t byte;
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(50)) > 0 && byte == '>') {
            ESP_LOGD(TAG, "AT< 收到 '>' 提示符");
            modem_unlock();
            return true;
        }
    }
    ESP_LOGW(TAG, "等待 '>' 提示符超时 (%lu ms)", timeout_ms);
    modem_unlock();
    return false;
}

// 仅读取 UART 直至 OK/ERROR 或超时，不发送、不 flush
int modem_read_until_ok_or_error(char *buf, size_t buf_size, unsigned long timeout_ms)
{
    modem_lock();
    int pos = 0;
    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    while ((xTaskGetTickCount() - start) < timeout_ticks && pos < (int)(buf_size - 1)) {
        uint8_t byte;
        int n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
        if (n > 0) {
            buf[pos++] = (char)byte;
            buf[pos] = '\0';
            if (strstr(buf, "OK") || strstr(buf, "ERROR")) {
                vTaskDelay(pdMS_TO_TICKS(50));
                while (pos < (int)(buf_size - 1)) {
                    n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(20));
                    if (n <= 0) break;
                    buf[pos++] = (char)byte;
                }
                buf[pos] = '\0';
                modem_unlock();
                return pos;
            }
        }
    }
    buf[pos] = '\0';
    ESP_LOGD(TAG, "AT< (read_until_ok_or_error 超时，收到 %d 字节): %s", pos, buf);
    modem_unlock();
    return pos;
}

// 4G Ping：ML307A AT+MPING 测试连通性
bool modem_ping(const char *host, char *msg, size_t msg_size)
{
    if (!msg || msg_size == 0) return false;
    msg[0] = '\0';

    modem_lock();

    const char *target = host ? host : "8.8.8.8";
    ESP_LOGI(TAG, "开始 4G Ping 测试，目标=%s", target);

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "AT+MPING=\"%s\",30,1", target);
    ESP_LOGD(TAG, "发送 MPING 命令: %s", cmd);

    uart_flush_input(MODEM_UART_NUM);
    s_line_pos = 0;
    uart_write_bytes(MODEM_UART_NUM, cmd, strlen(cmd));
    uart_write_bytes(MODEM_UART_NUM, "\r\n", 2);

    // 参考 Arduino 逻辑：等待最多 35 秒（30 秒超时 + 5 秒余量），期间不断累积串口数据并查找 +MPING 结果
    char buf[512];
    int pos = 0;
    buf[0] = '\0';

    uint32_t start = xTaskGetTickCount();
    uint32_t timeout_ticks = pdMS_TO_TICKS(35000);

    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        uint8_t byte;
        int n = uart_read_bytes(MODEM_UART_NUM, &byte, 1, pdMS_TO_TICKS(100));
        if (n > 0) {
            if (pos < (int)sizeof(buf) - 1) {
                buf[pos++] = (char)byte;
                buf[pos] = '\0';
            }

            // 检查是否收到 ERROR
            if (strstr(buf, "+CME ERROR") || strstr(buf, "ERROR")) {
                snprintf(msg, msg_size, "模组返回错误");
                ESP_LOGW(TAG, "MPING 期间收到 ERROR: %s", buf);
                modem_unlock();
                return false;
            }

            // 查找 +MPING 结果行（参考 code.ino：必须等整行收齐再解析，否则拿不到 延迟/TTL）
            char *mping = strstr(buf, "+MPING:");
            if (mping) {
                char *line_end = strchr(mping, '\n');
                if (!line_end) line_end = strchr(mping, '\r');
                if (!line_end) {
                    // 行未收齐，继续读；避免半行解析导致 params 为空
                    continue;
                }
                size_t line_len = (size_t)(line_end - mping);
                if (line_len > 0 && (line_end[-1] == '\r' || line_end[-1] == '\n'))
                    line_len--;  // 不把行尾 \r/\n 拷入 line

                char line[160];
                if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
                memcpy(line, mping, line_len);
                line[line_len] = '\0';
                ESP_LOGD(TAG, "MPING 行内容: %s", line);

                // line 形如：+MPING: <result>[,<ip>,<packet_len>,<time>,<ttl>]
                char *colon = strchr(line, ':');
                if (colon) {
                    char *params = colon + 1;
                    while (*params == ' ') params++;

                    // 解析第一个参数 result
                    char *first_comma = strchr(params, ',');
                    char result_str[8];
                    if (first_comma) {
                        size_t len = (size_t)(first_comma - params);
                        if (len >= sizeof(result_str)) len = sizeof(result_str) - 1;
                        memcpy(result_str, params, len);
                        result_str[len] = '\0';
                    } else {
                        strncpy(result_str, params, sizeof(result_str) - 1);
                        result_str[sizeof(result_str) - 1] = '\0';
                    }
                    int result = atoi(result_str);
                    ESP_LOGI(TAG, "MPING result=%d, params=%s", result, params);

                    bool ping_success = (result == 0 || result == 1) ||
                                        (first_comma != NULL && strlen(first_comma + 1) > 5);

                    if (ping_success) {
                        // 进一步解析 IP、时间、TTL
                        if (first_comma) {
                            char *rest = first_comma + 1;
                            // 处理 IP（可能带引号）
                            char ip[64] = {0};
                            char *p = rest;
                            char *next_comma = NULL;
                            if (*p == '\"') {
                                p++;
                                char *quote_end = strchr(p, '\"');
                                if (quote_end) {
                                    size_t len = (size_t)(quote_end - p);
                                    if (len >= sizeof(ip)) len = sizeof(ip) - 1;
                                    memcpy(ip, p, len);
                                    ip[len] = '\0';
                                    next_comma = strchr(quote_end + 1, ',');
                                }
                            } else {
                                next_comma = strchr(p, ',');
                                if (next_comma) {
                                    size_t len = (size_t)(next_comma - p);
                                    if (len >= sizeof(ip)) len = sizeof(ip) - 1;
                                    memcpy(ip, p, len);
                                    ip[len] = '\0';
                                }
                            }

                            if (!ip[0]) {
                                snprintf(ip, sizeof(ip), "%s", target);
                            }

                            if (next_comma) {
                                rest = next_comma + 1;              // packet_len 开始
                                char *comma_after_len = strchr(rest, ',');
                                if (comma_after_len) {
                                    rest = comma_after_len + 1;     // time 开始
                                    char *comma_after_time = strchr(rest, ',');
                                    char time_str[16] = {0};
                                    char ttl_str[16] = {0};
                                    if (comma_after_time) {
                                        size_t len_time = (size_t)(comma_after_time - rest);
                                        if (len_time >= sizeof(time_str)) len_time = sizeof(time_str) - 1;
                                        memcpy(time_str, rest, len_time);
                                        time_str[len_time] = '\0';

                                        strncpy(ttl_str, comma_after_time + 1, sizeof(ttl_str) - 1);
                                        ttl_str[sizeof(ttl_str) - 1] = '\0';
                                    } else {
                                        strncpy(time_str, rest, sizeof(time_str) - 1);
                                        time_str[sizeof(time_str) - 1] = '\0';
                                        strcpy(ttl_str, "N/A");
                                    }

                                    // 格式化提示信息
                                    snprintf(msg, msg_size, "目标: %s, 延迟: %sms, TTL: %s", ip, time_str, ttl_str);
                                    ESP_LOGI(TAG, "Ping 成功：%s", msg);
                                } else {
                                    snprintf(msg, msg_size, "目标: %s, Ping 成功", ip);
                                    ESP_LOGI(TAG, "Ping 成功：%s", msg);
                                }
                            } else {
                                snprintf(msg, msg_size, "目标: %s, Ping 成功", ip);
                                ESP_LOGI(TAG, "Ping 成功：%s", msg);
                            }
                        } else {
                            snprintf(msg, msg_size, "目标: %s, Ping 成功", target);
                            ESP_LOGI(TAG, "Ping 成功：%s", msg);
                        }
                        modem_unlock();
                        return true;
                    } else {
                        snprintf(msg, msg_size, "Ping 超时或目标不可达 (错误码: %d)", result);
                        ESP_LOGW(TAG, "%s", msg);
                        modem_unlock();
                        return false;
                    }
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    snprintf(msg, msg_size, "Ping 超时或目标无响应");
    ESP_LOGW(TAG, "MPING 在超时时间内未返回结果，原始缓冲区: %s", buf);
    modem_unlock();
    return false;
}
