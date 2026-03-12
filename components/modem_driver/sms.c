/**
 * @file sms.c
 * @brief SIM 短信收发、PDU 编解码、长短信合并与 URC 处理
 *
 * 功能概览：
 * - 长短信：多段 PDU 按 ref 合并后统一上报
 * - 单条短信：直接上报或从 SIM 按索引读取后上报
 * - URC：+CMT（直接 PDU）、+CMTI（存 SIM 后通知）两种模式
 * - 历史：内存环形缓冲最近若干条，供 Web GET /api/sms/history 读取
 */
#include "sms.h"
#include <config.h>
#include "modem.h"
#include "pdu_codec.h"
#include "app_events.h"
#include <utils.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "sms";

/* ========== 长短信合并（多段 PDU 按 concat_ref 缓存并组装） ========== */

typedef struct {
    bool valid;
    char text[PDU_MAX_TEXT_LEN];
} sms_part_t;

typedef struct {
    bool          in_use;
    int           ref_number;
    char          sender[PDU_MAX_SENDER_LEN];
    char          timestamp[PDU_MAX_TS_LEN];
    int           total_parts;
    int           received_parts;
    uint32_t      first_part_tick;
    sms_part_t    parts[MAX_CONCAT_PARTS];
} concat_sms_t;

static concat_sms_t s_concat_buf[MAX_CONCAT_MESSAGES];

/** URC 状态：IDLE=等待 +CMT/+CMTI，WAIT_PDU=已收 +CMT，等待下一行 PDU */
typedef enum { URC_IDLE, URC_WAIT_PDU } urc_state_t;
static urc_state_t s_urc_state = URC_IDLE;

/** +CMTI 待读队列：URC 只入队索引，由独立任务从 SIM 读取，避免在轮询中长时间持 modem 导致 WDT */
#define CMTI_QUEUE_LEN  8
static QueueHandle_t s_cmti_queue = NULL;

/* 前向声明：从 SIM 按索引读一条短信，实现在本文件后部 */
static void sms_read_from_sim_index(int index);
/** 已解码 PDU 统一入口：长短信合并后推送，单条直接推送 */
static void urc_handle_decoded_pdu(pdu_decode_result_t *decoded);

static void sms_cmti_reader_task(void *arg)
{
    int index;
    for (;;) {
        if (xQueueReceive(s_cmti_queue, &index, portMAX_DELAY) != pdTRUE) continue;
        if (index <= 0) continue;
        ESP_LOGI(TAG, "CMTI 队列取出 index=%d，准备从 SIM 读取短信", index);
        sms_read_from_sim_index(index);
    }
}

void sms_init(void)
{
    memset(s_concat_buf, 0, sizeof(s_concat_buf));
    s_urc_state = URC_IDLE;
    if (s_cmti_queue == NULL) {
        s_cmti_queue = xQueueCreate(CMTI_QUEUE_LEN, sizeof(int));
        if (s_cmti_queue != NULL) {
            xTaskCreate(sms_cmti_reader_task, "sms_cmti", 6144, NULL, 4, NULL);
            ESP_LOGI(TAG, "sms_init: CMTI 队列与读取任务已创建");
        } else {
            ESP_LOGE(TAG, "sms_init: 创建 CMTI 队列失败");
        }
    } else {
        ESP_LOGI(TAG, "sms_init: 复用已有 CMTI 队列");
    }
}

/* 行解析辅助（用于 AT 响应 \r\n / \n），实现在本文件后部 */
static const char *find_line_end(const char *p);
static int line_end_step(const char *line_end);
/* 短信历史环形缓冲写入，实现在本文件后部 */
static void sms_history_append(const char *sender, const char *text, const char *timestamp);

