#pragma once

/**
 * 协程（Fiber）类
 * 定义了协程的创建、调度、状态管理等核心功能。
 */

#include <stdio.h>
#include <ucontext.h> // 用于保存/切换上下文的核心API
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>
#include "utils.hpp" // 内部工具，如 CondPanic、GetElapsedMS 等

namespace monsoon
{
    class Fiber : public std::enable_shared_from_this<Fiber> // enable_shared_from_this<Fiber> 允许类内部通过 shared_from_this() 获取自己的 shared_ptr
    {
    public:
        typedef std::shared_ptr<Fiber> ptr;

        // Fiber状态机
        enum State
        {
            // 就绪态，刚创建后或者yield后状态
            READY,
            // 运行态，resume之后的状态
            RUNNING,
            // 结束态，协程的回调函数执行完之后的状态
            TERM,
        };

    private:
        // 初始化当前线程的协程功能，构造线程主协程对象， 私有的只能被GetThis()调用
        // 每个线程初始化时会调用它构造主协程，主协程是不能被 reset() 的，并且没有独立的栈空间，它只是一个上下文容器。
        Fiber();

    public:
        // 构造子协程
        // cb：协程执行的逻辑
        // stackSz：可指定栈大小（默认系统配置）
        // run_in_scheduler：是否参与调度器调度（主协程通常不参与）
        Fiber(std::function<void()> cb, size_t stackSz = 0, bool run_in_scheduler = true);
        ~Fiber();

        // 重置协程状态，复用栈空间
        // 将一个执行完的协程重新绑定新的任务函数cb
        void reset(std::function<void()> cb);
        // 切换协程到运行态
        void resume();
        // 让出协程执行权
        void yield();
        // 获取协程Id
        uint64_t getId() const { return id_; }
        // 获取协程状态
        State getState() const { return state_; }

        // 将当前线程的运行协程设置为 f，用于调度器内部切换协程。
        // resume()内部调用
        static void SetThis(Fiber *f);

        // 获取当前线程中的执行协程
        // 如果当前线程没有创建协程，则创建第一个协程，且该协程为当前线程的
        // 主协程，其他协程通过该协程来调度
        static Fiber::ptr GetThis();
        // 协程总数
        static uint64_t TotalFiberNum();
        // 协程回调函数
        static void MainFunc();
        // 获取当前协程Id
        static uint64_t GetCurFiberID();

    private:
        uint64_t id_ = 0;          // 协程ID
        uint32_t stackSize_ = 0;   // 协程栈大小
        State state_ = READY;      // 协程状态
        ucontext_t ctx_;           // 协程上下文
        void *stack_ptr = nullptr; // 协程栈地址
        std::function<void()> cb_; // 协程回调函数
        bool isRunInScheduler_;    // 本协程是否参与调度器调度
    };
}