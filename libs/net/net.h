#ifndef NET_H
#define NET_H
#include <string>
#include <cstring>
#include <set>

#ifdef WIN32
#include <winsock2.h>//recv, 必须​在windows.h前包含
#include <mswsock.h>//AcceptEx
#include <ws2tcpip.h>//socklen_t, inet_pton
#include <windows.h>
#else
#include <sys/socket.h>//recv
#include <arpa/inet.h>//inet_ntoa
#include <netinet/in.h>
#include <unistd.h>//read
#define INVALID_SOCKET -1
#endif

constexpr auto DEBUG = false;
#define SERVER_IP "127.0.0.1"
#define BUFFER_SIZE 1024

std::set<std::string> protocols = { "tcp", "udp", "http", "https" };
#endif//NET_H