/*
 * A single-slot asynchronous presentation worker.
 *
 * The producer is Mir's compositor thread. It must only retain a frame and
 * signal the worker; presenting the frame may block indefinitely on a slow
 * DRM/USB device and is deliberately confined to the worker thread.
 */
#ifndef MIR_GRAPHICS_ANDROID_GUD_PRESENTATION_WORKER_H_
#define MIR_GRAPHICS_ANDROID_GUD_PRESENTATION_WORKER_H_

#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace mir
{
namespace graphics
{
namespace android
{
template<typename Frame>
class LatestPresentationWorker
{
public:
    using Present = std::function<void(Frame const&)>;
    using Failure = std::function<void(std::exception_ptr)>;

    struct Statistics
    {
        uint64_t submitted;
        uint64_t coalesced;
        uint64_t started;
        uint64_t completed;
        uint64_t failed;
        bool active;
        bool pending;
    };

    LatestPresentationWorker(Present present, Failure failure) :
        present{std::move(present)},
        failure{std::move(failure)},
        worker{[this] { run(); }}
    {
    }

    ~LatestPresentationWorker()
    {
        stop();
    }

    LatestPresentationWorker(LatestPresentationWorker const&) = delete;
    LatestPresentationWorker& operator=(LatestPresentationWorker const&) = delete;

    /* Replacing pending releases the superseded frame before this returns. */
    void submit(Frame frame)
    {
        auto replacement = std::unique_ptr<Frame>{new Frame{std::move(frame)}};
        {
            std::lock_guard<std::mutex> lock{state_mutex};
            if (stopping)
                return;
            ++submitted;
            if (pending)
                ++coalesced;
            pending = std::move(replacement);
        }
        wakeup.notify_one();
    }

    Statistics statistics() const
    {
        std::lock_guard<std::mutex> lock{state_mutex};
        return {submitted, coalesced, started, completed, failed, active, static_cast<bool>(pending)};
    }

    /*
     * Drop the pending frame, then wait until the active present call has
     * returned before destroying state it can reference. No producer mutex is
     * held while joining, so shutdown cannot deadlock a producer.
     */
    void stop()
    {
        std::unique_lock<std::mutex> stop_lock{stop_mutex};
        {
            std::lock_guard<std::mutex> lock{state_mutex};
            stopping = true;
            pending.reset();
        }
        wakeup.notify_one();
        if (worker.joinable())
            worker.join();
    }

private:
    void run()
    {
        for (;;)
        {
            std::unique_ptr<Frame> frame;
            {
                std::unique_lock<std::mutex> lock{state_mutex};
                wakeup.wait(lock, [this] { return stopping || pending; });
                if (stopping)
                    return;
                frame = std::move(pending);
                ++started;
                active = true;
            }

            try
            {
                present(*frame);
                mark_completed();
            }
            catch (...)
            {
                mark_failed();
                report_failure(std::current_exception());
            }
        }
    }

    void report_failure(std::exception_ptr error) noexcept
    {
        try
        {
            failure(error);
        }
        catch (...)
        {
            // An error reporter must never terminate the presentation worker.
        }
    }

    void mark_completed()
    {
        std::lock_guard<std::mutex> lock{state_mutex};
        ++completed;
        active = false;
    }

    void mark_failed()
    {
        std::lock_guard<std::mutex> lock{state_mutex};
        ++failed;
        active = false;
    }

    Present present;
    Failure failure;
    mutable std::mutex state_mutex;
    std::mutex stop_mutex;
    std::condition_variable wakeup;
    std::unique_ptr<Frame> pending;
    bool stopping{false};
    uint64_t submitted{0};
    uint64_t coalesced{0};
    uint64_t started{0};
    uint64_t completed{0};
    uint64_t failed{0};
    bool active{false};
    std::thread worker;
};
}
}
}

#endif
