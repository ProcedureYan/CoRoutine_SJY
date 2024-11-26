#include "thread.h"
#include <iostream>
#include <memory>
#include <vector>
#include <unistd.h>  

using namespace sylar;

void func()
{
    std::cout << "id: " << Thread::GetThreadId() << ", name: " << Thread::GetName();
    std::cout << ", this id: " << Thread::GetThis()->getId() << ", this name: " << Thread::GetThis()->getName() << std::endl;

    sleep(60);
}

int main() {
    std::vector<std::shared_ptr<Thread>> thrs;
    std::cout << "In main thread: " << sylar::Thread::GetName() << std::endl;
    std::cout << "t_thread in main: " << (sylar::Thread::GetThis() ? "Not null" : "Null") << std::endl;
    for(int i=0;i<5;i++)
    {
        std::shared_ptr<Thread> thr = std::make_shared<Thread>(&func, "thread_"+std::to_string(i));
        thrs.push_back(thr);
    }

    for(int i=0;i<5;i++)
    {
        thrs[i]->join();
    }

    return 0;
}