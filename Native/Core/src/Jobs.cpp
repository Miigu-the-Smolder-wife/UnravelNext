#include "unx/core/Jobs.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace unx
{
struct Jobs::Impl
{
    struct Loop
    {
        const std::function<void(uint32_t)>* fn = nullptr;
        uint32_t count = 0;
        std::atomic<uint32_t> next{ 0 };
        std::atomic<uint32_t> done{ 0 };
        std::exception_ptr error;
        std::mutex errorMutex;
        uint32_t active = 0;  // workers inside run(); guarded by Impl::mutex so the caller never frees a loop in use
    };

    std::vector<std::thread> threads;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    Loop* loop = nullptr;
    uint64_t generation = 0;
    bool quit = false;

    static void run(Loop& l)
    {
        for (;;)
        {
            uint32_t i = l.next.fetch_add(1, std::memory_order_relaxed);
            if (i >= l.count) return;
            try
            {
                (*l.fn)(i);
            }
            catch (...)
            {
                std::lock_guard lock(l.errorMutex);
                if (!l.error) l.error = std::current_exception();
            }
            l.done.fetch_add(1, std::memory_order_acq_rel);
        }
    }

    void worker()
    {
        uint64_t seen = 0;
        for (;;)
        {
            Loop* l;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return quit || (loop && generation != seen); });
                if (quit) return;
                seen = generation;
                l = loop;
                ++l->active;
            }
            run(*l);
            std::lock_guard lock(mutex);
            --l->active;
            finished.notify_all();
        }
    }
};

Jobs& Jobs::instance()
{
    static Jobs jobs;
    return jobs;
}

Jobs::Jobs() : m_impl(new Impl)
{
    unsigned n = std::thread::hardware_concurrency();
    unsigned workers = n > 1 ? n - 1 : 1;
    for (unsigned i = 0; i < workers; ++i) m_impl->threads.emplace_back([this] { m_impl->worker(); });
}

Jobs::~Jobs()
{
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->quit = true;
    }
    m_impl->wake.notify_all();
    for (auto& t : m_impl->threads) t.join();
    delete m_impl;
}

uint32_t Jobs::workerCount() const { return (uint32_t)m_impl->threads.size(); }

void Jobs::parallelFor(uint32_t count, const std::function<void(uint32_t)>& fn)
{
    if (count == 0) return;
    if (count == 1)
    {
        fn(0);
        return;
    }
    Impl::Loop loop;
    loop.fn = &fn;
    loop.count = count;
    {
        std::lock_guard lock(m_impl->mutex);
        m_impl->loop = &loop;
        ++m_impl->generation;
    }
    m_impl->wake.notify_all();
    Impl::run(loop);
    {
        std::unique_lock lock(m_impl->mutex);
        m_impl->finished.wait(lock, [&] { return loop.done.load(std::memory_order_acquire) == loop.count && loop.active == 0; });
        m_impl->loop = nullptr;
    }
    if (loop.error) std::rethrow_exception(loop.error);
}
} // namespace unx
