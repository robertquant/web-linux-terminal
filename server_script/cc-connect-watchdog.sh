#!/bin/bash
#
# cc-connect Watchdog 守护脚本
# - 后台运行 cc-connect
# - 故障自动重启
# - 信号优雅退出
# - 记录日志
#

# 配置
CC_BIN="cc-connect"
CC_CONFIG="/root/.cc-connect/config.toml"
LOG_DIR="/root/.cc-connect/logs"
PID_FILE="$LOG_DIR/cc-connect-watchdog.pid"
LOG_FILE="$LOG_DIR/cc-connect-watchdog.log"
RESTART_DELAY=5
CHECK_INTERVAL=10

mkdir -p "$LOG_DIR"

# 日志
log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" | tee -a "$LOG_FILE"
}

# 获取 cc-connect 主进程 PID（排除 grep 和 watchdog 自身）
get_cc_pid() {
    pgrep -x "cc-connect" 2>/dev/null || true
}

# 检查 cc-connect 是否在运行
check_running() {
    local pids=$(get_cc_pid)
    [ -n "$pids" ]
}

# 停止所有 cc-connect 进程
stop_all() {
    log "停止 cc-connect 进程..."

    local pids=$(get_cc_pid)
    if [ -n "$pids" ]; then
        log "发送 SIGTERM: $pids"
        echo "$pids" | xargs kill 2>/dev/null || true
        sleep 3

        # 检查是否还在，强制杀
        pids=$(get_cc_pid)
        if [ -n "$pids" ]; then
            log "进程未退出，发送 SIGKILL: $pids"
            echo "$pids" | xargs kill -9 2>/dev/null || true
            sleep 1
        fi
    fi

    rm -f "$PID_FILE"
    log "已停止"
}

# 启动 cc-connect
start_cc() {
    log "启动 cc-connect..."

    nohup $CC_BIN --config "$CC_CONFIG" >> "$LOG_FILE" 2>&1 &
    local pid=$!
    sleep 3

    if kill -0 "$pid" 2>/dev/null; then
        log "cc-connect 已启动 (PID: $pid)"
        echo "$pid" > "$PID_FILE"
        return 0
    else
        log "错误: cc-connect 启动失败，检查日志:"
        tail -20 "$LOG_FILE" | tee -a "$LOG_FILE"
        return 1
    fi
}

# 优雅退出标志
RUNNING=true

cleanup() {
    local sig=$1
    log "收到退出信号 SIG${sig:-UNKNOWN}，正在停止..."
    RUNNING=false
    stop_all
    log "watchdog 已退出"
    exit 0
}

trap 'cleanup TERM' SIGTERM
trap 'cleanup INT' SIGINT
trap 'cleanup QUIT' SIGQUIT
trap 'cleanup HUP' SIGHUP

# 守护循环
daemon_loop() {
    log "watchdog 启动，进入守护模式..."

    # 完全脱离父进程组和终端会话
    disown -a 2>/dev/null || true
    
    echo "$$" > "$PID_FILE"
    stop_all
    start_cc

    while $RUNNING; do
        if ! check_running; then
            log "检测到 cc-connect 已退出，${RESTART_DELAY}秒后重启..."
            sleep "$RESTART_DELAY"
            start_cc
            if [ $? -ne 0 ]; then
                log "启动失败，${RESTART_DELAY}秒后重试..."
                sleep "$RESTART_DELAY"
            fi
        fi
        sleep "$CHECK_INTERVAL"
    done
}

# 单次启动
start_once() {
    if check_running; then
        log "cc-connect 已在运行中"
        local pids=$(get_cc_pid)
        log "PID: $pids"
        exit 0
    fi
    stop_all
    start_cc
}

# 显示状态
status() {
    echo "=== cc-connect 状态 ==="

    if check_running; then
        local pids=$(get_cc_pid)
        echo "cc-connect 进程: 运行中 (PID: $pids)"
    else
        echo "cc-connect 进程: 未运行"
    fi

    echo ""
    echo "日志文件: $LOG_FILE"
    echo "PID 文件: $PID_FILE"
    echo "配置文件: $CC_CONFIG"

    if [ -f "$LOG_FILE" ]; then
        echo ""
        echo "--- 最近日志 (最后10行) ---"
        tail -10 "$LOG_FILE"
    fi
}

# 用法
usage() {
    echo "用法: $0 {start|stop|restart|status|daemon}"
    echo ""
    echo "命令:"
    echo "  start   - 启动 cc-connect（单次，不监控）"
    echo "  stop    - 停止 cc-connect"
    echo "  restart - 重启 cc-connect"
    echo "  status  - 显示状态"
    echo "  daemon  - 启动守护模式（持续监控，自动重启）"
    echo ""
    echo "推荐用法："
    echo "  nohup $0 daemon &"
}

case "${1:-}" in
    start)
        start_once
        ;;
    stop)
        stop_all
        ;;
    restart)
        stop_all
        sleep 2
        start_once
        ;;
    status)
        status
        ;;
    daemon)
        daemon_loop
        ;;
    *)
        usage
        exit 1
        ;;
esac
