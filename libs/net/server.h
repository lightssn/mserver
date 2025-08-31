//跨平台webserver
//cd /mnt/f/codeTest/c++Test/netTest
//g++ -o server server.cpp -lssl -lcrypto -DUSE_SSL -lws2_32
//linux不带-lws2_32
//https需-lssl -lcrypto -DUSE_SSL
//使用：server 8888 (tcp/udp/http/https)
//访问：https://localhost:8888 https://127.0.0.1:8888
#include "../common/stl.h"
#include "net.h"
#include <sstream>
#include <algorithm>

#include "my_json/core.h"
#include <fmt/core.h>
#include <protocol/http.h>
#include <protocol/tcp.h>

#include "../thread/thread_pool_simple.h"
#include <os.h>

#ifdef USE_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

using m::net::http::Handler;
using m::net::http::EpollSelector;
using m::net::http::IOState;
using m::net::http::RpcFuncTable;

namespace mnet {
class Server {
        int _port;
        socklen_t _server_fd;//服务端套接字
        bool _running = true;
        int _socket_type = SOCK_STREAM;
        string _pType = "tcp";

        char _buffer[BUFFER_SIZE] = { 0 };
        sockaddr_in _client_addr;

#ifdef USE_SSL
        SSL_CTX* _ctx; //openssl全局上下文
#endif

        vector<shared_ptr<Handler>> m_handlers;
        RpcFuncTable m_rpc_funcs;//映射表

    public:
        Server(int port, string pType) : _port(port), _pType(pType), _server_fd((socklen_t)INVALID_SOCKET) {}
        ~Server() {
            stop();
            end();
            }

