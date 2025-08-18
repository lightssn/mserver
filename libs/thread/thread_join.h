#ifndef CPP_SIMPLE_WEB_SERVER_JTHREADS_HPP
#define CPP_SIMPLE_WEB_SERVER_JTHREADS_HPP
#include <thread>
#include <vector>
namespace m::thread {
//线程同步类，传入线程集，析构时全部join，防主函数提前返回未join导致线程资源泄露
class JoinThreads {
        std::vector<std::thread> &m_threads;
    public:
        explicit JoinThreads(std::vector<std::thread> &threads)
            : m_threads(threads) {}
        ~JoinThreads() {//确保所有线程在JoinThreads对象销毁时都已完成
            for (auto &t : m_threads)
                if (t.joinable())
                    t.join();
            }
    };
} // namespace m::thread
#endif // CPP_SIMPLE_WEB_SERVER_JTHREADS_HPP
