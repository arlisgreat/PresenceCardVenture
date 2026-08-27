import { useEffect, useMemo, useRef, useState } from 'react'
import { AiJob, FeedItem, Message, PlayType, bindDevice, createAiJob, deletePhoto, getAiJob, getDeviceState, getFeed, getMessages, pokePhoto, reactToPhoto, requestPairCode, sendMessage, uploadPhoto } from './api'
import './styles.css'

type View = 'feed' | 'create' | 'messages' | 'ai' | 'device' | 'footprint' | 'library'

const nav: Array<{ id: View; label: string; icon: string }> = [
  { id: 'feed', label: '圈子', icon: '◌' },
  { id: 'footprint', label: '足迹', icon: '⌂' },
  { id: 'library', label: '玩法库', icon: '▤' },
  { id: 'create', label: '释放', icon: '＋' },
  { id: 'messages', label: '轻信号', icon: '✦' },
  { id: 'ai', label: '合照', icon: '⌁' },
  { id: 'device', label: '设备', icon: '▣' },
]

const filters: Array<{ id: string; name: string; note: string; tone: string; play: PlayType }> = [
  { id: 'none', name: '原色', note: '轻美颜 · 真实向', tone: 'blush', play: 'beauty' },
  { id: 'warm', name: '拍立得', note: '暖白 · 留住当下', tone: 'rose', play: 'ccd' },
  { id: 'film', name: '千禧 CCD', note: '颗粒 · 青绿偏色', tone: 'blue', play: 'ccd' },
  { id: 'vivid', name: '日系过曝', note: '高光 · 低对比', tone: 'sage', play: 'ccd' },
  { id: 'template', name: '窗台手账', note: '素材模板 · 拼贴', tone: 'lilac', play: 'template' },
]

const circles = ['小圈', '傍晚的天空', '胶片味', '宿舍窗台']

function formatTime(value: string) {
  const date = new Date(value)
  const minutes = Math.round((Date.now() - date.getTime()) / 60000)
  if (minutes < 60) return `${Math.max(1, minutes)} 分钟前`
  if (minutes < 1440) return `${Math.round(minutes / 60)} 小时前`
  return `${Math.round(minutes / 1440)} 天前`
}

