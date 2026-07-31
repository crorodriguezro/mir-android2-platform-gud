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
    uint64_t in_flight{};
    uint64_t presented{};
    uint64_t dropped{};
    uint64_t cancelled{};
    uint64_t capture_failures{};
    uint64_t conversion_failures{};
    uint64_t release_failures{};
    uint64_t dump_failures{};
    uint64_t submit_failures{};
    uint64_t lifecycle_callback_failures{};
    XByteSamples x_byte_samples{};
    TimingSummary acquire_us{};
    TimingSummary conversion_us{};
    TimingSummary release_us{};
    TimingSummary capture_cycle_us{};
    TimingSummary submit_us{};
    TimingSummary pattern_source_generation_us{};
    TimingSummary pattern_format_conversion_us{};
    TimingSummary pattern_frame_total_us{};
    uint64_t conversion_path_counts[5]{};
};

inline bool accounting_ok(Stats const& stats)
{
    return stats.submitted == stats.presented + stats.dropped + stats.cancelled +
        stats.submit_failures + stats.in_flight;
}

inline uint64_t fps(uint64_t count, uint64_t elapsed_us)
{
    return elapsed_us ? count * 1000000ULL / elapsed_us : 0;
}

inline uint64_t counter_delta(uint64_t current, uint64_t previous)
{
    return current >= previous ? current - previous : 0;
}

inline double percent(uint64_t value, uint64_t total)
{
    return total ? static_cast<double>(value) * 100.0 / static_cast<double>(total) : 0.0;
}

inline std::string format_accounting_fields(Stats const& stats, bool final)
{
    std::ostringstream output;
    output << "report_kind=" << (final ? "final" : "periodic") <<
        " final=" << (final ? "true" : "false") <<
        " frames_received=" << stats.received <<
        " frames_submitted=" << stats.submitted <<
        " frames_in_flight=" << stats.in_flight <<
        " frames_presented=" << stats.presented <<
        " frames_dropped=" << stats.dropped <<
        " frames_cancelled=" << stats.cancelled <<
        " drop_percent=" << percent(stats.dropped, stats.submitted) <<
        " cancellation_percent=" << percent(stats.cancelled, stats.submitted) <<
        " capture_failures=" << stats.capture_failures <<
        " conversion_failures=" << stats.conversion_failures <<
        " release_failures=" << stats.release_failures <<
        " dump_failures=" << stats.dump_failures <<
        " gud_submit_failures=" << stats.submit_failures <<
        " lifecycle_callback_failures=" << stats.lifecycle_callback_failures <<
        " x_byte_samples=" << stats.x_byte_samples.count <<
        " x_byte_min=" << static_cast<unsigned>(stats.x_byte_samples.count ? stats.x_byte_samples.min : 0) <<
        " x_byte_max=" << static_cast<unsigned>(stats.x_byte_samples.max) <<
        " x_byte_constant=" << (stats.x_byte_samples.constant ? "true" : "false") <<
        " x_byte_zero_percent=" << percent(stats.x_byte_samples.zero, stats.x_byte_samples.count) <<
        " x_byte_ff_percent=" << percent(stats.x_byte_samples.ff, stats.x_byte_samples.count) <<
        " accounting_ok=" << (accounting_ok(stats) ? "true" : "false");
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
        std::unique_ptr<Frame> incoming;
        try
        {
            incoming = std::make_unique<Frame>(std::move(frame));
        }
        catch (...)
        {
            std::lock_guard<std::mutex> lock{mutex};
            ++statistics.submitted;
            ++statistics.submit_failures;
            throw;
        }
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

    void capture_failed() { increment(&Stats::capture_failures); }
    void conversion_failed() { increment(&Stats::conversion_failures); }
    void release_failed() { increment(&Stats::release_failures); }
    void dump_failed() { increment(&Stats::dump_failures); }

    void add_x_byte_samples(XByteSamples const& samples)
    {
        std::lock_guard<std::mutex> lock{mutex};
        if (!samples.count)
            return;
        if (!statistics.x_byte_samples.count)
            statistics.x_byte_samples = samples;
        else
        {
            auto& destination = statistics.x_byte_samples;
            destination.constant = destination.constant && samples.constant &&
                destination.first == samples.first;
            destination.count += samples.count;
            destination.min = std::min(destination.min, samples.min);
            destination.max = std::max(destination.max, samples.max);
            destination.zero += samples.zero;
            destination.ff += samples.ff;
        }
    }

    void received(uint64_t acquire_us, uint64_t conversion_us, uint64_t release_us,
                  uint64_t capture_cycle_us)
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.received;
        statistics.acquire_us.add(acquire_us);
        statistics.conversion_us.add(conversion_us);
        statistics.release_us.add(release_us);
        statistics.capture_cycle_us.add(capture_cycle_us);
        ++statistics.conversion_path_counts[static_cast<unsigned>(conversion_path)];
    }

    void add_pattern_timings(uint64_t source_generation_us, uint64_t format_conversion_us,
                             uint64_t frame_total_us)
    {
        std::lock_guard<std::mutex> lock{mutex};
        statistics.pattern_source_generation_us.add(source_generation_us);
        statistics.pattern_format_conversion_us.add(format_conversion_us);
        statistics.pattern_frame_total_us.add(frame_total_us);
    }

    void record_conversion_path(ConversionPath path)
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++statistics.conversion_path_counts[static_cast<unsigned>(path)];
    }

    void increment(uint64_t Stats::*member)
    {
        std::lock_guard<std::mutex> lock{mutex};
        ++(statistics.*member);
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
                ++statistics.in_flight;
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
                    --statistics.in_flight;
                    ++statistics.presented;
                    is_first = statistics.presented == 1;
                }
                if (is_first && first_presented)
                {
                    try
                    {
                        first_presented();
                    }
                    catch (...)
                    {
                        std::lock_guard<std::mutex> lock{mutex};
                        ++statistics.lifecycle_callback_failures;
                        presentation_failure = std::current_exception();
                        stopping = true;
                        if (pending)
                        {
                            ++statistics.cancelled;
                            pending.reset();
                        }
                        wakeup.notify_one();
                        return;
                    }
                }
            }
            catch (...)
            {
                auto const submit_end = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock{mutex};
                    statistics.submit_us.add(static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(submit_end - submit_start).count()));
                    --statistics.in_flight;
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
