import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'
import AutoImport from 'unplugin-auto-import/vite'
import Components from 'unplugin-vue-components/vite'
import { ElementPlusResolver } from 'unplugin-vue-components/resolvers'
import { viteSingleFile } from 'vite-plugin-singlefile'
import { visualizer } from 'rollup-plugin-visualizer'
import { createMockApiMiddleware } from './mock/api.js'

export default defineConfig({
  resolve: {
    // 保证 lodash-es / lodash-unified 只解析一份，便于 tree-shaking
    dedupe: ['lodash-es', 'lodash-unified'],
  },
  optimizeDeps: {
    // 不预打包图标与 lodash，交给构建阶段按需 tree-shake
    exclude: ['@element-plus/icons-vue', 'lodash-es', 'lodash-unified'],
  },
  plugins: [
    vue(),
    AutoImport({
      resolvers: [ElementPlusResolver()],
      dts: false,
    }),
    Components({
      resolvers: [ElementPlusResolver({ importStyle: 'css' })],
      dts: false,
    }),
    viteSingleFile(),
    visualizer({ filename: 'dist-web/stats.html', gzipSize: true, open: false }),
    // 开发环境mock API
    {
      name: 'mock-api',
      configureServer(server) {
        server.middlewares.use((req, res, next) => {
          if (!req.url?.startsWith('/api')) return next()
          createMockApiMiddleware()(req, res, next).catch(next)
        })
      },
    }
  ],
  base: './',
  build: {
    outDir: 'dist-web',
    minify: 'esbuild',
    reportCompressedSize: true,
    rollupOptions: {
      output: {
        manualChunks: undefined,
      },
    },
  },
})