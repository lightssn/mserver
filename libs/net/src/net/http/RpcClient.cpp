#include <sstream>
#include <string>
#include <iostream>
#include <protocol/rpc.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <cstring>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif

namespace m::net::rpc {
bool Client::dial(const std::string& address) {
    // 初始化Winsock (仅Windows需要)
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed" << std::endl;
        return false;
        }
#endif
    std::stringstream ss(address);
    std::getline(ss, m_ip, ':');
    if (!ss.eof()) ss >> m_port;
    else m_port = 80; // 默认端口

    memset(&m_serv_addr, 0, sizeof(m_serv_addr));
    m_serv_addr.sin_family = AF_INET;
    m_serv_addr.sin_port = htons(m_port);

    // IP地址转换
#ifdef _WIN32
    if (InetPtonA(AF_INET, m_ip.c_str(), &m_serv_addr.sin_addr) != 1) {
        std::cerr << "Invalid address or Address not supported" << std::endl;
        WSACleanup();
        return false;
        }
#else
    if (inet_pton(AF_INET, m_ip.c_str(), &m_serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address or Address not supported" << std::endl;
        return false;
        }
#endif
    return true;
    }
} // namespace m::net::rpc