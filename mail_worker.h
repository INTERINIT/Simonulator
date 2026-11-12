#pragma once
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace postoffice {

// Password derivation takes longer than a voice packet's 20 ms deadline.
// The worker handles one account operation at a time. Only the network or
// window thread applies its result to a client or GUI control.
class AccountWorker {
public:
    AccountWorker() : worker([this] { run(); }) {}

    ~AccountWorker() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            jobs.clear();
        }
        available.notify_one();
        worker.join();
    }

    bool submit(std::function<void()> job) {
        {
            std::lock_guard lock(mutex);
            if (jobs.size() >= 16 || stopping) return false;
            jobs.push_back(std::move(job));
        }
        available.notify_one();
        return true;
    }

private:
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::function<void()>> jobs;
    bool stopping = false;
    std::thread worker;

    void run() {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock lock(mutex);
                available.wait(lock, [this] { return stopping || !jobs.empty(); });
                if (stopping) return;
                job = std::move(jobs.front());
                jobs.pop_front();
            }
            job();
        }
    }
};
} // namespace postoffice
