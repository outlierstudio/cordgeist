// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Verifies the AOAHID_CHECK_SERIALIZATION debugging aid: it ends the process
 * when a second thread enters a Context another thread is inside, and stays
 * silent for separate Contexts and for nested calls on one thread. */
#include "aoahid.h"
#include "api/internal.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

// Every wait is an argument of the call and has no default; these are the tests' own choices.
inline constexpr std::uint32_t kControlTimeoutMs = 20U;
inline constexpr std::uint32_t kSendTimeoutMs = 20U;
inline constexpr std::uint32_t kDrainTimeoutMs = 20U;

namespace {

aoahid_context* make_context() noexcept {
    aoahid_context_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.event_mode = AOAHID_EVENT_CALLER_POLL;
    options.log_level = AOAHID_LOG_DISABLED;
    aoahid_context* context = nullptr;
    if (aoahid_context_create(&options, &context) != AOAHID_OK) {
        std::fprintf(stderr, "context_create failed\n");
        return nullptr;
    }
    return context;
}

// Holds `context` as if inside a call on it, on another thread, until told to go.
struct Holder {
    explicit Holder(aoahid_context* context) {
        thread = std::thread([this, context] {
            const aoa::detail::SerializationCheck inside(context, "test.holder");
            held.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        while (!held.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    ~Holder() {
        release.store(true, std::memory_order_release);
        thread.join();
    }
    std::atomic<bool> held{false};
    std::atomic<bool> release{false};
    std::thread thread;
};

int run_ok() {
    aoahid_context* first = make_context();
    aoahid_context* second = make_context();
    if (first == nullptr || second == nullptr) {
        return 1;
    }
    {
        // Another thread inside a different Context is no violation.
        const Holder other(second);
        static_cast<void>(aoahid_context_poll(first, 0U));
    }
    {
        // A call made from inside a call on the same thread is no violation.
        const aoa::detail::SerializationCheck outer(first, "test.outer");
        static_cast<void>(aoahid_context_poll(first, 0U));
    }
    // The claim is given up afterwards: another thread may call now.
    std::thread later([first] { static_cast<void>(aoahid_context_poll(first, 0U)); });
    later.join();
    const bool destroyed =
        aoahid_context_destroy(first, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK &&
        aoahid_context_destroy(second, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK;
    return destroyed ? 0 : 1;
}

int run_violation() {
    aoahid_context* context = make_context();
    if (context == nullptr) {
        return 1;
    }
    const Holder other(context);
    static_cast<void>(aoahid_context_poll(context, 0U));
    // Not reached when the check works.
    std::fprintf(stderr, "the call returned\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "ok") == 0) {
        return run_ok();
    }
    if (argc == 2 && std::strcmp(argv[1], "violation") == 0) {
        return run_violation();
    }
    std::fprintf(stderr, "usage: %s ok|violation\n", argc > 0 ? argv[0] : "test");
    return 2;
}
