<script setup>
import { ref, watch, onMounted } from 'vue'
import zhCn from 'element-plus/dist/locale/zh-cn.mjs'
import StatusView from './views/StatusView.vue'
import ConfigView from './views/ConfigView.vue'
import ScheduleView from './views/ScheduleView.vue'
import LogView from './views/LogView.vue'
import { getVersion } from './api'

const activeTab = ref('status')
const statusRef = ref(null)
const fwVersion = ref('')

onMounted(() => {
  getVersion().then((v) => { fwVersion.value = v }).catch(() => {})
})

watch(activeTab, (tab) => {
  if (tab === 'status') statusRef.value?.refreshSms()
})
</script>

<template>
  <el-config-provider :locale="zhCn">
  <div class="app">
    <header class="app-header">
      <h1>ESP32短信转发器</h1>
      <span v-if="fwVersion" class="app-version">{{ fwVersion }}</span>
    </header>
    <main class="app-main">
      <el-tabs v-model="activeTab" class="main-tabs">
        <el-tab-pane label="状态" name="status">
          <StatusView ref="statusRef" />
        </el-tab-pane>
        <el-tab-pane label="任务" name="schedule">
          <ScheduleView />
        </el-tab-pane>
        <el-tab-pane label="配置" name="config">
          <ConfigView />
        </el-tab-pane>
        <el-tab-pane label="工具" name="log">
          <LogView />
        </el-tab-pane>
      </el-tabs>
    </main>
  </div>
  </el-config-provider>
</template>

<style>
* { box-sizing: border-box; }

:root {
  --app-bg: #f5f7fa;
  --app-text: #303133;
  --app-card-bg: #ffffff;
  --app-card-border: #e4e7ed;
  --app-card-shadow: 0 2px 12px rgba(0, 0, 0, 0.05);
  --app-card-gap: 16px;
  --app-card-radius: 12px;
  --app-card-padding: 15px;
  --app-card-header-padding: 16px 20px;
  --app-color-primary: #409eff;
  --app-color-success: #67c23a;
  --app-color-warning: #e6a23c;
  --app-color-danger: #f56c6c;
}

html, body {
  height: 100%;
  overflow: hidden;
  margin: 0;
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
  font-size: 14px;
}

/* 全局隐藏滚动条（保留滚动能力），手机与 PC 均不显示 */
.app,
.app * {
  scrollbar-width: none;
}
.app::-webkit-scrollbar,
.app *::-webkit-scrollbar {
  display: none;
}

