#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// 推送通道类型
typedef enum {
    PUSH_TYPE_POST_JSON  = 1,  // POST JSON {"sender":"xxx","message":"xxx","timestamp":"xxx"}
    PUSH_TYPE_GET        = 2,  // GET请求，参数放URL中
    PUSH_TYPE_SMTP       = 3,  // 邮件(SMTP)
    PUSH_TYPE_BARK       = 4,  // Bark POST {"title":"xxx","body":"xxx"}
    PUSH_TYPE_DINGTALK   = 5,  // 钉钉机器人
    PUSH_TYPE_PUSHPLUS   = 6,  // PushPlus
    PUSH_TYPE_SERVERCHAN = 7,  // Server酱
    PUSH_TYPE_FEISHU     = 8,  // 飞书机器人
    PUSH_TYPE_GOTIFY     = 9,  // Gotify
    PUSH_TYPE_TELEGRAM   = 10, // Telegram Bot
    PUSH_TYPE_PUSHOVER   = 11  // Pushover
} push_type_t;

#define MAX_PUSH_CHANNELS     5
#define MAX_SCHEDULE_TASKS    3
#define MAX_STR_LEN           256
#define MAX_BODY_LEN          512
/** NVS 中每通道仅存 3 项：名称、类型、推送参数(二进制 blob)。params 为固定结构体，无 JSON 解析 */
#define MAX_LAST_MSG_LEN    64

#define DEFAULT_WEB_USER    "admin"
#define DEFAULT_WEB_PASS    "admin123"

// ========== Tagged Union：各平台独立参数结构体 ==========
#define PUSH_URL_LEN    MAX_STR_LEN
#define PUSH_BODY_LEN   MAX_BODY_LEN
#define PUSH_PORT_LEN   16
#define PUSH_CHANNEL_LEN 64

typedef struct { char url[PUSH_URL_LEN]; char body[PUSH_BODY_LEN]; } push_params_post_json_t;
typedef struct { char url[PUSH_URL_LEN]; } push_params_bark_t;
typedef struct { char url[PUSH_URL_LEN]; } push_params_get_t;
typedef struct { char url[PUSH_URL_LEN]; char secret[PUSH_URL_LEN]; } push_params_dingtalk_t;
typedef struct { char url[PUSH_URL_LEN]; char token[PUSH_URL_LEN]; char channel[PUSH_CHANNEL_LEN]; } push_params_pushplus_t;
typedef struct { char url[PUSH_URL_LEN]; char sendkey[PUSH_URL_LEN]; } push_params_serverchan_t;
typedef struct { char url[PUSH_URL_LEN]; char secret[PUSH_URL_LEN]; } push_params_feishu_t;
typedef struct { char url[PUSH_URL_LEN]; char token[PUSH_URL_LEN]; } push_params_gotify_t;
typedef struct { char url[PUSH_URL_LEN]; char chat_id[PUSH_URL_LEN]; char bot_token[PUSH_URL_LEN]; } push_params_telegram_t;
typedef struct { char server[PUSH_URL_LEN]; char port[PUSH_PORT_LEN]; char user[PUSH_URL_LEN]; char password[PUSH_URL_LEN]; char to[PUSH_URL_LEN]; } push_params_smtp_t;
typedef struct { char url[PUSH_URL_LEN]; char token[PUSH_URL_LEN]; char user[PUSH_URL_LEN]; } push_params_pushover_t;

/** 中层：联合体，按 tag 只使用其中一个成员 */
typedef union {
    push_params_post_json_t  post_json;
    push_params_bark_t       bark;
    push_params_get_t        get;
    push_params_dingtalk_t   dingtalk;
    push_params_pushplus_t   pushplus;
    push_params_serverchan_t serverchan;
    push_params_feishu_t     feishu;
    push_params_gotify_t     gotify;
    push_params_telegram_t   telegram;
    push_params_smtp_t       smtp;
    push_params_pushover_t   pushover;
} push_params_u;

/** 顶层：统一配置包装器（tag + name + union） */
typedef struct {
    bool          enabled;
    push_type_t   type;   /* tag：决定 params 中有效成员 */
    char          name[MAX_STR_LEN];
    push_params_u params;
} push_channel_t;

// 定时任务动作类型
typedef enum {
    SCHED_ACTION_SMS  = 0,
    SCHED_ACTION_PING = 1
} schedule_action_t;

// 定时任务周期类型
typedef enum {
    SCHED_KIND_DAY   = 0,  // 每天 时:分
    SCHED_KIND_WEEK  = 1,  // 每周 周几(多选) 时:分
    SCHED_KIND_MONTH = 2   // 从初始日期起每 N 个月执行
} schedule_kind_t;

// 单条定时任务配置及上次执行结果
typedef struct {
    bool     enabled;
    schedule_action_t action;      // 发短信 或 PING
    schedule_kind_t   kind;        // 日 / 周 / 月
    uint8_t  hour;                // 0-23
    uint8_t  minute;              // 0-59
    uint8_t  weekday_mask;        // 周几多选：bit0=周日, bit1=周一, ... bit6=周六
    uint8_t  month_interval;      // 每几个月（1-12）
    time_t   initial_ts;          // 每月逻辑：初始日期时间（时间戳），从中取月/日/时/分，每 month_interval 个月执行
    char     phone[32];           // 短信目标号码
    char     message[MAX_BODY_LEN]; // 短信内容
    char     ping_host[64];       // PING 目标，如 8.8.8.8
    time_t   last_run_ts;         // 上次执行时间（0 表示未执行过）
    bool     last_ok;             // 上次是否成功
    char     last_msg[MAX_LAST_MSG_LEN]; // 上次结果说明
} schedule_task_t;

// 全局配置（SMTP 已并入推送通道 PUSH_TYPE_SMTP，无全局邮件/管理员手机）
typedef struct {
    char web_user[64];
    char web_pass[64];
    push_channel_t push_channels[MAX_PUSH_CHANNELS];
} app_config_t;

// 全局配置实例
extern app_config_t g_config;
extern bool g_config_valid;

// 初始化NVS并加载配置
void config_init(void);

// 保存配置到NVS
void config_save(void);

// 加载配置（内部调用）
void config_load(void);

// 检查单个推送通道是否有效
bool config_is_push_channel_valid(const push_channel_t *ch);

// 检查全局配置是否有效（至少配置了邮件或任一推送通道）
bool config_is_valid(void);

// WiFi STA 凭证（存 NVS，供连接失败后 AP 模式重新配置）
// 从 NVS 读取，若存在则 ssid/pass 写入缓冲区，返回 true（要求 ssid 与 pass 均存在）
bool config_get_wifi_sta(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
// 从 NVS 读取 WiFi 凭证供 Web API 展示，password 缺失时填空串，返回 true 表示读到 ssid
bool config_get_wifi_creds(char *ssid, size_t ssid_size, char *pass, size_t pass_size);
// 写入 NVS 并提交
void config_set_wifi_sta(const char *ssid, const char *pass);

// 定时任务：全局数组，由 config_load 加载、schedule_save 保存
extern schedule_task_t g_schedule_tasks[MAX_SCHEDULE_TASKS];
void schedule_load(void);
void schedule_save(void);

