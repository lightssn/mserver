#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>  // for AcceptEx
#include <cerrno>
#include <fmt/format.h>
#include <stdexcept>
#include <vector>
#include <memory>
using std::runtime_error;
constexpr ULONG_PTR TIMER_KEY = 0x12345678;

// 全局 GUID（用于获取 AcceptEx 函数指针）
static GUID guidAcceptEx = WSAID_ACCEPTEX;

class IOCPSelector {
        HANDLE _iocp_handle;
        int _listen_fd;
    public:
        IOCPSelector(int listen_fd, size_t /*size*/)
            : _listen_fd(listen_fd) {
            //创建iocp句柄
            //_iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, NUM_THREADS);
            //if (_iocp_handle == NULL) {
            //    throw runtime_error("CreateIoCompletionPort failed: " + GetLastError());
            //}


            //// 创建I/O完成端口
            //_iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            //if (_iocp_handle == NULL) {
            //    throw runtime_error("Failed to create IOCP");
            //    }

            // 将监听socket与IOCP关联
            //if (CreateIoCompletionPort((HANDLE)_listen_fd, _iocp_handle, (ULONG_PTR)_listen_fd, 0) == NULL) {
            //    CloseHandle(_iocp_handle);
            //    throw runtime_error("Failed to bind socket to IOCP");
            //    }
            }//IOCPSelector()

        ~IOCPSelector() {
            CloseHandle(_iocp_handle);
            }

        void register_timer(int fd) {
            //IOCP不需要显式注册定时器
            //只需将socket与IOCP关联
            //associate_socket_with_iocp(fd);
            }

        void bind_timer(HANDLE hTimer) {
            if (CreateIoCompletionPort(hTimer, _iocp_handle, TIMER_KEY, 0) == NULL) {
                CloseHandle(hTimer);
                throw runtime_error("Failed to bind timer to IOCP(bind_timer)");
                }
            }

        void register_on_listening_lt(int fd) const {
            associate_socket_with_iocp(fd);
            // 提交初始读取操作
            post_recv(fd);
            }

        void register_on_reading(int fd, bool one_shot = true, bool blocking = false) const {
            //associate_socket_with_iocp(fd);
            post_recv(fd);
            }

        void unregister(int fd) const {
            // 在IOCP中，关闭socket会自动将其从完成端口移除
            closesocket(fd);
            }

        void read_again_on(int fd) const {
            post_recv(fd);
            }

        void write_again_on(int fd) const {
            post_send(fd);
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

            if (completion_key == _listen_fd) {
                // 新连接
                return { Event::Tag::CONNECTION, 0 };
                }

            // 判断是读操作还是写操作完成
            auto* io_operation = reinterpret_cast<IoOperation*>(overlapped);
            if (io_operation->type == IoOperation::Type::READ) {
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
        struct IoOperation : public OVERLAPPED {
            enum class Type { READ, WRITE };
            Type type;
            WSABUF wsa_buf;
            char buffer[8192]; // 可根据需要调整大小

            IoOperation(Type t) : type(t) {
                memset(this, 0, sizeof(OVERLAPPED));
                wsa_buf.buf = buffer;
                wsa_buf.len = sizeof(buffer);
                }
            };

        void associate_socket_with_iocp(int fd) const {
            if (CreateIoCompletionPort((HANDLE)fd, _iocp_handle, (ULONG_PTR)fd, 0) == NULL) {
                throw runtime_error("Failed to bind socket to IOCP 1");
                }
            }

        void post_recv(SOCKET fd) const {
            //检查套接字是否有效
            if (fd == INVALID_SOCKET) {
                throw runtime_error("Invalid socket");
                }
            //初始化异步操作
            auto* io_operation = new IoOperation(IoOperation::Type::READ);
            DWORD flags = 0;
            //异步读取
            if (WSARecv(fd, &io_operation->wsa_buf, 1, nullptr, &flags,
                        io_operation, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete io_operation;
                    throw runtime_error(
                        fmt::format("Failed to WSARecv: {}", error));
                    }
                }
            }//post_recv

        void post_send(int fd) const {
            auto* io_operation = new IoOperation(IoOperation::Type::WRITE);
            DWORD bytes_sent = 0;

            if (WSASend(fd, &io_operation->wsa_buf, 1, &bytes_sent, 0,
                        io_operation, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete io_operation;
                    throw runtime_error(
                        fmt::format("WSASend失败: {}", error));
                    }
                }
            }//post_send
    };//IOCPSelector

    // 自定义 post_accept_ex 函数
    bool post_accept_ex(SOCKET listenSocket) {
        // 1. 创建一个新套接字（用于 AcceptEx）
        SOCKET clientSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
        if (clientSocket == INVALID_SOCKET) {
            return false;
        }

        // 2. 准备 OVERLAPPED 结构（用于 IOCP 回调）
        OVERLAPPED* overlapped = new OVERLAPPED;
        ZeroMemory(overlapped, sizeof(OVERLAPPED));

        // 3. 获取 AcceptEx 函数指针（Windows 要求动态加载）
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
            delete overlapped;
            return false;
        }

        // 4. 调用 AcceptEx（异步接收新连接）
        char acceptBuffer[1024];  // 用于接收连接数据（可选）
        DWORD receivedBytes = 0;
        BOOL result = lpfnAcceptEx(
            listenSocket,
            clientSocket,
            acceptBuffer,
            0,  // 不接收数据，仅接受连接
            sizeof(sockaddr_in) + 16,
            sizeof(sockaddr_in) + 16,
            &receivedBytes,
            overlapped
        );

        // 5. 检查是否成功（WSA_IO_PENDING 是正常情况）
        if (!result) {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                closesocket(clientSocket);
                delete overlapped;
                return false;
            }
        }
        return true;
    }
#endif