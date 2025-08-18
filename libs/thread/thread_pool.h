#ifndef CPP_SIMPLE_WEB_SERVER_THREAD_POOL_HPP
#define CPP_SIMPLE_WEB_SERVER_THREAD_POOL_HPP
#include <atomic>
#include <functional>
#include "thread_join.h"
#include "thread_safe_queue.h"
namespace m::thread {
class ThreadPool {
        std::atomic_bool m_done;//原子变量，是否完成
        ThreadSafeQueue<std::function<void()>> m_work_queue;
        std::vector<std::thread> m_threads;
        size_t m_threads_count;
        JoinThreads m_joiner;

        //线程工作函数
        void worker_thread() {
            while (!m_done) {
                std::function<void()> task;
                m_work_queue.wait_and_pop(task);
//       auto p = m_work_queue.wait_and_pop();
                task();
                }
            }

    public:
        explicit ThreadPool(size_t size) : m_threads_count{size}, m_done{false}, m_joiner{m_threads} {
            try {
                for (unsigned i = 0; i < m_threads_count; ++i)
                    m_threads.emplace_back(&ThreadPool::worker_thread, this);
                //相当于thread m_thread = thread(&ThreadPool::worker_thread, this);
                }
            catch (...) {
                m_done = true;
                throw;
                }
            }//ThreadPool()
        ~ThreadPool() {
            m_done = true;
            }

        //向线程池提交新任务
        template <typename FunctionType>//支持任意可调用对象（如函数指针、lambda 表达式、函数对象等）
        void submit(FunctionType fun) {
            m_work_queue.push(std::function<void()>(std::move(fun)));//移动语义
            }
    };//class ThreadPool
} // namespace m::thread
#endif // CPP_SIMPLE_WEB_SERVER_THREAD_POOL_HPP
