#pragma once

#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

template <class T> class MPSCQueue
{
  private:
    std::queue<T> q;
    std::mutex mtx;
    std::condition_variable cv;

  public:
    MPSCQueue() : q(){};
    ~MPSCQueue(){};

    void enqueue(T item)
    {
        std::lock_guard lock(mtx);
        q.push(item);
        cv.notify_one();
    }

    T dequeue()
    {
        std::unique_lock lock(mtx);
        cv.wait(lock, [&] { return !q.empty(); });

        auto val = q.front();
        q.pop();
        return val;
    }

    std::optional<T> dequeue_timeout()
    {
        std::unique_lock lock(mtx);
        cv.wait_for(lock, std::chrono::milliseconds(100));
        if (q.empty())
            return std::nullopt;

        auto val = q.front();
        q.pop();
        return val;
    }
};
