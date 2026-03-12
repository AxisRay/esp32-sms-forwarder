<template>
  <div class="schedule-view">
    <el-card header="定时任务" shadow="never" class="app-card schedule-card">
      <p class="schedule-desc">最多 3 个定时任务，支持按日/按周/按月执行；动作为发送短信或 PING，执行结果会推送到已配置的推送通道。</p>
      <div v-for="(item, index) in displayedTasks" :key="item.slotIndex" class="collapse-card">
        <div class="collapse-card-header" @click="item.task.expand = !item.task.expand">
          <span class="expand-icon">{{ item.task.expand ? '▼' : '▶' }}</span>
          <span class="collapse-card-title">任务 {{ item.slotIndex + 1 }}</span>
          <span class="collapse-card-tag">{{ actionLabel(item.task.action) }}</span>
          <span class="collapse-card-tag">{{ kindLabel(item.task.kind) }}</span>
          <el-tag v-if="item.task.lastRunTs" :type="item.task.lastOk ? 'success' : 'danger'" size="small">
            {{ item.task.lastOk ? '成功' : '失败' }}
          </el-tag>
        </div>
        <div v-if="item.task.lastMsg" class="result-message" :class="item.task.lastOk ? 'success' : 'error'">
          上次执行: {{ formatTime(item.task.lastRunTs) }} — {{ item.task.lastMsg }}
        </div>
        <div v-show="item.task.expand" class="collapse-card-body">
          <el-form label-width="100px" size="small" label-position="left" class="schedule-form">
            <el-form-item label="启用">
              <el-switch v-model="item.task.enabled" />
            </el-form-item>
            <el-form-item label="动作">
              <el-select v-model="item.task.action" placeholder="选择" style="width: 100%">
                <el-option label="发送短信" :value="0" />
                <el-option label="PING" :value="1" />
              </el-select>
            </el-form-item>
            <el-form-item label="周期">
              <el-select v-model="item.task.kind" placeholder="选择" style="width: 100%">
                <el-option label="每天" :value="0" />
                <el-option label="每周" :value="1" />
                <el-option label="每月" :value="2" />
              </el-select>
            </el-form-item>
            <el-form-item v-if="item.task.kind !== 2" label="执行时间">
              <el-time-picker
                v-model="item.task.timeValue"
                format="HH:mm"
                value-format="HH:mm"
                :clearable="false"
                style="width: 120px"
              />
            </el-form-item>
            <template v-if="item.task.kind === 1">
              <el-form-item label="星期">
                <el-checkbox-group v-model="item.task.weekdayList">
                  <el-checkbox v-for="w in weekdayOpts" :key="w.value" :label="w.value">{{ w.label }}</el-checkbox>
                </el-checkbox-group>
              </el-form-item>
            </template>
            <template v-if="item.task.kind === 2">
              <el-form-item label="初始日期">
                <el-date-picker
                  v-model="item.task.initialTsMs"
                  type="datetime"
                  value-format="x"
                  format="YYYY-MM-DD HH:mm"
                  placeholder="选择日期与时间"
                  style="width: 200px"
                  :clearable="false"
                />
              </el-form-item>
              <el-form-item label="每 N 个月">
                <el-input-number v-model="item.task.monthInterval" :min="1" :max="12" style="width: 100px" />
              </el-form-item>
            </template>
            <template v-if="item.task.action === 0">
              <el-form-item label="目标号码">
                <el-input v-model="item.task.phone" placeholder="手机号" />
              </el-form-item>
              <el-form-item label="短信内容">
                <el-input v-model="item.task.message" type="textarea" :rows="2" placeholder="内容" />
              </el-form-item>
            </template>
            <template v-else>
              <el-form-item label="PING 目标">
                <el-input v-model="item.task.pingHost" placeholder="如 8.8.8.8" />
              </el-form-item>
            </template>
          </el-form>
          <div class="collapse-card-actions">
            <el-button type="primary" size="small" :loading="saving" @click="saveSchedule">保存</el-button>
            <el-button size="small" type="danger" plain @click="removeTask(item.slotIndex)">删除</el-button>
          </div>
        </div>
      </div>
      <el-button type="default" plain class="btn-add-block" :disabled="configuredCount >= 3" @click="addTask">+ 新增任务（最多 3 个）</el-button>
    </el-card>
  </div>
</template>

<script setup>
import { ref, computed, onMounted } from 'vue'
import { ElMessage } from 'element-plus'
import { getScheduleConfig, saveScheduleConfig } from '../api'

// 内部固定 3 个槽位，仅展示“已配置”的；通过「新增任务」在空槽位添加
const tasksFull = ref([])
const saving = ref(false)

function isTaskConfigured(t) {
  if (!t) return false
  return (t.enabled && (!!(t.phone && t.phone.trim()) || !!(t.pingHost && t.pingHost.trim()))) || (t.lastRunTs > 0)
}

