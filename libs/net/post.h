#include <winsock2.h>
#include <mswsock.h>  // for AcceptEx
#include <windows.h>

// 全局 GUID（用于获取 AcceptEx 函数指针）
static GUID guidAccept = WSAID_ACCEPTEX;

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
        &guidAccept,
        sizeof(guidAccept),
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