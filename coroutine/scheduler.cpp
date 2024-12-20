#include "scheduler.h"

static bool debug = false;

namespace sylar {

static thread_local Scheduler* t_scheduler = nullptr;

Scheduler* Scheduler::GetThis()
{
	return t_scheduler;
}

void Scheduler::SetThis()
{
	t_scheduler = this;
}

void Scheduler::SetStrategy(const std::string& strategy){
	Strategy pre_strategy = m_strategy;
	if(strategy == "FCFS"){
		m_strategy = FCFS;
		if(pre_strategy == PS){
			while (!m_tasks_ps.empty()) {
				m_tasks.push_back(m_tasks_ps.top());  // 获取优先队列的堆顶元素
				m_tasks_ps.pop();  // 移除堆顶元素
			}
		}
	}else if(strategy == "RR"){
		m_strategy = RR;

	}else if(strategy == "PS"){
		m_strategy = PS;
		// if(pre_strategy == FCFS){
		// 	for (auto& task : m_tasks) {
		// 		m_tasks_ps.push(task);
		// 	}
		// 	// 清空 m_tasks
		// 	m_tasks.clear();
		// }
	}else{
		std::cout << "Please enter the specified options: FCFS, RR, PS" << std::endl;
	}
}

void Scheduler::fcfs_push(bool& need_tickle, Scheduler::ScheduleTask task) {
	std::cout << "fcfs" << std::endl;
	std::lock_guard<std::mutex> lock(m_mutex);
	// 如果当前任务队列为空，调度器可能是休眠状态
	need_tickle = m_tasks.empty();
	if (task.fiber || task.cb) 
	{
		m_tasks.push_back(task);
	}
}

void Scheduler::ps_push(bool& need_tickle, Scheduler::ScheduleTask task) {
	std::cout << "ps" << std::endl;
	std::lock_guard<std::mutex> lock(m_mutex);
	// 如果当前任务队列为空，调度器可能是休眠状态
	need_tickle = m_tasks_ps.empty();
	if (task.fiber || task.cb) 
	{
		m_tasks_ps.push(task);
	}
}

Scheduler::Scheduler(size_t threads, bool use_caller, const std::string &name,const std::string& strategy):
m_useCaller(use_caller), m_name(name)
{
	assert(threads > 0 && Scheduler::GetThis() == nullptr);

	SetThis();

	Thread::SetName(m_name);
	SetStrategy(strategy);
	strategyInsert[FCFS] = [this](bool& need_tickle, ScheduleTask task) { fcfs_push(need_tickle, task); };
	// strategyInsert[RR] = [this](bool& need_tickle, ScheduleTask task) { rr_push(need_tickle, task); };
	strategyInsert[PS] = [this](bool& need_tickle, ScheduleTask task) { ps_push(need_tickle, task); };

	strategyConsume[FCFS] = [this](bool& tickle_me, ScheduleTask& task,int thread_id) { fcfs_consume(tickle_me, task, thread_id); };
	strategyConsume[PS] = [this](bool& tickle_me, ScheduleTask& task,int thread_id) { ps_consume(tickle_me, task, thread_id); };

	// 是否使用主线程当作工作线程
	if(use_caller)
	{
		// 主线程已经存在，之后可以少开一个线程
		threads --;

		// 创建主协程
		Fiber::GetThis();

		// 创建调度协程（让调度协程作为运行Scheduler::run的载体）
		m_schedulerFiber.reset(new Fiber(std::bind(&Scheduler::run, this), 0, false)); // false -> 该调度协程退出后将返回主协程
		Fiber::SetSchedulerFiber(m_schedulerFiber.get());
		
		m_rootThread = Thread::GetThreadId();
		m_threadIds.push_back(m_rootThread);
	}

	m_threadCount = threads;
	if(debug) std::cout << "Scheduler::Scheduler() success\n";
}

Scheduler::~Scheduler()
{
	assert(stopping()==true);
	if (GetThis() == this) 
	{
        t_scheduler = nullptr;
    }
    if(debug) std::cout << "Scheduler::~Scheduler() success\n";
}

void Scheduler::start()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	if(m_stopping)
	{
		std::cerr << "Scheduler is stopped" << std::endl;
		return;
	}

	assert(m_threads.empty());
	m_threads.resize(m_threadCount);
	for(size_t i = 0;i < m_threadCount;i++)
	{
		// 直接用线程执行调度器run函数，不需要额外设置协程
		m_threads[i].reset(new Thread(std::bind(&Scheduler::run, this), m_name + "_" + std::to_string(i)));
		m_threadIds.push_back(m_threads[i]->getId());
	}
	if(debug) std::cout << "Scheduler::start() success\n";
}

void Scheduler::fcfs_consume(bool& tickle_me, ScheduleTask& task, int thread_id){
	std::lock_guard<std::mutex> lock(m_mutex);
	auto it = m_tasks.begin();
	// 1 遍历任务队列
	while(it!=m_tasks.end())
	{
		if(it->thread != -1 && it->thread != thread_id)
		{
			it++;
			tickle_me = true;
			continue;
		}

		// 2 取出任务
		assert(it->fiber||it->cb);
		std::cout << "FCFS 执行" << std::endl;
		task = *it;
		m_tasks.erase(it); 
		m_activeThreadCount++;
		break;
	}	
	tickle_me = tickle_me || (it != m_tasks.end());
}

