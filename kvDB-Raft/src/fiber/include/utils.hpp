#pragma once

/**
 * monsoon 命名空间下的一个工具库头文件，主要提供了与线程、时间、断言、调用栈（Backtrace）等相关的 工具函数，常用于调试、诊断和异常处理
 */

#include <assert.h>
#include <cxxabi.h>
#include <execinfo.h>
#include <pthread.h>
#include <stdint.h> // 定义了一组精确宽度的整数类型
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <sstream>
#include <vector>

namespace monsoon
{
    pid_t GetThreadId();
    u_int32_t GetFiberId();

    // 获取当前启动的毫秒数
    // 系统从启动到当前时刻的毫秒数
    static uint64_t GetElapsedMS()
    {
        struct timespec ts = {0};
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    // 将 C++ 编译后的“乱码函数名”解码成人类可读的形式 _ZN7monsoon9Scheduler10CreateTaskEv   →   monsoon::Scheduler::CreateTask()
    static std::string demangle(const char *str)
    {
        size_t size = 0;
        int status = 0;
        std::string rt;
        rt.resize(256);
        if (1 == sscanf(str, "%*[^(]%*[^_]%255[^)+]", &rt[0])) // 从 str 中提取 mangled 名字
        {
            // 解析函数
            char *v = abi::__cxa_demangle(&rt[0], nullptr, &size, &status);
            if (v)
            {
                std::string result(v);
                free(v);
                return result;
            }
        }
        // 解析失败，返回原始函数名
        if (1 == sscanf(str, "%255s", &rt[0]))
        {
            return rt;
        }
        return str;
    }

    // 获取当前线程的调用栈信息
    static void Backtrace(std::vector<std::string> &bt, int size, int skip)
    {
        // 分配用于存储调用栈信息的数组
        void **array = (void **)malloc((sizeof(void *) * size));
        size_t s = ::backtrace(array, size); // backtrace() 捕获当前线程的调用栈地址（函数指针数组）

        // backtrace_symbols() 把函数地址转为字符串
        char **strings = backtrace_symbols(array, s);
        if (strings == NULL)
        {
            std::cout << "backtrace_synbols error" << std::endl;
            return;
        }
        // 解析每一个调用栈的信息，并将解析后的函数名添加到bt中
        for (size_t i = skip; i < s; ++i)
        {
            bt.push_back(demangle(strings[i]));
        }

        free(strings);
        free(array);
    }

    // 将调用栈以字符串形式返回（通常用于日志打印）。
    static std::string BacktraceToString(int size, int skip, const std::string &prefix)
    {
        std::vector<std::string> bt;
        Backtrace(bt, size, skip); // 调用 Backtrace() 获取函数名列表
        std::stringstream ss;
        for (size_t i = 0; i < bt.size(); ++i)
        {
            ss << prefix << bt[i] << std::endl; // 拼接成字符串
        }
        return ss.str();
    }

    // 断言处理
    // 打印断言信息 + 当前文件 + 行号 + 当前调用栈
    static void CondPanic(bool condition, std::string err)
    {
        if (!condition)
        {
            std::cout << "[assert by] (" << __FILE__ << ":" << __LINE__ << "),err: " << err << std::endl;
            std::cout << "[backtrace]\n"
                      << BacktraceToString(6, 3, "") << std::endl;
            assert(condition);
        }
    }
}