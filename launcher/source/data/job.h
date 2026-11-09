// A background task the UI polls once per frame (disc extraction, profile refresh, sign-in).
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace mnx {

struct JobStatus {
    bool running = false;
    bool done = false;
    bool ok = false;
    float fraction = -1.0f;  // < 0: indeterminate
    std::string stage;       // what it is doing now
    std::string message;     // the result, once done
};

// One job at a time. Run() starts `work` on a thread; `work` reports through the Reporter and
// returns its result. Cancel() asks it to stop; work checks Reporter::Cancelled().
class Job {
public:
    class Reporter {
    public:
        explicit Reporter(Job& job) : job_(job) {}
        void Stage(std::string s, float fraction = -1.0f) {
            std::lock_guard<std::mutex> lk(job_.mutex_);
            job_.status_.stage = std::move(s);
            job_.status_.fraction = fraction;
        }
        bool Cancelled() const { return job_.cancel_.load(); }

    private:
        Job& job_;
    };
    struct Result {
        bool ok = false;
        std::string message;
    };
    using Work = std::function<Result(Reporter&)>;

    // Leaving the app mid-job (HOME, a crash screen popping) stops the work at its next check.
    ~Job() {
        Cancel();
        Join();
    }

    // False if a job is still running.
    bool Run(Work work) {
        if (Status().running) return false;
        Join();
        cancel_ = false;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            status_ = JobStatus{};
            status_.running = true;
        }
        thread_ = std::thread([this, work = std::move(work)]() {
            Reporter r(*this);
            Result res = work(r);
            std::lock_guard<std::mutex> lk(mutex_);
            status_.running = false;
            status_.done = true;
            status_.ok = res.ok;
            status_.message = std::move(res.message);
        });
        return true;
    }
    JobStatus Status() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return status_;
    }
    void Cancel() { cancel_ = true; }
    void Join() {
        if (thread_.joinable()) thread_.join();
    }

private:
    mutable std::mutex mutex_;
    JobStatus status_;
    std::atomic<bool> cancel_{false};
    std::thread thread_;
};

}  // namespace mnx