function App() {
  const [view, setView] = useState<View>('feed')
  const [feed, setFeed] = useState<FeedItem[]>([])
  const [circle, setCircle] = useState('小圈')
  const [toast, setToast] = useState('')
  const [heartBurst, setHeartBurst] = useState<string | null>(null)
  const [deviceState, setDeviceState] = useState({ unseen_count: 3, pending_friend_requests: 1, server_time: '' })

  useEffect(() => { void getFeed().then(setFeed); void getDeviceState().then(setDeviceState) }, [])
  useEffect(() => { if (!toast) return; const timeout = window.setTimeout(() => setToast(''), 2600); return () => window.clearTimeout(timeout) }, [toast])

  const visibleFeed = useMemo(() => circle === '小圈' ? feed : feed.filter(item => item.circle === circle || item.circle === '小圈'), [circle, feed])

  async function onReact(item: FeedItem) {
    const active = !(item.my_reactions ?? []).includes('heart')
    setFeed(current => current.map(entry => entry.id === item.id ? { ...entry, my_reactions: active ? ['heart'] : [], reactions: { ...entry.reactions, heart: Math.max(0, (entry.reactions.heart ?? 0) + (active ? 1 : -1)) } } : entry))
    await reactToPhoto(item.id, active)
  }

  function onPoke(item: FeedItem) {
    void pokePhoto(item.id)
    setToast(`${item.author.display_name} 收到一颗轻信号`)
  }

  function onHeartBurst(item: FeedItem) {
    setHeartBurst(item.id)
    window.setTimeout(() => setHeartBurst(current => current === item.id ? null : current), 700)
    if (!(item.my_reactions ?? []).includes('heart')) void onReact(item)
  }

  async function onDelete(item: FeedItem) {
    await deletePhoto(item.id)
    setFeed(current => current.filter(entry => entry.id !== item.id))
    setToast('照片已从你的足迹中移除')
  }

  function onPublished(item: FeedItem) {
    setFeed(current => [item, ...current])
    setView('feed')
    setToast('已释放到小圈')
  }

  return (
    <div className="app-shell">
      <aside className="side-rail">
        <div className="brand-mark"><span>p</span><small>presence</small></div>
        <div className="rail-rule" />
        <nav className="rail-nav" aria-label="主导航">
          {nav.map(item => <button key={item.id} className={view === item.id ? 'nav-item active' : 'nav-item'} onClick={() => setView(item.id)}><span>{item.icon}</span><b>{item.label}</b></button>)}
        </nav>
        <div className="rail-bottom"><div className="avatar">阿</div><span>ayan</span><small>在线</small></div>
      </aside>

      <main className="main-column">
        <header className="topbar">
          <div><p className="eyebrow">PRESENCE · 01</p><h1>{view === 'feed' ? '朋友的在场' : nav.find(item => item.id === view)?.label}</h1></div>
          <div className="top-actions"><span className="live-dot" /> <span className="top-date">2026.08.28</span><button className="text-button" onClick={() => setView('device')}>设备 {deviceState.unseen_count}</button></div>
        </header>

        {view === 'feed' && <FeedView feed={visibleFeed} circle={circle} onCircle={setCircle} onReact={onReact} onPoke={onPoke} onHeartBurst={onHeartBurst} heartBurst={heartBurst} onDelete={onDelete} onCreate={() => setView('create')} />}
        {view === 'footprint' && <FootprintView feed={feed.filter(item => item.mine)} onDelete={onDelete} />}
        {view === 'library' && <PlayLibraryView onChoose={message => setToast(message)} />}
        {view === 'create' && <CreateView onPublished={onPublished} onCancel={() => setView('feed')} />}
        {view === 'messages' && <MessagesView onToast={setToast} />}
        {view === 'ai' && <AiView feed={feed} onToast={setToast} />}
        {view === 'device' && <DeviceView state={deviceState} feed={feed} onToast={setToast} />}
      </main>
      {toast && <div className="toast" role="status"><span>✦</span>{toast}</div>}
    </div>
  )
}

function FeedView({ feed, circle, onCircle, onReact, onPoke, onHeartBurst, heartBurst, onDelete, onCreate }: { feed: FeedItem[]; circle: string; onCircle: (value: string) => void; onReact: (item: FeedItem) => void; onPoke: (item: FeedItem) => void; onHeartBurst: (item: FeedItem) => void; heartBurst: string | null; onDelete: (item: FeedItem) => void; onCreate: () => void }) {
  return <section className="content-wrap feed-view">
    <div className="feed-intro"><div><p className="section-kicker">CIRCLE / 朋友的小圈</p><p className="intro-copy">没有推送，只有刚好想起你的人。</p></div><button className="primary-button" onClick={onCreate}><span>＋</span>释放一张</button></div>
    <div className="circle-tabs">{circles.map(item => <button key={item} className={item === circle ? 'circle-tab selected' : 'circle-tab'} onClick={() => onCircle(item)}>{item}<span>{item === '小圈' ? '04' : '12'}</span></button>)}</div>
    <div className="feed-grid">{feed.length === 0 ? <div className="empty-state"><span>◌</span><h2>圈子还在等第一张照片</h2><p>释放今天的一个瞬间，朋友会在这里遇见它。</p></div> : feed.map(item => <PhotoCard key={item.id} item={item} burst={heartBurst === item.id} onReact={() => onReact(item)} onPoke={() => onPoke(item)} onHeartBurst={() => onHeartBurst(item)} onDelete={() => onDelete(item)} />)}</div>
    <div className="feed-footer"><span>—</span> 今天的在场，到这里刚刚好 <span>—</span></div>
  </section>
}

