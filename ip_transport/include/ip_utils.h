#ifndef IP_UTILS_H
#define IP_UTILS_H

#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <optional>
#include <string>
#include <unordered_set>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/if_link.h>
#include <netinet/tcp.h>

#include <logging.h>
#include <message.h>
#include <peer_id.h>
#include <net/if.h>
#include <ifaddrs.h>

#define UDP_PORT 22855
#define TCP_LISTEN_PORT 22855
#define MAX_EVENTS 20
#define BUFFER_LEN 1024
#define HELLO_ID "Hello"
#define BROADCAST_DELAY 1
#define LOST_PEER_DELAY (10 * BROADCAST_DELAY)

#undef TAG
#define TAG "ip_utils"

inline bool operator<(sockaddr_in const &a, sockaddr_in const &b)
{
    return a.sin_addr.s_addr < b.sin_addr.s_addr;
}

inline bool operator==(sockaddr_in const &a, sockaddr_in const &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr;
}

/**
 * 1. Initiating connection
 *    - Connect to peer via TCP
 *    - Send Hello::Notify to peer
 *    - Read Hello::Answer from peer
 * 2. Receiving connection
 *    - Accept connection from peer via TCP
 *    - Read Hello::Notify from peer
 *    - Send Hello::Answer to peer
 */

enum class HelloType : uint8_t
{
    Notify,
    Answer,
};

struct HelloMsg
{
    char identifier[sizeof(HELLO_ID)] = HELLO_ID;
    diplom::PeerId id{};
    HelloType type = HelloType::Notify;

    bool IsValid()
    {
        return std::strcmp(identifier, HELLO_ID) == 0;
    }

    diplom::Message Serialize()
    {
        diplom::Message msg(sizeof(HelloMsg));
        std::copy(reinterpret_cast<std::byte *>(this), reinterpret_cast<std::byte *>(this) + sizeof(HelloMsg),
                  msg.data());
        return msg;
    }

    static std::optional<HelloMsg> Deserialize(diplom::Message msg)
    {
        HelloMsg hello{};
        if (msg.size() != sizeof(HelloMsg))
        {
            LOGE("Hello message size is wrong");
            return std::nullopt;
        }

        std::copy(msg.data(), msg.data() + sizeof(HelloMsg), reinterpret_cast<std::byte *>(&hello));

        if (!hello.IsValid())
        {
            LOGE("Hello message data is wrong");
            return std::nullopt;
        }

        return hello;
    }
} __attribute__((packed));

namespace diplom::transport::ip::common
{

    inline std::optional<sockaddr_in> get_fd_addr(int fd)
    {
        struct sockaddr_in addr{};
        socklen_t addr_size = sizeof(struct sockaddr_in);
        int res = getpeername(fd, (struct sockaddr *)&addr, &addr_size);
        if (res == 0)
        {
            return addr;
        }
        else
        {
            return std::nullopt;
        }
    }

