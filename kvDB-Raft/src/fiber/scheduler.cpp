#include "./include/scheduler.hpp"
#include "./include/fiber.hpp"
#include "./include/hook.hpp"

namespace monsoon
{
    // 每个线程都有自己的调度器指针和调度协程。
    // 当前线程的调度器，同一调度器下的所有线程共享同一调度器实例 （线程级调度器）
    static thread_local Scheduler *cur_scheduler = nullptr;
    // 当前线程中，从其他协程 yield 回来要恢复到的协程
    static thread_local Fiber *cur_scheduler_fiber = nullptr; // 调度协程

    const std::string LOG_HEAD = "[scheduler]";

    // threads是总共的线程数
    Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name)
    {
        CondPanic(threads > 0, "threads <= 0");

        isUseCaller_ = use_caller;
        name_ = name;

        // use_caller: 表示当前线程（构造调度器的线程）也参与协程调度。
        if (use_caller)
        {
            std::cout << LOG_HEAD << "current thread as called thread" << std::endl;

            // 如果主线程也参与调度，则从总线程数中减去 1，其他线程由线程池创建。
            --threads;
            // 获取当前线程的主协程
            Fiber::GetThis();
            std::cout << LOG_HEAD << "init caller thread's main fiber success" << std::endl;
            CondPanic(GetThis() == nullptr, "GetThis err:cur scheduler is not nullptr");
            // 设置当前线程为调度器线程
            cur_scheduler = this;
            // 初始化当前线程的调度协程，调度协程运行 Scheduler::run()，调度器主循环
            // rootFiber_只有创建调度器的线程才会有，其他线程不经过Scheduler的构造函数，它们只通过线程入口函数调用Scheduler::run也就是Scheduler::start中创建线程的时机
            rootFiber_.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // run_inscheduler = false：该协程不参与调度，只是调度器驱动程序
            std::cout << LOG_HEAD << "init caller thread's caller fiber success" << std::endl;

            Thread::SetName(name_);
            cur_scheduler_fiber = rootFiber_.get();
            rootThread_ = GetThreadId();
            threadIds_.push_back(rootThread_);
        }
        else
            rootThread_ = -1;
        threadCnt_ = threads;
        std::cout << "-------scheduler init success-------" << std::endl;
    }

    Scheduler *Scheduler::GetThis()
    {
        return cur_scheduler;
    }

    Fiber *Scheduler::GetMainFiber()
    {
        return cur_scheduler_fiber;
    }
    void Scheduler::setThis()
    {
        cur_scheduler = this;
    }
    Scheduler::~Scheduler()
    {
        CondPanic(isStopped_, "isstopped is false");
        if (GetThis() == this)
        {
            cur_scheduler = nullptr;
        }
    }

    // 调度器启动
    // 初始化调度线程池
    void Scheduler::start()
    {
        std::cout << LOG_HEAD << "scheduler start" << std::endl;
        Mutex::Lock lock(mutex_);
        if (isStopped_)
        {
            std::cout << "scheduler has stopped" << std::endl;
            return;
        }

        CondPanic(threadPool_.empty(), "thread pool is not empty");
        threadPool_.resize(threadCnt_);
        for (size_t i = 0; i < threadCnt_; i++)
        {
            threadPool_[i].reset(new Thread(std::bind(&Scheduler::run, this), name_ + "_" + std::to_string(i)));
            threadIds_.push_back(threadPool_[i]->getId());
        }
    }

    // 每个调度线程（包括 use_caller 情况下的 caller thread，caller thread是通过其调度协程来执行run的）执行的主函数
    // 会不断循环从调度器任务队列中取出任务（协程或回调），并恢复执行。
    void Scheduler::run()
    {
        std::cout << LOG_HEAD << "begin run" << std::endl;
        set_hook_enable(true);
        setThis(); // 设置当前线程调度器
        if (GetThreadId() != rootThread_) // 只有当 use_caller == true 时，才会把当前线程（调用调度器构造函数的线程）设为 rootThread_。
        {
            // 如果当前线程不是caller线程，则初始化该线程的主协程
            cur_scheduler_fiber = Fiber::GetThis().get();
        }

        // 创建idle协程， 当任务队列为空时执行（持续 yield 自己）。
        Fiber::ptr idleFiber(new Fiber(std::bind(&Scheduler::idle, this)));
        Fiber::ptr cbFiber;

        SchedulerTask task;
        while (true)
        {
            task.reset();
            bool tickle_me = false; // 是否需要“唤醒”其他线程（即是否还有任务没被调度）。
            {
                Mutex::Lock lock(mutex_);
                auto it = tasks_.begin();
                while (it != tasks_.end())
                {
                    // 任务被绑定到其他线程（该任务要在其他线程中执行），跳过当前任务
                    if (it->thread_ != -1 && it->thread_ != GetThreadId())
                    {
                        ++it;
                        tickle_me = true; // 设置 tickle_me = true，告诉其他线程队列有任务。
                        continue;
                    }
                    CondPanic(it->fiber_ || it->cb_, "task is nullptr");
                    if (it->fiber_) // 如果是协程任务，必须处于 READY 状态才能被调度
                        CondPanic(it->fiber_->getState() == Fiber::READY, "fiber task state error");

                    // 找到一个可进行任务，准备开始调度，从任务队列取出，活动线程加1
                    task = *it;
                    tasks_.erase(it++);
                    ++activeThreadCnt_;
                    break;
                }
                // 当前线程拿出一个任务后，任务队列还有任务要执行，那么告诉其他线程来调度
                tickle_me |= (it != tasks_.end());
            }
            if (tickle_me)
            {
                tickle();
            }

            if (task.fiber_)
            {
                // 开始执行 协程任务
                task.fiber_->resume();
                // 执行结束
                --activeThreadCnt_;
                task.reset(); // 如果这个task的任务执行到一半就退出了，那么应该要将此任务重新放入任务队列中。
            }
            else if (task.cb_)
            {
                // 如果是函数任务，用 cbFiber 包装成协程执行
                if (cbFiber)
                {
                    cbFiber->reset(task.cb_);
                }
                else
                {
                    cbFiber.reset(new Fiber(task.cb_));
                }
                task.reset();
                cbFiber->resume();
                --activeThreadCnt_;
                cbFiber.reset();
            }
            else // 任务队列为空
            {
                if (idleFiber->getState() == Fiber::TERM)
                {
                    // 如果调度器没有调度任务，那么idle协程会不停地resume/yield，不会结束，如果idle协程结束了，那一定是调度器停止了
                    std::cout << "idle fiber term" << std::endl;
                    break;
                }
                // idle协程不断空轮转
                ++idleThreadCnt_;
                idleFiber->resume();
                --idleThreadCnt_;
            }
        }
        std::cout << "run exit" << std::endl;
    }

    void Scheduler::tickle()
    {
        std::cout << "tickle" << std::endl;
    }

    // 返回调度器是否可以「安全地终止运行」
    bool Scheduler::stopping()
    {
        Mutex::Lock lock(mutex_);
        return isStopped_ && tasks_.empty() && activeThreadCnt_ == 0;
    }

    void Scheduler::idle()
    {
        while (!stopping())
        {
            Fiber::GetThis()->yield();
        }
    }

    // 使用caller线程，则调度线程依赖stop()来执行caller线程的调度协程
    // 不使用caller线程，只用caller线程去调度，则调度器真正开始执行的位置是stop()
    void Scheduler::stop()
    {
        std::cout << LOG_HEAD << "stop" << std::endl;
        if (stopping())
            return;

        isStopped_ = true;

        // stop指令只能由caller线程发起
        if (isUseCaller_)
        {
            CondPanic(GetThis() == this, "cur thread is not caller thread");
        }
        else
        {
            CondPanic(GetThis() != this, "cur thread is caller thread");
        }

        for (size_t i = 0; i<threadCnt_; i++)
            tickle();
        
        if (rootFiber_)
            tickle();
        
        // 在user_caller情况下，调度器协程（rootFiber）结束后，应该返回caller协程  
        // 使用了caller线程的情况下，调度器依赖stop方法来执行caller线程的调度协程，如果调度器只使用了caller线程来调度，那调度器真正开始执行调度的位置就是这个stop方法
        if (rootFiber_)
        {
            // 切换到调度协程，开始调度
            rootFiber_->resume();
            std::cout << "root fiber end" << std::endl;
        }

        std::vector<Thread::ptr> threads;
        {
            Mutex::Lock lock(mutex_);
            threads.swap(threadPool_);
        }
        for (auto &i : threads)
            i->join();
    }
}