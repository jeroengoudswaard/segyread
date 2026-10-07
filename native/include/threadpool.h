// Minimal persistent worker pool with a blocking parallelFor.
//
// A pyramid rebuild happens once per file load; a viewport re-decode happens
// on every pan/zoom frame. The latter is latency-sensitive enough that
// spawning/joining OS threads per call is worth avoiding, so the pool keeps
// its workers parked on a condition variable between calls instead of a
// full task-stealing scheduler (not needed: every call here is a single
// flat parallelFor, never nested or re-entrant).
#pragma once
#include <thread>
#include <vector>
#include <deque>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <atomic>

namespace segy {

class ThreadPool {
public:
    explicit ThreadPool(unsigned threadCount = 0) {
        if (threadCount == 0) threadCount = std::thread::hardware_concurrency();
        if (threadCount == 0) threadCount = 4;
        threadCount_ = threadCount;
        workers_.reserve(threadCount_);
        for (unsigned i = 0; i < threadCount_; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shuttingDown_ = true;
        }
        wakeCv_.notify_all();
        for (auto& t : workers_) t.join();
    }

    unsigned threadCount() const { return threadCount_; }

    // Splits [0, n) into `threadCount()` contiguous ranges (never fewer than
    // `minChunk` elements per range) and runs `fn(begin, end)` for each on a
    // worker, blocking until all finish. Runs inline with no thread
    // involvement if n is small enough for a single chunk.
    void parallelFor(size_t n, size_t minChunk,
                      const std::function<void(size_t begin, size_t end)>& fn) {
        if (n == 0) return;
        size_t chunks = std::min<size_t>(threadCount_, std::max<size_t>(1, (n + minChunk - 1) / minChunk));
        if (chunks <= 1) { fn(0, n); return; }

        size_t per = (n + chunks - 1) / chunks;
        std::atomic<size_t> remaining{0};
        std::mutex doneMutex;
        std::condition_variable doneCv;

        size_t submitted = 0;
        for (size_t c = 0; c < chunks; ++c) {
            size_t begin = c * per;
            size_t end = std::min(n, begin + per);
            if (begin >= end) break;
            ++submitted;
        }
        remaining.store(submitted);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (size_t c = 0; c < chunks; ++c) {
                size_t begin = c * per;
                size_t end = std::min(n, begin + per);
                if (begin >= end) break;
                tasks_.push_back([&fn, begin, end, &remaining, &doneMutex, &doneCv] {
                    fn(begin, end);
                    if (remaining.fetch_sub(1) == 1) {
                        std::lock_guard<std::mutex> dl(doneMutex);
                        doneCv.notify_one();
                    }
                });
            }
        }
        wakeCv_.notify_all();

        std::unique_lock<std::mutex> dl(doneMutex);
        doneCv.wait(dl, [&] { return remaining.load() == 0; });
    }

private:
    void workerLoop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wakeCv_.wait(lock, [this] { return shuttingDown_ || !tasks_.empty(); });
                if (shuttingDown_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    unsigned threadCount_ = 0;
    std::vector<std::thread> workers_;
    std::deque<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable wakeCv_;
    bool shuttingDown_ = false;
};

} // namespace segy
