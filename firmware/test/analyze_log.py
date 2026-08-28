#!/usr/bin/env python3
"""Presence Card 固件串口日志分析器。

用法:
    python3 analyze_log.py <log1> [log2 ...] [--json]

解析 [EV]/[FW] 埋点 (事件名清单见 main/pvc_trace.h), 输出各检查项
PASS/WARN/FAIL/SKIP 与统计。退出码: 有 FAIL 为 1, 否则 0 —— 可直接接 CI。
检查项与用例的对应关系见 test/testplan.md (C1..C12)。
"""
import json
import re
import statistics
import sys

EV_RE = re.compile(r"\[EV\] (\w+)\s*(.*)")
FW_HTTP_RE = re.compile(r"\[FW\] (GET|POST|DELETE) (\S+) (-?\d+) (\d+)")
FW_SLEEP_RE = re.compile(r"\[FW\] SLEEP (.*)")
FW_BOOT_RE = re.compile(r"\[FW\] boot (.*)")
PANIC_PATTERNS = [
    "guru meditation", "abort()", "assert failed", "stack smashing",
    "stack overflow", "corrupt heap", "heap corrupt", "wdt timeout",
    "loadprohibited", "storeprohibited", "illegalinstruction", "backtrace:",
    "task watchdog",
]


def kv(rest):
    return {m.group(1): m.group(2) for m in re.finditer(r"(\w+)=(\S+)", rest)}


def parse(paths):
    ev, http, sleeps, boots, panics = [], [], [], [], []
    for path in paths:
        with open(path, errors="replace") as f:
            for lineno, line in enumerate(f, 1):
                loc = f"{path}:{lineno}"
                low = line.lower()
                if any(p in low for p in PANIC_PATTERNS):
                    panics.append((loc, line.strip()))
                m = EV_RE.search(line)
                if m:
                    ev.append((loc, m.group(1), kv(m.group(2))))
                    continue
                m = FW_HTTP_RE.search(line)
                if m:
                    http.append((loc, {"method": m.group(1), "path": m.group(2),
                                       "status": int(m.group(3)),
                                       "ms": int(m.group(4))}))
                    continue
                m = FW_SLEEP_RE.search(line)
                if m:
                    sleeps.append((loc, kv(m.group(1))))
                    continue
                m = FW_BOOT_RE.search(line)
                if m:
                    boots.append((loc, kv(m.group(1))))
    return ev, http, sleeps, boots, panics


class Report:
    def __init__(self):
        self.items = []

    def add(self, cid, verdict, summary, evidence=None):
        self.items.append({"id": cid, "verdict": verdict, "summary": summary,
                           "evidence": evidence or []})

    def has_fail(self):
        return any(i["verdict"] == "FAIL" for i in self.items)


