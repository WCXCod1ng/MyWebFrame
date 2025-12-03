//
// Created by user on 2025/12/2.
//

#ifndef CURRENTTHREAD_H
#define CURRENTTHREAD_H
#include <pthread.h>
#include <string>

/// 定义专用于线程相关操作的工具函数
namespace fleabane::current_thread {
    // =======================================================
    // 1. 定义 thread_local 变量
    // =======================================================
    // inline (C++17): 允许在头文件中定义变量，避免多重定义错误
    // 默认名称为 "Unknown"，防止忘记设置时日志为空
    // 使用线程局部变量的目的在于：每个线程获取到的实际上都是独属于它的一份拷贝，多个线程之间互不干涉；
    // 另外线程局部变量的声明周期从其初始化开始直到该线程结束都有效，用于替代全局变量和静态变量
    inline thread_local std::string t_thread_name = "Unknown";

    // =======================================================
    // 2. 获取线程名 (供日志库使用)
    // =======================================================
    inline const std::string& name() {
        return t_thread_name;
    }

    // =======================================================
    // 3. 设置线程名 (同时设置 TLS 和 系统级名称)
    // =======================================================
    inline void set_name(const std::string& name) {
        // 1. 设置应用层名字 (TLS)，不受长度限制，日志里用这个
        t_thread_name = name;

        // 2. 设置操作系统级名字 (System Level)，方便 gdb/htop 调试
        //    注意：通常有长度限制 (Linux: 16 chars including null)
#ifdef __linux__
        pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
#elif defined(__APPLE__)
        pthread_setname_np(name.substr(0, 15).c_str());
#elif defined(_WIN32)
        // Windows 10 version 1607+
        // 需要宽字符转换，这里简化示意
        std::wstring wname(name.begin(), name.end());
        SetThreadDescription(GetCurrentThread(), wname.c_str());
#endif
    }
}


#endif //CURRENTTHREAD_H
