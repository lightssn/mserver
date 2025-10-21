#ifdef WIN32
#include "net.h"
#include "../common/stl.h"
#include "../thread/thread_pool_simple.h"
#include "include/protocol/rpc.h"
#include "include/protocol/http.h"
using m::net::http::Handler;
using m::net::http::IOState;
using m::net::http::RequestParser;
using m::net::http::ResponseBuffer;
using m::net::http::ResponseCode;

#define USE_POOL
#define AYSNC_ACCEPT

// 定义每个IOCP的工作线程数
const int NUM_THREADS = 5;

mutex cout_mutex;
template<typename... Args>
void safe_print(Args&&... args) {
    lock_guard<mutex> lock(cout_mutex);
    (cout << ... << forward<Args>(args)) << endl;
    }

//iocp会话结构体
struct Session {
    SOCKET socket;
    sockaddr_in client_addr;
    vector<char> buffer;
    WSABUF wsaBuf;
    OVERLAPPED overlapped;
    DWORD bytes_transferred;
    DWORD flags;
    string fun_str;
    enum {
        OP_ACCEPT, OP_RECV, OP_SEND, OP_CLOSE
        , OP_TIME
        } operation;  //操作类型
    };

class IOCPServer {
        vector<shared_ptr<Handler>> _handlers;
        SOCKET _server_fd;
        HANDLE _iocp_handle;
        RequestParser _request_parser;
        ResponseBuffer _response_buffer;//http响应数据
#ifdef USE_POOL
        ThreadPoolSimple _pool;
#else
        vector<thread> _threads;
#endif
        unordered_map<string, m::net::rpc::HandlerType> _rpc_funcs;
        unordered_map<SOCKET, shared_ptr<Session>> _sessions;
        mutex _sessions_mutex;
        bool _keep_alive;
        atomic<bool> _stop;
    public:
        IOCPServer(int port) : _server_fd(INVALID_SOCKET), _iocp_handle(NULL), _pool(NUM_THREADS), _rpc_funcs{} {
            //初始化Winsock
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw runtime_error("WSAStartup failed.");
                }

