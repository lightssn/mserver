#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cerrno>
#include <fmt/format.h>
#include <stdexcept>
#include <vector>
#include <memory>

class IocpSelector {
        HANDLE m_iocp_handle;
        int m_listen_fd;
    public:
        IocpSelector(int listen_fd, size_t /*size*/)
            : m_listen_fd(listen_fd) {
            // 创建I/O完成端口
            m_iocp_handle = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
            if (m_iocp_handle == NULL) {
                throw std::runtime_error("创建IOCP失败");
                }

            // 将监听socket与IOCP关联
            if (CreateIoCompletionPort((HANDLE)m_listen_fd, m_iocp_handle, (ULONG_PTR)m_listen_fd, 0) == NULL) {
                CloseHandle(m_iocp_handle);
                throw std::runtime_error("关联监听socket到IOCP失败");
                }
            }//IocpSelector()

        ~IocpSelector() {
            CloseHandle(m_iocp_handle);
            }

        void register_timer(int fd) {
            // IOCP不需要显式注册定时器
            // 只需将socket与IOCP关联
            associate_socket_with_iocp(fd);
            }

        void register_on_listening_lt(int fd) const {
            associate_socket_with_iocp(fd);
            // 提交初始读取操作
            post_recv(fd);
            }

        void register_on_reading(int fd, bool one_shot, bool blocking) const {
            associate_socket_with_iocp(fd);
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
            enum class Tag { CONNECTION, READ, WRITE, CLOSE };
            Tag tag;
            int fd;
            };

        Event get_next_event() {
            DWORD bytes_transferred = 0;
            ULONG_PTR completion_key = 0;
            LPOVERLAPPED overlapped = nullptr;

            if (!GetQueuedCompletionStatus(m_iocp_handle, &bytes_transferred,
                                           &completion_key, &overlapped, INFINITE)) {
                DWORD error = GetLastError();
                if (overlapped == nullptr) {
                    throw std::runtime_error(
                        fmt::format("GetQueuedCompletionStatus失败: {}", error));
                    }

                // socket发生错误
                return { Event::Tag::CLOSE, static_cast<int>(completion_key) };
                }

            if (bytes_transferred == 0) {
                // 连接正常关闭
                return { Event::Tag::CLOSE, static_cast<int>(completion_key) };
                }

            if (completion_key == m_listen_fd) {
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
            if (CreateIoCompletionPort((HANDLE)fd, m_iocp_handle, (ULONG_PTR)fd, 0) == NULL) {
                throw std::runtime_error("关联socket到IOCP失败");
                }
            }

        void post_recv(int fd) const {
            auto* io_operation = new IoOperation(IoOperation::Type::READ);
            DWORD flags = 0;

            if (WSARecv(fd, &io_operation->wsa_buf, 1, nullptr, &flags,
                        io_operation, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete io_operation;
                    throw std::runtime_error(
                        fmt::format("WSARecv失败: {}", error));
                    }
                }
            }

        void post_send(int fd) const {
            auto* io_operation = new IoOperation(IoOperation::Type::WRITE);
            DWORD bytes_sent = 0;

            if (WSASend(fd, &io_operation->wsa_buf, 1, &bytes_sent, 0,
                        io_operation, nullptr) == SOCKET_ERROR) {
                int error = WSAGetLastError();
                if (error != WSA_IO_PENDING) {
                    delete io_operation;
                    throw std::runtime_error(
                        fmt::format("WSASend失败: {}", error));
                    }
                }
            }
    };//IocpSelector
#endif