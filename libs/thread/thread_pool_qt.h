//qt线程池，有对象创建和事件循环开销
//用USE_STL为1785780 pages/min
//用QThread或QMutexLocker都会性能下降，都使用为1574628
#ifndef THREAD_POOL_QT_DIRECT_H
#define THREAD_POOL_QT_DIRECT_H
#include <QThread>
#include <QMutex>
#include <QWaitCondition>
#include "../common/stl.h"

class ThreadPoolQt {
#ifdef USE_STL
        vector<thread> _threads;
        mutex _mutex;
        condition_variable _condition;
#else
        vector<QThread*> _threads;
        QMutex _mutex;
        QWaitCondition _condition;
#endif
        queue<function<void()>> _queue;
        bool _stop;
    public:
        explicit ThreadPoolQt(size_t thread_count) : _stop(false) {
            for (size_t i = 0; i < thread_count; ++i) {
#ifdef USE_STL
                _threads.emplace_back([this] { threadWork(); });
#else
                QThread* thread = QThread::create([this]() {
                    threadWork();
                    });
                _threads.push_back(thread);
                thread->start();
#endif
                }
            }
        ~ThreadPoolQt() {
            stop();
            }

        void stop(bool discard_queue = false) {
                {
#ifdef USE_STL
                unique_lock<mutex> lock(_mutex);
#else
                QMutexLocker lock(&_mutex);
#endif
                _stop = true;
                if (discard_queue) {
                    while (!_queue.empty()) _queue.pop();
                    }
                }
#ifdef USE_STL
            _condition.notify_one();
#else
            _condition.notify_all();
#endif

#ifdef USE_STL
            for (auto& thread : _threads) {
                if (thread.joinable())
                    thread.join();
#else
            for (auto thread : _threads) {
                thread->wait();
                delete thread;
#endif
                }
            _threads.clear();
            }

        template <typename Func>
        void submit(Func fun) {
                {
#ifdef USE_STL
                unique_lock<mutex> lock(_mutex);
#else
                QMutexLocker lock(&_mutex);
#endif
                _queue.push(forward<Func>(fun));
                }
#ifdef USE_STL
            _condition.notify_one();
#else
            _condition.wakeOne();
#endif
            }

    private:
        void threadWork() {
            while (!_stop) {
                function<void()> task;
                    {
#ifdef USE_STL
                    unique_lock<mutex> lock(_mutex);
                    _condition.wait(lock, [this] { return _stop || !_queue.empty(); });
#else
                    QMutexLocker lock(&_mutex);
                    while (!_stop && _queue.empty()) {
                        _condition.wait(&_mutex);
                        }
#endif
                    if (_stop && _queue.empty()) return;
                    task = move(_queue.front());
                    _queue.pop();
                    }
                task();
                }
            }
    };
#endif // THREAD_POOL_QT_DIRECT_H