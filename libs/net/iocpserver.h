#include <winsock2.h>//必须​在windows.h前包含
#include <mswsock.h>//AcceptEx
#include <windows.h>
#include "../common/stl.h"
#include "../thread/thread_pool_simple.h"

#define USE_POOL

// 定义每个IOCP的工作线程数
const int NUM_THREADS = 1;

mutex cout_mutex;
template<typename... Args>
void safe_print(Args&&... args) {
    lock_guard<mutex> lock(cout_mutex);
    (cout << ... << forward<Args>(args)) << endl;
}

//客户端会话结构体
struct ClientSession {
    SOCKET socket;
    sockaddr_in client_addr;
    //char buffer[1024];
    vector<char> buffer;
    WSABUF wsaBuf;
    OVERLAPPED overlapped;
    DWORD bytes_transferred;
    DWORD flags;
    enum { OP_RECV, OP_SEND } operation;  //操作类型
};

class IOCPServer {
    SOCKET _server_fd;
    HANDLE _iocp_handle;
#ifdef USE_POOL
    ThreadPoolSimple _pool;
#else
    vector<thread> _threads;
#endif
    unordered_map<SOCKET, shared_ptr<ClientSession>> _sessions;
    mutex _sessions_mutex;
    atomic<bool> _stop;
public:
    IOCPServer(int port) : _server_fd(INVALID_SOCKET), _iocp_handle(NULL), _pool(NUM_THREADS) {
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

        // 绑定地址
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

#ifdef USE_POOL
        _pool.submit([this] { WorkerThread(); });
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

    void run() {
        cout << "Server started. Waiting for connections..." << endl;

        while (!_stop) {
            //接收
            sockaddr_in client_addr;
            socklen_t addrlen = sizeof(client_addr);
            SOCKET client_fd = accept(_server_fd, (sockaddr*)&client_addr, &addrlen);
            if (client_fd == INVALID_SOCKET) {
                cerr << "accept failed: " << WSAGetLastError() << endl;
                continue;
            }

            //创建客户端会话
            auto session = make_shared<ClientSession>();
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
            session->operation = ClientSession::OP_RECV;
            if (WSARecv(client_fd, &session->wsaBuf, 1, &bytesReceived, &session->flags, &session->overlapped, NULL) == SOCKET_ERROR) {
                if (WSAGetLastError() != WSA_IO_PENDING) {
                    cerr << "WSARecv failed: " << WSAGetLastError() << endl;
                    closesocket(client_fd);
                }
            }

            safe_print("New client connected: ", inet_ntoa(client_addr.sin_addr), ":", ntohs(client_addr.sin_port));
        }//while (true)
    }//run

private:
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

            auto session = reinterpret_cast<ClientSession*>(completion_key);
            if (!session) continue;

            if (session->operation == ClientSession::OP_SEND) {
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
            session->operation = ClientSession::OP_SEND;
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
};//IOCPServer