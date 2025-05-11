#include "./include/hook.hpp"
#include <dlfcn.h>
#include <cstdarg>
#include <string>
#include "./include/fd_manager.hpp"
#include "./include/fiber.hpp"
#include "./include/iomanager.hpp"

namespace monsoon
{
    // 当前线程是否启用hook
    static thread_local bool t_hook_enable = false;
    static int g_tcp_connect_timeout = 5000;

#define HOOK_FUN(XX) \
    XX(sleep)        \
    XX(usleep)       \
    XX(nanosleep)    \
    XX(socket)       \
    XX(connect)      \
    XX(accept)       \
    XX(read)         \
    XX(readv)        \
    XX(recv)         \
    XX(recvfrom)     \
    XX(recvmsg)      \
    XX(write)        \
    XX(writev)       \
    XX(send)         \
    XX(sendto)       \
    XX(sendmsg)      \
    XX(close)        \
    XX(fcntl)        \
    XX(ioctl)        \
    XX(getsockopt)   \
    XX(setsockopt)

    void hook_init()
    {
        static bool is_inited = false;
        if (is_inited)
            return;
        // dlsym:Dynamic LinKinf Library.返回指定符号的地址
#define XX(name) name##_f = (name##_fun)dlsym(RTLD_NEXT, #name); // 使用 dlsym(RTLD_NEXT, ...) 动态获取原始系统调用函数地址,并保存下来
        HOOK_FUN(XX);
#undef XX
    }

    static uint64_t s_connect_timeout = -1;
    struct _HOOKIniter
    {
        _HOOKIniter()
        {
            // hook_init放在静态对象中，则在main函数执行之前就会获取各个符号地址并
            // 保存到全局变量中
            hook_init();
            s_connect_timeout = g_tcp_connect_timeout;
        }
    };
    static _HOOKIniter s_hook_initer;

    bool is_hook_enable()
    {
        return t_hook_enable;
    }
    void set_hook_enable(const bool flag)
    {
        t_hook_enable = flag;
    }

    struct timer_info
    {
        int cnacelled = 0;
    };

    /** 封装socket上的所有 read/write/send/recv 等 IO 系统调用。
     * 执行逻辑：
     *  如果 hook 未启用，直接调用原始系统调用 fun(fd, args...)。
        获取 FdCtx（文件描述符上下文）检查是否是 socket 且是否为非用户主动设置的非阻塞。
        尝试执行 IO 操作，如果返回 EAGAIN，说明没有数据/缓冲区满：
            注册 IO 事件到 IOManager。
            添加超时定时器（可选）。
            当前协程 yield，让出执行权。
            等待 IOManager 触发 IO 或超时。
            返回后重新执行（goto retry）。
     */
    template <typename OriginFun, typename... Args>
    static ssize_t do_io(int fd, OriginFun fun, const char *hook_fun_name, uint32_t event, int timeout_so, Args &&...args)
    {
        if (!t_hook_enable)
            return fun(fd, std::forward<Args>(args)...);

        // 获取 FdCtx
        FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
        // 若未找到，说明不受支持（可能不是 socket），直接原始调用
        if (!ctx)
        {
            return fun(fd, std::forward<Args>(args)...);
        }

        if (ctx->isClose())
        {
            errno = EBADF;
            return -1;
        }

        // 非 socket 不支持协程挂起（比如普通文件、管道等）
        if (!ctx->isSocket() || ctx->getUserNonblock())
        {
            return fun(fd, std::forward<Args>(args)...);
        }

        // 获取对应type的fd超时时间
        uint64_t to = ctx->getTimeout(timeout_so);
        std::shared_ptr<timer_info> tinfo(new timer_info);

    retry:
        // 尝试执行 IO 操作
        ssize_t n = fun(fd, std::forward<Args>(args)...);
        while (n == -1 && errno == EINTR)
        {
            // 读取操作被信号中断，继续尝试
            n = fun(fd, std::forward<Args>(args)...);
        }
        if (n == -1 && errno == EAGAIN)
        {
            // 数据未就绪
            IOManager *iom = IOManager::GetThis();
            Timer::ptr timer;
            std::weak_ptr<timer_info> winfo(tinfo);

            /**
             * 创建一个定时器，时间为 to，一旦超时会
             *  设置 tinfo->cnacelled = ETIMEDOUT
                从 IOManager 中取消 fd 上的事件监听
             */
            if (to != (uint64_t)-1)
            {
                timer = iom->addConditionTimer(to, [winfo, fd, iom, event]()
                                               {
                    auto t = winfo.lock();
                    if (!t || t->cnacelled)
                        return;
                    t->cnacelled = ETIMEDOUT;
                    iom->cancelEvent(fd, (Event)(event)); }, winfo);
            }

            // 注册 IO 事件
            int rt = iom->addEvent(fd, (Event)(event));
            if (rt)
            {
                std::cout << hook_fun_name << " addEvent(" << fd << ", " << event << ")";
                if (timer)
                    timer->cancel();

                return -1;
            }
            else
            {
                // 协程让出执行权，等待 IO 事件或定时器唤醒
                // IOManager 会在事件就绪或定时器超时后，重新调度此协程运行
                Fiber::GetThis()->yield();
                if (timer)
                {
                    timer->cancel();
                }
                if (tinfo->cnacelled)
                {
                    errno = tinfo->cnacelled;
                    return -1;
                }
                goto retry;
            }
        }
        return n;
    }