function FootprintView({ feed, onDelete }: { feed: FeedItem[]; onDelete: (item: FeedItem) => void }) {
  return <section className="content-wrap footprint-view"><div className="studio-head"><div><p className="section-kicker">MY TRACE / 我的足迹</p><h2>把释放过的日子，收在这里。</h2><p>每一张照片都保留原图与处理后的样子。</p></div><span className="signal-count">{feed.length} 张照片</span></div><div className="trace-list">{feed.length === 0 ? <div className="empty-state"><span>⌂</span><h2>还没有留下足迹</h2><p>从释放一张照片开始，给今天一个位置。</p></div> : feed.map(item => <div className="trace-row" key={item.id}><div className="trace-date"><b>{new Date(item.created_at).getDate()}</b><span>{new Date(item.created_at).toLocaleDateString('zh-CN', { month: 'short' })}</span></div><div className="trace-photo"><img src={item.image_url} alt={item.caption ?? '我的照片'} /></div><div className="trace-copy"><p className="section-kicker">{item.filter_id.toUpperCase()} · {formatTime(item.created_at)}</p><h3>{item.caption ?? '今天也好好在场。'}</h3><p>已保存原图 · 可见于 {item.circle ?? '小圈'}</p></div><button className="delete-link" onClick={() => onDelete(item)}>移除</button></div>)}</div></section>
}

function PlayLibraryView({ onChoose }: { onChoose: (message: string) => void }) {
  const groups: Array<{ type: PlayType; title: string; intro: string; color: string; items: typeof filters }> = [
    { type: 'beauty', title: '轻美颜 · 真实向', intro: '把人拍好看，但克制。', color: 'blush', items: filters.filter(item => item.play === 'beauty') },
    { type: 'ccd', title: 'CCD 滤镜', intro: '颗粒、暗角和一点电子乡愁。', color: 'blue', items: filters.filter(item => item.play === 'ccd') },
    { type: 'template', title: '素材模板', intro: '贴纸、边框和手账感的拼贴。', color: 'sage', items: filters.filter(item => item.play === 'template') },
  ]
  return <section className="content-wrap library-view"><div className="studio-head"><div><p className="section-kicker">PLAY LIBRARY / 玩法库</p><h2>三个房间，三种心情。</h2><p>选中的玩法会作为下一次拍摄和小卡下发的配置。</p></div><span className="consent-badge">云端渲染 · 卡端显示</span></div><div className="library-groups">{groups.map(group => <div className={`library-group ${group.color}`} key={group.type}><div className="library-heading"><span className="play-number">{group.type === 'beauty' ? '01' : group.type === 'ccd' ? '02' : '03'}</span><div><h3>{group.title}</h3><p>{group.intro}</p></div><span className="library-arrow">→</span></div><div className="library-items">{group.items.map(item => <button className="library-item" key={item.id} onClick={() => onChoose(`${item.name} 已标记为小卡下发配置`)}><span className="filter-swatch" /><span><b>{item.name}</b><small>{item.note}</small></span><i>下发 →</i></button>)}</div></div>)}</div></section>
}

function PhotoCard({ item, burst, onReact, onPoke, onHeartBurst, onDelete }: { item: FeedItem; burst: boolean; onReact: () => void; onPoke: () => void; onHeartBurst: () => void; onDelete: () => void }) {
  const liked = (item.my_reactions ?? []).includes('heart')
  return <article className="photo-card">
    <div className="photo-frame" onDoubleClick={onHeartBurst}><img src={item.image_url} alt={item.caption ?? '朋友分享的照片'} /><span className="photo-type">{item.filter_id === 'film' ? 'CCD' : item.filter_id === 'template' ? 'TEMPLATE' : 'PHOTO'}</span>{burst && <span className="heart-burst" aria-hidden="true">✦</span>}</div>
    <div className="photo-meta"><div className="meta-line"><div className="mini-avatar">{item.author.display_name.slice(0, 1)}</div><div><b>{item.author.display_name}</b><span>{formatTime(item.created_at)} · {item.circle ?? '小圈'}</span></div><button className="more-button" aria-label="更多操作">···</button></div><p className="caption">{item.caption ?? '今天也好好在场。'}</p><div className="reaction-line"><button className={liked ? 'reaction active' : 'reaction'} onClick={onReact}><span>✦</span>{item.reactions.heart ?? 0}</button><button className="reaction poke" onClick={onPoke}><span>⌁</span>拍一拍</button>{item.mine && <button className="delete-link" onClick={onDelete}>移除</button>}</div></div>
  </article>
}

