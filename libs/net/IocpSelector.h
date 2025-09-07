#ifdef _WIN32
#include "net.h"
#include "../common/stl.h"
#include <cerrno>
#include <fmt/format.h>

constexpr ULONG_PTR TIMER_KEY = 0x12345678;
// 定义每个IOCP的工作线程数
const int NUM_THREADS = 1;

// 全局 GUID（用于获取 AcceptEx 函数指针）
static GUID guidAcceptEx = WSAID_ACCEPTEX;

struct Session : public OVERLAPPED {
    enum class Type { READ, WRITE };
    Type type;
    WSABUF wsabuf;
    char buffer[8192];
    OVERLAPPED overlapped;
    Session(Type t) : type(t) {
        memset(this, 0, sizeof(OVERLAPPED));
        wsabuf.buf = buffer;
        wsabuf.len = sizeof(buffer);
    }
};//Session

class IOCPSelector {
        HANDLE _iocp_handle;
        SOCKET _server_fd;
        unordered_map<SOCKET, shared_ptr<Session>> _sessions;
        mutex _sessions_mutex;
    public:
        IOCPSelector(int server_fd, size_t /*size*/)
            : _server_fd(server_fd) {
            //创建iocp句柄
            _iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, NUM_THREADS);
            if (_iocp_handle == NULL) {
                throw runtime_error("CreateIoCompletionPort failed: " + GetLastError());
                }
            }//IOCPSelector()

        ~IOCPSelector() {
            CloseHandle(_iocp_handle);
            }

        void register_timer(int fd) {
            //IOCP不需要显式注册定时器
            //只需将socket与IOCP关联
            }

        void bind_timer(HANDLE hTimer) {
            if (CreateIoCompletionPort(hTimer, _iocp_handle, TIMER_KEY, 0) == NULL) {
                CloseHandle(hTimer);
                throw runtime_error("Failed to bind timer to IOCP(bind_timer)");
                }
            }

        void register_on_listening_lt(SOCKET client_fd) const {
            auto session = make_shared<Session>();
            //客户端套接字关联到iocp
            if (CreateIoCompletionPort((HANDLE)client_fd, _iocp_handle, (ULONG_PTR)session.get(), 0) == NULL) {
                throw runtime_error("CreateIoCompletionPort for client failed");
                }
            _sessions[client_fd] = session;
            //提交初始读取操作
            post_recv(client_fd, session);
            }

        void register_on_reading(SOCKET client_fd, bool one_shot = true, bool blocking = false) const {
            auto session = _sessions[client_fd];
            post_recv(client_fd, session);
            }

        void unregister(SOCKET client_fd) const {
            closesocket(client_fd);
            }

        void read_again_on(SOCKET client_fd) const {
            auto session = _sessions[client_fd];
            post_recv(client_fd, session);
            }

        void write_again_on(SOCKET client_fd) const {
            auto session = _sessions[client_fd];
            post_recv(client_fd, session);
            }

        struct Event {
            enum class Tag { CONNECTION, READ, WRITE, CLOSE, TIME };
            Tag tag;
            int fd;
            };

        Event get_next_event() {
            DWORD bytes_transferred = 0;
            ULONG_PTR completion_key = 0;
            LPOVERLAPPED overlapped = nullptr;

            if (!GetQueuedCompletionStatus(_iocp_handle, &bytes_transferred,
                                           &completion_key, &overlapped, INFINITE)) {
                DWORD error = GetLastError();
                if (overlapped == nullptr) {
                    throw runtime_error(
                        fmt::format("GetQueuedCompletionStatus失败: {}", error));
                    }

                // socket发生错误
                return { Event::Tag::CLOSE, static_cast<int>(completion_key) };
                }

            if (bytes_transferred == 0) {
                // 连接正常关闭
                return { Event::Tag::CLOSE, static_cast<int>(completion_key) };
                }

            if (completion_key == TIMER_KEY) {
                return { Event::Tag::TIME, 0 };
                }

            if (completion_key == _server_fd) {
                // 新连接
                return { Event::Tag::CONNECTION, 0 };
                }

            // 判断是读操作还是写操作完成
            auto* operation = reinterpret_cast<Session*>(overlapped);
            if (operation->type == Session::Type::READ) {
                return { Event::Tag::READ, static_cast<int>(completion_key) };
                }
            else {
                return { Event::Tag::WRITE, static_cast<int>(completion_key) };
                }
            }//get_next_event

        std::tuple<SOCKET, sockaddr_in> accept_async(SOCKET listenSocket) {
            // 1. 创建客户端套接字
            SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
            if (clientSocket == INVALID_SOCKET) {
                throw runtime_error("Failed to create client socket");
                }

            // 2. 动态加载 AcceptEx 函数指针
            LPFN_ACCEPTEX lpfnAcceptEx = nullptr;
            DWORD bytesReturned = 0;
            if (WSAIoctl(
                        listenSocket,
                        SIO_GET_EXTENSION_FUNCTION_POINTER,
                        &guidAcceptEx,
                        sizeof(guidAcceptEx),
                        &lpfnAcceptEx,
                        sizeof(lpfnAcceptEx),
                        &bytesReturned,
                        NULL,
                        NULL
                    ) != 0) {
                closesocket(clientSocket);
                throw runtime_error("Failed to load AcceptEx");
                }

            // 3. 准备异步操作参数
            char acceptBuffer[sizeof(sockaddr_in) * 2 + 32];  // 存储客户端地址
            DWORD bytesReceived = 0;
            OVERLAPPED overlapped;
            ZeroMemory(&overlapped, sizeof(overlapped));

            // 4. 调用 AcceptEx
            BOOL result = lpfnAcceptEx(
                              listenSocket,
                              clientSocket,
                              acceptBuffer,
                              0,  // 不接收数据，仅接受连接
                              sizeof(sockaddr_in) + 16,
                              sizeof(sockaddr_in) + 16,
                              &bytesReceived,
                              &overlapped
                          );
            // 5. 检查错误（WSA_IO_PENDING 是正常情况）
            if (!result) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    closesocket(clientSocket);
                    throw runtime_error("AcceptEx failed");
                    }
                }
            // 6. 等待 IOCP 完成通知
            DWORD bytesTransferred;
            ULONG_PTR completionKey;
            OVERLAPPED* lpOverlapped;
            if (!GetQueuedCompletionStatus(
                        _iocp_handle,
                        &bytesTransferred,
                        &completionKey,
                        &lpOverlapped,
                        INFINITE
                    )) {
                closesocket(clientSocket);
                throw runtime_error("AcceptEx completion failed");
                }
            // 7. 解析客户端地址
            sockaddr_in clientAddr;
            sockaddr_in* localAddr = nullptr;
            sockaddr_in* remoteAddr = nullptr;
            int localLen = 0, remoteLen = 0;
            GetAcceptExSockaddrs(
                acceptBuffer,
                0,
                sizeof(sockaddr_in) + 16,
                sizeof(sockaddr_in) + 16,
                (sockaddr**)&localAddr,
                &localLen,
                (sockaddr**)&remoteAddr,
                &remoteLen
            );
            if (remoteAddr != nullptr) {
                clientAddr = *remoteAddr;
                }
            return { clientSocket, clientAddr };
            }

    private:


        void post_recv(SOCKET client_fd, shared_ptr<Session> session) const {
            //检查套接字是否有效
            if (client_fd == INVALID_SOCKET) {
                throw runtime_error("Invalid socket");
                }
            DWORD bytesReceived = 0;
            DWORD flags = 0;
            //异步读取
            if (WSARecv(client_fd, &session->wsabuf, 1, &bytesReceived, &flags, &session->overlapped, NULL) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    throw runtime_error(
                        fmt::format("Failed to WSARecv: {}", error));
                    }
                }
            }//post_recv

        void post_send(int client_fd) const {
            auto* session = new Session(Session::Type::WRITE);
            DWORD bytes_sent = 0;

            if (WSASend(client_fd, &session->wsabuf, 1, &bytes_sent, 0, session, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    throw runtime_error(
                        fmt::format("WSASend失败: {}", error));
                    }
                }
            }//post_send
    };//IOCPSelector
#endif