<template>
  <div class="config-view">
    <el-card header="Wi-Fi 配置" shadow="never" class="app-card">
      <el-form ref="wifiFormRef" :model="wifiForm" label-width="90px" class="wifi-form" label-position="left">
        <el-form-item label="SSID" prop="ssid">
          <el-input v-model="wifiForm.ssid" placeholder="Wi-Fi 名称" clearable />
        </el-form-item>
        <el-form-item label="密码" prop="password">
          <el-input
            v-model="wifiForm.password"
            type="password"
            :placeholder="wifiHasSavedPassword ? '已配置（输入新密码以修改）' : 'Wi-Fi 密码'"
            show-password
            clearable
          />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving.wifi" :disabled="!wifiFormDirty" @click="saveWifi">保存</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card header="账户密码（Basic 认证）" shadow="never" class="app-card auth-card">
      <p class="auth-desc">设置后，访问本设备 Web 与 API 需输入此用户名和密码。</p>
      <el-form ref="authFormRef" :model="authForm" label-width="90px" class="auth-form" label-position="left">
        <el-form-item label="用户名" prop="user">
          <el-input v-model="authForm.user" placeholder="登录用户名" clearable maxlength="60" show-word-limit />
        </el-form-item>
        <el-form-item label="密码" prop="password">
          <el-input
            v-model="authForm.password"
            type="password"
            :placeholder="authHasSavedPassword ? '已配置（输入新密码以修改）' : '登录密码'"
            show-password
            clearable
            maxlength="60"
            show-word-limit
          />
        </el-form-item>
        <el-form-item>
          <el-button type="primary" :loading="saving.auth" :disabled="!authFormDirty" @click="saveAuth">保存</el-button>
        </el-form-item>
      </el-form>
    </el-card>

    <el-card header="推送配置" shadow="never" class="app-card push-card">
      <p class="push-desc">通过按钮新增或删除推送通道，通道测试需要先保存。</p>
      <div v-for="(ch, index) in pushChannels" :key="index" class="collapse-card">
        <div class="collapse-card-header" @click="ch.expand = !ch.expand">
          <span class="expand-icon">{{ ch.expand ? '▼' : '▶' }}</span>
          <span class="collapse-card-title">{{ ch.name || '通道 ' + (index + 1) }}</span>
          <span class="collapse-card-tag">{{ pushTypeLabel(ch.type) }}</span>
        </div>
        <div v-if="ch.testResult" class="result-message" :class="ch.testResult.ok ? 'success' : 'error'">
          {{ ch.testResult.ok ? '✓ ' : '✗ ' }}{{ ch.testResult.msg }}
        </div>
        <div v-show="ch.expand" class="collapse-card-body">
          <el-form label-width="80px" size="small" label-position="left" class="channel-form">
            <el-form-item label="名称">
              <el-input v-model="ch.name" placeholder="自定义名称" />
            </el-form-item>
            <el-form-item label="类型">
              <el-select v-model="ch.type" placeholder="选择方式" style="width: 100%">
                <el-option v-for="opt in pushTypeOptions" :key="opt.value" :label="opt.label" :value="opt.value" />
              </el-select>
            </el-form-item>
            <!-- ch.type=3: SMTP 邮件 -->
            <template v-if="ch.type === 3">
              <el-form-item label="服务器">
                <el-input v-model="ch.url" placeholder="如 smtp.qq.com" />
              </el-form-item>
              <el-form-item label="端口">
                <el-input v-model="ch.port" placeholder="465" style="width: 100px" />
              </el-form-item>
              <el-form-item label="账号">
                <el-input v-model="ch.user" placeholder="邮箱账号" />
              </el-form-item>
              <el-form-item label="密码">
                <el-input v-model="ch.password" type="password" placeholder="密码" show-password />
              </el-form-item>
              <el-form-item label="收件人">
                <el-input v-model="ch.customBody" placeholder="接收推送的邮箱" />
              </el-form-item>
            </template>
            <!-- ch.type=1: POST -->
            <template v-else-if="ch.type === 1">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="请求体" required>
                <el-input v-model="ch.customBody" type="textarea" :rows="3" placeholder='例：{"from":"{sender}","text":"{message}","time":"{timestamp}"}' />
              </el-form-item>
            </template>
            <!-- ch.type=2/4: GET / Bark -->
            <template v-else-if="ch.type === 2 || ch.type === 4">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
            </template>
            <!-- ch.type=5/8: 钉钉 / 飞书 -->
            <template v-else-if="ch.type === 5 || ch.type === 8">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="签名">
                <el-input v-model="ch.secret" placeholder="可选" />
              </el-form-item>
            </template>
            <!-- ch.type=6: PushPlus -->
            <template v-else-if="ch.type === 6">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="Token">
                <el-input v-model="ch.token" placeholder="PushPlus Token" />
              </el-form-item>
              <el-form-item label="渠道">
                <el-input v-model="ch.channel" placeholder="wechat / extension / app" />
              </el-form-item>
            </template>
            <!-- ch.type=7: Server酱 -->
            <template v-else-if="ch.type === 7">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="SendKey">
                <el-input v-model="ch.token" placeholder="SCT..." />
              </el-form-item>
            </template>
            <!-- ch.type=9: Gotify -->
            <template v-else-if="ch.type === 9">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="Token">
                <el-input v-model="ch.token" placeholder="Gotify Token" />
              </el-form-item>
            </template>
            <!-- ch.type=10: Telegram Bot -->
            <template v-else-if="ch.type === 10">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="BotToken">
                <el-input v-model="ch.token" placeholder="12345678:ABC..." />
              </el-form-item>
              <el-form-item label="Chat ID">
                <el-input v-model="ch.user" placeholder="123456789" />
              </el-form-item>
            </template>
            <!-- ch.type=11: Pushover -->
            <template v-else-if="ch.type === 11">
              <el-form-item label="URL" required>
                <el-input v-model="ch.url" placeholder="Webhook或推送地址URL" />
              </el-form-item>
              <el-form-item label="AppToken">
                <el-input v-model="ch.token" placeholder="应用 Token" />
              </el-form-item>
              <el-form-item label="UserKey">
                <el-input v-model="ch.user" placeholder="用户 Key" />
              </el-form-item>
            </template>
          </el-form>
          <div class="form-hint form-hint-common">占位符：{sender} 发送方 · {message} 内容 · {timestamp} 时间</div>
          <div class="collapse-card-actions">
            <el-button type="primary" size="small" :loading="saving.push" @click="savePush">保存</el-button>
            <el-button size="small" type="primary" plain :loading="ch.testing" @click="testChannel(index)">测试</el-button>
            <el-button size="small" type="danger" plain @click="removeChannel(index)">删除</el-button>
          </div>
        </div>
      </div>
      <el-button type="default" plain class="btn-add-block" :disabled="pushChannels.length >= 5" @click="addChannel">+ 新增通道（最多5个）</el-button>
    </el-card>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { getWifiConfig, saveWifiConfig as apiSaveWifi, getAuthConfig, saveAuthConfig as apiSaveAuth, getPushConfig, savePushConfig as apiSavePush, postPushTest } from '../api'

