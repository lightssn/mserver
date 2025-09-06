#include <stdexcept>
#include "my_json/core.h"
#include <fmt/core.h>
#include <protocol/http.h>
#include <protocol/tcp.h>
#include <os.h>
#include <thread_pool.h>
#include <thread_pool_simple.h>
#include <thread_pool_qt.h>
#ifdef WIN32
#include <ws2tcpip.h>//InetPton
#include "../../libs/net/IOCPSelector.h"
#else
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
#endif
constexpr auto DEBUG = false;
using namespace std;

namespace m::net::http {
Reactor::Reactor(const Config &cfg) : _config{cfg}, _handlers(MAX_FD), _rpc_funcs{} {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        throw runtime_error("WSAStartup failed");//抛出异常对象
        //throw make_exception_ptr(runtime_error("WSAStartup failed"));//抛出共享异常对象指针
        }
#endif

    //创建服务端套接字
#ifdef _WIN32
    _server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#else
    _server_fd = socket(PF_INET, SOCK_STREAM, 0);
#endif
    if (_server_fd < 0) {
        throw runtime_error{ "cannot create socket" };
    }
    //设置套接字SO_LINGER，延迟关闭，延迟时间为1秒
    //  struct linger tmp {1, 1};
    //  setsockopt(_server_fd, SOL_SOCKET, SO_LINGER, &tmp, sizeof(tmp));

    //设置 SO_REUSEADDR 允许端口复用。服务器重启时，如果没有设置该选项，可能会因为旧的连接还处于 TIME_WAIT 状态而导致无法立即绑定到相同的地址和端口
    int flag = 1;//表示启用
    if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&flag), sizeof(flag)) < 0) {
        throw runtime_error("setsockopt failed.");
    }
    //绑定ip和端口到套接字
    string ip = cfg.ip.data();
    struct sockaddr_in address {};
    memset(&address, 0, sizeof(address)); //用memset替代 bzero初始化为零
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
    address.sin_port = htons(static_cast<u_short>(cfg.port));  //将端口号从主机字节序转换为网络字节序，Windows 需要显式转换为 u_short
#ifdef _WIN32
    if (::bind(_server_fd, (struct sockaddr*)&address, sizeof(address)) == SOCKET_ERROR) {
        int err = WSAGetLastError();
        throw runtime_error{ fmt::format("Cannot bind on {}:{}, error code: {}", ip, cfg.port, err) };
    }
#else
    if (bind(_server_fd, (struct sockaddr*)&address, sizeof(address)) != 0) {
        throw runtime_error{ fmt::format("Cannot bind on {}:{}, error: {}", ip, cfg.port, strerror(errno)) };
    }
#endif

    //监听并设置监听队列长度(允许等待处理的客户端连接请求数)
    if (::listen(_server_fd, cfg.listen_size) != 0) {
        throw runtime_error{ "bad listen" };
    }
    fmt::println("[INFO] Server listening on {}:{}", cfg.ip, cfg.port);
    }//Reactor

