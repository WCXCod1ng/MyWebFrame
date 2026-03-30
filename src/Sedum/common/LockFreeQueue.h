#ifndef LOCKFREEQUEUE_H
#define LOCKFREEQUEUE_H
#include <atomic>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "LockFreeFreeList.h"
#include "common/LockFreeDefines.h"

namespace common {
    /**
     * 无锁队列的节点
     * @tparam T 数据类型
     */
    template <typename T>
    struct Node {
        // 数据
        T data;

        // 指针，指向下一个节点，这是无锁队列的关键，需要通过atomic配合上述的TaggedPtr来定义
        std::atomic<TaggedPtr<Node<T>>> next;
std::allocator<>
        Node(const T& val) : data(val), next(TaggedPtr<Node<T>>(nullptr)) {}
    };



    /**
     * 基于Michael-Scott算法的无锁队列
     * @tparam T 元素类型
     */
    template <typename T>
    requires std::copyable<T> // 假设类型T是可拷贝的，后续需要优化（例如可移动的）
    class LockFreeQueue {
        // static_assert(std::atomic<TaggedPtr<Node<T>>>::is_always_lock_free,
    // "Error: 128-bit atomics are not lock-free. Missing -mcx16 or alignment?");
    public:
        LockFreeQueue() {
            // 分配哨兵节点
            Node<T>* dummy = new Node<T>(T());

            // 初始化TaggedPtr
            TaggedPtr<Node<T>> sentinel(dummy, 0);

            // 使用Head和Tail同时指向哨兵
            // 注意：构造函数在对象发布前执行，天然是单线程环境，
            // 所以使用 store 的非原子版本或者 relaxed 序即可，但在 std::atomic 接口中
            // store 默认是 seq_cst，为了性能可以用 memory_order_relaxed。
            // 但为了安全起见，这里用 store 默认行为即可，构造只有一次。
            mHead.store(sentinel);
            mTail.store(sentinel);

            // 检查当前平台是否真正支持无锁，如果不支持，会使用atomic实现的锁机制来实现
            if(!mHead.is_lock_free()) {
                // throw std::runtime_error("not support");
                std::cout << "not support" << std::endl;
            }
        }

        ~LockFreeQueue() {
            // 析构时，假设没有其他线程在访问该对象，不需要使用原子操作，直接循环删除
            TaggedPtr<Node<T>> curr = mHead.load();
            while(curr.ptr != nullptr) {
                // 获取当前待删除的节点
                Node<T>* nodeToDelete = curr.ptr;
                // 获取下一个节点
                TaggedPtr<Node<T>> next = nodeToDelete->next.load();
                delete nodeToDelete;
                // 移动到下一个为止
                curr = next;
            }
        }

        // 禁止拷贝构造和赋值（因为内部包含原子成员且独占资源）
        LockFreeQueue(const LockFreeQueue&) = delete;
        LockFreeQueue& operator=(const LockFreeQueue&) = delete;


        void push(const T& value) {
            // 创建新节点
            Node<T>* rawNode = allocNode(value);
            TaggedPtr<Node<T>> newNode(rawNode, 0);

            // 开启CAS循环
            TaggedPtr<Node<T>> tail;
            TaggedPtr<Node<T>> tail_next;

            while(true) {
                // 读取当前tail的快照
                tail = mTail.load();
                // 读取tail的next指针
                tail_next = tail.ptr->next.load();

                // 在读取tail_next的过程中，tail可能已经被别的线程修改了，如果改变了，刚才读的tail_next就没有意义了（就不是结尾了），需要重新尝试
                if(tail != mTail.load()) {
                    continue;
                }

                // 到现在说明tail还没有被修改，接下来需要判断tail_next（之前拿到的next）是否还是空
                if(tail_next.ptr == nullptr) {
                    // case 1：为空，说明tail确实还是队尾
                    // 要尝试将新节点链接到tail的默认，也就是将tail_next原子替换为newNode
                    // expected: 期望还是空（tail_next）
                    // 如果满足，则将其原子替换为newNode，同时返回true
                    // 如果不满足，则将返回false
                    if(tail.ptr->next.compare_exchange_weak(tail_next, newNode, std::memory_order_release, std::memory_order_relaxed)) {
                        // 成功替换，说明已经把newNode挂到tail的next了
                        // 下一步是尝试更新全局的mTail指针，也就是从旧的tail替换为新的newNode
                        // 并且还要注意，为了防止ABA的问题，新的tail不能直接是newNode，我们还需要更新它的tag（为旧的tail的tag+1）
                        TaggedPtr<Node<T>> newTail(rawNode, tail.tag + 1);
                        mTail.compare_exchange_strong(tail, newTail, std::memory_order_release, std::memory_order_relaxed);
                        // 这里没有使用for循环，原因是即使失败了也没关系，会采用协助式策略，由其他线程负责完成最后一步的更新
                        return;
                    }
                } else {
                    // case 2：不为空，说明mTail不是最新的结尾了，说明tail->next有新的元素了
                    // 说明之前有一个线程完成了“链接”的动作
                    // 但是还没来得及更新全局的mTail（否则就会走case1了），因此当前线程必须协助他来更新mTail，否则自己也无法入队
                    // 尝试更新全局mTail，同样也需要更新tag
                    TaggedPtr<Node<T>> newTail(tail_next.ptr, tail.tag + 1);
                    mTail.compare_exchange_strong(tail, newTail, std::memory_order_release, std::memory_order_relaxed); // 同样只需要一个线程完成这个操作即可
                    // 协助完了之后还需要进行下一轮循环，以实现“链接”操作
                }
            }
        }

