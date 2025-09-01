#ifdef __linux__
#include <cerrno>
#include <fmt/format.h>
#include <protocol/http.h>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>
#include <sys/fcntl.h>

namespace m::net::http {
//创建Epoll实例，该实例本质上是一个内核事件表，用于存储要监控的文件描述符及其对应的事件
EpollSelector::EpollSelector(int listen_fd, size_t size)
    : m_listen_fd{listen_fd} {
    m_epoll_fd = epoll_create(size);//返回引用该实例的非负文件描述符，失败返回-1，并设置errno错误原因。size参数已弃用，通常传入大于0的值
    }

EpollSelector::~EpollSelector() {
    close(m_epoll_fd);
    }

static auto set_nonblocking_fd(int fd) -> int {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
    }

auto EpollSelector::set_nonblocking_fd(int fd) -> int {
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
    }

//使用 epoll_ctl 系统调用向 Epoll 实例中添加、修改或删除要监控的文件描述符及其对应的事件。例如，可以指定要监控文件描述符的读事件、写事件等
auto EpollSelector::register_timer(int fd) -> void {
    epoll_event event{.events = EPOLLIN | EPOLLERR | EPOLLHUP, .data = {.fd = fd}};
    /*
    int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
    参数：
    epfd：Epoll 实例的文件描述符，由 epoll_create 返回。
    op：操作类型，可以是 EPOLL_CTL_ADD（添加）、EPOLL_CTL_MOD（修改）或 EPOLL_CTL_DEL（删除）。
    fd：要监控的文件描述符。
    event：指向 struct epoll_event 结构体的指针，用于指定要监控的事件类型和相关数据。
    返回值：成功时返回 0，失败时返回 -1，并设置 errno 以指示错误原因。
    */
    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking_fd(fd);
    }

void EpollSelector::register_on_listening_lt(int fd) const {
    epoll_event event{.events = EPOLLIN | EPOLLRDHUP, .data = {.fd = fd}};
    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event);
    set_nonblocking_fd(fd);//设为非阻塞模式
    }

void EpollSelector::register_on_reading(int fd, bool one_shot,
                                        bool blocking) const {
    epoll_event event{.events = EPOLLIN | EPOLLET | EPOLLRDHUP,
                      .data = {.fd = fd}};
    if (one_shot)
        event.events |= EPOLLONESHOT;
    epoll_ctl(m_epoll_fd, EPOLL_CTL_ADD, fd, &event);
    if (!blocking)
        set_nonblocking_fd(fd);
    }

auto EpollSelector::unregister(int fd) const -> void {
    epoll_ctl(m_epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
    }

auto EpollSelector::read_again_on(int fd) const -> void {
    epoll_event event{.events = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, .data = {.fd = fd}};
    epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &event);
    }

auto EpollSelector::write_again_on(int fd) const -> void {
    epoll_event event{.events = EPOLLOUT | EPOLLET | EPOLLONESHOT | EPOLLRDHUP, .data = {.fd = fd}};
    epoll_ctl(m_epoll_fd, EPOLL_CTL_MOD, fd, &event);
    }

//使用 epoll_wait 系统调用等待 Epoll 实例中注册的文件描述符上的事件发生。当有事件发生时，epoll_wait 会返回发生事件的文件描述符列表
auto EpollSelector::get_next_event() -> Event {
    while (m_current_event_index >= m_current_event_num) {
        m_current_event_index = 0;
        /*epoll_wait
        * 功能：等待 Epoll 实例中注册的文件描述符上的事件发生。
        参数：
        epfd：Epoll 实例的文件描述符，由 epoll_create 返回。
        events：指向 struct epoll_event 结构体数组的指针，用于存储发生事件的文件描述符信息。
        maxevents：events 数组的最大元素个数。
        timeout：超时时间，单位为毫秒。如果设置为 -1，则表示无限等待；如果设置为 0，则表示立即返回。
        返回值：成功时返回发生事件的文件描述符数量，超时返回 0，失败时返回 -1，并设置 errno 以指示错误原因。
        */
        m_current_event_num = epoll_wait(m_epoll_fd, m_events.data(), MAX_EVENT_NUM, m_timeout);
        if (m_current_event_num < 0 && (errno != EINTR)) {
            auto err = errno;
            throw std::runtime_error{
                fmt::format("bad epoll trigger, errno = {}", err)};
            }
        }
    auto const &e = m_events[m_current_event_index++];
    auto fd = e.data.fd;
    auto event = e.events;
    if (fd == m_listen_fd)
        return {Event::Tag::CONNECTION, {}};
    if (event & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
        return {Event::Tag::CLOSE, fd};
    if (event & EPOLLIN)
        return {Event::Tag::READ, fd};
    if (event & EPOLLOUT)
        return {Event::Tag::WRITE, fd};
    throw std::runtime_error{
        fmt::format("unknown epoll event type, event={}", event)};
    }
} // namespace m::net::http
#endif