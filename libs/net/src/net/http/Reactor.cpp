#include <stdexcept>
#include "my_json/core.h"
#include <fmt/core.h>
#include <protocol/http.h>
#include <protocol/tcp.h>
#include <os.h>
#include <thread_pool.h>
#include <thread_pool_simple.h>
#ifdef WIN32
#include "../../libs/net/IocpSelector.h"
#include <winsock2.h>
#include <ws2tcpip.h>    // 扩展TCP/IP功能（如InetPton）
#include <mswsock.h>     // AcceptEx等扩展API
#else
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif
constexpr auto DEBUG = false;
using namespace std;

namespace m::net::http {
//创建TCP服务器套接字，绑定IP和端口，开始监听
Reactor::Reactor(const Reactor::Config &cfg) : m_config{cfg}, m_handlers(MAX_FD), m_rpc_funcs{} {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw runtime_error("WSAStartup failed");//抛出异常对象
        //throw make_exception_ptr(runtime_error("WSAStartup failed"));//抛出共享异常对象指针
        }
#endif
    //创建 TCP 套接字
    m_server_fd = tcp::create_socket();

    //设置套接字SO_LINGER，延迟关闭，延迟时间为1秒
    //  struct linger tmp {1, 1};
    //  setsockopt(m_server_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    //设置套接字SO_REUSEADDR，允许地址复用。服务器重启时，如果没有设置该选项，可能会因为旧的连接还处于 TIME_WAIT 状态而导致无法立即绑定到相同的地址和端口
    int flag = 1;//表示启用
    setsockopt(m_server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag), sizeof(flag));

    tcp::bind(m_server_fd, cfg.ip, cfg.port);//绑定套接字的IP和端口
    tcp::listen(m_server_fd, cfg.listen_size);//开始监听并设置监听队列长度（允许等待处理的客户端连接请求数）
    fmt::println("[INFO] Server listening on {}:{}", cfg.ip, cfg.port);
    }//Reactor

Reactor::~Reactor() {
    closesocket(m_server_fd);
    for (int i = 0; i < m_handlers.size(); ++i)
        if (m_handlers[i] != nullptr)
            closesocket(i);
#ifdef _WIN32
    if (WSACleanup() == SOCKET_ERROR) {
        throw runtime_error("WSACleanup failed. Error code: " + WSAGetLastError());
        }
#endif
    }

