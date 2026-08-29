#!/bin/sh
# 自制中文字体 (main/pvc_font_cn16.c) 再生脚本
# 字库: Noto Sans SC Regular (OFL 许可, 可随产品分发)
# 字符集: 内置 Source Han 16 CJK 的 1187 常用字全集 + 固件全部 UI 用字
#         + CJK 标点 + U+00B7/U+2026 + ASCII (共 ~1423 字形)
# 新增 UI 文案含新字时: 重跑本脚本 (自动重扫 main/**.c 的字符串)
set -e
cd "$(dirname "$0")/.."
FONT=${1:-$HOME/Library/Fonts/NotoSansSC-Regular.ttf}
python3 - <<'PYEOF'
import re, glob
src=open('managed_components/lvgl__lvgl/src/font/lv_font_source_han_sans_sc_16_cjk.c',encoding='utf-8',errors='replace').read()
cps=set()
cmaps=re.findall(r'\{\s*\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = \d+,\s*\.unicode_list = (\w+|NULL),[^}]*?\.list_length = (\d+), \.type = (\w+)', src)
lists={m.group(1):[int(x,0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', m.group(2))] for m in re.finditer(r'static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};', src, re.S)}
for start,length,ul,ll,typ in cmaps:
    start=int(start); length=int(length)
    if ul=='NULL' or 'FORMAT0' in typ: cps.update(range(start,start+length))
    else: cps.update(start+o for o in lists.get(ul,[]))
cps={c for c in cps if c>126}
for f in glob.glob('main/*.c')+glob.glob('main/*.h')+glob.glob('main/net/*.c'):
    s=open(f,encoding='utf-8',errors='replace').read()
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', s):
        for ch in m.group(1):
            if ord(ch)>126: cps.add(ord(ch))
cps.add(0xB7); cps.add(0x2026)
open('/tmp/pvc_font_syms.txt','w',encoding='utf-8').write(''.join(chr(c) for c in sorted(cps)))
print("codepoints:", len(cps))
PYEOF
npx --yes lv_font_conv --font "$FONT" -r 0x20-0x7E \
  --symbols "$(cat /tmp/pvc_font_syms.txt)" --size 16 --bpp 4 \
  --format lvgl --lv-include lvgl.h --no-compress -o main/pvc_font_cn16.c
echo "OK -> main/pvc_font_cn16.c"
