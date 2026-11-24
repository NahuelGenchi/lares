// SPDX-FileCopyrightText: 2026 Nahuel Genchi
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/* lares-share.exe's main loop (issue #109): what GLib's main context is to
 * src/share.c. Everything the Share model touches runs on it; other threads
 * post work to it. */
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>

namespace lares {

class Loop {
public:
    using Fn = std::function<void()>;
    using Clock = std::chrono::steady_clock;

    /* Runs `fn` on the loop's thread soon; any thread. */
    void post(Fn fn)
    {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }

    /* Runs `fn` once after `ms`; returns an ID for cancel, never 0. Loop's
     * thread only. */
    unsigned after(unsigned ms, Fn fn)
    {
        unsigned id = ++next_id_;
        timers_.emplace(id, Timer{ Clock::now() + std::chrono::milliseconds(ms), std::move(fn) });
        return id;
    }

    /* Cancels a timer and zeroes the ID, as g_clear_handle_id does. */
    void cancel(unsigned& id)
    {
        if (id) timers_.erase(id);
        id = 0;
    }

    void quit()
    {
        post([this] { running_ = false; });
    }

    void run()
    {
        running_ = true;
        while (running_) {
            Fn fn;
            {
                std::unique_lock<std::mutex> lock(mu_);
                auto due = next_due();
                if (queue_.empty()) {
                    if (due == Clock::time_point::max()) cv_.wait(lock, [this] { return !queue_.empty(); });
                    else cv_.wait_until(lock, due, [this] { return !queue_.empty(); });
                }
                if (!queue_.empty()) {
                    fn = std::move(queue_.front());
                    queue_.pop_front();
                }
            }
            if (fn) {
                fn();
                continue;
            }
            fire_due();
        }
    }

    /* Runs what's queued and due now, without waiting: for a loop that is
     * winding down. */
    void run_pending()
    {
        for (;;) {
            Fn fn;
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (queue_.empty()) break;
                fn = std::move(queue_.front());
                queue_.pop_front();
            }
            fn();
        }
        fire_due();
    }

private:
    struct Timer {
        Clock::time_point at;
        Fn fn;
    };

    Clock::time_point next_due() const
    {
        auto t = Clock::time_point::max();
        for (const auto& [id, timer] : timers_)
            if (timer.at < t) t = timer.at;
        return t;
    }

    void fire_due()
    {
        auto now = Clock::now();
        for (auto it = timers_.begin(); it != timers_.end();) {
            if (it->second.at <= now) {
                Fn fn = std::move(it->second.fn);
                it = timers_.erase(it);
                fn();
                /* A timer may have changed the set: start over. */
                return fire_due();
            }
            ++it;
        }
    }

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Fn> queue_;
    std::map<unsigned, Timer> timers_;
    unsigned next_id_ = 0;
    bool running_ = false;
};

} // namespace lares