/** 按 ref+sender 查找已有槽位，否则分配空闲或覆盖最老槽位，返回槽位索引 */
static int find_or_create_concat_slot(int ref, const char *sender, int total)
{
    for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
        if (s_concat_buf[i].in_use &&
            s_concat_buf[i].ref_number == ref &&
            strcmp(s_concat_buf[i].sender, sender) == 0) {
            return i;
        }
    }
    for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
        if (!s_concat_buf[i].in_use) {
            s_concat_buf[i].in_use = true;
            s_concat_buf[i].ref_number = ref;
            safe_strcpy(s_concat_buf[i].sender, sender, sizeof(s_concat_buf[i].sender));
            s_concat_buf[i].total_parts = total;
            s_concat_buf[i].received_parts = 0;
            s_concat_buf[i].first_part_tick = xTaskGetTickCount();
            memset(s_concat_buf[i].parts, 0, sizeof(s_concat_buf[i].parts));
            return i;
        }
    }
    /* 无空闲：覆盖 first_part_tick 最老的槽位 */
    int oldest = 0;
    uint32_t oldest_tick = s_concat_buf[0].first_part_tick;
    for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
        if (s_concat_buf[i].first_part_tick < oldest_tick) {
            oldest_tick = s_concat_buf[i].first_part_tick;
            oldest = i;
        }
    }
    ESP_LOGW(TAG, "长短信缓存已满，覆盖最老的槽位");
    memset(&s_concat_buf[oldest], 0, sizeof(concat_sms_t));
    s_concat_buf[oldest].in_use = true;
    s_concat_buf[oldest].ref_number = ref;
    safe_strcpy(s_concat_buf[oldest].sender, sender, sizeof(s_concat_buf[oldest].sender));
    s_concat_buf[oldest].total_parts = total;
    s_concat_buf[oldest].first_part_tick = xTaskGetTickCount();
    return oldest;
}

/** 将指定槽位的分段按序拼接到 out，缺失分段用 [缺失分段N] 占位；避免循环内 strlen 保持 O(n) */
static void assemble_concat_sms(int slot, char *out, size_t out_size)
{
    size_t len = 0;
    out[0] = '\0';
    if (out_size == 0) return;
    for (int i = 0; i < s_concat_buf[slot].total_parts && len < out_size - 64; i++) {
        const char *src;
        size_t src_len;
        if (s_concat_buf[slot].parts[i].valid) {
            src = s_concat_buf[slot].parts[i].text;
            src_len = strlen(src);
        } else {
            char missing[32];
            int n = snprintf(missing, sizeof(missing), "[缺失分段%d]", i + 1);
            src = missing;
            src_len = (size_t)(n > 0 ? n : 0);
        }
        if (len + src_len >= out_size) src_len = out_size - len - 1;
        memcpy(out + len, src, src_len + 1);
        len += src_len;
    }
}

/** 清空指定长短信槽位，释放后可被 find_or_create_concat_slot 复用 */
static void clear_concat_slot(int slot)
{
    memset(&s_concat_buf[slot], 0, sizeof(concat_sms_t));
}

/**
 * 从 SIM 按索引读一条短信（AT+CMGR），响应格式：+CMGR: <stat>,... 换行 <PDU>。
 * 解析到 PDU 后解码并走 sms_process_content 统一处理。
 */
