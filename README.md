# ESP32 短信转发器 (ESP32 SMS Forwarder)

基于 **ESP32-C3** 与 **L307A** 4G Cat.1 模组的智能短信转发器。设备插入 SIM 卡并连接 WiFi 后，可将收到的短信实时转发到邮件、Bark、钉钉、飞书、Telegram、PushPlus、Server 酱、Gotify 及自定义 Webhook 等通道，并支持定时任务（定时发短信或 PING 检测）、Web 可视化管理与短信远程配网。

---

## 项目介绍

### 功能特性

- **短信接收与处理**：通过 AT 指令与 PDU 模式接收、解析短信，支持长短信自动合并。
- **多通道推送**：最多 5 个独立推送通道，支持：
  - 邮件 (SMTP，支持 SSL/TLS，常用端口 465)
  - Bark、钉钉、飞书、Telegram Bot、PushPlus、Server 酱、Gotify
  - 自定义 Webhook (GET/POST JSON 或自定义 Payload)
- **Web 管理后台**：内置 HTTP 服务器与 Vue 单页应用，可配置 Wi-Fi、推送、定时任务、查看状态与日志。
- **短信远程配网**：向设备 SIM 发送 `WIFI:SSID,密码` 即可更新 Wi-Fi 并重启。
- **定时任务**：最多 3 个任务，支持按日/周/月执行「发送短信」或「PING」，结果推送到已配置通道。
- **NVS 默认配置**：通过 `nvs_config.csv` 预置默认 Wi-Fi、后台账号等，便于量产或恢复出厂。

### 硬件平台

| 项目     | 型号说明                    |
|----------|-----------------------------|
| 主控     | **ESP32-C3**（RISC-V 单核）  |
| 通信模组 | **ML307A**（4G Cat.1） |
| 串口     | UART1，115200 8N1            |

### 接线说明

模组与 ESP32-C3 通过 UART 连接，**交叉连接**：MCU 的 TX 接模组的 RX，MCU 的 RX 接模组的 TX。

| ESP32-C3 引脚 | 连接至 ML307A | 说明           |
|---------------|----------------|----------------|
| **GPIO5**     | MODEM_EN       | 模组使能/开关  |
| **GPIO3**     | 模组 **RX**    | ESP32 的 UART TX（发往模组） |
| **GPIO4**     | 模组 **TX**    | ESP32 的 UART RX（来自模组） |

接线图（文字示意）：

```
  ESP32-C3                    ML307A
  ┌─────────┐                 ┌──────────┐
  │  GPIO5  ├────────────────►│ MODEM_EN │
  │  GPIO3  ├────────────────►│   RX     │   (ESP32 TX → 模组 RX)
  │  GPIO4  ├◄────────────────┤   TX     │   (ESP32 RX ← 模组 TX)
  │  GND    ├────────────────►│   GND    │
  │  VCC    │─────────────────│   VCC    │
  └─────────┘                 └──────────┘
```

- 电源：ML307A 供电需按模组规格单独提供（电压/电流满足要求）；若开发板可从 3.3V 取电且电流足够，可与 ESP32-C3 共电源。
- 若使用其他 ESP32 系列（如 ESP32-S3），需在 `components/modem_driver/include/modem.h` 中确认或修改引脚宏（当前为 GPIO3/4/5）。

---

## 刷写说明

### 环境要求

- **ESP-IDF v5.x**（推荐 v5.5 或与项目 `sdkconfig` 兼容版本）
- 前端构建：**Node.js 18+**、**npm**（用于 `web/` 下 Vue 打包，`idf.py build` 会自动执行）

### 1. 准备 ESP-IDF

```bash
# 激活 IDF 环境（路径按本机安装调整）
source /path/to/esp-idf/export.sh
```

### 2. 编译

在项目根目录执行（会自动先构建 `web/` 前端再编译固件）：

```bash
idf.py build
```

### 3. 烧录

连接 ESP32-C3 到电脑，确认串口（如 Linux 下 `/dev/ttyACM0` 或 `/dev/ttyUSB0`）：

```bash
idf.py -p /dev/ttyACM0 flash
```

烧录时会将 `nvs_config.csv` 打包为 NVS 分区一并写入，**会覆盖设备内已有 NVS 配置**（如 Wi-Fi、推送、后台密码等）。

### 4. 仅更新固件、保留配置

若只想更新程序，不覆盖 NVS 中已保存的配置：

