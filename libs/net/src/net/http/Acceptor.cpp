//
// Created by wwww on 2023/8/24.
//
#include <protocol/http.h>
#include <protocol/tcp.h>
namespace m::net::http {

Acceptor::Acceptor(int fd) : m_listen_fd{fd} {}

auto Acceptor::accept() const -> std::tuple<int,struct sockaddr_in> {
    return m::net::tcp::accept(m_listen_fd);
    }
} // namespace m::net::http