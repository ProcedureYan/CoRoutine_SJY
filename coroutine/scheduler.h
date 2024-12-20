#ifndef _SCHEDULER_H_
#define _SCHEDULER_H_

#include "fiber.h"
#include "thread.h"

#include <mutex>
#include <vector>
#include <functional>
#include <queue>

namespace sylar {

class Scheduler
{
public:
	// 调度策略
	enum Strategy
	{
		FCFS, 
		RR, 
		PS 
	};


	Scheduler(size_t threads = 1, bool use_caller = true, const std::string& name="Scheduler", const std::string& strategy="FCFS");
	virtual ~Scheduler();
	
	const std::string& getName() const {return m_name;}

public:	
	// 获取正在运行的调度器
	static Scheduler* GetThis();

protected:
	// 设置正在运行的调度器
	void SetThis();

public:	
	// 添加任务到任务队列
    template <class FiberOrCb>
    void scheduleLock(FiberOrCb fc, int nice = 19, int thread = -1) 
    {
    	bool need_tickle = false;
		ScheduleTask task(fc, thread, nice);

    	strategyInsert[m_strategy](need_tickle, task);

    	if(need_tickle)
    	{
    		tickle();
    	}
    }
	
	// 启动线程池
	virtual void start();
	// 关闭线程池
	virtual void stop();	

public:
	void SetStrategy(const std::string& strategy);

protected:
	virtual void tickle();
	
	// 线程函数
	virtual void run();

	// 用作空闲协程的函数
	virtual void idle();
	
	// 是否可以关闭
	virtual bool stopping();

	bool hasIdleThreads() {return m_idleThreadCount>0;}

private:
	// 任务
	struct ScheduleTask
	{
		std::shared_ptr<Fiber> fiber;
		std::function<void()> cb;
		int thread; // 指定任务需要运行的线程id

		int t_nice; // 协程优先级，数字越小优先级越高，范围：[-20, 19]

		ScheduleTask()
		{
			fiber = nullptr;
			cb = nullptr;
			thread = -1;
			t_nice = 19;
		}

		ScheduleTask(std::shared_ptr<Fiber> f, int thr,int nice = 19):
		t_nice(nice)
		{
			fiber = f;
			thread = thr;
		}

		ScheduleTask(std::shared_ptr<Fiber>* f, int thr,int nice = 19):
		t_nice(nice)
		{
			fiber.swap(*f);
			thread = thr;
		}	

		ScheduleTask(std::function<void()> f, int thr,int nice = 19):
		t_nice(nice)
		{
			cb = f;
			thread = thr;
		}		

		ScheduleTask(std::function<void()>* f, int thr,int nice = 19):
		t_nice(nice)
		{
			cb.swap(*f);
			thread = thr;
		}

		void reset()
		{
			fiber = nullptr;
			cb = nullptr;
			thread = -1;
			t_nice = 19;
		}
	};

	// 定义不同的调度策略处理函数
	void fcfs_push(bool& need_tickle, ScheduleTask task);

	void ps_push(bool& need_tickle, ScheduleTask task);
	
	void fcfs_consume(bool& need_tickle, ScheduleTask& task, int thread_id);

	void ps_consume(bool& need_tickle, ScheduleTask& task, int thread_id);

	std::function<void(bool&, ScheduleTask)> strategyInsert[3];

	std::function<void(bool&, ScheduleTask&, int)> strategyConsume[3];


private:
	std::string m_name;
	// 互斥锁 -> 保护任务队列
	std::mutex m_mutex;
	// 线程池
	std::vector<std::shared_ptr<Thread>> m_threads;

	// 任务队列1，用于默认任务队列（FCFS）
	std::vector<ScheduleTask> m_tasks;

    // 任务队列2：声明一个自定义类型的优先队列,用于优先级调度PS
	struct compare {
		bool operator()(ScheduleTask a, ScheduleTask b) {
			return a.t_nice > b.t_nice; // 这里定义了小根堆
		}
	};
	std::priority_queue<ScheduleTask, std::vector<ScheduleTask>, compare> m_tasks_ps;



	// 存储工作线程的线程id
	std::vector<int> m_threadIds;
	// 需要额外创建的线程数
	size_t m_threadCount = 0;
	// 活跃线程数
	std::atomic<size_t> m_activeThreadCount = {0};
	// 空闲线程数
	std::atomic<size_t> m_idleThreadCount = {0};
	// 
	Strategy m_strategy = FCFS;

	// 主线程是否用作工作线程
	bool m_useCaller;
	// 如果是 -> 需要额外创建调度协程
	std::shared_ptr<Fiber> m_schedulerFiber;
	// 如果是 -> 记录主线程的线程id
	int m_rootThread = -1;
	// 是否正在关闭
	bool m_stopping = false;	
};

}

#endif