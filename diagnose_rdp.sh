#!/bin/bash

# RDP转发器诊断脚本

echo "=== RDP转发器诊断工具 ==="

# 检查程序是否存在
if [ ! -f "./rdp_forwarder" ]; then
    echo "错误: rdp_forwarder 程序不存在"
    exit 1
fi

echo "1. 检查目标主机连通性..."
TARGET_IP=${1:-192.168.192.100}
echo "目标IP: $TARGET_IP"

# 检查网络连通性
if ping -c 3 -W 3 $TARGET_IP > /dev/null 2>&1; then
    echo "✓ 网络连通性: 正常"
else
    echo "✗ 网络连通性: 失败"
    echo "请检查目标主机IP地址和网络连接"
fi

# 检查目标端口
echo "2. 检查目标端口3389..."
if timeout 5 bash -c "echo >/dev/tcp/$TARGET_IP/3389" 2>/dev/null; then
    echo "✓ 目标端口3389: 可达"
else
    echo "✗ 目标端口3389: 不可达"
    echo "请检查目标主机RDP服务是否启动"
fi

# 检查本地端口占用
echo "3. 检查本地端口占用..."
if netstat -ln | grep -q ":3389 "; then
    echo "⚠ 端口3389已被占用:"
    netstat -ln | grep ":3389 "
    echo "请停止占用端口的程序或使用其他端口"
else
    echo "✓ 端口3389: 可用"
fi

echo "4. 测试基本TCP转发..."

# 创建测试配置
cat > test_diagnose.conf << EOF
target_ip=$TARGET_IP
target_port=3389
listen_port=3390
verbose_logging=1
enable_fast_reconnect=0
transport_mode=tcp
max_clients=5
buffer_size=8192
EOF

echo "使用测试端口3390进行诊断..."

# 启动转发器
./rdp_forwarder -c test_diagnose.conf &
FORWARDER_PID=$!

# 等待启动
sleep 2

if ps -p $FORWARDER_PID > /dev/null; then
    echo "✓ 转发器启动成功"
    
    # 检查监听
    if netstat -ln | grep -q ":3390 "; then
        echo "✓ 监听端口3390: 正常"
        
        # 测试连接
        echo "5. 测试连接转发..."
        if timeout 3 telnet 127.0.0.1 3390 < /dev/null > /dev/null 2>&1; then
            echo "✓ 连接转发: 正常"
        else
            echo "✗ 连接转发: 失败"
        fi
    else
        echo "✗ 监听端口3390: 失败"
    fi
    
    # 停止转发器
    kill $FORWARDER_PID 2>/dev/null
    wait $FORWARDER_PID 2>/dev/null
else
    echo "✗ 转发器启动失败"
fi

# 清理
rm -f test_diagnose.conf

echo ""
echo "=== 诊断建议 ==="

echo "如果RDP连接卡在'正在配置远程会话'："
echo "1. 确保目标主机RDP服务正常运行"
echo "2. 检查目标主机防火墙设置"
echo "3. 验证RDP用户权限和认证设置"
echo "4. 尝试直接连接目标主机（绕过转发器）"

echo ""
echo "如果出现连接循环断开："
echo "1. 临时禁用快速重连功能"
echo "2. 使用纯TCP模式而非混合传输"
echo "3. 增加缓冲区大小"
echo "4. 检查网络MTU设置"

echo ""
echo "推荐的故障排除配置:"
echo "transport_mode=tcp"
echo "enable_fast_reconnect=0"
echo "buffer_size=16384"
echo "verbose_logging=1"

echo ""
echo "=== 实时日志监控 ==="
echo "使用以下命令监控实时日志:"
echo "sudo journalctl -u rdp_forwarder -f"
echo ""
echo "或者使用管理脚本:"
echo "./rdp_forwarder_ctl.sh logs"
