#pragma once
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace zenasset
{
// Import-scoped workers. A bounded queue prevents unbounded pending task state;
// mesh stages separately bound their in-flight result buffers.
class WorkerPool
{
public:
    explicit WorkerPool(uint32_t workerCount)
    {
        const uint32_t available = std::max(1u, std::thread::hardware_concurrency());
        const uint32_t count     = workerCount == 0 ? available : std::min(workerCount, available);
        m_capacity               = count * 2;
        for (uint32_t index = 0; index < count; ++index)
        {
            m_threads.emplace_back(&WorkerPool::Run, this);
        }
    }
    WorkerPool(const WorkerPool&)            = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;
    ~WorkerPool()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stopping = true;
        }
        m_work.notify_all();
        for (std::thread& thread : m_threads)
        {
            thread.join();
        }
    }
    size_t GetSize() const
    {
        return m_threads.size();
    }
    template <class F, class... Args> std::future<std::invoke_result_t<F, Args...>> Push(F&& function, Args&&... args)
    {
        using Result = std::invoke_result_t<F, Args...>;
        std::shared_ptr<std::packaged_task<Result()>> task =
            std::make_shared<std::packaged_task<Result()>>(std::bind(std::forward<F>(function), std::forward<Args>(args)...));
        std::future<Result> result = task->get_future();
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_space.wait(lock, [this] { return m_queue.size() < m_capacity; });
            m_queue.emplace_back([task] { (*task)(); });
        }
        m_work.notify_one();
        return result;
    }

private:
    void Run()
    {
        bool running = true;
        while (running)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_work.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
                if (!m_queue.empty())
                {
                    task = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                else
                {
                    running = false;
                }
            }
            m_space.notify_one();
            if (task)
            {
                task();
            }
        }
    }
    std::mutex                        m_mutex;
    std::condition_variable           m_work;
    std::condition_variable           m_space;
    std::deque<std::function<void()>> m_queue;
    std::vector<std::thread>          m_threads;
    size_t                            m_capacity{1};
    bool                              m_stopping{false};
};
} // namespace zenasset
