#include "fiber.h"
#include <vector>

static bool debug = false;

namespace sylar {

// 当前线程上的协程控制信息

// 维护协程嵌套的上下文结构
static struct FibersEnv{
	std::vector<std::shared_ptr<Fiber>> pCallStack;
	int pCallSize;

}FibersEnv;

// 正在运行的协程
static thread_local Fiber* t_fiber = nullptr;
// 主协程
static thread_local std::shared_ptr<Fiber> t_main_fiber = nullptr;
// 调度协程
static thread_local Fiber* t_scheduler_fiber = nullptr;

// 协程计数器
static std::atomic<uint64_t> s_fiber_id{0};
// 协程id
static std::atomic<uint64_t> s_fiber_count{0};

void Fiber::SetThis(Fiber *f)
{
	t_fiber = f;
}

// 首先运行该函数创建主协程
std::shared_ptr<Fiber> Fiber::GetThis()
{
	if(t_fiber)
	{	
		return t_fiber->shared_from_this();
	}

	std::shared_ptr<Fiber> main_fiber(new Fiber());
	t_main_fiber = main_fiber;
	// t_scheduler_fiber = main_fiber.get(); // 除非主动设置 主协程默认为调度协程
	
	assert(t_fiber == main_fiber.get());
	return t_fiber->shared_from_this();
}

void Fiber::SetSchedulerFiber(Fiber* f)
{
	t_scheduler_fiber = f;
}

uint64_t Fiber::GetFiberId()
{
	if(t_fiber)
	{
		return t_fiber->getId();
	}
	return (uint64_t)-1;
}

Fiber::Fiber()
{
	SetThis(this);
	m_state = RUNNING;
	
	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber() failed\n";
		pthread_exit(NULL);
	}
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): main id = " << m_id << std::endl;
}

Fiber::Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler):
m_cb(cb), m_runInScheduler(run_in_scheduler)
{
	m_state = READY;

	// 分配协程栈空间
	m_stacksize = stacksize ? stacksize : 128000;
	m_stack = malloc(m_stacksize);

	if(getcontext(&m_ctx))
	{
		std::cerr << "Fiber(std::function<void()> cb, size_t stacksize, bool run_in_scheduler) failed\n";
		pthread_exit(NULL);
	}
	
	m_ctx.uc_link = nullptr;
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
	
	m_id = s_fiber_id++;
	s_fiber_count ++;
	if(debug) std::cout << "Fiber(): child id = " << m_id << std::endl;
}

Fiber::~Fiber()
{
	s_fiber_count --;
	if(m_stack)
	{
		free(m_stack);
	}
	if(debug) std::cout << "~Fiber(): id = " << m_id << std::endl;	
}

void Fiber::reset(std::function<void()> cb)
{
	assert(m_stack != nullptr&&m_state == TERM);

	m_state = READY;
	m_cb = cb;

	if(getcontext(&m_ctx))
	{
		std::cerr << "reset() failed\n";
		pthread_exit(NULL);
	}

	m_ctx.uc_link = nullptr;
	m_ctx.uc_stack.ss_sp = m_stack;
	m_ctx.uc_stack.ss_size = m_stacksize;
	makecontext(&m_ctx, &Fiber::MainFunc, 0);
}

void Fiber::resume()
{
	assert(m_state==READY);
	
	m_state = RUNNING;
	// 输出当前协程的信息
    // std::cout << t_fiber->getId() << std::endl;

	if(FibersEnv.pCallSize == 0){ //证明是主线程
		FibersEnv.pCallStack.push_back(shared_from_this());
		++ FibersEnv.pCallSize;

		SetThis(this);
		if(swapcontext(&(t_main_fiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_main_fiber failed\n";
			pthread_exit(NULL);
		}	
	}else{
		std::shared_ptr<Fiber> thisFiber = FibersEnv.pCallStack.back();
		FibersEnv.pCallStack.push_back(shared_from_this());
		++ FibersEnv.pCallSize;

		SetThis(this);
		if(swapcontext(&(thisFiber->m_ctx), &m_ctx))
		{
			std::cerr << "resume() to t_main_fiber failed\n";
			pthread_exit(NULL);
		}
	}

}

void Fiber::yield()
{
	assert(m_state==RUNNING || m_state==TERM);

	if(m_state!=TERM)
	{
		m_state = READY;
	}

	FibersEnv.pCallStack.pop_back();
	-- FibersEnv.pCallSize;

	if(FibersEnv.pCallSize > 0){
		std::shared_ptr<Fiber> lastFiber = FibersEnv.pCallStack.back();

		SetThis(lastFiber.get());
		if(swapcontext(&m_ctx, &(lastFiber->m_ctx)))
		{
			std::cerr << "yield() to t_main_fiber failed\n";
			pthread_exit(NULL);
		}	
	}
	else{
		SetThis(t_main_fiber.get());
		if(swapcontext(&m_ctx, &(t_main_fiber->m_ctx)))
		{
			std::cerr << "yield() to t_main_fiber failed\n";
			pthread_exit(NULL);
		}
	}
}

void Fiber::MainFunc()
{
	std::shared_ptr<Fiber> curr = GetThis();
	assert(curr!=nullptr);

	curr->m_cb(); 
	curr->m_cb = nullptr;
	curr->m_state = TERM;

	// 运行完毕 -> 让出执行权
	auto raw_ptr = curr.get();
	curr.reset(); 
	raw_ptr->yield();
}

}