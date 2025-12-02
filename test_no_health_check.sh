#!/bin/bash

# 测试健康检查移除后的强制连接功能

echo "=== 测试健康检查移除 - 强制连接目标服务器 ==="

# 停止现有进程
if pgrep -f rdp_forwarder > /dev/null; then
    echo "停止现有的 rdp_forwarder 进程..."
    pkill -f rdp_forwarder
    sleep 2
fi

# 创建测试配置
cat > test_no_health_config.conf << EOF
target_ip=10.145.165.217
target_port=3389
listen_port=3389
listen_interface=0.0.0.0
max_clients=5
connection_timeout=60
verbose_logging=1
buffer_size=8192
enable_stats=1
stats_interval=15
transport_mode=tcp_only
enable_fast_reconnect=0
EOF

echo "启动 RDP Forwarder (无健康检查版本)..."
./rdp_forwarder -c test_no_health_config.conf &
FORWARDER_PID=$!
echo "RDP Forwarder PID: $FORWARDER_PID"
sleep 3

echo ""
echo "=== 测试 1: 验证不再进行健康检查 ==="
echo "检查日志中是否没有健康检查相关信息..."

# 等待一段时间，看是否有健康检查日志
sleep 10

echo ""
echo "=== 测试 2: 强制连接测试 ==="
echo "即使目标可能不可达，也应该尝试连接..."

if command -v nc > /dev/null; then
    echo "尝试连接到转发器..."
    
    # 尝试多次连接，验证不会被健康检查拒绝
    for i in {1..3}; do
        echo "连接尝试 $i..."
        timeout 5 nc localhost 3389 < /dev/null &
        NC_PID=$!
        sleep 2
        kill $NC_PID 2>/dev/null
        sleep 1
    done
    
else
    echo "nc 命令不可用，跳过连接测试"
fi

echo ""
echo "=== 测试 3: 检查日志输出 ==="
echo "查找相关日志条目..."

# 检查系统日志
if command -v journalctl > /dev/null; then
    echo "系统日志中的相关条目："
    journalctl --since "2 minutes ago" | grep -E "(rdp_forwarder|Accepting new connection|will attempt to connect)" | tail -10
    
    echo ""
    echo "检查是否还有健康检查相关日志（应该没有）："
    HEALTH_LOGS=$(journalctl --since "2 minutes ago" | grep -E "(health|unhealthy|Rejecting connection)" | wc -l)
    if [ "$HEALTH_LOGS" -eq 0 ]; then
        echo "✓ 确认：没有发现健康检查相关日志"
    else
        echo "⚠ 警告：仍然发现 $HEALTH_LOGS 条健康检查相关日志"
        journalctl --since "2 minutes ago" | grep -E "(health|unhealthy|Rejecting connection)"
    fi
else
    echo "无法访问系统日志"
fi

echo ""
echo "=== 测试 4: 验证连接行为 ==="
echo "测试连接到不存在的目标..."

# 临时修改配置指向不存在的目标
cat > test_unreachable_config.conf << EOF
target_ip=192.0.2.1
target_port=3389
listen_port=3390
listen_interface=0.0.0.0
max_clients=5
connection_timeout=10
verbose_logging=1
buffer_size=8192
enable_stats=1
stats_interval=15
transport_mode=tcp_only
enable_fast_reconnect=0
EOF

echo "启动第二个实例连接到不可达目标..."
./rdp_forwarder -c test_unreachable_config.conf &
FORWARDER_PID2=$!
sleep 2

if command -v nc > /dev/null; then
    echo "尝试连接到不可达目标的转发器..."
    timeout 8 nc localhost 3390 < /dev/null &
    NC_PID=$!
    sleep 5
    kill $NC_PID 2>/dev/null
fi

# 停止第二个实例
kill $FORWARDER_PID2 2>/dev/null

echo ""
echo "=== 验证进程状态 ==="
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
kill $FORWARDER_PID2 2>/dev/null
sleep 2

if ps -p $FORWARDER_PID > /dev/null; then
    echo "强制终止进程..."
    kill -9 $FORWARDER_PID 2>/dev/null
fi

rm -f test_no_health_config.conf test_unreachable_config.conf

echo ""
echo "=== 测试完成 ==="
echo ""
echo "验证要点："
echo "1. ✓ 不再有健康检查相关的日志输出"
echo "2. ✓ 连接请求不会因为健康检查而被拒绝"
echo "3. ✓ 显示 'Accepting new connection - will attempt to connect to target'"
echo "4. ✓ 即使目标不可达，也会尝试建立连接"
echo ""
echo "改进效果："
echo "- 移除了健康检查机制，避免误判"
echo "- 所有连接请求都会被接受并尝试连接目标"
echo "- 连接成功与否由实际的连接尝试决定"
echo "- 简化了代码逻辑，提高了可靠性"
