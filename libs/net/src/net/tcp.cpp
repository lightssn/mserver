#include <fmt/format.h>
#include <iostream>
#include <stdexcept>
#ifdef WIN32
#include <ws2tcpip.h>
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif
using namespace std;
namespace m::net::tcp {
int create_socket() {
    auto fd = socket(PF_INET, SOCK_STREAM, 0);//创建IPv4 TCP套接字
    if (fd < 0)
        throw runtime_error{"cannot create new tcp socket"};
    return fd;
    }

void bind(int fd, string_view ip, size_t port) {
    struct sockaddr_in address {};
    memset(&address, 0, sizeof(address)); //使用 memset 替代 bzero初始化为零
    address.sin_family = AF_INET;  // 使用 IPv4
    size_t protocol_pos = ip.find("://");
    if (protocol_pos != string_view::npos) {
        ip = ip.substr(protocol_pos + 3); //去掉http://
    }
    //字符串ip转二进制
#ifdef _WIN32
    if (InetPton(AF_INET, ip.data(), &address.sin_addr) != 1) {
        throw runtime_error("Invalid IP address: " + string(ip));
    }
#else
    if (inet_pton(AF_INET, ip.data(), &address.sin_addr) <= 0) {
        throw runtime_error("Invalid IP address: " + string(ip));
        //throw runtime_error{ fmt::format("Invalid IP address: {}", ip) };//编译期格式化
    }
#endif
    address.sin_port = htons(static_cast<u_short>(port));  //将端口号从主机字节序转换为网络字节序，Windows 需要显式转换为 u_short

    // 绑定操作
#ifdef _WIN32
    if (::bind(fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        throw runtime_error{ fmt::format("Cannot bind on {}:{}, error code: {}", ip, port, err) };
    }
#else
    if (bind(fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        throw runtime_error{ fmt::format("Cannot bind on {}:{}, error: {}", ip, port, strerror(errno)) };
    }
#endif
}

void listen(int fd, size_t n) {
    auto ret = ::listen(fd, n);//n为最多排队数
    if (ret != 0)
        throw runtime_error{"bad listen"};
    }

//从监听套接字接受客户端连接请求，返回新的套接字fd用于与该客户端进行通信，同时返回客户端的地址信息
tuple<int, struct sockaddr_in> accept(int listen_fd) {
    struct sockaddr_in client_address {};//存储客户端地址信息的结构体
    socklen_t client_addr_length = sizeof(client_address);
    int fd = accept(listen_fd, (struct sockaddr *)&client_address,
                    &client_addr_length);
    if (fd < 0) {
        auto err = errno;
        cout << err << endl;
        throw runtime_error{"bad accept"};
        }
    return {fd, client_address};
    }
} // namespace m::net::tcp