const displayedTasks = computed(() =>
  tasksFull.value
    .map((task, slotIndex) => ({ task, slotIndex }))
    .filter((item) => isTaskConfigured(item.task))
)

const configuredCount = computed(() => displayedTasks.value.length)

function addTask() {
  const list = tasksFull.value
  for (let i = 0; i < 3; i++) {
    if (!isTaskConfigured(list[i])) {
      list[i] = rawToTask({ enabled: true, action: 0, kind: 0 }, i)
      list[i].expand = true
      tasksFull.value = [...list]
      return
    }
  }
}

function removeTask(slotIndex) {
  const list = [...tasksFull.value]
  list[slotIndex] = rawToTask({ enabled: false, action: 0, kind: 0 }, slotIndex)
  tasksFull.value = list
}

const weekdayOpts = [
  { value: 0, label: '日' },
  { value: 1, label: '一' },
  { value: 2, label: '二' },
  { value: 3, label: '三' },
  { value: 4, label: '四' },
  { value: 5, label: '五' },
  { value: 6, label: '六' },
]

function actionLabel(action) {
  return action === 1 ? 'PING' : '短信'
}

function kindLabel(kind) {
  const m = { 0: '每天', 1: '每周', 2: '每月' }
  return m[kind] ?? '未知'
}

function formatTime(ts) {
  if (!ts || ts <= 0) return '未执行'
  const d = new Date(ts * 1000)
  return d.toLocaleString('zh-CN', { hour12: false })
}

function weekdayMaskToList(mask) {
  const list = []
  for (let i = 0; i <= 6; i++) {
    if (mask & (1 << i)) list.push(i)
  }
  return list
}

function listToWeekdayMask(list) {
  let mask = 0
  for (const i of list) mask |= 1 << (i & 7)
  return mask
}

function timeValueToHM(str) {
  if (!str || typeof str !== 'string') return { h: 0, m: 0 }
  const [h, m] = str.split(':').map(Number)
  return { h: isNaN(h) ? 0 : h % 24, m: isNaN(m) ? 0 : m % 60 }
}

function rawToTask(raw, index) {
  const hour = raw.hour ?? 0
  const minute = raw.minute ?? 0
  const timeStr = `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`
  return {
    enabled: !!raw.enabled,
    action: raw.action ?? 0,
    kind: raw.kind ?? 0,
    hour,
    minute,
    timeValue: timeStr,
    weekdayMask: raw.weekdayMask ?? 0,
    weekdayList: weekdayMaskToList(raw.weekdayMask ?? 0),
    monthInterval: Math.min(12, Math.max(1, raw.monthInterval ?? 1)),
    initialTsMs: Math.max(0, (raw.initialTs ?? 0) * 1000) || new Date(new Date().getFullYear(), new Date().getMonth(), 1, 0, 0).getTime(),
    phone: raw.phone ?? '',
    message: raw.message ?? '',
    pingHost: raw.pingHost ?? '8.8.8.8',
    lastRunTs: raw.lastRunTs ?? 0,
    lastOk: !!raw.lastOk,
    lastMsg: raw.lastMsg ?? '',
    expand: index === 0,
  }
}

function taskToRaw(t) {
  const { h, m } = timeValueToHM(t.timeValue)
  return {
    enabled: t.enabled,
    action: t.action,
    kind: t.kind,
    hour: h,
    minute: m,
    weekdayMask: listToWeekdayMask(t.weekdayList || []),
    monthInterval: t.monthInterval ?? 1,
    initialTs: Math.floor(Number(t.initialTsMs ?? 0) / 1000),
    phone: t.phone ?? '',
    message: t.message ?? '',
    pingHost: t.pingHost ?? '8.8.8.8',
  }
}

async function loadSchedule() {
  try {
    const data = await getScheduleConfig()
    const arr = data?.tasks ?? []
    const list = arr.slice(0, 3).map((t, i) => rawToTask(t, i))
    while (list.length < 3) list.push(rawToTask({ enabled: false, action: 0, kind: 0 }, list.length))
    tasksFull.value = list
  } catch (_) {
    tasksFull.value = [rawToTask({}, 0), rawToTask({}, 1), rawToTask({}, 2)]
  }
}

async function saveSchedule() {
  saving.value = true
  try {
    const payload = tasksFull.value.map(taskToRaw)
    await saveScheduleConfig({ tasks: payload })
    ElMessage.success('定时任务已保存')
    await loadSchedule()
  } catch (e) {
    ElMessage.error(e?.message || '保存失败')
  } finally {
    saving.value = false
  }
}

onMounted(() => {
  loadSchedule()
})
</script>

<style scoped>
/* 与其它 Tab 统一：首卡距 tab 栏 24px，仅靠 padding 不额外 margin */
.schedule-view { padding: 8px 0; width: 100%; min-width: 0; }
.schedule-desc { font-size: 12px; color: var(--el-text-color-secondary); margin-bottom: 12px; text-align: left; }
.schedule-form { width: 100%; }
.collapse-card + .collapse-card { margin-top: 10px; }
</style>