        bool pop(T& res) {
            TaggedPtr<Node<T>> head;
            TaggedPtr<Node<T>> tail;
            TaggedPtr<Node<T>> next;

            while(true) {
                head = mHead.load();
                tail = mTail.load();

                // 读取head的下一个节点，由于dummy node节点的存在，所以数据实际上存放在head->next上
                if(head.ptr == nullptr) continue; // head为空实际上表示构造函数才刚开始？实际上不可能
                next = head.ptr->next.load();

                // 检查mHead是否已经被其他线程更改了，如果已经被其他线程更改了，那么本次操作作废，需要重来一次
                if(head != mHead.load()) {
                    continue;
                }

                // 检查队列是否为空
                if(head.ptr == tail.ptr) {
                    // case 1：队列看起来是一个空最列，但是可能不是（因为可能有一个链接阶段，但是没有更新全局mtail）

                    if(next.ptr == nullptr) {
                        // next为空，说明队列确实为空，返回false
                        return false;
                    }

                    // 看起来是空的，但是实际上有节点了，原因是tail滞后了
                    // 和push一样，我们需要尝试协助之前链接的线程完成更新tail的操作
                    TaggedPtr<Node<T>> newTail(next.ptr, tail.tag + 1);
                    mTail.compare_exchange_strong(tail, newTail, std::memory_order_release, std::memory_order_relaxed);

                    // 协助完成后进入下一轮循环再尝试pop
                } else {
                    // case 2：队列不为空，此时next肯定指向了一个有效的数据节点

                    // 尝试将mHead移动到next，当然也需要更新tag
                    TaggedPtr<Node<T>> newHead(next.ptr, head.tag + 1);
                    if(mHead.compare_exchange_weak(head, newHead, std::memory_order_release, std::memory_order_relaxed)) {
                        // 移动mHead成功，那么就可以取出数据节点了

                        // 数据节点实际上在head->next，也就是next上（注意不是mHead的next，因为它已经更新过了）
                        res = next.ptr->data;

                        // 关键点，需要归还节点，而不是直接delete
                        freeNode(head.ptr);

                        return true;
                    }
                }
            }
        }

    private:
        /// 封装内存申请逻辑
        Node<T>* allocNode(const T& data) {
            // 尝试直接从池子中获取
            Node<T>* node = mPool.pop();

            if(node == nullptr) {
                // 池子为空，向OS申请
                node = new Node<T>(data);
            } else {
                // 池子不为空，那么就复用节点，这里实际上是假设data可以拷贝
                node->data = data;
                // 注意，还需要重置next指针
                node->next.store(TaggedPtr<Node<T>>(nullptr, 0));
            }

            return node;
        }

        /// 封装释放逻辑
        void freeNode(Node<T>* node) {
            mPool.push(node);
        }


    private:
        /// 队首指针，消费者需要竞争它，使用atomic保护
        alignas(16) std::atomic<TaggedPtr<Node<T>>> mHead;

        // -----------------------------------------------------
        // 关键优化：缓存行填充 (Padding)
        // 作用：强制隔离 head_ 和 tail_ 到不同的 CPU 缓存行 (通常是 64 字节)
        // 防止 "伪共享" (False Sharing) 导致的高速缓存抖动
        // -----------------------------------------------------
        char padding1_[64 - sizeof(std::atomic<TaggedPtr<Node<T>>>)];

        /// 队尾指针：生产者需要竞争它
        alignas(16) std::atomic<TaggedPtr<Node<T>>> mTail;

        // 尾部也填充一下，防止与相邻内存变量发生伪共享
        char padding2_[64 - sizeof(std::atomic<TaggedPtr<Node<T>>>)];

        /// 新增内部对象池
        LockFreeFreeList<Node<T>> mPool;
    };
}

#endif //LOCKFREEQUEUE_H
