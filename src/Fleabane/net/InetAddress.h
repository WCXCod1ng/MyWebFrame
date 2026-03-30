//
// Created by user on 2025/11/25.
//

#ifndef INETADDRESS_H
#define INETADDRESS_H

#include <cstring>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <string>

#include "log/Logger.h"

namespace fleabane {
    class InetAddress {
    public:
        // 构造函数：仅指定端口，默认监听所有网卡 (INADDR_ANY)
        explicit InetAddress(uint16_t port = 0, std::string ip = "127.0.0.1");

        // 构造函数：直接包装 sockaddr_in
        explicit InetAddress(const struct sockaddr_in& addr)
            : addr_(addr) {}

        // 根据sockfd获取对应的IP地址和端口
        static InetAddress getInetAddress(int sockfd) {
            struct sockaddr_in addr;
            socklen_t len = sizeof(addr);

            std::memset(&addr, 0, sizeof(addr));
            if (::getsockname(sockfd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0) {
                LOG_ERROR("getsockname error");
                return InetAddress();  // 返回默认构造
            }

            return InetAddress(addr);
        }

        std::string toIp() const;
        std::string toIpPort() const;
        uint16_t toPort() const;

        const struct sockaddr_in* getSockAddr() const { return &addr_; }
        void setSockAddr(const struct sockaddr_in& addr) { addr_ = addr; }

    private:
        struct sockaddr_in addr_;
    };
}
#endif //INETADDRESS_H
