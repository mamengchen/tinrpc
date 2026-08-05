#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace rpc {
class EventLoop;
}

namespace game {

class DbWorker {
public:
    using Task = std::function<void()>;

    explicit DbWorker(rpc::EventLoop* loop);
    ~DbWorker();

    DbWorker(const DbWorker&) = delete;
    DbWorker& operator=(const DbWorker&) = delete;

    void Start();
    void Stop();
    void Post(Task task);
    void PostToLoop(std::function<void()> fn);

private:
    void WorkerLoop();

    rpc::EventLoop* loop_;
    std::thread thr_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Task> q_;
    bool stop_ = false;
};

} // namespace game
