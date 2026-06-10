#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>

class ThreadPool {
public:
    ThreadPool(size_t threads);
    
    // 投递任务的模板函数
    template<class F, class... Args>
    auto enqueue(F&& f, Args&&... args) 
        -> std::future<typename std::result_of<F(Args...)>::type>;
        
    ~ThreadPool();

private:
    std::vector<std::thread> workers;        // 工作线程池
    std::queue<std::function<void()>> tasks; // 任务队列
    
    std::mutex queue_mutex;                  // 队列锁
    std::condition_variable condition;       // 条件变量
    bool stop;                               // 停止标志
};

// 构造函数：启动指定数量的工作线程
inline ThreadPool::ThreadPool(size_t threads) : stop(false) {
    for(size_t i = 0; i<threads; ++i)
        workers.emplace_back([this] {
            for(;;) {
                std::function<void()> task;
                {
                    std::unique_lock<std::mutex> lock(this->queue_mutex);
                    // 等待直到有任务或者线程池停止
                    this->condition.wait(lock, [this]{ return this->stop || !this->tasks.empty(); });
                    if(this->stop && this->tasks.empty()) return;
                    task = std::move(this->tasks.front());
                    this->tasks.pop();
                }
                task(); // 执行任务
            }
        });
}

// 投递任务
template<class F, class... Args>
auto ThreadPool::enqueue(F&& f, Args&&... args) 
    -> std::future<typename std::result_of<F(Args...)>::type> 
{
    using return_type = typename std::result_of<F(Args...)>::type;

    auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        
    std::future<return_type> res = task->get_future();
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        if(stop) throw std::runtime_error("enqueue on stopped ThreadPool");
        tasks.emplace([task](){ (*task)(); });
    }
    condition.notify_one(); // 唤醒一个线程
    return res;
}

// 析构函数：停止所有线程
inline ThreadPool::~ThreadPool() {
    {
        std::unique_lock<std::mutex> lock(queue_mutex);
        stop = true;
    }
    condition.notify_all();
    for(std::thread &worker: workers)
        worker.join();
}

#endif