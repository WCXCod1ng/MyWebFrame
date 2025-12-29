//
// Created by user on 2025/12/14.
//

#ifndef OBJECTPOOL_H
#define OBJECTPOOL_H
#include <functional>
#include <memory>
#include <base/NonCopyable.h>

namespace fleabane {
    class NonCopyable;
}

namespace common {
    /**
     * @brief 通用对象池，注意它不保证线程安全
     * @tparam T 对象类型
     */
    template <typename T>
    class ObjectPool : fleabane::NonCopyable {
    public:
        // 定义智能指针类型，使用自定义删除器
        // 当这个 shared_ptr 销毁时，它不会 delete T，而是调用 lambda 将 T 还回池子
        using ObjectPtr = std::shared_ptr<T>;

        // defaultSize: 初始预分配数量（可选）
        // maxSize: 池子最大保留数量
        ObjectPool(size_t maxSize = 1000, size_t defaultSize = 10) : maxSize_(maxSize) {
            // 初始时刻创建 defaultSize 个“raw memory”
            for(size_t i = 0; i < defaultSize; ++i) {
                // ::operator new 只负责分配内存，类似于 malloc
                // 它不会调用 T 的构造函数
                void* raw_mem = ::operator new(sizeof(T));

                // 强转为 T* 存入池子，但此时它指向的还不是合法的 T 对象
                pool_.push_back(static_cast<T*>(raw_mem));
            }
        }

        ~ObjectPool() {
            // 销毁池中所有现存的对象
            for (T* ptr : pool_) {
                ::operator delete(ptr);
            }
            pool_.clear();
        }

        /// 核心接口：借出一个对象
        ObjectPtr acquire() {

            T* ptr = nullptr;
            if (!pool_.empty()) {
                // 1. 如果池子里有，直接拿出来（复用）
                ptr = pool_.back();
                pool_.pop_back();
            } else {
                // // 2. 如果池子空了，new 一个新的
                // ptr = new T();
                // (不调用构造函数)
                // 相当于 malloc(sizeof(T))
                ptr = static_cast<T *>(::operator new(sizeof(T)));
            }

            // 3. 【关键】原位构造 (Placement New)
            // 在 ptr 指向的内存上调用 T 的构造函数。
            // 这会重置对象的所有状态，包括 enable_shared_from_this 的 weak_ptr！
            new (ptr) T();

            // 4. 包装成智能指针返回
            // 这里的 deleter 捕获了 this 指针，负责归还逻辑
            // 当 shared_ptr 引用计数为 0 时，不会 delete ptr，而是调用 release(ptr)
            return ObjectPtr(ptr, [this](T* p) {
                this->release(p);
            });
        }

        // 设置新的上限
        void setMaxCapacity(size_t maxCap) {
            maxSize_ = maxCap;
            // 如果当前已经超了，可以立刻缩容
            while (pool_.size() > maxSize_) {
                delete pool_.back();
                pool_.pop_back();
            }
        }

        /// 获取当前池中空闲对象的数量（调试用）
        size_t size() {
            return pool_.size();
        }

    private:
        /// 内部接口：归还一个对象
        void release(T* ptr) {
            // 增加判断逻辑，防止对象池对象数太多
            if(pool_.size() >= maxSize_) {
                delete ptr;
            } else {
                // 【关键】手动调用析构函数
                // 既然我们要重置状态，归还时就应该析构，释放它持有的资源（如 conn_, vector 等）
                // 这代替了你之前的 Context::reset()
                ptr->~T();
                pool_.push_back(ptr);
            }
        }

        // 存储空闲对象的栈（使用 vector 模拟栈，缓存友好）
        std::vector<T*> pool_;
        // 对象池中的对象的最大上限
        size_t maxSize_;
    };
}


#endif //OBJECTPOOL_H
