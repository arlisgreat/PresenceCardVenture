import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'

export default defineConfig({
  plugins: [react()],
  server: {
    // 本地开发：/v1 代理到 API（docs/03 契约）
    proxy: { '/v1': 'http://localhost:3000' },
  },
})
