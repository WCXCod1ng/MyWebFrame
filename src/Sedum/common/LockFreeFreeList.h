//
// Created by user on 2026/1/16.
//

#ifndef LOCKFREEFREELIST_H
#define LOCKFREEFREELIST_H

#include <atomic>
#include <concepts>

#include "common/LockFreeDefines.h"

namespace common {
    template <typename T>
    concept HasSelfNextPtr = requires(T t) {
        // { t.next } -> std::convertible_to<T*>; // 只要t.next可以隐式转化为T*，那么就允许
        t.next; // 只要t.next可以隐式转化为T*，那么就允许
    };

    /**
     * 无锁对象池栈
     * @tparam Node
     */
    template <typename Node>
    requires HasSelfNextPtr<Node> // 注意，这里显式要求它要有next指针，而且next指针指向自己
    class LockFreeFreeList {
    private:
        // 栈顶
        std::atomic<TaggedPtr<Node>> mHead;

    public:
        LockFreeFreeList() : mHead(TaggedPtr<Node>(nullptr, 0)) {}

        ~LockFreeFreeList() {
            // 析构时，说明整个容器都要销毁了
            // 此时不再有并发，直接遍历删除所有缓存的节点，真正归还给 OS
            TaggedPtr<Node> curr = mHead.load();
            while (curr.ptr != nullptr) {
                Node* temp = curr.ptr;
                // 注意：这里假设 Node 里的 next 是 atomic<TaggedPtr> 类型
                // 我们以 relaxed 序读取即可
                TaggedPtr<Node> next = temp->next.load(std::memory_order_relaxed);
                delete temp; // 真正释放内存
                curr = next;
            }
        }

        /**
         * 向FreeList中插入一个节点
         * @param node
         */
        void push(Node* node) {
            // 1. 读取当前的栈顶
            TaggedPtr<Node> oldHead = mHead.load(std::memory_order_relaxed);
            TaggedPtr<Node> newHead(node);

            while (true) {
                // 2. 让新节点的 next 指向当前的栈顶
                // 这一步是本地操作，或者非竞争操作，因为 node 此时是私有的
                // 注意：我们需要构造一个 TaggedPtr 指向 oldHead.ptr
                // 在池子里，节点的 next 的 tag 并不重要，只要 ptr 对就行
                node->next.store(oldHead, std::memory_order_relaxed);

                // 3. 准备新的栈顶
                // 关键：必须增加 tag，防止 FreeList 自身的 ABA
                newHead.tag = oldHead.tag + 1;

                // 4. CAS 尝试更新 head_
                // 比较 head_ 是否等于 oldHead，如果是，则设置为 newHead
                if (mHead.compare_exchange_weak(oldHead, newHead,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {
                    return; // 成功归还
                }
                // 失败：说明有别的线程先 push 或 pop 了，oldHead 已经被 CAS 自动更新为最新值
                // 循环重试...
            }
        }

        /**
         * 从FreeList中取出一个节点
         * @return 栈顶元素指针
         */
        Node* pop() {
            // 1. 读取当前栈顶
            TaggedPtr<Node> oldHead = mHead.load(std::memory_order_acquire);

            while (true) {
                // 2. 判空
                if (oldHead.ptr == nullptr) {
                    return nullptr;
                }

                // 3. 读取下一个节点 (用于成为新的 head)
                // 这是一个潜在的风险点，但在 FreeList 场景下是安全的。
                // 只要我们不真正 delete 节点（只在析构时 delete），
                // oldHead.ptr 指向的内存就是可读的。
                TaggedPtr<Node> nextNode = oldHead.ptr->next.load(std::memory_order_relaxed);

                // 4. 准备新 head
                // 指向 nextNode，且 tag 增加
                TaggedPtr<Node> newHead(nextNode.ptr, oldHead.tag + 1);

                // 5. CAS 尝试更新 head_
                if (mHead.compare_exchange_weak(oldHead, newHead,
                                                std::memory_order_acquire,
                                                std::memory_order_acquire)) {
                    return oldHead.ptr; // 成功获取
                }
                // 失败：oldHead 已被更新，循环重试
            }
        }
    };
}

#endif //LOCKFREEFREELIST_H
