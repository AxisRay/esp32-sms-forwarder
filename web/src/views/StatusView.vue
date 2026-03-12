<template>
  <div class="status-view">
    <el-card shadow="never" class="app-card status-card">
      <template #header><span class="card-title">Wi-Fi</span></template>
      <template v-if="loading.network">
        <el-skeleton :rows="3" animated />
      </template>
      <template v-else-if="network?.wifi">
        <div class="status-block">
          <div class="status-badge" :class="network.wifi.connected ? 'on' : 'off'">
            <span class="dot" />
            <span class="txt">{{ network.wifi.connected ? '已连接' : '未连接' }}</span>
          </div>
          <div class="signal-row">
            <span class="info-label">信号</span>
            <div class="signal-bars" :class="'level-' + wifiLevel">
              <span v-for="n in 5" :key="n" class="bar" />
            </div>
            <span class="signal-value">{{ wifiSignalText }}</span>
          </div>
          <div class="info-row"><span class="info-label">SSID</span><span class="info-value">{{ network.wifi.ssid || '-' }}</span></div>
          <div class="info-row"><span class="info-label">IP 地址</span><span class="info-value">{{ network.wifi.ip || '-' }}</span></div>
        </div>
      </template>
      <template v-else><span class="muted">暂无数据</span></template>
    </el-card>

    <el-card shadow="never" class="app-card status-card">
      <template #header><span class="card-title">4G 模组</span></template>
      <template v-if="loading.network">
        <el-skeleton :rows="3" animated />
      </template>
      <template v-else-if="network?.signal || network?.network">
        <div class="status-block">
          <div class="status-badge" :class="network.network?.registered ? 'on' : 'off'">
            <span class="dot" />
            <span class="txt">{{ network.network?.registered ? '已注册' : '未注册' }}</span>
          </div>
          <div class="signal-row">
            <span class="info-label">信号</span>
            <div class="signal-bars" :class="'level-' + cellularLevel">
              <span v-for="n in 5" :key="n" class="bar" />
            </div>
            <span class="signal-value">{{ cellularSignalText }}</span>
          </div>
          <div class="info-row"><span class="info-label">注册</span><span class="info-value">{{ network.network?.reg_status ?? '-' }}</span></div>
          <div class="info-row"><span class="info-label">运营商</span><span class="info-value">{{ network.network?.operator ?? '-' }}</span></div>
        </div>
      </template>
      <template v-else><span class="muted">暂无数据</span></template>
    </el-card>

    <el-card shadow="never" class="app-card sms-card">
      <template #header>
        <span class="card-title">短信历史</span>
        <span class="total-inline">共 {{ smsTotal }} 条</span>
      </template>
      <template v-if="loading.sms">
        <el-skeleton :rows="3" animated />
      </template>
      <template v-else>
        <el-table :data="smsList" stripe :row-key="(row, i) => row.id ?? i" size="small" class="sms-table">
          <el-table-column type="expand" width="36">
            <template #default="{ row }">
              <div class="expand-content">{{ row.content || '-' }}</div>
            </template>
          </el-table-column>
          <el-table-column prop="ts" label="时间" width="75" align="left">
            <template #default="{ row }">{{ formatTs(row.ts) }}</template>
          </el-table-column>
          <el-table-column prop="sender" label="发件人" width="102" align="left" show-overflow-tooltip />
          <el-table-column prop="content" label="内容摘要" min-width="80" align="left" class-name="col-content-summary">
            <template #default="{ row }">
              <span class="content-preview">{{ row.content || '-' }}</span>
            </template>
          </el-table-column>
        </el-table>
        <div class="pagination-wrap">
          <el-button v-if="smsList.length < smsTotal && !loading.sms" type="primary" link size="small" @click="loadMoreSms">加载更多</el-button>
        </div>
      </template>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import { getNetworkStatus, getSmsHistory } from '../api'

const network = ref(null)
const smsList = ref([])
const smsTotal = ref(0)
const loading = ref({ network: true, sms: true })
let smsOffset = 0
/** 短信历史定时刷新间隔（毫秒） */
const SMS_REFRESH_INTERVAL = 30000
let smsRefreshTimer = null
/** 是否有短信列表请求在进行中（未返回前不发起下一次） */
let smsRequestInProgress = false

