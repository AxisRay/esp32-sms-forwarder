#!/usr/bin/env bash
# 在后台启动 monitor 日志抓取；需在已 source IDF 的环境下运行，或设置 IDF_PATH。
# 用法: ./scripts/run_monitor_logger.sh [串口]
# 例:   ./scripts/run_monitor_logger.sh /dev/ttyACM0

set -e
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PORT="${1:-/dev/ttyACM0}"
LOG_DIR="${PROJECT_DIR}/monitor_logs"
PID_FILE="${LOG_DIR}/monitor_logger.pid"

mkdir -p "$LOG_DIR"
cd "$PROJECT_DIR"

if ! command -v idf.py &>/dev/null; then
  echo "未找到 idf.py，请先执行: source \$IDF_PATH/export.sh"
  exit 1
fi

if [[ -f "$PID_FILE" ]]; then
  OLD_PID=$(cat "$PID_FILE")
  if kill -0 "$OLD_PID" 2>/dev/null; then
    echo "monitor_logger 已在运行 (PID $OLD_PID)，如需重启请先: kill $OLD_PID"
    exit 0
  fi
  rm -f "$PID_FILE"
fi

nohup python3 "$SCRIPT_DIR/monitor_logger.py" -p "$PORT" -d "$LOG_DIR" \
  </dev/null >> "${LOG_DIR}/monitor_logger.out" 2>&1 &
echo $! > "$PID_FILE"
echo "monitor_logger 已后台启动 (PID $(cat "$PID_FILE"))，日志目录: $LOG_DIR"
echo "查看实时日志: tail -f $LOG_DIR/monitor_*.log（每次重启会生成新文件）"
