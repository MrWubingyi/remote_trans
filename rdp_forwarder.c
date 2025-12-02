#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <syslog.h>
#include <stdarg.h>
#include <netinet/tcp.h>
#include "hybrid_transport.h"

#define DEFAULT_RDP_PORT 3389
#define DEFAULT_BUFFER_SIZE 8192
#define DEFAULT_MAX_CLIENTS 10
#define DEFAULT_CONNECTION_TIMEOUT 300  // 5分钟超时
#define DEFAULT_RECONNECT_INTERVAL 5    // 重连间隔秒数
#define CONFIG_FILE "/etc/rdp_forwarder.conf"
#define MAX_CONFIG_LINE 256

typedef enum {
    CONN_STATE_INIT = 0,        // 初始状态
    CONN_STATE_CONNECTING,      // 正在连接
    CONN_STATE_CONNECTED,       // 已连接
    CONN_STATE_ACTIVE,          // 活跃传输中
    CONN_STATE_CLIENT_DISCONNECTED, // 客户端断开
    CONN_STATE_TARGET_DISCONNECTED, // 目标端断开
    CONN_STATE_RECONNECTING,    // 重连中
    CONN_STATE_ERROR,           // 错误状态
    CONN_STATE_CLOSING          // 正在关闭
} connection_state_t;

typedef struct {
    int client_fd;
    int target_fd;
    char target_ip[16];
    time_t last_activity;
    int is_active;
    unsigned long bytes_sent;
    unsigned long bytes_received;

    // 混合传输连接
    ht_connection_t* ht_conn;
    int use_hybrid_transport;

    // 快速重连状态
    int client_disconnected;
    int target_ready;
    time_t disconnect_time;
    int reconnect_attempts;

    // 连接状态跟踪
    connection_state_t state;
    time_t state_change_time;
    time_t connection_start_time;
    char last_error[256];
    int error_count;
} connection_pair_t;

typedef struct {
    char target_ip[16];
    int target_port;
    int listen_port;
    char listen_interface[16];
    int max_clients;
    int connection_timeout;
    int reconnect_interval;
    int verbose_logging;
    int buffer_size;
    int socket_timeout;
    int enable_stats;
    int stats_interval;
    char log_file[256];

    // 混合传输配置
    ht_transport_mode_t transport_mode;
    float udp_preference;
    int retransmit_timeout;
    int max_retransmit;
    int heartbeat_interval;

    // 快速重连配置
    int enable_fast_reconnect;
    int keep_target_alive;
    int reconnect_delay;
    int max_reconnect_attempts;
    int connection_pool_size;
} config_t;

// 全局配置和状态
config_t config;
connection_pair_t* connections;
int connection_count = 0;
volatile int running = 1;

// 统计信息
typedef struct {
    unsigned long total_connections;
    unsigned long active_connections;
    unsigned long total_bytes_sent;
    unsigned long total_bytes_received;
    time_t start_time;
    time_t last_stats_time;
} stats_t;

stats_t stats;

// 健康检查已移除 - 强制连接目标服务器

// 函数声明
void log_message(int priority, const char* format, ...);
int set_nonblocking(int fd);
void cleanup_connection(int index);
int forward_data(int from_fd, int to_fd, connection_pair_t* conn, int is_client_to_target);
int create_listen_socket(int port);
int connect_to_target(const char* target_ip, int port);
void signal_handler(int sig);
void init_config(void);
int load_config(const char* config_file);
void init_stats(void);
void print_stats(void);
void update_stats(void);
// 健康检查函数已移除
int create_hybrid_connection(connection_pair_t* conn, const char* target_ip, int port);
int forward_data_hybrid(connection_pair_t* conn, int from_client);
void handle_client_disconnect(connection_pair_t* conn);
void log_connection_error(connection_pair_t* conn, int error_code, const char* context, int is_client_side);
int try_reconnect_target(connection_pair_t* conn);
void reset_connection_for_reuse(connection_pair_t* conn);
int is_client_socket_alive(int fd);
void set_connection_state(connection_pair_t* conn, connection_state_t new_state, const char* reason);
const char* get_connection_state_name(connection_state_t state);
void log_connection_state_change(connection_pair_t* conn, int conn_index);

// TCP socket 参数调优（在客户端和目标端两侧保持一致行为，提升 RDP 兼容性）
static void configure_tcp_socket(int fd);

// 信号处理函数
void signal_handler(int sig) {
    log_message(LOG_INFO, "Received signal %d, shutting down gracefully...", sig);
    running = 0;
}

