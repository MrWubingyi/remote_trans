#include "hybrid_transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <time.h>
#include <netinet/tcp.h>

// 协议魔数
#define HT_MAGIC 0x48545250  // "HTRP" - Hybrid Transport Protocol

// 全局变量
static int ht_initialized = 0;

// 工具函数：获取当前时间戳(毫秒)
static uint32_t get_timestamp_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

// 工具函数：计算时间差(毫秒)
static uint32_t time_diff_ms(struct timeval* start, struct timeval* end) {
    return (uint32_t)((end->tv_sec - start->tv_sec) * 1000 + 
                      (end->tv_usec - start->tv_usec) / 1000);
}

// 初始化混合传输协议
int ht_init(void) {
    if (ht_initialized) {
        return 0;
    }
    
    // 初始化随机数种子
    srand(time(NULL));
    
    ht_initialized = 1;
    return 0;
}

// 清理混合传输协议
void ht_cleanup(void) {
    ht_initialized = 0;
}

// 计算校验和
uint32_t ht_calculate_checksum(const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t checksum = 0;
    
    for (size_t i = 0; i < size; i++) {
        checksum += bytes[i];
        checksum = (checksum << 1) | (checksum >> 31); // 循环左移
    }
    
    return checksum;
}

// 设置socket为非阻塞模式
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// 创建UDP socket
static int create_udp_socket(void) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("UDP socket creation failed");
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // 设置为非阻塞模式
    if (set_nonblocking(sockfd) < 0) {
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// 创建TCP socket
static int create_tcp_socket(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("TCP socket creation failed");
        return -1;
    }
    
    // 设置socket选项
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    // 设置为非阻塞模式
    if (set_nonblocking(sockfd) < 0) {
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

// 创建混合传输连接
ht_connection_t* ht_create_connection(const char* remote_ip, int remote_port, ht_transport_mode_t mode) {
    if (!ht_initialized) {
        ht_init();
    }
    
    ht_connection_t* conn = calloc(1, sizeof(ht_connection_t));
    if (!conn) {
        return NULL;
    }
    
    // 初始化基本信息
    conn->mode = mode;
    conn->udp_fd = -1;
    conn->tcp_fd = -1;
    
    // 设置远程地址
    memset(&conn->remote_addr, 0, sizeof(conn->remote_addr));
    conn->remote_addr.sin_family = AF_INET;
    conn->remote_addr.sin_port = htons(remote_port);
    if (inet_pton(AF_INET, remote_ip, &conn->remote_addr.sin_addr) <= 0) {
        free(conn);
        return NULL;
    }
    
    // 初始化序列号
    conn->send_sequence = rand() % HT_MAX_SEQUENCE;
    conn->recv_sequence = 0;
    conn->ack_sequence = 0;
    
    // 初始化窗口大小
    conn->send_window_size = HT_WINDOW_SIZE;
    conn->recv_window_size = HT_WINDOW_SIZE;
    
    // 初始化配置参数
    conn->retransmit_timeout = HT_RETRANSMIT_TIMEOUT;
    conn->max_retransmit = HT_MAX_RETRANSMIT;
    conn->udp_preference = 0.8f; // 默认80%使用UDP
    
    // 初始化统计信息
    memset(&conn->stats, 0, sizeof(conn->stats));
    conn->stats.rtt_min = UINT32_MAX;
    
    // 获取当前时间
    gettimeofday(&conn->last_activity, NULL);
    conn->last_heartbeat = conn->last_activity;
    
    return conn;
}

// 销毁混合传输连接
void ht_destroy_connection(ht_connection_t* conn) {
    if (!conn) {
        return;
    }
    
    // 关闭socket
    if (conn->udp_fd >= 0) {
        close(conn->udp_fd);
    }
    if (conn->tcp_fd >= 0) {
        close(conn->tcp_fd);
    }
    
    // 清理发送缓冲区
    ht_send_buffer_entry_t* send_entry = conn->send_buffer;
    while (send_entry) {
        ht_send_buffer_entry_t* next = send_entry->next;
        free(send_entry);
        send_entry = next;
    }
    
    // 清理接收缓冲区
    ht_recv_buffer_entry_t* recv_entry = conn->recv_buffer;
    while (recv_entry) {
        ht_recv_buffer_entry_t* next = recv_entry->next;
        free(recv_entry);
        recv_entry = next;
    }
    
    free(conn);
}

// 连接到远程主机
int ht_connect(ht_connection_t* conn) {
    if (!conn || conn->is_connected) {
        return -1;
    }
    
    // 创建UDP socket
    if (conn->mode != HT_MODE_TCP_ONLY) {
        conn->udp_fd = create_udp_socket();
        if (conn->udp_fd < 0) {
            return -1;
        }
    }
    
    // 创建TCP socket
    if (conn->mode != HT_MODE_UDP_ONLY) {
        conn->tcp_fd = create_tcp_socket();
        if (conn->tcp_fd < 0) {
            if (conn->udp_fd >= 0) {
                close(conn->udp_fd);
                conn->udp_fd = -1;
            }
            return -1;
        }
        
        // 尝试TCP连接
        int result = connect(conn->tcp_fd, (struct sockaddr*)&conn->remote_addr, 
                           sizeof(conn->remote_addr));
        if (result < 0 && errno != EINPROGRESS) {
            close(conn->tcp_fd);
            conn->tcp_fd = -1;
            if (conn->mode == HT_MODE_TCP_ONLY) {
                if (conn->udp_fd >= 0) {
                    close(conn->udp_fd);
                    conn->udp_fd = -1;
                }
                return -1;
            }
        }
    }
    
    conn->is_connected = 1;
    gettimeofday(&conn->last_activity, NULL);
    
    return 0;
}

// 断开连接
int ht_disconnect(ht_connection_t* conn) {
    if (!conn || !conn->is_connected) {
        return -1;
    }
    
    conn->is_closing = 1;
    
    // 发送关闭通知包
    ht_packet_t close_packet;
    memset(&close_packet, 0, sizeof(close_packet));
    close_packet.header.magic = HT_MAGIC;
    close_packet.header.version = 1;
    close_packet.header.type = HT_TYPE_CONTROL;
    close_packet.header.flags = 0x01; // 关闭标志
    close_packet.header.sequence = conn->send_sequence++;
    close_packet.header.timestamp = get_timestamp_ms();
    
    // 尝试通过两个通道发送关闭包
    if (conn->udp_fd >= 0) {
        ht_send_packet(conn, &close_packet, 0);
    }
    if (conn->tcp_fd >= 0) {
        ht_send_packet(conn, &close_packet, 1);
    }
    
    conn->is_connected = 0;
    return 0;
}

// 获取连接统计信息
void ht_get_stats(ht_connection_t* conn, ht_connection_stats_t* stats) {
    if (!conn || !stats) {
        return;
    }
    
    *stats = conn->stats;
    
    // 计算传输比例
    uint64_t total_packets = stats->packets_sent;
    if (total_packets > 0) {
        // 这里需要根据实际发送记录计算，暂时使用估算值
        stats->udp_ratio = conn->udp_preference;
        stats->tcp_ratio = 1.0f - conn->udp_preference;
    }
    
    // 计算丢包率
    if (stats->packets_sent > 0) {
        stats->packet_loss_rate = (float)stats->packets_lost / stats->packets_sent;
    }
}

// 重置统计信息
void ht_reset_stats(ht_connection_t* conn) {
    if (!conn) {
        return;
    }

    memset(&conn->stats, 0, sizeof(conn->stats));
    conn->stats.rtt_min = UINT32_MAX;
}

// 判断是否应该使用TCP传输
int ht_should_use_tcp(ht_connection_t* conn) {
    if (!conn) {
        return 0;
    }

    // 如果只有TCP模式，总是使用TCP
    if (conn->mode == HT_MODE_TCP_ONLY) {
        return 1;
    }

    // 如果只有UDP模式，总是使用UDP
    if (conn->mode == HT_MODE_UDP_ONLY) {
        return 0;
    }

    // 混合模式或自动模式的决策逻辑
    float tcp_probability = 1.0f - conn->udp_preference;

    // 根据网络状况调整TCP使用概率
    if (conn->stats.packet_loss_rate > 0.05f) { // 丢包率超过5%
        tcp_probability += 0.3f;
    }

    if (conn->stats.rtt_avg > 200) { // 延迟超过200ms
        tcp_probability += 0.2f;
    }

    // 限制概率范围
    if (tcp_probability > 1.0f) tcp_probability = 1.0f;
    if (tcp_probability < 0.0f) tcp_probability = 0.0f;

    // 随机决策
    return (rand() / (float)RAND_MAX) < tcp_probability;
}

// 发送数据包
int ht_send_packet(ht_connection_t* conn, ht_packet_t* packet, int use_tcp) {
    if (!conn || !packet) {
        return -1;
    }

    // 计算校验和
    packet->header.checksum = 0;
    packet->header.checksum = ht_calculate_checksum(packet,
        sizeof(ht_packet_header_t) + packet->header.payload_size);

    int bytes_sent = 0;

    if (use_tcp && conn->tcp_fd >= 0) {
        // TCP发送
        size_t total_size = sizeof(ht_packet_header_t) + packet->header.payload_size;
        bytes_sent = send(conn->tcp_fd, packet, total_size, MSG_NOSIGNAL);
    } else if (!use_tcp && conn->udp_fd >= 0) {
        // UDP发送
        size_t total_size = sizeof(ht_packet_header_t) + packet->header.payload_size;
        bytes_sent = sendto(conn->udp_fd, packet, total_size, 0,
                           (struct sockaddr*)&conn->remote_addr, sizeof(conn->remote_addr));
    } else {
        return -1;
    }

    if (bytes_sent > 0) {
        conn->stats.packets_sent++;
        conn->stats.bytes_sent += bytes_sent;
        gettimeofday(&conn->last_activity, NULL);
    }

    return bytes_sent;
}

// 接收数据包
int ht_recv_packet(ht_connection_t* conn, ht_packet_t* packet, int* from_tcp) {
    if (!conn || !packet || !from_tcp) {
        return -1;
    }

    int bytes_received = 0;
    *from_tcp = 0;

    // 首先尝试从UDP接收
    if (conn->udp_fd >= 0) {
        struct sockaddr_in sender_addr;
        socklen_t addr_len = sizeof(sender_addr);

        bytes_received = recvfrom(conn->udp_fd, packet, sizeof(ht_packet_t), 0,
                                 (struct sockaddr*)&sender_addr, &addr_len);

        if (bytes_received > 0) {
            *from_tcp = 0;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // UDP接收错误
            return -1;
        }
    }

    // 如果UDP没有数据，尝试从TCP接收
    if (bytes_received <= 0 && conn->tcp_fd >= 0) {
        bytes_received = recv(conn->tcp_fd, packet, sizeof(ht_packet_t), 0);

        if (bytes_received > 0) {
            *from_tcp = 1;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            // TCP接收错误
            return -1;
        }
    }

    if (bytes_received > 0) {
        // 验证数据包
        if (packet->header.magic != HT_MAGIC) {
            return -1; // 无效的数据包
        }

        // 验证校验和
        uint32_t received_checksum = packet->header.checksum;
        packet->header.checksum = 0;
        uint32_t calculated_checksum = ht_calculate_checksum(packet,
            sizeof(ht_packet_header_t) + packet->header.payload_size);

        if (received_checksum != calculated_checksum) {
            return -1; // 校验和错误
        }

        packet->header.checksum = received_checksum;

        conn->stats.packets_received++;
        conn->stats.bytes_received += bytes_received;
        gettimeofday(&conn->last_activity, NULL);
    }

    return bytes_received;
}

// 更新RTT统计
void ht_update_rtt(ht_connection_t* conn, uint32_t rtt) {
    if (!conn) {
        return;
    }

    if (rtt < conn->stats.rtt_min) {
        conn->stats.rtt_min = rtt;
    }

    if (rtt > conn->stats.rtt_max) {
        conn->stats.rtt_max = rtt;
    }

    // 计算平均RTT (使用指数移动平均)
    if (conn->stats.rtt_avg == 0) {
        conn->stats.rtt_avg = rtt;
    } else {
        conn->stats.rtt_avg = (conn->stats.rtt_avg * 7 + rtt) / 8;
    }
}

// 发送应用数据
int ht_send_data(ht_connection_t* conn, const void* data, size_t size) {
    if (!conn || !data || size == 0 || !conn->is_connected) {
        return -1;
    }

    const uint8_t* bytes = (const uint8_t*)data;
    size_t bytes_sent = 0;

    while (bytes_sent < size) {
        // 计算本次发送的数据大小
        size_t chunk_size = size - bytes_sent;
        if (chunk_size > HT_MAX_PAYLOAD_SIZE) {
            chunk_size = HT_MAX_PAYLOAD_SIZE;
        }

        // 创建数据包
        ht_packet_t packet;
        memset(&packet, 0, sizeof(packet));

        packet.header.magic = HT_MAGIC;
        packet.header.version = 1;
        packet.header.type = HT_TYPE_DATA;
        packet.header.sequence = conn->send_sequence++;
        packet.header.ack_sequence = conn->ack_sequence;
        packet.header.window_size = conn->recv_window_size;
        packet.header.payload_size = chunk_size;
        packet.header.timestamp = get_timestamp_ms();

        // 复制载荷数据
        memcpy(packet.payload, bytes + bytes_sent, chunk_size);

        // 决定使用哪个传输通道
        int use_tcp = ht_should_use_tcp(conn);

        // 发送数据包
        int result = ht_send_packet(conn, &packet, use_tcp);
        if (result < 0) {
            // 如果首选通道失败，尝试另一个通道
            use_tcp = !use_tcp;
            result = ht_send_packet(conn, &packet, use_tcp);
            if (result < 0) {
                break;
            }
        }

        // 将数据包添加到发送缓冲区（用于重传）
        if (packet.header.type == HT_TYPE_DATA) {
            ht_send_buffer_entry_t* entry = malloc(sizeof(ht_send_buffer_entry_t));
            if (entry) {
                entry->packet = packet;
                gettimeofday(&entry->send_time, NULL);
                entry->retransmit_count = 0;
                entry->next = conn->send_buffer;
                conn->send_buffer = entry;
            }
        }

        bytes_sent += chunk_size;
    }

    return bytes_sent;
}

// 接收应用数据
int ht_recv_data(ht_connection_t* conn, void* buffer, size_t buffer_size) {
    if (!conn || !buffer || buffer_size == 0 || !conn->is_connected) {
        return -1;
    }

    uint8_t* bytes = (uint8_t*)buffer;
    size_t bytes_received = 0;

    // 处理接收缓冲区中的有序数据
    ht_recv_buffer_entry_t* entry = conn->recv_buffer;
    ht_recv_buffer_entry_t* prev = NULL;

    while (entry && bytes_received < buffer_size) {
        if (entry->received && entry->packet.header.sequence == conn->recv_sequence) {
            // 找到下一个期望的数据包
            size_t copy_size = entry->packet.header.payload_size;
            if (copy_size > buffer_size - bytes_received) {
                copy_size = buffer_size - bytes_received;
            }

            memcpy(bytes + bytes_received, entry->packet.payload, copy_size);
            bytes_received += copy_size;
            conn->recv_sequence++;

            // 从缓冲区中移除已处理的数据包
            if (prev) {
                prev->next = entry->next;
            } else {
                conn->recv_buffer = entry->next;
            }

            free(entry);
            entry = prev ? prev->next : conn->recv_buffer;
        } else {
            prev = entry;
            entry = entry->next;
        }
    }

    return bytes_received;
}

// 处理事件（接收数据包、处理ACK等）
int ht_process_events(ht_connection_t* conn) {
    if (!conn || !conn->is_connected) {
        return -1;
    }

    ht_packet_t packet;
    int from_tcp;
    int processed = 0;

    // 处理所有可用的数据包
    while (ht_recv_packet(conn, &packet, &from_tcp) > 0) {
        processed++;

        switch (packet.header.type) {
            case HT_TYPE_DATA:
                // 处理数据包
                {
                    // 发送ACK
                    ht_packet_t ack_packet;
                    memset(&ack_packet, 0, sizeof(ack_packet));
                    ack_packet.header.magic = HT_MAGIC;
                    ack_packet.header.version = 1;
                    ack_packet.header.type = HT_TYPE_ACK;
                    ack_packet.header.sequence = conn->send_sequence++;
                    ack_packet.header.ack_sequence = packet.header.sequence;
                    ack_packet.header.timestamp = get_timestamp_ms();

                    ht_send_packet(conn, &ack_packet, from_tcp);

                    // 将数据包添加到接收缓冲区
                    ht_recv_buffer_entry_t* entry = malloc(sizeof(ht_recv_buffer_entry_t));
                    if (entry) {
                        entry->packet = packet;
                        gettimeofday(&entry->recv_time, NULL);
                        entry->received = 1;
                        entry->next = conn->recv_buffer;
                        conn->recv_buffer = entry;
                    }
                }
                break;

            case HT_TYPE_ACK:
                // 处理ACK包
                {
                    uint32_t acked_seq = packet.header.ack_sequence;

                    // 从发送缓冲区中移除已确认的数据包
                    ht_send_buffer_entry_t* send_entry = conn->send_buffer;
                    ht_send_buffer_entry_t* send_prev = NULL;

                    while (send_entry) {
                        if (send_entry->packet.header.sequence == acked_seq) {
                            // 计算RTT
                            struct timeval now;
                            gettimeofday(&now, NULL);
                            uint32_t rtt = time_diff_ms(&send_entry->send_time, &now);
                            ht_update_rtt(conn, rtt);

                            // 移除已确认的数据包
                            if (send_prev) {
                                send_prev->next = send_entry->next;
                            } else {
                                conn->send_buffer = send_entry->next;
                            }

                            free(send_entry);
                            break;
                        }
                        send_prev = send_entry;
                        send_entry = send_entry->next;
                    }
                }
                break;

            case HT_TYPE_HEARTBEAT:
                // 处理心跳包
                gettimeofday(&conn->last_activity, NULL);
                break;

            case HT_TYPE_CONTROL:
                // 处理控制包
                if (packet.header.flags & 0x01) { // 关闭标志
                    conn->is_connected = 0;
                }
                break;
        }
    }

    return processed;
}

// 处理超时事件（重传、心跳等）
int ht_handle_timeout(ht_connection_t* conn) {
    if (!conn || !conn->is_connected) {
        return -1;
    }

    struct timeval now;
    gettimeofday(&now, NULL);
    int actions = 0;

    // 检查发送缓冲区中需要重传的数据包
    ht_send_buffer_entry_t* entry = conn->send_buffer;
    ht_send_buffer_entry_t* prev = NULL;

    while (entry) {
        uint32_t elapsed = time_diff_ms(&entry->send_time, &now);

        if (elapsed > conn->retransmit_timeout) {
            if (entry->retransmit_count < conn->max_retransmit) {
                // 重传数据包
                entry->retransmit_count++;
                entry->send_time = now;

                // 优先使用TCP重传
                int result = ht_send_packet(conn, &entry->packet, 1);
                if (result < 0) {
                    // TCP失败，尝试UDP
                    result = ht_send_packet(conn, &entry->packet, 0);
                }

                if (result > 0) {
                    conn->stats.packets_retransmitted++;
                    actions++;
                }

                entry = entry->next;
            } else {
                // 超过最大重传次数，丢弃数据包
                conn->stats.packets_lost++;

                if (prev) {
                    prev->next = entry->next;
                } else {
                    conn->send_buffer = entry->next;
                }

                ht_send_buffer_entry_t* to_free = entry;
                entry = entry->next;
                free(to_free);
                actions++;
            }
        } else {
            prev = entry;
            entry = entry->next;
        }
    }

    // 检查是否需要发送心跳
    uint32_t heartbeat_elapsed = time_diff_ms(&conn->last_heartbeat, &now);
    if (heartbeat_elapsed > HT_HEARTBEAT_INTERVAL) {
        ht_packet_t heartbeat;
        memset(&heartbeat, 0, sizeof(heartbeat));

        heartbeat.header.magic = HT_MAGIC;
        heartbeat.header.version = 1;
        heartbeat.header.type = HT_TYPE_HEARTBEAT;
        heartbeat.header.sequence = conn->send_sequence++;
        heartbeat.header.timestamp = get_timestamp_ms();

        // 心跳包优先使用UDP
        int result = ht_send_packet(conn, &heartbeat, 0);
        if (result < 0) {
            result = ht_send_packet(conn, &heartbeat, 1);
        }

        if (result > 0) {
            conn->last_heartbeat = now;
            actions++;
        }
    }

    // 检查连接是否超时
    uint32_t activity_elapsed = time_diff_ms(&conn->last_activity, &now);
    if (activity_elapsed > 30000) { // 30秒无活动
        conn->is_connected = 0;
        return -1;
    }

    return actions;
}
