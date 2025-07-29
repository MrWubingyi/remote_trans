#!/bin/bash

# RDP Forwarder 管理脚本

SERVICE_NAME="rdp_forwarder"
CONFIG_FILE="/etc/rdp_forwarder.conf"
LOG_FILE="/var/log/rdp_forwarder.log"

show_usage() {
    echo "RDP Forwarder 管理脚本"
    echo ""
    echo "用法: $0 {start|stop|restart|status|logs|config|stats|health|test|install|uninstall}"
    echo ""
    echo "命令说明:"
    echo "  start     - 启动服务"
    echo "  stop      - 停止服务"
    echo "  restart   - 重启服务"
    echo "  status    - 查看服务状态"
    echo "  logs      - 查看实时日志"
    echo "  config    - 编辑配置文件"
    echo "  stats     - 显示连接统计"
    echo "  health    - 检查目标健康状态"
    echo "  test      - 运行快速重连测试"
    echo "  install   - 安装服务"
    echo "  uninstall - 卸载服务"
}

check_root() {
    if [[ $EUID -ne 0 ]]; then
        echo "错误: 此操作需要root权限，请使用sudo"
        exit 1
    fi
}

start_service() {
    echo "启动 RDP Forwarder 服务..."
    systemctl start $SERVICE_NAME
    if [ $? -eq 0 ]; then
        echo "✓ 服务启动成功"
        systemctl status $SERVICE_NAME --no-pager -l
    else
        echo "✗ 服务启动失败"
        exit 1
    fi
}

stop_service() {
    echo "停止 RDP Forwarder 服务..."
    systemctl stop $SERVICE_NAME
    if [ $? -eq 0 ]; then
        echo "✓ 服务停止成功"
    else
        echo "✗ 服务停止失败"
        exit 1
    fi
}

restart_service() {
    echo "重启 RDP Forwarder 服务..."
    systemctl restart $SERVICE_NAME
    if [ $? -eq 0 ]; then
        echo "✓ 服务重启成功"
        systemctl status $SERVICE_NAME --no-pager -l
    else
        echo "✗ 服务重启失败"
        exit 1
    fi
}

show_status() {
    echo "=== RDP Forwarder 服务状态 ==="
    systemctl status $SERVICE_NAME --no-pager -l
    
    echo ""
    echo "=== 网络监听状态 ==="
    netstat -tlnp | grep rdp_forwarder || echo "未找到监听端口"
    
    echo ""
    echo "=== 最近日志 ==="
    journalctl -u $SERVICE_NAME -n 10 --no-pager
}

show_logs() {
    echo "显示 RDP Forwarder 实时日志 (按 Ctrl+C 退出)..."
    journalctl -u $SERVICE_NAME -f
}

edit_config() {
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "错误: 配置文件 $CONFIG_FILE 不存在"
        exit 1
    fi
    
    echo "编辑配置文件: $CONFIG_FILE"
    ${EDITOR:-nano} $CONFIG_FILE
    
    echo "配置文件已修改，是否重启服务? (y/N)"
    read -r response
    if [[ "$response" =~ ^[Yy]$ ]]; then
        restart_service
    fi
}

show_stats() {
    echo "=== RDP Forwarder 统计信息 ==="
    
    # 从日志中提取统计信息
    if journalctl -u $SERVICE_NAME --since "1 hour ago" | grep -q "Statistics"; then
        echo "最近统计信息:"
        journalctl -u $SERVICE_NAME --since "1 hour ago" | grep -A 10 "Statistics" | tail -10
    else
        echo "未找到统计信息，可能服务未运行或统计功能未启用"
    fi
    
    echo ""
    echo "=== 当前连接状态 ==="
    if command -v ss &> /dev/null; then
        ss -tnp | grep rdp_forwarder || echo "未找到活跃连接"
    else
        netstat -tnp | grep rdp_forwarder || echo "未找到活跃连接"
    fi
}

check_health() {
    echo "=== 目标主机健康检查 ==="
    
    if [ ! -f "$CONFIG_FILE" ]; then
        echo "错误: 配置文件不存在"
        exit 1
    fi
    
    # 从配置文件读取目标IP和端口
    target_ip=$(grep "^target_ip=" $CONFIG_FILE | cut -d'=' -f2)
    target_port=$(grep "^target_port=" $CONFIG_FILE | cut -d'=' -f2)
    
    if [ -z "$target_ip" ] || [ -z "$target_port" ]; then
        echo "错误: 无法从配置文件读取目标IP或端口"
        exit 1
    fi
    
    echo "检查目标: $target_ip:$target_port"
    
    # 检查网络连通性
    if ping -c 3 -W 3 $target_ip > /dev/null 2>&1; then
        echo "✓ 网络连通性: 正常"
    else
        echo "✗ 网络连通性: 失败"
    fi
    
    # 检查端口连通性
    if timeout 5 bash -c "echo >/dev/tcp/$target_ip/$target_port" 2>/dev/null; then
        echo "✓ 端口连通性: 正常"
    else
        echo "✗ 端口连通性: 失败"
    fi
    
    # 检查服务日志中的健康状态
    if journalctl -u $SERVICE_NAME --since "10 minutes ago" | grep -q "unhealthy"; then
        echo "⚠ 警告: 最近日志中发现健康检查失败"
    else
        echo "✓ 健康检查: 正常"
    fi
}

install_service() {
    check_root
    echo "安装 RDP Forwarder 服务..."
    ./setup.sh
}

uninstall_service() {
    check_root
    echo "卸载 RDP Forwarder 服务..."
    
    # 停止并禁用服务
    systemctl stop $SERVICE_NAME 2>/dev/null
    systemctl disable $SERVICE_NAME 2>/dev/null
    
    # 删除服务文件
    rm -f /etc/systemd/system/$SERVICE_NAME.service
    systemctl daemon-reload
    
    # 删除二进制文件
    rm -f /usr/local/bin/rdp_forwarder
    
    # 询问是否删除配置和日志
    echo "是否删除配置文件和日志? (y/N)"
    read -r response
    if [[ "$response" =~ ^[Yy]$ ]]; then
        rm -f $CONFIG_FILE
        rm -f $LOG_FILE
        echo "✓ 配置文件和日志已删除"
    fi
    
    echo "✓ RDP Forwarder 已卸载"
}

run_test() {
    echo "=== RDP Forwarder 快速重连测试 ==="

    # 检查测试脚本是否存在
    if [ -f "./test_fast_reconnect.sh" ]; then
        echo "运行快速重连测试..."
        ./test_fast_reconnect.sh
    else
        echo "错误: 测试脚本 test_fast_reconnect.sh 不存在"
        echo "请确保在程序目录中运行此命令"
        exit 1
    fi
}

# 主程序
case "$1" in
    start)
        check_root
        start_service
        ;;
    stop)
        check_root
        stop_service
        ;;
    restart)
        check_root
        restart_service
        ;;
    status)
        show_status
        ;;
    logs)
        show_logs
        ;;
    config)
        check_root
        edit_config
        ;;
    stats)
        show_stats
        ;;
    health)
        check_health
        ;;
    test)
        run_test
        ;;
    install)
        install_service
        ;;
    uninstall)
        uninstall_service
        ;;
    *)
        show_usage
        exit 1
        ;;
esac

exit 0
