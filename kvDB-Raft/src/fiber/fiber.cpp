#include "./include/fiber.hpp"
#include <assert.h>
#include <atomic>
#include "./include/scheduler.hpp"
#include "./include/utils.hpp"

namespace monsoon
{
    const bool DEBUG = true;

    // 当前线程正在运行的协程  必须时刻指向当前正在运行的协程对象
    static thread_local Fiber *cur_fiber = nullptr;
    // 当前线程的主协程  协程模块初始化时，t_thread_fiber指向线程主协程对象。当子协程resume时，通过swapcontext将主协程的上下文保存到t_thread_fiber的ucontext_t成员中，同时激活子协程的ucontext_t上下文。当子协程yield时，从t_thread_fiber中取得主协程的上下文并恢复运行。
    static thread_local Fiber::ptr cur_thread_fiber = nullptr;
    // 用于生成协程Id
    static std::atomic<uint64_t> cur_fiber_id{0};
    // 统计当前协程数
    static std::atomic<uint64_t> fiber_count{0};
    // 协程栈默认大小 128k
    static int g_fiber_stack_size = 128 * 1024;

    class StackAllocator
    {
    public:
        // 从堆中申请 size 字节的原始内存 在协程中，这块内存将作为栈空间使用
        static void *Alloc(size_t size)
        {
            return malloc(size);
        }

        static void Delete(void *vp, size_t size)
        {
            return free(vp);
        }
    };

    // 构造当前线程的主协程。只在 Fiber::GetThis() 中第一次调用。 不带参的构造函数用于初始化当前线程的协程功能，构造线程主协程对象
    Fiber::Fiber()
    {
        SetThis(this);
        state_ = RUNNING;
        CondPanic(getcontext(&ctx_) == 0, "getcontext error");
        ++fiber_count;
        id_ = cur_fiber_id++;
        std::cout << "[fiber] create fiber , id = " << id_ << std::endl;
    }

    // 设置当前协程
    void Fiber::SetThis(Fiber *f)
    {
        cur_fiber = f;
    }

    // 获取当前线程中正在执行的协程 Fiber，如果还没有，则自动创建一个“主协程”并返回。 其他协程都通过这个协程来调度，也就是说，其他协程结束时,都要切回到主协程，由主协程重新选择新的协程进行resume
    Fiber::ptr Fiber::GetThis()
    {
        if (cur_fiber)
            return cur_fiber->shared_from_this();

        // 创建主协程并初始化
        Fiber::ptr main_fiber(new Fiber);
        CondPanic(cur_fiber == main_fiber.get(), "cur_fiber need to be main_fiber");
        cur_thread_fiber = main_fiber;
        return cur_fiber->shared_from_this();
    }

    // 有参构造，并为新的子协程创建栈空间
    Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_inscheduler)
        : id_(cur_fiber_id++), cb_(cb), isRunInScheduler_(run_inscheduler)
    {
        ++fiber_count;
        stackSize_ = stacksize > 0 ? stacksize : g_fiber_stack_size;
        stack_ptr = StackAllocator::Alloc(stackSize_);

        // 获取当前线程的上下文信息, 把当前线程的运行状态填充到 ctx_ 结构里
        CondPanic(getcontext(&ctx_) == 0, "getcontext error"); 
        // 初始化新协程上下文
        ctx_.uc_link = nullptr;
        ctx_.uc_stack.ss_sp = stack_ptr;
        ctx_.uc_stack.ss_size = stackSize_;
        makecontext(&ctx_, &Fiber::MainFunc, 0); // 当使用swapcontext()恢复此上下文时，程序调用MainFunc继续执行
    }

    // 切换当前协程到执行态,并保存主协程的上下文
    void Fiber::resume()
    {
        /*
            根据协程是否参与 Scheduler 调度器决定与哪个上下文 swap：
                是调度器内协程，和调度器协程 swapcontext
                否则与主协程 swap
        */

        CondPanic(state_ != TERM && state_ != RUNNING, "state error");
        SetThis(this);
        state_ = RUNNING;

        if (isRunInScheduler_) 
        {
            // 返回到线程中的cur_scheduler_fiber
            CondPanic(0 == swapcontext(&(Scheduler::GetMainFiber()->ctx_), &ctx_), "isRunInScheduler_ = true,swapcontext error");
        }
        else
        {
            CondPanic(0 == swapcontext(&(cur_thread_fiber->ctx_), &ctx_), "isRunInScheduler_ = false,swapcontext error");
        }
    }

    // 当前协程让出 CPU
    // 协程执行完成之后会自动yield,回到主协程，此时状态为TEAM
    void Fiber::yield()
    {
        CondPanic(state_ == TERM || state_ == RUNNING, "state error");
        SetThis(cur_thread_fiber.get()); // 切回主协程
        if (state_ != TERM)
            state_ = READY; // 将协程设为ReaDY态
        if (isRunInScheduler_)
        {
            CondPanic(0 == swapcontext(&ctx_, &(Scheduler::GetMainFiber()->ctx_)), "isRunInScheduler_ = true,swapcontext error");
        }
        else
        {
            // 切换当前协程到主协程，并保存子协程的上下文到主协程ctx_
            CondPanic(0 == swapcontext(&ctx_, &(cur_thread_fiber->ctx_)), "swapcontext failed");
        }
    }

    void Fiber::MainFunc()
    {
        Fiber::ptr cur = GetThis();
        CondPanic(cur != nullptr, "cur is nullptr");

        cur->cb_();
        cur->cb_ = nullptr;
        cur->state_ = TERM; // 协程任务结束，变成TERM态
        // 手动使得cur_fiber引用计数减1
        auto raw_ptr = cur.get();
        cur.reset();
        // 协程结束，自动yield,回到主协程
        // 访问原始指针原因：reset后cur已经被释放
        raw_ptr->yield();
    }

    // 协程重置（复用已经结束的协程，复用其栈空间，创建新协程）
    void Fiber::reset(std::function<void()> cb)
    {
        CondPanic(stack_ptr, "stack is nullptr");
        CondPanic(state_ == TERM, "state isnot TERM"); // 强制只有TERM状态的协程才可以重置
        cb_ = cb;
        CondPanic(0 == getcontext(&ctx_), "getcontext failed");
        ctx_.uc_link = nullptr;
        ctx_.uc_stack.ss_sp = stack_ptr;
        ctx_.uc_stack.ss_size = stackSize_;
        
        makecontext(&ctx_, &Fiber::MainFunc, 0);
        state_ = READY;
    }

    Fiber::~Fiber()
    {
        --fiber_count;
        if (stack_ptr)
        {
            // 有栈空间，说明是子协程
            CondPanic(state_ == TERM, "fiber state should be term");
            StackAllocator::Delete(stack_ptr, stackSize_);
        }
        else
        {
            CondPanic(!cb_, "main fiber no callback");
            CondPanic(state_ == RUNNING, "main fiber state should be running");

            Fiber *cur = cur_fiber;
            if (cur == this)
                SetThis(nullptr);
        }
    }
}