// 初始化默认配置
void init_config(void) {
    strcpy(config.target_ip, "192.168.192.100");
    config.target_port = DEFAULT_RDP_PORT;
    config.listen_port = DEFAULT_RDP_PORT;
    strcpy(config.listen_interface, "0.0.0.0");
    config.max_clients = DEFAULT_MAX_CLIENTS;
    config.connection_timeout = DEFAULT_CONNECTION_TIMEOUT;
    config.reconnect_interval = DEFAULT_RECONNECT_INTERVAL;
    config.verbose_logging = 1;
    config.buffer_size = DEFAULT_BUFFER_SIZE;
    config.socket_timeout = 30;
    config.enable_stats = 1;
    config.stats_interval = 60;
    strcpy(config.log_file, "/var/log/rdp_forwarder.log");

    // 混合传输默认配置（暂时使用TCP模式确保兼容性）
    config.transport_mode = HT_MODE_TCP_ONLY;
    config.udp_preference = 0.0f;
    config.retransmit_timeout = 100;
    config.max_retransmit = 3;
    config.heartbeat_interval = 1000;

    // 快速重连默认配置（暂时禁用以确保基本功能正常）
    config.enable_fast_reconnect = 0;
    config.keep_target_alive = 1;
    config.reconnect_delay = 100;
    config.max_reconnect_attempts = 5;
    config.connection_pool_size = 2;
}

// 去除字符串首尾空白字符
char* trim(char* str) {
    char* end;
    while (*str == ' ' || *str == '\t') str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    *(end + 1) = 0;
    return str;
}

// 加载配置文件
int load_config(const char* config_file) {
    FILE* fp = fopen(config_file, "r");
    if (!fp) {
        log_message(LOG_WARNING, "Cannot open config file %s, using defaults", config_file);
        return 0;
    }

    char line[MAX_CONFIG_LINE];
    int line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;
        char* trimmed = trim(line);

        // 跳过空行和注释
        if (strlen(trimmed) == 0 || trimmed[0] == '#') {
            continue;
        }

        // 查找等号
        char* eq = strchr(trimmed, '=');
        if (!eq) {
            // 安全地记录无效配置行，避免格式字符串攻击
            if (trimmed && strlen(trimmed) > 0) {
                log_message(LOG_WARNING, "Invalid config line %d: %.*s", line_num, (int)strlen(trimmed), trimmed);
            } else {
                log_message(LOG_WARNING, "Invalid config line %d: (empty)", line_num);
            }
            continue;
        }

        *eq = '\0';
        char* key = trim(trimmed);
        char* value = trim(eq + 1);

        // 解析配置项
        if (strcmp(key, "target_ip") == 0) {
            strncpy(config.target_ip, value, sizeof(config.target_ip) - 1);
        } else if (strcmp(key, "target_port") == 0) {
            config.target_port = atoi(value);
        } else if (strcmp(key, "listen_port") == 0) {
            config.listen_port = atoi(value);
        } else if (strcmp(key, "listen_interface") == 0) {
            strncpy(config.listen_interface, value, sizeof(config.listen_interface) - 1);
        } else if (strcmp(key, "max_clients") == 0) {
            config.max_clients = atoi(value);
        } else if (strcmp(key, "connection_timeout") == 0) {
            config.connection_timeout = atoi(value);
        } else if (strcmp(key, "reconnect_interval") == 0) {
            config.reconnect_interval = atoi(value);
        } else if (strcmp(key, "verbose_logging") == 0) {
            config.verbose_logging = atoi(value);
        } else if (strcmp(key, "buffer_size") == 0) {
            config.buffer_size = atoi(value);
        } else if (strcmp(key, "socket_timeout") == 0) {
            config.socket_timeout = atoi(value);
        } else if (strcmp(key, "enable_stats") == 0) {
            config.enable_stats = atoi(value);
        } else if (strcmp(key, "stats_interval") == 0) {
            config.stats_interval = atoi(value);
        } else if (strcmp(key, "log_file") == 0) {
            strncpy(config.log_file, value, sizeof(config.log_file) - 1);
        } else if (strcmp(key, "transport_mode") == 0) {
            if (strcmp(value, "udp") == 0) {
                config.transport_mode = HT_MODE_UDP_ONLY;
            } else if (strcmp(value, "tcp") == 0) {
                config.transport_mode = HT_MODE_TCP_ONLY;
            } else if (strcmp(value, "hybrid") == 0) {
                config.transport_mode = HT_MODE_HYBRID;
            } else if (strcmp(value, "auto") == 0) {
                config.transport_mode = HT_MODE_AUTO;
            }
        } else if (strcmp(key, "udp_preference") == 0) {
            config.udp_preference = atof(value);
            if (config.udp_preference < 0.0f) config.udp_preference = 0.0f;
            if (config.udp_preference > 1.0f) config.udp_preference = 1.0f;
        } else if (strcmp(key, "retransmit_timeout") == 0) {
            config.retransmit_timeout = atoi(value);
        } else if (strcmp(key, "max_retransmit") == 0) {
            config.max_retransmit = atoi(value);
        } else if (strcmp(key, "heartbeat_interval") == 0) {
            config.heartbeat_interval = atoi(value);
        } else if (strcmp(key, "enable_fast_reconnect") == 0) {
            config.enable_fast_reconnect = atoi(value);
        } else if (strcmp(key, "keep_target_alive") == 0) {
            config.keep_target_alive = atoi(value);
        } else if (strcmp(key, "reconnect_delay") == 0) {
            config.reconnect_delay = atoi(value);
        } else if (strcmp(key, "max_reconnect_attempts") == 0) {
            config.max_reconnect_attempts = atoi(value);
        } else if (strcmp(key, "connection_pool_size") == 0) {
            config.connection_pool_size = atoi(value);
        } else {
            // 安全地记录未知配置键，避免格式字符串攻击
            if (key && strlen(key) > 0) {
                log_message(LOG_WARNING, "Unknown config key: %.*s", (int)strlen(key), key);
            } else {
                log_message(LOG_WARNING, "Unknown config key: (empty)");
            }
        }
    }

    fclose(fp);
    log_message(LOG_INFO, "Configuration loaded from %s", config_file);
    return 1;
}

