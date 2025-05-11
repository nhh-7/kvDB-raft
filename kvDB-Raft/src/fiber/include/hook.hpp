#pragma once

/**
 * 作用是将系统调用（如 sleep、read、write、connect 等）“劫持”到自定义的协程调度器上，以便协程在遇到阻塞操作时不阻塞整个线程，而是挂起当前协程并调度其他协程继续执行。
 *  目标： 协程 Hook 系统调用：替换系统的阻塞调用，使协程遇到 I/O 或 sleep 等阻塞操作时不会真的阻塞线程，而是挂起当前协程，等条件就绪再恢复。
 */

#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
// #include "hook.hpp" // ?

namespace monsoon
{
    // 开启 Hook 后，像 sleep() 就不调用系统的，而是调用自己实现的非阻塞版本。
    // 当前线程是否hook
    bool is_hook_enable();
    // 设置当前线程hook
    void set_hook_enable(bool flag);
}

extern "C" // 用 C 的方式来编译和链接这些声明的函数。
{
    // sleep
    typedef unsigned int (*sleep_fun)(unsigned int seconds); // typedef一个函数指针
    extern sleep_fun sleep_f;                                // extern 表示这个变量 不是在这里定义的，而是在 .cpp 文件中定义的，这里只是告诉编译器它的存在，以便链接时使用。
    /*
        sleep_f = dlsym(RTLD_NEXT, "sleep");
        表示你可以通过 sleep_f() 调用真正的系统 sleep()，而不是你重定义的 sleep()。这是 Hook 的关键。
    */

    typedef int (*usleep_fun)(useconds_t usec);
    extern usleep_fun usleep_f;

    typedef int (*nanosleep_fun)(const struct timespec *req, struct timespec *rem);
    extern nanosleep_fun nanosleep_f;

    // socket
    typedef int (*socket_fun)(int domain, int type, int protocol);
    extern socket_fun socket_f;

    typedef int (*connect_fun)(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
    extern connect_fun connect_f;

    typedef int (*accept_fun)(int s, struct sockaddr *addr, socklen_t *addrlen);
    extern accept_fun accept_f;

    // read
    /*  例如
        ssize_t read(int fd, void *buf, size_t count) {
            if (!is_hook_enable()) {
                return read_f(fd, buf, count); // 调用原系统函数
            }
        // 否则走协程调度逻辑，比如epoll协助非阻塞读，挂起当前协程
        }
    */
    typedef ssize_t (*read_fun)(int fd, void *buf, size_t count);
    extern read_fun read_f;

    typedef ssize_t (*readv_fun)(int fd, const struct iovec *iov, int iovcnt);
    extern readv_fun readv_f;

    typedef ssize_t (*recv_fun)(int sockfd, void *buf, size_t len, int flags);
    extern recv_fun recv_f;

    typedef ssize_t (*recvfrom_fun)(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr,
                                    socklen_t *addrlen);
    extern recvfrom_fun recvfrom_f;

    typedef ssize_t (*recvmsg_fun)(int sockfd, struct msghdr *msg, int flags);
    extern recvmsg_fun recvmsg_f;

    // write
    typedef ssize_t (*write_fun)(int fd, const void *buf, size_t count);
    extern write_fun write_f;

    typedef ssize_t (*writev_fun)(int fd, const struct iovec *iov, int iovcnt);
    extern writev_fun writev_f;

    typedef ssize_t (*send_fun)(int s, const void *msg, size_t len, int flags);
    extern send_fun send_f;

    typedef ssize_t (*sendto_fun)(int s, const void *msg, size_t len, int flags, const struct sockaddr *to,
                                  socklen_t tolen);
    extern sendto_fun sendto_f;

    typedef ssize_t (*sendmsg_fun)(int s, const struct msghdr *msg, int flags);
    extern sendmsg_fun sendmsg_f;

    typedef int (*close_fun)(int fd);
    extern close_fun close_f;

    //
    typedef int (*fcntl_fun)(int fd, int cmd, ... /* arg */);
    extern fcntl_fun fcntl_f;

    typedef int (*ioctl_fun)(int d, unsigned long int request, ...);
    extern ioctl_fun ioctl_f;

    typedef int (*getsockopt_fun)(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
    extern getsockopt_fun getsockopt_f;

    typedef int (*setsockopt_fun)(int sockfd, int level, int optname, const void *optval, socklen_t optlen);
    extern setsockopt_fun setsockopt_f;

    extern int connect_with_timeout(int fd, const struct sockaddr *addr, socklen_t addrlen, uint64_t timeout_ms);
}