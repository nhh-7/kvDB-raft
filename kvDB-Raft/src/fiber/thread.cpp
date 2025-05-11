#include "./include/thread.hpp"
#include "./include/utils.hpp"

namespace monsoon
{
    // 指向当前线程
    static thread_local Thread *cur_thread = nullptr;
    static thread_local std::string cur_thread_name = "UNKNOW";

    Thread::Thread(std::function<void()> cb, const std::string &name = "UNKNOW")
        : cb_(cb), name_(name)
    {
        if (name.empty())
        {
            name_ = "UNKNOW";
        }

        int rt = pthread_create(&thread_, nullptr, &Thread::run, this);
        // 成功返回0
        if (rt)
        {
            std::cout << "pthread_create error, name:" << name_ << std::endl;
            throw std::logic_error("pthread_create");
        }
    }

    void *Thread::run(void *args)
    {
        Thread *thread = (Thread *)args;
        cur_thread = thread;
        cur_thread_name = thread->name_;
        thread->id_ = monsoon::GetThreadId(); // 获取系统线程 ID

        pthread_setname_np(pthread_self(), thread->name_.substr(0, 15).c_str());
        std::function<void()> cb;
        cb.swap(thread->cb_);

        cb();
        return 0;
    }

    Thread::~Thread()
    {
        if (thread_)
            pthread_detach(thread_);
    }

    void Thread::join()
    {
        if (thread_)
        {
            int rt = pthread_join(thread_, nullptr); // 以阻塞的方式等待thread指定的线程结束
            if (rt)
            {
                std::cout << "pthread_join error,name:" << name_ << std::endl;
                throw std::logic_error("pthread_join");
            }
            thread_ = 0;
        }
    }

    Thread *Thread::GetThis()
    {
        return cur_thread;
    }

    const std::string &Thread::GetName()
    {
        return cur_thread_name;
    }

    void Thread::SetName(const std::string &name)
    {
        if (name.empty())
            return;
        
        if (cur_thread)
        {
            cur_thread->name_ = name;
        }
        cur_thread_name = name;
    }
}