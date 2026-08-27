#!/usr/bin/env bash
# 每日备份：pg_dump → 本地 → OSS（crontab 示例：0 3 * * * /path/to/backup.sh）
# 运维基线见 docs/01 §9
set -euo pipefail
TS=$(date -u +%Y%m%dT%H%M%SZ)
OUT="/tmp/presence_${TS}.sql.gz"

docker compose -f "$(dirname "$0")/docker-compose.yml" exec -T db \
  pg_dump -U postgres presence | gzip > "$OUT"

# TODO(全栈): 装好 ossutil 后取消注释（保留 30 天，生命周期规则在 OSS 控制台配）
# ossutil cp "$OUT" "oss://YOUR_BACKUP_BUCKET/db/presence_${TS}.sql.gz"

echo "backup written: $OUT"
