#include <stdexcept>
#include <string>
#include <os/signal.h>

#ifdef _WIN32
#include <windows.h>
// Windows平台信号常量定义
#define SIGINT  0  // CTRL_C_EVENT
#define SIGTERM 1  // CTRL_CLOSE_EVENT
#define SIGBREAK 2 // CTRL_BREAK_EVENT
#define SIGHUP  3  // CTRL_LOGOFF_EVENT
#else
#include <csignal>
#include <cstring>
#endif

namespace m::os {

    /**
     * 注册信号处理函数
     * @param sig 需要处理的信号（如 SIGINT、SIGTERM）
     * @param handler 信号处理函数指针
     * @param restart 信号处理完成后是否自动重启被中断的系统调用（仅Linux有效）
     * @throws std::runtime_error 当信号注册失败时抛出
     */
    void handle_signal(int sig, void (*handler)(int), bool restart) {
#ifdef _WIN32
        /* Windows 信号处理实现说明：
         * 1. 使用 SetConsoleCtrlHandler 替代 sigaction
         * 2. 仅支持以下控制台事件：
         *    - CTRL_C_EVENT (Ctrl+C)
         *    - CTRL_BREAK_EVENT (Ctrl+Break)
         *    - CTRL_CLOSE_EVENT (控制台关闭)
         *    - CTRL_LOGOFF_EVENT (用户注销)
         *    - CTRL_SHUTDOWN_EVENT (系统关机)
         * 3. 不支持 SA_RESTART 功能
         * 4. 处理函数返回 TRUE 表示已处理，FALSE 传递给下一个处理程序
         */
        switch (sig) {
        case SIGINT:  // Ctrl+C
        case SIGTERM: // 终止信号
            if (!SetConsoleCtrlHandler(
                [](DWORD dwCtrlType) -> BOOL {
                    switch (dwCtrlType) {
                    case CTRL_C_EVENT:      // SIGINT 等效
                    case CTRL_BREAK_EVENT:  // SIGBREAK 等效
                    case CTRL_CLOSE_EVENT:  // SIGTERM 等效
                    case CTRL_LOGOFF_EVENT: // SIGHUP 近似
                    case CTRL_SHUTDOWN_EVENT:
                        // 调用注册的处理函数
                        //handler(static_cast<int>(dwCtrlType));//t
                        return TRUE; // 表示已处理
                    default:
                        return FALSE; // 传递给其他处理程序
                    }
                },
                TRUE // 添加处理程序
            )) {
                throw std::runtime_error("Failed to set Windows console control handler");
            }
            break;
        default:
            throw std::runtime_error("Unsupported signal on Windows: " + std::to_string(sig));
        }
#else
        /* Linux/Unix 信号处理实现（保留原有注释）：
         * 1. sa_handler - 设置信号处理函数
         * 2. sa_flags - SA_RESTART 标志使被中断的系统调用自动重启
         * 3. sa_mask - 信号掩码，初始化为包含所有信号以阻塞所有信号
         */
        struct sigaction sa {};
        memset(&sa, '\0', sizeof(sa));      // 内存区域清零
        sa.sa_handler = handler;             // 设置信号处理函数
        if (restart)
            sa.sa_flags |= SA_RESTART;       // 信号处理完成后自动重启被中断的系统调用
        sigfillset(&sa.sa_mask);             // 阻塞所有信号

        if (auto ret = sigaction(sig, &sa, nullptr); ret != 0) {
            throw std::runtime_error(std::string("Bad signal sigaction, sig=") + std::to_string(sig));
        }
#endif
    }

} // namespace m::os