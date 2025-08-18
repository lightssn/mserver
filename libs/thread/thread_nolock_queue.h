//单生产者单消费者无锁队列，基于单向链表
#include <atomic>
#include <memory>
#include <iostream>
using std::atomic;
using std::shared_ptr;
using std::make_shared;
using std::move;

template <typename T> class NoLockQueue {
        struct Node {
            shared_ptr<T> data;
            atomic<Node*> next;

            Node(T value) : data(make_shared<T>(move(value))), next(nullptr) {}
            };

        atomic<Node*> head;
        atomic<Node*> tail;

    public:
        NoLockQueue() {
            Node* dummy = new Node(T());  // 哨兵节点
            head.store(dummy);
            tail.store(dummy);
            }

        ~NoLockQueue() {
            while (Node* old_head = head.load()) {
                head.store(old_head->next);
                delete old_head;
                }
            }

        void enqueue(T value) {
            Node* new_node = new Node(move(value));
            Node* old_tail = tail.load();
            Node* null_ptr = nullptr;

            while (true) {
                Node* next = old_tail->next.load();
                if (next == nullptr) {
                    if (old_tail->next.compare_exchange_weak(null_ptr, new_node)) {
                        tail.compare_exchange_weak(old_tail, new_node);
                        return;
                        }
                    }
                else {
                    tail.compare_exchange_weak(old_tail, next);
                    }
                old_tail = tail.load();
                }
            }

        shared_ptr<T> dequeue() {
            Node* old_head = head.load();
            Node* old_tail = tail.load();
            Node* next;

            while (true) {
                next = old_head->next.load();
                if (next == nullptr) {
                    return shared_ptr<T>();  // 队列为空
                    }
                if (old_head == old_tail) {
                    tail.compare_exchange_weak(old_tail, next);
                    }
                else {
                    if (head.compare_exchange_weak(old_head, next)) {
                        shared_ptr<T> res = next->data;
                        delete old_head;
                        return res;
                        }
                    }
                }
            }
    };//NoLockQueue