            //创建服务端套接字
            _server_fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            //_server_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (_server_fd == INVALID_SOCKET) {
                throw runtime_error("socket failed.");
                }

            //设置 SO_REUSEADDR允许端口复用
            int reuseAddr = 1;
            if (setsockopt(_server_fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr)) == SOCKET_ERROR) {
                throw runtime_error("setsockopt failed.");
                }

            //绑定地址
            sockaddr_in server_addr;
            server_addr.sin_family = AF_INET;
            server_addr.sin_addr.s_addr = INADDR_ANY;
            server_addr.sin_port = htons(port);

            if (bind(_server_fd, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
                cerr << "bind failed: " << WSAGetLastError() << endl;
                throw runtime_error("bind failed: " + WSAGetLastError());
                }

            //监听
            if (listen(_server_fd, SOMAXCONN) == SOCKET_ERROR) {
                throw runtime_error("listen failed: " + WSAGetLastError());
                }

            //创建iocp句柄
            _iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, NUM_THREADS);
            if (_iocp_handle == NULL) {
                throw runtime_error("CreateIoCompletionPort failed: " + GetLastError());
                }

            //服务端套接字关联到iocp
            if (CreateIoCompletionPort((HANDLE)_server_fd, _iocp_handle, (ULONG_PTR)_server_fd, 0) == NULL) {
                throw runtime_error("Failed to associate listen socket with IOCP");
                }
            //第三个参数替换为nullptr则不会识别套接字
            }//IOCPServer

        ~IOCPServer() {
            stop();
            WSACleanup();
            }

        template <typename Func>
        //将RPC远程过程调用的函数注册到映射表。将函数名与默认路由前缀组合成 URL，并将其与一个包装后的函数一起插入到映射表中，实现 RPC 函数的注册和调用。并处理 JSON 解析和类型转换的错误情况
        bool rpc_register(string const& name, Func func) {
            auto url = string{ m::net::rpc::default_route_prefix } + name;//拼接完整url
            if (_rpc_funcs.find(url) != _rpc_funcs.end())//如果映射表已存在该url，注册失败，返回false
                return false;

            //lambda函数，处理url
            _rpc_funcs[url] = [func](string_view content) -> string {
                using namespace MyJson;
                auto j = Json::from_json_text(content);//将lambda输入的content解析为json
                if (!j.has_value())//解析失败时，返回错误信息的json
                    return R"({"rpc_error":"broken client json"})";
                using ArgT = remove_cv_t<remove_reference_t<m::net::rpc::FirstArgT<Func>>>;//获取输入func的第一个参数类型
                //      static_assert(is_same_v<ArgT, int>);
                auto arg = j.value().to_type<ArgT>();//将解析后的json转为该类型
                if (!arg.has_value())//转换失败时，返回错误信息的json
                    return R"({"rpc_error":"broken client arg"})";
                return Json{ func(arg.value()) }.to_json_text();//调用func处理并将结果转为json返回
                };
            return true;
            }//rpc_register

        void run() {
#ifdef _DEBUG
            safe_print("thread ", get_id(), ": sever waiting for connections...");
#endif

#ifdef AYSNC_ACCEPT
            for (int i = 0; i < NUM_THREADS; ++i) {
                accept_async_session();
                }
#endif

            while (!_stop) {
                //TODO 改成预先投递多个AcceptEx，避免新连接到来时来不及处理
#ifdef AYSNC_ACCEPT//异步接收，不能放在这，不然无限投递
#else //阻塞接收
                accept_sync();
#endif

                Session* session = get_event();//阻塞取iocp事件
                if (!session) continue;

                if (session->operation == Session::OP_SEND) {
                    continue;  //忽略发送完成事件
                    }
                if (session->operation == Session::OP_ACCEPT) {//新连接事件(AcceptEx完成)
                    post_accept(session);
                    }
                else {//WSARecv事件
                    IOState state = post_recv(session);
                    if (state != IOState::OK) {
                        //关闭套接字，置为INVALID_SOCKET
                        closesocket(session->socket);
                        _sessions.erase(session->socket);
                        //之后WSASend/WSARecv会报错10058(WSAESOCKTNOSUPPORT)
                        //stop();
                        continue;
                        }
#ifdef USE_POOL
                    _pool.submit([&, session = session]() {
                        auto state = post_work(session);
                        if (state == IOState::PENDING) {
                            post_recv(session);
                        }
                        else {
                            if (post_check(session) == IOState::OK) {
                                post_send(session);
                            }
                        }
                        });
#else
                    //创建线程池
                    for (int i = 0; i < NUM_THREADS; ++i) {
                        _threads.emplace_back(&IOCPServer::WorkerThread, this);//立即执行
                        }
#endif
                    }
                }//while (true)
            }//run

    private:
        //投递同步阻塞accept，弃用，未测试
        bool accept_sync() {
            sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            SOCKET client_fd = accept(_server_fd, (sockaddr*)&client_addr, &addrlen);
            if (client_fd == INVALID_SOCKET) {
                cerr << "accept failed: " << WSAGetLastError() << endl;
                return false;
                }
            //创建客户端会话
            auto session = make_shared<Session>();
            session->socket = client_fd;
            session->client_addr = client_addr;
            session->buffer.resize(1024); //预分配，避免重分配
            //执行push_back、resize后，vector可能重新分配内存，导致data()指针失效，要重新绑定wsaBuf.buf
            session->wsaBuf.buf = session->buffer.data();
            session->wsaBuf.len = session->buffer.size();
            session->bytes_transferred = 0;
            session->flags = 0;
            ZeroMemory(&session->overlapped, sizeof(OVERLAPPED));
                {
                lock_guard<mutex> lock(_sessions_mutex);
                _sessions[client_fd] = session;
                }

            //客户端套接字关联到iocp
            if (CreateIoCompletionPort((HANDLE)client_fd, _iocp_handle, (ULONG_PTR)session.get(), 0) == NULL) {
                cerr << "CreateIoCompletionPort for client failed: " << GetLastError() << endl;
                closesocket(client_fd);
                return false;
                }
            //异步接收
            DWORD bytesReceived = 0;
            session->operation = Session::OP_RECV;
            if (WSARecv(client_fd, &session->wsaBuf, 1, &bytesReceived, &session->flags, &session->overlapped, NULL) == SOCKET_ERROR) {
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    cerr << "WSARecv failed: " << WSAGetLastError() << endl;
                    closesocket(client_fd);
                    return false;
                    }
                }

            safe_print("New client connected: ", inet_ntoa(client_addr.sin_addr), ":", ntohs(client_addr.sin_port));
            return true;
            }//accept_sync

        //须先同步或异步投递，否则无法接收
        bool accept_async_session() {
            //创建客户端套接字
            SOCKET client_fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (client_fd == INVALID_SOCKET) {
                cerr << "socket failed: " << WSAGetLastError() << endl;
                return false;
                }

            //创建会话对象
            auto session = make_shared<Session>();
            //Session* session = new Session();
            session->socket = client_fd;
            session->buffer.resize(1024);
            session->wsaBuf.buf = session->buffer.data();
            session->wsaBuf.len = session->buffer.size();
            session->operation = Session::OP_ACCEPT;
            ZeroMemory(&session->overlapped, sizeof(OVERLAPPED));

            //客户端套接字关联到iocp，须在投递前操作
            if (CreateIoCompletionPort((HANDLE)client_fd, _iocp_handle, (ULONG_PTR)session.get(), 0) == NULL) {
                cerr << "Failed to associate client socket with IOCP: " << GetLastError() << endl;
                closesocket(client_fd);
                return false;
                }

            //投递AcceptEx
            GUID guid = WSAID_ACCEPTEX;
            LPFN_ACCEPTEX AcceptEx;
            DWORD bytes;
            WSAIoctl(_server_fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &AcceptEx, sizeof(AcceptEx), &bytes, NULL, NULL);

            DWORD bytes2;
            if (AcceptEx(
                        _server_fd,                  // 监听套接字
                        client_fd,                   // 客户端套接字（预先创建）
                        session->buffer.data(),      // 接收缓冲区（可选，可用于获取首个数据包）
                        0,                           // 不接收数据（设为 0）
                        sizeof(sockaddr_in) + 16,    // 本地地址大小
                        sizeof(sockaddr_in) + 16,    // 远程地址大小
                        &bytes2,              // 实际接收的字节数（OUT 参数）
                        &session->overlapped         // OVERLAPPED 结构
                    ) == FALSE) {
                // 正常情况下，AcceptEx 会返回 FALSE，并且 WSAGetLastError = ERROR_IO_PENDING
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    cerr << "AcceptEx failed: " << WSAGetLastError() << endl;
                    closesocket(client_fd);
                    return false;
                    }
                }

            //存储会话
                {
                lock_guard<mutex> lock(_sessions_mutex);
                _sessions[client_fd] = session;
                }
            }//accept_async_session

        void stop() {
            if (_stop) return;
            _stop = true;

#ifdef USE_POOL
            _pool.stop();
#else
            //通知所有工作线程退出
            for (size_t i = 0; i < _threads.size(); ++i) {
                PostQueuedCompletionStatus(_iocp_handle, 0, 0, nullptr);
                }
            //等待所有线程结束
            for (auto& thread : _threads) {
                if (thread.joinable()) {
                    thread.join();
                    }
                }
            _threads.clear();
#endif
            //关闭服务器套接字
            if (_server_fd != INVALID_SOCKET) {
                closesocket(_server_fd);
                _server_fd = INVALID_SOCKET;
                }
            //关闭所有客户端套接字
                {
                lock_guard<mutex> lock(_sessions_mutex);
                for (auto& pair : _sessions) {
                    if (pair.second->socket != INVALID_SOCKET) {
                        closesocket(pair.second->socket);
                        pair.second->socket = INVALID_SOCKET;
                        }
                    }
                _sessions.clear();
                }
            //关闭icop句柄
            if (_iocp_handle != NULL) {
                CloseHandle(_iocp_handle);
                _iocp_handle = NULL;
                }
            }//stop

        //获取事件
        Session* get_event() {
            DWORD bytes = 0;
            ULONG_PTR completion_key = 0;
            LPOVERLAPPED overlapped = nullptr;
            BOOL result = GetQueuedCompletionStatus(
                              _iocp_handle,
                              &bytes,/*客户端断开则=0*/
                              &completion_key,
                              &overlapped,
                              INFINITE /*设为0立即返回*/
                          );

            if (!result || !overlapped) {
                DWORD error = GetLastError();
                // 客户端断开连接
                safe_print("Client disconnected");
                return nullptr;
                }

            //取出会话对象
            Session* session = CONTAINING_RECORD(overlapped, Session, overlapped);//用win api从成员变量地址反推对象地址
            //auto session = reinterpret_cast<Session*>(completion_key);//也有效，需在新连接事件重新关联iocp
            if (bytes == 0) {//新连接
                session->operation = Session::OP_ACCEPT;
                }
            else {//数据到达
                //session->operation = Session::OP_RECV;
                session->bytes_transferred = bytes;
            }
            return session;
            }//get_event

        //新连接事件
        void post_accept(Session* session) {
            SOCKET client_fd = session->socket;
            sockaddr_in client_addr = session->client_addr;

            // 解析 AcceptEx 缓冲区中的远程地址
            sockaddr_in* remote_addr = reinterpret_cast<sockaddr_in*>(session->buffer.data() + sizeof(sockaddr_in) + 16);
            session->client_addr = *remote_addr;

            setsockopt(session->socket, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&_server_fd, sizeof(_server_fd));
            //重新关联客户端套接字到 IOCP（使用 Session* 作为 completion_key）
            CreateIoCompletionPort((HANDLE)client_fd, _iocp_handle, (ULONG_PTR)session, 0);
            //safe_print("Get AcceptEx from ", inet_ntoa(session->client_addr.sin_addr), ":", ntohs(session->client_addr.sin_port));

            //继续投递AcceptEx，保持并发
            accept_async_session();

            //投递读请求
            DWORD bytes = 0;
            session->operation = Session::OP_RECV;
            WSARecv(client_fd, &session->wsaBuf, 1, &bytes, &session->flags, &session->overlapped, NULL);
            }//post_accept

        //读事件
        IOState post_recv(Session* session) {
#ifdef _DEBUG
            //显示数据，实际为"message\0\0\0..."
            safe_print(/*"thread ", get_id(), ": ",*/ "received from ", inet_ntoa(session->client_addr.sin_addr), ":", ntohs(session->client_addr.sin_port), " : ", session->buffer.data());
#endif
            //识别字符判断是否关闭
            if (string_view(session->buffer.data(), session->buffer.size()).substr(0, 5) == "close") {
                safe_print("Received close command, shutting down server...");
                //发送fin包，若客户端已断开，会返回SOCKET_ERROR，且WSAGetLastError置为10053(WSAECONNABORTED)或10054(WSAECONNRESET)
                //shutdown(session->socket, SD_SEND);
                return IOState::BAD;
                }
            return IOState::OK;
            }//post_recv

        //写事件
        void post_send(Session* session) {
            //构造响应头
            string response = _response_buffer.s;
                //"HTTP/1.1 200 OK\r\n"
                //"Content-Type: text/plain\r\n"
                //"Content-Length: 17\r\n"
                //"Connection: keep-alive\r\n"
                //"\r\n"
                //"Hello from server!";
            session->buffer.assign(response.begin(), response.end());
            session->wsaBuf.buf = session->buffer.data();
            session->wsaBuf.len = session->buffer.size();
            ZeroMemory(&session->overlapped, sizeof(OVERLAPPED));

            //发送数据
            DWORD bytesSent = 0;
            session->operation = Session::OP_SEND;
            if (WSASend(session->socket, &session->wsaBuf, 1, &bytesSent, 0, &session->overlapped, NULL) == SOCKET_ERROR) {
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    safe_print("WSASend failed: ", WSAGetLastError());
                    closesocket(session->socket);
                    _sessions.erase(session->socket);
                    return;
                    }
                }
            }//post_send

        IOState post_check(Session* session) {
            //检查套接字内核事件队列，决定是否继续连接，有切换内核态开销，不可频繁使用，应用心跳
            WSANETWORKEVENTS events;
            if (WSAEnumNetworkEvents(session->socket, NULL, &events) == SOCKET_ERROR) {
                int err = WSAGetLastError();
                if (err == WSAENOTSOCK || err == WSAENOTCONN) {  // 连接已失效
                    safe_print("Client disconnected (WSAError: ", err, ")");
                    closesocket(session->socket);
                    _sessions.erase(session->socket);
                    return IOState::BAD;
                    }
                }
            //检查客户端是否已发送rst触发FD_CLOSE
            if (events.lNetworkEvents & FD_CLOSE) {
                safe_print("Client gracefully closed connection");
                closesocket(session->socket);
                _sessions.erase(session->socket);
                return IOState::BAD;
                }
            //getsockopt/select/WSASend都是内核缓冲区操作，不能及时检测
            return IOState::OK;
            }//post_check

        IOState post_work(Session* session) {
            //解析读取缓冲区中的请求内容
            string m_read_buffer = session->buffer.data();
            int m_read_index = session->bytes_transferred;
            auto state = _request_parser.update(string_view(m_read_buffer.data(), m_read_index));
            if (state == IOState::PENDING)
                return IOState::PENDING;//请求数据不完整，直接返回
            else if (state == IOState::BAD)//请求格式错误
                _response_buffer.code = ResponseCode::BAD_REQUEST;
            auto& request = _request_parser.unwrap();//解析后的请求对象的引用

            //处理rpc请求
            if constexpr (true) {
                auto rpc_url = string{ request.url };
                //检查请求url是否在rpc函数映射表中
                if (_rpc_funcs.find(rpc_url) != _rpc_funcs.end()) {
                    //如果存在，调用该rpc函数，传入请求，得到响应结果res
                    auto res = _rpc_funcs[rpc_url](request.content);
                    _keep_alive = request.keep_alive;
                    //构建http响应头和响应体
                    stringstream ss;
                    ss << "HTTP/1.1 200 OK\r\n";
                    ss << "Connection: " << (_keep_alive ? "keep-alive" : "close") << "\r\n";
                    ss << "Content-Length: " << res.size() << "\r\n\r\n";
                    ss << res;
                    _response_buffer.s = ss.str();
                    return IOState::OK;
                    }
                }//constexpr
            }//post_work
    };//IOCPServer
#endif