// 初始化统计信息
void init_stats(void) {
    memset(&stats, 0, sizeof(stats));
    stats.start_time = time(NULL);
    stats.last_stats_time = stats.start_time;
}

// 更新统计信息
void update_stats(void) {
    stats.active_connections = 0;
    stats.total_bytes_sent = 0;
    stats.total_bytes_received = 0;

    for (int i = 0; i < connection_count; i++) {
        if (connections[i].is_active) {
            stats.active_connections++;
            stats.total_bytes_sent += connections[i].bytes_sent;
            stats.total_bytes_received += connections[i].bytes_received;
        }
    }
}

// 打印统计信息
void print_stats(void) {
    time_t now = time(NULL);
    time_t uptime = now - stats.start_time;

    update_stats();

    log_message(LOG_INFO, "=== RDP Forwarder Statistics ===");
    log_message(LOG_INFO, "Uptime: %ld seconds", uptime);
    log_message(LOG_INFO, "Total connections: %lu", stats.total_connections);
    log_message(LOG_INFO, "Active connections: %lu", stats.active_connections);
    log_message(LOG_INFO, "Total bytes sent: %lu", stats.total_bytes_sent);
    log_message(LOG_INFO, "Total bytes received: %lu", stats.total_bytes_received);
    log_message(LOG_INFO, "Average throughput: %.2f KB/s",
               uptime > 0 ? (stats.total_bytes_sent + stats.total_bytes_received) / 1024.0 / uptime : 0);

    stats.last_stats_time = now;
}

// 健康检查函数已移除 - 强制连接目标服务器

// 获取连接状态名称
const char* get_connection_state_name(connection_state_t state) {
    switch (state) {
        case CONN_STATE_INIT: return "INIT";
        case CONN_STATE_CONNECTING: return "CONNECTING";
        case CONN_STATE_CONNECTED: return "CONNECTED";
        case CONN_STATE_ACTIVE: return "ACTIVE";
        case CONN_STATE_CLIENT_DISCONNECTED: return "CLIENT_DISCONNECTED";
        case CONN_STATE_TARGET_DISCONNECTED: return "TARGET_DISCONNECTED";
        case CONN_STATE_RECONNECTING: return "RECONNECTING";
        case CONN_STATE_ERROR: return "ERROR";
        case CONN_STATE_CLOSING: return "CLOSING";
        default: return "UNKNOWN";
    }
}

// 设置连接状态
void set_connection_state(connection_pair_t* conn, connection_state_t new_state, const char* reason) {
    if (!conn) return;

    connection_state_t old_state = conn->state;
    if (old_state != new_state) {
        conn->state = new_state;
        conn->state_change_time = time(NULL);

        if (config.verbose_logging) {
            log_message(LOG_INFO, "Connection state changed: %s -> %s (%s)",
                       get_connection_state_name(old_state),
                       get_connection_state_name(new_state),
                       reason ? reason : "no reason");
        }

        // 记录错误状态的原因
        if (new_state == CONN_STATE_ERROR && reason) {
            strncpy(conn->last_error, reason, sizeof(conn->last_error) - 1);
            conn->last_error[sizeof(conn->last_error) - 1] = '\0';
            conn->error_count++;
        }
    }
}

// 记录连接状态变化
void log_connection_state_change(connection_pair_t* conn, int conn_index) {
    if (!conn) return;

    time_t now = time(NULL);
    time_t duration = now - conn->connection_start_time;
    time_t state_duration = now - conn->state_change_time;

    log_message(LOG_INFO, "Connection %d status: state=%s, duration=%lds, state_duration=%lds, errors=%d, sent=%lu, received=%lu",
               conn_index, get_connection_state_name(conn->state), duration, state_duration,
               conn->error_count, conn->bytes_sent, conn->bytes_received);

    if (conn->error_count > 0 && strlen(conn->last_error) > 0) {
        log_message(LOG_INFO, "Connection %d last error: %s", conn_index, conn->last_error);
    }
}

