#ifndef __SYLAR_TIMER_H__
#define __SYLAR_TIMER_H__

#include <memory>
#include <vector>
#include <set>
#include <shared_mutex>
#include <assert.h>
#include <functional>
#include <mutex>
// #include <queue>

namespace sylar {

class TimerManager;

class Timer : public std::enable_shared_from_this<Timer> 
{
    friend class TimerManager;
public:
    // 从时间堆中删除timer
    bool cancel();
    // 刷新timer
    bool refresh();
    // 重设timer的超时时间
    bool reset(uint64_t ms, bool from_now);

private:
    Timer(uint64_t ms, std::function<void()> cb, bool recurring, TimerManager* manager);
 
private:
    // 是否循环
    bool m_recurring = false;
    // 超时时间
    uint64_t m_ms = 0;
    // 绝对超时时间
    std::chrono::time_point<std::chrono::system_clock> m_next;
    // 超时时触发的回调函数
    std::function<void()> m_cb;
    // 管理此timer的管理器 
    TimerManager* m_manager = nullptr;

private:
    // 用于小根堆的比较函数
    struct Comparator 
    {
        bool operator()(const std::shared_ptr<Timer>& lhs, const std::shared_ptr<Timer>& rhs) const;
    };
};

class TimerManager 
{
    friend class Timer;
public:
    TimerManager();
    virtual ~TimerManager();

    // 添加timer
    std::shared_ptr<Timer> addTimer(uint64_t ms, std::function<void()> cb, bool recurring = false);

    // 添加条件timer
    std::shared_ptr<Timer> addConditionTimer(uint64_t ms, std::function<void()> cb, std::weak_ptr<void> weak_cond, bool recurring = false);

    uint64_t getNextTimer();

    void listExpiredCb(std::vector<std::function<void()>>& cbs);

    bool hasTimer();

protected:
    virtual void onTimerInsertedAtFront() {};

    void addTimer(std::shared_ptr<Timer> timer);

private:
    bool detectClockRollover();

private:
    std::shared_mutex m_mutex;
    // 时间堆, 改用set建堆
    // std::priority_queue<std::shared_ptr<Timer>, std::vector<std::shared_ptr<Timer>>, Timer::Comparator> m_timers;
    std::set<std::shared_ptr<Timer>, Timer::Comparator> m_timers;
    
    bool m_tickled = false;
    // 上次检查系统时间是否回退的绝对时间
    std::chrono::time_point<std::chrono::system_clock> m_previouseTime;
};

}

#endif