const wifiFormRef = ref(null)
const wifiForm = reactive({
  ssid: '',
  password: '', // 仅用于用户输入的新密码，不回显真实密码
})
// 从接口拉取后保存的真实密码，仅用于“未改密码时”提交，不参与展示
const savedWifiPassword = ref('')
const initialWifiSsid = ref('')

const wifiHasSavedPassword = computed(() => (savedWifiPassword.value || '').length > 0)
const wifiFormDirty = computed(
  () => wifiForm.ssid !== initialWifiSsid.value || (wifiForm.password || '').length > 0
)

const authFormRef = ref(null)
const authForm = reactive({
  user: '',
  password: '',
})
const savedAuthPassword = ref('')
const initialAuthUser = ref('')
const authHasSavedPassword = computed(() => (savedAuthPassword.value || '').length > 0)
const authFormDirty = computed(
  () => authForm.user !== initialAuthUser.value || (authForm.password || '').length > 0
)

const pushChannels = ref([])
const saving = reactive({ wifi: false, auth: false, push: false })

const pushTypeOptions = [
  { value: 1, label: 'POST' },
  { value: 2, label: 'GET' },
  { value: 3, label: '邮件(SMTP)' },
  { value: 4, label: 'Bark（iOS）' },
  { value: 5, label: '钉钉机器人' },
  { value: 6, label: 'PushPlus' },
  { value: 7, label: 'Server酱' },
  { value: 8, label: '飞书机器人' },
  { value: 9, label: 'Gotify' },
  { value: 10, label: 'Telegram Bot' },
  { value: 11, label: 'Pushover' },
]

function pushTypeLabel(type) {
  const o = pushTypeOptions.find((x) => x.value === type)
  return o ? o.label : '未知'
}

function newChannel(index) {
  return {
    enabled: true,
    type: 1,
    name: '通道' + (index + 1),
    url: '',
    token: '',
    user: '',
    secret: '',
    password: '',
    customBody: '',
    port: '465',
    channel: '',
    expand: true,
    testing: false,
    testResult: null,
  }
}

function addChannel() {
  if (pushChannels.value.length >= 5) return
  pushChannels.value.push(newChannel(pushChannels.value.length))
}

