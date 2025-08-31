#ifndef THREAD_POOL_TEST_HPP
#define THREAD_POOL_TEST_HPP
//out/build/WSL-GCC-Release/libs/thread/thread_pool_test
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <vector>
#include "thread_pool.h"
#include "thread_pool_simple.h"
using namespace std;
using namespace m::thread;
using namespace chrono_literals;

class ThreadPoolTest : public ::testing::Test {
    protected:
        ThreadPoolSimple pool{ 2 };
        void SetUp() override {
            //每个测试前重置原子计数器
            task_counter.store(0);
            exception_counter.store(0);
            }
        atomic<int> task_counter{ 0 };
        atomic<int> exception_counter{ 0 };
    };

//空任务
TEST_F(ThreadPoolTest, EmptyTask) {
    promise<void> promise;
    auto future = promise.get_future();
    pool.submit([&promise] {
        promise.set_value();
        });
    future.wait();
}

//基本任务
TEST_F(ThreadPoolTest, BasicTaskExecution) {
    promise<void> promise;//未来值
    auto future = promise.get_future();//未来状态
    pool.submit([&promise, this] {//向线程池提交任务
        task_counter++;
        promise.set_value();//通知未来状态就绪
        });
    future.wait();//阻塞等待完成
    EXPECT_EQ(task_counter.load(), 1);//检查计数器是否为1
    }

//同步任务-传递异常
TEST_F(ThreadPoolTest, ExceptionPropagation) {
    auto future = pool.submitSync([] {
        throw runtime_error("Task failed");
        return 0;
        });
    EXPECT_THROW(future.get(), runtime_error);
}

//多个任务
TEST_F(ThreadPoolTest, MultipleTasksExecution) {
    const int num_tasks = 100;
    vector<promise<void>> promises(num_tasks);
    vector<future<void>> futures;
    for (auto& p : promises) {
        futures.push_back(p.get_future());
        }
    for (int i = 0; i < num_tasks; ++i) {
        pool.submit([&promises, i, this] {
            task_counter++;
            promises[i].set_value();
            });
        }
    // 等待所有任务完成
    for (auto& f : futures) {
        f.wait();
        }
    EXPECT_EQ(task_counter.load(), num_tasks);
    }

//任务返回值（通过共享状态）
TEST_F(ThreadPoolTest, TaskWithReturnValue) {
    promise<int> promise;
    auto future = promise.get_future();

    pool.submit([&promise] {
        promise.set_value(42);
        });

    EXPECT_EQ(future.get(), 42);
    }

//异步任务-线程池内部捕获异常
TEST_F(ThreadPoolTest, ExceptionHandling) {
    promise<void> promise;
    auto future = promise.get_future();
    pool.submit([&promise, this] {
        try {
            throw runtime_error("Test exception");
            }
        catch (...) {//捕获
            exception_counter++;
            promise.set_value();//通知测试线程异常已捕获
            //promise.set_exception(current_exception());//会重新抛出捕获的异常，EXPECT_NO_THROW会失败
            }
        });
    future.wait();//等待完成
    //future.get()会阻塞直到promise.set_value或set_exception并返回值，只能调用一次，可设置超时
    EXPECT_NO_THROW(future.get());//验证未抛异常
    EXPECT_EQ(exception_counter.load(), 1);
    }

/*
// 测试并发安全性
TEST_F(ThreadPoolTest, ConcurrentSubmission) {
    const int num_threads = 10;
    const int tasks_per_thread = 50;
    vector<thread> submit_threads;
    for (int i = 0; i < num_threads; ++i) {
        submit_threads.emplace_back([this, &pool] {
            for (int j = 0; j < tasks_per_thread; ++j) {
                pool.submit([this] {
                    task_counter++;
                    });
                }
            });
        }
    for (auto& t : submit_threads) {
        t.join();
        }
    // 给线程池时间处理所有任务
    this_thread::sleep_for(100ms);
    EXPECT_EQ(task_counter.load(), num_threads * tasks_per_thread);
    }

// 测试析构时等待任务完成
TEST_F(ThreadPoolTest, DestructorWaitsForCompletion) {
    atomic<int> completed_tasks{ 0 };
        {
        ThreadPool pool(2);
        promise<void> slow_task_promise;
        auto slow_task_future = slow_task_promise.get_future();

        // 提交一个慢任务
        pool.submit([&slow_task_promise, &completed_tasks] {
            this_thread::sleep_for(50ms);
            completed_tasks++;
            slow_task_promise.set_value();
            });

        // 提交一些快速任务
        for (int i = 0; i < 10; ++i) {
            pool.submit([&completed_tasks] {
                completed_tasks++;
                });
            }

        // 等待慢任务开始
        slow_task_future.wait();
        } // pool 析构时会等待所有任务完成

    EXPECT_EQ(completed_tasks.load(), 11);
    }

// 测试不同线程数量
TEST_F(ThreadPoolTest, DifferentThreadCounts) {
    for (int thread_count : {
                1, 2, 4, 8
            }) {
        ThreadPool pool(thread_count);
        const int num_tasks = 20;
        vector<promise<void>> promises(num_tasks);
        vector<future<void>> futures;

        for (auto& p : promises) {
            futures.push_back(p.get_future());
            }

        for (int i = 0; i < num_tasks; ++i) {
            pool.submit([&promises, i, this] {
                task_counter++;
                promises[i].set_value();
                });
            }

        for (auto& f : futures) {
            f.wait();
            }

        EXPECT_EQ(task_counter.load(), num_tasks);
        task_counter.store(0); // 重置计数器
        }
    SUCCEED();
    }

// 测试移动语义
TEST_F(ThreadPoolTest, MoveSemantics) {
    promise<unique_ptr<int>> promise;
    auto future = promise.get_future();

    auto unique_val = make_unique<int>(100);
    pool.submit([&promise, val = move(unique_val)]() mutable {
        promise.set_value(move(val));
        });

    auto result = future.get();
    EXPECT_EQ(*result, 100);
    }
*/

#endif // THREAD_POOL_TEST_HPP