// 记录连接错误的详细信息
void log_connection_error(connection_pair_t* conn, int error_code, const char* context, int is_client_side) {
    const char* side = is_client_side ? "client" : "target";
    const char* error_desc = strerror(error_code);

    if (conn) {
        time_t connection_duration = time(NULL) - conn->connection_start_time;
        const char* transport_type = conn->use_hybrid_transport ? "hybrid" : "tcp";

        // 分析可能的断开原因
        const char* likely_reason = "";
        if (error_code == ECONNRESET) {
            likely_reason = " (connection forcibly closed by remote host)";
        } else if (error_code == ETIMEDOUT) {
            likely_reason = " (connection timed out)";
        } else if (error_code == ECONNREFUSED) {
            likely_reason = " (connection refused by remote host)";
        } else if (error_code == ENETUNREACH) {
            likely_reason = " (network unreachable)";
        } else if (error_code == EHOSTUNREACH) {
            likely_reason = " (host unreachable)";
        }

        log_message(LOG_ERR, "%s %s error: %s%s [state: %s, transport: %s, duration: %ld seconds, sent: %lu bytes, received: %lu bytes]",
                   context, side, error_desc, likely_reason, get_connection_state_name(conn->state), transport_type,
                   connection_duration, conn->bytes_sent, conn->bytes_received);

        // 更新连接状态
        char error_reason[512];
        snprintf(error_reason, sizeof(error_reason), "%s %s error: %s%s", context, side, error_desc, likely_reason);
        if (is_client_side) {
            set_connection_state(conn, CONN_STATE_CLIENT_DISCONNECTED, error_reason);
        } else {
            set_connection_state(conn, CONN_STATE_TARGET_DISCONNECTED, error_reason);
        }
    } else {
        log_message(LOG_ERR, "%s %s error: %s", context, side, error_desc);
    }
}

// 日志记录函数
void log_message(int priority, const char* format, ...) {
    va_list args;
    va_start(args, format);

    if (config.verbose_logging) {
        char timestamp[64];
        char message[1024];
        time_t now = time(NULL);
        struct tm* tm_info = localtime(&now);
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", tm_info);

        // 使用vsnprintf安全地格式化消息
        vsnprintf(message, sizeof(message), format, args);
        printf("[%s] %s\n", timestamp, message);
        fflush(stdout);

        // 重新开始va_list用于syslog
        va_end(args);
        va_start(args, format);
    }

    // 使用vsyslog
    vsyslog(priority, format, args);
    va_end(args);
}

// 设置socket为非阻塞模式
int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 为客户端和目标端 TCP socket 设置通用参数，尽量减少因为 TCP 行为差异导致的 RDP 断连
static void configure_tcp_socket(int fd) {
    if (fd <= 0) {
        return;
    }

    int opt = 1;

    // RDP 对交互延迟比较敏感，禁用 Nagle 算法可以降低小包延迟
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        log_message(LOG_WARNING, "Failed to set TCP_NODELAY on socket %d: %s", fd, strerror(errno));
    }

    // 开启 TCP KeepAlive，帮助穿越某些对长连接不友好的中间设备
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt)) < 0) {
        log_message(LOG_WARNING, "Failed to set SO_KEEPALIVE on socket %d: %s", fd, strerror(errno));
    }

    // 如配置了 socket_timeout，则为收发都设置超时，避免在异常情况下无限阻塞
    if (config.socket_timeout > 0) {
        struct timeval tv;
        tv.tv_sec = config.socket_timeout;
        tv.tv_usec = 0;

        if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            log_message(LOG_WARNING, "Failed to set SO_RCVTIMEO on socket %d: %s", fd, strerror(errno));
        }
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
            log_message(LOG_WARNING, "Failed to set SO_SNDTIMEO on socket %d: %s", fd, strerror(errno));
        }
    }
}

// 清理连接
void cleanup_connection(int index) {
    if (index < 0 || index >= connection_count) {
        return;
    }

    connection_pair_t* conn = &connections[index];

    // 记录连接状态和统计信息
    set_connection_state(conn, CONN_STATE_CLOSING, "connection cleanup");
    log_connection_state_change(conn, index);

    log_message(LOG_INFO, "Cleaning up connection %d (sent: %lu bytes, received: %lu bytes)",
                index, conn->bytes_sent, conn->bytes_received);

    if (conn->client_fd > 0) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }

    if (conn->target_fd > 0) {
        close(conn->target_fd);
        conn->target_fd = -1;
    }

    // 清理混合传输连接
    if (conn->ht_conn) {
        ht_disconnect(conn->ht_conn);
        ht_destroy_connection(conn->ht_conn);
        conn->ht_conn = NULL;
    }

    conn->is_active = 0;
    conn->use_hybrid_transport = 0;

    // 健康状态重置逻辑已移除 - 不再进行健康检查

    // 移动后面的连接向前填补空隙
    for (int i = index; i < connection_count - 1; i++) {
        connections[i] = connections[i + 1];
    }
    connection_count--;
}

