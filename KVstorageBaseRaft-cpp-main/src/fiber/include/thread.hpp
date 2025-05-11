#ifndef __SYLAR_THREAD_H_
#define __SYLAR_THREAD_H_

/**
 * 定义了一个自封装的 线程类 Thread，用于替代 std::thread 或 pthread，实现更灵活的线程创建与管理，并增加了线程命名和线程 ID 等功能，广泛用于协程调度器 Scheduler 中。
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
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
    Thread(std::function<void()> cb, const std::string &name);
    ~Thread();
    pid_t getId() const { return id_; }
    const std::string &getName() const { return name_; }
    void join();
    static Thread *GetThis();
    static const std::string &GetName();
    static void SetName(const std::string &name);

  private:
    Thread(const Thread &) = delete;
    Thread(const Thread &&) = delete;
    Thread operator=(const Thread &) = delete;

    static void *run(void *args);

  private:
    pid_t id_;
    pthread_t thread_;
    std::function<void()> cb_;
    std::string name_;
  };
} // namespace monsoon

#endif