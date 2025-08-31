#ifndef THREAD_POOL_SIMPLE_H
#define THREAD_POOL_SIMPLE_H
#include "../common/stl.h"

class ThreadPoolSimple {
        vector<thread> _threads;
        queue<function<void()>> _queue;//任务队列
        mutex _mutex;
        condition_variable _condition;//唤醒线程的条件变量
        atomic<bool> _stop;
    public:
        explicit ThreadPoolSimple(size_t thread_count) : _stop(false) {//构造，依次往线程池加入线程函数：循环{取任务，线程同步，执行}
            for (size_t i = 0; i < thread_count; ++i) {
                _threads.emplace_back([this] {
                    while (true) {
                        function<void()> task;
                            {
                            //rall保证异常中断也能释放锁
                            unique_lock<mutex> lock(_mutex);
                            //用条件变量阻塞线程，释放锁，等待唤醒。被其他线程用notify唤醒时，获取锁，判断：需停止或队列非空，则退出等待，否则释放锁，阻塞等待
                            _condition.wait(lock, [this] { return _stop || !_queue.empty(); });
                            //需停止且队列空，线程退出
                            if (_stop && _queue.empty()) return;
                            //用移动语义拷贝任务
                            task = move(_queue.front());
                            _queue.pop();
                            }
                        task();//执行
                        }//while
                    });//emplace_back lambda
                }//for
            }//ThreadPoolSimple
        ~ThreadPoolSimple() {//析构：停止并等待线程释放
            stop();
            }//~ThreadPoolSimple

        //提交新任务
        //①异步执行(模板版本，避免function的构造和类型擦除开销)
        //支持函数指针、函数对象、lambda、bind表达式
        //必须在头文件实现
        //传入不可调用对象，编译时报错
        template <typename Func>
        void submit(Func fun) {
                {
                lock_guard<mutex> lock(_mutex);
                _queue.push(forward<Func>(fun));//forward完美转发(泛型编程中无损传递参数值、左值/右值引用)
                //不能用move(强制转右值引用，导致传入左值失效)
                }
            _condition.notify_one();
            }
        //②异步执行(function版本，有开销)
        //可能抛出bad_function_call
        void submit(function<void()> fun) {
                {
                lock_guard<mutex> lock(_mutex);
                _queue.push(move(fun));
                }
            _condition.notify_one();//唤醒一个线程
            }
        //③同步执行(模板版本)
        template <typename Func>
        auto submitSync(Func&& func) -> future<decltype(func())> {
            //任务包装为可调用对象指针，绑定未来状态future
            using ReturnType = decltype(func());
            auto task = make_shared<packaged_task<ReturnType()>>(
                            forward<Func>(func)
                        );
            future<ReturnType> future = task->get_future();
                {
                lock_guard<mutex> lock(_mutex);
                _queue.emplace([task]() {
                    (*task)();//存入队列：执行可调用对象
                    });
                }
            _condition.notify_one();
            return future;//调用方可通过future.get()阻塞等待
            }

        void stop(bool discard_queue = false) {//立即退出
                {
                lock_guard<mutex> lock(_mutex);
                _stop = true; //设置停止标志
                if (discard_queue)//清空队列，丢弃未执行任务
                    while (!_queue.empty()) _queue.pop();
                }
            _condition.notify_all();//唤醒所有线程
            for (auto& thread : _threads) {
                if (thread.joinable())
                    thread.join(); //等待线程退出
                }
            _threads.clear();
            }
    };//class ThreadPoolSimple
#endif // THREAD_POOL_SIMPLE_H