// 创建监听socket
int create_listen_socket(int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    
    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }
    
    if (listen(sockfd, 5) < 0) {
        perror("listen");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// 连接到目标主机
int connect_to_target(const char* target_ip, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, target_ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }
    
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// 改进的数据转发函数
int forward_data(int from_fd, int to_fd, connection_pair_t* conn, int is_client_to_target) {
    char* buffer = malloc(config.buffer_size);
    if (!buffer) {
        log_message(LOG_ERR, "Failed to allocate buffer");
        return -1;
    }
    ssize_t bytes_read = recv(from_fd, buffer, config.buffer_size, 0);

    if (bytes_read < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            free(buffer);
            return 0; // 非阻塞模式下没有数据可读
        }
        log_connection_error(conn, errno, "recv", is_client_to_target);
        free(buffer);
        return -1; // 连接错误
    }

    if (bytes_read == 0) {
        log_message(LOG_INFO, "Connection closed by %s",
                   is_client_to_target ? "client" : "target");
        free(buffer);

        // 如果是客户端断开且启用了快速重连，特殊处理
        // 但要确保连接已经建立一段时间，避免在RDP握手阶段误判
        if (is_client_to_target && config.enable_fast_reconnect &&
            conn && (time(NULL) - conn->last_activity > 5)) {
            return -2; // 特殊返回值表示客户端断开
        }

        return -1; // 连接关闭
    }

    ssize_t bytes_sent = 0;
    while (bytes_sent < bytes_read) {
        ssize_t sent = send(to_fd, buffer + bytes_sent, bytes_read - bytes_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // 目标socket缓冲区满，稍后重试
                usleep(1000); // 等待1ms
                continue;
            }
            log_connection_error(conn, errno, "send", !is_client_to_target);
            free(buffer);
            return -1;
        }
        if (sent == 0) {
            log_message(LOG_WARNING, "send returned 0, connection may be closed");
            free(buffer);
            return -1;
        }
        bytes_sent += sent;
    }

    // 更新统计信息
    if (is_client_to_target) {
        conn->bytes_sent += bytes_read;
    } else {
        conn->bytes_received += bytes_read;
    }

    conn->last_activity = time(NULL);

    // 如果这是第一次数据传输，更新状态为活跃
    if (conn->state == CONN_STATE_CONNECTED) {
        set_connection_state(conn, CONN_STATE_ACTIVE, "data transfer started");
    }

    free(buffer);
    return bytes_read;
}

// 检查客户端socket是否还活着
int is_client_socket_alive(int fd) {
    if (fd <= 0) {
        return 0;
    }

    // 使用MSG_PEEK标志检查socket状态，不会消费数据
    char test_byte;
    int result = recv(fd, &test_byte, 1, MSG_PEEK | MSG_DONTWAIT);

    if (result == 0) {
        // 连接已关闭
        return 0;
    } else if (result < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // 没有数据可读，但连接正常
            return 1;
        } else {
            // 连接错误
            return 0;
        }
    }

    // 有数据可读，连接正常
    return 1;
}

// 处理客户端断开连接
void handle_client_disconnect(connection_pair_t* conn) {
    if (!conn) {
        return;
    }

    log_message(LOG_INFO, "Client disconnected, preparing for fast reconnect");

    // 关闭客户端socket
    if (conn->client_fd > 0) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }

    // 标记客户端已断开
    conn->client_disconnected = 1;
    conn->disconnect_time = time(NULL);
    conn->reconnect_attempts = 0;

    // 如果启用了保持目标连接活跃，则不关闭目标连接
    if (config.keep_target_alive) {
        log_message(LOG_INFO, "Keeping target connection alive for fast reconnect");
        conn->target_ready = 1;
    } else {
        // 关闭目标连接
        if (conn->target_fd > 0) {
            close(conn->target_fd);
            conn->target_fd = -1;
        }

        if (conn->ht_conn) {
            ht_disconnect(conn->ht_conn);
            ht_destroy_connection(conn->ht_conn);
            conn->ht_conn = NULL;
        }

        conn->target_ready = 0;
        conn->use_hybrid_transport = 0;
    }
}

// 重置连接以供重用
void reset_connection_for_reuse(connection_pair_t* conn) {
    if (!conn) {
        return;
    }

    conn->client_disconnected = 0;
    conn->target_ready = 0;
    conn->disconnect_time = 0;
    conn->reconnect_attempts = 0;
    conn->last_activity = time(NULL);
    conn->bytes_sent = 0;
    conn->bytes_received = 0;
}

