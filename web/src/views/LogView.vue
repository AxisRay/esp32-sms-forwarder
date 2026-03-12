<template>
  <div class="page-container">
    <!-- 短信发送测试 -->
    <el-card shadow="never" class="app-card">
      <template #header>
        <span class="card-title">短信发送测试</span>
      </template>
      <el-form ref="smsFormRef" :model="smsForm" :rules="smsRules" label-width="90px" class="full-width-form" label-position="left">
        <el-form-item label="目标号码" prop="phone">
          <el-input v-model="smsForm.phone" placeholder="手机号" />
        </el-form-item>
        <el-form-item label="内容" prop="message">
          <el-input v-model="smsForm.message" type="textarea" :rows="3" placeholder="短信内容" />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="sendingSms" @click="doSendSms">发送</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <!-- 4G 网络测试 -->
    <el-card shadow="never" class="app-card">
      <template #header>
        <span class="card-title">4G 网络测试</span>
      </template>
      <p v-if="pingResult" :class="['ping-result', pingResult.ok ? 'result-success' : 'result-error']">{{ pingResult.msg }}</p>
      <el-button type="warning" :loading="pinging" class="ping-btn" @click="doPing">Ping 测试</el-button>
      <p class="text-muted" style="margin: 10px 0 0">通过 4G 模组向 8.8.8.8 执行 Ping，会消耗少量流量。</p>
    </el-card>

    <!-- AT 指令测试 -->
    <el-card shadow="never" class="app-card">
      <template #header>
        <span class="card-title">AT 指令测试</span>
      </template>
      <div class="text-muted" style="margin-bottom: 6px">回显</div>
      <div ref="atOutputRef" class="code-block at-output">
        <pre>{{ atOutput || '（暂无输出）' }}</pre>
      </div>
      <div class="at-quick">
        <span class="text-muted" style="margin-right: 4px; white-space: nowrap; flex-shrink: 0">常用命令：</span>
        <el-button v-for="btn in atQuickBtns" :key="btn.cmd" size="small" @click="setAtAndSend(btn.cmd)">{{ btn.label }}</el-button>
      </div>
      <div class="at-input-row">
        <el-input-number v-model="atTimeout" :min="1" :max="30" :step="1" placeholder="超时(s)" class="at-timeout" controls-position="right" />
        <el-input v-model="atCmd" placeholder="输入 AT 指令，如 AT+CSQ" class="at-input" clearable @keyup.enter="doRunAt" />
        <el-button type="primary" :loading="sendingAt" @click="doRunAt">发送</el-button>
      </div>
      <p v-if="atError" class="at-error-hint">请输入 AT 指令</p>
    </el-card>

    <!-- ESP 模块日志 -->
    <el-card shadow="never" class="app-card">
      <template #header>
        <div class="log-card-header">
          <span class="card-title">ESP 日志</span>
          <div class="log-card-header-right">
            <span v-if="wsConnected" class="log-status result-success">实时</span>
            <span v-else class="log-status text-muted">未连接</span>
            <span v-if="memory.free != null" class="text-muted">内存 已用 {{ memoryUsedKb }} KB / 剩余 {{ memoryFreeKb }} KB</span>
          </div>
        </div>
      </template>
      <div class="log-toolbar">
        <el-button size="small" @click="refresh">刷新</el-button>
        <el-button size="small" @click="logLines = []">清空</el-button>
        <el-switch v-model="logAutoScroll" active-text="自动滚动" inline />
      </div>
      <div ref="logContainerRef" class="code-block log-content">
        <div v-for="(line, i) in logLines" :key="i" class="log-line">{{ line }}</div>
        <div v-if="loadingLogs" class="text-muted">加载中…</div>
        <div v-else-if="logLines.length === 0" class="text-muted">
          暂无日志（请连接设备以查看实时日志）
          <span v-if="isDev" class="dev-hint">开发时请在 .env 中设置 VITE_DEVICE_HOST 为设备 IP，或从设备地址打开页面</span>
        </div>
      </div>
    </el-card>
  </div>
</template>

<script setup>
import { ref, reactive, nextTick, watch, computed, onMounted, onUnmounted } from 'vue'
import { ElMessage } from 'element-plus'
import { sendSms, runAt, postPing, getMemory } from '../api'

const isDev = import.meta.env.DEV

// 短信发送测试
const smsFormRef = ref(null)
const smsForm = reactive({ phone: '', message: '' })
const smsRules = {
  phone: [{ required: true, message: '请填写目标号码', trigger: 'blur' }],
  message: [{ required: true, message: '请填写内容', trigger: 'blur' }],
}
const sendingSms = ref(false)

// 4G 网络测试
const pingResult = ref(null)
const pinging = ref(false)

// AT 指令测试
const atCmd = ref('AT')
const atOutput = ref('')
const atTimeout = ref(3)
const sendingAt = ref(false)
const atError = ref(false)
const atOutputRef = ref(null)

const atQuickBtns = [
  { cmd: 'AT', label: 'AT' },
  { cmd: 'ATI', label: 'ATI' },
  { cmd: 'AT+CSQ', label: '信号' },
  { cmd: 'AT+CEREG?', label: '注册' },
  { cmd: 'AT+COPS?', label: '运营商' },
  { cmd: 'AT+CIMI', label: 'IMSI' },
  { cmd: 'AT+ICCID', label: 'ICCID' },
]

// ESP 模块日志
const logLines = ref([])
const memory = ref({})
const memoryFreeKb = computed(() => (memory.value.free != null ? Math.round(memory.value.free / 1024) : '--'))
const memoryUsedKb = computed(() => (memory.value.used != null ? Math.round(memory.value.used / 1024) : '--'))
const loadingLogs = ref(false)
const logAutoScroll = ref(true)
const logContainerRef = ref(null)
const wsConnected = ref(false)

