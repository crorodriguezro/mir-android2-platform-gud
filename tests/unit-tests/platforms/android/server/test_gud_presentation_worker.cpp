#include "src/platforms/android/server/gud_presentation_worker.h"
#include "src/platforms/android/server/gud_hwc_boundary.h"
#include "src/platforms/android/server/gud_mode_selection.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace mga = mir::graphics::android;
using namespace std::chrono;

TEST(GudHwcBoundary, excludes_only_synthetic_external_from_android_hwc)
{
    EXPECT_TRUE(mga::should_submit_to_android_hwc(false, mga::DisplayName::primary));
    EXPECT_TRUE(mga::should_submit_to_android_hwc(false, mga::DisplayName::external));
    EXPECT_TRUE(mga::should_submit_to_android_hwc(true, mga::DisplayName::primary));
    EXPECT_FALSE(mga::should_submit_to_android_hwc(true, mga::DisplayName::external));
    EXPECT_TRUE(mga::should_submit_to_android_hwc(true, mga::DisplayName::virt));

    EXPECT_TRUE(mga::should_arm_android_hwc_acquire_fence(true, mga::DisplayName::primary));
    EXPECT_FALSE(mga::should_arm_android_hwc_acquire_fence(true, mga::DisplayName::external));
    EXPECT_TRUE(mga::should_arm_android_hwc_acquire_fence(true, mga::DisplayName::virt));
}

TEST(GudModeSelection, prefers_the_advertised_preferred_mode)
{
    mga::GudModeCandidate const modes[] = {
        {1280, 720, 0, false},
        {1920, 1080, 1, true},
    };

    auto const* selected = mga::select_startup_gud_mode(modes, 2);

    ASSERT_NE(nullptr, selected);
    EXPECT_EQ(1u, selected->index);
}

TEST(GudModeSelection, uses_the_first_usable_mode_without_a_preference)
{
    mga::GudModeCandidate const modes[] = {
        {0, 720, 0, true},
        {1920, 1080, 1, false},
        {1280, 720, 2, false},
    };

    auto const* selected = mga::select_startup_gud_mode(modes, 3);

    ASSERT_NE(nullptr, selected);
    EXPECT_EQ(1u, selected->index);
}

TEST(GudPresentationWorker, coalesces_pending_frames_to_the_newest)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool first_started{false};
    bool allow_first_to_finish{false};
    std::vector<int> presented;
    mga::LatestPresentationWorker<int> worker{
        [&](int const& frame)
        {
            std::unique_lock<std::mutex> lock{mutex};
            presented.push_back(frame);
            if (frame == 1)
            {
                first_started = true;
                changed.notify_all();
                changed.wait(lock, [&] { return allow_first_to_finish; });
            }
            changed.notify_all();
        },
        [](std::exception_ptr) { FAIL() << "the test presenter must not fail"; }};

    worker.submit(1);
    bool started = false;
    {
        std::unique_lock<std::mutex> lock{mutex};
        started = changed.wait_for(lock, seconds{1}, [&] { return first_started; });
    }
    if (!started)
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            allow_first_to_finish = true;
        }
        changed.notify_all();
        worker.stop();
        FAIL() << "first frame was not presented";
    }
    worker.submit(2);
    worker.submit(3);
    {
        std::lock_guard<std::mutex> lock{mutex};
        allow_first_to_finish = true;
    }
    changed.notify_all();
    {
        std::unique_lock<std::mutex> lock{mutex};
        ASSERT_TRUE(changed.wait_for(lock, seconds{1}, [&]
        {
            return presented.size() == 2 && worker.statistics().completed == 2;
        }));
        EXPECT_EQ((std::vector<int>{1, 3}), presented);
    }
    auto const stats = worker.statistics();
    EXPECT_EQ(3u, stats.submitted);
    EXPECT_EQ(1u, stats.coalesced);
    EXPECT_EQ(2u, stats.started);
    EXPECT_EQ(2u, stats.completed);
    EXPECT_EQ(0u, stats.failed);
    EXPECT_FALSE(stats.active);
    EXPECT_FALSE(stats.pending);
    worker.stop();
}

