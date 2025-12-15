//
// Created by user on 2025/12/14.
//

#ifndef CONCURRENTOBJECTPOOL_H
#define CONCURRENTOBJECTPOOL_H
#include <functional>
#include <memory>
#include <base/NonCopyable.h>

namespace fleabane {
    class NonCopyable;
}

namespace common {
    /**
     * @brief 通用线程安全对象池
     * @tparam T 对象类型
     */
    template <typename T>
    class ConcurrentObjectPool : fleabane::NonCopyable {
    public:
        // 定义智能指针类型，使用自定义删除器
        // 当这个 shared_ptr 销毁时，它不会 delete T，而是调用 lambda 将 T 还回池子
        using ObjectPtr = std::shared_ptr<T>;

        // defaultSize: 初始预分配数量（可选）
        // maxSize: 池子最大保留数量
        ConcurrentObjectPool(size_t maxSize = 1000, size_t defaultSize = 10) : maxSize_(maxSize) {
            // 初始时刻创建defaultSize个对象
            for(size_t i = 0; i < defaultSize; ++i) {
                pool_.push_back(new T());
            }
        }

        ~ConcurrentObjectPool() {
            std::lock_guard<std::mutex> lock(mutex_);
            // 销毁池中所有现存的对象
            for (T* ptr : pool_) {
                delete ptr;
            }
            pool_.clear();
        }

        /// 核心接口：借出一个对象
        ObjectPtr acquire() {
            std::lock_guard<std::mutex> lock(mutex_);

            T* ptr = nullptr;
            if (!pool_.empty()) {
                // 1. 如果池子里有，直接拿出来（复用）
                ptr = pool_.back();
                pool_.pop_back();
            } else {
                // 2. 如果池子空了，new 一个新的
                ptr = new T();
            }

            // 3. 包装成智能指针返回
            // 这里的 deleter 捕获了 this 指针，负责归还逻辑
            // 当 shared_ptr 引用计数为 0 时，不会 delete ptr，而是调用 release(ptr)
            return ObjectPtr(ptr, [this](T* p) {
                this->release(p);
            });
        }

        // 设置新的上限
        void setMaxCapacity(size_t maxCap) {
            std::lock_guard<std::mutex> lock(mutex_);
            maxSize_ = maxCap;
            // 如果当前已经超了，可以立刻缩容
            while (pool_.size() > maxSize_) {
                delete pool_.back();
                pool_.pop_back();
            }
        }

        /// 获取当前池中空闲对象的数量（调试用）
        size_t size() {
            std::lock_guard<std::mutex> lock(mutex_);
            return pool_.size();
        }

    private:
        /// 内部接口：归还一个对象
        void release(T* ptr) {
            std::lock_guard<std::mutex> lock(mutex_);
            // 增加判断逻辑，防止对象池太多
            if(pool_.size() >= maxSize_) {
                delete ptr;
            } else {
                pool_.push_back(ptr);
            }
        }

        // 存储空闲对象的栈（使用 vector 模拟栈，缓存友好）
        std::vector<T*> pool_;
        // 保护 pool_ 的线程安全
        std::mutex mutex_;
        // 对象池中的对象的最大上限
        size_t maxSize_;
    };
}


#endif //CONCURRENTOBJECTPOOL_H
