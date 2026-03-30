//
// Created by user on 2026/1/16.
//

#ifndef LOCKFREEDEFINES_H
#define LOCKFREEDEFINES_H

namespace common {
    /**
     * 不使用原始指针，而是带标签，用于解决ABA的问题
     * @tparam Node 指针类型
     */
    template <typename Node>
    struct alignas(16) TaggedPtr {
        // 为了使用 std::atomic，这个结构体必须是 Trivial 的
        // 在 x64 架构上，这通常占用 16 字节 (128 bit)
        // 需要 CPU 支持双字节宽度的原子操作 (DWCAS, e.g., cmpxchg16b)
        Node* ptr; // 实际的内存指针
        uintptr_t tag; // 版本号/标签

        TaggedPtr() noexcept : ptr(nullptr), tag(0) {}

        TaggedPtr(Node* p, uintptr_t t = 0) noexcept : ptr(p), tag(t) {}

        // 辅助函数，用于比较两个TaggedPtr是否相等
        bool operator==(const TaggedPtr<Node>& rhs) {
            return ptr == rhs.ptr && tag == rhs.tag;
        }

        bool operator!=(const TaggedPtr<Node>& rhs) {
            return !(*this == rhs);
        }
    };
}

#endif //LOCKFREEDEFINES_H
