#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "../common/stl.h"
#include "thread_join.h"
#include "thread_safe_queue.h"
namespace m::thread {
using std::atomic_bool;
using std::vector;
using std::queue;
using std::thread;
using std::function;
using std::move;

class ThreadPool {
        atomic_bool m_done;//原子变量，是否完成
        ThreadSafeQueue<function<void()>> _queue;
        vector<thread> m_threads;
        size_t m_threads_count;
        JoinThreads m_joiner;

        //线程工作函数
        void worker_thread() {
            while (!m_done) {
                function<void()> task;
                _queue.wait_and_pop(task);
//       auto p = _queue.wait_and_pop();
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
            _queue.notify_all();
            }

        //向线程池提交新任务
        template <typename T>//支持任意可调用对象（如函数指针、lambda 表达式、函数对象等）
        void submit(T fun) {
            _queue.push(function<void()>(move(fun)));//移动语义
            }
    };//class ThreadPool
} // namespace m::thread
#endif // THREAD_POOL_H
