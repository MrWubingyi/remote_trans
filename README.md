# RDP 转发程序

这是一个高性能的RDP流量转发程序，专为解决网络延迟不稳定问题而设计。程序在Linux服务器上运行，将来自客户端的RDP连接转发到目标主机。

## 功能特性

- **高性能转发**: 使用非阻塞I/O和select()多路复用，支持多个并发连接
- **混合传输协议**: UDP主传输+TCP纠错，保证低延迟和高可靠性
- **快速重连机制**: 客户端断开时立即重置，保持目标连接活跃，实现毫秒级重连
- **配置文件支持**: 灵活的配置管理，无需重新编译即可调整参数
- **连接管理**: 自动检测连接断开，超时处理，资源清理
- **详细日志**: 支持syslog和文件日志，包含连接状态和传输统计
- **统计监控**: 实时统计连接数、传输量等信息
- **系统服务**: 支持systemd服务管理，开机自启动
- **优雅关闭**: 支持信号处理，安全关闭所有连接

## 系统要求

- Linux系统 (CentOS Stream, Ubuntu, Debian等)
- GCC编译器
- Make工具
- systemd (用于服务管理)

## 安装步骤

### 1. 快速安装

```bash
# 克隆或下载代码到服务器
# 运行安装脚本
sudo ./setup.sh
```

### 2. 手动安装

```bash
# 编译程序
make clean && make

# 安装二进制文件
sudo make install

# 复制配置文件
sudo cp rdp_forwarder.conf /etc/

# 安装systemd服务
sudo cp rdp_forwarder.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable rdp_forwarder

# 配置防火墙
sudo firewall-cmd --permanent --add-port=3389/tcp
sudo firewall-cmd --reload
```

## 配置说明

编辑配置文件 `/etc/rdp_forwarder.conf`:

```ini
# 目标主机配置
target_ip=192.168.192.100        # 目标主机的ZeroTier IP
target_port=3389                 # 目标端口

# 监听配置
listen_port=3389                 # 监听端口
listen_interface=0.0.0.0         # 监听接口

# 连接管理
max_clients=10                   # 最大并发连接数
connection_timeout=300           # 连接超时时间(秒)
reconnect_interval=5             # 重连间隔(秒)

# 日志配置
verbose_logging=1                # 详细日志
log_file=/var/log/rdp_forwarder.log

# 性能配置
buffer_size=8192                 # 缓冲区大小
socket_timeout=30                # Socket超时

# 监控配置
enable_stats=1                   # 启用统计
stats_interval=60                # 统计输出间隔(秒)

# 混合传输配置
transport_mode=hybrid            # 传输模式(udp/tcp/hybrid/auto)
udp_preference=0.8               # UDP偏好度(0.0-1.0)
retransmit_timeout=100           # 重传超时(毫秒)
max_retransmit=3                 # 最大重传次数
heartbeat_interval=1000          # 心跳间隔(毫秒)

# 快速重连配置
enable_fast_reconnect=1          # 启用快速重连
keep_target_alive=1              # 保持目标连接活跃
reconnect_delay=100              # 重连延迟(毫秒)
max_reconnect_attempts=5         # 最大重连尝试次数
connection_pool_size=2           # 连接池大小
```

## 使用方法

### 启动服务

```bash
sudo systemctl start rdp_forwarder
```

### 检查状态

```bash
sudo systemctl status rdp_forwarder
```

### 查看日志

```bash
# 查看systemd日志
sudo journalctl -u rdp_forwarder -f

# 查看文件日志
sudo tail -f /var/log/rdp_forwarder.log
```

### 重启服务

```bash
sudo systemctl restart rdp_forwarder
```

### 停止服务

```bash
sudo systemctl stop rdp_forwarder
```

### 测试快速重连

```bash
# 运行快速重连测试
./test_fast_reconnect.sh

# 手动测试
telnet <服务器IP> 3389
# 断开连接后立即重连，观察重连速度
```

## 网络架构

```
[主机A] --RDP--> [公网服务器B] --ZeroTier--> [主机C]
  |                    |                        |
受限网络            Linux转发程序              目标主机
                   (本程序)
```

1. 主机A通过公网IP连接到服务器B的3389端口
2. 服务器B上的转发程序接收连接
3. 程序通过ZeroTier网络转发到主机C的3389端口
4. 实现稳定的RDP连接

## 故障排除

### 1. 服务无法启动

```bash
# 检查配置文件语法
sudo rdp_forwarder -c /etc/rdp_forwarder.conf

# 检查端口占用
sudo netstat -tlnp | grep 3389

# 检查防火墙
sudo firewall-cmd --list-ports
```

### 2. 连接失败

```bash
# 检查目标主机连通性
ping 192.168.192.100

# 测试目标端口
telnet 192.168.192.100 3389

# 查看详细日志
sudo journalctl -u rdp_forwarder -n 50
```

### 3. 性能问题

- 调整 `buffer_size` 参数
- 增加 `max_clients` 数量
- 检查网络延迟和带宽

## 测试

运行测试脚本验证功能：

```bash
./test_forwarder.sh
```

## 维护

### 日志轮转

建议配置logrotate来管理日志文件：

```bash
sudo cat > /etc/logrotate.d/rdp_forwarder << EOF
/var/log/rdp_forwarder.log {
    daily
    rotate 7
    compress
    delaycompress
    missingok
    notifempty
    postrotate
        systemctl reload rdp_forwarder
    endscript
}
EOF
```

### 监控

可以通过以下方式监控程序状态：

- systemctl status检查服务状态
- journalctl查看日志
- 程序内置的统计信息
- 网络连接监控

## 许可证

本程序为开源软件，请根据实际需要使用和修改。
