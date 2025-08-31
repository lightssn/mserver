#ifndef CPP_SIMPLE_WEB_SERVER_TCP_H
#define CPP_SIMPLE_WEB_SERVER_TCP_H
#include <string_view>
#ifdef WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif
namespace m::net::tcp {
// create some tcp socket file descriptor using sys/socket.h
int create_socket();

// throw std::runtime_error when fails
void bind(int fd, std::string_view ip, size_t port);

// throw std::runtime_error when fails
void listen(int fd, size_t n = 5);

// throw std::runtime_error when fails
std::tuple<int, struct sockaddr_in> accept(int listen_fd);
} // namespace m::net
#endif // CPP_SIMPLE_WEB_SERVER_TCP_H
