#include "wifi_mgr.h"
#include "config.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#include <freertos/FreeRTOS.h>
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECT_RETRY_MAX  5   // 连续失败此次数后进入 AP 模式
#define WIFI_AP_SSID            "SMS-Forwarder-Setup"
#define WIFI_AP_PASS            "12345678"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_GIVE_UP_BIT    BIT1
static bool s_time_synced = false;
static bool s_ap_mode = false;
static int s_retry_count = 0;

static void wifi_mgr_apply_timezone(void)
{
    // 默认按中国时区（UTC+8）运行；若需要其它时区，可后续做成可配置项
    setenv("TZ", "CST-8", 1);
    tzset();

    time_t now = time(NULL);
    if (now > 100000) {
        struct tm t;
        char buf[32];
        localtime_r(&now, &t);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
        ESP_LOGI(TAG, "时区已设置为 CST-8，本地时间: %s", buf);
    } else {
        ESP_LOGI(TAG, "时区已设置为 CST-8（当前时间未同步，稍后生效）");
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_retry_count = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        s_retry_count++;
        ESP_LOGW(TAG, "WiFi 断开，重试 %d/%d ...", s_retry_count, WIFI_CONNECT_RETRY_MAX);
        if (s_retry_count >= WIFI_CONNECT_RETRY_MAX) {
            ESP_LOGW(TAG, "连接失败次数已达 %d 次，进入 AP 模式便于重新配置", WIFI_CONNECT_RETRY_MAX);
            xEventGroupSetBits(s_wifi_event_group, WIFI_GIVE_UP_BIT);
        } else {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_retry_count = 0;
        ESP_LOGI(TAG, "获取到IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void start_ap_mode(void)
{
    ESP_LOGI(TAG, "启动 AP 模式: SSID=%s", WIFI_AP_SSID);
    // 若从未启动过 STA，stop 会返回错误，忽略即可
    esp_wifi_stop();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid_len = (uint8_t)strlen(WIFI_AP_SSID),
            .channel = 1,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .max_connection = 4,
            .pmf_cfg = { .required = false },
        },
    };
    strncpy((char *)ap_cfg.ap.ssid, WIFI_AP_SSID, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, WIFI_AP_PASS, sizeof(ap_cfg.ap.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_ap_mode = true;
}

void wifi_mgr_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    s_ap_mode = false;
    s_retry_count = 0;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // WiFi 账号密码仅从 NVS 读取，默认为空；无合法配置则直接进入 AP 模式
    char ssid[33] = {0}, pass[65] = {0};
    if (!config_get_wifi_sta(ssid, sizeof(ssid), pass, sizeof(pass)) || strlen(ssid) == 0) {
        ESP_LOGI(TAG, "NVS 中无有效 WiFi 配置，直接进入 AP 模式");
        start_ap_mode();
        ESP_LOGI(TAG, "请连接 WiFi \"%s\" 密码 \"%s\" 后访问 http://192.168.4.1 配置", WIFI_AP_SSID, WIFI_AP_PASS);
        return;
    }

    wifi_config_t wifi_cfg = {
        .sta = {
            .scan_method = WIFI_ALL_CHANNEL_SCAN,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    strncpy((char *)wifi_cfg.sta.ssid, ssid, sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, pass, sizeof(wifi_cfg.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi 初始化完成，正在连接 %s ...", ssid);

    // 等待连接成功或失败次数达到上限
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_GIVE_UP_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);
    if (bits & WIFI_GIVE_UP_BIT) {
        start_ap_mode();
        ESP_LOGI(TAG, "连接失败次数已达上限，请连接 \"%s\" 密码 \"%s\" 后访问 http://192.168.4.1 重新配置", WIFI_AP_SSID, WIFI_AP_PASS);
        return;
    }
    ESP_LOGI(TAG, "WiFi 已连接");
}

bool wifi_mgr_is_ap_mode(void)
{
    return s_ap_mode;
}

bool wifi_mgr_is_connected(void)
{
    if (!s_wifi_event_group) return false;
    return (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

char *wifi_mgr_get_ip(char *buf, size_t buf_size)
{
    if (s_ap_mode) {
        snprintf(buf, buf_size, "192.168.4.1");
        return buf;
    }
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(buf, buf_size, IPSTR, IP2STR(&ip_info.ip));
    } else {
        snprintf(buf, buf_size, "0.0.0.0");
    }
    return buf;
}

void wifi_mgr_sync_ntp(void)
{
    ESP_LOGI(TAG, "正在同步NTP时间...");

    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.ntsc.ac.cn");
    esp_sntp_setservername(1, "ntp.aliyun.com");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();

    int retry = 0;
    while (time(NULL) < 100000 && retry < 100) {
        vTaskDelay(pdMS_TO_TICKS(100));
        retry++;
    }

    if (time(NULL) >= 100000) {
        s_time_synced = true;
        ESP_LOGI(TAG, "NTP时间同步成功，UTC时间戳: %lld", (long long)time(NULL));
        wifi_mgr_apply_timezone();
    } else {
        ESP_LOGW(TAG, "NTP时间同步失败，将使用设备时间");
        wifi_mgr_apply_timezone();
    }
}

bool wifi_mgr_time_synced(void)
{
    return s_time_synced;
}

char *wifi_mgr_get_device_url(char *buf, size_t buf_size)
{
    char ip[16];
    wifi_mgr_get_ip(ip, sizeof(ip));
    snprintf(buf, buf_size, "http://%s/", ip);
    return buf;
}

void wifi_mgr_get_info(wifi_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->connected = wifi_mgr_is_connected();

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        strncpy(info->ssid, (const char *)ap.ssid, sizeof(info->ssid) - 1);
        info->rssi = ap.rssi;
        snprintf(info->bssid, sizeof(info->bssid),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 ap.bssid[0], ap.bssid[1], ap.bssid[2],
                 ap.bssid[3], ap.bssid[4], ap.bssid[5]);
        info->channel = ap.primary;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(info->ip, sizeof(info->ip), IPSTR, IP2STR(&ip_info.ip));
        snprintf(info->gateway, sizeof(info->gateway), IPSTR, IP2STR(&ip_info.gw));
        snprintf(info->netmask, sizeof(info->netmask), IPSTR, IP2STR(&ip_info.netmask));
    }

    esp_netif_dns_info_t dns_info;
    if (netif && esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK) {
        snprintf(info->dns, sizeof(info->dns), IPSTR,
                 IP2STR(&dns_info.ip.u_addr.ip4));
    }

    uint8_t mac[6];
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) == ESP_OK) {
        snprintf(info->mac, sizeof(info->mac),
                 "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}
