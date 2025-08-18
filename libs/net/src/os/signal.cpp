//
// Created by wwww on 2023/8/23.
//

#include <csignal>
#include <cstring>
#include <os/signal.h>
#include <stdexcept>
using namespace std;

namespace m::os {
//注册信号处理函数
void handle_signal(int sig, void (*handler)(int), bool restart) {
    struct sigaction sa {};
    memset(&sa, '\0', sizeof(sa));//内存区域清零
    sa.sa_handler = handler;//设置信号处理函数
    if (restart)
        sa.sa_flags |= SA_RESTART;//信号处理完成后自动重启被中断的系统调用
    sigfillset(&sa.sa_mask);//信号掩码，sigfillset初始化为包含所有信号，以阻塞所有信号
    if (auto ret = sigaction(sig,//需要处理的信号（如 SIGINT、SIGTERM）
                             &sa, nullptr//旧的信号处理行为
                            ); ret != 0)
        throw runtime_error{string{"Bad signal sigaction, sig="} + to_string(sig)};
    }
} // namespace m::os