function CreateView({ onPublished, onCancel }: { onPublished: (item: FeedItem) => void; onCancel: () => void }) {
  const [file, setFile] = useState<File | null>(null)
  const [preview, setPreview] = useState('')
  const [play, setPlay] = useState<PlayType>('beauty')
  const [filterId, setFilterId] = useState('none')
  const [caption, setCaption] = useState('')
  const [beauty, setBeauty] = useState(42)
  const [sticker, setSticker] = useState('none')
  const [busy, setBusy] = useState(false)
  const [dragActive, setDragActive] = useState(false)
  const inputRef = useRef<HTMLInputElement>(null)

  function selectFile(next: File | undefined) { if (!next) return; setFile(next); setPreview(URL.createObjectURL(next)) }
  async function publish() { if (!file) { inputRef.current?.click(); return }; setBusy(true); const item = await uploadPhoto(file, { filterId, caption, play, beauty, sticker }); setBusy(false); onPublished(item) }

  const playOptions: Array<{ id: PlayType; title: string; copy: string; color: string }> = [
    { id: 'beauty', title: '轻美颜', copy: '把人拍好看，但克制', color: 'blush' },
    { id: 'ccd', title: 'CCD 滤镜', copy: '胶片 · 颗粒 · 电子乡愁', color: 'blue' },
    { id: 'template', title: '素材模板', copy: '贴纸 · 边框 · 手账感', color: 'sage' },
  ]
  return <section className="content-wrap studio-view"><div className="studio-head"><div><p className="section-kicker">CREATE / 释放</p><h2>把今天折成一张小卡。</h2><p>选择一个房间，云端会替你把它修成想要的样子。</p></div><button className="quiet-button" onClick={onCancel}>返回圈子</button></div>
    <div className="play-switcher">{playOptions.map(option => <button key={option.id} className={play === option.id ? `play-choice active ${option.color}` : 'play-choice'} onClick={() => { setPlay(option.id); setFilterId(option.id === 'beauty' ? 'none' : option.id === 'template' ? 'template' : 'film') }}><span className="play-number">0{playOptions.findIndex(item => item.id === option.id) + 1}</span><strong>{option.title}</strong><small>{option.copy}</small></button>)}</div>
    <div className="studio-layout"><div className={`${dragActive ? 'upload-stage dragging' : 'upload-stage'}${play === 'template' ? ' template-mode' : ''}`} onClick={() => inputRef.current?.click()} onDragOver={event => { event.preventDefault(); setDragActive(true) }} onDragLeave={() => setDragActive(false)} onDrop={event => { event.preventDefault(); setDragActive(false); selectFile(event.dataTransfer.files?.[0]) }}>{preview ? <><img className={`preview-image filter-${filterId}`} src={preview} alt="待处理预览" />{sticker !== 'none' && <span className={`template-sticker sticker-${sticker}`} aria-label="已选素材">{sticker === 'star' ? '✦' : '28·08'}</span>}</> : <div className="upload-prompt"><span>＋</span><strong>{dragActive ? '松开，放进今天' : '放一张照片进来'}</strong><small>点击或拖入 · JPG / PNG · 最大 1MB</small></div>}<input ref={inputRef} type="file" accept="image/jpeg,image/png" onChange={event => selectFile(event.target.files?.[0])} /><div className="stage-stamp">{filterId === 'none' ? 'ORIGINAL' : filterId.toUpperCase()}</div></div>
      <div className="control-panel"><div className="panel-section"><p className="panel-label">玩法库 · {playOptions.find(item => item.id === play)?.title}</p><div className="filter-list">{filters.filter(item => item.play === play).map(item => <button key={item.id} className={item.id === filterId ? `filter-option selected ${item.tone}` : 'filter-option'} onClick={() => setFilterId(item.id)}><span className="filter-swatch" /><span><b>{item.name}</b><small>{item.note}</small></span>{item.id === filterId && <i>✓</i>}</button>)}</div></div>{play === 'beauty' && <div className="panel-section"><div className="slider-label"><span>美颜强度</span><b>{beauty}</b></div><input className="range" type="range" min="0" max="100" value={beauty} onChange={event => setBeauty(Number(event.target.value))} /><div className="range-hints"><span>自然</span><span>更亮一点</span></div></div>}{play === 'template' && <div className="panel-section"><p className="panel-label">素材</p><div className="sticker-row">{['none', 'star', 'date'].map(item => <button key={item} className={sticker === item ? 'sticker selected' : 'sticker'} onClick={() => setSticker(item)}>{item === 'star' ? '✦' : item === 'date' ? '28·08' : '无'}</button>)}</div></div>}<div className="panel-section caption-section"><label className="panel-label" htmlFor="caption">写一句话 <span>{caption.length}/140</span></label><textarea id="caption" value={caption} maxLength={140} onChange={event => setCaption(event.target.value)} placeholder="让朋友知道你此刻在哪里……" /></div><button className="primary-button wide" disabled={busy} onClick={() => void publish()}>{busy ? '云端处理中…' : '释放到小圈'}<span>→</span></button></div></div>
  </section>
}