static void sms_read_from_sim_index(int index)
{
    if (index <= 0) return;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGR=%d", index);
    char *buf = malloc(4096);
    if (!buf) {
        ESP_LOGE(TAG, "内存不足，无法读取 SIM 短信 index=%d", index);
        return;
    }
    int n = modem_send_at(cmd, buf, 4095, 15000);
    if (n <= 0) {
        ESP_LOGE(TAG, "AT+CMGR 读取失败 index=%d, n=%d", index, n);
        free(buf);
        return;
    }
    buf[n] = '\0';
    ESP_LOGI(TAG, "AT+CMGR 响应 index=%d: n=%d, 含+CMGR=%s, 前96字节: %.96s",
             index, n, strstr(buf, "+CMGR") ? "是" : "否", buf);

    bool processed = false;
    const char *p = buf;
    while (*p) {
        const char *line_end = find_line_end(p);
        size_t line_len = (size_t)(line_end - p);
        int step = line_end_step(line_end);
        if (line_len == 0 || line_len >= 256) {
            p = line_end + step;
            continue;
        }
        if (strncmp(p, "+CMGR:", 6) != 0) {
            p = line_end + step;
            continue;
        }
        /* 当前行是 +CMGR:，下一行应为 PDU */
        p = line_end + step;
        const char *pdu_end = find_line_end(p);
        size_t pdu_len = (size_t)(pdu_end - p);
        ESP_LOGI(TAG, "[CMGR 调试] index=%d 本行+CMGR, 下一行 pdu_len=%u, PDU_MAX_HEX_LEN=%d",
                 index, (unsigned)pdu_len, PDU_MAX_HEX_LEN);
        if (pdu_len == 0 || pdu_len >= PDU_MAX_HEX_LEN) {
            ESP_LOGW(TAG, "[CMGR 调试] 跳过: pdu_len 无效或超长");
            break;
        }
        /* 先拷贝并补 \0，再判 hex：p 指向的 PDU 行在 buf 中无 \0，pdu_is_hex_string 会读到行尾 \r\n 导致失败 */
        char hex_buf[PDU_MAX_HEX_LEN];
        memcpy(hex_buf, p, pdu_len);
        hex_buf[pdu_len] = '\0';
        if (!pdu_is_hex_string(hex_buf)) {
            ESP_LOGW(TAG, "[CMGR 调试] 非纯十六进制, 前32字符: %.32s", hex_buf);
            break;
        }
        pdu_decode_result_t dec;
        if (!pdu_decode(hex_buf, &dec)) {
            ESP_LOGE(TAG, "AT+CMGR PDU 解析失败 index=%d (hex_len=%u)", index, (unsigned)pdu_len);
            break;
        }
        /* 走统一入口：长短信入 concat 槽位，收齐后合并再推送；单条直接推送 */
        ESP_LOGI(TAG, "从SIM读取 index=%d sender=%s 交长短信合并逻辑", index, dec.sender);
        urc_handle_decoded_pdu(&dec);
        processed = true;
        break;
    }
    if (processed) {
        // 读取并处理成功后，清空 SIM 中所有短信，避免存储空间逐条累积导致再次占满
        snprintf(cmd, sizeof(cmd), "AT+CMGD=1,4");
        char del_resp[64];
        int del_len = modem_send_at(cmd, del_resp, sizeof(del_resp) - 1, 5000);
        if (del_len > 0) {
            ESP_LOGI(TAG, "已清空 SIM 短信存储");
        } else {
            ESP_LOGW(TAG, "清空 SIM 短信存储失败, del_len=%d", del_len);
        }
    } else if (strstr(buf, "+CMGR")) {
        ESP_LOGW(TAG, "AT+CMGR 响应含+CMGR但解析/处理未完成 index=%d", index);
    }
    free(buf);
}

/* ========== 对外接口：事件上报、发短信 ========== */

/** 将单条短信内容通过 APP_EVENT_SMS_RECEIVED 上报，并打日志 */
void sms_process_content(const char *sender, const char *text, const char *timestamp)
{
    ESP_LOGI(TAG, "=== 处理短信 === 发送者: %s, 时间: %s, 内容: %s",
             sender, timestamp, text);
    ESP_LOGI(TAG, "post APP_EVENT_SMS_RECEIVED（由 main 入队后 sms_notify_task 推送）");

    // 堆分配事件数据（2KB+），避免大结构体压栈导致主任务栈溢出
    app_event_sms_received_data_t *event_data = malloc(sizeof(app_event_sms_received_data_t));
    if (!event_data) {
        ESP_LOGE(TAG, "内存不足，无法分配事件数据");
        return;
    }
    safe_strcpy(event_data->sender, sender, sizeof(event_data->sender));
    safe_strcpy(event_data->text, text, sizeof(event_data->text));
    safe_strcpy(event_data->timestamp, timestamp, sizeof(event_data->timestamp));

    esp_event_post(APP_EVENT_BASE, APP_EVENT_SMS_RECEIVED, event_data, sizeof(*event_data), portMAX_DELAY);
    free(event_data);

    // 写入内存环形缓冲，供 GET /api/sms/history 读取（长短信合并后此处只调用一次，视为一条）
    sms_history_append(sender, text, timestamp);
}

/* ========== 短信历史环形缓冲区（内存，最多 5 条，不读 SIM） ========== */
/* 每条含 content[2048]，保留 5 条需约 10.5KB */
#define SMS_HISTORY_RING_BUF_BYTES  (12 * 1024)
#define SMS_HISTORY_RING_SLOTS      ((SMS_HISTORY_RING_BUF_BYTES / (int)sizeof(sms_history_entry_t)) > 5 ? 5 : (SMS_HISTORY_RING_BUF_BYTES / (int)sizeof(sms_history_entry_t)))

