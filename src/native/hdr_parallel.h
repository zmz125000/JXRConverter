#pragma once

#include "jxrc/export.hpp"
#include <algorithm>
#include <cstddef>
#include <system_error>
#include <thread>
#include <vector>

// A budget belongs to the calling conversion, not to the process or GUI pool.
JXRC_API unsigned jxrc_thread_budget();
JXRC_API void jxrc_set_thread_budget(unsigned);
struct JxrcThreadBudget {
    unsigned previous;
    explicit JxrcThreadBudget(unsigned count) : previous(jxrc_thread_budget()) { jxrc_set_thread_budget(count); }
    ~JxrcThreadBudget() { jxrc_set_thread_budget(previous); }
};
inline unsigned jxrc_limit_threads(unsigned count) {
    const unsigned budget = jxrc_thread_budget();
    return std::max(1u, budget ? std::min(count, budget) : count);
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
        std::min(max_threads, 8u), std::max<size_t>(1, pixels / 131072)));
}

JXRC_API void jxrc_parallel_for(size_t pixels, unsigned jobs,
                                void (*fn)(void*, size_t, size_t, unsigned), void* ctx);

template <typename Work>
void hdr_parallel_pixels(size_t pixels, unsigned jobs, Work work) {
    if (jobs <= 1) {
        work(0, pixels, 0);
        return;
    }
    jxrc_parallel_for(
        pixels, jobs,
        [](void* ctx, size_t begin, size_t end, unsigned job) {
            (*static_cast<Work*>(ctx))(begin, end, job);
        },
        &work);
}