TEST(GudPresentationWorker, submit_does_not_wait_for_a_slow_present)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool presenting{false};
    bool release{false};
    mga::LatestPresentationWorker<int> worker{
        [&](int const&)
        {
            std::unique_lock<std::mutex> lock{mutex};
            presenting = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        },
        [](std::exception_ptr) { FAIL() << "the test presenter must not fail"; }};

    worker.submit(1);
    bool started = false;
    {
        std::unique_lock<std::mutex> lock{mutex};
        started = changed.wait_for(lock, seconds{1}, [&] { return presenting; });
    }
    if (!started)
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            release = true;
        }
        changed.notify_all();
        worker.stop();
        FAIL() << "slow presenter did not start";
    }

    auto const start = steady_clock::now();
    worker.submit(2);
    EXPECT_LT(duration_cast<milliseconds>(steady_clock::now() - start), milliseconds{100});

    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    changed.notify_all();
    worker.stop();
}

TEST(GudPresentationWorker, retains_active_frame_and_releases_superseded_pending_frame)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool presenting{false};
    bool release{false};
    mga::LatestPresentationWorker<std::shared_ptr<int>> worker{
        [&](std::shared_ptr<int> const&)
        {
            std::unique_lock<std::mutex> lock{mutex};
            presenting = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        },
        [](std::exception_ptr) { FAIL() << "the test presenter must not fail"; }};

    auto active = std::make_shared<int>(1);
    std::weak_ptr<int> active_weak{active};
    worker.submit(active);
    active.reset();
    bool started = false;
    {
        std::unique_lock<std::mutex> lock{mutex};
        started = changed.wait_for(lock, seconds{1}, [&] { return presenting; });
    }
    if (!started)
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            release = true;
        }
        changed.notify_all();
        worker.stop();
        FAIL() << "active frame was not presented";
    }
    EXPECT_FALSE(active_weak.expired());

    auto superseded = std::make_shared<int>(2);
    std::weak_ptr<int> superseded_weak{superseded};
    worker.submit(superseded);
    superseded.reset();
    worker.submit(std::make_shared<int>(3));
    EXPECT_TRUE(superseded_weak.expired());

    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    changed.notify_all();
    worker.stop();
    EXPECT_TRUE(active_weak.expired());
}

TEST(GudPresentationWorker, contains_presentation_errors_and_continues)
{
    std::promise<void> second_frame_presented;
    std::promise<void> failure_reported;
    mga::LatestPresentationWorker<int> worker{
        [&](int const& frame)
        {
            if (frame == 1)
                throw std::runtime_error{"synthetic GUD I/O error"};
            second_frame_presented.set_value();
        },
        [&](std::exception_ptr error)
        {
            try
            {
                std::rethrow_exception(error);
            }
            catch (std::runtime_error const&)
            {
                failure_reported.set_value();
            }
        }};

    worker.submit(1);
    ASSERT_EQ(std::future_status::ready,
              failure_reported.get_future().wait_for(seconds{1}));
    worker.submit(2);
    EXPECT_EQ(std::future_status::ready,
              second_frame_presented.get_future().wait_for(seconds{1}));
    auto const stats = worker.statistics();
    EXPECT_EQ(2u, stats.submitted);
    EXPECT_EQ(2u, stats.started);
    EXPECT_EQ(1u, stats.completed);
    EXPECT_EQ(1u, stats.failed);
    EXPECT_FALSE(stats.active);
    worker.stop();
}

TEST(GudPresentationWorker, shutdown_discards_pending_frame_and_joins_active_work)
{
    std::mutex mutex;
    std::condition_variable changed;
    bool first_started{false};
    bool release{false};
    std::vector<int> presented;
    mga::LatestPresentationWorker<int> worker{
        [&](int const& frame)
        {
            std::unique_lock<std::mutex> lock{mutex};
            presented.push_back(frame);
            first_started = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        },
        [](std::exception_ptr) { FAIL() << "the test presenter must not fail"; }};

    worker.submit(1);
    bool started = false;
    {
        std::unique_lock<std::mutex> lock{mutex};
        started = changed.wait_for(lock, seconds{1}, [&] { return first_started; });
    }
    if (!started)
    {
        {
            std::lock_guard<std::mutex> lock{mutex};
            release = true;
        }
        changed.notify_all();
        worker.stop();
        FAIL() << "first frame was not presented";
    }
    worker.submit(2);
    auto stopped = std::async(std::launch::async, [&] { worker.stop(); });
    EXPECT_EQ(std::future_status::timeout, stopped.wait_for(milliseconds{20}));
    {
        std::lock_guard<std::mutex> lock{mutex};
        release = true;
    }
    changed.notify_all();
    EXPECT_EQ(std::future_status::ready, stopped.wait_for(seconds{1}));
    EXPECT_EQ((std::vector<int>{1}), presented);
}
