#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <atomic>
#include <mutex>
#include <vector>
#include <queue>
#include <cstdint>
#include <thread>
#include <functional>
#include <condition_variable>
#include <future>
#include <type_traits>


// 单例线程池，全局共享
class ThreadPool{
public:
    // 获取线程池实例
    static ThreadPool& get_thread_pool_instance();

    // 禁止拷贝/移动
    ThreadPool(const ThreadPool&) = delete;  //拷贝构造
    ThreadPool(ThreadPool&&) = delete;   //移动构造
    ThreadPool& operator=(const ThreadPool&) = delete;  //拷贝赋值
    ThreadPool& operator=(ThreadPool&&) = delete;   //移动赋值

    ~ThreadPool();

    
    void resize(uint16_t target_thread_num);  //调整线程池大小
    void stop();    //停止线程池，可以手动调用

    // 提交任务到线程池
    template<typename Func, typename ...Arg>
    auto put_task(Func&& func, Arg&&... args)-> std::future<std::invoke_result_t<Func, Arg...>> {

        // 任务队列的元素时无参无返回值类型，所以这里要将用户传入的函数与参数包装成一个无参无返回值的函数
        
        // 1. packaged_task只能移动不能拷贝
        // 2. 确保packaged_task在lambda执行前不会被销毁，所以用shared_ptr
        auto task = std::make_shared<std::packaged_task<std::invoke_result_t<Func, Arg...>()>>(
            std::bind(std::forward<Func>(func), std::forward<Arg>(args)...)
        );
        
        {
            // 直到put_task被调用再启动线程池，因为单例是全局共享的，所以我们默认有多个线程调用put_task
            std::lock_guard<std::mutex> lck(this->mtx);
            if(!this->is_running.load()){
                this->start();
            }
            //对队列的操作需要加锁，要不保证不了队列的原子性
            if(!this->is_running.load()){
                throw std::runtime_error("put_task失败, 线程池未运行");
            }
            // 值捕获拷贝，引用计数+1
            this->tasks_queue.push([task](){(*task)();}); 
            this->cv.notify_one();  //通知一个线程执行任务
        }

        return task->get_future();
    }
        
private:
    ThreadPool();
    void start();   //启动线程池，不允许手动调用，只能在put_task时调用

    uint16_t thread_num;
    std::vector<std::thread> threads;
    std::queue<std::function<void()>> tasks_queue;  //任务队列，等待线程从中取出任务
    std::atomic<bool> is_running{false};
    std::mutex mtx;
    std::condition_variable cv;

    void thread_worker();  //持续检查线程中是否有任务
};




#endif