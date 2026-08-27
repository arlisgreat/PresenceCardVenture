// 占位壳：UI 高保真稿（W2，见 docs/04 §6）落地后替换。
// 页面清单见 docs/00 §5：时间线 / 绑定设备 / 加好友 / 我的主页
export default function App() {
  return (
    <main style={{ fontFamily: 'system-ui', maxWidth: 480, margin: '48px auto', padding: 16 }}>
      <h1>小卡社区</h1>
      <p>Presence Card · 社区 Web 占位页</p>
      <ul>
        <li>时间线（贴贴风，仅好友可见）</li>
        <li>绑定我的小卡（输入 6 位配对码）</li>
        <li>加好友（输入对方 friend_code）</li>
      </ul>
      <p style={{ color: '#888' }}>
        API: <code>/v1</code> → 契约见 docs/03-device-api.openapi.yaml
      </p>
    </main>
  )
}
