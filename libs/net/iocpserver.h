#ifdef WIN32
#include "net.h"
#include "../common/stl.h"
#include "../thread/thread_pool_simple.h"
#include "include/protocol/rpc.h"
#include "include/protocol/http.h"
using m::net::http::Handler;
using m::net::http::IOState;

#define USE_POOL
//#define AYSNC_ACCEPT

// 定义每个IOCP的工作线程数
const int NUM_THREADS = 1;

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
    enum {
        OP_ACCEPT, OP_RECV, OP_SEND, OP_CLOSE
    , OP_TIME } operation;  //操作类型
    };

class IOCPServer {
        vector<shared_ptr<Handler>> _handlers;
        SOCKET _server_fd;
        HANDLE _iocp_handle;
#ifdef USE_POOL
        ThreadPoolSimple _pool;
#else
        vector<thread> _threads;
#endif
        unordered_map<string, m::net::rpc::HandlerType> _rpc_funcs;
        unordered_map<SOCKET, shared_ptr<Session>> _sessions;
        mutex _sessions_mutex;
        atomic<bool> _stop;
    public:
        IOCPServer(int port) : _server_fd(INVALID_SOCKET), _iocp_handle(NULL), _pool(NUM_THREADS), _rpc_funcs{} {
            //初始化 Winsock
            WSADATA wsaData;
            if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
                throw runtime_error("WSAStartup failed.");
                }

            // 创建监听套接字
            _server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (_server_fd == INVALID_SOCKET) {
                throw runtime_error("socket failed.");
                }

            // 设置 SO_REUSEADDR 允许端口复用
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
            if (CreateIoCompletionPort((HANDLE)_server_fd, _iocp_handle, (ULONG_PTR)nullptr, 0) == NULL) {
                throw runtime_error("Failed to associate listen socket with IOCP");
            }

#ifdef USE_POOL
            //_pool.submit([this] { WorkerThread(); });
#else
            //创建线程池
            for (int i = 0; i < NUM_THREADS; ++i) {
                _threads.emplace_back(&IOCPServer::WorkerThread, this);//立即执行，在工作函数里阻塞等待iocp事件
                }
#endif
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
            cout << "Server started. Waiting for connections..." << endl;

#ifdef AYSNC_ACCEPT
            for (int i = 0; i < 1; ++i) {
                post_accept();
            }
#endif