// Wi-Fi: rssi -> 1-5 (>= -55:5, -65:4, -75:3, -85:2, else 1)
const wifiLevel = computed(() => {
  if (!network.value?.wifi?.connected || network.value.wifi.rssi == null) return 0
  const r = network.value.wifi.rssi
  if (r >= -55) return 5
  if (r >= -65) return 4
  if (r >= -75) return 3
  if (r >= -85) return 2
  return 1
})
const wifiSignalText = computed(() => {
  if (!network.value?.wifi) return '-'
  if (!network.value.wifi.connected) return '未连接'
  const w = network.value.wifi
  const r = w.rssi != null ? `${w.rssi} dBm` : ''
  return [w.status, r].filter(Boolean).join(' ') || '-'
})

// 4G: 使用 API 的 level 1-5，无则用 quality 推断
const cellularLevel = computed(() => {
  const l = network.value?.signal?.level
  if (l != null && l >= 0 && l <= 5) return l
  if (!network.value?.signal?.quality) return 0
  const q = network.value.signal.quality
  if (q === '极好') return 5
  if (q === '良好') return 4
  if (q === '一般') return 3
  if (q === '较弱') return 2
  if (q === '很差') return 1
  return 0
})
const cellularSignalText = computed(() => {
  if (!network.value?.signal) return '-'
  const s = network.value.signal
  const a = [s.rsrp_str, s.quality].filter(Boolean)
  return a.join(' ') || '-'
})

function formatTs(ts) {
  if (!ts) return '-'
  const d = new Date(typeof ts === 'number' ? ts : parseInt(ts, 10))
  if (isNaN(d.getTime())) return String(ts)
  const now = new Date()
  const isToday = d.getFullYear() === now.getFullYear() &&
                  d.getMonth() === now.getMonth() &&
                  d.getDate() === now.getDate()
  if (isToday) {
    return d.toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', hour12: false })
  }
  if (d.getFullYear() === now.getFullYear()) {
    return `${d.getMonth() + 1}/${d.getDate()}`
  }
  return `${String(d.getFullYear()).slice(2)}/${d.getMonth() + 1}/${d.getDate()}`
}
async function fetchNetwork() {
  loading.value.network = true
  try {
    network.value = await getNetworkStatus()
  } catch (e) {
    network.value = null
  } finally {
    loading.value.network = false
  }
}
/**
 * @param {boolean} append - 是否追加（加载更多）；否则从第一页刷新
 * @param {boolean} silent - 为 true 时不置 loading.sms，表格不闪骨架，用于定时刷新
 */
async function fetchSms(append = false, silent = false) {
  if (smsRequestInProgress) return
  smsRequestInProgress = true
  if (!append) smsOffset = 0
  if (!silent) loading.value.sms = true
  try {
    const { list, total } = await getSmsHistory({ offset: smsOffset, limit: 20 })
    if (append) smsList.value = smsList.value.concat(list)
    else smsList.value = list
    smsTotal.value = total
    smsOffset += list.length
  } catch (e) {
    if (!append) smsList.value = []
    smsTotal.value = 0
  } finally {
    if (!silent) loading.value.sms = false
    smsRequestInProgress = false
  }
}
function loadMoreSms() { fetchSms(true, false) }

onMounted(() => {
  fetchNetwork()
  fetchSms(false, false)
  smsRefreshTimer = setInterval(() => fetchSms(false, true), SMS_REFRESH_INTERVAL)
})

onUnmounted(() => {
  if (smsRefreshTimer) {
    clearInterval(smsRefreshTimer)
    smsRefreshTimer = null
  }
})

/** 供父组件在 tab 切回时调用，静默刷新短信列表 */
function refreshSms() {
  fetchSms(false, true)
}
defineExpose({ refreshSms })
</script>

