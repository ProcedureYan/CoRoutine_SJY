#include <unistd.h>    
#include <sys/epoll.h> 
#include <fcntl.h>     
#include <cstring>

#include "ioscheduler.h"

static bool debug = true;

namespace sylar {

// 获取当前线程的 IOManager 实例
IOManager* IOManager::GetThis() 
{
    // 获取当前线程的 IOManager 对象
    return dynamic_cast<IOManager*>(Scheduler::GetThis());
}

// 获取指定事件的上下文（事件上下文包括调度器、回调函数和 fiber）
IOManager::FdContext::EventContext& IOManager::FdContext::getEventContext(Event event) 
{
    assert(event==READ || event==WRITE);    
    switch (event) 
    {
    case READ:
        return read; // 返回读事件的上下文
    case WRITE:
        return write; // 返回写事件的上下文
    }
    throw std::invalid_argument("Unsupported event type");
}

// 重置事件上下文，清空调度器、fiber 和回调函数
void IOManager::FdContext::resetEventContext(EventContext &ctx) 
{
    ctx.scheduler = nullptr;
    ctx.fiber.reset();
    ctx.cb = nullptr;
}

// 触发事件，标记事件已经触发并执行相应的回调fiber
// 不加锁，因为该函数调用时已经在上层函数加过锁
void IOManager::FdContext::triggerEvent(IOManager::Event event) {
    assert(events & event);

    events = (Event)(events & ~event); // 从事件中移除已触发的事件
    
    EventContext& ctx = getEventContext(event); // 获取事件的上下文
    if (ctx.cb) // 调度回调函数
    {
        ctx.scheduler->scheduleLock(&ctx.cb); 
    } 
    else 
    {
        ctx.scheduler->scheduleLock(&ctx.fiber);
    }

    resetEventContext(ctx); // 重置事件上下文
    return;
}

IOManager::IOManager(size_t threads, bool use_caller, const std::string &name): 
Scheduler(threads, use_caller, name), TimerManager()
{

    m_epfd = epoll_create(5000); // 创建 epoll 实例，最多可以管理 5000 个文件描述符
    assert(m_epfd > 0);

    int rt = pipe(m_tickleFds); // 创建用于唤醒 epoll 等待的管道
    assert(!rt);

    epoll_event event;
    event.events  = EPOLLIN | EPOLLET;  // 设置 epoll 事件为可读和边缘触发
    event.data.fd = m_tickleFds[0]; // 设置文件描述符为管道的读取端

    rt = fcntl(m_tickleFds[0], F_SETFL, O_NONBLOCK); // 设置管道的读取端为非阻塞
    assert(!rt);

    rt = epoll_ctl(m_epfd, EPOLL_CTL_ADD, m_tickleFds[0], &event); // 将管道加入到 epoll 中
    assert(!rt);

    contextResize(32); // 初始化文件描述符上下文的大小

    start();
}

IOManager::~IOManager() {
    stop();
    close(m_epfd);
    close(m_tickleFds[0]);
    close(m_tickleFds[1]);

    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]) 
        {
            delete m_fdContexts[i];
        }
    }
}

// 调整文件描述符上下文的大小
void IOManager::contextResize(size_t size) 
{
    m_fdContexts.resize(size);

    // 为每个新的文件描述符分配 FdContext
    for (size_t i = 0; i < m_fdContexts.size(); ++i) 
    {
        if (m_fdContexts[i]==nullptr) 
        {
            m_fdContexts[i] = new FdContext();
            m_fdContexts[i]->fd = i; // 设置文件描述符索引
        }
    }
}