static sms_history_entry_t s_history_ring[SMS_HISTORY_RING_SLOTS];
static int s_history_head;
static int s_history_count;
static uint32_t s_history_next_id;

/** 从字符串 p 读取两位数字，返回 0–99，失败返回 -1；*out 指向下一个字符 */
static int parse_two_digits(const char *p, const char **out)
{
    if (!p || p[0] < '0' || p[0] > '9' || p[1] < '0' || p[1] > '9') return -1;
    if (out) *out = p + 2;
    return (p[0] - '0') * 10 + (p[1] - '0');
}

/** 将 PDU 时间戳 "YY/MM/DD HH:MM:SS" 转为 Unix 毫秒；无效返回 0。避免 sscanf 占用大栈。 */
static int64_t parse_pdu_ts_to_ms(const char *ts_str)
{
    if (!ts_str || strlen(ts_str) < 14) return 0;
    const char *p = ts_str;
    int yy = parse_two_digits(p, &p);   if (yy < 0 || *p != '/') return 0; p++;
    int mm = parse_two_digits(p, &p);   if (mm < 0 || *p != '/') return 0; p++;
    int dd = parse_two_digits(p, &p);   if (dd < 0 || *p != ' ') return 0; p++;
    int hh = parse_two_digits(p, &p);   if (hh < 0 || *p != ':') return 0; p++;
    int mi = parse_two_digits(p, &p);   if (mi < 0 || *p != ':') return 0; p++;
    int ss = parse_two_digits(p, &p);   if (ss < 0) return 0;
    struct tm t = {0};
    t.tm_year = (yy >= 0 && yy <= 99) ? (yy + 100) : yy; /* 00->2000, 99->1999 */
    t.tm_mon  = (mm >= 1 && mm <= 12) ? (mm - 1) : 0;
    t.tm_mday = (dd >= 1 && dd <= 31) ? dd : 1;
    t.tm_hour = (hh >= 0 && hh <= 23) ? hh : 0;
    t.tm_min  = (mi >= 0 && mi <= 59) ? mi : 0;
    t.tm_sec  = (ss >= 0 && ss <= 59) ? ss : 0;
    time_t sec = mktime(&t);
    return (sec != (time_t)-1) ? (int64_t)sec * 1000 : 0;
}

static void sms_history_append(const char *sender, const char *text, const char *timestamp)
{
    sms_history_entry_t e;
    e.id = (int)(s_history_next_id++);
    e.ts = parse_pdu_ts_to_ms(timestamp);
    safe_strcpy(e.sender, sender, SMS_SENDER_LEN);
    safe_strcpy(e.content, text, SMS_CONTENT_LEN);

    int write_idx = (s_history_head + s_history_count) % SMS_HISTORY_RING_SLOTS;
    if (s_history_count >= SMS_HISTORY_RING_SLOTS)
        s_history_head = (s_history_head + 1) % SMS_HISTORY_RING_SLOTS;
    else
        s_history_count++;
    s_history_ring[write_idx] = e;
}

int sms_history_get(sms_history_entry_t *out, int max_count)
{
    if (!out || max_count <= 0) return 0;
    int n = s_history_count < max_count ? s_history_count : max_count;
    for (int i = 0; i < n; i++)
        out[i] = s_history_ring[(s_history_head + s_history_count - 1 - i + SMS_HISTORY_RING_SLOTS) % SMS_HISTORY_RING_SLOTS];
    return n;
}

