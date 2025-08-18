#ifndef CPP_SIMPLE_WEB_SERVER_THREAD_SAFE_QUEUE_HPP
#define CPP_SIMPLE_WEB_SERVER_THREAD_SAFE_QUEUE_HPP
#include <condition_variable>
#include <mutex>
#include <queue>
namespace m::thread {
using std::mutex;
using std::unique_lock;
using std::unique_ptr;
using std::lock_guard;
//线程安全的队列实现
template <typename T> class ThreadSafeQueue {
        struct Node {
            unique_ptr<T> data;
            unique_ptr<Node> next;
            };
        mutex head_mutex, tail_mutex;
        unique_ptr<Node> head;
        Node *tail;
        std::condition_variable data_cond;

        Node *get_tail() {
            lock_guard<mutex> tail_lock(tail_mutex);
            return tail;
            }
        unique_ptr<Node> pop_head() {
            unique_ptr<Node> old_head = move(head);
            head = move(old_head->next);
            return old_head;
            }
        unique_lock<mutex> wait_for_data() {
            unique_lock<mutex> head_lock(head_mutex);
            data_cond.wait(head_lock, [&] { return head.get() != get_tail(); });
            return move(head_lock);
            }
        unique_ptr<Node> wait_pop_head() {
            unique_lock<mutex> head_lock(wait_for_data());
            return pop_head();
            }
        unique_ptr<Node> wait_pop_head(T &value) {
            unique_lock<mutex> head_lock(wait_for_data());
            value = move(*head->data);
            return pop_head();
            }
        unique_ptr<Node> try_pop_head() {
            lock_guard<mutex> head_lock(head_mutex);
            if (head.get() == get_tail()) {
                return unique_ptr<Node>();
                }
            return pop_head();
            }
        unique_ptr<Node> try_pop_head(T &value) {
            lock_guard<mutex> head_lock(head_mutex);
            if (head.get() == get_tail()) {
                return unique_ptr<Node>();
                }
            value = move(*head->data);
            return pop_head();
            }

    public:
        ThreadSafeQueue() : head(new Node), tail(head.get()) {}
        ThreadSafeQueue(const ThreadSafeQueue &other) = delete;//禁止拷贝构造
        ThreadSafeQueue &operator=(const ThreadSafeQueue &other) = delete;//禁止赋值操作
        unique_ptr<T> try_pop() {
            unique_ptr<Node> old_head = try_pop_head();
            return old_head ? old_head->data : unique_ptr<T>();
            }
        bool try_pop(T &value) {
            unique_ptr<Node> const old_head = try_pop_head(value);
            return old_head != nullptr;
            }
        unique_ptr<T> wait_and_pop() {
            unique_ptr<Node>  old_head = wait_pop_head();
            return move(old_head->data);
            };
        void wait_and_pop(T &value) {
            unique_ptr<Node> const old_head = wait_pop_head(value);
            }
        void push(T new_value) {
            auto new_data = std::make_unique<T>(move(new_value));
            unique_ptr<Node> p(new Node);
                {
                lock_guard<mutex> tail_lock(tail_mutex);
                tail->data = std::move(new_data);
                Node *const new_tail = p.get();
                tail->next = std::move(p);
                tail = new_tail;
                }
            data_cond.notify_one();
            }
        bool empty() {
            lock_guard<mutex> head_lock(head_mutex);
            return (head.get() == get_tail());
            }
    };//class ThreadSafeQueue
} // namespace m::thread
#endif // CPP_SIMPLE_WEB_SERVER_THREAD_SAFE_QUEUE_HPP