    inline const char *get_interface_by_ip(struct in_addr addr)
    {
        struct ifaddrs *ifaddr, *ifa;
        static char interface_name[IF_NAMESIZE + 1];

        if (getifaddrs(&ifaddr) == -1)
        {
            LOGE("Error receiving interfaces");
            return nullptr;
        }

        for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr && ifa->ifa_netmask && ifa->ifa_addr->sa_family == AF_INET)
            {
                auto ifaddr_in = (struct sockaddr_in *)ifa->ifa_addr;
                auto ifaddr_mask = (struct sockaddr_in *)ifa->ifa_netmask;
                if ((ifaddr_in->sin_addr.s_addr & (ifaddr_mask->sin_addr.s_addr)) == (addr.s_addr & (ifaddr_mask->sin_addr.s_addr)))
                {
                    strcpy(interface_name, ifa->ifa_name);
                    freeifaddrs(ifaddr);
                    LOGD("Found interface: %s", interface_name);
                    return interface_name;
                }
            }
        }

        freeifaddrs(ifaddr);
        return nullptr;
    }

    inline std::unordered_set<std::string> get_interfaces() {
        struct ifaddrs *ifaddr, *ifa;
        static char interface_name[IF_NAMESIZE + 1];

        if (getifaddrs(&ifaddr) == -1)
        {
            LOGE("Error receiving interfaces");
            return {};
        }

        std::unordered_set<std::string> interfaces;

        for (ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next)
        {
            if (ifa->ifa_addr && ifa->ifa_netmask && ifa->ifa_addr->sa_family == AF_INET && (ifa->ifa_flags & IFF_UP))
            {
                auto ifaddr_in = (struct sockaddr_in *)ifa->ifa_addr;
                auto ifaddr_mask = (struct sockaddr_in *)ifa->ifa_netmask;
                strcpy(interface_name, ifa->ifa_name);
                interfaces.insert(std::string(interface_name));
            }
        }

        freeifaddrs(ifaddr);

        return interfaces;
    }

    inline int create_netlink_socket() {
        int netlink_socket_fd = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK, NETLINK_ROUTE);
        if (netlink_socket_fd < 0) {
            return -1;
        }
    
        struct sockaddr_nl addr;
        memset(&addr, 0, sizeof(addr));
        addr.nl_family = AF_NETLINK;
        addr.nl_groups = RTMGRP_LINK;  
        addr.nl_pid = getpid();        
    
        if (bind(netlink_socket_fd, (struct sockaddr*)&addr, sizeof(addr))) {
            close(netlink_socket_fd);
            return -1;
        }
    
        return netlink_socket_fd;
    }

    inline void clear_netlink_socker(int fd) {
        static char buffer[BUFFER_LEN];
        while (1) {
            ssize_t len = recv(fd, buffer, BUFFER_LEN, 0);
            if (len < 1) {
                break;
            }
        }
    }

    inline int create_broadcast_socket(int port = UDP_PORT)
    {
        int udp_socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
        if (udp_socket_fd == -1)
        {
            return -1;
        }

        int broadcast_enable = 1;
        if (setsockopt(udp_socket_fd, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable)))
        {
            close(udp_socket_fd);
            return -1;
        }

        struct sockaddr_in udp_socket_address{
            //
            .sin_family = AF_INET,              //
            .sin_port = htons(port),            //
            .sin_addr = {.s_addr = INADDR_ANY}, //
        };

        if (bind(udp_socket_fd, (struct sockaddr *)&udp_socket_address, sizeof(udp_socket_address)) == -1)
        {
            close(udp_socket_fd);
            return -1;
        }

        return udp_socket_fd;
    }

    inline int create_tcp_socket(int local_port = 0)
    {
        int tcp_socket_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, IPPROTO_TCP);
        if (tcp_socket_fd == -1)
        {
            return -1;
        }

        if (local_port)
        {
            int opt = 1;
            if (setsockopt(tcp_socket_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
            {
                close(tcp_socket_fd);
                return -1;
            }
        }

        int opt = 1;
        if (setsockopt(tcp_socket_fd, IPPROTO_TCP, TCP_NODELAY, (char *)&opt, sizeof(int)) < 0) {
            close(tcp_socket_fd);
            return -1;
        }

        if (setsockopt(tcp_socket_fd, SOL_SOCKET, SO_OOBINLINE, (char *)&opt, sizeof(int)) < 0) {
            close(tcp_socket_fd);
            return -1;
        }

        struct sockaddr_in tcp_socket_address = {
            .sin_family = AF_INET, .sin_port = htons(local_port), .sin_addr = {.s_addr = INADDR_ANY}};

        if (bind(tcp_socket_fd, (struct sockaddr *)&tcp_socket_address, sizeof(tcp_socket_address)) == -1)
        {
            close(tcp_socket_fd);
            return -1;
        }

        return tcp_socket_fd;
    }

    inline ssize_t send_broadcast(int udp_fd, char const *msg, int len, in_addr_t addr)
    {
        struct sockaddr_in br_address = {
            .sin_family = AF_INET,
            .sin_port = htons(UDP_PORT),
            .sin_addr = {.s_addr = addr},
        };

        return sendto(udp_fd, msg, len, 0, (struct sockaddr *)&br_address, sizeof(br_address));
    }

    inline int set_timer(int timer_fd, int sec)
    {
        itimerspec ts = {
            .it_interval =
                {
                    .tv_sec = sec, //
                    .tv_nsec = 0   //
                },                 //
            .it_value =
                {
                    .tv_sec = sec, //
                    .tv_nsec = 0   //
                } //
        };
        return timerfd_settime(timer_fd, 0, &ts, nullptr);
    }

    inline int create_timer(int sec)
    {
        int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
        set_timer(timer_fd, sec);
        return timer_fd;
    }

    inline std::optional<std::pair<std::vector<std::byte>, struct in_addr>> read_broadcast(int fd)
    {
        std::byte buffer[BUFFER_LEN];
        ssize_t len = 0;
        struct sockaddr_in addr{};
        socklen_t addr_len = sizeof(addr);

        len = recvfrom(fd, buffer, BUFFER_LEN, 0, (struct sockaddr *)&addr, &addr_len);
        if (len == -1)
        {
            return std::nullopt;
        }

        return std::make_pair(std::vector<std::byte>(buffer, buffer + len), addr.sin_addr);
    }

    inline std::pair<struct sockaddr_in, int> accept_client(int fd)
    {
        LOGD("accept_client");
        struct sockaddr_in client_address{};
        socklen_t client_addr_len = sizeof(client_address);
        int client_fd = accept(fd, (struct sockaddr *)&client_address, (socklen_t *)&client_addr_len);
        int flags;
        if (-1 == (flags = fcntl(fd, F_GETFL, 0)))
            flags = 0;
        LOGD("Okay");
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        return {client_address, client_fd};
    }

    static std::string in_addr_to_string(const struct in_addr &addr)
    {
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);
        return ip_str;
    }

    static std::string sockaddr_in_to_string(const struct sockaddr_in &addr)
    {
        uint16_t port = ntohs(addr.sin_port);
        return in_addr_to_string(addr.sin_addr) + ":" + std::to_string(port);
    }

} // namespace diplom::transport::ip::common

static std::string EpollEventToString(uint32_t events)
{
    std::string result;

    if (events & EPOLLIN)
    {
        result += "EPOLLIN";
    }

    if (events & EPOLLOUT)
    {
        if (!result.empty())
            result += "|";
        result += "EPOLLOUT";
    }

    if (events & EPOLLERR)
    {
        if (!result.empty())
            result += "|";
        result += "EPOLLERR";
    }

    if (events & EPOLLHUP)
    {
        if (!result.empty())
            result += "|";
        result += "EPOLLHUP";
    }

    if (events & EPOLLET)
    {
        if (!result.empty())
            result += "|";
        result += "EPOLLET";
    }

    if (result.empty())
    {
        return "NONE";
    }

    return result;
}

#endif