function MessagesView({ onToast }: { onToast: (message: string) => void }) {
  const [friend, setFriend] = useState('luna'); const [messages, setMessages] = useState<Message[]>([]); const [draft, setDraft] = useState(''); const [uploading, setUploading] = useState(false); const imageInputRef = useRef<HTMLInputElement>(null)
  useEffect(() => { void getMessages(friend).then(setMessages) }, [friend])
  async function send() { const body = draft.trim(); if (!body) return; const message = await sendMessage(friend, body); setMessages(current => [...current, message]); setDraft(''); onToast('轻信号已送达') }
  async function sendImage(file: File | undefined) {
    if (!file) return
    setUploading(true)
    try {
      const photo = await uploadPhoto(file, { filterId: 'none', caption: '', play: 'beauty', beauty: 0, sticker: 'none' })
      const message = await sendMessage(friend, '', photo.id)
      setMessages(current => [...current, { ...message, image_url: message.image_url ?? photo.image_url }])
      onToast('照片已送达')
    } catch { onToast('照片暂时没有送达') } finally { setUploading(false); if (imageInputRef.current) imageInputRef.current.value = '' }
  }
  return <section className="content-wrap messages-view"><div className="studio-head"><div><p className="section-kicker">SIGNALS / 轻回应</p><h2>一句话，轻轻抵达。</h2><p>没有未读红点，只有朋友留在这里的信号。</p></div><span className="signal-count">3 条新信号</span></div><div className="messages-layout"><div className="friend-list">{[['luna', '露娜', '今天 18:42'], ['momo', '墨墨', '昨天 21:06'], ['jiji', '吉吉', '周三 09:12']].map(([id, name, time]) => <button key={id} className={friend === id ? 'friend-row active' : 'friend-row'} onClick={() => setFriend(id)}><span className="friend-avatar">{name.slice(0, 1)}</span><span><b>{name}</b><small>{time}</small></span><i>·</i></button>)}</div><div className="conversation"><div className="conversation-head"><span>与 <b>{friend === 'luna' ? '露娜' : friend === 'momo' ? '墨墨' : '吉吉'} 的小信号</b></span><span className="online-label">● 在场</span></div><div className="message-stream">{messages.map(message => <div key={message.id} className={message.sender === 'me' ? 'bubble mine' : 'bubble'}>{message.kind === 'image' && (message.image_url || message.photo_id) && <img className="message-image" src={message.image_url ?? `/v1/photos/${message.photo_id}/image`} alt="发送的照片" />}{message.body && <p>{message.body}</p>}<small>{formatTime(message.createdAt)}</small></div>)}</div><div className="message-compose"><button aria-label="添加图片" disabled={uploading} onClick={() => imageInputRef.current?.click()}>{uploading ? '…' : '＋'}</button><input ref={imageInputRef} className="visually-hidden" type="file" accept="image/jpeg,image/png" onChange={event => void sendImage(event.target.files?.[0])} /><input value={draft} onChange={event => setDraft(event.target.value)} onKeyDown={event => { if (event.key === 'Enter') void send() }} placeholder="写一句话……" /><button className="send-button" onClick={() => void send()}>→</button></div></div></div></section>
}

