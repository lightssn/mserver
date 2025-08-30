#ifndef THREAD_POOL_TEST_HPP
#define THREAD_POOL_TEST_HPP

#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <future>
#include <vector>
#include "thread_pool.h"

using namespace m::thread;
using namespace std::chrono_literals;

class ThreadPoolTest : public ::testing::Test {
    protected:
        void SetUp() override {
            // 每个测试前重置原子计数器
            task_counter.store(0);
            exception_counter.store(0);
            }

        std::atomic<int> task_counter{ 0 };
        std::atomic<int> exception_counter{ 0 };
    };

// 测试基本任务执行
TEST_F(ThreadPoolTest, BasicTaskExecution) {
    ThreadPool pool(2);
    std::promise<void> promise;
    auto future = promise.get_future();

    pool.submit([&promise, this] {
        task_counter++;
        promise.set_value();
        });

    // 等待任务完成
    future.wait();
    EXPECT_EQ(task_counter.load(), 1);
    }

// 测试多个任务执行
TEST_F(ThreadPoolTest, MultipleTasksExecution) {
    ThreadPool pool(4);
    const int num_tasks = 100;
    std::vector<std::promise<void>> promises(num_tasks);
    std::vector<std::future<void>> futures;

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

// 测试任务返回值（通过共享状态）
TEST_F(ThreadPoolTest, TaskWithReturnValue) {
    ThreadPool pool(2);
    std::promise<int> promise;
    auto future = promise.get_future();

    pool.submit([&promise] {
        promise.set_value(42);
        });

    EXPECT_EQ(future.get(), 42);
    }

// 测试异常处理
TEST_F(ThreadPoolTest, ExceptionHandling) {
    ThreadPool pool(2);
    std::promise<void> promise;
    auto future = promise.get_future();

    pool.submit([&promise, this] {
        try {
            throw std::runtime_error("Test exception");
            }
        catch (...) {
            exception_counter++;
            promise.set_value();
            }
        });

    future.wait();
    EXPECT_EQ(exception_counter.load(), 1);
    }

// 测试并发安全性
TEST_F(ThreadPoolTest, ConcurrentSubmission) {
    ThreadPool pool(4);
    const int num_threads = 10;
    const int tasks_per_thread = 50;
    std::vector<std::thread> submit_threads;

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
    std::this_thread::sleep_for(100ms);

    EXPECT_EQ(task_counter.load(), num_threads * tasks_per_thread);
    }

// 测试析构时等待任务完成
TEST_F(ThreadPoolTest, DestructorWaitsForCompletion) {
    std::atomic<int> completed_tasks{ 0 };

        {
        ThreadPool pool(2);
        std::promise<void> slow_task_promise;
        auto slow_task_future = slow_task_promise.get_future();

        // 提交一个慢任务
        pool.submit([&slow_task_promise, &completed_tasks] {
            std::this_thread::sleep_for(50ms);
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
        std::vector<std::promise<void>> promises(num_tasks);
        std::vector<std::future<void>> futures;

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
    }

// 测试移动语义
TEST_F(ThreadPoolTest, MoveSemantics) {
    ThreadPool pool(2);
    std::promise<std::unique_ptr<int>> promise;
    auto future = promise.get_future();

    auto unique_val = std::make_unique<int>(100);
    pool.submit([&promise, val = std::move(unique_val)]() mutable {
        promise.set_value(std::move(val));
        });

    auto result = future.get();
    EXPECT_EQ(*result, 100);
    }

// 测试空任务
TEST_F(ThreadPoolTest, EmptyTask) {
    ThreadPool pool(2);
    std::promise<void> promise;
    auto future = promise.get_future();

    pool.submit([&promise] {
        // 空任务，什么都不做
        promise.set_value();
        });

    future.wait(); // 应该正常完成而不崩溃
    SUCCEED();
    }

#endif // THREAD_POOL_TEST_HPP