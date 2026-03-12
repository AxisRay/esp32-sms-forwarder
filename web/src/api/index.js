/**
 * 前端 API 封装，与 ESP32 web_server 接口约定一致。
 * 开发时由 Vite mock 提供 /api 响应，生产时请求设备地址。
 */

const API_BASE = ''

function get(url) {
  return fetch(API_BASE + url, { method: 'GET' }).then((r) => r.json())
}

function post(url, body) {
  return fetch(API_BASE + url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  }).then((r) => r.json())
}

// ---------- 状态 ----------
/** GET /api/status/network：Wi-Fi + 4G 状态 */
export function getNetworkStatus() {
  return get('/api/status/network')
}

/** GET /api/sms/history?offset=&limit= 短信历史，返回 { list, total } */
export function getSmsHistory(params = {}) {
  const u = new URLSearchParams()
  if (params.offset != null) u.set('offset', params.offset)
  if (params.limit != null) u.set('limit', params.limit)
  const q = u.toString()
  return get('/api/sms/history' + (q ? '?' + q : '')).then((res) => ({
    list: res.list ?? [],
    total: res.total ?? 0,
  }))
}

// ---------- 配置 ----------

/** GET /api/config/wifi，返回 { ssid, password } */
export function getWifiConfig() {
  return get('/api/config/wifi')
}

/** POST /api/config/wifi */
export function saveWifiConfig(body) {
  return post('/api/config/wifi', body)
}

/** GET /api/config/auth，返回 { user, hasPassword }，不返回真实密码 */
export function getAuthConfig() {
  return get('/api/config/auth')
}

/** POST /api/config/auth，body: { user, password }，密码留空表示不修改 */
export function saveAuthConfig(body) {
  return post('/api/config/auth', body)
}

/** GET /api/config/push，返回 { channels: [...] } */
export function getPushConfig() {
  return get('/api/config/push').then((res) => ({
    channels: res.channels ?? [],
  }))
}

/** POST /api/config/push，body: { channels: [...] } */
export function savePushConfig(body) {
  return post('/api/config/push', body)
}

/** GET /api/config/schedule，返回 { tasks: [...] }，最多 3 条 */
export function getScheduleConfig() {
  return get('/api/config/schedule').then((res) => ({
    tasks: res.tasks ?? [],
  }))
}

/** POST /api/config/schedule，body: { tasks: [...] } */
export function saveScheduleConfig(body) {
  return post('/api/config/schedule', body)
}

// ---------- 调试 ----------
/** POST /api/sms/send 发送短信 */
export function sendSms(phone, message) {
  return post('/api/sms/send', { phone, message })
}

/** POST /api/at 执行 AT 指令 */
export function runAt(cmd, timeout = 3000) {
  return post('/api/at', { cmd, timeout })
}

/** POST /api/debug/ping 4G 模组 Ping 测试（消耗少量流量） */
export function postPing() {
  return post('/api/debug/ping', {})
}

/** GET /api/debug/memory 设备内存：{ free, total, min_free, used } 单位字节 */
export function getMemory() {
  return get('/api/debug/memory')
}

/** POST /api/push/test 测试指定推送通道，body: { index: number } */
export function postPushTest(payload) {
  return post('/api/push/test', payload)
}