<style scoped>
/* 统一卡片间距：纵向 12px，避免不均匀 */
/* 与其它 Tab 统一：首卡距 tab 栏 24px（tab 内容区 16px + 本容器 8px） */
.status-view {
  display: flex;
  flex-direction: column;
  gap: 12px;
  padding: 8px 0;
  width: 100%;
  min-width: 0;
}
.status-card { min-height: 160px; flex: 0 0 auto; }
.status-card :deep(.el-card__body) { min-height: 124px; }
.status-block { text-align: left; font-size: 13px; }
.status-badge { display: inline-flex; align-items: center; gap: 6px; margin-bottom: 8px; }
.status-badge .dot { width: 8px; height: 8px; border-radius: 50%; background: var(--el-text-color-placeholder); }
.status-badge.on .dot { background: var(--el-color-success); }
.status-badge.off .dot { background: var(--el-text-color-placeholder); }
.status-badge .txt { font-weight: 500; }
.signal-row { display: flex; align-items: center; gap: 8px; line-height: 1.6; min-height: 22px; margin-bottom: 2px; }
.signal-bars { display: inline-flex; align-items: flex-end; gap: 2px; height: 14px; }
.signal-bars .bar { width: 4px; border-radius: 1px; background: var(--el-border-color-darker); min-height: 4px; }
.signal-bars.level-1 .bar:nth-child(1) { height: 4px; background: var(--el-color-danger); }
.signal-bars.level-2 .bar:nth-child(1), .signal-bars.level-2 .bar:nth-child(2) { height: 6px; background: var(--el-color-warning); }
.signal-bars.level-3 .bar:nth-child(1), .signal-bars.level-3 .bar:nth-child(2), .signal-bars.level-3 .bar:nth-child(3) { height: 8px; background: var(--el-color-primary); }
.signal-bars.level-4 .bar:nth-child(1), .signal-bars.level-4 .bar:nth-child(2), .signal-bars.level-4 .bar:nth-child(3), .signal-bars.level-4 .bar:nth-child(4) { height: 10px; background: var(--el-color-primary); }
.signal-bars.level-5 .bar { height: 12px; background: var(--el-color-success); }
.signal-value { font-size: 12px; color: var(--el-text-color-regular); }
.info-row { display: flex; gap: 8px; line-height: 1.6; min-height: 22px; }
.info-label { flex: 0 0 72px; color: var(--el-text-color-secondary); }
.info-value { flex: 1; min-width: 0; word-break: break-all; text-align: left; }
.sms-table :deep(.el-table__body .cell),
.sms-table :deep(.el-table th.el-table__cell > .cell) { text-align: left; }
/* 展开列：尽量窄，去黑框 */
.sms-table :deep(.el-table__expand-icon-cell) { padding: 0 4px !important; }
.sms-table :deep(.el-table__expand-icon-cell .cell) { padding: 0 4px !important; }
.sms-table :deep(.el-table__expand-icon-cell),
.sms-table :deep(.el-table__expand-icon-cell *),
.sms-table :deep(.el-table__expand-icon),
.sms-table :deep(.el-table__expand-icon:focus),
.sms-table :deep(.el-table__expand-icon:focus-visible) {
  outline: none !important;
  box-shadow: none !important;
}
.total-inline { float: right; font-weight: normal; color: var(--el-text-color-secondary); font-size: 12px; }
.muted { color: var(--el-text-color-secondary); font-size: 13px; text-align: left; }
.sms-card { margin: 0; min-width: 0; overflow: hidden; }
.sms-card :deep(.el-card__body) { overflow-x: hidden; min-width: 0; }
/* 表格不撑出容器，避免横向滚动条 */
/* 同时覆盖 EP JS 动态注入的 style.width，必须用 !important 作用于实际 <table> 元素 */
.sms-table { overflow: hidden; }
.sms-table :deep(.el-table) { width: 100% !important; min-width: 0 !important; table-layout: fixed; overflow: hidden; }
.sms-table :deep(.el-table table) { width: 100% !important; max-width: 100% !important; table-layout: fixed !important; }
.sms-table :deep(.el-table__inner-wrapper) { width: 100% !important; max-width: 100% !important; overflow: hidden !important; }
.sms-table :deep(.el-table__body-wrapper),
.sms-table :deep(.el-table__header-wrapper) { overflow-x: hidden !important; }
.sms-table :deep(.el-table__cell) { min-width: 0; }
.sms-table :deep(.el-table__cell .cell) { padding-left: 8px !important; padding-right: 8px !important; }
.content-preview { font-size: 13px; display: block; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.sms-table :deep(.col-content-summary .cell) { overflow: hidden; min-width: 0; }
/* 手机端：缩小卡片内边距以腾出更多空间给表格 */
@media (max-width: 640px) {
  .sms-card :deep(.el-card__body) { padding: 15px !important; }
}
.expand-content { padding: 8px 12px; font-size: 13px; line-height: 1.5; white-space: pre-wrap; word-break: break-all; background: var(--el-fill-color-light); border-radius: 4px; margin: 0 0 0 36px; text-align: left; }
.pagination-wrap { margin-top: 8px; }
</style>
