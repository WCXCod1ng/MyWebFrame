
# 为run_tests可执行文件添加CMake配置
add_executable(run_tests
        cpp/lock_test.cpp
        cpp/mysql_connection_pool_test.cpp
        cpp/thread_pool_test.cpp
        cpp/block_queue_test.cpp
        cpp/logger_test.cpp
        cpp/simple_server_test.cpp
        cpp/coroutine_example_test.cpp
        cpp/lock_free_queue_test.cpp
)

# 1. 检查编译器类型，如果是 GCC 或 Clang，添加 -mcx16 标志
#    这告诉编译器可以使用 cmpxchg16b 指令，从而让 16 字节原子操作变成 lock-free
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(run_tests PRIVATE -mcx16)
endif()

# 为http_server_test可执行文件添加CMake配置
add_executable(http_server_test
        cpp/http_server_test.cpp
)
target_link_libraries(http_server_test
        PRIVATE
        Fleabane
        Sedum
)

# 为web_frame_test可执行文件添加CMake配置
add_executable(web_frame_test
        cpp/web_frame_test.cpp
)
target_link_libraries(web_frame_test
        PRIVATE
        Fleabane
        Sedum
        jwt-cpp::jwt-cpp
)

# 3. 链接 GoogleTest 库
#    GTest::gtest_main 包含了 GTest 库本身以及一个预编译好的 main 函数
target_link_libraries(run_tests
        PRIVATE
        Fleabane
        Sedum
        GTest::gtest_main
        mysqlclient
        atomic
)

# 4. 将可执行文件注册为 CTest 测试用例 (这一步保持不变)
add_test(NAME ServerLogicTest COMMAND run_tests)