function removeChannel(index) {
  pushChannels.value.splice(index, 1)
}

async function testChannel(index) {
  const ch = pushChannels.value[index]
  if (!ch) return
  ch.testing = true
  ch.testResult = null
  try {
    const res = await postPushTest({ index })
    const ok = res?.success === true
    const msg = (res?.message || (ok ? '测试成功' : '测试失败')).replace(/\s*\[Mock\]\s*$/, '')
    ch.testResult = { ok, msg }
  } catch (e) {
    ch.testResult = { ok: false, msg: e?.message || '请求失败' }
  } finally {
    ch.testing = false
  }
}

async function loadWifi() {
  try {
    const c = await getWifiConfig()
    if (c) {
      const ssid = (c.ssid ?? c.wifiSsid ?? '').trim()
      const pass = c.password ?? c.wifiPass ?? ''
      wifiForm.ssid = ssid
      initialWifiSsid.value = ssid
      savedWifiPassword.value = pass
      wifiForm.password = '' // 不回显真实密码，仅在有修改时保存
    }
  } catch (_) {}
}

async function saveWifi() {
  if (!wifiFormDirty.value) return
  saving.wifi = true
  try {
    const payload = {
      ssid: (wifiForm.ssid || '').trim(),
      password: (wifiForm.password || savedWifiPassword.value) || '',
    }
    await apiSaveWifi(payload)
    ElMessage.success('Wi-Fi 配置已保存')
    initialWifiSsid.value = payload.ssid
    savedWifiPassword.value = payload.password
    wifiForm.password = ''
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    saving.wifi = false
  }
}

async function loadAuth() {
  try {
    const c = await getAuthConfig()
    if (c) {
      const user = (c.user ?? '').trim()
      authForm.user = user
      initialAuthUser.value = user
      savedAuthPassword.value = c.hasPassword ? '***' : ''
      authForm.password = ''
    }
  } catch (_) {}
}

async function saveAuth() {
  if (!authFormDirty.value) return
  saving.auth = true
  try {
    const user = (authForm.user || '').trim()
    const newPass = (authForm.password || '').trim()
    // 密码留空表示不修改；后端不返回真实密码，故只能传用户输入的新密码或空
    await apiSaveAuth({ user, password: newPass })
    ElMessage.success('账户密码已保存')
    initialAuthUser.value = user
    savedAuthPassword.value = newPass || savedAuthPassword.value ? '***' : ''
    authForm.password = ''
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    saving.auth = false
  }
}

async function loadPush() {
  try {
    const data = await getPushConfig()
    const ch = data?.channels ?? []
    pushChannels.value = (ch.length ? ch : []).map((c, i) => ({
      enabled: !!c.enabled,
      type: c.type ?? 1,
      name: c.name ?? '通道' + (i + 1),
      url: c.url ?? '',
      token: c.token ?? '',
      user: c.user ?? '',
      secret: c.secret ?? '',
      password: c.password ?? '',
      customBody: c.customBody ?? '',
      port: c.port ?? '465',
      channel: c.channel ?? '',
      expand: false,
      testing: false,
      testResult: null,
    }))
    if (pushChannels.value.length === 0) pushChannels.value = []
  } catch (_) {
    pushChannels.value = []
  }
}

async function savePush() {
  saving.push = true
  try {
    const payload = pushChannels.value.map((c) => ({
      enabled: c.enabled,
      type: c.type,
      name: c.name,
      url: c.url,
      token: c.token,
      user: c.user,
      secret: c.secret,
      password: c.password,
      customBody: c.customBody,
      port: c.port,
      channel: c.channel,
    }))
    await apiSavePush({ channels: payload })
    ElMessage.success('推送配置已保存')
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    saving.push = false
  }
}

onMounted(() => {
  loadWifi()
  loadAuth()
  loadPush()
})
</script>

<style scoped>
.config-view { padding: 8px 0; width: 100%; min-width: 0; }
.wifi-form, .auth-form, .channel-form { width: 100%; }
.channel-form :deep(.el-form-item__label) { white-space: nowrap; }
.form-hint { margin-left: 8px; font-size: 12px; color: var(--el-text-color-secondary); }
.form-hint-common { margin-top: 4px; margin-bottom: 8px; }
.auth-card { margin-top: 16px; }
.auth-desc { font-size: 12px; color: var(--el-text-color-secondary); margin: 0 0 12px 0; text-align: left; }
.push-card { margin-top: 16px; }
.push-desc { font-size: 12px; color: var(--el-text-color-secondary); margin-bottom: 12px; text-align: left; }
.collapse-card + .collapse-card { margin-top: 10px; }
@media (max-width: 480px) {
  .channel-form :deep(.el-form-item__label) { width: 74px !important; }
}
</style>
