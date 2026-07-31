#ifndef MIR_UTILS_GUD_SCREENCAST_PRESENTER_H_
#define MIR_UTILS_GUD_SCREENCAST_PRESENTER_H_

#include "gud_screencast_format.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace mirgud
{
struct Stats
{
    uint64_t received{};
    uint64_t submitted{};
    uint64_t presented{};
    uint64_t dropped{};
    uint64_t cancelled{};
    uint64_t conversion_failures{};
    uint64_t submit_failures{};
    TimingSummary capture_us{};
    TimingSummary conversion_us{};
    TimingSummary submit_us{};
    uint64_t conversion_path_counts[5]{};
};

inline bool accounting_ok(Stats const& stats)
{
    return stats.submitted == stats.presented + stats.dropped + stats.cancelled + stats.submit_failures;
}

inline double percent(uint64_t value, uint64_t total)
{
    return total ? static_cast<double>(value) * 100.0 / static_cast<double>(total) : 0.0;
}

inline std::string format_accounting_fields(Stats const& stats, bool final)
{
    std::ostringstream output;
    output << "report_kind=" << (final ? "final" : "periodic") <<
        " final=" << final <<
        " frames_received=" << stats.received <<
        " frames_submitted=" << stats.submitted <<
        " frames_presented=" << stats.presented <<
        " frames_dropped=" << stats.dropped <<
        " frames_cancelled=" << stats.cancelled <<
        " drop_percent=" << percent(stats.dropped, stats.submitted) <<
        " cancellation_percent=" << percent(stats.cancelled, stats.submitted) <<
        " conversion_failures=" << stats.conversion_failures <<
        " gud_submit_failures=" << stats.submit_failures <<
        " accounting_ok=" << accounting_ok(stats);
    return output.str();
}

class LatestFramePresenter
{
public:
    explicit LatestFramePresenter(
        std::function<void(Frame const&)> present,
        std::function<void()> first_presented = {},
        std::function<void()> presentation_failed = {}) :
        present{std::move(present)}, first_presented{std::move(first_presented)},
        presentation_failed{std::move(presentation_failed)}, worker{[this] { work(); }}
    {
    }

    ~LatestFramePresenter()
    {
        stop();
    }

    LatestFramePresenter(LatestFramePresenter const&) = delete;
    LatestFramePresenter& operator=(LatestFramePresenter const&) = delete;

    void submit(Frame frame)
    {
        auto incoming = std::make_unique<Frame>(std::move(frame));
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (stopping)
                return;
            ++statistics.submitted;
            if (pending)
                ++statistics.dropped;
            pending = std::move(incoming);
        }
        wakeup.notify_one();
    }

    void conversion_failed()
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.conversion_failures;
    }

    void received(uint64_t capture_us, uint64_t conversion_us)
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.received;
        statistics.capture_us.add(capture_us);
        statistics.conversion_us.add(conversion_us);
        ++statistics.conversion_path_counts[static_cast<unsigned>(conversion_path)];
    }

    Stats stats() const
    {
        std::lock_guard<std::mutex> lock{mutex};
        return statistics;
    }

    void set_conversion_path(ConversionPath path)
    {
        std::lock_guard<std::mutex> lock{mutex};
        conversion_path = path;
    }

    void rethrow_failure()
    {
        std::exception_ptr failure;
        {
            std::lock_guard<std::mutex> lock{mutex};
            failure = presentation_failure;
        }
        if (failure)
            std::rethrow_exception(failure);
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            if (!stopping)
            {
                stopping = true;
                if (pending)
                {
                    ++statistics.cancelled;
                    pending.reset();
                }
            }
        }
        wakeup.notify_one();
        if (worker.joinable())
            worker.join();
    }

private:
    void work()
    {
        while (true)
        {
            std::unique_ptr<Frame> frame;
            {
                std::unique_lock<std::mutex> lock{mutex};
                wakeup.wait(lock, [this] { return stopping || pending; });
                if (stopping)
                    return;
                frame = std::move(pending);
            }

            auto const submit_start = std::chrono::steady_clock::now();
            try
            {
                present(*frame);
                auto const submit_end = std::chrono::steady_clock::now();
                bool is_first{};
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    statistics.submit_us.add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count()));
                    ++statistics.presented;
                    is_first = statistics.presented == 1;
                }
                if (is_first && first_presented)
                    first_presented();
            }
            catch (...)
            {
                auto const submit_end = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    statistics.submit_us.add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count()));
                    ++statistics.submit_failures;
                    presentation_failure = std::current_exception();
                    stopping = true;
                    if (pending)
                    {
                        ++statistics.cancelled;
                        pending.reset();
                    }
                }
                wakeup.notify_one();
                if (presentation_failed)
                    presentation_failed();
                return;
            }
        }
    }

    std::function<void(Frame const&)> present;
    std::function<void()> first_presented;
    std::function<void()> presentation_failed;
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::unique_ptr<Frame> pending;
    Stats statistics;
    ConversionPath conversion_path{ConversionPath::channel_reorder};
    std::exception_ptr presentation_failure;
    bool stopping{};
    std::thread worker;
};
}

#endif
