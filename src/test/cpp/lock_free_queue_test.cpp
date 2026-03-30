#include "common/LockFreeQueue.h"
#include <queue>
#include <gtest/gtest.h>



// ============================================================================
// 辅助基准：传统的 Mutex Queue
// ============================================================================
template <typename T>
class MutexQueue {
    std::queue<T> q;
    std::mutex m;
public:
    void push(const T& val) {
        std::lock_guard<std::mutex> lock(m);
        q.push(val);
    }
    bool pop(T& val) {
        std::lock_guard<std::mutex> lock(m);
        if (q.empty()) return false;
        val = q.front();
        q.pop();
        return true;
    }
};


// 1. 基础功能测试
TEST(LockFreeQueueTest, BasicOperations) {
    common::LockFreeQueue<int> q;
    int val;

    // 初始应为空
    ASSERT_FALSE(q.pop(val));

    // 单线程 Push/Pop
    q.push(10);
    q.push(20);
    q.push(30);

    ASSERT_TRUE(q.pop(val));
    EXPECT_EQ(val, 10);
    ASSERT_TRUE(q.pop(val));
    EXPECT_EQ(val, 20);
    ASSERT_TRUE(q.pop(val));
    EXPECT_EQ(val, 30);

    // 再次变空
    ASSERT_FALSE(q.pop(val));
}

// 2. 复杂对象拷贝测试
TEST(LockFreeQueueTest, ComplexObjectCopy) {
    struct ComplexType {
        int id;
        std::string name;
        std::vector<double> data;

        // 验证可拷贝性
        ComplexType(int i, std::string n) : id(i), name(n), data(10, 1.0) {}
        ComplexType() : id(0) {}

        bool operator==(const ComplexType& other) const {
            return id == other.id && name == other.name && data.size() == other.data.size();
        }
    };

    common::LockFreeQueue<ComplexType> q;
    ComplexType in1(1, "Alice");
    ComplexType in2(2, "Bob");

    q.push(in1);
    q.push(in2);

    ComplexType out;
    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, in1); // 检查拷贝的数据是否正确

    ASSERT_TRUE(q.pop(out));
    EXPECT_EQ(out, in2);
}

// 3. 并发压力测试 (MPMC - 多生产者多消费者)
TEST(LockFreeQueueTest, ConcurrentCorrectness_MPMC) {
    common::LockFreeQueue<int> q;
    const int num_producers = 4;
    const int num_consumers = 4;
    const int items_per_thread = 50000;
    const int expected_total = num_producers * items_per_thread;

    std::atomic<int> start_flag(0);
    std::atomic<long long> total_consumed(0);
    std::atomic<int> producers_done(0);

    std::vector<std::thread> threads;

    // 生产者
    for (int i = 0; i < num_producers; ++i) {
        threads.emplace_back([&, i]() {
            while (!start_flag) std::this_thread::yield(); // 自旋等待开始
            for (int j = 0; j < items_per_thread; ++j) {
                // 存入带有唯一标识的数据，方便校验 (虽然这里我们只校验总和)
                q.push(1);
            }
            producers_done++;
        });
    }

    // 消费者
    for (int i = 0; i < num_consumers; ++i) {
        threads.emplace_back([&]() {
            while (!start_flag) std::this_thread::yield();
            int val;
            long long local_count = 0;
            while (true) {
                if (q.pop(val)) {
                    local_count += val;
                } else {
                    // 如果队列空了，且生产者都结束了，说明消费完了
                    if (producers_done == num_producers) {
                        // 双重检查：确保真的很空 (防止生产者还在最后一步Swing Tail)
                        if (!q.pop(val)) break;
                        local_count += val; // 还是捞到了一个
                    } else {
                        std::this_thread::yield(); // 生产者还没完，稍微等等
                    }
                }
            }
            total_consumed += local_count;
        });
    }

    // 开始所有线程
    start_flag = 1;

    for (auto& t : threads) {
        if (t.joinable()) t.join();
    }

    EXPECT_EQ(total_consumed.load(), expected_total) << "消费的元素总数与生产总数不匹配，存在数据丢失或重复！";
}

// 4. 对象池复用验证 (ABA 压力测试)
// 这个测试使用非常小的范围反复 Push/Pop，迫使 Freelist 频繁复用节点。
// 如果 ABA 处理不当，这里极易崩溃或死循环。
TEST(LockFreeQueueTest, MemoryPoolResuse_ABA_Stress) {
    common::LockFreeQueue<int> q;
    const int iterations = 100000;

    std::thread producer([&]() {
        for (int i = 0; i < iterations; ++i) q.push(i);
    });

    std::thread consumer([&]() {
        int val;
        for (int i = 0; i < iterations; ++i) {
            while (!q.pop(val)) std::this_thread::yield();
            ASSERT_EQ(val, i); // 确保顺序也是对的
        }
    });

    producer.join();
    consumer.join();
}

// ============================================================================
// [Part 4] 性能对比测试
// ============================================================================

class PerformanceTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}

    template<typename Q>
    long long MeasureThroughput(const char* name, int num_threads, int count_per_thread) {
        Q queue;
        std::vector<std::thread> threads;
        std::atomic<bool> start(false);
        std::atomic<int> producers_done(0);

        auto start_time = std::chrono::high_resolution_clock::now();

        // 混合读写 (50% Push, 50% Pop 模拟)
        for (int i = 0; i < num_threads; ++i) {
            threads.emplace_back([&, i]() {
                while (!start) std::this_thread::yield();
                int val;
                for (int j = 0; j < count_per_thread; ++j) {
                    queue.push(j);
                    queue.pop(val); // 立即 Pop，模拟高竞争
                }
            });
        }

        start = true; // 发令枪
        for (auto& t : threads) t.join();

        auto end_time = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

        long long total_ops = num_threads * count_per_thread * 2LL; // Push + Pop
        std::cout << "[ Perf ] " << name << " (" << num_threads << " threads): "
                  << total_ops << " ops in " << duration << "ms. "
                  << "Throughput: " << (total_ops * 1000 / (duration + 1)) << " ops/sec" << std::endl;

        return duration;
    }
};

TEST_F(PerformanceTest, CompareThroughput) {
    // 根据机器核心数调整，但至少要有竞争
    int thread_counts[] = {1, 2, 4, 8, 16};
    int ops_per_thread = 100000;

    std::cout << "\n=== Performance Benchmark: LockFree vs Mutex ===\n" << std::endl;

    for (int t : thread_counts) {
        long long t_lockfree = MeasureThroughput<common::LockFreeQueue<int>>("LockFreeQueue", t, ops_per_thread);
        long long t_mutex    = MeasureThroughput<MutexQueue<int>>   ("MutexQueue   ", t, ops_per_thread);

        if (t > 1) {
            // 在单线程下，无锁队列并不一定比无锁快（因为原子指令开销）
            // 但在多线程下，期望无锁队列能保持较好的延展性
            // 注意：这取决于具体的 CPU 调度和负载类型
            EXPECT_LE(t_lockfree, t_mutex); // 仅作为参考，不强制 assert，防止CI环境波动失败
        }
    }
    std::cout << "\n================================================\n" << std::endl;
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}