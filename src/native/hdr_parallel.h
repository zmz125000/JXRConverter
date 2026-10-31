#pragma once

#include <algorithm>
#include <cstddef>
#include <system_error>
#include <thread>
#include <vector>

// A budget belongs to the calling conversion, not to the process or GUI pool.
inline thread_local unsigned jxrc_codec_threads = 0;
struct JxrcThreadBudget {
    unsigned previous;
    explicit JxrcThreadBudget(unsigned count) : previous(jxrc_codec_threads) { jxrc_codec_threads = count; }
    ~JxrcThreadBudget() { jxrc_codec_threads = previous; }
};
inline unsigned jxrc_limit_threads(unsigned count) {
    return std::max(1u, jxrc_codec_threads ? std::min(count, jxrc_codec_threads) : count);
}

// Reader pages can decode concurrently. Share the cores and avoid starting
// workers for small buffers; the calling thread is included in the budget.
inline unsigned hdr_pixel_jobs(size_t pixels, unsigned max_threads = 0) {
    if (max_threads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        max_threads = std::max(1u, (hw == 0 ? 4u : hw) / 2);
    }
    max_threads = jxrc_limit_threads(max_threads);
    return static_cast<unsigned>(std::min<size_t>(
        std::min(max_threads, 8u), std::max<size_t>(1, pixels / 262144)));
}

template <typename Work>
void hdr_parallel_pixels(size_t pixels, unsigned jobs, Work work) {
    std::vector<std::thread> workers;
    workers.reserve(jobs - 1);
    for (unsigned job = 1; job < jobs; ++job) {
        const size_t begin = pixels * job / jobs;
        const size_t end = pixels * (job + 1) / jobs;
        try {
            workers.emplace_back(work, begin, end, job);
        } catch (const std::system_error&) {
            work(begin, end, job);
        }
    }
    work(0, pixels / jobs, 0);
    for (auto& worker : workers) worker.join();
}
