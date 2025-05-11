#pragma once

/**
 * 定义的是一个基于 epoll 的 IO 管理器（IOManager），是一个协程调度器（Scheduler）的扩展，用来实现 协程级别的异步 IO 事件管理。
 */

#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>
#include "scheduler.hpp"
#include "timer.hpp"

namespace monsoon
{
    // IO 事件的类型
    enum Event
    {
        NONE = 0x0,
        READ = 0x1,
        WRITE = 0x4,
    };

    // 每个文件描述符的每个事件（读/写）都对应一个 EventContext，它表示当事件发生时，需要调用的调度器、协程或函数
    struct EventContext
    {
        Scheduler *scheduler = nullptr; // 执行事件回调的调度器
        Fiber::ptr fiber;               // 事件回调协程
        std::function<void()> cb;       // 事件回调函数
    };

    // FdContext 表示一个文件描述符（fd）及其关联的事件和行为
    class FdContext
    {
        friend class IOManager;

    public:
        // 获取事件上下文
        EventContext &getEveContext(Event event);

        // 重置事件上下文
        void resetEveContext(EventContext &ctx);

        // 触发事件 （将 fiber 或 cb 加入调度器）
        void triggerEvent(Event event);

    private:
        EventContext read;  // 读事件的上下文
        EventContext write; // 写事件的上下文
        int fd = 0;
        Event events = NONE; // 当前注册的事件（读/写）
        Mutex mutex;
    };

    // 专门处理 IO 事件和协程调度的结合。
    class IOManager : public Scheduler, public TimerManager
    {
    public:
        typedef std::shared_ptr<IOManager> ptr;

        IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
        ~IOManager();

        // 添加事件
        int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
        // 删除事件
        bool delEvent(int fd, Event event);
        // 取消事件
        bool cancelEvent(int fd, Event event);
        // 取消所有事件
        bool cancelAll(int fd);
        static IOManager *GetThis();

    protected:
        // 当有新任务到来时，唤醒 epoll 阻塞；
        void tickle() override;
        bool stopping() override;
        // 如果当前线程没有任务可执行，则进入 idle 协程，执行 epoll_wait 等待事件
        void idle() override;
        // 判断是否可以停止，同时获取最近一个定时超时时间
        bool stopping(uint64_t &timeout);

        // 当新的定时器任务提前插入到队列最前时，唤醒 idle
        void OnTimerInsertedAtFront() override;
        void contextResize(size_t size);

    private:
        int epfd_ = 0;     // epoll 文件描述符
        int tickleFds_[2]; // pipe 用于唤醒 idle 协程（用于线程间通信） 用于当有新任务时通知 epoll 等待线程， fd[0]读端，fd[1]写端
        // 正在等待执行的IO事件数量
        std::atomic<size_t> pendingEventCnt_ = {0};
        RWMutex mutex_;
        std::vector<FdContext *> fdcontexts_;
    };
}