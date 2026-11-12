// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Verifies internal-event-thread completion, teardown, and synchronization
 * without making any claim about physical Android hardware. */
#include "aoahid.h"
#include "api/internal.hpp"
#include "fake_libusb/libusb.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Every wait is an argument of the call and has no default; these are the tests' own choices.
inline constexpr std::uint32_t kControlTimeoutMs = 100U;
inline constexpr std::uint32_t kSendTimeoutMs = 100U;
inline constexpr std::uint32_t kDrainTimeoutMs = 1000U;

namespace {

bool require_ok(const aoahid_result result, const char* operation) noexcept {
    if (result == AOAHID_OK) {
        return true;
    }
    const aoahid_error_detail* detail = aoahid_last_error();
    std::fprintf(stderr, "%s failed: result=%s field=%s reason=%s\n", operation,
                 aoahid_result_name(result),
                 detail != nullptr && detail->field != nullptr ? detail->field : "",
                 detail != nullptr && detail->reason != nullptr ? detail->reason : "");
    return false;
}

aoahid_device_options make_device_options() noexcept {
    aoahid_device_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.descriptor_fragment_bytes = 64U;
    options.transfer_pool_slots = 2U;
    options.maximum_report_bytes = 64U;
    options.validate_reports = 1U;
    options.aoa_descriptor_wire_policy_bytes = 1024U;
    options.linux_hid.descriptor_bytes = 1024U;
    options.linux_hid.fields_per_report = 256U;
    options.linux_hid.global_stack_depth = 4U;
    options.linux_hid.usages = 12288U;
    options.linux_hid.report_data_bits = 65528U;
    options.linux_hid.report_size_bits = 256U;
    options.target_ep0_data_policy_bytes = 4096U;
    options.host_control_buffer_policy_bytes = 4096U;
    options.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    options.interface_number = -1;
    return options;
}

bool inject_terminal_event_thread_failure(aoahid_context* context) noexcept {
    aoa::detail::DeferredError failure{};
    failure.transport = aoa::transport::ErrorInfo{AOAHID_ERR_INTERNAL, 0, 0, 0U, 0U, 0U};
    failure.field = "context.event_pump";
    failure.reason = "Injected terminal event-thread failure for deterministic teardown testing.";
    failure.pending = true;
    {
        const std::lock_guard<std::mutex> guard(context->mutex);
        context->event_thread_error = failure;
        if (!context->deferred_error.pending)
            context->deferred_error = failure;
    }
    context->event_thread_failed.store(true, std::memory_order_release);
    context->stop_event_thread.store(true, std::memory_order_release);
    context->runtime->interrupt_event_handler();
    context->graveyard_cv.notify_all();
    std::unique_lock<std::mutex> lock(context->mutex);
    static_cast<void>(
        context->graveyard_cv.wait_for(lock, std::chrono::seconds(5U), [context]() noexcept {
            return context->event_thread_exited.load(std::memory_order_acquire);
        }));
    return context->event_thread_exited.load(std::memory_order_acquire) &&
           context->event_thread.joinable();
}

} // namespace