            while (!_stop) {
                //TODO 改成预先投递多个AcceptEx，避免新连接到来时来不及处理
#ifdef AYSNC_ACCEPT//异步接收
#else //阻塞接收
                sockaddr_in client_addr;
                socklen_t addrlen = sizeof(client_addr);
                SOCKET client_fd = accept(_server_fd, (sockaddr*)&client_addr, &addrlen);
                if (client_fd == INVALID_SOCKET) {
                    cerr << "accept failed: " << WSAGetLastError() << endl;
                    continue;
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
                    continue;
                }
                //异步接收
                DWORD bytesReceived = 0;
                session->operation = Session::OP_RECV;
                if (WSARecv(client_fd, &session->wsaBuf, 1, &bytesReceived, &session->flags, &session->overlapped, NULL) == SOCKET_ERROR) {
                    if (WSAGetLastError() != WSA_IO_PENDING) {
                        cerr << "WSARecv failed: " << WSAGetLastError() << endl;
                        closesocket(client_fd);
                    }
                }

                safe_print("New client connected: ", inet_ntoa(client_addr.sin_addr), ":", ntohs(client_addr.sin_port));
#endif

                _pool.submit([&, handlers = _handlers]() {

#if 0//如果收不到客户端连接，用WSAWaitForMultipleEvents测试是否能接收
                    WSAEVENT hEvent = WSACreateEvent();
                    if (WSAEventSelect(_server_fd, hEvent, FD_ACCEPT) == SOCKET_ERROR) {
                        cerr << "WSAEventSelect failed: " << WSAGetLastError() << endl;
                        return;
                    }
                    if (WSAWaitForMultipleEvents(1, &hEvent, FALSE, 5000, FALSE) == WSA_WAIT_EVENT_0) {//超时前接收到则会打印
                        safe_print("New connection arrived (FD_ACCEPT)");
                    }
                    WSACloseEvent(hEvent);
                    safe_print("Server FD: ", _server_fd);
#endif

#if 0//如果收不到客户端连接，用select测试是否能接收
                    fd_set readSet;
                    FD_ZERO(&readSet);
                    FD_SET(_server_fd, &readSet);
                    timeval timeout = { 5, 0 }; // 5秒超时
                    if (select(0, &readSet, NULL, NULL, &timeout) > 0) {//超时前接收到则会打印
                        safe_print("select detected new connection");
                    }
                    else {
                        safe_print("select timeout");
                    }
#endif

                    DWORD bytes_transferred = 0;
                    ULONG_PTR completion_key = 0;
                    LPOVERLAPPED overlapped = nullptr;
                    BOOL result = GetQueuedCompletionStatus(
                        _iocp_handle,
                        &bytes_transferred,/*客户端断开则=0*/
                        &completion_key,
                        &overlapped,
                        INFINITE /*设为0立即返回*/
                    );

                    auto session = reinterpret_cast<Session*>(completion_key);
                    if (!session) return;
                    if (!result || bytes_transferred == 0) {
                        // 客户端断开连接
                        safe_print("Client disconnected");
                        closesocket(session->socket);
                        return;
                    }

                    if (session->operation == Session::OP_SEND) {
                        return;  //忽略发送完成事件
                    }
                    safe_print("Received from ", inet_ntoa(session->client_addr.sin_addr), ": ", session->buffer.data());


                    // 如果是 AcceptEx 完成（新连接）
                    if (session->operation == Session::OP_ACCEPT) {
                        // 获取客户端地址
                        sockaddr_in* localAddr = nullptr;
                        sockaddr_in* remoteAddr = nullptr;
                        int localLen = sizeof(sockaddr_in) + 16;
                        int remoteLen = sizeof(sockaddr_in) + 16;
                        GetAcceptExSockaddrs(
                            session->buffer.data(),  // AcceptEx 的缓冲区
                            0,                       // 未接收数据
                            localLen,                // 本地地址大小
                            remoteLen,               // 远程地址大小
                            (sockaddr**)&localAddr,  // 输出本地地址
                            &localLen,
                            (sockaddr**)&remoteAddr, // 输出远程地址
                            &remoteLen
                        );

                        safe_print("New client connected: ", inet_ntoa(remoteAddr->sin_addr), ":", ntohs(remoteAddr->sin_port));

                        // 继续投递新的 AcceptEx（保持并发）
                        post_accept();

                        // 投递 WSARecv 开始接收数据
                        DWORD bytesReceived = 0;
                        session->operation = Session::OP_RECV;
                        if (WSARecv(
                            session->socket,
                            &session->wsaBuf,
                            1,
                            &bytesReceived,
                            &session->flags,
                            &session->overlapped,
                            NULL
                        ) == SOCKET_ERROR) {
                            if (WSAGetLastError() != WSA_IO_PENDING) {
                                safe_print("WSARecv failed: ", WSAGetLastError());
                                closesocket(session->socket);
                            }
                        }
                    }//if (session->operation == ClientSession::OP_ACCEPT)

                    //检查套接字内核事件队列，决定是否继续连接
                    WSANETWORKEVENTS events;
                    if (WSAEnumNetworkEvents(session->socket, NULL, &events) == SOCKET_ERROR) {
                        int err = WSAGetLastError();
                        if (err == WSAENOTSOCK || err == WSAENOTCONN) {  // 连接已失效
                            safe_print("Client disconnected (WSAError: ", err, ")");
                            closesocket(session->socket);
                            return;
                        }
                    }

                    string_view default_html_dir = "/var/www/html";
                    //auto state = handlers[session->socket]->work(default_html_dir, _rpc_funcs);
                    //if (state == IOState::PENDING) {
                    //    handlers[session->socket]->post_recv(session->socket);
                    //}
                    //else {
                    //    handlers[session->socket]->post_send(session->socket);
                    //}
                    });
                }//while (true)
            }//run

    private:
        //投递一个AcceptEx异步请求
        void post_accept() {
            if (false) {//有效，定位是客户端套接字没先关联iocp
                // 将监听套接字关联到 IOCP
                //CreateIoCompletionPort((HANDLE)_server_fd, _iocp_handle, (ULONG_PTR)nullptr, 0);

                // 投递一个 AcceptEx 请求（需动态加载 AcceptEx）
                LPFN_ACCEPTEX AcceptEx = nullptr;
                GUID guid = WSAID_ACCEPTEX;
                DWORD bytesReturned;
                WSAIoctl(_server_fd, SIO_GET_EXTENSION_FUNCTION_POINTER, &guid, sizeof(guid), &AcceptEx, sizeof(AcceptEx), &bytesReturned, NULL, NULL);

                SOCKET client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                char buffer[1024];
                OVERLAPPED ov = { 0 };
                AcceptEx(_server_fd, client_fd, buffer, 0, sizeof(sockaddr_in) + 16, sizeof(sockaddr_in) + 16, NULL, &ov);
                return;
            }

            //创建客户端套接字
            SOCKET client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (client_fd == INVALID_SOCKET) {
                cerr << "socket failed: " << WSAGetLastError() << endl;
                return;
            }

            //创建会话对象
            auto session = make_shared<Session>();
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
                return;
            }

            // 投递 AcceptEx
            DWORD bytesReceived = 0;
            if (AcceptEx(
                _server_fd,                  // 监听套接字
                client_fd,                   // 客户端套接字（预先创建）
                session->buffer.data(),      // 接收缓冲区（可选，可用于获取首个数据包）
                0,                           // 不接收数据（设为 0）
                sizeof(sockaddr_in) + 16,    // 本地地址大小
                sizeof(sockaddr_in) + 16,    // 远程地址大小
                &bytesReceived,              // 实际接收的字节数（OUT 参数）
                &session->overlapped         // OVERLAPPED 结构
            ) == FALSE) {
                // 正常情况下，AcceptEx 会返回 FALSE，并且 WSAGetLastError = ERROR_IO_PENDING
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    cerr << "AcceptEx failed: " << WSAGetLastError() << endl;
                    closesocket(client_fd);
                    return;
                }
            }

            // 存储会话
            {
                lock_guard<mutex> lock(_sessions_mutex);
                _sessions[client_fd] = session;
            }
        }//post_accept

        void WorkerThread() {
            while (!_stop) {
                DWORD bytes_transferred = 0;
                ULONG_PTR completion_key = 0;
                LPOVERLAPPED overlapped = nullptr;

                //阻塞等待iocp事件(WSARecv/WSASend完成/客户端断开连接/手动投递事件PostQueuedCompletionStatus)
                BOOL result = GetQueuedCompletionStatus(
                                  _iocp_handle,
                                  &bytes_transferred,/*客户端断开则=0*/
                                  &completion_key,
                                  &overlapped,
                                  INFINITE /*设为0立即返回*/
                              );
                //BOOL是win api，S_OK=FALSE=0

                auto session = reinterpret_cast<Session*>(completion_key);
                if (!session) continue;

                if (session->operation == Session::OP_SEND) {
                    continue;  //忽略发送完成事件
                    }

                //判断客户端是否断开连接(暂时没测出有效性)
                if (!result || bytes_transferred == 0) {
                    if (session) {
                        safe_print("Client disconnected: ", inet_ntoa(session->client_addr.sin_addr), ":", ntohs(session->client_addr.sin_port));
                        closesocket(session->socket);
                        session->socket = INVALID_SOCKET;
                        }
                    continue;
                    }

                //显示数据，实际为"message\0\0\0..."
                safe_print("Received from ", inet_ntoa(session->client_addr.sin_addr), ": ", session->buffer.data());

                //识别字符判断是否关闭
                if (string_view(session->buffer.data(), session->buffer.size()).substr(0, 5) == "close") {
                    safe_print("Received close command, shutting down server...");
                    //发送fin包，若客户端已断开，会返回SOCKET_ERROR，且WSAGetLastError置为10053(WSAECONNABORTED)或10054(WSAECONNRESET)
                    //shutdown(session->socket, SD_SEND);

                    //关闭套接字，置为INVALID_SOCKET
                    closesocket(session->socket);
                    //之后WSASend/WSARecv会报错10058(WSAESOCKTNOSUPPORT)
                    stop();
                    return;
                    }

                //检查套接字内核事件队列，决定是否继续连接，有切换内核态开销，不可频繁使用，应用心跳
                WSANETWORKEVENTS events;
                if (WSAEnumNetworkEvents(session->socket, NULL, &events) == SOCKET_ERROR) {
                    int err = WSAGetLastError();
                    if (err == WSAENOTSOCK || err == WSAENOTCONN) {  // 连接已失效
                        safe_print("Client disconnected (WSAError: ", err, ")");
                        closesocket(session->socket);
                        continue;
                        }
                    }
                //检查客户端是否已发送rst触发FD_CLOSE
                if (events.lNetworkEvents & FD_CLOSE) {
                    safe_print("Client gracefully closed connection");
                    closesocket(session->socket);
                    continue;
                    }
                //getsockopt/select/WSASend都是内核缓冲区操作，不能及时检测

                //修改数据
                string response = "Hello from server!";
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
                        continue;
                        }
                    }
                //WSASend的&session->overlapped替换为NULL则不会触发iocp事件
                }//while (true)
            }//work

        void stop() {
            if (_stop) return;
            _stop = true;
            //关闭服务器套接字
            if (_server_fd != INVALID_SOCKET) {
                closesocket(_server_fd);
                _server_fd = INVALID_SOCKET;
                }
            //关闭所有客户端连接
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
            //关闭IOCP句柄
            if (_iocp_handle != NULL) {
                CloseHandle(_iocp_handle);
                _iocp_handle = NULL;
                }
            }//stop
        private:

    };//IOCPServer
#endif