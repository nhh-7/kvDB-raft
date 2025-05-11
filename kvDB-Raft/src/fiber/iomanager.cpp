#include "./include/iomanager.hpp"

namespace monsoon
{
    // 获取事件上下文
    EventContext &FdContext::getEveContext(Event event)
    {
        switch (event)
        {
        case READ:
            return read;
        case WRITE:
            return write;
        default:
            CondPanic(false, "getContext error: unknow event");
        }
        throw std::invalid_argument("getContext invalid event");
    }

    // 重置事件上下文 用于事件被触发后清理现场。
    void FdContext::resetEveContext(EventContext &ctx)
    {
        ctx.scheduler = nullptr;
        ctx.fiber.reset();
        ctx.cb = nullptr;
    }

    // 触发事件（只是将对应的fiber or cb 加入scheduler tasklist）
    // 触发指定的 IO 事件 —— 将事件绑定的协程/回调加入调度器执行队列
    void FdContext::triggerEvent(Event event)
    {
        CondPanic(events & event, "event hasnot been registed");
        events = (Event)(events & ~event); // 清除 events 中该事件标志。
        EventContext &ctx = getEveContext(event);
        if (ctx.cb)
            ctx.scheduler->scheduler(ctx.cb);
        else
            ctx.scheduler->scheduler(ctx.fiber);
        resetEveContext(ctx);
        return;
    }

    IOManager::IOManager(size_t threads, bool use_caller, const std::string &name)
        : Scheduler(threads, use_caller, name)
    {
        epfd_ = epoll_create(5000);
        int ret = pipe(tickleFds_);
        CondPanic(ret == 0, "pipe error");

        // 注册pipe读句柄的可读事件，用于tickle调度协程
        epoll_event event{};
        memset(&event, 0, sizeof(epoll_event));
        event.events = EPOLLIN | EPOLLET;
        event.data.fd = tickleFds_[0]; // 通过epoll_event.data.fd保存描述符

        // 边缘触发，设置非阻塞
        ret = fcntl(tickleFds_[0], F_SETFL, O_NONBLOCK);
        CondPanic(ret == 0, "set fd nonblock error");

        // 将管道的读描述符加入epoll多路复用 如果管道可读，idle中的epoll_wait会返回
        ret = epoll_ctl(epfd_, EPOLL_CTL_ADD, tickleFds_[0], &event); // tickleFds_[0] 是读取端：注册为边缘触发 + 非阻塞输入事件
        CondPanic(ret == 0, "epoll_ctl error");

        contextResize(32); // 初始化 fd 上下文数组大小

        // 启动scheduler，开始进行协程调度
        start();
    }

    IOManager::~IOManager()
    {
        stop(); // 停止调度器
        close(epfd_);
        close(tickleFds_[0]);
        close(tickleFds_[1]);

        for (size_t i = 0; i < fdcontexts_.size(); i++)
        {
            if (fdcontexts_[i])
            {
                delete fdcontexts_[i];
            }
        }
    }

    // 添加事件 为某个 fd 添加 READ/WRITE 事件，并绑定协程或回调。 fd描述符发生了event事件时执行cb函数
    int IOManager::addEvent(int fd, Event event, std::function<void()> cb)
    {
        FdContext *fd_ctx = nullptr;
        RWMutex::ReadLock lock(mutex_);

        // 找到fd对应的fdContext,没有则创建
        if ((int)fdcontexts_.size() > fd)
        {
            fd_ctx = fdcontexts_[fd];
            lock.unlock();
        }
        else
        {
            lock.unlock();
            RWMutex::WriteLock lock2(mutex_);
            contextResize(fd * 1.5);
            fd_ctx = fdcontexts_[fd];
        }

        Mutex::Lock ctxLock(fd_ctx->mutex);
        // 检查是否重复添加事件
        CondPanic(!(fd_ctx->events & event), "addevent error, fd=" + fd);

        int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
        epoll_event epevent;
        epevent.events = EPOLLET | fd_ctx->events | event; // 设置为 边缘触发（EPOLLET），并合并原有事件和新事件
        epevent.data.ptr = fd_ctx;

        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "addevent: epoll ctl error" << std::endl;
            return -1;
        }
        // 待执行IO事件数量
        ++pendingEventCnt_;

        // 更新 FdContext 的事件信息，添加本次事件类型。
        fd_ctx->events = (Event)(fd_ctx->events | event);
        // 获取该事件的上下文引用（READ 或 WRITE）。
        EventContext &event_ctx = fd_ctx->getEveContext(event);
        CondPanic(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb, "event-ctx is nullptr");

