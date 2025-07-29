#!/bin/bash

set -e  # 遇到错误时退出

echo "=== RDP Forwarder Installation Script ==="

# 检查是否为root用户
if [[ $EUID -ne 0 ]]; then
   echo "This script must be run as root (use sudo)"
   exit 1
fi

# 编译程序
echo "Building RDP Forwarder..."
make clean && make

# 安装程序
echo "Installing binary..."
make install

# 安装配置文件
echo "Installing configuration file..."
if [ ! -f /etc/rdp_forwarder.conf ]; then
    cp rdp_forwarder.conf /etc/rdp_forwarder.conf
    echo "Configuration file installed to /etc/rdp_forwarder.conf"
    echo "Please edit /etc/rdp_forwarder.conf to set your target IP address"
else
    echo "Configuration file already exists at /etc/rdp_forwarder.conf"
    echo "Backup created as /etc/rdp_forwarder.conf.backup"
    cp /etc/rdp_forwarder.conf /etc/rdp_forwarder.conf.backup
    cp rdp_forwarder.conf /etc/rdp_forwarder.conf.new
    echo "New configuration template saved as /etc/rdp_forwarder.conf.new"
fi

# 创建日志目录
echo "Creating log directory..."
mkdir -p /var/log
touch /var/log/rdp_forwarder.log
chmod 644 /var/log/rdp_forwarder.log

# 配置防火墙
echo "Configuring firewall..."
if command -v firewall-cmd &> /dev/null; then
    firewall-cmd --permanent --add-port=3389/tcp
    firewall-cmd --reload
    echo "Firewall configured (firewalld)"
elif command -v ufw &> /dev/null; then
    ufw allow 3389/tcp
    echo "Firewall configured (ufw)"
else
    echo "Warning: No supported firewall found. Please manually open port 3389/tcp"
fi

# 安装systemd服务
echo "Installing systemd service..."
cp rdp_forwarder.service /etc/systemd/system/
systemctl daemon-reload
systemctl enable rdp_forwarder

# 启动服务
echo "Starting RDP Forwarder service..."
systemctl start rdp_forwarder

# 检查状态
sleep 2
status=$(systemctl is-active rdp_forwarder)
echo ""
echo "=== Installation Complete ==="
echo "Service status: $status"

if [ "$status" = "active" ]; then
    echo "✓ RDP Forwarder is running successfully"
    echo ""
    echo "Configuration file: /etc/rdp_forwarder.conf"
    echo "Log file: /var/log/rdp_forwarder.log"
    echo "Service management:"
    echo "  - Check status: systemctl status rdp_forwarder"
    echo "  - View logs: journalctl -u rdp_forwarder -f"
    echo "  - Restart: systemctl restart rdp_forwarder"
    echo ""
    echo "Remember to edit /etc/rdp_forwarder.conf with your target IP address!"
else
    echo "✗ Service failed to start. Check logs with: journalctl -u rdp_forwarder"
    exit 1
fi