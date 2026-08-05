#include "game/db_worker.h"

#include "rpc/event_loop.h"

namespace game {

DbWorker::DbWorker(rpc::EventLoop* loop) : loop_(loop) {
}

DbWorker::~DbWorker() {
    Stop();
}

void DbWorker::Start() {
    std::lock_guard<std::mutex> lock(mu_);
    if (thr_.joinable()) {
        return;
    }

    stop_ = false;
    thr_ = std::thread(&DbWorker::WorkerLoop, this);
}

void DbWorker::Stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!thr_.joinable()) {
            return;
        }
        stop_ = true;
    }
    cv_.notify_all();
    thr_.join();
}

void DbWorker::Post(Task task) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_) {
            return;
        }
        q_.push_back(std::move(task));
    }
    cv_.notify_one();
}

void DbWorker::PostToLoop(std::function<void()> fn) {
    loop_->RunInLoop(std::move(fn));
}

void DbWorker::WorkerLoop() {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mu_);
            cv_.wait(lock, [this] { return stop_ || !q_.empty(); });
            if (stop_ && q_.empty()) {
                return;
            }
            task = std::move(q_.front());
            q_.pop_front();
        }
        task();
    }
}

} // namespace game
