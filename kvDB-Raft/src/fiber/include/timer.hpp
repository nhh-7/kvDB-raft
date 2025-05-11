#pragma once

#include <memory>
#include <set>
#include <vector>
#include "mutex.hpp"

/**
 *  Timer：表示一个定时器对象；
    TimerManager：定时器管理器，用于管理所有定时器的生命周期和调度。
 */

namespace monsoon
{
    class TimerManager;

    class Timer : public std::enable_shared_from_this<Timer>
    {
        friend class TimerManager;

    public:
        typedef std::shared_ptr<Timer> ptr;

        // 取消定时器
        bool cancel();
        // 重新计算 next_ 时间
        bool refresh();
        // 重置定时器，重新设置定时器触发时间
        bool reset(uint64_t ms, bool from_now);

    private:
        // 为什么要私有, 保证只有TimerManager友元类可以构造他
        Timer(uint64_t ms, std::function<void()> cb, bool recuring, TimerManager *manager);
        Timer(uint64_t next);

        bool recurring_ = false;          // 是否是循环定时器
        uint64_t ms_ = 0;                 // 执行周期
        uint64_t next_ = 0;               // 精确的执行时间
        std::function<void()> cb_;        // 回调函数
        TimerManager *manager_ = nullptr; // 管理器

    private:
        struct Comparator
        {
            bool operator()(const Timer::ptr &lhs, const Timer::ptr &rhs) const;
        };
    };

    class TimerManager
    {
        friend class Timer;

    public:
        TimerManager();
        virtual ~TimerManager();
        Timer::ptr addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);
        Timer::ptr addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

        // 到最近一个定时器的时间间隔（ms） 还有多久触发最近的定时器
        uint64_t getNextTimer();

        // 获取需要执行的定时器的回调函数列表
        void listExpiredCb(std::vector<std::function<void()>> &cbs);
        // 是否有定时器
        bool hasTimer();

    protected:
        // 当一个定时器被插入到了最前面时，就会调用这个函数 当新的定时器插入到Timer集合的首部时，TimerManager通过该方法来通知IOManager立刻更新当前的epoll_wait超时。
        virtual void OnTimerInsertedAtFront() = 0;
        // 将定时器添加到管理器
        void addTimer(Timer::ptr val, RWMutex::WriteLock &lock);

    private:
        // 检测服务器时间是否被调后了
        bool detectClockRollover(uint64_t now_ms);

        RWMutex mutex_;
        // 定时器集合
        std::set<Timer::ptr, Timer::Comparator> timers_;
        // 是否触发过OnTimerInsertedAtFront
        bool tickled_ = false;
        // 上次执行时间
        uint64_t previouseTime_ = 0;
    };
}