let ws = null

// 短信发送测试方法
async function doSendSms() {
  try {
    await smsFormRef.value?.validate()
  } catch {
    return
  }
  sendingSms.value = true
  try {
    const res = await sendSms(smsForm.phone.trim(), smsForm.message.trim())
    if (res?.status === 'ok') {
      ElMessage.success('发送成功')
    } else {
      ElMessage.error(res?.message || '发送失败')
    }
  } catch (e) {
    ElMessage.error(e?.message || '请求失败')
  } finally {
    sendingSms.value = false
  }
}

// 4G 网络测试方法
async function doPing() {
  pinging.value = true
  pingResult.value = null
  try {
    const res = await postPing()
    const ok = res?.success === true
    pingResult.value = { ok, msg: ok ? (res.message || 'Ping 成功') : (res.message || 'Ping 失败') }
  } catch (e) {
    pingResult.value = { ok: false, msg: e?.message || '请求失败' }
  } finally {
    pinging.value = false
  }
}

// AT 指令测试方法
function scrollAtOutputToBottom() {
  nextTick(() => {
    const el = atOutputRef.value
    if (el) el.scrollTop = el.scrollHeight
  })
}

watch(atOutput, scrollAtOutputToBottom)

function setAtAndSend(cmd) {
  atCmd.value = cmd
  doRunAt()
}

async function doRunAt() {
  const cmd = atCmd.value?.trim()
  atError.value = !cmd
  if (!cmd) return
  sendingAt.value = true
  const timeout = Math.max(1000, Math.min(30000, Number(atTimeout.value) * 1000 || 3000))
  try {
    const res = await runAt(cmd, timeout)
    const text = res?.response ?? res?.data?.response ?? JSON.stringify(res)
    atOutput.value = (atOutput.value ? atOutput.value + '\n\n' : '') + text
    scrollAtOutputToBottom()
  } catch (e) {
    atOutput.value = (atOutput.value ? atOutput.value + '\n\n' : '') + 'Error: ' + (e?.message || '请求失败')
    scrollAtOutputToBottom()
  } finally {
    sendingAt.value = false
  }
}

// ESP 模块日志方法
// 开发时页面在 localhost，需通过 VITE_DEVICE_HOST 指定设备地址才能连上设备 WebSocket；生产时用当前 host 即可
function getWsUrl() {
  const proto = location.protocol === 'https:' ? 'wss:' : 'ws:'
  const host =
    (import.meta.env.DEV && import.meta.env.VITE_DEVICE_HOST) || location.host
  return `${proto}//${host}/ws/log`
}

function scrollLogToBottom() {
  const el = logContainerRef.value
  if (el) el.scrollTop = el.scrollHeight
}

function connectWs() {
  if (ws) {
    try { ws.close() } catch (_) {}
    ws = null
  }
  wsConnected.value = false
  try {
    ws = new WebSocket(getWsUrl())
    ws.binaryType = 'blob'
    ws.onopen = () => {
      wsConnected.value = true
      loadingLogs.value = false
    }
    ws.onmessage = (ev) => {
      const text = typeof ev.data === 'string' ? ev.data : (ev.data && ev.data.toString ? ev.data.toString() : '')
      if (text) {
        logLines.value = logLines.value.concat(text.split('\n').filter(Boolean))
        if (logAutoScroll.value) nextTick(scrollLogToBottom)
      }
    }
    ws.onclose = () => {
      wsConnected.value = false
      ws = null
    }
    ws.onerror = () => {
      wsConnected.value = false
      ws = null
    }
  } catch (_) {}
}

async function fetchMemory() {
  try {
    memory.value = await getMemory()
  } catch (_) {
    memory.value = {}
  }
}

function refresh() {
  loadingLogs.value = true
  connectWs()
}

watch(logAutoScroll, (v) => { if (v) nextTick(scrollLogToBottom) })

let memoryInterval = null
onMounted(() => {
  loadingLogs.value = true
  connectWs()
  fetchMemory()
  memoryInterval = setInterval(fetchMemory, 10000)
})

onUnmounted(() => {
  if (memoryInterval) clearInterval(memoryInterval)
  if (ws) {
    try { ws.close() } catch (_) {}
    ws = null
  }
})
</script>

<style scoped>
.full-width-form { width: 100%; }
.ping-result { font-size: 13px; margin: 0 0 10px; text-align: left; min-height: 1.4em; }
.ping-btn { width: 100%; }

.at-output { height: 200px; margin-bottom: 10px; }
.at-quick { margin-bottom: 10px; display: flex; flex-wrap: nowrap; align-items: center; gap: 6px; overflow-x: auto; scrollbar-width: none; -webkit-overflow-scrolling: touch; }
.at-quick::-webkit-scrollbar { display: none; }
.at-quick .el-button { flex-shrink: 0; }
.at-input-row { display: flex; gap: 8px; align-items: center; }
.at-timeout { width: 80px; flex-shrink: 0; }
.at-input { flex: 1; min-width: 120px; font-family: ui-monospace, monospace; }
.at-error-hint { margin: 6px 0 0; font-size: 12px; color: var(--el-color-danger); text-align: left; }

.log-status { font-size: 12px; }
.log-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  width: 100%;
  gap: 8px;
}
.log-card-header-right {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}
.log-toolbar { margin-bottom: 8px; display: flex; align-items: center; gap: 8px; }
.log-content { height: 480px; }
.log-line { white-space: pre-wrap; word-break: break-all; }
.dev-hint { display: block; margin-top: 6px; font-size: 12px; opacity: 0.85; }
@media (max-width: 480px) {
  .log-content { height: 240px; }
}
</style>