    extern "C"
    {
#define XX(name) name##_fun name##_f = nullptr;
        HOOK_FUN(XX);
#undef XX

        unsigned int sleep(unsigned int seconds)
        {
            // 不允许hook,则直接使用系统调用
            if (!t_hook_enable)
                return sleep_f(seconds);

            // 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager *iom = IOManager::GetThis();
            iom->addTimer(seconds * 1000, std::bind((void (Scheduler::*)(Fiber::ptr, int thread))&IOManager::scheduler, iom, fiber, -1)); // 在 IOManager 中注册一个定时器，seconds 秒后，调用 iom->scheduler(fiber, -1)，把 fiber 这个协程重新交由调度器调度执行。
            Fiber::GetThis()->yield();
            return 0;
        }

        // usleep 在指定的微妙数内暂停线程运行
        int usleep(useconds_t usec)
        {
            if (!t_hook_enable)
            {
                // 不允许hook,则直接使用系统调用
                auto ret = usleep_f(usec);
                return 0;
            }
            // 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager *iom = IOManager::GetThis();
            iom->addTimer(usec / 1000,
                          std::bind((void (Scheduler::*)(Fiber::ptr, int thread))&IOManager::scheduler, iom, fiber, -1));
            Fiber::GetThis()->yield();
            return 0;
        }

        // nanosleep 在指定的纳秒数内暂停当前线程的执行
        int nanosleep(const struct timespec *req, struct timespec *rem)
        {
            if (!t_hook_enable)
            {
                // 不允许hook,则直接使用系统调用
                return nanosleep_f(req, rem);
            }
            // 允许hook,则直接让当前协程退出，seconds秒后再重启（by定时器）
            Fiber::ptr fiber = Fiber::GetThis();
            IOManager *iom = IOManager::GetThis();
            int timeout_s = req->tv_sec * 1000 + req->tv_nsec / 1000 / 1000;
            iom->addTimer(timeout_s,
                          std::bind((void (Scheduler::*)(Fiber::ptr, int thread))&IOManager::scheduler, iom, fiber, -1));
            Fiber::GetThis()->yield();
            return 0;
        }

        int socket(int domain, int type, int protocol)
        {
            if (!t_hook_enable)
                return socket_f(domain, type, protocol);

            int fd = socket_f(domain, type, protocol);
            if (fd == -1)
                return fd;
            // 将fd加入Fdmanager中
            FdMgr::GetInstance()->get(fd, true);
            return fd;
        }

        // 处理 connect 的超时问题
        // 让原本阻塞的 connect 在协程中“变成异步”的形式运行，并支持超时控制
        int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms)
        {
            if (!t_hook_enable)
                return connect_f(fd, addr, addrlen);
            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if (!ctx || ctx->isClose())
            {
                errno = EBADF;
                return -1;
            }

            if (!ctx->isSocket())
                return connect_f(fd, addr, addrlen);

            if (ctx->getUserNonblock())
                return connect_f(fd, addr, addrlen);

            int n = connect_f(fd, addr, addrlen);
            if (n == 0)
                return 0;
            else if (n != -1 || errno != EINPROGRESS)
                return n;

            // 返回EINPEOGRESS:正在进行，但是尚未完成
            IOManager *iom = IOManager::GetThis();
            Timer::ptr timer;
            std::shared_ptr<timer_info> tinfo(new timer_info);
            std::weak_ptr<timer_info> winfo(tinfo);

            if (timeout_ms != (uint64_t)-1)
            {
                // 添加条件定时器
                timer = iom->addConditionTimer(timeout_ms, [winfo, fd, iom]()
                                               {
                    auto t = winfo.lock();
                    if (!t || t->cnacelled)
                        return;
                    //定时时间到达，设置超时标志，触发一次WRITE事件
                    t->cnacelled = ETIMEDOUT;
                    // cancelEvent(fd, WRITE) 会强制唤醒那个正在等待 WRITE 的协程
                    iom->cancelEvent(fd, WRITE); }, winfo);
            }

            // 添加WRITE事件，并yield,等待WRITE事件触发再往下执行
            int rt = iom->addEvent(fd, WRITE);
            if (rt == 0)
            {
                Fiber::GetThis()->yield();
                // 等待超时or套接字可写，协程返回
                if (timer)
                    timer->cancel();
                // 超时返回，通过超时标志设置errno并返回-1
                if (tinfo->cnacelled)
                {
                    errno = tinfo->cnacelled;
                    return -1;
                }
            }
            else
            {
                if (timer)
                    timer->cancel();
                std::cout << "connect addEvent(" << fd << ", WRITE) error" << std::endl;
            }

            int error = 0;
            socklen_t len = sizeof(int);
            // 获取套接字的错误状态
            if (-1 == getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len))
            {
                return -1;
            }
            if (!error)
                return 0;
            else
            {
                errno = error;
                return -1;
            }
        }

        int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
        {
            return monsoon::connect_with_timeout(sockfd, addr, addrlen, s_connect_timeout);
        }

        int accept(int s, struct sockaddr *addr, socklen_t *addrlen)
        {
            int fd = do_io(s, accept_f, "accept", READ, SO_RCVTIMEO, addr, addrlen);
            if (fd >= 0)
            {
                FdMgr::GetInstance()->get(fd, true);
            }
            return fd;
        }

        ssize_t read(int fd, void *buf, size_t count) { return do_io(fd, read_f, "read", READ, SO_RCVTIMEO, buf, count); }

        ssize_t readv(int fd, const struct iovec *iov, int iovcnt)
        {
            return do_io(fd, readv_f, "readv", READ, SO_RCVTIMEO, iov, iovcnt);
        }

        ssize_t recv(int sockfd, void *buf, size_t len, int flags)
        {
            return do_io(sockfd, recv_f, "recv", READ, SO_RCVTIMEO, buf, len, flags);
        }

        ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
        {
            return do_io(sockfd, recvfrom_f, "recvfrom", READ, SO_RCVTIMEO, buf, len, flags, src_addr, addrlen);
        }

        ssize_t recvmsg(int sockfd, struct msghdr *msg, int flags)
        {
            return do_io(sockfd, recvmsg_f, "recvmsg", READ, SO_RCVTIMEO, msg, flags);
        }

        ssize_t write(int fd, const void *buf, size_t count)
        {
            return do_io(fd, write_f, "write", WRITE, SO_SNDTIMEO, buf, count);
        }

        ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
        {
            return do_io(fd, writev_f, "writev", WRITE, SO_SNDTIMEO, iov, iovcnt);
        }

        ssize_t send(int s, const void *msg, size_t len, int flags)
        {
            return do_io(s, send_f, "send", WRITE, SO_SNDTIMEO, msg, len, flags);
        }

        ssize_t sendto(int s, const void *msg, size_t len, int flags, const struct sockaddr *to, socklen_t tolen)
        {
            return do_io(s, sendto_f, "sendto", WRITE, SO_SNDTIMEO, msg, len, flags, to, tolen);
        }

        ssize_t sendmsg(int s, const struct msghdr *msg, int flags)
        {
            return do_io(s, sendmsg_f, "sendmsg", WRITE, SO_SNDTIMEO, msg, flags);
        }

        int close(int fd)
        {
            if (!t_hook_enable)
                return close_f(fd);

            FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
            if (ctx)
            {
                auto iom = IOManager::GetThis();
                if (iom)
                    iom->cancelAll(fd);
                FdMgr::GetInstance()->del(fd);
            }
            return close_f(fd);
        }

        int fcntl(int fd, int cmd, ... /* arg */)
        {
            va_list va;
            va_start(va, cmd);
            switch (cmd)
            {
            case F_SETFL:
            {
                int arg = va_arg(va, int);
                va_end(va);
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if (!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return fcntl_f(fd, cmd, arg);
                }
                ctx->setUserNonblock(arg & O_NONBLOCK);
                if (ctx->getSysNonblock())
                {
                    arg |= O_NONBLOCK;
                }
                else
                {
                    arg &= ~O_NONBLOCK;
                }
                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETFL:
            {
                va_end(va);
                int arg = fcntl_f(fd, cmd);
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(fd);
                if (!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return arg;
                }
                if (ctx->getUserNonblock())
                {
                    return arg | O_NONBLOCK;
                }
                else
                {
                    return arg & ~O_NONBLOCK;
                }
            }
            break;
            case F_DUPFD:
            case F_DUPFD_CLOEXEC:
            case F_SETFD:
            case F_SETOWN:
            case F_SETSIG:
            case F_SETLEASE:
            case F_NOTIFY:
#ifdef F_SETPIPE_SZ
            case F_SETPIPE_SZ:
#endif
            {
                int arg = va_arg(va, int);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETFD:
            case F_GETOWN:
            case F_GETSIG:
            case F_GETLEASE:
#ifdef F_GETPIPE_SZ
            case F_GETPIPE_SZ:
#endif
            {
                va_end(va);
                return fcntl_f(fd, cmd);
            }
            break;
            case F_SETLK:
            case F_SETLKW:
            case F_GETLK:
            {
                struct flock *arg = va_arg(va, struct flock *);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
            case F_GETOWN_EX:
            case F_SETOWN_EX:
            {
                struct f_owner_exlock *arg = va_arg(va, struct f_owner_exlock *);
                va_end(va);
                return fcntl_f(fd, cmd, arg);
            }
            break;
            default:
                va_end(va);
                return fcntl_f(fd, cmd);
            }
        }

        int ioctl(int d, unsigned long int request, ...)
        {
            va_list va;
            va_start(va, request);
            void *arg = va_arg(va, void *);
            va_end(va);

            if (FIONBIO == request)
            {
                bool user_nonblock = !!*(int *)arg;
                FdCtx::ptr ctx = FdMgr::GetInstance()->get(d);
                if (!ctx || ctx->isClose() || !ctx->isSocket())
                {
                    return ioctl_f(d, request, arg);
                }
                ctx->setUserNonblock(user_nonblock);
            }
            return ioctl_f(d, request, arg);
        }

        int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen)
        {
            return getsockopt_f(sockfd, level, optname, optval, optlen);
        }

        int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
        {
            if (!t_hook_enable)
            {
                return setsockopt_f(sockfd, level, optname, optval, optlen);
            }
            if (level == SOL_SOCKET)
            {
                if (optname == SO_RCVTIMEO || optname == SO_SNDTIMEO)
                {
                    FdCtx::ptr ctx = FdMgr::GetInstance()->get(sockfd);
                    if (ctx)
                    {
                        const timeval *v = (const timeval *)optval;
                        ctx->setTimeout(optname, v->tv_sec * 1000 + v->tv_usec / 1000);
                    }
                }
            }
            return setsockopt_f(sockfd, level, optname, optval, optlen);
        }
    }

}