//基于事件驱动的网络服务器反应器Reactor。使用 Epoll 作为 I/O 多路复用机制，能够处理多个客户端连接，并通过线程池处理客户端请求。同时，它还具备连接空闲超时处理功能，当连接空闲时间超过设定的阈值时，会自动关闭连接
void Reactor::run() {
    auto lazy_current_time = chrono::steady_clock::now();

    //管理 I/O 事件的对象，传入服务器套接字描述符和选择器大小
#ifdef _WIN32
    auto selector = IocpSelector(m_server_fd, m_config.selector_size);
#else
    auto selector = EpollSelector(m_server_fd, m_config.selector_size);
#endif

    //Lambda函数，关闭指定文件描述符对应的连接，包括释放处理程序、从选择器中注销和关闭套接字
    auto close_connection = [&](int fd) {
        m_handlers[fd].reset();
        selector.unregister(fd);
#ifdef _WIN32
        closesocket(fd);
#else
        close(fd);
#endif
        };

    //Lambda函数，添加新的客户端连接，将其注册到选择器的读事件中，并创建对应的处理程序
    auto add_connection = [&](int client_fd, const sockaddr_in &client_addr) {
#ifdef _WIN32
        // Windows 下需要立即投递读操作
        //selector.register_on_reading(client_fd);//t
        auto state = m_handlers[client_fd]->post_recv(client_fd);
        if (state != IOState::OK) {
            close_connection(client_fd);
            return;
            }
#else
        selector.register_on_reading(client_fd);
#endif
        auto &handler = m_handlers[client_fd];
        handler = make_shared<Handler>(lazy_current_time, client_addr);
        };

    // 注册监听socket
#ifdef _WIN32
    selector.register_on_listening_lt(m_server_fd);
    // Windows 需要预先投递AcceptEx
    //post_accept();
#else
    selector.register_on_listening_lt(m_server_fd);
#endif

    //创建处理客户端请求的线程池
    size_t size = m_config.working_thread_num;
    //auto thread_pool = thread::ThreadPool(size);
    auto thread_pool = ThreadPoolSimple(size);

    //创建定时器，用于检查连接的空闲时间
    // Windows IOCP 不需要单独的定时器fd，可以使用WaitableTimer
#ifdef _WIN32
    HANDLE hTimer = CreateWaitableTimer(NULL, TRUE, NULL);
    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart = -static_cast<LONGLONG>(m_config.max_idle_seconds) * 10000000LL;
    SetWaitableTimer(hTimer, &liDueTime, m_config.max_idle_seconds * 1000, NULL, NULL, FALSE);
#else
    auto max_connection_idle_time = chrono::seconds { m_config.max_idle_seconds };
    auto idle_timer = m::os::Timer(max_connection_idle_time.count(), 0);
    auto idle_timer_fd = idle_timer.get_fd();
    selector.register_timer(idle_timer_fd);

    //Lambda函数，空闲连接处理。遍历所有处理程序，检查每个连接的空闲时间。如果连接的空闲时间超过了 max_connection_idle_time，则关闭该连接；否则，更新连接的最后活跃时间
    auto remove_idle_connections = [&] {
        lazy_current_time = chrono::steady_clock::now();
        for (int i = 0; i < m_handlers.size(); ++i) {
            if (m_handlers[i] == nullptr)
                continue;
            if (m_handlers[i]->m_last_alive_time + max_connection_idle_time <
                    lazy_current_time)
                close_connection(i);
            else
                m_handlers[i]->update_current_time(lazy_current_time);
            }
        };
#endif

    //事件循环，从选择器中获取下一个事件并处理
    while (true) {
#ifdef _WIN32
        using EventTag = IocpSelector::Event::Tag;
#else
        using EventTag = EpollSelector::Event::Tag;
#endif
        auto [tag, fd] = selector.get_next_event();

#ifdef _WIN32
        // Windows 下需要处理定时器事件
        //if (tag == EventTag::TIMER) {
        //    remove_idle_connections();
        //    continue;
        //}
#endif

        //新连接事件
        if (tag == EventTag::CONNECTION) {
            //接受新连接
#ifdef _WIN32
// Windows 下使用AcceptEx接受的连接
            //auto [client_fd, client_addr] = get_accept_result();//t
            // 继续投递新的AcceptEx
            //post_accept();
#else
            auto &&[client_fd, client_addr] = m::net::tcp::accept(m_server_fd);
#endif
            add_connection(client_fd, client_addr);//处理新连接
            if constexpr (DEBUG) {
                fmt::println("[INFO] hello from {}:{} on fd {}",
                             string_view{inet_ntoa(client_addr.sin_addr)},
                             ntohs(client_addr.sin_port), client_fd);
                }
            continue;
            }

        //定时器事件
        if (fd == idle_timer_fd) {
            idle_timer.tick();//处理定时器滴答
            remove_idle_connections();//检查并关闭空闲连接
            continue;
            }

        auto &handler = m_handlers[fd];

        //关闭事件
        if (tag == EventTag::CLOSE) {
            if constexpr (DEBUG) {
                fmt::println("[INFO] bye to {}  on fd {}", handler->get_addr_str(), fd);
                }
            close_connection(fd);//关闭连接
            }

        //读事件。如果读取状态不是 IOState::OK，则关闭连接；否则，将处理任务提交到线程池，根据处理结果决定是再次注册读事件还是写事件
        else if (tag == EventTag::READ) {
#ifdef _WIN32
            // Windows 下数据已经在Handler的buffer中
            thread_pool.submit([&, h = m_handlers[fd], fd = fd]() {
                auto state = h->work(m_config.mapping_path, m_rpc_funcs);
                if (state == IOState::PENDING) {
                    h->post_recv(fd);
                    }
                else {
                    h->post_send(fd);
                    }
                });
#else
            auto state = handler->read(fd);
            if (state != IOState::OK) {
                close_connection(fd);
                continue;
                }
            thread_pool.submit([&, h = m_handlers[fd], fd = fd]() {
                auto state = h->work(m_config.mapping_path, m_rpc_funcs);
                if (state == IOState::PENDING)
                    selector.read_again_on(fd);
                else
                    selector.write_again_on(fd);
                });
#endif
            }

        //写事件。调用处理程序的 write 函数写入数据。根据写入状态，决定是再次注册写事件、读事件还是关闭连接
        else if (tag == EventTag::WRITE) {
#ifdef _WIN32
            close_connection(fd);//temp
            //auto state = handler->complete_send(fd);
            //if (state == IOState::PENDING) {
            //    handler->continue_send(fd);
            //}
            //else if (state == IOState::KEEP_ALIVE) {
            //    handler->post_recv(fd);
            //}
            //else {
            //    close_connection(fd);
            //}
#else
            auto state = handler->write(fd);
            if (state == IOState::PENDING)
                selector.write_again_on(fd);
            else if (state == IOState::KEEP_ALIVE)
                selector.read_again_on(fd);
            else
                close_connection(fd);
#endif
            }
        }//while(true)
    }//run()
} // namespace m::net::http