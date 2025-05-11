#pragma once

/**
 * 定义了一个协程调度器（Scheduler）的核心逻辑，是一个 支持 N-M 模型的协程调度框架 —— 即 N 个线程调度 M 个协程。
 * 主要职责：
 *  管理多个线程的协程调度。
    支持将协程/函数调度到指定线程。
    使用 任务队列 和 线程池 实现协程的并发执行。
    提供 idle 协程，在没有任务时保持线程挂起。
 */

#include <atomic>
#include <boost/type_index.hpp> // 获取某种类型的具体类型名称，不管是静态类型名称，还是带有cvr（const-volatile-reference）修饰的名称，还可以获取某个对象运行时的名称。
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include "fiber.hpp"
#include "mutex.hpp"
#include "thread.hpp"
#include "utils.hpp"

namespace monsoon
{
    // 对任务的封装，（任务可以是一个协程或者一个函数）
    class SchedulerTask
    {
    public:
        friend class Scheduler;

        SchedulerTask()
        {
            thread_ = -1;
        }
        SchedulerTask(Fiber::ptr f, int t) : fiber_(f), thread_(t) {}
        SchedulerTask(Fiber::ptr *f, int t) // 传入一个协程的指针的地址
        {
            fiber_.swap(*f);
            thread_ = t;
        }
        SchedulerTask(std::function<void()> f, int t)
        {
            cb_ = f;
            thread_ = t;
        }

        // 清空任务
        void reset()
        {
            fiber_ = nullptr;
            cb_ = nullptr;
            thread_ = -1;
        }

    private:
        Fiber::ptr fiber_;
        std::function<void()> cb_;
        int thread_; // 期望任务在thread_线程中运行
    };

    // N->M协程调度器
    class Scheduler
    {
    public:
        typedef std::shared_ptr<Scheduler> ptr;

        Scheduler(size_t threads = 1, bool use_caller = true, const std::string &name = "Scheduler");
        virtual ~Scheduler();
        const std::string &getName() const
        {
            return name_;
        }
        // 每个线程都有自己的调度器，获取当前线程的调度器
        static Scheduler *GetThis();
        // 获取当前线程中运行调度器的主协程
        static Fiber *GetMainFiber();
 
        /**
         *  添加调度任务
         * TaskType 任务类型，可以是协程对象或函数指针
         *  task 任务
         * thread 指定执行函数的线程，-1为不指定
         */
        template <typename TaskType>
        void scheduler(TaskType task, int thread = -1)
        {
            bool isNeedTickle = false;
            {
                Mutex::Lock lock(mutex_);
                isNeedTickle = schedulerNoLock(task, thread);
            }
            if (isNeedTickle)
            {
                tickle(); // 唤醒idle协程
            }
        }

        // 启动调度器
        void start();
        // 停止调度器，等待所有任务结束
        void stop();

    protected:
        // 通知调度器任务到达
        virtual void tickle();

        /**
         *  协程调度函数,
         * 默认会启用hook
         */
        void run();
        // 无任务时执行idle协程
        virtual void idle();
        // 返回是否可以停止
        virtual bool stopping();

        // 设置当前线程调度器
        void setThis();

        bool isHasIdleThreads()
        {
            return idleThreadCnt_ > 0;
        }

    private:
        // 无锁下 添加调度任务
        template <typename TaskType>
        bool schedulerNoLock(TaskType t, int thread)
        {
            // 如果任务队列为空，那么在添加任务之后，要调用一次tickle方法以通知各调度线程的调度协程有新任务来了
            bool isNeedTickle = tasks_.empty(); // 如果是空的，说明当前没有任务在排队，需要通过 tickle() 通知工作线程有新任务进来了
            SchedulerTask task(t, thread);
            if (task.fiber_ || task.cb_)
            {
                tasks_.push_back(task);
            }
            return isNeedTickle;
        }

        // 调度器名称
        std::string name_;
        // 互斥锁
        Mutex mutex_;
        // 线程池
        std::vector<Thread::ptr> threadPool_;
        // 任务队列
        std::list<SchedulerTask> tasks_;
        // 线程池id数组
        std::vector<int> threadIds_;
        // 工作线程数量（不包含use_caller的主线程）
        size_t threadCnt_ = 0;
        // 活跃线程数
        std::atomic<size_t> activeThreadCnt_ = {0};
        // IDLE线程数
        std::atomic<size_t> idleThreadCnt_ = {0};
        // 是否是use caller // 是否使用主线程执行任务
        bool isUseCaller_;
        // use_caller 模式下的主协程
        Fiber::ptr rootFiber_;
        // use caller = true,调度器协程所在线程的id
        int rootThread_ = 0;
        bool isStopped_ = false;
    };
}