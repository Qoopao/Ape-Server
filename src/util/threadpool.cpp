#include "util/threadpool.h"
#include <cstdint>
#include <functional>
#include <mutex>
#include <spdlog/spdlog.h>
#include <thread>

ThreadPool::ThreadPool(){
    this->thread_num = 4;
    this->threads.reserve(this->thread_num);
}

void ThreadPool::start(){
    if(is_running.load()){
        return;
    }
    this->is_running.store(true);

    // 根据线程池线程数来创建线程
    for(uint16_t i = 0; i < this->thread_num; i++){
        try{
            this->threads.emplace_back(std::thread([this](){this->thread_worker();}));
        }
        catch(std::exception& e){
            this->is_running.store(false);
            try{
                this->stop();
            }
            catch(std::exception& e){
                spdlog::error("回滚线程时发生异常: " + std::string(e.what()));
            }
            
            throw std::runtime_error("线程池创建线程" + std::to_string(i) + "时发生未知异常: " + std::string(e.what()));
        }
    }
}

void ThreadPool::resize(uint16_t target_thread_num){
    // TO-DO: 线程池大小调整
}

void ThreadPool::stop(){

    // 加锁避免丢失唤醒（保证顺序），导致线程永久阻塞
    // 如果一个工作线程在准备让出锁的时候CPU控制权被执行stop的线程抢占了，stop设置is_running为false，
    // 此时工作线程还没进入等待队列，所以会错过通知，
    // 随后工作线程再次获得锁，发现条件不满足，让出锁，进入等待队列，等待下一次通知，这次通知就错过了，但是没有下次通知了
    {
        // 保证调用stop的线程的操作与工作线程互斥，能调用is_running.store(false);的线程在
        // 执行时所有工作线程都要么处于等待队列，要么还没拿到锁进入wait逻辑
        std::lock_guard<std::mutex> lck(this->mtx);
        is_running.store(false);
    }

    // 所有线程都会获得一次锁，所以确保所有线程依次退出
    this->cv.notify_all();

    // 等待所有线程退出
    for(auto& thread : this->threads){
        if(thread.joinable()){
            try{
                thread.join();
            }
            catch(std::exception& e){
                spdlog::error("线程池等待线程退出时发生未知异常: " + std::string(e.what()));
            }
        }
    }

    this->threads.clear();
    //清空任务队列
    std::queue<std::function<void()>> empty_queue;
    std::swap(tasks_queue, empty_queue);
    // 原队列出作用域后被析构
}

ThreadPool::~ThreadPool(){
    if(is_running.load()){
        this->stop();
    }
}

void ThreadPool::thread_worker(){
    while(true){
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> ulock(this->mtx);
            this->cv.wait(ulock, [this]()->bool{return !this->is_running.load() || !this->tasks_queue.empty();});
            if(!this->is_running.load() && this->tasks_queue.empty()){ //等待所有线程退出的条件是任务队列为空且线程池停止
                return;
            }
            task = std::move(this->tasks_queue.front());
            this->tasks_queue.pop();
        }
        try{
            task();  //执行任务
        }
        catch(...){
            spdlog::error("线程执行任务时发生未知异常");
            throw;
        }
    }
}

// 第一次调用时初始化
ThreadPool& ThreadPool::get_thread_pool_instance(){
    static ThreadPool thread_pool_instance;
    return thread_pool_instance;
}