function AiView({ feed, onToast }: { feed: FeedItem[]; onToast: (message: string) => void }) {
  const candidates = feed.slice(0, 4); const [selected, setSelected] = useState<string[]>([]); const [job, setJob] = useState<AiJob | null>(null)
  useEffect(() => { if (!job || (job.status !== 'queued' && job.status !== 'processing')) return; const timer = window.setTimeout(() => void getAiJob(job.id).then(setJob), 1600); return () => window.clearTimeout(timer) }, [job])
  async function create() { if (selected.length < 2) { onToast('请先选两张已授权的照片'); return }; setJob(await createAiJob(selected)); onToast('合照任务已开始') }
  return <section className="content-wrap ai-view"><div className="studio-head"><div><p className="section-kicker">AI STUDIO / 合照</p><h2>让两份在场，遇见一次。</h2><p>选择已授权的素材，云端会为你们合成一张新的记忆。</p></div><div className="consent-badge">⌁ 素材已授权</div></div><div className="ai-layout"><div><div className="material-grid">{candidates.map(item => <button key={item.id} className={selected.includes(item.id) ? 'material selected' : 'material'} onClick={() => setSelected(current => current.includes(item.id) ? current.filter(id => id !== item.id) : current.length < 2 ? [...current, item.id] : current)}><img src={item.image_url} alt="可用于合照的照片" /><span>{selected.includes(item.id) ? `0${selected.indexOf(item.id) + 1}` : '＋'}</span><small>{item.author.display_name} · {item.filter_id}</small></button>)}</div><div className="selection-meter"><span>已选素材</span><b>{selected.length} / 2</b><i><em style={{ width: `${selected.length * 50}%` }} /></i></div></div><div className="ai-result"><div className="result-art">{job?.status === 'completed' ? <img src={job.resultUrl} alt="AI 合照结果" /> : <><span className="result-symbol">⌁</span><p>{job?.status === 'processing' ? '正在把两份记忆放在一起…' : job?.status === 'queued' ? '已排队，云端马上开始…' : '合照会出现在这里'}</p></>}</div><div className="job-status"><div><span className={job?.status === 'completed' ? 'status-dot done' : 'status-dot'} /><b>{job?.status === 'completed' ? '合照已生成' : job ? '云端处理中' : '等待选择素材'}</b></div><button className="primary-button" onClick={() => void create()} disabled={job?.status === 'queued' || job?.status === 'processing'}>{job?.status === 'completed' ? '再生成一张' : '生成合照'}<span>→</span></button></div></div></div></section>
}

