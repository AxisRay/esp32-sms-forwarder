/**
 * 开发环境 API Mock，供 npm run dev 时前端调用，便于无设备时测试验证。
 * 与 ESP32 web_server 的接口形状保持一致。
 */

// 内存中的 mock 状态，便于配置/短信测试
let mockConfig = {
  wifiSsid: 'MyWiFi',
  wifiPass: '********',
}
let mockWifi = {
  ssid: 'MyWiFi',
  password: '********',
}
let mockAuth = { user: '', password: '' }
// 推送通道类型与 code.ino / config.h 一致
const PUSH_TYPES = [
  { value: 0, label: '未启用' },
  { value: 1, label: 'POST JSON' },
  { value: 2, label: 'GET' },
  { value: 3, label: '邮件(SMTP)' },
  { value: 4, label: 'Bark' },
  { value: 5, label: '钉钉' },
  { value: 6, label: 'PushPlus' },
  { value: 7, label: 'Server酱' },
  { value: 8, label: '飞书' },
  { value: 9, label: 'Gotify' },
  { value: 10, label: 'Telegram' },
]
const MAX_PUSH_CHANNELS = 5
function defaultChannel(i) {
  return {
    enabled: false,
    type: 1,
    name: '通道' + (i + 1),
    url: '',
    key1: '',
    key2: '',
    customBody: '',
  }
}
let mockPushChannels = [
  { ...defaultChannel(0), enabled: true, type: 1, name: '示例', url: 'https://example.com/webhook' },
  defaultChannel(1),
  defaultChannel(2),
  defaultChannel(3),
  defaultChannel(4),
]
const mockSmsHistory = [
  { id: 1, ts: Date.now() - 3600000, sender: '10086', content: '【中国移动】您的余额不足10元。', port: 0 },
  { id: 2, ts: Date.now() - 3600000, sender: '18124667412', content: '测试短信', port: 0 },
  { id: 3, ts: Date.now() - 10200000, sender: '95588', content: '【工商银行】您尾号1234的账户收到转账100元。您尾号1234的账户收到转账100元。您尾号1234的账户收到转账100元。您尾号1234的账户收到转账100元。您尾号1234的账户收到转账100元。您尾号1234的账户收到转账100元。', port: 0 },
]
const mockLogLines = [
  '[I][wifi_mgr] WiFi connected, ip: 192.168.1.100',
  '[I][modem] AT+CEREG? -> 0,1',
  '[I][web_srv] 启动 Web 服务器，端口: 80',
]

function sendJson(res, statusCode, body) {
  res.statusCode = statusCode
  res.setHeader('Content-Type', 'application/json; charset=utf-8')
  res.end(JSON.stringify(body))
}

function parseBody(req) {
  return new Promise((resolve) => {
    let body = ''
    req.on('data', (chunk) => { body += chunk })
    req.on('end', () => {
      try {
        resolve(body ? JSON.parse(body) : {})
      } catch {
        resolve({})
      }
    })
  })
}

/** 统一 API 响应格式：{ code: 0, msg: "ok", data } / { code: 非0, msg } */
function apiSuccess(res, data = null) {
  sendJson(res, 200, data !== null ? { code: 0, msg: 'ok', data } : { code: 0, msg: 'ok' })
}
function apiError(res, code, msg) {
  sendJson(res, 200, { code, msg })
}

