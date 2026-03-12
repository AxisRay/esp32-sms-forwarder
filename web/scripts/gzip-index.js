/**
 * 将 dist-web/index.html 压缩为 index.html.gz，供 ESP32 web_server 嵌入
 */
import { createGzip } from 'zlib'
import { createReadStream, createWriteStream } from 'fs'
import { pipeline } from 'stream/promises'
import { join } from 'path'
import { fileURLToPath } from 'url'

const __dirname = fileURLToPath(new URL('.', import.meta.url))
const outDir = join(__dirname, '..', 'dist-web')
const src = join(outDir, 'index.html')
const dest = join(outDir, 'index.html.gz')

await pipeline(
  createReadStream(src),
  createGzip(),
  createWriteStream(dest)
)
console.log('已生成', dest)