// 向 epoll 中添加事件（读/写事件），并绑定回调函数
int IOManager::addEvent(int fd, Event event, std::function<void()> cb) 
{
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        std::unique_lock<std::shared_mutex> write_lock(m_mutex);
        contextResize(fd * 1.5);
        fd_ctx = m_fdContexts[fd];
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    if(fd_ctx->events & event) 
    {
        return -1;
    }
    // 判断当前文件描述符（fd）是否已经在 epoll 中注册过事件
    // EPOLL_CTL_MOD（修改事件）,EPOLL_CTL_ADD（添加事件）
    int op = fd_ctx->events ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    epoll_event epevent;
    // 设置 epoll 事件
    // EPOLLET：边缘触发模式。EPOLLET 使得 epoll 在事件发生时只通知一次，不会再次通知
    epevent.events   = EPOLLET | fd_ctx->events | event; 
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent); // 将事件添加到 epoll 中
    if (rt) 
    {
        std::cerr << "addEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    ++m_pendingEventCount; // 增加挂起事件计数

    fd_ctx->events = (Event)(fd_ctx->events | event); // 更新文件描述符的事件

    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event); // 获取事件上下文
    assert(!event_ctx.scheduler && !event_ctx.fiber && !event_ctx.cb);
    event_ctx.scheduler = Scheduler::GetThis();  // 设置事件的调度器
    if (cb) 
    {
        event_ctx.cb.swap(cb);  // 如果有回调，设置回调函数
    } 
    else 
    {
        event_ctx.fiber = Fiber::GetThis(); // 如果没有回调，则绑定当前 Fiber
        assert(event_ctx.fiber->getState() == Fiber::RUNNING); // 确保 Fiber 处于运行状态
    }
    return 0;
}

// 删除指定文件描述符上的事件
bool IOManager::delEvent(int fd, Event event) { 
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];  // 获取文件描述符上下文
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;  // 文件描述符无效
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

     // 删除事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 判断是修改还是删除事件
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events; // 更新 epoll 事件
    epevent.data.ptr = fd_ctx;

    int rt = epoll_ctl(m_epfd, op, fd, &epevent); // 执行 epoll 控制
    if (rt) 
    {
        std::cerr << "delEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }


    --m_pendingEventCount; // 减少挂起事件计数

    fd_ctx->events = new_events; // 更新事件
    FdContext::EventContext& event_ctx = fd_ctx->getEventContext(event);
    fd_ctx->resetEventContext(event_ctx);  // 重置事件上下文
    return true;
}

// 取消指定文件描述符上的某个事件
bool IOManager::cancelEvent(int fd, Event event) {
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);
    if (!(fd_ctx->events & event)) 
    {
        return false;
    }

    // 移除事件
    Event new_events = (Event)(fd_ctx->events & ~event);
    int op           = new_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL; // 根据剩余事件决定是修改还是删除
    epoll_event epevent;
    epevent.events   = EPOLLET | new_events;  // 设置新的 epoll 事件
    epevent.data.ptr = fd_ctx;
    
    // 将事件更新到 epoll 中
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "cancelEvent::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1; // 如果 epoll_ctl 调用失败，返回失败
    }

    --m_pendingEventCount; // 事件计数减 1

    fd_ctx->triggerEvent(event); // 触发该事件
    return true;
}

// 取消指定文件描述符上的所有事件
bool IOManager::cancelAll(int fd) {
    FdContext *fd_ctx = nullptr;
    
    std::shared_lock<std::shared_mutex> read_lock(m_mutex);
    if ((int)m_fdContexts.size() > fd) 
    {
        fd_ctx = m_fdContexts[fd];
        read_lock.unlock();
    }
    else 
    {
        read_lock.unlock();
        return false;
    }

    std::lock_guard<std::mutex> lock(fd_ctx->mutex);

    if (!fd_ctx->events) 
    {
        return false;
    }

    // 删除文件描述符上的所有事件
    int op = EPOLL_CTL_DEL;
    epoll_event epevent;
    epevent.events   = 0;
    epevent.data.ptr = fd_ctx;
    
    // 从 epoll 中删除所有事件
    int rt = epoll_ctl(m_epfd, op, fd, &epevent);
    if (rt) 
    {
        std::cerr << "IOManager::epoll_ctl failed: " << strerror(errno) << std::endl; 
        return -1;
    }

    if (fd_ctx->events & READ) 
    {
        fd_ctx->triggerEvent(READ);
        --m_pendingEventCount;
    }

    if (fd_ctx->events & WRITE) 
    {
        fd_ctx->triggerEvent(WRITE);
        --m_pendingEventCount;
    }

    assert(fd_ctx->events == 0);
    return true;
}