#app {
  height: 100%;
  display: flex;
  flex-direction: column;
  background: var(--app-bg, #f5f7fa);
  color: var(--app-text, #303133);
}

/* 深色模式：提升对比度与可读性 */
@media (prefers-color-scheme: dark) {
  :root {
    --app-bg: #0f1114;
    --app-text: #e8eaed;
    --app-card-bg: #1a1d21;
    --app-card-border: #2c3036;
    --app-card-shadow: 0 2px 16px rgba(0, 0, 0, 0.4);
    /* Element Plus 深色变量：统一背景/文字/边框，避免组件发灰难读 */
    --el-bg-color: #1a1d21;
    --el-bg-color-page: #0f1114;
    --el-bg-color-overlay: #25282e;
    --el-text-color-primary: #e8eaed;
    --el-text-color-regular: #d0d4d9;
    --el-text-color-secondary: #9ca3af;
    --el-text-color-placeholder: #6b7280;
    --el-border-color: #2c3036;
    --el-border-color-light: #373b42;
    --el-border-color-lighter: #3d424a;
    --el-border-color-extra-light: #454a52;
    --el-fill-color: #25282e;
    --el-fill-color-light: #2c3036;
    --el-fill-color-lighter: #32363c;
    --el-fill-color-extra-light: #3d424a;
    --el-color-primary: #5b9cf4;
    --el-color-success: #6bcf7a;
    --el-color-warning: #e5b84d;
    --el-color-danger: #e87878;
  }

  .el-card {
    --el-card-bg-color: #1a1d21;
    --el-card-border-color: #2c3036;
  }

  .el-tabs__item { color: var(--el-text-color-regular); }
  .el-tabs__item.is-active { color: var(--el-color-primary); font-weight: 600; }
  .el-tabs__nav-wrap::after { background-color: var(--el-border-color); }
  .el-tabs__active-bar { background-color: var(--el-color-primary); }
  .el-tabs__indicator { background-color: var(--el-color-primary); }
  .el-tabs__item:hover { color: var(--el-color-primary); }

  .el-input__wrapper,
  .el-select .el-select__wrapper,
  .el-textarea__inner {
    background-color: #25282e !important;
    border: 1px solid #3d424a;
    color: #e8eaed;
    box-shadow: 0 0 0 1px #3d424a;
  }
  .el-input__wrapper:hover,
  .el-select .el-select__wrapper:hover { border-color: #5b9cf4; box-shadow: 0 0 0 1px #5b9cf4; }
  .el-input__inner::placeholder,
  .el-textarea__inner::placeholder { color: #6b7280; }

  .el-table {
    --el-table-bg-color: #1a1d21;
    --el-table-tr-bg-color: #1a1d21;
    --el-table-row-hover-bg-color: #25282e;
    --el-table-tr-hover-bg-color: #25282e;
    --el-table-border-color: #2c3036;
    --el-table-header-bg-color: #25282e;
  }
  .el-table th.el-table__cell { color: var(--el-text-color-secondary); font-weight: 600; }
  .el-table td.el-table__cell { color: var(--el-text-color-regular); }
  .el-table--striped .el-table__body tr.el-table__row--striped td { background: #1f2328; }

  .el-button--primary { --el-button-bg-color: #3b82f6; --el-button-border-color: #3b82f6; --el-button-text-color: #fff; }
  .el-button--primary:hover { --el-button-bg-color: #5b9cf4; --el-button-border-color: #5b9cf4; }
  .el-skeleton__item { background: linear-gradient(90deg, #25282e 25%, #2c3036 50%, #25282e 75%); }

  /* 代码/日志块：深色下用稍亮背景 + 高对比文字 */
  .code-block,
  .log-content,
  .at-output {
    background: #1f2328 !important;
    color: #d4d8dc !important;
    border-color: #2c3036 !important;
  }
  .code-block pre { color: #d4d8dc; }

  /* 成功/错误结果：深色下用半透明底色 + 亮字 */
  .result-success.bg,
  .result-message.success {
    background: rgba(107, 207, 122, 0.18);
    color: #86ef96;
  }
  .result-error.bg,
  .result-message.error {
    background: rgba(232, 120, 120, 0.18);
    color: #fca5a5;
  }

  .text-muted { color: #9ca3af !important; }
  .collapse-card-header { background: #25282e !important; color: #e8eaed; }
  .collapse-card-header:hover { background: #2c3036 !important; }
  .collapse-card-body { background: #1a1d21 !important; }
  .collapse-card-tag { color: #9ca3af !important; }

  /* 状态页：徽章、标签与信息行 */
  .status-badge.on .txt { color: #86ef96; }
  .status-badge.off .txt { color: #9ca3af; }
  .status-row .label,
  .info-label { color: #9ca3af; }
  .status-row .value,
  .info-value { color: #e8eaed; }
  .signal-text,
  .signal-value { color: #d0d4d9; }
  .count,
  .total-inline { color: #9ca3af; }
  .muted { color: #9ca3af; }
  .expand-content,
  .sms-content { color: #d0d4d9; background: #25282e; }
  .content-preview { color: #e8eaed; }
}

/* 浅色为默认，仅 dark 媒体查询覆盖变量 */

/* 全局卡片样式（所有 Tab 页面统一） */
.app-card {
  background: var(--app-card-bg);
  border: 1px solid var(--app-card-border);
  border-radius: var(--app-card-radius);
  box-shadow: var(--app-card-shadow);
  transition: all 0.2s ease;
  margin: 0;
}
.app-card:hover {
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.08);
}
/* 四个 Tab 内卡片统一样式：圆角、标题与内边距一致 */
.app-card .el-card__header,
.app-main .el-card .el-card__header {
  padding: var(--app-card-header-padding);
  font-size: 15px;
  font-weight: 600;
  color: var(--el-text-color-primary);
  border-bottom: 1px solid var(--el-border-color-lighter);
}
.app-card .el-card__body,
.app-main .el-card .el-card__body {
  padding: var(--app-card-padding);
  min-width: 0;
  overflow-x: hidden;
}
.app-card,
.app-main .el-card {
  border-radius: var(--app-card-radius);
}
.app-main .el-card {
  background: var(--app-card-bg);
  border: 1px solid var(--app-card-border);
  box-shadow: var(--app-card-shadow);
}

/* 卡片标题（各 Tab 内复用，与 header 一致） */
.card-title {
  font-weight: 600;
  font-size: 15px;
}

/* 页面容器（各 Tab 内容区统一） */
.page-container {
  padding: 8px 0;
  display: flex;
  flex-direction: column;
  gap: var(--app-card-gap);
  width: 100%;
  min-height: 100%;
}

/* 可折叠卡片（ConfigView/ScheduleView 共用），圆角与主卡片一致 */
.collapse-card {
  border: 1px solid var(--app-card-border);
  border-radius: var(--app-card-radius);
  overflow: hidden;
}
.collapse-card-header {
  display: flex;
  align-items: center;
  gap: 10px;
  padding: 12px 16px;
  cursor: pointer;
  background: var(--el-fill-color-lighter);
}
.collapse-card-header:hover {
  background: var(--el-fill-color);
}
.collapse-card-header .expand-icon {
  font-size: 14px;
  color: var(--el-text-color-secondary);
  flex-shrink: 0;
}
.collapse-card-title {
  flex: 1;
  text-align: left;
  font-weight: 600;
  font-size: 15px;
}
.collapse-card-tag {
  font-size: 12px;
  color: var(--el-text-color-secondary);
  flex-shrink: 0;
}
.collapse-card-body {
  padding: var(--app-card-padding);
  border-top: 1px solid var(--app-card-border);
  background: var(--app-card-bg);
}
.collapse-card-actions {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  margin-top: 12px;
  padding-top: 12px;
  border-top: 1px solid var(--el-border-color-lighter);
}
.btn-add-block {
  width: 100%;
  margin-top: 10px;
  margin-bottom: 8px;
}
.btn-add-block:not(.is-disabled) {
  background: transparent;
}

/* 成功/错误结果提示（LogView/ConfigView/ScheduleView 共用） */
.result-success,
.result-ok {
  color: var(--el-color-success);
}
.result-success.bg,
.result-message.success {
  background: var(--el-color-success-light-9);
  color: var(--el-color-success-dark-2);
  font-size: 12px;
  padding: 6px 12px;
  margin: 0 12px 0 0;
  border-radius: 4px;
  text-align: left;
}
.result-error,
.result-err {
  color: var(--el-color-danger);
}
.result-error.bg,
.result-message.error {
  background: var(--el-color-danger-light-9);
  color: var(--el-color-danger-dark-2);
  font-size: 12px;
  padding: 6px 12px;
  margin: 0 12px 0 0;
  border-radius: 4px;
  text-align: left;
}

/* 次要文字/描述（小号、灰色、左对齐） */
.text-muted {
  font-size: 12px;
  color: var(--el-text-color-secondary);
  text-align: left;
}

/* 代码/日志展示块（LogView at-output、log-content 共用） */
.code-block {
  padding: 10px 12px;
  background: var(--el-fill-color-dark);
  color: var(--el-text-color-primary);
  border: 1px solid var(--el-border-color-lighter);
  border-radius: 6px;
  overflow: auto;
  font-size: 13px;
  font-family: ui-monospace, monospace;
  text-align: left;
  line-height: 1.5;
}
.code-block pre { margin: 0; white-space: pre-wrap; word-break: break-all; }
</style>

<style scoped>
.app {
  height: 100%;
  display: flex;
  flex-direction: column;
}

.app-header {
  padding: 16px 24px;
  border-bottom: 1px solid var(--app-card-border);
  background: var(--app-card-bg);
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
  flex-wrap: wrap;
}
.app-header h1 {
  margin: 0;
  font-size: 18px;
  font-weight: 600;
  color: var(--app-text);
}
.app-version {
  font-size: 12px;
  color: var(--el-text-color-secondary);
  font-family: ui-monospace, monospace;
}

.app-main {
  flex: 1;
  overflow: hidden;
  padding: 16px 24px;
  max-width: 900px;
  margin: 0 auto;
  width: 100%;
}

.main-tabs {
  height: 100%;
  display: flex;
  flex-direction: column;
}
.main-tabs :deep(.el-tabs__content) {
  flex: 1;
  overflow: auto;
  padding-top: 16px;
}
.main-tabs :deep(.el-tabs__nav) {
  width: 100%;
  display: flex;
}
.main-tabs :deep(.el-tabs__item) {
  flex: 1;
  text-align: center;
}

/* 移动端适配 */
@media (max-width: 640px) {
  .app-header {
    padding: 12px 16px;
  }
  .app-main {
    padding: 12px 16px;
  }
  .main-tabs :deep(.el-tabs__item) {
    padding: 0 8px;
    font-size: 13px;
  }
}
</style>