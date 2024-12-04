#ifndef __SYLAR_IOMANAGER_H__
#define __SYLAR_IOMANAGER_H__

#include "scheduler.h"
#include "timer.h"

namespace sylar {

class IOManager : public Scheduler, public TimerManager 
{
public:
    enum Event 
    {
        NONE = 0x0,
        READ = 0x1,
        WRITE = 0x4
    };

private:
    struct FdContext 
    {
        struct EventContext
        {
            Scheduler *scheduler = nullptr;
            std::shared_ptr<Fiber> fiber;
            std::function<void()> cb;
        };

        EventContext read; 
        EventContext write;
        int fd = 0;
        Event events = NONE;
        std::mutex mutex;

        EventContext& getEventContext(Event event);
        void resetEventContext(EventContext &ctx);
        void triggerEvent(Event event);        
    };

public:
    IOManager(size_t threads = 1, bool use_caller = true, const std::string &name = "IOManager");
    ~IOManager();

    int addEvent(int fd, Event event, std::function<void()> cb = nullptr);
    bool delEvent(int fd, Event event);
    bool cancelEvent(int fd, Event event);
    bool cancelAll(int fd);

    static IOManager* GetThis();

protected:
    void tickle() override;
    
    bool stopping() override;
    
    void idle() override;

    void onTimerInsertedAtFront() override;

    void contextResize(size_t size);

private:
    int m_epfd = 0;
    int m_tickleFds[2];
    std::atomic<size_t> m_pendingEventCount = {0};
    std::shared_mutex m_mutex;
    std::vector<FdContext *> m_fdContexts;
};

} 

#endif