def run_checks(ev, http, sleeps, boots, panics):
    r = Report()

    def evs(name):
        return [d for _, n, d in ev if n == name]

    # C1 无崩溃 / 断言 / 看门狗
    if panics:
        r.add("C1", "FAIL", f"发现 {len(panics)} 处疑似崩溃输出",
              [f"{loc}: {line}" for loc, line in panics[:10]])
    else:
        r.add("C1", "PASS", "无 panic/assert/看门狗输出")

    # C2 wifi 连通
    ups, downs = evs("wifi_up"), evs("wifi_down")
    if ups or downs:
        r.add("C2", "PASS" if ups else "FAIL",
              f"wifi up {len(ups)} 次 / down {len(downs)} 次")
    else:
        r.add("C2", "SKIP", "无 wifi 事件")

    # C3 配对流程
    codes, bound, fails = evs("pair_code"), evs("pair_bound"), evs("pair_fail")
    if codes or bound or fails:
        if bound:
            r.add("C3", "PASS", f"配对完成 {len(bound)} 次 (领码 {len(codes)} 次)")
        elif fails:
            r.add("C3", "FAIL", f"配对失败 {len(fails)} 次, 未见成功")
        else:
            r.add("C3", "WARN", f"领码 {len(codes)} 次但未见绑定 (日志截断或未在 web 输码)")
    else:
        r.add("C3", "SKIP", "无配对事件 (已有 token, 正常)")

    # C4 上传闭环 + 幂等
    queued = {d["key"]: d for d in evs("upload_queued") if "key" in d}
    sent = {}
    for d in evs("upload_sent"):
        sent.setdefault(d.get("key", "?"), []).append(int(d.get("status", 0)))
    drops = {d.get("key", "?"): int(d.get("status", 0)) for d in evs("upload_drop")}
    missing = [k for k in queued if k not in sent and k not in drops]
    dup201 = {k: v for k, v in sent.items() if v.count(201) > 1}
    if not queued and not sent:
        r.add("C4", "SKIP", "无上传事件")
    elif drops:
        r.add("C4", "FAIL", f"{len(drops)} 张照片被服务器拒绝 (协议不一致)",
              [f"{k}: {s}" for k, s in list(drops.items())[:5]])
    elif dup201:
        r.add("C4", "FAIL", "同一幂等键多次 201 (幂等失效)", list(dup201)[:5])
    elif missing:
        r.add("C4", "WARN", f"{len(missing)} 张已入队未见成功回执 (日志截断或仍在队列)",
              missing[:5])
    else:
        r.add("C4", "PASS", f"{len(queued)} 张照片全部上传闭环, 幂等正常")

    # C5 断网补传
    deferred = {d.get("key", "?") for d in evs("upload_defer")}
    if deferred:
        still = [k for k in deferred if k not in sent]
        r.add("C5", "WARN" if still else "PASS",
              f"补传: {len(deferred)} 张曾延迟, {len(deferred) - len(still)} 张最终成功"
              + (f", 未闭环: {still[:3]}" if still else ""))
    else:
        r.add("C5", "SKIP", "无延迟重传事件")

    # C6 HTTP 健康度
    bad = [(loc, d) for loc, d in http if d["status"] in (400, 403, 413, 415)]
    unauth = [(loc, d) for loc, d in http if d["status"] == 401]
    react_401 = [x for x in unauth if "/reactions" in x[1]["path"]]
    other_401 = [x for x in unauth if "/reactions" not in x[1]["path"]]
    err5xx = [d for _, d in http if d["status"] >= 500]
    neg = [d for _, d in http if d["status"] < 0]
    if bad:
        r.add("C6", "FAIL", f"{len(bad)} 次协议级 4xx (接口不一致/固件 bug)",
              [f"{loc}: {d['method']} {d['path']} {d['status']}" for loc, d in bad[:8]])
    else:
        r.add("C6", "PASS", f"HTTP 共 {len(http)} 次, 无 400/403/413/415")
    if other_401:
        r.add("C6a", "INFO", f"非点赞 401 共 {len(other_401)} 次 (解绑测试则为预期)")
    if err5xx or neg:
        r.add("C6b", "WARN",
              f"5xx {len(err5xx)} 次 / 传输层失败 {len(neg)} 次 (server/网络侧)")

    # C7 feed
    polls = evs("feed_poll")
    errs = evs("feed_err")
    if polls or errs:
        n304 = sum(1 for d in polls if d.get("not_modified") == "1")
        nskip = sum(1 for d in polls if d.get("skipped") == "1")
        incomplete = [d for d in polls if d.get("complete") == "0"]
        v = "WARN" if incomplete or errs else "PASS"
        r.add("C7", v,
              f"feed 轮询 {len(polls)} 次 (304 {n304} / unseen=0 跳过 {nskip}"
              f" / 出错 {len(errs)})"
              + (f", {len(incomplete)} 次下载不完整(自动重拉)" if incomplete else ""))
    else:
        r.add("C7", "SKIP", "无 feed 事件")

    # C8 点赞 (server 设备 token 缺口 → 401 记 WARN)
    reacts = evs("react_send")
    if reacts:
        ok = [d for d in reacts if d.get("status") in ("200", "201")]
        r401 = [d for d in reacts if d.get("status") == "401"]
        other = [d for d in reacts if d not in ok and d not in r401]
        if ok and not other:
            r.add("C8", "PASS", f"点赞成功 {len(ok)} 次" +
                  (f" (另有 {len(r401)} 次 401)" if r401 else ""))
        elif r401 and not other:
            r.add("C8", "WARN", f"点赞全部 401 ({len(r401)} 次): server reactions "
                                 "暂不认设备 token (已知问题, 待全栈修复)")
        else:
            r.add("C8", "FAIL", "点赞出现异常状态", [str(d) for d in other[:5]])
    else:
        r.add("C8", "SKIP", "无点赞事件")

    # C9 配置下发闭环
    recv_ids = {d["id"] for d in evs("config_recv") if "id" in d}
    ack_ids = {d["id"] for d in evs("config_ack") if "id" in d}
    if recv_ids:
        un = recv_ids - ack_ids
        r.add("C9", "FAIL" if un else "PASS",
              f"配置下发 {len(recv_ids)} 个, 回执 {len(ack_ids)} 个"
              + (f", 未回执: {sorted(un)[:3]}" if un else ""))
    else:
        r.add("C9", "SKIP", "无配置下发事件")

    # C10 静默轮询纪律
    quiet_boots = [d for _, d in boots if d.get("quiet") == "1"]
    quiet_sleeps = [d for _, d in sleeps
                    if d.get("reason") in ("poll_done", "poll_timeout")]
    if quiet_boots:
        ups_ms = [int(d.get("uptime_ms", "0")) for d in quiet_sleeps]
        over = [u for u in ups_ms if u > 60000]
        if not quiet_sleeps:
            r.add("C10", "FAIL",
                  f"{len(quiet_boots)} 次静默唤醒但无回睡记录 (静默失效或被误唤醒)")
        elif over:
            r.add("C10", "FAIL", f"静默在线超时: {over} ms (硬限 45s + 余量)")
        else:
            r.add("C10", "PASS",
                  f"静默轮询 {len(quiet_sleeps)} 次, 在线 avg={int(statistics.mean(ups_ms))}ms"
                  f" max={max(ups_ms)}ms (目标<20000)")
        timeouts = [d for d in quiet_sleeps if d.get("reason") == "poll_timeout"]
        if timeouts:
            r.add("C10a", "WARN", f"{len(timeouts)} 次静默唤醒未完成同步即超时回睡")
    else:
        r.add("C10", "SKIP", "无静默唤醒 (未跑睡眠周期)")

    # C11 堆健康
    stats = evs("stat")
    heaps = [int(d["heap"]) for d in stats if "heap" in d]
    if len(heaps) >= 4:
        half = len(heaps) // 2
        early, late = statistics.mean(heaps[:half]), statistics.mean(heaps[half:])
        min_heap = min(int(d.get("min_heap", h)) for d, h in zip(stats, heaps))
        drop = early - late
        if min_heap < 20 * 1024:
            r.add("C11", "FAIL",
                  f"最低堆水位 {min_heap}B < 20KB (TLS 握手将失败)")
        elif drop > 20 * 1024:
            r.add("C11", "WARN",
                  f"堆前后半程均值下降 {int(drop)}B, 疑似泄漏 (min={min_heap}B)")
        else:
            r.add("C11", "PASS",
                  f"堆稳定: 均值 {int(statistics.mean(heaps))}B, 最低 {min_heap}B")
    else:
        r.add("C11", "SKIP", f"stat 心跳不足 ({len(heaps)} 条, 需≥4, 跑久一点)")

    # C13 预览帧率 (目标 25fps, 定时器 40ms; 低于阈值说明渲染/总线过载)
    pp = evs("perf_preview")
    fps = [int(d["fps"]) for d in pp if "fps" in d]
    # 拍照/翻相册的那一秒帧率会掉, 排除 0 帧样本后再统计, 但单独报告停顿
    active = [x for x in fps if x > 0]
    if active:
        avg = statistics.mean(active)
        cpu = [int(d.get("cpu_pct", 0)) for d in pp]
        stalls = len(fps) - len(active)
        if avg < 10:
            v = "FAIL"
        elif avg < 20:
            v = "WARN"
        else:
            v = "PASS"
        r.add("C13", v,
              f"预览帧率 avg={avg:.1f} min={min(active)} max={max(active)} fps "
              f"(样本 {len(active)}s, 渲染 CPU avg={statistics.mean(cpu):.0f}%"
              + (f", 全停顿 {stalls}s" if stalls else "") + ")")
    else:
        r.add("C13", "SKIP", "无 perf_preview 事件 (预览未运行)")

    # C14 拍照管线时延 (快门 -> JPEG 入队)
    photos = evs("perf_photo")
    encs = evs("perf_encode")
    if photos:
        def col(rows, k):
            return [int(d[k]) for d in rows if k in d]
        totals = col(photos, "total_ms")
        lines = [
            "grab avg=%dms  blur avg=%dms  filter avg=%dms  bmp avg=%dms" % (
                statistics.mean(col(photos, "grab_ms") or [0]),
                statistics.mean(col(photos, "blur_ms") or [0]),
                statistics.mean(col(photos, "filter_ms") or [0]),
                statistics.mean(col(photos, "bmp_ms") or [0])),
        ]
        if encs:
            lines.append("scale avg=%dms  swap avg=%dms  jpeg avg=%dms  avg %dKB" % (
                statistics.mean(col(encs, "scale_ms") or [0]),
                statistics.mean(col(encs, "swap_ms") or [0]),
                statistics.mean(col(encs, "encode_ms") or [0]),
                statistics.mean(col(encs, "bytes") or [0]) / 1024))
        avg_t = statistics.mean(totals) if totals else 0
        v = "FAIL" if avg_t > 5000 else ("WARN" if avg_t > 3000 else "PASS")
        r.add("C14", v,
              f"拍照管线 {len(photos)} 次: 快门→入队 avg={int(avg_t)}ms "
              f"max={max(totals) if totals else 0}ms (阈值 warn>3s fail>5s)", lines)
        feed_dec = col(evs("perf_feed_decode"), "ms")
        if feed_dec:
            r.add("C14a", "INFO",
                  f"feed 解码 avg={int(statistics.mean(feed_dec))}ms "
                  f"max={max(feed_dec)}ms ({len(feed_dec)} 次)")
    else:
        r.add("C14", "SKIP", "无拍照性能事件")

    # C12 延迟统计 (信息项)
    lat = {}
    for _, d in http:
        lat.setdefault(d["path"].split("?")[0], []).append(d["ms"])
    lines = []
    for p, xs in sorted(lat.items()):
        xs.sort()
        p95 = xs[max(0, int(len(xs) * 0.95) - 1)]
        lines.append(f"{p}: n={len(xs)} avg={int(statistics.mean(xs))}ms"
                     f" p95={p95}ms max={xs[-1]}ms")
    r.add("C12", "INFO", "请求延迟统计", lines)

    return r


def main():
    paths = [a for a in sys.argv[1:] if not a.startswith("--")]
    if not paths:
        print(__doc__)
        return 2
    ev, http, sleeps, boots, panics = parse(paths)
    total = len(ev) + len(http) + len(sleeps) + len(boots)
    if total == 0:
        print("未解析到任何 [EV]/[FW] 事件 —— 日志为空或埋点未生效?")
        return 1
    rep = run_checks(ev, http, sleeps, boots, panics)
    if "--json" in sys.argv:
        print(json.dumps(rep.items, ensure_ascii=False, indent=2))
    else:
        print(f"解析 {total} 条事件 (EV {len(ev)} / HTTP {len(http)} / "
              f"SLEEP {len(sleeps)} / BOOT {len(boots)}), 文件 {len(paths)} 个\n")
        mark = {"PASS": "✅", "WARN": "⚠️ ", "FAIL": "❌", "SKIP": "⏭ ", "INFO": "ℹ️ "}
        for i in rep.items:
            print(f"{mark[i['verdict']]} {i['id']:<4} {i['verdict']:<5} {i['summary']}")
            for e in i["evidence"]:
                print(f"         {e}")
    return 1 if rep.has_fail() else 0


if __name__ == "__main__":
    sys.exit(main())