Reactor::~Reactor() {
    closesocket(_server_fd);
    for (int i = 0; i < _handlers.size(); ++i)
        if (_handlers[i] != nullptr)
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

    //管理I/O事件对象，传入服务器套接字描述符和选择器大小
#ifdef _WIN32
    auto selector = IOCPSelector(_server_fd, _config.selector_size);
#else
    auto selector = EpollSelector(_server_fd, _config.selector_size);
#endif

    //lambda，关闭指定文件描述符对应的连接，包括释放处理程序、从选择器中注销和关闭套接字
    auto close_connection = [&](int fd) {
        _handlers[fd].reset();
        selector.unregister(fd);
        closesocket(fd);
        };

    //lambda，添加新连接，注册到选择器读事件，创建处理程序
    auto add_connection = [&](int client_fd, const sockaddr_in &client_addr) {
        auto& handler = _handlers[client_fd];
        handler = make_shared<Handler>(lazy_current_time, client_addr);
#ifdef _WIN32
        //IOCP 中，​必须先发起一个 I/O 操作（如 WSARecv）​，然后才能等待完成通知。如果不调用 post_recv，IOCP 不会自动监测该套接字的可读事件
        auto state = _handlers[client_fd]->post_recv(client_fd);
        if (state != IOState::OK) {
            close_connection(client_fd);
            return;
            }
#endif
        selector.register_on_reading(client_fd);
        };

#ifdef _WIN32
    // Windows 需要预先投递AcceptEx
    for (int i = 0; i < 5; i++) {
        post_accept_ex(_server_fd);
    }
#endif
    //注册监听socket
    selector.register_on_listening_lt(_server_fd);

    //创建处理客户端请求的线程池
    size_t size = _config.working_thread_num;
    //auto thread_pool = thread::ThreadPool(size);
    auto thread_pool = ThreadPoolSimple(size);//1776336 pages/min
    //auto thread_pool = ThreadPoolQt(size);//1574628 pages/min

    //创建定时器，用于检查连接的空闲时间
#ifdef _WIN32
    HANDLE hTimer = CreateWaitableTimer(NULL, FALSE, NULL);//TRUE手动重置，FALSE自动重置
    if (hTimer == NULL) {
        throw runtime_error("Failed to create timer");
    }

	selector.bind_timer(hTimer);

    //设置定时器(首次触发和触发间隔都为30s)
    LARGE_INTEGER liDueTime;
    liDueTime.QuadPart = -static_cast<LONGLONG>(_config.max_idle_seconds) * 10000000LL;
    if (!SetWaitableTimer(hTimer, &liDueTime, _config.max_idle_seconds * 1000, NULL, NULL, FALSE)) {
        CloseHandle(hTimer);
        throw runtime_error("Failed to SetWaitableTimer");
    }
#else

#endif
    auto max_connection_idle_time = chrono::seconds{ _config.max_idle_seconds };
    auto idle_timer = m::os::Timer(max_connection_idle_time.count(), 0);
    auto idle_timer_fd = idle_timer.get_fd();
    selector.register_timer(idle_timer_fd);

    //lambda，空闲连接处理。遍历所有处理程序，检查每个连接的空闲时间。如果超过max_connection_idle_time，关闭连接；否则，更新连接最后活跃时间
    auto remove_idle_connections = [&] {
        lazy_current_time = chrono::steady_clock::now();
        for (int i = 0; i < _handlers.size(); ++i) {
            if (_handlers[i] == nullptr)
                continue;
            if (_handlers[i]->m_last_alive_time + max_connection_idle_time <
                lazy_current_time)
                close_connection(i);
            else
                _handlers[i]->update_current_time(lazy_current_time);
        }
        };

    //事件循环，从选择器中获取下一个事件并处理
    while (true) {
#ifdef _WIN32
        using EventTag = IOCPSelector::Event::Tag;
#else
        using EventTag = EpollSelector::Event::Tag;
#endif
        auto [tag, fd] = selector.get_next_event();

        //新连接事件
        if (tag == EventTag::CONNECTION) {
            //接受新连接
#ifdef _WIN32
// Windows 下使用AcceptEx接受连接
            auto [client_fd, client_addr] = selector.accept_async(_server_fd);
            // 继续投递新的AcceptEx
            post_accept_ex(_server_fd);
#else
            auto &&[client_fd, client_addr] = m::net::tcp::accept(_server_fd);
#endif
            add_connection(client_fd, client_addr);//处理新连接
            if constexpr (DEBUG) {
                //fmt::println("[INFO] hello from {}:{} on fd {}",
                //             string_view{inet_ntoa(client_addr.sin_addr)},
                //             ntohs(client_addr.sin_port), client_fd);
                }
            continue;
            }

        //定时器事件
#ifdef _WIN32
        if (tag == EventTag::TIME) {
            remove_idle_connections();
            continue;
        }
#else
        if (fd == idle_timer_fd) {
            idle_timer.tick();//处理定时器滴答
            remove_idle_connections();//检查并关闭空闲连接
            continue;
        }
#endif

        auto &handler = _handlers[fd];

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
            thread_pool.submit([&, h = _handlers[fd], fd = fd]() {
                auto state = h->work(_config.mapping_path, _rpc_funcs);
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
            thread_pool.submit([&, h = _handlers[fd], fd = fd]() {
                auto state = h->work(_config.mapping_path, _rpc_funcs);
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