#ifndef HYBRID_TRANSPORT_H
#define HYBRID_TRANSPORT_H

#include <stdint.h>
#include <sys/time.h>
#include <netinet/in.h>

// 协议常量
#define HT_MAX_PACKET_SIZE 1400        // 最大UDP包大小（避免分片）
#define HT_MAX_PAYLOAD_SIZE 1350       // 最大载荷大小
#define HT_HEADER_SIZE 50               // 协议头大小
#define HT_MAX_SEQUENCE 0xFFFFFFFF      // 最大序列号
#define HT_RETRANSMIT_TIMEOUT 100       // 重传超时(ms)
#define HT_MAX_RETRANSMIT 3             // 最大重传次数
#define HT_WINDOW_SIZE 64               // 滑动窗口大小
#define HT_HEARTBEAT_INTERVAL 1000      // 心跳间隔(ms)

// 数据包类型
typedef enum {
    HT_TYPE_DATA = 1,           // 数据包
    HT_TYPE_ACK = 2,            // 确认包
    HT_TYPE_NACK = 3,           // 否定确认包
    HT_TYPE_HEARTBEAT = 4,      // 心跳包
    HT_TYPE_CONTROL = 5,        // 控制包
    HT_TYPE_RETRANSMIT = 6      // 重传请求包
} ht_packet_type_t;

// 传输模式
typedef enum {
    HT_MODE_UDP_ONLY = 1,       // 仅UDP模式
    HT_MODE_TCP_ONLY = 2,       // 仅TCP模式
    HT_MODE_HYBRID = 3,         // 混合模式（默认）
    HT_MODE_AUTO = 4            // 自动模式
} ht_transport_mode_t;

// 数据包头结构
typedef struct {
    uint32_t magic;             // 魔数标识
    uint8_t version;            // 协议版本
    uint8_t type;               // 包类型
    uint16_t flags;             // 标志位
    uint32_t sequence;          // 序列号
    uint32_t ack_sequence;      // 确认序列号
    uint16_t window_size;       // 窗口大小
    uint16_t payload_size;      // 载荷大小
    uint32_t timestamp;         // 时间戳
    uint32_t checksum;          // 校验和
    uint8_t reserved[8];        // 保留字段
} __attribute__((packed)) ht_packet_header_t;

// 数据包结构
typedef struct {
    ht_packet_header_t header;
    uint8_t payload[HT_MAX_PAYLOAD_SIZE];
} ht_packet_t;

// 发送缓冲区条目
typedef struct ht_send_buffer_entry {
    ht_packet_t packet;
    struct timeval send_time;
    int retransmit_count;
    struct ht_send_buffer_entry* next;
} ht_send_buffer_entry_t;

// 接收缓冲区条目
typedef struct ht_recv_buffer_entry {
    ht_packet_t packet;
    struct timeval recv_time;
    int received;
    struct ht_recv_buffer_entry* next;
} ht_recv_buffer_entry_t;

// 连接统计信息
typedef struct {
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_lost;
    uint64_t packets_retransmitted;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint32_t rtt_avg;           // 平均往返时间(ms)
    uint32_t rtt_min;           // 最小往返时间(ms)
    uint32_t rtt_max;           // 最大往返时间(ms)
    float packet_loss_rate;     // 丢包率
    float udp_ratio;            // UDP传输比例
    float tcp_ratio;            // TCP传输比例
} ht_connection_stats_t;

// 混合传输连接结构
typedef struct {
    // 基本信息
    int udp_fd;                 // UDP socket
    int tcp_fd;                 // TCP socket
    struct sockaddr_in remote_addr; // 远程地址
    ht_transport_mode_t mode;   // 传输模式
    
    // 序列号管理
    uint32_t send_sequence;     // 发送序列号
    uint32_t recv_sequence;     // 接收序列号
    uint32_t ack_sequence;      // 确认序列号
    
    // 缓冲区管理
    ht_send_buffer_entry_t* send_buffer;   // 发送缓冲区
    ht_recv_buffer_entry_t* recv_buffer;   // 接收缓冲区
    uint16_t send_window_size;  // 发送窗口大小
    uint16_t recv_window_size;  // 接收窗口大小
    
    // 时间管理
    struct timeval last_heartbeat;  // 最后心跳时间
    struct timeval last_activity;   // 最后活动时间
    
    // 统计信息
    ht_connection_stats_t stats;
    
    // 状态标志
    int is_connected;
    int is_closing;
    
    // 配置参数
    int retransmit_timeout;     // 重传超时时间
    int max_retransmit;         // 最大重传次数
    float udp_preference;       // UDP偏好度(0.0-1.0)
} ht_connection_t;

// 函数声明
int ht_init(void);
void ht_cleanup(void);

ht_connection_t* ht_create_connection(const char* remote_ip, int remote_port, ht_transport_mode_t mode);
void ht_destroy_connection(ht_connection_t* conn);

int ht_connect(ht_connection_t* conn);
int ht_disconnect(ht_connection_t* conn);

int ht_send_data(ht_connection_t* conn, const void* data, size_t size);
int ht_recv_data(ht_connection_t* conn, void* buffer, size_t buffer_size);

int ht_process_events(ht_connection_t* conn);
int ht_handle_timeout(ht_connection_t* conn);

void ht_get_stats(ht_connection_t* conn, ht_connection_stats_t* stats);
void ht_reset_stats(ht_connection_t* conn);

// 内部函数
uint32_t ht_calculate_checksum(const void* data, size_t size);
int ht_send_packet(ht_connection_t* conn, ht_packet_t* packet, int use_tcp);
int ht_recv_packet(ht_connection_t* conn, ht_packet_t* packet, int* from_tcp);
void ht_update_rtt(ht_connection_t* conn, uint32_t rtt);
int ht_should_use_tcp(ht_connection_t* conn);

#endif // HYBRID_TRANSPORT_H