// 创建混合传输连接
int create_hybrid_connection(connection_pair_t* conn, const char* target_ip, int port) {
    if (!conn) {
        return -1;
    }

    // 创建混合传输连接
    conn->ht_conn = ht_create_connection(target_ip, port, config.transport_mode);
    if (!conn->ht_conn) {
        log_message(LOG_ERR, "Failed to create hybrid transport connection");
        return -1;
    }

    // 配置混合传输参数
    conn->ht_conn->udp_preference = config.udp_preference;
    conn->ht_conn->retransmit_timeout = config.retransmit_timeout;
    conn->ht_conn->max_retransmit = config.max_retransmit;

    // 建立连接
    if (ht_connect(conn->ht_conn) < 0) {
        log_message(LOG_ERR, "Failed to connect via hybrid transport");
        ht_destroy_connection(conn->ht_conn);
        conn->ht_conn = NULL;
        return -1;
    }

    conn->use_hybrid_transport = 1;
    log_message(LOG_INFO, "Hybrid transport connection established to %s:%d", target_ip, port);

    return 0;
}

// 混合传输数据转发
int forward_data_hybrid(connection_pair_t* conn, int from_client) {
    if (!conn || !conn->ht_conn || !conn->use_hybrid_transport) {
        return -1;
    }

    char buffer[8192];
    int bytes_transferred = 0;

    if (from_client) {
        // 从客户端读取数据，通过混合传输发送到目标
        ssize_t bytes_read = recv(conn->client_fd, buffer, sizeof(buffer), 0);
        if (bytes_read <= 0) {
            if (bytes_read == 0) {
                log_message(LOG_INFO, "Client connection closed");
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_connection_error(conn, errno, "recv", 1);
            }
            return bytes_read;
        }

        // 通过混合传输发送数据
        int sent = ht_send_data(conn->ht_conn, buffer, bytes_read);
        if (sent > 0) {
            conn->bytes_sent += sent;
            bytes_transferred = sent;
        }
    } else {
        // 从混合传输接收数据，发送到客户端
        ssize_t bytes_read = ht_recv_data(conn->ht_conn, buffer, sizeof(buffer));
        if (bytes_read > 0) {
            ssize_t bytes_sent = 0;
            while (bytes_sent < bytes_read) {
                ssize_t sent = send(conn->client_fd, buffer + bytes_sent,
                                  bytes_read - bytes_sent, MSG_NOSIGNAL);
                if (sent <= 0) {
                    if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        log_connection_error(conn, errno, "send", 1);
                        return -1;
                    }
                    break;
                }
                bytes_sent += sent;
            }

            if (bytes_sent > 0) {
                conn->bytes_received += bytes_sent;
                bytes_transferred = bytes_sent;
            }
        }
    }

    if (bytes_transferred > 0) {
        conn->last_activity = time(NULL);
    }

    // 处理混合传输事件
    ht_process_events(conn->ht_conn);
    ht_handle_timeout(conn->ht_conn);

    return bytes_transferred;
}

// 尝试重连目标
int try_reconnect_target(connection_pair_t* conn) {
    if (!conn || !conn->client_disconnected) {
        return -1;
    }

    // 检查是否超过最大重试次数
    if (conn->reconnect_attempts >= config.max_reconnect_attempts) {
        log_message(LOG_WARNING, "Max reconnect attempts reached, giving up");
        return -1;
    }

    conn->reconnect_attempts++;

    // 如果目标连接还活着，直接返回成功
    if (conn->target_ready &&
        ((conn->target_fd > 0) || (conn->ht_conn && conn->use_hybrid_transport))) {
        log_message(LOG_INFO, "Target connection still alive, ready for new client");
        return 0;
    }

    // 重新建立目标连接
    log_message(LOG_INFO, "Reconnecting to target (attempt %d/%d)",
               conn->reconnect_attempts, config.max_reconnect_attempts);

    int connection_success = 0;

    // 根据配置选择传输模式
    if (config.transport_mode != HT_MODE_TCP_ONLY) {
        // 尝试创建混合传输连接
        if (create_hybrid_connection(conn, config.target_ip, config.target_port) == 0) {
            connection_success = 1;
            conn->target_ready = 1;
        }
    }

    // 如果混合传输失败，回退到传统TCP
	    if (!connection_success) {
	        int target_fd = connect_to_target(config.target_ip, config.target_port);
	        if (target_fd >= 0) {
	            // 设置目标socket为非阻塞模式并调整TCP参数
	            if (set_nonblocking(target_fd) < 0) {
	                log_message(LOG_WARNING, "Failed to set target socket non-blocking");
	            }
	            configure_tcp_socket(target_fd);

	            conn->target_fd = target_fd;
	            conn->target_ready = 1;
	            connection_success = 1;
	            log_message(LOG_INFO, "Target reconnected using TCP");
	        }
	    }

    if (connection_success) {
        log_message(LOG_INFO, "Target reconnection successful");
        return 0;
    } else {
        log_message(LOG_ERR, "Target reconnection failed");
        return -1;
    }
}

