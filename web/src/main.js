import { createApp } from 'vue'
import App from './App.vue'
import { ElMessage } from 'element-plus'
import 'element-plus/es/components/message/style/css'

// 统一 ElMessage 默认 offset/zIndex，避免被顶栏遮挡
const messageDefaults = { offset: 56, zIndex: 9999 }
const mergeOpts = (arg) => (typeof arg === 'string' ? { message: arg, ...messageDefaults } : { ...messageDefaults, ...arg })
;['success', 'warning', 'error', 'info'].forEach((type) => {
  const orig = ElMessage[type]
  ElMessage[type] = (arg) => orig(mergeOpts(arg))
})

const app = createApp(App)
app.mount('#app')