int main() {
    aoahid_fake_libusb_reset();

    // libusb 1.0.30 retains a user interrupt that arrives just before an
    // event handler starts. Model and verify that race explicitly so a stop
    // cannot be hidden behind the long fallback timeout.
    aoa::transport::Runtime* prewait_runtime = nullptr;
    aoa::transport::Runtime* other_runtime = nullptr;
    if (!require_ok(aoa::transport::Runtime::create(AOAHID_EVENT_INTERNAL_THREAD, &prewait_runtime),
                    "prewait_runtime_create") ||
        !require_ok(aoa::transport::Runtime::create(AOAHID_EVENT_INTERNAL_THREAD, &other_runtime),
                    "other_runtime_create")) {
        return 1;
    }
    aoahid_fake_libusb_event_stats before_prewait_interrupt{};
    if (aoahid_fake_libusb_get_event_stats(&before_prewait_interrupt) != LIBUSB_SUCCESS) {
        std::fputs("failed to read pre-interrupt event statistics\n", stderr);
        return 1;
    }
    prewait_runtime->interrupt_event_handler();
    if (!require_ok(other_runtime->poll(0U), "other_runtime_poll")) {
        return 1;
    }
    aoahid_fake_libusb_event_stats after_other_context_poll{};
    if (aoahid_fake_libusb_get_event_stats(&after_other_context_poll) != LIBUSB_SUCCESS ||
        after_other_context_poll.interrupt_wakeups != before_prewait_interrupt.interrupt_wakeups) {
        std::fputs("event-handler interrupt leaked into a different libusb Context\n", stderr);
        return 1;
    }
    if (!require_ok(prewait_runtime->poll(100U), "prewait_interrupt_poll")) {
        return 1;
    }
    aoahid_fake_libusb_event_stats after_prewait_interrupt{};
    if (aoahid_fake_libusb_get_event_stats(&after_prewait_interrupt) != LIBUSB_SUCCESS ||
        after_prewait_interrupt.interrupt_calls != before_prewait_interrupt.interrupt_calls + 1U ||
        after_prewait_interrupt.interrupt_wakeups !=
            before_prewait_interrupt.interrupt_wakeups + 1U ||
        after_prewait_interrupt.waits_started != before_prewait_interrupt.waits_started ||
        after_prewait_interrupt.timeout_wakeups != before_prewait_interrupt.timeout_wakeups ||
        after_prewait_interrupt.active_waiters != 0U) {
        std::fputs("pre-wait event-handler interrupt was not retained and consumed\n", stderr);
        return 1;
    }
    delete other_runtime;
    delete prewait_runtime;
    aoahid_fake_libusb_reset();

    static constexpr std::array<std::uint8_t, 2U> port_path{4U, 2U};
    const aoahid_fake_libusb_device_config fake{
        3U,      9U, port_path.data(),         port_path.size(), 0x18D1U,
        0x2D00U, 2U, "internal-thread-target", "Android",        LIBUSB_SUCCESS};
    if (aoahid_fake_libusb_add_device(&fake) < 0) {
        std::fputs("failed to add fake target\n", stderr);
        return 1;
    }

    aoahid_context_options context_options{};
    context_options.struct_size = static_cast<std::uint32_t>(sizeof(context_options));
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    context_options.log_level = AOAHID_LOG_DISABLED;
    aoahid_context* context = nullptr;
    aoahid_fake_libusb_queue_event_result(LIBUSB_ERROR_IO);
    if (!require_ok(aoahid_context_create(&context_options, &context), "context_create")) {
        return 1;
    }
    if (context->active_state_mutex != &context->mutex) {
        std::fputs("internal-thread Context did not enable event-state synchronization\n", stderr);
        return 1;
    }

    // Seeing a second backend call proves the event thread returned from the
    // injected first failure, latched it, rate-limited that failure path, and
    // continued pumping.
    if (aoahid_fake_libusb_wait_for_event_calls(2U, 5'000U) != LIBUSB_SUCCESS) {
        std::fputs("internal event thread did not continue after injected error\n", stderr);
        return 1;
    }
    if (aoahid_fake_libusb_wait_for_event_waiters(1U, 5'000U) != LIBUSB_SUCCESS) {
        std::fputs("internal event thread did not enter its bounded idle wait\n", stderr);
        return 1;
    }
    aoahid_fake_libusb_event_stats idle_stats{};
    if (aoahid_fake_libusb_get_event_stats(&idle_stats) != LIBUSB_SUCCESS ||
        idle_stats.handle_calls < 2U || idle_stats.zero_timeout_calls != 0U ||
        idle_stats.requested_timeout_us != idle_stats.handle_calls * 60'000'000U ||
        idle_stats.waits_started + 1U != idle_stats.handle_calls ||
        idle_stats.successful_submits != 0U || idle_stats.callbacks_dispatched != 0U) {
        std::fputs("idle internal event thread did not use the bounded low-wakeup pump path\n",
                   stderr);
        return 1;
    }
    aoahid_discovery* deferred_discovery = nullptr;
    if (aoahid_discover(context, 100U, &deferred_discovery) != AOAHID_ERR_IO) {
        std::fputs("internal event failure was not delivered to caller thread\n", stderr);
        return 1;
    }
    const aoahid_error_detail* event_error = aoahid_last_error();
    if (event_error == nullptr || event_error->transport_status != LIBUSB_ERROR_IO ||
        event_error->field == nullptr ||
        std::strcmp(event_error->field, "context.event_pump") != 0) {
        std::fputs("internal event failure metadata was not retained exactly\n", stderr);
        return 1;
    }
    if (!require_ok(aoahid_discover(context, 100U, &deferred_discovery),
                    "discover_after_event_error")) {
        return 1;
    }
    aoahid_discovery_destroy(deferred_discovery);

    const aoahid_device_info selected{
        fake.bus,        fake.address, fake.port_path, fake.port_path_length, fake.vendor_id,
        fake.product_id, 0U,           fake.serial,    fake.product};
    aoahid_device_options device_options = make_device_options();
    aoahid_device* device = nullptr;
    if (!require_ok(aoahid_device_open(context, &selected, &device_options, &device),
                    "device_open")) {
        return 1;
    }

    aoahid_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 3U;
    mouse.x = {-127, 127, 8U, {}};
    mouse.y = {-127, 127, 8U, {}};
    mouse.enable_wheel = 1U;
    mouse.wheel = {-127, 127, 8U, {}};
    mouse.enable_pan = 1U;
    mouse.pan = {-127, 127, 8U, {}};
    aoahid_spec* spec = nullptr;
    if (!require_ok(aoahid_spec_create_mouse(&mouse, &spec), "spec_create_mouse")) {
        return 1;
    }

    const aoahid_node_options node_options{static_cast<std::uint32_t>(sizeof(aoahid_node_options)),
                                           0U, 1U, 0U};
    // Race terminal callbacks against standalone Node deletion repeatedly.
    // The callback publishes transfer_inflight=false under completion_mutex;
    // Node destruction takes that mutex as a callback-exit barrier.
    for (std::size_t iteration = 0U; iteration < 128U; ++iteration) {
        aoahid_node* node = nullptr;
        if (!require_ok(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node),
                        "node_open")) {
            return 1;
        }
        if (node->active_completion_mutex != &node->completion_mutex) {
            std::fputs("internal-thread Node did not enable completion synchronization\n", stderr);
            return 1;
        }
        if (!require_ok(aoahid_mouse_move(node, 17, -9), "mouse_move") ||
            !require_ok(aoahid_mouse_scroll(node, 1, -1), "mouse_scroll") ||
            !require_ok(aoahid_node_submit(node, kSendTimeoutMs), "node_submit") ||
            !require_ok(aoahid_node_close(node, kControlTimeoutMs, kDrainTimeoutMs),
                        "node_close")) {
            return 1;
        }
    }
    aoahid_spec_release(spec);

    const aoahid_result first_device_close =
        aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs);
    // The transport may still be leaving the Node's callback, in which case the Context
    // graveyard finishes the teardown; the call returns AOAHID_OK either way.
    if (!require_ok(first_device_close, "device_close")) {
        return 1;
    }
    if (!require_ok(
            aoahid_context_destroy_blocking(context, kControlTimeoutMs, kDrainTimeoutMs, 1000U),
            "context_destroy_blocking")) {
        return 1;
    }
    if (aoahid_fake_libusb_pending_transfer_count() != 0U) {
        std::fputs("internal-thread test left a pending transfer\n", stderr);
        return 1;
    }

    // Stopping an idle Context must interrupt the active long wait rather than
    // waiting for its one-minute safety bound. Fake counters make the wake
    // cause deterministic without relying on a scheduler-sensitive benchmark.
    aoahid_context* teardown_context = nullptr;
    if (!require_ok(aoahid_context_create(&context_options, &teardown_context),
                    "teardown_context_create") ||
        aoahid_fake_libusb_wait_for_event_waiters(1U, 5'000U) != LIBUSB_SUCCESS) {
        std::fputs("teardown Context did not reach the idle event wait\n", stderr);
        return 1;
    }
    aoahid_fake_libusb_event_stats before_teardown{};
    if (aoahid_fake_libusb_get_event_stats(&before_teardown) != LIBUSB_SUCCESS) {
        std::fputs("failed to read pre-teardown event statistics\n", stderr);
        return 1;
    }
    if (!require_ok(aoahid_context_destroy_blocking(teardown_context, kControlTimeoutMs,
                                                    kDrainTimeoutMs, 2'000U),
                    "teardown_context_destroy")) {
        return 1;
    }
    aoahid_fake_libusb_event_stats after_teardown{};
    if (aoahid_fake_libusb_get_event_stats(&after_teardown) != LIBUSB_SUCCESS ||
        after_teardown.interrupt_calls != before_teardown.interrupt_calls + 1U ||
        after_teardown.interrupt_wakeups != before_teardown.interrupt_wakeups + 1U ||
        after_teardown.timeout_wakeups != before_teardown.timeout_wakeups ||
        after_teardown.active_waiters != 0U) {
        std::fputs("idle Context teardown did not interrupt and leave the long wait\n", stderr);
        return 1;
    }

    // A terminal event-thread failure must not strand a cancelled transfer in
    // the Context graveyard. Once the failed thread has exited (but remains
    // joinable), blocking destroy joins it and becomes the sole libusb pumper.
    aoahid_fake_libusb_reset();
    if (aoahid_fake_libusb_add_device(&fake) < 0) {
        std::fputs("failed to add fallback target\n", stderr);
        return 1;
    }
    context = nullptr;
    if (!require_ok(aoahid_context_create(&context_options, &context), "fallback_context_create")) {
        return 1;
    }
    device = nullptr;
    if (!require_ok(aoahid_device_open(context, &selected, &device_options, &device),
                    "fallback_device_open")) {
        return 1;
    }
    static constexpr std::array<std::uint8_t, 21U> raw_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x25U,
        0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report raw_report{{1U, 1U, {}}, 2U};
    const aoahid_raw_options raw_options{sizeof(aoahid_raw_options),
                                         0U,
                                         raw_descriptor.data(),
                                         raw_descriptor.size(),
                                         &raw_report,
                                         1U,
                                         1U,
                                         0U,
                                         0U};
    spec = nullptr;
    if (!require_ok(aoahid_spec_create_raw(&raw_options, &spec), "fallback_raw_spec")) {
        return 1;
    }
    aoahid_node* delayed_node = nullptr;
    if (!require_ok(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &delayed_node),
                    "fallback_node_open")) {
        return 1;
    }
    aoahid_spec_release(spec);
    static constexpr std::array<std::uint8_t, 2U> raw_payload{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,
                                              static_cast<int>(raw_payload.size()), 1'000U);
    if (!require_ok(
            aoahid_raw_submit(delayed_node, raw_payload.data(), raw_payload.size(), kSendTimeoutMs),
            "fallback_raw_submit")) {
        return 1;
    }
    const std::size_t polls_before_recoverable_error = aoahid_fake_libusb_event_handle_count();
    aoahid_fake_libusb_queue_event_result(LIBUSB_ERROR_IO);
    if (aoahid_fake_libusb_wait_for_event_calls(polls_before_recoverable_error + 2U, 5'000U) !=
        LIBUSB_SUCCESS) {
        std::fputs("recoverable event error was not latched before terminal failure\n", stderr);
        return 1;
    }
    if (!inject_terminal_event_thread_failure(context)) {
        std::fputs("failed event thread did not exit while retaining a join handle\n", stderr);
        return 1;
    }
    if (aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) != AOAHID_OK ||
        aoahid_fake_libusb_pending_transfer_count() == 0U) {
        std::fputs("failed-pump Device did not move to the graveyard\n", stderr);
        return 1;
    }
    const std::size_t polls_before_fallback = aoahid_fake_libusb_event_handle_count();
    if (aoahid_context_destroy_blocking(context, kControlTimeoutMs, kDrainTimeoutMs, 1000U) !=
        AOAHID_ERR_INTERNAL) {
        std::fputs("blocking destroy did not return the original terminal pump error\n", stderr);
        return 1;
    }
    const aoahid_error_detail* terminal_error = aoahid_last_error();
    if (terminal_error == nullptr || terminal_error->field == nullptr ||
        std::strcmp(terminal_error->field, "context.event_pump") != 0 ||
        aoahid_fake_libusb_event_handle_count() <= polls_before_fallback ||
        aoahid_fake_libusb_pending_transfer_count() != 0U) {
        std::fputs("caller fallback did not poll, drain, and preserve terminal metadata\n", stderr);
        return 1;
    }
    aoahid_fake_libusb_reset();
    return 0;
}