int main(int argc, char *argv[]) {
    // 初始化默认配置
    init_config();

    // 早期初始化syslog，以便在配置加载时使用
    openlog("rdp_forwarder", LOG_PID | LOG_CONS, LOG_DAEMON);

    // 初始化混合传输协议
    if (ht_init() < 0) {
        fprintf(stderr, "Failed to initialize hybrid transport\n");
        exit(1);
    }

    // 加载配置文件
    const char* config_file = CONFIG_FILE;
    if (argc > 1 && strcmp(argv[1], "-c") == 0 && argc > 2) {
        config_file = argv[2];
    } else if (argc == 2) {
        // 兼容旧版本：直接指定目标IP
        strcpy(config.target_ip, argv[1]);
    }

    load_config(config_file);

    // 分配连接数组
    connections = malloc(config.max_clients * sizeof(connection_pair_t));
    if (!connections) {
        fprintf(stderr, "Failed to allocate memory for connections\n");
        exit(1);
    }

    // 设置信号处理
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);

    // 初始化统计
    init_stats();

    int listen_fd = create_listen_socket(config.listen_port);
    if (listen_fd < 0) {
        exit(1);
    }

    log_message(LOG_INFO, "RDP Forwarder started, listening on port %d, forwarding to %s:%d",
               config.listen_port, config.target_ip, config.target_port);

    while (running) {
        // 定期打印统计信息和连接状态
        if (config.enable_stats) {
            time_t now = time(NULL);
            if (now - stats.last_stats_time >= config.stats_interval) {
                print_stats();

                // 如果启用详细日志，也打印连接状态
                if (config.verbose_logging && connection_count > 0) {
                    log_message(LOG_INFO, "=== Connection Status Report ===");
                    for (int i = 0; i < connection_count; i++) {
                        log_connection_state_change(&connections[i], i);
                    }
                }
            }
        }

        // 健康检查已移除 - 强制连接目标服务器

        // 处理断开连接的快速重连
        if (config.enable_fast_reconnect) {
            for (int i = 0; i < connection_count; i++) {
                if (connections[i].client_disconnected && !connections[i].target_ready) {
                    // 尝试重连目标
                    time_t now = time(NULL);
                    if (now - connections[i].disconnect_time >= config.reconnect_delay / 1000) {
                        try_reconnect_target(&connections[i]);
                    }
                }
            }
        }

        fd_set readfds;
        int max_fd = listen_fd;
        
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        
        // 添加所有活跃连接到select
        for (int i = 0; i < connection_count; i++) {
            if (connections[i].client_fd > 0) {
                FD_SET(connections[i].client_fd, &readfds);
                max_fd = (connections[i].client_fd > max_fd) ? connections[i].client_fd : max_fd;
            }
            if (connections[i].target_fd > 0) {
                FD_SET(connections[i].target_fd, &readfds);
                max_fd = (connections[i].target_fd > max_fd) ? connections[i].target_fd : max_fd;
            }
        }
        
        int activity = select(max_fd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) {
            perror("select");
            continue;
        }
        
        // 处理新连接
        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);

            if (client_fd >= 0) {
                // 首先检查是否有可重用的连接（快速重连）
                int reused_connection = -1;
                if (config.enable_fast_reconnect) {
                    for (int i = 0; i < connection_count; i++) {
                        if (connections[i].client_disconnected && connections[i].target_ready) {
                            reused_connection = i;
                            break;
                        }
                    }
                }

                if (reused_connection >= 0) {
                    // 重用现有连接
                    log_message(LOG_INFO, "Reusing connection %d for fast reconnect", reused_connection);

	            // 设置客户端socket为非阻塞模式并调整TCP参数
	            if (set_nonblocking(client_fd) < 0) {
	                log_message(LOG_WARNING, "Failed to set client socket non-blocking");
	            }
	            configure_tcp_socket(client_fd);

                    connections[reused_connection].client_fd = client_fd;
                    reset_connection_for_reuse(&connections[reused_connection]);

                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

                    log_message(LOG_INFO, "Fast reconnect successful: %s:%d -> %s:%d",
                               client_ip, ntohs(client_addr.sin_port),
                               config.target_ip, config.target_port);

                } else if (connection_count < config.max_clients) {
                // 健康检查已移除 - 强制连接目标服务器
                log_message(LOG_INFO, "Accepting new connection - will attempt to connect to target");

	                // 设置客户端socket为非阻塞模式并调整TCP参数
	                if (set_nonblocking(client_fd) < 0) {
	                    log_message(LOG_WARNING, "Failed to set client socket non-blocking");
	                }
	                configure_tcp_socket(client_fd);

                // 初始化连接结构
                memset(&connections[connection_count], 0, sizeof(connection_pair_t));
                connections[connection_count].client_fd = client_fd;
                connections[connection_count].target_fd = -1;
                strcpy(connections[connection_count].target_ip, config.target_ip);
                connections[connection_count].last_activity = time(NULL);
                connections[connection_count].connection_start_time = time(NULL);
                connections[connection_count].is_active = 1;
                connections[connection_count].bytes_sent = 0;
                connections[connection_count].bytes_received = 0;
                connections[connection_count].ht_conn = NULL;
                connections[connection_count].use_hybrid_transport = 0;

                // 设置初始状态
                set_connection_state(&connections[connection_count], CONN_STATE_CONNECTING, "new client connection");

                // 初始化快速重连状态
                connections[connection_count].client_disconnected = 0;
                connections[connection_count].target_ready = 0;
                connections[connection_count].disconnect_time = 0;
                connections[connection_count].reconnect_attempts = 0;

                int connection_success = 0;

                // 根据配置选择传输模式
                // 暂时禁用混合传输，确保RDP协议兼容性
                if (0 && config.transport_mode != HT_MODE_TCP_ONLY) {
                    // 尝试创建混合传输连接
                    if (create_hybrid_connection(&connections[connection_count],
                                               config.target_ip, config.target_port) == 0) {
                        connection_success = 1;
                    }
                }

                // 如果混合传输失败，回退到传统TCP
	                if (!connection_success) {
	                    int target_fd = connect_to_target(config.target_ip, config.target_port);
	                    if (target_fd >= 0) {
	                        // 设置目标socket为非阻塞模式并调整TCP参数
	                        if (set_nonblocking(target_fd) < 0) {
	                            log_message(LOG_WARNING, "Failed to set target socket non-blocking");
	                        }
	                        configure_tcp_socket(target_fd);

	                        connections[connection_count].target_fd = target_fd;
	                        connection_success = 1;
	                        log_message(LOG_INFO, "Using traditional TCP transport");
	                    }
	                }

                if (connection_success) {
                    char client_ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

                    const char* transport_type = connections[connection_count].use_hybrid_transport ?
                                               "hybrid" : "tcp";

                    log_message(LOG_INFO, "New connection %d established (%s): %s:%d -> %s:%d",
                               connection_count, transport_type, client_ip, ntohs(client_addr.sin_port),
                               config.target_ip, config.target_port);

                    // 更新连接状态为已连接
                    set_connection_state(&connections[connection_count], CONN_STATE_CONNECTED, "target connection established");

                    connection_count++;
                    stats.total_connections++;
                } else {
                    log_message(LOG_ERR, "Failed to connect to target %s:%d", config.target_ip, config.target_port);
                    close(client_fd);
                }
                } else if (client_fd >= 0) {
                log_message(LOG_WARNING, "Maximum connections reached, rejecting new connection");
                close(client_fd);
            }
            }
        }

        // 处理数据转发
        for (int i = 0; i < connection_count; i++) {
            if (!connections[i].is_active) {
                continue;
            }

            // 检查连接超时
            time_t now = time(NULL);
            if (now - connections[i].last_activity > config.connection_timeout) {
                log_message(LOG_INFO, "Connection %d timed out", i);
                cleanup_connection(i);
                i--; // 因为cleanup_connection会移动数组元素
                continue;
            }

            int connection_error = 0;

            if (connections[i].use_hybrid_transport) {
                // 使用混合传输模式

                // 客户端到目标的数据转发
                if (connections[i].client_fd > 0 && FD_ISSET(connections[i].client_fd, &readfds)) {
                    int result = forward_data_hybrid(&connections[i], 1);
                    if (result < 0) {
                        connection_error = 1;
                    }
                }

                // 混合传输到客户端的数据转发（定期检查）
                if (!connection_error) {
                    int result = forward_data_hybrid(&connections[i], 0);
                    if (result < 0) {
                        connection_error = 1;
                    }
                }
            } else {
                // 使用传统TCP模式

                // 客户端到目标的数据转发
                if (connections[i].client_fd > 0 && FD_ISSET(connections[i].client_fd, &readfds)) {
                    int result = forward_data(connections[i].client_fd, connections[i].target_fd,
                                            &connections[i], 1);
                    if (result == -2 && config.enable_fast_reconnect) {
                        // 客户端断开，启用快速重连
                        log_message(LOG_INFO, "Client disconnected, enabling fast reconnect for connection %d", i);
                        handle_client_disconnect(&connections[i]);
                        continue; // 不标记为连接错误，保持目标连接
                    } else if (result < 0) {
                        connection_error = 1;
                    }
                }

                // 目标到客户端的数据转发
                if (!connection_error && connections[i].target_fd > 0 &&
                    FD_ISSET(connections[i].target_fd, &readfds)) {
                    int result = forward_data(connections[i].target_fd, connections[i].client_fd,
                                            &connections[i], 0);
                    if (result < 0) {
                        connection_error = 1;
                    }
                }
            }

            // 如果有连接错误，清理连接
            if (connection_error) {
                cleanup_connection(i);
                i--; // 因为cleanup_connection会移动数组元素
            }
        }
    }

    // 清理所有连接
    log_message(LOG_INFO, "Cleaning up %d active connections...", connection_count);
    for (int i = connection_count - 1; i >= 0; i--) {
        cleanup_connection(i);
    }

    close(listen_fd);
    free(connections);

    // 清理混合传输协议
    ht_cleanup();

    log_message(LOG_INFO, "RDP Forwarder shutdown complete");
    closelog();
    return 0;
}