function DeviceView({ state, feed, onToast }: { state: { unseen_count: number; pending_friend_requests: number; server_time: string }; feed: FeedItem[]; onToast: (message: string) => void }) {
  const [logs, setLogs] = useState<string[]>(['device boot · CoreS3 Lite', 'GET /v1/device/state · 200', `unseen_count = ${state.unseen_count}`])
  const [busyAction, setBusyAction] = useState<string | null>(null)
  const [pairCode, setPairCode] = useState('')
  const [pairingBusy, setPairingBusy] = useState(false)
  function log(message: string) { setLogs(current => [`${new Date().toLocaleTimeString('zh-CN', { hour12: false })}  ${message}`, ...current].slice(0, 8)); onToast(message) }
  function runAction(action: string, message: string) { setBusyAction(action); window.setTimeout(() => { log(message); setBusyAction(null) }, 650) }
  async function pair() { setPairingBusy(true); try { const result = await requestPairCode('dvc_a1b2c3d4'); setPairCode(result.pair_code); log(`POST /v1/pair/code · 200 · expires ${result.expires_in}s`) } catch { onToast('配对码暂时无法获取') } finally { setPairingBusy(false) } }
  async function bind() { if (!pairCode) { onToast('先领取设备配对码'); return }; setPairingBusy(true); try { await bindDevice('dvc_a1b2c3d4', pairCode); log('POST /v1/pair/bind · 200 · device bound') } catch { onToast('配对码已过期，请重新领取') } finally { setPairingBusy(false) } }
  return <section className="content-wrap device-view"><div className="studio-head"><div><p className="section-kicker">DEVICE LAB / 联调</p><h2>小卡，准备好在场。</h2><p>用模拟器验证上传、拉取、下发和轻回应，不需要真实硬件。</p></div><span className="device-pill"><span className="live-dot" /> dvc_a1b2c3d4</span></div><div className="device-layout"><div className="device-card"><div className="device-screen"><div className="screen-top"><span>小卡 · 01</span><span>Wi-Fi ●</span></div><div className="screen-photo">{feed[0] ? <img src={feed[0].image_url} alt="设备当前照片" /> : <span>等待照片</span>}</div><div className="screen-bottom"><span>✦ {feed[0]?.reactions.heart ?? 0}</span><span>320 × 240</span></div></div><div className="device-controls"><button disabled={busyAction !== null} onClick={() => runAction('upload', 'POST /v1/photos · 201 · photo uploaded')}>拍照并上传 <span>{busyAction === 'upload' ? '…' : '↑'}</span></button><button disabled={busyAction !== null} onClick={() => runAction('feed', `GET /v1/feed · 200 · ${feed.length} items`)}>拉取圈子 <span>{busyAction === 'feed' ? '…' : '↓'}</span></button><button disabled={busyAction !== null} onClick={() => runAction('ack', 'POST /v1/device/ack · 204 · acknowledged')}>回执轻信号 <span>{busyAction === 'ack' ? '…' : '✦'}</span></button></div></div><div className="device-info"><div className="pairing-panel"><div><p className="panel-label">设备配对</p><small>设备先领码，再由当前账号绑定。</small></div><div className="pairing-actions"><button onClick={() => void pair()} disabled={pairingBusy}>{pairingBusy ? '处理中…' : '领取配对码'}</button><input aria-label="配对码" inputMode="numeric" maxLength={6} value={pairCode} onChange={event => setPairCode(event.target.value.replace(/\D/g, ''))} placeholder="6 位配对码" /><button onClick={() => void bind()} disabled={pairingBusy || pairCode.length !== 6}>绑定</button></div></div><div className="info-row"><span>设备状态</span><b className="green-text">在线</b></div><div className="info-row"><span>未看动态</span><b>{state.unseen_count}</b></div><div className="info-row"><span>待处理好友</span><b>{state.pending_friend_requests}</b></div><div className="info-row"><span>最后同步</span><b>{state.server_time ? formatTime(state.server_time) : '刚刚'}</b></div><div className="log-box"><p>REQUEST LOG</p>{logs.map((log, index) => <code key={`${log}-${index}`}>{log}</code>)}</div></div></div></section>
}

export default App