        bool init() {
            if (!init()) return false;
            if (_pType == "tcp" || _pType == "http") _socket_type = SOCK_STREAM;
            else if (_pType == "udp") _socket_type = SOCK_STREAM;
#ifdef USE_SSL
            else if (_pType == "https") {
                _socket_type = SOCK_STREAM;
                //初始化openssl
                SSL_library_init();
                OpenSSL_add_all_algorithms();
                SSL_load_error_strings();
                _ctx = SSL_CTX_new(TLS_server_method());  // 创建 TLS 服务器上下文
                if (!_ctx) {
                    cerr << "SSL_CTX_new failed: " << ERR_error_string(ERR_get_error(), nullptr) << endl;
                    return false;
                    }
                //加载同目录的证书和私钥，地址要使用双反斜杠
                if (SSL_CTX_use_certificate_file(_ctx, "server.crt", SSL_FILETYPE_PEM) <= 0) {
                    cerr << "SSL_CTX_use_certificate_file failed: " << ERR_error_string(ERR_get_error(), nullptr) << endl;
                    return false;
                    }
                if (SSL_CTX_use_PrivateKey_file(_ctx, "server.key", SSL_FILETYPE_PEM) <= 0) {
                    cerr << "SSL_CTX_use_PrivateKey_file failed: " << ERR_error_string(ERR_get_error(), nullptr) << endl;
                    return false;
                    }
                }//https
#endif
            else return false;

            //创建套接字
            if ((_server_fd = socket(AF_INET, _socket_type, 0)) == INVALID_SOCKET) {
                cerr << "Error creating socket" << endl;
                return false;
                }
            //设为非阻塞：ioctlsocket(_server_fd, FIONBIO, &1);

            //设置地址复用，非win手动设置端口复用
            int option = SO_REUSEADDR;
            int opt = 1;
#ifndef _WIN32
            option |= SO_REUSEPORT;
#endif
            if (setsockopt(_server_fd, SOL_SOCKET, option, (const char*)&opt, sizeof(opt)) == INVALID_SOCKET) {
                cerr << "Setsockopt error" << endl;
                return false;
                }

            //绑定地址和端口
            sockaddr_in server_addr;
            memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_addr.s_addr = INADDR_ANY;
            server_addr.sin_port = htons(_port);

            if (bind(_server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == INVALID_SOCKET) {
                cerr << "Error binding socket" << endl;
                return false;
                }
            return true;
            }

        void start() {
            if (_socket_type == SOCK_STREAM) {
                //监听
                if (listen(_server_fd, 3) < 0) {
                    cerr << "Listen error" << endl;
                    return;
                    }
                }
            cout << _pType << "Server started on port " << _port << endl;
            while (_running) {
                if (_pType == "tcp") listenTCP();
                else if (_pType == "udp") listenUDP();
                else if (_pType == "http") listenTCP(true);
#ifdef USE_SSL
                else if (_pType == "https") listenHTTPS();
#endif
                else return;
                }
            }//start

        void run() {
            auto lazy_current_time = steady_clock::now();
            size_t working_thread_num = 4, max_idle_seconds = 30, listen_size = 5, selector_size = 5;

            auto selector = EpollSelector(_server_fd, selector_size);//管理 I/O 事件的对象，传入服务器套接字描述符和选择器大小

            //Lambda函数，关闭指定文件描述符对应的连接，包括释放处理程序、从选择器中注销和关闭套接字
            auto close_connection = [&](int fd) {
                m_handlers[fd].reset();
                selector.unregister(fd);
                close(fd);
                };

            //Lambda函数，添加新的客户端连接，将其注册到选择器的读事件中，并创建对应的处理程序
            auto add_connection = [&](int client_fd, const sockaddr_in& client_addr) {
                selector.register_on_reading(client_fd);
                auto& handler = m_handlers[client_fd];
                handler = make_shared<Handler>(lazy_current_time, client_addr);
                };

            selector.register_on_listening_lt(_server_fd);

            auto thread_pool = ThreadPoolSimple(selector_size);

            //创建定时器，用于检查连接的空闲时间
            auto max_connection_idle_time = seconds{ max_idle_seconds };
            auto idle_timer = m::os::Timer(max_connection_idle_time.count(), 0);
            auto idle_timer_fd = idle_timer.get_fd();
            selector.register_timer(idle_timer_fd);

            //Lambda函数，空闲连接处理。遍历所有处理程序，检查每个连接的空闲时间。如果连接的空闲时间超过了 max_connection_idle_time，则关闭该连接；否则，更新连接的最后活跃时间
            auto remove_idle_connections = [&] {
                lazy_current_time = steady_clock::now();
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

            //事件循环，从选择器中获取下一个事件并处理
            while (true) {
                using EventTag = EpollSelector::Event::Tag;
                auto [tag, fd] = selector.get_next_event();

                //新连接事件
                if (tag == EventTag::CONNECTION) {
                    // incoming request
                    auto&& [client_fd, client_addr] = m::net::tcp::accept(_server_fd);//接受新连接

                    add_connection(client_fd, client_addr);//处理新连接
                    if constexpr (DEBUG) {
                        fmt::println("[INFO] hello from {}:{} on fd {}",
                            string_view{ inet_ntoa(client_addr.sin_addr) },
                            ntohs(client_addr.sin_port), client_fd);
                    }
                    continue;
                }

                //定时器事件
                if (fd == idle_timer_fd) {
                    //            fmt::println("tick {}", idle_timer.tick());
                    idle_timer.tick();//处理定时器滴答
                    remove_idle_connections();//检查并关闭空闲连接
                    continue;
                }

                auto& handler = m_handlers[fd];

                //关闭事件
                if (tag == EventTag::CLOSE) {
                    if constexpr (DEBUG) {
                        fmt::println("[INFO] bye to {}  on fd {}", handler->get_addr_str(), fd);
                    }
                    close_connection(fd);//关闭连接
                }

                //读事件。如果读取状态不是 IOState::OK，则关闭连接；否则，将处理任务提交到线程池，根据处理结果决定是再次注册读事件还是写事件
                else if (tag == EventTag::READ) {
                    auto state = handler->read(fd);
                    if (state != IOState::OK) {
                        close_connection(fd);
                        continue;
                    }
                    string_view mapping_path = "/var/www/html";
                    thread_pool.submit([&, h = m_handlers[fd], fd = fd]() {
                        auto state = h->work(mapping_path, m_rpc_funcs);
                        if (state == IOState::PENDING)
                            selector.read_again_on(fd);
                        else
                            selector.write_again_on(fd);
                        });
                }

                //写事件。调用处理程序的 write 函数写入数据。根据写入状态，决定是再次注册写事件、读事件还是关闭连接
                else if (tag == EventTag::WRITE) {
                    auto state = handler->write(fd);
                    if (state == IOState::PENDING)
                        selector.write_again_on(fd);
                    else if (state == IOState::KEEP_ALIVE)
                        selector.read_again_on(fd);
                    else
                        close_connection(fd);
                }
            }//while(true)
            }//run

        void listenTCP(bool bHttp = false) {
            //接收
            socklen_t new_socket;
            socklen_t addrlen = sizeof(_client_addr);
            if ((new_socket = accept(_server_fd, (struct sockaddr*)&_client_addr, &addrlen)) == INVALID_SOCKET) {
                cerr << "Accept error" << endl;
                stop(new_socket);
                return;
                }

            //读取
            memset(_buffer, 0, sizeof(_buffer));
            //使用string转换_buffer再cout，避免缓冲区越界
            int bytes_received = recv(new_socket, _buffer, BUFFER_SIZE, 0);
            if (bytes_received > 0) {
                string message(_buffer, bytes_received);
                cout << "Received from " << inet_ntoa(_client_addr.sin_addr) << ":" << ntohs(_client_addr.sin_port) << ": " << message << endl;
                }
            //linux也可用int read(sock, buffer, BUFFER_SIZE);

            string response = "Hello from server!";
            if (bHttp) {
                //解析http请求
                istringstream iss(_buffer);
                string request_line;
                getline(iss, request_line);//第一行 GET/HTTP/1.1

                //解析方法和路径
                istringstream request_line_stream(request_line);
                string method, path, http_version;
                request_line_stream >> method >> path >> http_version;

                //构造响应
                if (method == "GET") {
                    //简单响应，返回 200 OK 和html内容
                    response = "HTTP/1.1 200 OK\r\n"
                               "Content-Type: text/html\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "<html><body><h1>" + response + "</h1>"
                               "<p>Requested path: " + path + "</p>"
                               "</body></html>";
                    }
                else {
                    // 不支持的 HTTP 方法（返回 405 Method Not Allowed）
                    response = "HTTP/1.1 405 Method Not Allowed\r\n"
                               "Content-Type: text/plain\r\n"
                               "Connection: close\r\n"
                               "\r\n"
                               "Method not allowed";
                    }
                }

            //发送
            send(new_socket, response.c_str(), static_cast<int>(response.length()), 0);
            //length参数，win需int，linux可隐转int，最好显转
            stop(new_socket);//关闭客户端连接
            }

        void listenUDP() {
            memset(_buffer, 0, sizeof(_buffer));
            socklen_t addrlen = sizeof(_client_addr);
            int bytes_received = recvfrom(_server_fd, _buffer, sizeof(_buffer) - 1, 0, (sockaddr*)&_client_addr, &addrlen);
            if (bytes_received == -1) {
                cerr << "Error receiving data" << endl;
                return;
                }

            string message(_buffer, bytes_received);
            cout << "Received from " << inet_ntoa(_client_addr.sin_addr) << ":" << ntohs(_client_addr.sin_port) << ": " << message << endl;

            //发送
            const string response = "Hello from server!";
            sendto(_server_fd, response.c_str(), response.length(), 0, (sockaddr*)&_client_addr, sizeof(_client_addr));
            }

#ifdef USE_SSL
        void listenHTTPS() {
            sockaddr_in client_addr{};
            socklen_t addrlen = sizeof(client_addr);

            //接收
            socklen_t client_fd = accept(_server_fd, (sockaddr*)&client_addr, &addrlen);
            if (client_fd == INVALID_SOCKET) {
                cerr << "Accept failed" << endl;
                return;
                }

            //升级为ssl连接
            SSL* ssl = SSL_new(_ctx);
            SSL_set_fd(ssl, client_fd);
            //三次tls握手，自签名证书会握手失败，不影响接收请求
            if (SSL_accept(ssl) <= 0) {
                cerr << "SSL_accept failed: " << ERR_error_string(ERR_get_error(), nullptr) << endl;
                SSL_free(ssl);
                stop(client_fd);
                return;
                }

            //处理https请求
            memset(_buffer, 0, sizeof(_buffer));
            int bytes = SSL_read(ssl, _buffer, sizeof(_buffer) - 1);  //读取加密数据
            if (bytes > 0) {
                cout << "Received HTTPS request:\n" << _buffer << endl;

                //构造响应
                string response = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: text/html\r\n"
                                  "Connection: close\r\n"
                                  "\r\n"
                                  "<html><body><h1>Hello from HTTPS Server!</h1></body></html>";

                //发送加密响应
                SSL_write(ssl, response.c_str(), response.length());
                }

            // 4. 清理
            SSL_shutdown(ssl);
            SSL_free(ssl);
            stop(client_fd);
            }
#endif

        void stop(socklen_t fd) {
            stop(fd);
            }
        void stop() {
            stop(_server_fd);
#ifdef USE_SSL
            if (_ctx) SSL_CTX_free(_ctx);
#endif
            }
    };//Server
}//net