#pragma once

/**
 * 实现了一个线程同步工具集合，位于命名空间 monsoon 中。它是一个轻量的“锁机制库”，封装了：
    POSIX 互斥锁（pthread_mutex_t）
    读写锁（pthread_rwlock_t）
    信号量（sem_t）
    作用域锁模板类（类似于 std::lock_guard）
 */

#include <pthread.h>
#include <semaphore.h>
#include <stdint.h>
#include <atomic>
#include <functional>
#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <thread>

#include "noncopyable.hpp"
#include "utils.hpp"

namespace monsoon
{
    // 信号量
    class Semaphore : Nonecopyable
    {
    public:
        Semaphore(uint32_t count = 0); // 初始化信号量值
        ~Semaphore();

        void wait();
        void notify();

    private:
        sem_t semaphore_;
    };

    // 局部锁类模板 // 构造时加锁 析构时自动解锁 类似于std::lock_guard
    // 支持手动 lock() 和 unlock()
    template <typename T>
    struct ScopedLockImpl
    {
    public:
        ScopedLockImpl(T &mutex) : m_(mutex)
        {
            m_.lock();
            isLocked_ = true;
        }

        void lock()
        {
            if (!isLocked_)
            {
                // std::cout << "lock" << std::endl;
                isLocked_ = true;
                m_.lock();
            }
        }
        void unlock()
        {
            if (isLocked_)
            {
                // std::cout << "unlock" << std::endl;
                m_.unlock();
                isLocked_ = false;
            }
        }
        ~ScopedLockImpl()
        {
            unlock();
        }

    private:
        T &m_;          // mutex
        bool isLocked_; // 是否已经上锁
    };

    // 读锁（多个线程可同时获取，不会写）
    template <typename T>
    struct ReadScopedLockImpl
    {
    public:
        ReadScopedLockImpl(T &mutex) : mutex_(mutex)
        {
            mutex_.rdlock();
            isLocked_ = true;
        }
        ~ReadScopedLockImpl()
        {
            unlock();
        }
        void lock()
        {
            if (!isLocked_)
            {
                mutex_.rdlock();
                isLocked_ = true;
            }
        }
        void unlock()
        {
            if (isLocked_)
            {
                mutex_.unlock();
                isLocked_ = false;
            }
        }

    private:
        T &mutex_;
        bool isLocked_; // 是否已上锁
    };

    // 写锁（排他）
    template <class T>
    struct WriteScopedLockImpl
    {
    public:
        WriteScopedLockImpl(T &mutex) : mutex_(mutex)
        {
            mutex_.wrlock();
            isLocked_ = true;
        }

        ~WriteScopedLockImpl() { unlock(); }
        void lock()
        {
            if (!isLocked_)
            {
                mutex_.wrlock();
                isLocked_ = true;
            }
        }
        void unlock()
        {
            if (isLocked_)
            {
                mutex_.unlock();
                isLocked_ = false;
            }
        }

    private:
        /// Mutex
        T &mutex_;
        /// 是否已上锁
        bool isLocked_;
    };

    // 对pthread_mutex_t 的封装。 用于互斥访问共享资源
    class Mutex : Nonecopyable
    {
    public:
        // 方便用户在使用 Mutex 的时候写
        typedef ScopedLockImpl<Mutex> Lock;
        Mutex()
        {
            CondPanic(0 == pthread_mutex_init(&m_, nullptr), "lock init error"); // 如果m_初始化失败，打印调用栈并终止程序
        }

        void lock()
        {
            CondPanic(0 == pthread_mutex_lock(&m_), "lock error");
        }

        void unlock()
        {
            CondPanic(0 == pthread_mutex_unlock(&m_), "unlock error");
        }

        ~Mutex()
        {
            CondPanic(0 == pthread_mutex_destroy(&m_), "destroy lock error");
        }

    private:
        pthread_mutex_t m_;
    };

    // 封装pthread_rwlock_t类型，支持多个线程同时读取，但有线程要写入时，需要独占锁
    class RWMutex : Nonecopyable
    {
    public:
        // 局部读锁
        typedef ReadScopedLockImpl<RWMutex> ReadLock;
        // 局部写锁
        typedef WriteScopedLockImpl<RWMutex> WriteLock;
        /* 用例
            // 读线程
            void reader() {
            monsoon::RWMutex::ReadLock lock(rwlock);  // 自动加读锁 rwlock是一个RWMutex对象
            // ... 执行只读操作 ...
            }

            // 写线程
            void writer() {
            monsoon::RWMutex::WriteLock lock(rwlock);  // 自动加写锁
            // ... 执行修改操作 ...
            }
        */

        RWMutex()
        {
            pthread_rwlock_init(&m_, nullptr);
        }
        ~RWMutex()
        {
            pthread_rwlock_destroy(&m_);
        }

        // 如果已经有其他读线程持有锁，它会一起共享, 但如果有线程持有写锁，它会阻塞
        void rdlock()
        {
            pthread_rwlock_rdlock(&m_);
        }

        // 会等待所有读锁释放，并且在它持有锁时，其他读/写线程都会阻塞
        void wrlock()
        {
            pthread_rwlock_wrlock(&m_);
        }

        void unlock()
        {
            pthread_rwlock_unlock(&m_);
        }

    private:
        pthread_rwlock_t m_;
    };
}