void Scheduler::ps_consume(bool& tickle_me, ScheduleTask& task, int thread_id){
	std::lock_guard<std::mutex> lock(m_mutex);

	// 1 遍历任务队列
	while(!m_tasks_ps.empty())
	{
		ScheduleTask top_task = m_tasks_ps.top();  // 获取优先队列顶部的任务

        // 检查任务的线程ID是否匹配
        if (top_task.thread != -1 && top_task.thread != thread_id) {
            tickle_me = true;  // 如果线程ID不匹配，设置tickle_me为true
			continue;
        } else {
            // 2. 任务符合条件，可以执行
            assert(top_task.fiber||top_task.cb);  // 确保任务是有效的
            std::cout << "Executing task with priority " << top_task.t_nice << std::endl;
            task = top_task;

            // 移除任务
            m_tasks_ps.pop();
            m_activeThreadCount++;
			break;
        }
	}
	tickle_me = tickle_me || (!m_tasks_ps.empty());
}

void Scheduler::run()
{
	int thread_id = Thread::GetThreadId();
	if(debug) std::cout << "Schedule::run() starts in thread: " << thread_id << std::endl;

	SetThis();

	// 运行在新创建的线程 -> 需要创建主协程
	if(thread_id != m_rootThread)
	{
		Fiber::GetThis();
	}

	std::shared_ptr<Fiber> idle_fiber = std::make_shared<Fiber>(std::bind(&Scheduler::idle, this));
	ScheduleTask task;
	
	while(true)
	{
		task.reset();
		bool tickle_me = false;

		strategyConsume[m_strategy](tickle_me, task, thread_id);

		if(tickle_me)
		{
			tickle();
		}

		// 3 执行任务
		if(task.fiber)
		{
			{					
				std::lock_guard<std::mutex> lock(task.fiber->m_mutex);
				if(task.fiber->getState()!=Fiber::TERM)
				{
					task.fiber->resume();	
				}
			}
			m_activeThreadCount--;
			task.reset();
		}
		else if(task.cb)
		{
			std::shared_ptr<Fiber> cb_fiber = std::make_shared<Fiber>(task.cb);
			{
				std::lock_guard<std::mutex> lock(cb_fiber->m_mutex);
				cb_fiber->resume();			
			}
			m_activeThreadCount--;
			task.reset();	
		}
		// 4 无任务 -> 执行空闲协程
		else
		{		
			// 系统关闭 -> idle协程将从死循环跳出并结束 -> 此时的idle协程状态为TERM -> 再次进入将跳出循环并退出run()
            if (idle_fiber->getState() == Fiber::TERM) 
            {
            	if(debug) std::cout << "Schedule::run() ends in thread: " << thread_id << std::endl;
                break;
            }
			m_idleThreadCount++;
			idle_fiber->resume();				
			m_idleThreadCount--;
		}
	}
	
}

void Scheduler::stop()
{
	if(debug) std::cout << "Schedule::stop() starts in thread: " << Thread::GetThreadId() << std::endl;
	
	if(stopping())
	{
		return;
	}

	m_stopping = true;	

    if (m_useCaller) 
    {
        assert(GetThis() == this);
    } 
    else 
    {
        assert(GetThis() != this);
    }
	
	for (size_t i = 0; i < m_threadCount; i++) 
	{
		tickle();
	}

	if (m_schedulerFiber) 
	{
		tickle();
	}

	if(m_schedulerFiber)
	{
		m_schedulerFiber->resume();
		if(debug) std::cout << "m_schedulerFiber ends in thread:" << Thread::GetThreadId() << std::endl;
	}

	std::vector<std::shared_ptr<Thread>> thrs;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		thrs.swap(m_threads);
	}

	for(auto &i : thrs)
	{
		i->join();
	}
	if(debug) std::cout << "Schedule::stop() ends in thread:" << Thread::GetThreadId() << std::endl;
}

void Scheduler::tickle()
{
	std::cout << "tickle执行" << std::endl;
}

// 休眠 1s 以释放CPU资源，然后执行yield让调度协程查看有没有新的任务
// 如果没有就重新执行idle协程，直到有新的任务
void Scheduler::idle()
{
	while(!stopping())
	{
		if(debug) std::cout << "Scheduler::idle(), sleeping in thread: " << Thread::GetThreadId() << std::endl;	
		sleep(1);	
		Fiber::GetThis()->yield();
	}
}

bool Scheduler::stopping() 
{
    std::lock_guard<std::mutex> lock(m_mutex);
	if(m_strategy == FCFS)
    	return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
	else if(m_strategy == PS)
		return m_stopping && m_tasks_ps.empty() && m_activeThreadCount == 0;
	return m_stopping && m_tasks.empty() && m_activeThreadCount == 0;
}


}