```bash
idf.py -DSKIP_NVS_FLASH=ON fullclean
idf.py -DSKIP_NVS_FLASH=ON build
idf.py -p /dev/ttyACM0 -DSKIP_NVS_FLASH=ON flash
```

### 5. 查看串口日志

```bash
idf.py -p /dev/ttyACM0 monitor
```

退出 monitor：`Ctrl + ]`。

---

## 使用说明

### 1. 首次上电与 NVS 默认值

- 首次烧录后，设备会使用 `nvs_config.csv` 中的默认值（若已配置）：
  - **Web 后台账号**：默认用户名 `admin`，密码 `admin123`（建议首次登录后在「配置」中修改）。
  - 可在该 CSV 中预置 `wifi_ssid`、`wifi_pass` 等，减少首次配网步骤。

### 2. 连接 WiFi

- **方式一：Web 后台**  
  设备连上 WiFi 后，在浏览器访问设备 IP（见串口日志或启动推送中的「设备地址」），用上述账号登录，在「配置 → Wi-Fi 配置」中填写 SSID 和密码并保存。

- **方式二：短信配网**  
  向设备中的 SIM 卡发送短信，内容格式：
  ```text
  WIFI:你的WiFi名称,你的WiFi密码
  ```
  设备会保存并重启，连接新 WiFi。

### 3. Web 后台功能概览

- **状态**：Wi-Fi 连接状态、4G 模组信号与注册状态、短信历史列表。
- **任务**：定时任务（最多 3 个），可设每日/每周/每月在指定时间「发送短信」或「PING」，执行结果会推送到已配置的通道。
- **配置**：  
  - Wi-Fi 配置  
  - 账户密码（Web 与 API 的 Basic 认证）  
  - 推送配置：多通道（含 SMTP 邮件、Bark、钉钉、飞书、Telegram、PushPlus、Server 酱、Gotify、自定义 Webhook），每通道可单独测试。
- **工具**：实时日志（WebSocket）、4G Ping 测试等。

### 4. 推送与邮件

- 在「配置 → 推送配置」中新增通道并填写对应参数（如 SMTP 服务器、端口、发件人、授权码、收件人；或各平台 Webhook/Token）。
- 使用 SMTP 时，多数邮箱需使用**授权码**而非登录密码。
- 收到短信后，设备会将发送方与内容推送到所有已启用通道（含邮件）。

### 5. 注意事项

- 插入的 **SIM 卡**需开通短信功能并处于有效信号覆盖下；模组需完成网络注册（可在「状态」页查看）。
- 若只更新固件、不想丢失当前配置，请使用上文「仅更新固件、保留配置」的刷写方式。

---

## 目录结构

```text
├── CMakeLists.txt           # 项目 CMake，含 NVS 分区与前端构建依赖
├── nvs_config.csv           # NVS 默认配置（烧录时写入设备）
├── sdkconfig.defaults       # 默认 sdkconfig 选项（如 TCP 窗口、WebSocket）
├── main/
│   ├── main.c               # 主入口、任务调度与初始化
│   └── CMakeLists.txt
├── components/
│   ├── app_core/            # 配置管理、事件、定时任务结构
│   ├── modem_driver/        # ML307A UART 驱动、AT、PDU、Ping
│   ├── network_manager/     # WiFi 与 NTP
│   ├── notification/        # SMTP 与多通道推送 (push.c)
│   └── web_server/          # HTTP 服务、API、WebSocket 日志
├── web/                     # Vue 前端（构建后嵌入固件）
│   ├── src/
│   │   ├── views/           # 状态、任务、配置、工具页
│   │   └── api/
│   └── package.json
└── scripts/                 # 如 monitor 日志抓取脚本
```

---

## 许可证与致谢

**本项目采用 [GNU General Public License v3.0 (GPLv3)](https://www.gnu.org/licenses/gpl-3.0.html) 开源协议。** 使用、修改或再分发本代码须遵守 GPLv3 条款。

本项目在设计与功能上参考了 [chenxuuu/sms_forwarding](https://github.com/chenxuuu/sms_forwarding)（超低成本硬件短信转发器，ML307R+ESP32C3），该参考项目采用 **MIT 协议**，特此致谢。

本项目基于 ESP-IDF 与 Vue/Element Plus 等开源组件开发，请遵守各组件对应许可证。  
硬件目标：ESP32-C3 + ML307A；其他模组需自行适配 AT/PDU 与引脚。