/** PDU 编码后发 AT+CMGS=<len>，等 ">" 再发 PDU 十六进制 + Ctrl+Z，根据响应判断成功与否 */
bool sms_send(const char *phone, const char *message)
{
    ESP_LOGI(TAG, "发送短信到 %s: %s", phone, message);

    pdu_encode_result_t pdu;
    if (!pdu_encode(phone, message, &pdu)) {
        ESP_LOGE(TAG, "PDU编码失败: 错误码 %d", pdu_encode_last_error());
        return false;
    }

    ESP_LOGI(TAG, "PDU数据: %s (tpdu_len=%d)", pdu.hex, pdu.tpdu_len);

    // 发送 AT+CMGS=<tpdu_len>，等待 > 后发 PDU + Ctrl+Z（参考 PDUlib / Arduino 流程）
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CMGS=%d", pdu.tpdu_len);

    ESP_LOGI(TAG, "准备发送 AT 命令: %s", cmd);
    modem_flush_rx();
    modem_writeln(cmd);

    if (!modem_wait_prompt(5000)) {
        ESP_LOGE(TAG, "未收到 > 提示符");
        return false;
    }
    ESP_LOGI(TAG, "已收到 > 提示符，开始发送 PDU 数据和 Ctrl+Z 结束符");

    // 发送 PDU 十六进制串 + Ctrl+Z 结束
    modem_write(pdu.hex, strlen(pdu.hex));
    uint8_t ctrlz = 0x1A;
    modem_write((const char *)&ctrlz, 1);

    // 仅读取响应，不再发送任何 AT，避免干扰模组
    char resp[256];
    modem_read_until_ok_or_error(resp, sizeof(resp), 30000);

    if (strstr(resp, "OK")) {
        ESP_LOGI(TAG, "短信发送成功");
        return true;
    }
    ESP_LOGE(TAG, "短信发送失败: %s", resp);
    return false;
}

/* ========== URC 处理与长短信合并 ========== */

/** 解析 +CMTI: "SM",<index> 并将索引入队。轻量解析避免 sscanf 大栈占用。 */
static void urc_handle_cmti(const char *line)
{
    const char *p = line;
    if (strncmp(p, "+CMTI:", 6) != 0) return;
    p += 6;
    while (*p == ' ') p++;
    if (*p != '"') return;
    p++;
    const char *mem_start = p;
    while (*p && *p != '"') p++;
    if (*p != '"') return;
    size_t mem_len = (size_t)(p - mem_start); /* 在 p++ 前取长度 */
    p++;
    if (*p != ',') return;
    p++;
    int index = atoi(p);
    if (index <= 0) {
        ESP_LOGW(TAG, "CMTI 格式无法解析: %s", line);
        return;
    }
    char mem[8] = {0};
    if (mem_len > sizeof(mem) - 1) mem_len = sizeof(mem) - 1;
    if (mem_len > 0u) {
        memcpy(mem, mem_start, mem_len);
        mem[mem_len] = '\0';
    }
    ESP_LOGI(TAG, "新短信存储位置: %s,%d，已入队待读", mem, index);
    if (s_cmti_queue != NULL) {
        if (xQueueSend(s_cmti_queue, &index, pdMS_TO_TICKS(500)) != pdTRUE) {
            ESP_LOGW(TAG, "CMTI 队列已满，丢弃 index=%d", index);
        }
    } else {
        sms_read_from_sim_index(index);
    }
}

/** 处理一条已解码的 PDU：长短信写入槽位并视情况合并上报，普通短信直接上报。调用方负责 free(decoded)。 */
static void urc_handle_decoded_pdu(pdu_decode_result_t *decoded)
{
    if (decoded->concat_total > 1 && decoded->concat_part > 0) {
        int slot = find_or_create_concat_slot(
            decoded->concat_ref, decoded->sender, decoded->concat_total);
        int idx = decoded->concat_part - 1;
        ESP_LOGI(TAG, "收到长短信分段 %d/%d", decoded->concat_part, decoded->concat_total);

        if (idx >= 0 && idx < MAX_CONCAT_PARTS && !s_concat_buf[slot].parts[idx].valid) {
            s_concat_buf[slot].parts[idx].valid = true;
            safe_strcpy(s_concat_buf[slot].parts[idx].text, decoded->text,
                    sizeof(s_concat_buf[slot].parts[idx].text));
            s_concat_buf[slot].received_parts++;
            if (s_concat_buf[slot].received_parts == 1) {
                safe_strcpy(s_concat_buf[slot].timestamp, decoded->timestamp,
                        sizeof(s_concat_buf[slot].timestamp));
            }
            ESP_LOGI(TAG, "已缓存分段%d，进度%d/%d",
                     decoded->concat_part, s_concat_buf[slot].received_parts, decoded->concat_total);
        }

        if (s_concat_buf[slot].received_parts >= s_concat_buf[slot].total_parts) {
            ESP_LOGI(TAG, "长短信已收齐，开始合并转发");
            char *full_text = malloc(2048);
            if (full_text) {
                full_text[0] = '\0';
                assemble_concat_sms(slot, full_text, 2048);
                sms_process_content(s_concat_buf[slot].sender, full_text, s_concat_buf[slot].timestamp);
                free(full_text);
            } else {
                ESP_LOGE(TAG, "内存不足，无法合并长短信");
            }
            clear_concat_slot(slot);
        }
    } else {
        sms_process_content(decoded->sender, decoded->text, decoded->timestamp);
    }
}