// 唤醒阻塞的 IO 管理器线程
void IOManager::tickle() 
{

    if(!hasIdleThreads()) 
    {
        return;
    }
    // 向 tickleFds[1] 写入 "T"，用来唤醒阻塞的 IO 管理器线程
    int rt = write(m_tickleFds[1], "T", 1);
    assert(rt == 1);
}

// 判断 IO 管理器是否已经停止
bool IOManager::stopping() 
{
    uint64_t timeout = getNextTimer();

    return timeout == ~0ull && m_pendingEventCount == 0 && Scheduler::stopping();
}

// IO 管理器的空闲循环，执行事件处理
void IOManager::idle() 
{    
    static const uint64_t MAX_EVNETS = 256; // 最大事件数
    std::unique_ptr<epoll_event[]> events(new epoll_event[MAX_EVNETS]);

    while (true) 
    {
        if(debug) std::cout << "IOManager::idle(),run in thread: " << Thread::GetThreadId() << std::endl; 

        if(stopping()) 
        {
            if(debug) std::cout << "name = " << getName() << " idle exits in thread: " << Thread::GetThreadId() << std::endl;
            break;
        }

        int rt = 0;
        while(true)
        {
            static const uint64_t MAX_TIMEOUT = 5000; // 设置最大超时
            uint64_t next_timeout = getNextTimer();  // 获取下一个超时时间
            next_timeout = std::min(next_timeout, MAX_TIMEOUT); // 取最小值，避免等待过长时间

            // 调用 epoll_wait，阻塞等待事件
            rt = epoll_wait(m_epfd, events.get(), MAX_EVNETS, (int)next_timeout);

            if(rt < 0 && errno == EINTR) 
            {
                continue;
            } 
            else 
            {
                break;
            }
        };

        // 处理定时器到期的回调函数
        std::vector<std::function<void()>> cbs;
        listExpiredCb(cbs);// 获取到期的回调
        if(!cbs.empty()) 
        {
            for(const auto& cb : cbs) 
            {
                scheduleLock(cb);// 安全地调度回调
            }
            cbs.clear();
        }

        // 处理 epoll 中的事件
        for (int i = 0; i < rt; ++i) 
        {
            epoll_event& event = events[i];

            if (event.data.fd == m_tickleFds[0]) // 如果是唤醒事件
            {
                uint8_t dummy[256];
                while (read(m_tickleFds[0], dummy, sizeof(dummy)) > 0);
                continue;
            }

            FdContext *fd_ctx = (FdContext *)event.data.ptr;
            std::lock_guard<std::mutex> lock(fd_ctx->mutex);

            // 如果发生了错误事件，标记为可读或可写事件
            if (event.events & (EPOLLERR | EPOLLHUP)) 
            {
                event.events |= (EPOLLIN | EPOLLOUT) & fd_ctx->events;
            }

            int real_events = NONE; // 实际的事件类型
            if (event.events & EPOLLIN) 
            {
                real_events |= READ;
            }
            if (event.events & EPOLLOUT) 
            {
                real_events |= WRITE;
            }

            // 如果该事件没有被处理，则跳过
            if ((fd_ctx->events & real_events) == NONE) 
            {
                continue;
            }
            // 更新事件，准备从 epoll 中删除
            int left_events = (fd_ctx->events & ~real_events);
            int op          = left_events ? EPOLL_CTL_MOD : EPOLL_CTL_DEL;
            event.events    = EPOLLET | left_events;

            // 更新 epoll 中的事件
            int rt2 = epoll_ctl(m_epfd, op, fd_ctx->fd, &event);
            if (rt2) 
            {
                std::cerr << "idle::epoll_ctl failed: " << strerror(errno) << std::endl; 
                continue;
            }

            // 触发实际的事件回调
            if (real_events & READ) 
            {
                fd_ctx->triggerEvent(READ);
                --m_pendingEventCount;
            }
            if (real_events & WRITE) 
            {
                fd_ctx->triggerEvent(WRITE);
                --m_pendingEventCount;
            }
        }
        // 当前协程让出执行权
        Fiber::GetThis()->yield();
  
    }
}

void IOManager::onTimerInsertedAtFront() 
{
    tickle();
}

}