#include "hdr_parallel.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>

namespace {
thread_local unsigned budget = 0;
thread_local bool in_pool_worker = false;

class PixelThreadPool {
public:
    struct Batch {
        void (*fn)(void*, size_t, size_t, unsigned);
        void* ctx;
        size_t pixels;
        unsigned jobs;
        std::atomic<unsigned> next_job{1};
        unsigned remaining = jobs;
        std::mutex done_mutex{};
        std::condition_variable done_cv{};
        std::exception_ptr error{};
    };

    PixelThreadPool() {
        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned count = std::min(std::max(1u, hw ? hw : 4u), 8u);
        workers_.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            try {
                workers_.emplace_back([this] { worker_loop(); });
            } catch (const std::system_error&) {
                break;
            }
        }
    }

    ~PixelThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
    }

    void run(size_t pixels, unsigned jobs, void (*fn)(void*, size_t, size_t, unsigned), void* ctx) {
        if (jobs <= 1 || in_pool_worker || workers_.empty()) {
            fn(ctx, 0, pixels, 0);
            return;
        }
        Batch batch{fn, ctx, pixels, jobs};
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(&batch);
        }
        cv_.notify_all();

        // Execute slice 0 on the calling thread, then help drain remaining slices if needed.
        execute_slice(batch, 0);
        for (;;) {
            const unsigned job = batch.next_job.fetch_add(1, std::memory_order_relaxed);
            if (job >= jobs) break;
            execute_slice(batch, job);
        }
        {
            // The last worker must finish notifying before this stack batch can be destroyed.
            std::unique_lock<std::mutex> lock(batch.done_mutex);
            batch.done_cv.wait(lock, [&] { return batch.remaining == 0; });
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto it = std::find(queue_.begin(), queue_.end(), &batch);
            if (it != queue_.end()) queue_.erase(it);
        }
        if (batch.error) std::rethrow_exception(batch.error);
    }

private:
    static void execute_slice(Batch& batch, unsigned job) {
        const size_t begin = batch.pixels * job / batch.jobs;
        const size_t end = batch.pixels * (job + 1) / batch.jobs;
        std::exception_ptr error;
        try {
            batch.fn(batch.ctx, begin, end, job);
        } catch (...) {
            error = std::current_exception();
        }
        std::lock_guard<std::mutex> lock(batch.done_mutex);
        if (error && !batch.error) batch.error = error;
        if (--batch.remaining == 0) batch.done_cv.notify_one();
    }

    void worker_loop() {
        in_pool_worker = true;
        for (;;) {
            Batch* batch = nullptr;
            unsigned job = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
                if (stopping_ && queue_.empty()) return;
                while (!queue_.empty()) {
                    Batch* front = queue_.front();
                    const unsigned claimed = front->next_job.fetch_add(1, std::memory_order_relaxed);
                    if (claimed < front->jobs) {
                        batch = front;
                        job = claimed;
                        if (claimed + 1 >= front->jobs) queue_.pop_front();
                        break;
                    }
                    queue_.pop_front();
                }
            }
            if (!batch) continue;
            execute_slice(*batch, job);
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Batch*> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

PixelThreadPool& pool() {
    static PixelThreadPool instance;
    return instance;
}
} // namespace

unsigned jxrc_thread_budget() { return budget; }
void jxrc_set_thread_budget(unsigned value) { budget = value; }
void jxrc_parallel_for(size_t pixels, unsigned jobs,
                       void (*fn)(void*, size_t, size_t, unsigned), void* ctx) {
    pool().run(pixels, jobs, fn, ctx);
}