/** 轮询入口：读一行 URC，按状态机处理 +CMT / +CMTI / 下一行 PDU */
void sms_check_urc(void)
{
    char line[512];
    if (!modem_read_line(line, sizeof(line))) return;
    ESP_LOGD(TAG, "Debug> %s", line);

    if (s_urc_state == URC_IDLE) {
        if (strncmp(line, "+CMT:", 5) == 0) {
            ESP_LOGI(TAG, "检测到+CMT，等待PDU数据...");
            s_urc_state = URC_WAIT_PDU;
            return;
        }
        if (strncmp(line, "+CMTI:", 6) == 0) {
            ESP_LOGI(TAG, "检测到+CMTI，来自SIM的新短信: %s", line);
            urc_handle_cmti(line);
        }
        return;
    }

    if (s_urc_state != URC_WAIT_PDU) return;
    if (strlen(line) == 0) return;

    if (!pdu_is_hex_string(line)) {
        ESP_LOGD(TAG, "收到非PDU数据，返回IDLE");
        s_urc_state = URC_IDLE;
        return;
    }

    ESP_LOGI(TAG, "收到PDU数据(%d字符)", (int)strlen(line));
    pdu_decode_result_t *decoded = malloc(sizeof(pdu_decode_result_t));
    if (!decoded) {
        ESP_LOGE(TAG, "内存不足，无法分配解码缓冲");
        s_urc_state = URC_IDLE;
        return;
    }
    if (!pdu_decode(line, decoded)) {
        ESP_LOGE(TAG, "PDU解析失败");
        free(decoded);
        s_urc_state = URC_IDLE;
        return;
    }
    ESP_LOGI(TAG, "PDU解析成功: sender=%s, concat=%d/%d, content: %s",
             decoded->sender, decoded->concat_part, decoded->concat_total, decoded->text);
    urc_handle_decoded_pdu(decoded);
    free(decoded);
    s_urc_state = URC_IDLE;
}

/* 行解析辅助（sms_read_from_sim_index 等用） */
/** 查找行尾：优先 \r\n，否则 \n，否则视为整行到串尾（兼容不同模组） */
static const char *find_line_end(const char *p)
{
    const char *r = strstr(p, "\r\n");
    const char *n = strchr(p, '\n');
    if (r && (!n || r <= n)) return r;
    if (n) return n;
    return p + strlen(p);
}

/** 行尾占用的字节数：\r\n=2，\n=1 */
static int line_end_step(const char *line_end)
{
    if (line_end[0] == '\r' && line_end[1] == '\n') return 2;
    if (line_end[0] == '\n') return 1;
    return 1;
}

/** 超时未收齐的长短信：强制按已收分段合并上报并清空槽位 */
void sms_check_concat_timeout(void)
{
    uint32_t now = xTaskGetTickCount();
    for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
        if (!s_concat_buf[i].in_use) continue;
        uint32_t elapsed = (now - s_concat_buf[i].first_part_tick) * portTICK_PERIOD_MS;
        if (elapsed < CONCAT_TIMEOUT_MS) continue;

        ESP_LOGW(TAG, "长短信超时，强制转发不完整消息 (ref=%d, %d/%d)",
                 s_concat_buf[i].ref_number,
                 s_concat_buf[i].received_parts,
                 s_concat_buf[i].total_parts);
        char *full_text = malloc(2048);
        if (!full_text) {
            ESP_LOGE(TAG, "内存不足，无法合并长短信");
            clear_concat_slot(i);
            continue;
        }
        full_text[0] = '\0';
        assemble_concat_sms(i, full_text, 2048);
        sms_process_content(s_concat_buf[i].sender, full_text, s_concat_buf[i].timestamp);
        free(full_text);
        clear_concat_slot(i);
    }
}