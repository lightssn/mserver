//
// Created by wwww on 2023/8/23.
//

#ifndef CPP_SIMPLE_WEB_SERVER_OS_HPP
#define CPP_SIMPLE_WEB_SERVER_OS_HPP
#include <os/signal.h>
#include <cstdint>
#ifdef WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif
namespace m::os {
class Timer {
    private:
        int m_fd;

    public:
        explicit Timer(size_t seconds, size_t nanoseconds = 0);
        [[nodiscard]] inline int get_fd() const {
            return m_fd;
            };
        ~Timer() {
#ifdef _WIN32
            closesocket(m_fd);
#else
            close(m_fd);
#endif
            }
        std::uint64_t tick() const;
    };
} // namespace m::os
#endif // CPP_SIMPLE_WEB_SERVER_OS_HPP