        event_ctx.scheduler = Scheduler::GetThis();
        if (cb)
        {
            event_ctx.cb.swap(cb);
        }
        else
        {
            // 如果回调函数为空，则把当前协程当成回调执行体
            event_ctx.fiber = Fiber::GetThis();
            CondPanic(event_ctx.fiber->getState() == Fiber::RUNNING, "state=" + event_ctx.fiber->getState());
        }
        std::cout << "add event success,fd = " << fd << std::endl;
        return 0;
    }

    // 删除事件(删除前不会主动触发事件)
    bool IOManager::delEvent(int fd, Event event)
    {
        RWMutex::ReadLock lock(mutex_);
        if ((int)fdcontexts_.size() <= fd)
        {
            // 找不到当前事件，返回
            return false;
        }

        FdContext *fd_ctx = fdcontexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        // 如果没有注册该事件，返回
        if (!(fd_ctx->events & event))
            return false;

        Event new_events = (Event)(fd_ctx->events & ~event); // 清理event事件
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int ret = epoll_ctl(epfd_, op, fd, &epevent); // 返回0表示成功
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        --pendingEventCnt_;
        fd_ctx->events = new_events;
        EventContext &event_ctx = fd_ctx->getEveContext(event);
        fd_ctx->resetEveContext(event_ctx);
        return true;
    }

    // 取消事件 （取消前会主动触发事件）
    bool IOManager::cancelEvent(int fd, Event event)
    {
        RWMutex::ReadLock lock(mutex_);
        if ((int)fdcontexts_.size() <= fd)
            return false;

        FdContext *fd_ctx = fdcontexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        if (!(fd_ctx->events & event)) // 该事件没有注册过，也返回 false。
            return false;

        // 清理指定事件
        Event new_events = (Event)(fd_ctx->events & ~event);
        int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = EPOLLET | new_events;
        epevent.data.ptr = fd_ctx;

        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        // 删除之前，触发以此事件
        fd_ctx->triggerEvent(event);
        --pendingEventCnt_;
        return true;
    }

    // 取消fd所有事件
    bool IOManager::cancelAll(int fd)
    {
        RWMutex::ReadLock lock(mutex_);
        if ((int)fdcontexts_.size() <= fd)
            return false;

        FdContext *fd_ctx = fdcontexts_[fd];
        lock.unlock();

        Mutex::Lock ctxLock(fd_ctx->mutex);
        if (!fd_ctx->events)
            return false;

        int op = EPOLL_CTL_DEL;
        epoll_event epevent;
        epevent.events = 0;
        epevent.data.ptr = fd_ctx;

        int ret = epoll_ctl(epfd_, op, fd, &epevent);
        if (ret)
        {
            std::cout << "delevent: epoll_ctl error" << std::endl;
            return false;
        }

        // 触发全部已注册事件
        if (fd_ctx->events & READ)
        {
            fd_ctx->triggerEvent(READ);
            --pendingEventCnt_;
        }
        if (fd_ctx->events & WRITE)
        {
            fd_ctx->triggerEvent(WRITE);
            --pendingEventCnt_;
        }

        CondPanic(fd_ctx->events == 0, "fd not totally clear");
        return true;
    }

    IOManager *IOManager::GetThis()
    {
        return dynamic_cast<IOManager *>(Scheduler::GetThis()); // 将当前线程绑定的调度器向下转换成 IOManager*。
    }

    // 通知调度器有任务到来
    void IOManager::tickle()
    {
        if (!isHasIdleThreads())
        {
            // 当前没有空闲调度线程
            return;
        }

        // 通过 pipe 写入字节唤醒它，用于有任务要调度时唤醒协程线程
        int rt = write(tickleFds_[1], "T", 1);
        CondPanic(rt == 1, "write pipe error");
    }

    /**
     * @brief idle协程
     * @details 对于IO协程调度来说，应阻塞在等待IO事件上，idle退出的时机是epoll_wait返回，对应的操作是tickle或注册的IO事件就绪
     * 调度器无调度任务时会阻塞idle协程上，对IO调度器而言，idle状态应该关注两件事，一是有没有新的调度任务，对应Schduler::schedule()，
     * 如果有新的调度任务，那应该立即退出idle状态，并执行对应的任务；二是关注当前注册的所有IO事件有没有触发，如果有触发，那么应该执行
     * IO事件对应的回调函数
     */
    void IOManager::idle()
    {
        const uint64_t MAX_EVENTS = 256;
        epoll_event *events = new epoll_event[MAX_EVENTS]();
        std::shared_ptr<epoll_event> shared_events(events, [](epoll_event *ptr)
                                                   { delete[] ptr; });

        while (true)
        {
            //  获取下一个定时器超时时间，同时判断调度器是否已经stop
            uint64_t next_timeout = 0;
            if (stopping(next_timeout))
            {
                std::cout << "name=" << getName() << "idle stopping exit";
                break;
            }

            // 阻塞等待，等待事件发生 或者 定时器超时
            int ret = 0;
            do
            {
                static const int MAX_TIMEOUT = 5000;

                if (next_timeout != ~0ull)
                    next_timeout = std::min((int)next_timeout, MAX_TIMEOUT);
                else
                    next_timeout = MAX_TIMEOUT;

                // 阻塞等待事件就绪
                ret = epoll_wait(epfd_, events, MAX_EVENTS, (int)next_timeout);

                if (ret < 0)
                {
                    if (errno == EINTR)
                    {
                        // 系统调用被信号中断
                        continue;
                    }
                    std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
                    break;
                }
                else
                    break;
            } while (true);

            std::vector<std::function<void()>> cbs;
            listExpiredCb(cbs);
            if (!cbs.empty())
            {
                for (const auto &cb : cbs)
                    scheduler(cb);
                cbs.clear();
            }

            for (int i = 0; i < ret; i++)
            {
                epoll_event &event = events[i];
                if (event.data.fd == tickleFds_[0])
                {
                    // pipe管道内数据无意义，只是tickle意义,读完即可
                    uint8_t dummy[256];

                    while (read(tickleFds_[0], dummy, sizeof(dummy)) > 0)
                        ;
                    continue;
                }
                //  通过epoll_event的私有指针获取FdContext
                FdContext *fd_ctx = (FdContext *)event.data.ptr;
                Mutex::Lock lock(fd_ctx->mutex);

                /**
                 * EPOLLERR: 出错，比如写读端已经关闭的pipe
                 * EPOLLHUP: 套接字对端关闭
                 * 出现这两种事件，应该同时触发fd的读和写事件，否则有可能出现注册的事件永远执行不到的情况
                 */
                if (event.events & (EPOLLERR | EPOLLHUP))
                {
                    std::cout << "error events" << std::endl;
                    event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
                }

                // 实际发生的事件类型
                int real_events = NONE;
                if (event.events & EPOLLIN)
                {
                    real_events |= READ;
                }
                if (event.events & EPOLLOUT)
                {
                    real_events |= WRITE;
                }
                if ((fd_ctx->events & real_events) == NONE)
                {
                    // 触发的事件类型与注册的事件类型无交集
                    continue;
                }

                // 剔除已经发生的事件，将剩余的事件重新加入epoll_wait
                int left_events = (fd_ctx->events & ~real_events);
                int op = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
                event.events = EPOLLET | left_events;

                int ret2 = epoll_ctl(epfd_, op, fd_ctx->fd, &event);
                if (ret2)
                {
                    std::cout << "epoll_wait [" << epfd_ << "] errno,err: " << errno << std::endl;
                    continue;
                }

                // 触发事件，唤醒协程
                // triggerEvent实际也只是把对应的fiber重新加入调度，要执行的话还要等idle协程退出
                if (real_events & READ)
                {
                    fd_ctx->triggerEvent(READ);
                    --pendingEventCnt_;
                }
                if (real_events & WRITE)
                {
                    fd_ctx->triggerEvent(WRITE);
                    --pendingEventCnt_;
                }
            }

            // 处理结束，idle协程yield,此时调度协程可以执行run去tasklist中
            // 检测，拿取新任务去调度
            Fiber::ptr cur = Fiber::GetThis();
            auto raw_ptr = cur.get();
            cur.reset();
            raw_ptr->yield();
        }
    }

    bool IOManager::stopping()
    {
        uint64_t timeout = 0;
        return stopping(timeout);
    }

    bool IOManager::stopping(uint64_t &timeout)
    {
        timeout = getNextTimer();
        return timeout == ~0ull && pendingEventCnt_ == 0 && Scheduler::stopping();
    }

    // 扩容 fdContexts_ 的数组，并为每一个空位分配新的 FdContext。
    void IOManager::contextResize(size_t size)
    {
        fdcontexts_.resize(size);
        for (size_t i = 0; i < fdcontexts_.size(); i++)
        {
            if (!fdcontexts_[i])
            {
                fdcontexts_[i] = new FdContext;
                fdcontexts_[i]->fd = i;
            }
        }
    }

    void IOManager::OnTimerInsertedAtFront()
    {
        tickle();
    }
}