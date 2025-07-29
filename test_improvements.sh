#!/bin/bash

# 测试改进功能的脚本
# 用于验证健康检查优化和连接状态跟踪

echo "=== RDP Forwarder 改进功能测试 ==="

# 检查是否有现有的进程
if pgrep -f rdp_forwarder > /dev/null; then
    echo "停止现有的 rdp_forwarder 进程..."
    pkill -f rdp_forwarder
    sleep 2
fi

# 创建测试配置文件
cat > test_config.conf << EOF
target_ip=10.145.165.217
target_port=3389
listen_port=3389
listen_interface=0.0.0.0
max_clients=10
connection_timeout=300
verbose_logging=1
buffer_size=8192
enable_stats=1
stats_interval=30
transport_mode=tcp_only
enable_fast_reconnect=1
keep_target_alive=1
reconnect_delay=1000
max_reconnect_attempts=3
EOF

echo "启动 RDP Forwarder 进行测试..."
./rdp_forwarder -c test_config.conf &
FORWARDER_PID=$!

echo "RDP Forwarder PID: $FORWARDER_PID"
sleep 3

echo ""
echo "=== 测试 1: 健康检查功能 ==="
echo "检查初始健康状态..."
sleep 5

echo ""
echo "=== 测试 2: 模拟连接断开 ==="
echo "尝试连接到转发器..."

# 使用 telnet 或 nc 测试连接
if command -v nc > /dev/null; then
    echo "使用 nc 测试连接..."
    timeout 5 nc localhost 3389 < /dev/null &
    NC_PID=$!
    sleep 2
    
    echo "断开连接..."
    kill $NC_PID 2>/dev/null
    
    echo "等待健康检查响应..."
    sleep 8
    
    echo "再次尝试连接..."
    timeout 5 nc localhost 3389 < /dev/null &
    NC_PID2=$!
    sleep 2
    kill $NC_PID2 2>/dev/null
else
    echo "nc 命令不可用，跳过连接测试"
fi

echo ""
echo "=== 测试 3: 查看日志输出 ==="
echo "最近的日志条目："
journalctl -u rdp_forwarder --since "1 minute ago" -n 20 2>/dev/null || echo "无法访问 systemd 日志"

echo ""
echo "=== 测试 4: 检查进程状态 ==="
if ps -p $FORWARDER_PID > /dev/null; then
    echo "RDP Forwarder 仍在运行"
    echo "进程信息："
    ps -p $FORWARDER_PID -o pid,ppid,cmd,etime
else
    echo "RDP Forwarder 进程已退出"
fi

echo ""
echo "=== 清理 ==="
echo "停止测试进程..."
kill $FORWARDER_PID 2>/dev/null
sleep 2

if ps -p $FORWARDER_PID > /dev/null; then
    echo "强制终止进程..."
    kill -9 $FORWARDER_PID 2>/dev/null
fi

rm -f test_config.conf

echo ""
echo "=== 测试完成 ==="
echo "请检查上述输出以验证以下改进："
echo "1. 健康检查间隔动态调整（健康时10秒，不健康时3秒）"
echo "2. 更详细的连接拒绝信息（包含恢复时间预估）"
echo "3. 增强的错误日志（包含连接状态、传输类型、持续时间等）"
echo "4. 连接状态跟踪（INIT -> CONNECTING -> CONNECTED -> ACTIVE 等）"
echo ""
echo "建议查看系统日志获取完整的测试结果："
echo "journalctl -f | grep rdp_forwarder"