export function createMockApiMiddleware() {
  return async (req, res, next) => {
    const url = req.url?.split('?')[0] || ''
    const method = req.method

    // GET /api/config -> 兼容现有 ESP 返回（无 code 包装）
    if (method === 'GET' && url === '/api/config') {
      sendJson(res, 200, {
        wifiSsid: mockConfig.wifiSsid,
        wifiPass: mockConfig.wifiPass,
      })
      return
    }

    // POST /api/config
    if (method === 'POST' && url === '/api/config') {
      const body = await parseBody(req)
      if (body.wifiSsid != null) mockConfig.wifiSsid = String(body.wifiSsid)
      if (body.wifiPass != null) mockConfig.wifiPass = String(body.wifiPass)
      sendJson(res, 200, { status: 'ok' })
      return
    }

    // GET /api/status/network -> 网络/模组状态
    if (method === 'GET' && url === '/api/status/network') {
      sendJson(res, 200, {
        wifi: {
          mode: 'STA',
          connected: true,
          ssid: mockWifi.ssid,
          ip: '192.168.1.100',
          gateway: '192.168.1.1',
          rssi: -62,
          channel: 6,
          status: '强',
        },
        signal: {
          rsrp_str: '-85 dBm',
          rsrq_str: '-10.5 dB',
          quality: '良好',
          rsrp_dbm: -85,
          level: 4,
        },
        network: {
          reg_status: '已注册（本地）',
          registered: true,
          operator: '中国移动',
        },
      })
      return
    }

    // GET /api/version -> 固件版本与编译信息
    if (method === 'GET' && url === '/api/version') {
      sendJson(res, 200, { fw_version: 'V1.0.0 Build 202603131149 [Mock]' })
      return
    }

    // GET /api/sms/history?offset=0&limit=20
    if (method === 'GET' && url === '/api/sms/history') {
      const u = new URL(req.url || '', 'http://localhost')
      const offset = Math.max(0, parseInt(u.searchParams.get('offset') || '0', 10))
      const limit = Math.min(100, Math.max(1, parseInt(u.searchParams.get('limit') || '20', 10)))
      const list = mockSmsHistory.slice(offset, offset + limit)
      sendJson(res, 200, { list, total: mockSmsHistory.length })
      return
    }

    // POST /api/sms/send
    if (method === 'POST' && url === '/api/sms/send') {
      const body = await parseBody(req)
      const phone = body.phone || body.to
      const message = body.message || body.content
      if (!phone || !message) {
        sendJson(res, 200, { status: 'error', message: '缺少 phone 或 message' })
        return
      }
      sendJson(res, 200, { success: true, message: '发送成功', status: 'ok' })
      return
    }

    // POST /api/at
    if (method === 'POST' && url === '/api/at') {
      const body = await parseBody(req)
      const cmd = body.cmd || ''
      if (!cmd) {
        sendJson(res, 400, { error: 'Missing cmd' })
        return
      }
      sendJson(res, 200, {
        response: `\r\n${cmd}\r\nOK\r\n[Mock] 已模拟执行: ${cmd}\r\n`,
      })
      return
    }

    // GET /api/config/wifi
    if (method === 'GET' && url === '/api/config/wifi') {
      sendJson(res, 200, { ...mockWifi })
      return
    }

    // POST /api/config/wifi
    if (method === 'POST' && url === '/api/config/wifi') {
      const body = await parseBody(req)
      if (body.ssid != null) mockWifi.ssid = String(body.ssid)
      if (body.password != null) mockWifi.password = String(body.password)
      sendJson(res, 200, { status: 'ok' })
      return
    }

    // GET /api/config/auth -> Basic 认证账户，不返回真实密码
    if (method === 'GET' && url === '/api/config/auth') {
      sendJson(res, 200, { user: mockAuth.user, hasPassword: mockAuth.password.length > 0 })
      return
    }

    // POST /api/config/auth
    if (method === 'POST' && url === '/api/config/auth') {
      const body = await parseBody(req)
      if (body.user != null) mockAuth.user = String(body.user)
      if (body.password != null && body.password !== '') mockAuth.password = String(body.password)
      sendJson(res, 200, { status: 'ok' })
      return
    }

    // GET /api/config/push -> 多通道
    if (method === 'GET' && url === '/api/config/push') {
      sendJson(res, 200, { channels: mockPushChannels.map((c) => ({ ...c })) })
      return
    }

    // POST /api/config/push
    if (method === 'POST' && url === '/api/config/push') {
      const body = await parseBody(req)
      if (Array.isArray(body.channels)) {
        body.channels.slice(0, MAX_PUSH_CHANNELS).forEach((c, i) => {
          if (!mockPushChannels[i]) mockPushChannels[i] = defaultChannel(i)
          mockPushChannels[i] = { ...defaultChannel(i), ...c, type: Number(c.type) || 0 }
        })
      }
      sendJson(res, 200, { status: 'ok' })
      return
    }

    // GET /api/config/schedule
    if (method === 'GET' && url === '/api/config/schedule') {
      sendJson(res, 200, { tasks: [] })
      return
    }

    // POST /api/config/schedule
    if (method === 'POST' && url === '/api/config/schedule') {
      sendJson(res, 200, { status: 'ok' })
      return
    }

    // GET /api/debug/memory
    if (method === 'GET' && url === '/api/debug/memory') {
      sendJson(res, 200, { free: 128 * 1024, total: 320 * 1024, min_free: 64 * 1024, used: 192 * 1024 })
      return
    }

    // POST /api/debug/ping 4G Ping 测试
    if (method === 'POST' && url === '/api/debug/ping') {
      sendJson(res, 200, { success: true, message: '目标: 8.8.8.8, 延迟: 128ms, TTL: 64 [Mock]' })
      return
    }

    // POST /api/push/test 测试推送通道
    if (method === 'POST' && url === '/api/push/test') {
      const body = await parseBody(req)
      const idx = body.index != null ? body.index : 0
      sendJson(res, 200, { success: true, message: `通道 ${idx + 1} 测试成功 [Mock]` })
      return
    }

    next()
  }
}
