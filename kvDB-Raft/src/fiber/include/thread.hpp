#pragma once

/**
 * 定义了一个自封装的 线程类 Thread，用于替代 std::thread 或 pthread，实现更灵活的线程创建与管理，并增加了线程命名和线程 ID 等功能，广泛用于协程调度器 Scheduler 中。
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h> // 定义基本系统数据类型
#include <unistd.h>    // 提供对 POSIX 操作系统 API 的访问功能的头文件的名称
#include <functional>
#include <iostream>
#include <memory>
#include <thread>

namespace monsoon
{
    class Thread
    {
    public:
        typedef std::shared_ptr<Thread> ptr;
        Thread(std::function<void()> cb, const std::string &name); // 创建线程并执行 run(void*) 静态函数
        ~Thread();

        pid_t getId() const { return id_; }
        const std::string &getName() const
        {
            return name_;
        }

        void join();                                  // 等待线程执行完毕，等价于 pthread_join
        static Thread *GetThis();                     // 获取当前线程的 Thread 对象
        static const std::string &GetName();          // 获取当前线程名字
        static void SetName(const std::string &name); // 设置线程名字

    private:
        Thread(const Thread &) = delete;
        Thread(const Thread &&) = delete;
        Thread operator=(const Thread &) = delete;

        static void *run(void *args); // 线程创建时真正执行的函数体
        /**
         * 设置为static的目的，减少创建线程时需要传入的参数个数，static没有this指针了
         */

    private:
        pid_t id_;
        pthread_t thread_;
        std::function<void()> cb_;
        std::string name_;
    };
}