#pragma once

#include <stdbool.h>
#include <stddef.h>

// 初始化WiFi STA模式并连接
void wifi_mgr_init(void);

// WiFi是否已连接
bool wifi_mgr_is_connected(void);

// 获取IP地址字符串（写入buf，返回buf）
char *wifi_mgr_get_ip(char *buf, size_t buf_size);

// 同步NTP时间
void wifi_mgr_sync_ntp(void);

// NTP时间是否已同步
bool wifi_mgr_time_synced(void);

// 获取设备URL（如 "http://192.168.1.100/"）
char *wifi_mgr_get_device_url(char *buf, size_t buf_size);

// WiFi信息结构体（用于Web查询）
typedef struct {
    bool  connected;
    char  ssid[33];
    int   rssi;
    char  ip[16];
    char  gateway[16];
    char  netmask[16];
    char  dns[16];
    char  mac[18];
    char  bssid[18];
    int   channel;
} wifi_info_t;

// 获取WiFi详细信息
void wifi_mgr_get_info(wifi_info_t *info);

// 当前是否处于 AP 模式（连接失败多次后进入，便于重新配置）
bool wifi_mgr_is_ap_mode(void);
