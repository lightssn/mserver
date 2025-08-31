#include <iostream>
#include <string>
#include <cstring>
#include <set>

#ifdef _WIN32
#include <winsock2.h>//recv
#include <ws2tcpip.h>//socklen_t
//#pragma comment(lib, "ws2_32.lib")//cpp里无效？需在cmake或编译时指定
#else
#include <sys/socket.h>//recv
#include <arpa/inet.h>//inet_ntoa
#include <unistd.h>//read
#define INVALID_SOCKET -1
#endif

constexpr auto DEBUG = false;
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

std::set<std::string> protocols = { "tcp", "udp", "http", "https" };

namespace mnet {
//初始化Winsock
bool init() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return false;
        }
#endif
    return true;
    }

void stop(socklen_t fd) {
    if (fd != (socklen_t)INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        fd = (socklen_t)INVALID_SOCKET;
        }
    std::cout << "stopped " << std::endl;
    return;
    }

void end() {
#ifdef _WIN32
    WSACleanup();
#endif
    }
}