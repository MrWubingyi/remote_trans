#!/bin/bash

# RDP Forwarder 测试脚本

echo "=== RDP Forwarder Test Script ==="

# 检查程序是否存在
if [ ! -f "./rdp_forwarder" ]; then
    echo "Error: rdp_forwarder binary not found. Run 'make' first."
    exit 1
fi

# 检查配置文件
if [ ! -f "./rdp_forwarder.conf" ]; then
    echo "Error: rdp_forwarder.conf not found."
    exit 1
fi

echo "1. Testing configuration file parsing..."
echo "Configuration file content:"
cat rdp_forwarder.conf | grep -v "^#" | grep -v "^$"

echo ""
echo "2. Testing program startup (dry run)..."

# 创建测试配置文件
cat > test_config.conf << EOF
# Test configuration
target_ip=127.0.0.1
target_port=22
listen_port=2222
listen_interface=127.0.0.1
max_clients=5
connection_timeout=60
reconnect_interval=5
verbose_logging=1
buffer_size=4096
socket_timeout=30
enable_stats=1
stats_interval=30
log_file=/tmp/rdp_forwarder_test.log
EOF

echo "Created test configuration:"
cat test_config.conf

echo ""
echo "3. Testing program with test configuration..."
echo "Starting forwarder on port 2222 -> 127.0.0.1:22 (SSH)"
echo "Press Ctrl+C to stop the test"

# 运行程序（会在前台运行，便于测试）
./rdp_forwarder -c test_config.conf &
FORWARDER_PID=$!

# 等待程序启动
sleep 2

echo ""
echo "4. Testing connection..."
if netstat -ln | grep -q ":2222 "; then
    echo "✓ Forwarder is listening on port 2222"
    
    # 尝试连接测试
    echo "Testing connection with telnet..."
    timeout 5 telnet 127.0.0.1 2222 < /dev/null && echo "✓ Connection test passed" || echo "✗ Connection test failed (expected if SSH not running)"
else
    echo "✗ Forwarder is not listening on port 2222"
fi

echo ""
echo "5. Checking log output..."
if [ -f "/tmp/rdp_forwarder_test.log" ]; then
    echo "Log file contents:"
    cat /tmp/rdp_forwarder_test.log
else
    echo "No log file found (logging to syslog/journal)"
fi

# 清理
echo ""
echo "Cleaning up..."
kill $FORWARDER_PID 2>/dev/null || true
rm -f test_config.conf /tmp/rdp_forwarder_test.log

echo "Test completed."
