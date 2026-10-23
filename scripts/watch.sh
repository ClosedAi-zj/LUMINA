#!/bin/bash
# LUMINA 开发进度盯梢脚本（主理人齐活林 · 自动巡检）
# 每 20 分钟记录一次两条实现线的健康度到 docs/progress-log.md
# 停滞 > 45 分钟标记 STALL；发现构建错误日志标记 ERR

LOG=/d/LUMINA/docs/progress-log.md
[ -f "$LOG" ] || echo "# LUMINA 开发进度日志（自动巡检）" > "$LOG"

check() {
  ts=$(date '+%Y-%m-%d %H:%M:%S')
  e=$(find /d/LUMINA/engine -type f 2>/dev/null | wc -l)
  a=$(find /d/LUMINA/app -type f -not -path "*/node_modules/*" 2>/dev/null | wc -l)
  newest=$(find /d/LUMINA/engine /d/LUMINA/app -type f -not -path "*/node_modules/*" -printf '%T@\n' 2>/dev/null | sort -rn | head -1)
  now=$(date +%s)
  idle=0
  [ -n "$newest" ] && idle=$(( (now - ${newest%.*}) / 60 ))
  b=0
  [ -d /d/LUMINA/build ] && b=$(find /d/LUMINA/build -type f 2>/dev/null | wc -l)
  status="OK"
  [ "$idle" -gt 45 ] && status="STALL(${idle}min)"
  if ls /d/LUMINA/build/engine/CMakeFiles/CMakeError.log >/dev/null 2>&1; then
    status="$status ERR(cmake-error-log)"
  fi
  if ls /d/LUMINA/app/node_modules/.logs >/dev/null 2>&1; then
    status="$status ERR(npm-logs)"
  fi
  echo "| $ts | engine=$e | app=$a | build_files=$b | idle=${idle}min | $status |" >> "$LOG"
}

# 立即建立基线，然后每 20 分钟一次，共 7 小时（21 次）
check
for i in $(seq 1 20); do
  sleep 1200
  check
done
