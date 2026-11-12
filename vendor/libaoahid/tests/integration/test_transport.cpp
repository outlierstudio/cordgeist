// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * This suite checks the exact AOA control sequence and transfer lifecycle at
 * the transport boundary. The deterministic backend contains no HID policy.
 */

#include "api/internal.hpp"
#include "fake_libusb/libusb.h"
#include "test.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

// Every wait is an argument of the call and has no default; these are the tests' own choices.
inline constexpr std::uint32_t kControlTimeoutMs = 20U;
inline constexpr std::uint32_t kSendTimeoutMs = 20U;
inline constexpr std::uint32_t kDrainTimeoutMs = 1U;

namespace allocation_probe {

std::atomic<bool> active{false};
std::atomic<std::size_t> count{0U};

void record() noexcept {
    if (active.load(std::memory_order_relaxed)) {
        count.fetch_add(1U, std::memory_order_relaxed);
    }
}

void* allocate(const std::size_t requested) {
    record();
    if (void* storage = std::malloc(requested == 0U ? 1U : requested); storage != nullptr) {
        return storage;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(const std::size_t requested, const std::size_t alignment) {
    record();
#if defined(_MSC_VER)
    if (void* storage = _aligned_malloc(requested == 0U ? 1U : requested, alignment);
        storage != nullptr) {
        return storage;
    }
#else
    if (requested > std::numeric_limits<std::size_t>::max() - (alignment - 1U)) {
        throw std::bad_alloc();
    }
    const std::size_t adjusted =
        std::max(alignment, ((requested + alignment - 1U) / alignment) * alignment);
    if (void* storage = std::aligned_alloc(alignment, adjusted); storage != nullptr) {
        return storage;
    }
#endif
    throw std::bad_alloc();
}

void deallocate_aligned(void* storage) noexcept {
#if defined(_MSC_VER)
    _aligned_free(storage);
#else
    std::free(storage);
#endif
}

void begin() noexcept {
    count.store(0U, std::memory_order_relaxed);
    active.store(true, std::memory_order_release);
}

std::size_t finish() noexcept {
    active.store(false, std::memory_order_release);
    return count.load(std::memory_order_relaxed);
}

} // namespace allocation_probe

void* operator new(const std::size_t size) { return allocation_probe::allocate(size); }
void* operator new[](const std::size_t size) { return allocation_probe::allocate(size); }
void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* storage) noexcept { std::free(storage); }
void operator delete[](void* storage) noexcept { std::free(storage); }
void operator delete(void* storage, std::size_t) noexcept { std::free(storage); }
void operator delete[](void* storage, std::size_t) noexcept { std::free(storage); }
void operator delete(void* storage, const std::nothrow_t&) noexcept { std::free(storage); }
void operator delete[](void* storage, const std::nothrow_t&) noexcept { std::free(storage); }

#if defined(__cpp_aligned_new)
void* operator new(const std::size_t size, const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](const std::size_t size, const std::align_val_t alignment) {
    return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
}
void* operator new(const std::size_t size, const std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](const std::size_t size, const std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    try {
        return allocation_probe::allocate_aligned(size, static_cast<std::size_t>(alignment));
    } catch (...) {
        return nullptr;
    }
}
void operator delete(void* storage, std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
void operator delete[](void* storage, std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
void operator delete(void* storage, std::size_t, std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
void operator delete[](void* storage, std::size_t, std::align_val_t) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
void operator delete(void* storage, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
void operator delete[](void* storage, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_probe::deallocate_aligned(storage);
}
#endif

namespace {

using aoa::transport::Candidate;
using aoa::transport::Device;
using aoa::transport::DeviceConfig;
using aoa::transport::Port;
using aoa::transport::PreparedTransfer;
using aoa::transport::Runtime;

struct CompletionState {
    std::size_t calls{};
    aoahid_result result{AOAHID_OK};
    std::int32_t native_status{};
};

struct BlockingCompletionState {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<aoahid_result> result{AOAHID_OK};
};

void completed(void* user, const aoahid_result result, const std::int32_t native_status) noexcept {
    auto* state = static_cast<CompletionState*>(user);
    if (state != nullptr) {
        ++state->calls;
        state->result = result;
        state->native_status = native_status;
    }
}

void blocking_completed(void* user, const aoahid_result result,
                        const std::int32_t native_status) noexcept {
    static_cast<void>(native_status);
    auto* state = static_cast<BlockingCompletionState*>(user);
    if (state == nullptr) {
        return;
    }
    state->result.store(result, std::memory_order_relaxed);
    state->entered.store(true, std::memory_order_release);
    state->entered.notify_all();
    state->release.wait(false, std::memory_order_acquire);
}

DeviceConfig current_mode_config() {
    DeviceConfig config{};
    config.event_mode = AOAHID_EVENT_CALLER_POLL;
    config.descriptor_fragment_bytes = 64U;
    config.pool_slots = 2U;
    config.maximum_report_bytes = 64U;
    config.claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    config.interface_number = -1;
    return config;
}

Candidate add_candidate(const std::uint8_t address, const std::array<std::uint8_t, 2U>& path,
                        const std::uint16_t vendor = 0x18D1U,
                        const std::uint16_t product = 0x2D00U) {
    const aoahid_fake_libusb_device_config fake{1U,        address,       path.data(), path.size(),
                                                vendor,    product,       2U,          "serial",
                                                "Android", LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&fake) >= 0);
    Candidate candidate{};
    candidate.bus = fake.bus;
    candidate.address = fake.address;
    candidate.port_path.assign(path.begin(), path.end());
    candidate.vendor_id = fake.vendor_id;
    candidate.product_id = fake.product_id;
    candidate.serial = fake.serial;
    candidate.product = fake.product;
    return candidate;
}

std::vector<aoahid_fake_libusb_control_record> controls_for(const std::uint8_t request) {
    std::vector<aoahid_fake_libusb_control_record> records;
    const std::size_t count = aoahid_fake_libusb_control_count();
    for (std::size_t index = 0U; index < count; ++index) {
        aoahid_fake_libusb_control_record record{};
        AOAHID_CHECK(aoahid_fake_libusb_get_control(index, &record) == LIBUSB_SUCCESS);
        if (record.request == request) {
            records.push_back(record);
        }
    }
    return records;
}

aoahid_fake_libusb_event_stats event_stats() {
    aoahid_fake_libusb_event_stats stats{};
    AOAHID_CHECK(aoahid_fake_libusb_get_event_stats(&stats) == LIBUSB_SUCCESS);
    return stats;
}

std::vector<std::vector<std::uint8_t>> control_payloads_for(const std::uint8_t request) {
    std::vector<std::vector<std::uint8_t>> payloads;
    const std::size_t count = aoahid_fake_libusb_control_count();
    for (std::size_t index = 0U; index < count; ++index) {
        aoahid_fake_libusb_control_record record{};
        AOAHID_CHECK(aoahid_fake_libusb_get_control(index, &record) == LIBUSB_SUCCESS);
        if (record.request != request)
            continue;
        std::vector<std::uint8_t> payload(record.length);
        AOAHID_CHECK(aoahid_fake_libusb_copy_control_data(index, payload.data(), payload.size()) ==
                     payload.size());
        payloads.push_back(std::move(payload));
    }
    return payloads;
}

std::uint64_t extract_report_value(const std::vector<std::uint8_t>& report,
                                   const aoa::hid::FieldLayout& field) {
    std::uint64_t value = 0U;
    for (std::uint8_t bit = 0U; bit < field.bit_width; ++bit) {
        const std::size_t position = field.bit_offset + bit;
        const std::uint64_t byte = report[position / 8U];
        value |= ((byte >> (position & 7U)) & std::uint64_t{1}) << bit;
    }
    return value;
}

const aoa::hid::FieldLayout* profile_field(const aoahid_spec* spec,
                                           const aoa::hid::FieldSemantic semantic,
                                           const std::uint16_t instance = 0U) {
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == semantic && candidate.instance == instance)
            return &candidate;
    }
    return nullptr;
}

void poll_until(Runtime* runtime, CompletionState* state) {
    for (std::size_t attempt = 0U; attempt < 20U && state->calls == 0U; ++attempt) {
        AOAHID_CHECK(runtime->poll(1U) == AOAHID_OK);
    }
    AOAHID_CHECK(state->calls == 1U);
}

void test_runtime_initialization_options() {
    aoahid_fake_libusb_reset();
    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_init_context_count() == 1U);
    AOAHID_CHECK(aoahid_fake_libusb_init_option_count() == 2U);

    aoahid_fake_libusb_init_option_record log_level{};
    aoahid_fake_libusb_init_option_record log_callback{};
    AOAHID_CHECK(aoahid_fake_libusb_get_init_option(0U, &log_level) == LIBUSB_SUCCESS);
    AOAHID_CHECK(aoahid_fake_libusb_get_init_option(1U, &log_callback) == LIBUSB_SUCCESS);
    AOAHID_CHECK(log_level.option == LIBUSB_OPTION_LOG_LEVEL);
    AOAHID_CHECK(log_level.integer_value == LIBUSB_LOG_LEVEL_NONE);
    AOAHID_CHECK(log_level.callback_present == 0);
    AOAHID_CHECK(log_callback.option == LIBUSB_OPTION_LOG_CB);
    AOAHID_CHECK(log_callback.callback_present == 1);

    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_registration_reservations_and_async_results() {
    aoahid_fake_libusb_reset();
    const auto candidate = add_candidate(3U, {2U, 4U});

    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    Device* device = nullptr;
    DeviceConfig config = current_mode_config();
    config.pool_slots = 3U;
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    std::array<std::uint8_t, 130U> descriptor{};
    for (std::size_t index = 0U; index < descriptor.size(); ++index) {
        descriptor[index] = static_cast<std::uint8_t>(index);
    }
    AOAHID_CHECK(device->register_hid(7U, descriptor.data(), descriptor.size(),
                                      kControlTimeoutMs) == AOAHID_OK);
    const auto registrations = controls_for(54U);
    AOAHID_CHECK(registrations.size() == 1U);
    if (!registrations.empty()) {
        AOAHID_CHECK(registrations[0].request_type == 0x40U && registrations[0].value == 7U &&
                     registrations[0].index == descriptor.size() && registrations[0].length == 0U &&
                     registrations[0].asynchronous == 0);
    }
    const auto fragments = controls_for(56U);
    AOAHID_CHECK(fragments.size() == 3U);
    if (fragments.size() == 3U) {
        AOAHID_CHECK(fragments[0].value == 7U && fragments[0].index == 0U &&
                     fragments[0].length == 64U);
        AOAHID_CHECK(fragments[1].value == 7U && fragments[1].index == 64U &&
                     fragments[1].length == 64U);
        AOAHID_CHECK(fragments[2].value == 7U && fragments[2].index == 128U &&
                     fragments[2].length == 2U);
        AOAHID_CHECK(fragments[0].request_type == 0x40U && fragments[0].asynchronous == 0);
    }
    const auto descriptor_payloads = control_payloads_for(56U);
    AOAHID_CHECK(descriptor_payloads.size() == 3U);
    if (descriptor_payloads.size() == 3U) {
        AOAHID_CHECK(std::equal(descriptor_payloads[0].begin(), descriptor_payloads[0].end(),
                                descriptor.begin()));
        AOAHID_CHECK(std::equal(descriptor_payloads[1].begin(), descriptor_payloads[1].end(),
                                descriptor.begin() + 64));
        AOAHID_CHECK(std::equal(descriptor_payloads[2].begin(), descriptor_payloads[2].end(),
                                descriptor.begin() + 128));
    }

    aoa::transport::ReservationToken reservation7{};
    aoa::transport::ReservationToken reservation8{};
    aoa::transport::ReservationToken reservation9{};
    AOAHID_CHECK(device->set_reservation(7U, 1U, &reservation7) == AOAHID_OK);
    AOAHID_CHECK(device->set_reservation(8U, 1U, &reservation8) == AOAHID_OK);
    CompletionState first{};
    CompletionState second{};
    CompletionState shared{};
    PreparedTransfer prepared_first{};
    PreparedTransfer prepared_second{};
    PreparedTransfer prepared_shared{};
    AOAHID_CHECK(device->acquire(9U, {}, 3U, &shared, &completed, &prepared_shared) == AOAHID_OK);
    AOAHID_CHECK(device->acquire(9U, {}, 3U, &first, &completed, &prepared_first) ==
                 AOAHID_ERR_BUSY);
    AOAHID_CHECK(device->acquire(7U, reservation7, 3U, &first, &completed, &prepared_first) ==
                 AOAHID_OK);
    AOAHID_CHECK(device->clear_reservation(reservation7) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(device->acquire(9U, {}, 3U, &second, &completed, &prepared_second) ==
                 AOAHID_ERR_BUSY);
    AOAHID_CHECK(device->acquire(8U, reservation8, 3U, &second, &completed, &prepared_second) ==
                 AOAHID_OK);
    device->abandon(prepared_shared);
    device->abandon(prepared_first);
    device->abandon(prepared_second);
    AOAHID_CHECK(device->clear_reservation(reservation7) == AOAHID_OK);
    AOAHID_CHECK(device->clear_reservation(reservation8) == AOAHID_OK);

    AOAHID_CHECK(device->set_reservation(7U, 0U, &reservation7) == AOAHID_OK);
    AOAHID_CHECK(device->set_reservation(8U, 2U, &reservation8) == AOAHID_OK);
    AOAHID_CHECK(device->set_reservation(9U, 1U, &reservation9) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(device->clear_reservation(reservation7) == AOAHID_OK);
    AOAHID_CHECK(device->clear_reservation(reservation8) == AOAHID_OK);

    // Since 2.0.0 a STALL, even on a Node's first report, is returned as is:
    // exactly one request 57 goes out and the caller decides what to do.
    CompletionState stalled{};
    PreparedTransfer stalled_transfer{};
    AOAHID_CHECK(device->acquire(7U, {}, 3U, &stalled, &completed, &stalled_transfer) == AOAHID_OK);
    stalled_transfer.payload[0] = 1U;
    stalled_transfer.payload[1] = 2U;
    stalled_transfer.payload[2] = 3U;
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL, 0, 0U);
    AOAHID_CHECK(device->submit(stalled_transfer, 3U, 11U, kSendTimeoutMs) == AOAHID_OK);
    poll_until(runtime, &stalled);
    AOAHID_CHECK(stalled.result == AOAHID_ERR_STALL);
    const auto stalled_events = controls_for(57U);
    AOAHID_CHECK(stalled_events.size() == 1U);
    for (const aoahid_fake_libusb_control_record event : stalled_events) {
        AOAHID_CHECK(event.request_type == 0x40U && event.value == 7U && event.index == 0U &&
                     event.length == 3U && event.asynchronous == 1);
    }
    const auto stalled_payloads = control_payloads_for(57U);
    AOAHID_CHECK(stalled_payloads.size() == 1U);
    for (const auto& payload : stalled_payloads) {
        AOAHID_CHECK(payload == std::vector<std::uint8_t>({1U, 2U, 3U}));
    }
    AOAHID_CHECK(device->latched_error() == AOAHID_ERR_STALL);

    CompletionState short_transfer{};
    PreparedTransfer short_prepared{};
    AOAHID_CHECK(device->acquire(7U, {}, 3U, &short_transfer, &completed, &short_prepared) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 2, 0U);
    AOAHID_CHECK(device->submit(short_prepared, 3U, 23U, kSendTimeoutMs) == AOAHID_OK);
    poll_until(runtime, &short_transfer);
    AOAHID_CHECK(short_transfer.result == AOAHID_ERR_SHORT_TRANSFER);
    aoa::transport::ErrorInfo short_error{};
    AOAHID_CHECK(device->latched_error(&short_error) == AOAHID_ERR_SHORT_TRANSFER);
    AOAHID_CHECK(short_error.hid_id == 7U && short_error.report_id == 23U);
    AOAHID_CHECK(device->latched_error() == AOAHID_OK);

    CompletionState overflow{};
    PreparedTransfer overflow_prepared{};
    AOAHID_CHECK(device->acquire(7U, {}, 3U, &overflow, &completed, &overflow_prepared) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_OVERFLOW, 0, 0U);
    AOAHID_CHECK(device->submit(overflow_prepared, 3U, 29U, kSendTimeoutMs) == AOAHID_OK);
    poll_until(runtime, &overflow);
    AOAHID_CHECK(overflow.result == AOAHID_ERR_OVERFLOW);
    AOAHID_CHECK(overflow.native_status == LIBUSB_TRANSFER_OVERFLOW);
    aoa::transport::ErrorInfo overflow_error{};
    AOAHID_CHECK(device->latched_error(&overflow_error) == AOAHID_ERR_OVERFLOW);
    AOAHID_CHECK(overflow_error.hid_id == 7U && overflow_error.report_id == 29U);
    AOAHID_CHECK(overflow_error.length == 3U);
    AOAHID_CHECK(device->latched_error() == AOAHID_OK);

    CompletionState removed{};
    PreparedTransfer removed_prepared{};
    AOAHID_CHECK(device->acquire(7U, {}, 2U, &removed, &completed, &removed_prepared) == AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_NO_DEVICE, 0, 0U);
    AOAHID_CHECK(device->submit(removed_prepared, 2U, 47U, kSendTimeoutMs) == AOAHID_OK);
    poll_until(runtime, &removed);
    AOAHID_CHECK(removed.result == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(!device->present());
    PreparedTransfer rejected{};
    AOAHID_CHECK(device->acquire(7U, {}, 1U, &removed, &completed, &rejected) ==
                 AOAHID_ERR_NO_DEVICE);
    aoa::transport::ErrorInfo removed_error{};
    AOAHID_CHECK(device->latched_error(&removed_error) == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(removed_error.hid_id == 7U && removed_error.report_id == 47U);
    AOAHID_CHECK(device->latched_error(&removed_error) == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(removed_error.report_id == 47U);

    delete device;
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_uncertain_register_attempts_unregister_and_preserves_error() {
    aoahid_fake_libusb_reset();
    const auto candidate = add_candidate(4U, {2U, 5U});

    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    Device* device = nullptr;
    const DeviceConfig config = current_mode_config();
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    static constexpr std::array<std::uint8_t, 3U> descriptor{0x05U, 0x01U, 0xC0U};
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_TIMEOUT);
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(device->register_hid(19U, descriptor.data(), descriptor.size(),
                                      kControlTimeoutMs) == AOAHID_ERR_TIMEOUT);

    const auto registrations = controls_for(54U);
    const auto cleanups = controls_for(55U);
    AOAHID_CHECK(registrations.size() == 1U);
    AOAHID_CHECK(cleanups.size() == 1U);
    if (!cleanups.empty()) {
        AOAHID_CHECK(cleanups[0].value == 19U && cleanups[0].index == 0U &&
                     cleanups[0].length == 0U);
    }
    const aoa::transport::ErrorInfo failure = aoa::transport::last_error();
    AOAHID_CHECK(failure.result == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(failure.native_status == LIBUSB_ERROR_TIMEOUT);
    AOAHID_CHECK(failure.aoa_request == 54 && failure.hid_id == 19U);
    AOAHID_CHECK(failure.length == descriptor.size());

    struct RegisterFailureCase {
        int native_status;
        aoahid_result expected;
        std::uint16_t hid_id;
    };
    static constexpr std::array<RegisterFailureCase, 2U> ambiguous_failures{
        RegisterFailureCase{LIBUSB_ERROR_IO, AOAHID_ERR_IO, 20U},
        RegisterFailureCase{1, AOAHID_ERR_SHORT_TRANSFER, 21U}};
    for (const RegisterFailureCase attempt : ambiguous_failures) {
        const std::size_t cleanup_count = controls_for(55U).size();
        aoahid_fake_libusb_queue_control_result(attempt.native_status);
        aoahid_fake_libusb_queue_control_result(LIBUSB_SUCCESS);
        AOAHID_CHECK(device->register_hid(attempt.hid_id, descriptor.data(), descriptor.size(),
                                          kControlTimeoutMs) == attempt.expected);
        AOAHID_CHECK(controls_for(55U).size() == cleanup_count + 1U);
        const aoa::transport::ErrorInfo attempt_error = aoa::transport::last_error();
        AOAHID_CHECK(attempt_error.result == attempt.expected);
        AOAHID_CHECK(attempt_error.native_status == attempt.native_status);
        AOAHID_CHECK(attempt_error.aoa_request == 54 && attempt_error.hid_id == attempt.hid_id);
    }

    // A setup STALL is an explicit target rejection, unlike timeout/I/O/short
    // completion ambiguity, so there is no possibly-live request-54 entry to
    // clean up.
    const std::size_t cleanup_count = controls_for(55U).size();
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(device->register_hid(22U, descriptor.data(), descriptor.size(),
                                      kControlTimeoutMs) == AOAHID_ERR_STALL);
    AOAHID_CHECK(controls_for(55U).size() == cleanup_count);

    // On disconnect the audited Android gadget destroys every HID registration;
    // request 55 cannot be delivered and is unnecessary.
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_NO_DEVICE);
    AOAHID_CHECK(device->register_hid(24U, descriptor.data(), descriptor.size(),
                                      kControlTimeoutMs) == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(controls_for(55U).size() == cleanup_count);
    AOAHID_CHECK(!device->present());

    delete device;
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_descriptor_failure_unregisters_and_preserves_error() {
    aoahid_fake_libusb_reset();
    const auto candidate = add_candidate(12U, {2U, 6U});

    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    Device* device = nullptr;
    const DeviceConfig config = current_mode_config();
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    std::array<std::uint8_t, 130U> descriptor{};
    aoahid_fake_libusb_queue_control_result(LIBUSB_SUCCESS); // request 54
    aoahid_fake_libusb_queue_control_result(64);             // request 56, offset 0
    aoahid_fake_libusb_queue_control_result(1);              // request 56, offset 64
    // The cleanup result must not replace the partial request-56 diagnostic.
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(device->register_hid(23U, descriptor.data(), descriptor.size(),
                                      kControlTimeoutMs) == AOAHID_ERR_SHORT_TRANSFER);

    const auto fragments = controls_for(56U);
    AOAHID_CHECK(fragments.size() == 2U);
    if (fragments.size() == 2U) {
        AOAHID_CHECK(fragments[0].value == 23U && fragments[0].index == 0U &&
                     fragments[0].length == 64U);
        AOAHID_CHECK(fragments[1].value == 23U && fragments[1].index == 64U &&
                     fragments[1].length == 64U);
    }
    const auto cleanups = controls_for(55U);
    AOAHID_CHECK(cleanups.size() == 1U);
    if (!cleanups.empty()) {
        AOAHID_CHECK(cleanups[0].value == 23U);
    }
    const aoa::transport::ErrorInfo failure = aoa::transport::last_error();
    AOAHID_CHECK(failure.result == AOAHID_ERR_SHORT_TRANSFER);
    AOAHID_CHECK(failure.native_status == 1);
    AOAHID_CHECK(failure.aoa_request == 56 && failure.hid_id == 23U);
    AOAHID_CHECK(failure.offset == 64U && failure.length == 64U);

    delete device;
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_error_report_id_is_published_before_user_callback_returns() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(10U, {3U, 8U});
    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_INTERNAL_THREAD, &runtime) == AOAHID_OK);
    DeviceConfig config = current_mode_config();
    config.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    Device* device = nullptr;
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    BlockingCompletionState completion{};
    PreparedTransfer prepared{};
    AOAHID_CHECK(device->acquire(37U, {}, 2U, &completion, &blocking_completed, &prepared) ==
                 AOAHID_OK);
    prepared.payload[0] = 91U;
    prepared.payload[1] = 1U;
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_TIMED_OUT, 0, 0U);
    AOAHID_CHECK(device->submit(prepared, 2U, 91U, kSendTimeoutMs) == AOAHID_OK);

    std::atomic<aoahid_result> poll_result{AOAHID_ERR_INTERNAL};
    std::thread event_thread(
        [&] { poll_result.store(runtime->poll(100U), std::memory_order_release); });
    completion.entered.wait(false, std::memory_order_acquire);

    aoa::transport::ErrorInfo error{};
    AOAHID_CHECK(device->latched_error(&error) == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(error.hid_id == 37U && error.report_id == 91U);
    AOAHID_CHECK(error.length == 2U);

    completion.release.store(true, std::memory_order_release);
    completion.release.notify_all();
    event_thread.join();
    AOAHID_CHECK(poll_result.load(std::memory_order_acquire) == AOAHID_OK);
    AOAHID_CHECK(completion.result.load(std::memory_order_relaxed) == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(device->drained());

    delete device;
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_current_mode_open_and_explicit_claim() {
    aoahid_fake_libusb_reset();
    const auto candidate = add_candidate(5U, {1U, 6U}, 0x1234U, 0x5678U);

    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    DeviceConfig config = current_mode_config();
    config.claim_policy = AOAHID_INTERFACE_CLAIM_EXPLICIT;
    config.interface_number = 7;

    Device* device = nullptr;
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    AOAHID_CHECK(aoahid_fake_libusb_control_count() == 0U);

    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 1U);
    aoahid_fake_libusb_claim_record claim{};
    AOAHID_CHECK(aoahid_fake_libusb_get_claim(0U, &claim) == LIBUSB_SUCCESS);
    AOAHID_CHECK(claim.interface_number == 7 && claim.release == 0);
    delete device;
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 2U);
    AOAHID_CHECK(aoahid_fake_libusb_get_claim(1U, &claim) == LIBUSB_SUCCESS);
    AOAHID_CHECK(claim.interface_number == 7 && claim.release == 1);
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_port_ownership_and_discovery_reuse() {
    aoahid_fake_libusb_reset();
    const auto candidate = add_candidate(5U, {1U, 6U}, 0x1234U, 0x5678U);

    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_INTERNAL_THREAD, &runtime) == AOAHID_OK);

    // Each Device open owns one handle through its own Port.
    DeviceConfig config = current_mode_config();
    config.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    Device* first = nullptr;
    Device* second = nullptr;
    AOAHID_CHECK(Device::open(runtime, candidate, config, &first) == AOAHID_OK);
    AOAHID_CHECK(Device::open(runtime, candidate, config, &second) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 2U);
    AOAHID_CHECK(first->port() != nullptr && first->port() != second->port());

    // Discovery probes an open device through its existing handle.
    const std::size_t opens_before = aoahid_fake_libusb_open_call_count();
    std::vector<Candidate> found;
    AOAHID_CHECK(runtime->discover(20U, &found) == AOAHID_OK);
    AOAHID_CHECK(found.size() == 1U);
    AOAHID_CHECK(aoahid_fake_libusb_open_call_count() == opens_before);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 2U);
    delete first;
    delete second;
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    found.clear();
    AOAHID_CHECK(runtime->discover(20U, &found) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_call_count() == opens_before + 1U);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);

    // A Port closes only with its last reference; claims are counted.
    Port* port = nullptr;
    AOAHID_CHECK(runtime->acquire_port(candidate, &port) == AOAHID_OK);
    AOAHID_CHECK(port != nullptr && port->references() == 1U);
    port->retain();
    AOAHID_CHECK(port->claim_interface(3) == LIBUSB_SUCCESS);
    AOAHID_CHECK(port->claim_interface(3) == LIBUSB_SUCCESS);
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 1U);
    port->release_interface(3);
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 1U);
    port->release_interface(3);
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 2U);
    aoahid_fake_libusb_claim_record claim{};
    AOAHID_CHECK(aoahid_fake_libusb_get_claim(1U, &claim) == LIBUSB_SUCCESS);
    AOAHID_CHECK(claim.interface_number == 3 && claim.release == 1);
    // A claim still held at the last release is released before close.
    AOAHID_CHECK(port->claim_interface(4) == LIBUSB_SUCCESS);
    port->release();
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 1U);
    port->release();
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 4U);
    AOAHID_CHECK(aoahid_fake_libusb_get_claim(3U, &claim) == LIBUSB_SUCCESS);
    AOAHID_CHECK(claim.interface_number == 4 && claim.release == 1);

    // Opening an absent device leaves nothing behind.
    aoahid_fake_libusb_set_present(0U, 0);
    Port* missing = nullptr;
    AOAHID_CHECK(runtime->acquire_port(candidate, &missing) == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(missing == nullptr && aoahid_fake_libusb_open_handle_count() == 0U);

    delete runtime;
    aoahid_fake_libusb_reset();
}

aoahid_context_options public_context_options();
aoahid_device_options public_device_options();

aoahid_device_info public_info(const Candidate& candidate) {
    return aoahid_device_info{candidate.bus,
                              candidate.address,
                              candidate.port_path.data(),
                              candidate.port_path.size(),
                              candidate.vendor_id,
                              candidate.product_id,
                              0U,
                              candidate.serial.c_str(),
                              candidate.product.c_str()};
}

constexpr std::uint32_t kAccessoryControlTimeoutMs = 500U;

aoahid_accessory_options accessory_options(const char* manufacturer, const char* model) {
    aoahid_accessory_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.strings.manufacturer = manufacturer;
    options.strings.model = model;
    return options;
}

std::vector<std::uint8_t> nul_terminated(const char* text) {
    return {reinterpret_cast<const std::uint8_t*>(text),
            reinterpret_cast<const std::uint8_t*>(text) + std::char_traits<char>::length(text) +
                1U};
}

void test_accessory_start_sends_51_52_53_and_closes() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_candidate(4U, {2U, 3U}, 0x04E8U, 0x6860U);
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);

    aoahid_accessory_options options = accessory_options("Maker", "Model");
    options.strings.version = "1.0";
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(controls_for(51U).size() == 1U);
    const auto strings = controls_for(52U);
    const auto payloads = control_payloads_for(52U);
    // Only non-null strings are sent, each with its AOA string ID and NUL.
    AOAHID_CHECK(strings.size() == 3U && payloads.size() == 3U);
    if (strings.size() == 3U && payloads.size() == 3U) {
        AOAHID_CHECK(strings[0].index == 0U && payloads[0] == nul_terminated("Maker"));
        AOAHID_CHECK(strings[1].index == 1U && payloads[1] == nul_terminated("Model"));
        AOAHID_CHECK(strings[2].index == 3U && payloads[2] == nul_terminated("1.0"));
        for (const aoahid_fake_libusb_control_record& record : strings) {
            AOAHID_CHECK(record.request_type == 0x40U && record.value == 0U &&
                         record.timeout == 500U && record.asynchronous == 0);
        }
    }
    const auto start = controls_for(53U);
    AOAHID_CHECK(start.size() == 1U);
    if (!start.empty()) {
        AOAHID_CHECK(start[0].request_type == 0x40U && start[0].value == 0U &&
                     start[0].index == 0U && start[0].length == 0U);
    }
    // Nothing is left open and nothing waits for re-enumeration.
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    AOAHID_CHECK(controls_for(54U).empty());

    // The phone re-enumerates as an accessory on the same port; the ordinary
    // discover/open/node sequence takes over from here.
    aoahid_fake_libusb_unplug(0U);
    const Candidate accessory = add_candidate(9U, {2U, 3U}, 0x18D1U, 0x2D00U);
    aoahid_discovery* discovery = nullptr;
    AOAHID_CHECK(aoahid_discover(context, 0U, &discovery) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(discovery == nullptr);
    AOAHID_CHECK(aoahid_discover(context, 100U, &discovery) == AOAHID_OK);
    AOAHID_CHECK(aoahid_discovery_count(discovery) == 1U);
    const aoahid_device_info* found = aoahid_discovery_get(discovery, 0U);
    AOAHID_CHECK(found != nullptr && found->vendor_id == 0x18D1U && found->product_id == 0x2D00U &&
                 found->port_path_length == 2U && found->port_path[0] == 2U &&
                 found->protocol_version == 2U);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(found != nullptr &&
                 aoahid_device_open(context, found, &device_options, &device) == AOAHID_OK);
    aoahid_discovery_destroy(discovery);
    // A second start on an accessory-mode device sends nothing.
    const std::size_t before = aoahid_fake_libusb_control_count();
    const aoahid_device_info accessory_info = public_info(accessory);
    AOAHID_CHECK(aoahid_accessory_start(context, &accessory_info, &options,
                                        kAccessoryControlTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_control_count() == before);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_accessory_start_rejects_and_reports_failures() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_candidate(4U, {2U, 3U}, 0x04E8U, 0x6860U);
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);

    aoahid_accessory_options options = accessory_options("Maker", nullptr);
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_UNSET_FIELD);
    options = accessory_options("", "Model");
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_UNSET_FIELD);
    options = accessory_options("Maker", "Model");
    options.reserved = 1U;
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_UNSET_FIELD);
    options = accessory_options("Maker", "Model");
    options.struct_size -= 1U;
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_PARAM);
    options = accessory_options("Maker", "Model");
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, 0U) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_accessory_start(nullptr, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_accessory_start(context, nullptr, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, nullptr, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_PARAM);
    // AOA 1.0 caps each string at 256 bytes with its NUL; an oversized one is
    // rejected before any request, and exactly 256 bytes is accepted later.
    const std::string longest(255U, 'x');
    const std::string too_long(256U, 'x');
    options = accessory_options("Maker", "Model");
    options.strings.description = too_long.c_str();
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_OVERFLOW);
    const aoahid_error_detail* overflow = aoahid_last_error();
    AOAHID_CHECK(overflow != nullptr && overflow->aoa_request == 52 && overflow->offset == 2U);
    AOAHID_CHECK(aoahid_fake_libusb_control_count() == 0U);
    options = accessory_options(longest.c_str(), "Model");
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_OK);
    const auto longest_payloads = control_payloads_for(52U);
    AOAHID_CHECK(!longest_payloads.empty() && longest_payloads[0].size() == 256U);
    aoahid_fake_libusb_reset();
    static_cast<void>(add_candidate(4U, {2U, 3U}, 0x04E8U, 0x6860U));
    options = accessory_options("Maker", "Model");

    // A device without AOA support gets neither strings nor START.
    aoahid_fake_libusb_set_protocol(0U, 0U);
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_NOT_AOA);
    AOAHID_CHECK(controls_for(52U).empty() && controls_for(53U).empty());
    aoahid_fake_libusb_set_protocol(0U, 2U);

    // A failed string request stops the sequence; nothing is retried.
    aoahid_fake_libusb_queue_control_result(2);
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_ERR_STALL);
    const aoahid_error_detail* detail = aoahid_last_error();
    AOAHID_CHECK(detail != nullptr && detail->aoa_request == 52 && detail->offset == 0U);
    AOAHID_CHECK(controls_for(52U).size() == 1U && controls_for(53U).empty());
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);

    // An open Device lends its handle instead of a second libusb_open.
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);
    const std::size_t opens = aoahid_fake_libusb_open_call_count();
    AOAHID_CHECK(aoahid_accessory_start(context, &selected, &options, kAccessoryControlTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_call_count() == opens);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 1U);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

constexpr std::uint8_t kAdbEndpointIn = 0x81U;
constexpr std::uint8_t kAdbEndpointOut = 0x01U;
constexpr std::uint16_t kAdbMaxPacket = 512U;

// An Android device exposing the ADB interface (AOSP adb.h: 0xFF/0x42/0x01).
Candidate add_adb_phone() {
    const Candidate phone = add_candidate(4U, {2U, 3U}, 0x18D1U, 0x4EE7U);
    const aoahid_fake_libusb_bulk_interface adb{
        1U, 0xFFU, 0x42U, 0x01U, kAdbEndpointIn, kAdbEndpointOut, kAdbMaxPacket};
    aoahid_fake_libusb_add_bulk_interface(0U, &adb);
    return phone;
}

// The ADB interface read as a stream of one-packet transfers, so a test sees each packet.
aoahid_channel_options adb_channel_options() {
    aoahid_channel_options options;
    aoahid_channel_options_init(&options);
    options.read_mode = AOAHID_CHANNEL_READ_STREAM;
    options.in_transfers = 2U;
    options.transfer_bytes = 512U;
    return options;
}

bool wait_until_true(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!ready()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

void test_channel_shares_the_device_handle_internal_thread() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_adb_phone();
    aoahid_context_options context_options = public_context_options();
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);
    const std::size_t opens = aoahid_fake_libusb_open_call_count();

    aoahid_channel* channel = nullptr;
    aoahid_channel_options options = adb_channel_options();
    options.interface_subclass = 0xFFU;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) ==
                 AOAHID_ERR_UNSUPPORTED);
    AOAHID_CHECK(channel == nullptr);
    options = adb_channel_options();
    options.zero_length_termination = 2U;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) ==
                 AOAHID_ERR_UNSET_FIELD);
    options = adb_channel_options();
    options.reserved8 = 1U;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) ==
                 AOAHID_ERR_UNSET_FIELD);
    options = adb_channel_options();
    options.struct_size += 1U;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) ==
                 AOAHID_ERR_PARAM);
    options = adb_channel_options();
    AOAHID_CHECK(aoahid_channel_open(nullptr, &options, kDrainTimeoutMs, &channel) ==
                 AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) == AOAHID_OK);
    // An already configured device gets no SET_CONFIGURATION (it would act as
    // a lightweight reset).
    AOAHID_CHECK(aoahid_fake_libusb_set_configuration_count() == 0U);
    // One handle for HID and Bulk; the Bulk interface is claimed on it.
    AOAHID_CHECK(aoahid_fake_libusb_open_call_count() == opens);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 1U);
    aoahid_fake_libusb_claim_record claim{};
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 1U &&
                 aoahid_fake_libusb_get_claim(0U, &claim) == LIBUSB_SUCCESS &&
                 claim.interface_number == 1 && claim.release == 0);

    std::array<std::uint8_t, 64> buffer{};
    std::size_t received = 7U;
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(received == 0U);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 0U, &received, 0U) ==
                 AOAHID_ERR_PARAM);

    // An ADB CNXN message: 24-byte header plus "host::".
    const std::array<std::uint8_t, 30> cnxn{
        'C',  'N',  'X',  'N',  0x01, 0x00, 0x00, 0x01, 0x00, 0x10, 0x00, 0x00, 0x06, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0xBC, 0xB1, 0xA7, 0xB1, 'h',  'o',  's',  't',  ':',  ':'};
    std::size_t written = 0U;
    AOAHID_CHECK(aoahid_channel_write(channel, cnxn.data(), cnxn.size(), &written, 1000U) ==
                 AOAHID_OK);
    AOAHID_CHECK(written == cnxn.size());
    std::array<std::uint8_t, 64> sent{};
    AOAHID_CHECK(aoahid_fake_libusb_bulk_out_count() == 1U &&
                 aoahid_fake_libusb_copy_bulk_out(0U, sent.data(), sent.size()) == cnxn.size() &&
                 std::equal(cnxn.begin(), cnxn.end(), sent.begin()));

    // The reply arrives in two USB transfers; the reader sees one stream.
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, cnxn.data(), 24U);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, cnxn.data() + 24U, cnxn.size() - 24U);
    std::vector<std::uint8_t> stream;
    while (stream.size() < cnxn.size()) {
        std::array<std::uint8_t, 16> part{};
        const aoahid_result read =
            aoahid_channel_read(channel, part.data(), part.size(), &received, 1000U);
        AOAHID_CHECK(read == AOAHID_OK && received != 0U);
        if (read != AOAHID_OK)
            break;
        stream.insert(stream.end(), part.begin(),
                      part.begin() + static_cast<std::ptrdiff_t>(received));
    }
    AOAHID_CHECK(stream == std::vector<std::uint8_t>(cnxn.begin(), cnxn.end()));

    // One max packet exactly: data, then an explicit zero-length packet.
    const std::vector<std::uint8_t> packet(kAdbMaxPacket, 0x5AU);
    AOAHID_CHECK(aoahid_channel_write(channel, packet.data(), packet.size(), &written, 1000U) ==
                 AOAHID_OK);
    AOAHID_CHECK(wait_until_true([]() { return aoahid_fake_libusb_bulk_out_count() == 3U; }));
    AOAHID_CHECK(aoahid_fake_libusb_copy_bulk_out(1U, nullptr, 0U) == kAdbMaxPacket);
    AOAHID_CHECK(aoahid_fake_libusb_copy_bulk_out(2U, nullptr, 0U) == 0U);

    // HID on the same handle keeps working next to the Channel.
    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 0U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 1000U) == AOAHID_OK);

    // Unplugging loses the Channel: it reports NO_DEVICE and must be closed.
    aoahid_fake_libusb_unplug(0U);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 1000U) ==
                 AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_channel_write(channel, cnxn.data(), cnxn.size(), &written, 0U) ==
                 AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_channel_close(channel, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_channel_close(nullptr, kDrainTimeoutMs) == AOAHID_ERR_PARAM);
    static_cast<void>(aoahid_node_close(node, kControlTimeoutMs, kDrainTimeoutMs));
    static_cast<void>(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs));
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    aoahid_fake_libusb_reset();
}

// Request mode: no read-ahead; each read with nothing buffered submits one IN
// transfer sized from its capacity (whole packets, at most transfer_bytes).
void test_channel_request_reads() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_adb_phone();
    aoahid_context_options context_options = public_context_options();
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_channel* channel = nullptr;
    aoahid_channel_options options = adb_channel_options();
    for (const aoahid_channel_read_mode mode : {0, 3}) {
        options.read_mode = mode;
        AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) ==
                     AOAHID_ERR_UNSET_FIELD);
    }
    options = adb_channel_options();
    options.read_mode = AOAHID_CHANNEL_READ_REQUEST;
    options.in_transfers = 7U;      // ignored in request mode
    options.transfer_bytes = 4000U; // rounded up to 4096
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_count() == 0U);

    std::vector<std::uint8_t> buffer(100000U);
    std::size_t received = 0U;
    // A 24-byte header read asks for one whole packet; a timeout keeps it
    // pending instead of submitting a second transfer.
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_count() == 1U &&
                 aoahid_fake_libusb_bulk_in_submit_length(0U) == kAdbMaxPacket);
    std::vector<std::uint8_t> header(24U);
    for (std::size_t index = 0U; index < header.size(); ++index)
        header[index] = static_cast<std::uint8_t>(index + 1U);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, header.data(), header.size());
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 1000U) == AOAHID_OK &&
                 received == 24U && std::equal(header.begin(), header.end(), buffer.begin()));

    // A packet-aligned payload with no zero-length packet: the transfer is
    // exactly the payload length, so it completes when full.
    const std::vector<std::uint8_t> aligned(2U * kAdbMaxPacket, 0xA5U);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, aligned.data(), aligned.size());
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), aligned.size(), &received, 1000U) ==
                     AOAHID_OK &&
                 received == aligned.size());
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_count() == 2U &&
                 aoahid_fake_libusb_bulk_in_submit_length(1U) == static_cast<int>(aligned.size()));

    // A capacity above transfer_bytes is capped at transfer_bytes.
    const std::vector<std::uint8_t> full(4096U, 0x3CU);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, full.data(), full.size());
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 1000U) ==
                     AOAHID_OK &&
                 received == full.size());
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_length(2U) == 4096);

    // Bytes beyond a smaller later capacity stay buffered; no new transfer
    // is submitted until they are consumed.
    std::vector<std::uint8_t> short_data(1000U);
    for (std::size_t index = 0U; index < short_data.size(); ++index)
        short_data[index] = static_cast<std::uint8_t>(index);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, short_data.data(), short_data.size());
    std::vector<std::uint8_t> stream;
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 600U, &received, 1000U) == AOAHID_OK &&
                 received == 600U);
    stream.insert(stream.end(), buffer.begin(), buffer.begin() + 600);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_length(3U) == 1024);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 16U, &received, 0U) == AOAHID_OK &&
                 received == 16U);
    stream.insert(stream.end(), buffer.begin(), buffer.begin() + 16);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 0U) ==
                     AOAHID_OK &&
                 received == 384U);
    stream.insert(stream.end(), buffer.begin(), buffer.begin() + 384);
    AOAHID_CHECK(stream == short_data);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_count() == 4U);

    // A zero-length packet carries no data: the read waits past it.
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, nullptr, 0U);
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, header.data(), header.size());
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 1000U) == AOAHID_OK &&
                 received == 24U);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_in_submit_count() == 6U);

    // Writes are unchanged by the read mode.
    std::size_t written = 0U;
    AOAHID_CHECK(aoahid_channel_write(channel, header.data(), header.size(), &written, 1000U) ==
                     AOAHID_OK &&
                 written == header.size());

    // Unplugging with a request pending loses the Channel.
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    aoahid_fake_libusb_unplug(0U);
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), 24U, &received, 1000U) ==
                 AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_channel_close(channel, kDrainTimeoutMs) == AOAHID_OK);
    static_cast<void>(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs));
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    aoahid_fake_libusb_reset();
}

void test_channel_configures_an_unconfigured_device() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_adb_phone();
    aoahid_fake_libusb_set_active_configuration(0U, 0);
    aoahid_context_options context_options = public_context_options();
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);
    const aoahid_channel_options options = adb_channel_options();
    aoahid_channel* channel = nullptr;
    // AOA 1.0: the host selects configuration 1 before using the endpoints.
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_set_configuration_count() == 1U);
    AOAHID_CHECK(aoahid_channel_close(channel, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_hid_adb_and_accessory_bulk_run_together() {
    aoahid_fake_libusb_reset();
    // AOA 1.0: 0x2D01 has the accessory interface first and ADB second.
    const Candidate phone = add_candidate(9U, {2U, 3U}, 0x18D1U, 0x2D01U);
    const aoahid_fake_libusb_bulk_interface accessory{0U, 0xFFU, 0xFFU, 0x00U, 0x81U, 0x01U, 512U};
    const aoahid_fake_libusb_bulk_interface adb{1U, 0xFFU, 0x42U, 0x01U, 0x82U, 0x02U, 512U};
    aoahid_fake_libusb_add_bulk_interface(0U, &accessory);
    aoahid_fake_libusb_add_bulk_interface(0U, &adb);
    aoahid_context_options context_options = public_context_options();
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 0U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);

    aoahid_channel_options accessory_options = adb_channel_options();
    accessory_options.interface_subclass = 0xFFU;
    accessory_options.interface_protocol = 0x00U;
    aoahid_channel* app = nullptr;
    aoahid_channel* shell = nullptr;
    AOAHID_CHECK(aoahid_channel_open(device, &accessory_options, kDrainTimeoutMs, &app) ==
                 AOAHID_OK);
    const aoahid_channel_options adb_options = adb_channel_options();
    AOAHID_CHECK(aoahid_channel_open(device, &adb_options, kDrainTimeoutMs, &shell) == AOAHID_OK);
    // One handle; interfaces 0 and 1 claimed on it.
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 1U);
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 2U);

    // Each Channel reads on its own thread while HID reports go out.
    const std::array<std::uint8_t, 5> app_data{'A', 'P', 'P', '!', '\n'};
    const std::array<std::uint8_t, 4> adb_data{'O', 'K', 'A', 'Y'};
    std::vector<std::uint8_t> app_stream;
    std::vector<std::uint8_t> adb_stream;
    const auto reader = [](aoahid_channel* channel, std::size_t expected,
                           std::vector<std::uint8_t>* out) {
        while (out->size() < expected) {
            std::array<std::uint8_t, 16> part{};
            std::size_t received = 0U;
            if (aoahid_channel_read(channel, part.data(), part.size(), &received, 2000U) !=
                AOAHID_OK)
                return;
            out->insert(out->end(), part.begin(),
                        part.begin() + static_cast<std::ptrdiff_t>(received));
        }
    };
    std::thread app_reader(reader, app, app_data.size(), &app_stream);
    std::thread adb_reader(reader, shell, adb_data.size(), &adb_stream);
    aoahid_fake_libusb_push_bulk_in(0U, 0x81U, app_data.data(), app_data.size());
    aoahid_fake_libusb_push_bulk_in(0U, 0x82U, adb_data.data(), adb_data.size());
    std::size_t written = 0U;
    AOAHID_CHECK(aoahid_channel_write(app, app_data.data(), app_data.size(), &written, 1000U) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_channel_write(shell, adb_data.data(), adb_data.size(), &written, 1000U) ==
                 AOAHID_OK);
    for (std::uint16_t usage = 0x04U; usage < 0x0CU; ++usage) {
        AOAHID_CHECK(aoahid_keyboard_key(node, usage, 1U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_node_submit_blocking(node, 1000U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_keyboard_key(node, usage, 0U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_node_submit_blocking(node, 1000U) == AOAHID_OK);
    }
    app_reader.join();
    adb_reader.join();
    // Data stays on its own interface: nothing crosses between Channels.
    AOAHID_CHECK(app_stream == std::vector<std::uint8_t>(app_data.begin(), app_data.end()));
    AOAHID_CHECK(adb_stream == std::vector<std::uint8_t>(adb_data.begin(), adb_data.end()));
    AOAHID_CHECK(controls_for(57U).size() == 16U);
    AOAHID_CHECK(aoahid_fake_libusb_bulk_out_count() == 2U);

    AOAHID_CHECK(aoahid_channel_close(app, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    // The Device close also consumes the still-open ADB Channel.
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_channel_caller_poll_and_device_close() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_adb_phone();
    aoahid_context_options context_options = public_context_options();
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);
    const aoahid_channel_options options = adb_channel_options();
    aoahid_channel* channel = nullptr;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) == AOAHID_OK);

    // A waiting read drives libusb events itself in caller-poll mode.
    const std::array<std::uint8_t, 4> okay{'O', 'K', 'A', 'Y'};
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, okay.data(), okay.size());
    std::array<std::uint8_t, 8> buffer{};
    std::size_t received = 0U;
    AOAHID_CHECK(aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 100U) ==
                 AOAHID_OK);
    AOAHID_CHECK(received == okay.size() && std::equal(okay.begin(), okay.end(), buffer.begin()));

    // Without a pump, a zero timeout never waits: two OUT transfers fill the
    // pool and the third chunk reports TIMEOUT with the queued byte count.
    const std::vector<std::uint8_t> data(3U * kAdbMaxPacket + 1U, 0x11U);
    std::size_t written = 0U;
    AOAHID_CHECK(aoahid_channel_write(channel, data.data(), data.size(), &written, 0U) ==
                 AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(written == 2U * kAdbMaxPacket);
    // With a timeout the same call pumps completions and finishes.
    AOAHID_CHECK(aoahid_channel_write(channel, data.data() + written, data.size() - written,
                                      &written, 100U) == AOAHID_OK);
    AOAHID_CHECK(written == kAdbMaxPacket + 1U);

    // Closing the Device consumes the still-open Channel and its handle.
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_open_handle_count() == 0U);
    aoahid_fake_libusb_claim_record claim{};
    AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 2U &&
                 aoahid_fake_libusb_get_claim(1U, &claim) == LIBUSB_SUCCESS &&
                 claim.interface_number == 1 && claim.release == 1);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_open_sends_no_aoa_request() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(6U, {1U, 7U});
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};
    aoahid_device_options options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) == AOAHID_OK);
    AOAHID_CHECK(device != nullptr);
    AOAHID_CHECK(aoahid_fake_libusb_control_count() == 0U);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);

    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_required_device_policies_precede_usb_io() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(6U, {1U, 7U});
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};

    using ClearPolicy = void (*)(aoahid_device_options&);
    const std::array<ClearPolicy, 4U> required_byte_policies{
        [](aoahid_device_options& options) { options.aoa_descriptor_wire_policy_bytes = 0U; },
        [](aoahid_device_options& options) { options.linux_hid.descriptor_bytes = 0U; },
        [](aoahid_device_options& options) { options.target_ep0_data_policy_bytes = 0U; },
        [](aoahid_device_options& options) { options.host_control_buffer_policy_bytes = 0U; }};
    for (const ClearPolicy clear : required_byte_policies) {
        aoahid_device_options rejected = public_device_options();
        clear(rejected);
        aoahid_device* rejected_device = nullptr;
        AOAHID_CHECK(aoahid_device_open(context, &selected, &rejected, &rejected_device) ==
                     AOAHID_ERR_UNSET_FIELD);
        AOAHID_CHECK(rejected_device == nullptr);
        AOAHID_CHECK(aoahid_fake_libusb_control_count() == 0U);
        AOAHID_CHECK(aoahid_fake_libusb_claim_count() == 0U);
        AOAHID_CHECK(aoahid_last_error()->field != nullptr);
        if (aoahid_last_error()->field != nullptr) {
            AOAHID_CHECK(std::string_view(aoahid_last_error()->field) == "device_options");
        }
    }

    aoahid_device* device = nullptr;
    aoahid_device_options options = public_device_options();
    options.linux_hid.fields_per_report = 0U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(device == nullptr && aoahid_fake_libusb_control_count() == 0U);

    options = public_device_options();
    options.linux_hid.usages = 0U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(device == nullptr && aoahid_fake_libusb_control_count() == 0U);

    options = public_device_options();
    options.linux_hid.global_stack_depth = 0U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(device == nullptr && aoahid_fake_libusb_control_count() == 0U);

    options = public_device_options();
    options.linux_hid.report_data_bits = 0U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(device == nullptr && aoahid_fake_libusb_control_count() == 0U);

    options = public_device_options();
    options.linux_hid.report_size_bits = 0U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) ==
                 AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(device == nullptr && aoahid_fake_libusb_control_count() == 0U);

    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_cancel_and_device_isolation() {
    aoahid_fake_libusb_reset();
    const auto first_candidate = add_candidate(2U, {3U, 1U});
    const auto second_candidate = add_candidate(4U, {3U, 2U});
    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_CALLER_POLL, &runtime) == AOAHID_OK);
    DeviceConfig config = current_mode_config();
    Device* first = nullptr;
    Device* second = nullptr;
    AOAHID_CHECK(Device::open(runtime, first_candidate, config, &first) == AOAHID_OK);
    AOAHID_CHECK(Device::open(runtime, second_candidate, config, &second) == AOAHID_OK);

    CompletionState cancelled{};
    CompletionState unaffected{};
    PreparedTransfer first_transfer{};
    PreparedTransfer second_transfer{};
    AOAHID_CHECK(first->acquire(1U, {}, 1U, &cancelled, &completed, &first_transfer) == AOAHID_OK);
    AOAHID_CHECK(second->acquire(1U, {}, 1U, &unaffected, &completed, &second_transfer) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 1, 50U);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 1, 0U);
    AOAHID_CHECK(first->submit(first_transfer, 1U, 0U, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(second->submit(second_transfer, 1U, 0U, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(first->cancel_all() == AOAHID_OK);
    for (std::size_t poll = 0U; poll < 20U && (cancelled.calls == 0U || unaffected.calls == 0U);
         ++poll) {
        AOAHID_CHECK(runtime->poll(1U) == AOAHID_OK);
    }
    AOAHID_CHECK(cancelled.calls == 1U && cancelled.result == AOAHID_ERR_IO);
    AOAHID_CHECK(unaffected.calls == 1U && unaffected.result == AOAHID_OK);
    AOAHID_CHECK(first->drained());
    AOAHID_CHECK(second->drained());
    AOAHID_CHECK(aoahid_fake_libusb_cancel_count() == 1U);

    delete first;
    delete second;
    delete runtime;
    aoahid_fake_libusb_reset();
}

void test_blocking_event_wait_is_woken_by_cancel() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(5U, {3U, 3U});
    Runtime* runtime = nullptr;
    AOAHID_CHECK(Runtime::create(AOAHID_EVENT_INTERNAL_THREAD, &runtime) == AOAHID_OK);
    DeviceConfig config = current_mode_config();
    config.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    Device* device = nullptr;
    AOAHID_CHECK(Device::open(runtime, candidate, config, &device) == AOAHID_OK);

    CompletionState completion{};
    PreparedTransfer prepared{};
    AOAHID_CHECK(device->acquire(1U, {}, 1U, &completion, &completed, &prepared) == AOAHID_OK);
    prepared.payload[0] = 1U;
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 1, 100'000U);
    AOAHID_CHECK(device->submit(prepared, 1U, 0U, kSendTimeoutMs) == AOAHID_OK);

    // The generous timeout is only a deadlock escape, not a performance
    // threshold. The fake backend reports when poll() is actually blocked, so
    // cancellation can be issued at a deterministic synchronization point.
    std::atomic<aoahid_result> poll_result{AOAHID_ERR_INTERNAL};
    std::thread event_thread(
        [&] { poll_result.store(runtime->poll(5'000U), std::memory_order_release); });
    AOAHID_CHECK(aoahid_fake_libusb_wait_for_event_waiters(1U, 5'000U) == LIBUSB_SUCCESS);
    const aoahid_fake_libusb_event_stats waiting = event_stats();
    AOAHID_CHECK(waiting.active_waiters == 1U);
    AOAHID_CHECK(waiting.zero_timeout_calls == 0U);
    AOAHID_CHECK(waiting.requested_timeout_us == 5'000'000U);
    AOAHID_CHECK(waiting.callbacks_dispatched == 0U);

    AOAHID_CHECK(device->cancel_all() == AOAHID_OK);
    event_thread.join();
    const aoahid_fake_libusb_event_stats completed_stats = event_stats();
    AOAHID_CHECK(poll_result.load(std::memory_order_acquire) == AOAHID_OK);
    AOAHID_CHECK(completed_stats.notification_wakeups == waiting.notification_wakeups + 1U);
    AOAHID_CHECK(completed_stats.cancellation_wakeups == waiting.cancellation_wakeups + 1U);
    AOAHID_CHECK(completed_stats.callbacks_dispatched == 1U);
    AOAHID_CHECK(completion.calls == 1U && completion.result == AOAHID_ERR_IO);
    AOAHID_CHECK(device->drained());
    AOAHID_CHECK(aoahid_fake_libusb_cancel_count() == 1U);

    delete device;
    delete runtime;
    aoahid_fake_libusb_reset();
}

aoahid_context_options public_context_options() {
    aoahid_context_options options{};
    options.struct_size = sizeof(options);
    options.event_mode = AOAHID_EVENT_CALLER_POLL;
    options.log_level = AOAHID_LOG_DISABLED;
    return options;
}

aoahid_device_options public_device_options() {
    aoahid_device_options options{};
    options.struct_size = sizeof(options);
    options.descriptor_fragment_bytes = 64U;
    options.transfer_pool_slots = 1U;
    options.maximum_report_bytes = 64U;
    options.validate_reports = 0U;
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

// Only the initializers' values: a Context, a Device and a Node of every profile that has a
// complete initializer, and one report through it.
void test_initializers_open_a_working_session() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(13U, {5U, 3U});
    aoahid_context_options context_options;
    aoahid_context_options_init(&context_options);
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(candidate);
    aoahid_device_options device_options;
    aoahid_device_options_init(&device_options);
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_keyboard_options keyboard;
    aoahid_keyboard_options_init(&keyboard);
    aoahid_mouse_options mouse;
    aoahid_mouse_options_init(&mouse);
    aoahid_gamepad_options gamepad;
    aoahid_gamepad_options_init(&gamepad);
    aoahid_touchscreen_options touchscreen;
    aoahid_touchscreen_options_init(&touchscreen);
    aoahid_touchpad_options touchpad;
    aoahid_touchpad_options_init(&touchpad);
    aoahid_pen_options pen;
    aoahid_pen_options_init(&pen);
    aoahid_digitizer_options digitizer;
    aoahid_digitizer_options_init(&digitizer);
    std::array<aoahid_spec*, 7U> specs{};
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &specs[0]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_mouse(&mouse, &specs[1]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_gamepad(&gamepad, &specs[2]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&touchscreen, &specs[3]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_touchpad(&touchpad, &specs[4]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_pen(&pen, &specs[5]) == AOAHID_OK);
    AOAHID_CHECK(aoahid_spec_create_digitizer(&digitizer, &specs[6]) == AOAHID_OK);

    aoahid_node_options node_options;
    aoahid_node_options_init(&node_options);
    std::array<aoahid_node*, 7U> nodes{};
    for (std::size_t index = 0U; index < specs.size(); ++index) {
        AOAHID_CHECK(aoahid_node_open(device, specs[index], &node_options,
                                      AOAHID_CONTROL_TIMEOUT_MS, &nodes[index]) == AOAHID_OK);
        aoahid_spec_release(specs[index]);
    }
    AOAHID_CHECK(aoahid_keyboard_key(nodes[0], 0x04U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(nodes[0], AOAHID_SEND_TIMEOUT_MS) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(nodes[0], 0x04U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(nodes[0], AOAHID_SEND_TIMEOUT_MS) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy_blocking(context, AOAHID_CONTROL_TIMEOUT_MS,
                                                 AOAHID_DRAIN_TIMEOUT_MS,
                                                 AOAHID_DRAIN_TIMEOUT_MS) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_tuning_values_required_and_zero_reservation() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(12U, {5U, 2U});
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};
    // No tuning value has a default: each zero is refused.
    aoahid_device* device = nullptr;
    for (std::uint32_t aoahid_device_options::*field :
         {&aoahid_device_options::descriptor_fragment_bytes,
          &aoahid_device_options::transfer_pool_slots,
          &aoahid_device_options::maximum_report_bytes}) {
        aoahid_device_options zero = public_device_options();
        zero.*field = 0U;
        AOAHID_CHECK(aoahid_device_open(context, &selected, &zero, &device) ==
                         AOAHID_ERR_UNSET_FIELD &&
                     device == nullptr);
    }

    // The values the test used to get from zero, now given.
    aoahid_device_options options = public_device_options();
    options.transfer_pool_slots = 8U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) == AOAHID_OK);
    AOAHID_CHECK(device != nullptr);
    if (device != nullptr) {
        AOAHID_CHECK(device->config.pool_slots == options.transfer_pool_slots);
    }

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    aoahid_node_options node_options{};
    node_options.struct_size = sizeof(node_options);
    aoahid_node* node = nullptr;
    // A zero wait is refused before anything is sent, and nothing is consumed.
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, 0U, &node) ==
                     AOAHID_ERR_UNSET_FIELD &&
                 node == nullptr);
    AOAHID_CHECK(aoahid_last_error()->field != nullptr &&
                 std::string_view(aoahid_last_error()->field) == "control_timeout_ms");
    AOAHID_CHECK(controls_for(54U).empty());
    // The waits are the call's own: distinct values show which one reaches libusb.
    constexpr std::uint32_t open_control_timeout_ms = 777U;
    constexpr std::uint32_t submit_timeout_ms = 1234U;
    // The wait this test used to give through the options.
    constexpr std::uint32_t drain_timeout_ms = 1000U;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, open_control_timeout_ms, &node) ==
                 AOAHID_OK);
    AOAHID_CHECK(node != nullptr && node->reserved_slots == 0U);
    for (const std::uint8_t request : {std::uint8_t{54U}, std::uint8_t{56U}}) {
        const auto records = controls_for(request);
        AOAHID_CHECK(!records.empty());
        for (const auto& record : records)
            AOAHID_CHECK(record.timeout == open_control_timeout_ms);
    }

    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit(node, 0U) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_node_submit(node, submit_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    const auto reports = controls_for(57U);
    AOAHID_CHECK(!reports.empty() && reports.front().timeout == submit_timeout_ms);

    AOAHID_CHECK(aoahid_node_close(node, 0U, drain_timeout_ms) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, 0U) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_device_close(device, 0U, drain_timeout_ms) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, 0U) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_descriptor_requirements_are_caller_policies() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(13U, {5U, 3U});
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* keyboard_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &keyboard_spec) == AOAHID_OK);
    AOAHID_CHECK(keyboard_spec != nullptr);
    if (keyboard_spec != nullptr) {
        AOAHID_CHECK(keyboard_spec->requirements.maximum_fields_per_report == 2U);
        AOAHID_CHECK(keyboard_spec->requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(keyboard_spec->requirements.maximum_report_size_bits == 6U);
        AOAHID_CHECK(keyboard_spec->requirements.maximum_usages == 98U);
        AOAHID_CHECK(keyboard_spec->requirements.maximum_report_data_bits == 112U);
    }

    std::vector<std::uint8_t> raw_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U,
        0x01U,
        // First field declares 20001 Local Usages without expanding them.
        0x1AU, 0x00U, 0x00U, 0x2AU, 0x20U, 0x4EU, 0x81U, 0x02U};
    raw_descriptor.reserve(raw_descriptor.size() + (256U * 4U) + 1U);
    for (std::uint32_t field_index = 1U; field_index < 257U; ++field_index) {
        raw_descriptor.push_back(0x09U);
        raw_descriptor.push_back(0x01U);
        raw_descriptor.push_back(0x81U);
        raw_descriptor.push_back(0x02U);
    }
    raw_descriptor.push_back(0xC0U);
    const aoahid_raw_report raw_report{{0U, 0U, {}}, 33U};
    const aoahid_raw_options raw_options{sizeof(aoahid_raw_options),
                                         0U,
                                         raw_descriptor.data(),
                                         raw_descriptor.size(),
                                         &raw_report,
                                         1U,
                                         1U,
                                         0U,
                                         0U};
    aoahid_spec* raw_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&raw_options, &raw_spec) == AOAHID_OK);
    AOAHID_CHECK(raw_spec != nullptr);
    if (raw_spec != nullptr) {
        AOAHID_CHECK(raw_spec->requirements.maximum_fields_per_report == 257U);
        AOAHID_CHECK(raw_spec->requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(raw_spec->requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(raw_spec->requirements.maximum_usages == 20001U);
        AOAHID_CHECK(raw_spec->requirements.maximum_report_data_bits == 257U);
    }

    const std::array<std::uint8_t, 29U> stack_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0xA4U, 0xA4U, 0xA4U, 0xA4U,
        0xA4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U, 0x15U, 0x00U, 0x25U, 0x01U,
        0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report stack_report{{0U, 0U, {}}, 1U};
    const aoahid_raw_options stack_options{sizeof(aoahid_raw_options),
                                           0U,
                                           stack_descriptor.data(),
                                           stack_descriptor.size(),
                                           &stack_report,
                                           1U,
                                           1U,
                                           0U,
                                           0U};
    aoahid_spec* stack_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&stack_options, &stack_spec) == AOAHID_OK);
    AOAHID_CHECK(stack_spec != nullptr);
    if (stack_spec != nullptr) {
        AOAHID_CHECK(stack_spec->requirements.maximum_global_stack_depth == 5U);
        AOAHID_CHECK(stack_spec->requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(stack_spec->requirements.maximum_report_data_bits == 1U);
    }

    const std::array<std::uint8_t, 24U> report_size_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x77U, 0x01U, 0x01U, 0x00U, 0x00U, 0x15U,
        0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report report_size_report{{0U, 0U, {}}, 1U};
    const aoahid_raw_options report_size_options{sizeof(aoahid_raw_options),
                                                 0U,
                                                 report_size_descriptor.data(),
                                                 report_size_descriptor.size(),
                                                 &report_size_report,
                                                 1U,
                                                 1U,
                                                 0U,
                                                 0U};
    aoahid_spec* report_size_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&report_size_options, &report_size_spec) == AOAHID_OK);
    AOAHID_CHECK(report_size_spec != nullptr);
    if (report_size_spec != nullptr) {
        AOAHID_CHECK(report_size_spec->requirements.maximum_report_size_bits == 257U);
        AOAHID_CHECK(report_size_spec->requirements.maximum_report_data_bits == 1U);
    }

    const std::array<std::uint8_t, 20U> large_report_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x15U, 0x00U, 0x25U, 0x01U,
        0x75U, 0x20U, 0x96U, 0x00U, 0x08U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report large_report{{0U, 0U, {}}, 8192U};
    const aoahid_raw_options large_report_options{sizeof(aoahid_raw_options),
                                                  0U,
                                                  large_report_descriptor.data(),
                                                  large_report_descriptor.size(),
                                                  &large_report,
                                                  1U,
                                                  1U,
                                                  0U,
                                                  0U};
    aoahid_spec* large_report_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&large_report_options, &large_report_spec) == AOAHID_OK);
    AOAHID_CHECK(large_report_spec != nullptr);
    if (large_report_spec != nullptr) {
        AOAHID_CHECK(large_report_spec->requirements.maximum_report_size_bits == 32U);
        AOAHID_CHECK(large_report_spec->requirements.maximum_report_data_bits == 65536U);
    }

    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    const auto reject_before_registration =
        [&](aoahid_spec* spec, const std::uint32_t fields_policy, const std::uint32_t stack_policy,
            const std::uint64_t usages_policy, const std::uint64_t report_bits_policy,
            const std::uint32_t report_size_policy, const char* expected_field) {
            aoahid_device_options options = public_device_options();
            options.aoa_descriptor_wire_policy_bytes = 4096U;
            options.linux_hid.descriptor_bytes = 4096U;
            options.linux_hid.fields_per_report = fields_policy;
            options.linux_hid.global_stack_depth = stack_policy;
            options.linux_hid.usages = usages_policy;
            options.linux_hid.report_data_bits = report_bits_policy;
            options.linux_hid.report_size_bits = report_size_policy;
            options.maximum_report_bytes = 8192U;
            options.target_ep0_data_policy_bytes = 8192U;
            options.host_control_buffer_policy_bytes = 8192U;
            aoahid_device* device = nullptr;
            AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) == AOAHID_OK);
            AOAHID_CHECK(device != nullptr);
            if (device == nullptr)
                return;
            AOAHID_CHECK(device->linux_hid.fields_per_report == fields_policy);
            AOAHID_CHECK(device->linux_hid.global_stack_depth == stack_policy);
            AOAHID_CHECK(device->linux_hid.usages == usages_policy);
            AOAHID_CHECK(device->linux_hid.report_data_bits == report_bits_policy);
            AOAHID_CHECK(device->linux_hid.report_size_bits == report_size_policy);
            aoahid_node* node = nullptr;
            AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                         AOAHID_ERR_OVERFLOW);
            AOAHID_CHECK(node == nullptr);
            AOAHID_CHECK(aoahid_last_error()->field != nullptr);
            if (aoahid_last_error()->field != nullptr) {
                AOAHID_CHECK(std::string_view(aoahid_last_error()->field) == expected_field);
            }
            AOAHID_CHECK(controls_for(54U).empty());
            AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) ==
                         AOAHID_OK);
        };

    if (keyboard_spec != nullptr) {
        reject_before_registration(keyboard_spec, 1U, 4U, 1000U, 65528U, 256U,
                                   "spec.requirements.maximum_fields_per_report");
        reject_before_registration(keyboard_spec, 10U, 4U, 97U, 65528U, 256U,
                                   "spec.requirements.maximum_usages");
    }
    if (raw_spec != nullptr) {
        reject_before_registration(raw_spec, 256U, 4U, 50000U, 65528U, 256U,
                                   "spec.requirements.maximum_fields_per_report");
        reject_before_registration(raw_spec, 1000U, 4U, 12288U, 65528U, 256U,
                                   "spec.requirements.maximum_usages");
    }

    if (stack_spec != nullptr) {
        reject_before_registration(stack_spec, 256U, 4U, 12288U, 65528U, 256U,
                                   "spec.requirements.maximum_global_stack_depth");
    }
    if (report_size_spec != nullptr) {
        reject_before_registration(report_size_spec, 256U, 4U, 12288U, 65528U, 256U,
                                   "spec.requirements.maximum_report_size_bits");
    }
    if (large_report_spec != nullptr) {
        reject_before_registration(large_report_spec, 256U, 4U, 12288U, 65528U, 256U,
                                   "spec.requirements.maximum_report_data_bits");
    }

    aoahid_spec_release(large_report_spec);
    aoahid_spec_release(report_size_spec);
    aoahid_spec_release(stack_spec);
    aoahid_spec_release(raw_spec);
    aoahid_spec_release(keyboard_spec);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_touchpad_button_only_close() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(12U, {5U, 2U});

    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);

    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};
    aoahid_device* device = nullptr;
    aoahid_device_options device_options = public_device_options();
    const std::uint32_t drain_timeout_ms = 100U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_touchpad_options touchpad{};
    touchpad.struct_size = sizeof(touchpad);
    touchpad.maximum_contacts = 2U;
    touchpad.contacts_per_report = 2U;
    touchpad.contact_id = {0, 15, 4U, {}};
    touchpad.x = {0, 1000, 16U, {}};
    touchpad.y = {0, 1000, 16U, {}};
    touchpad.contact_count = {0, 2, 2U, {}};
    touchpad.enable_scan_time = 1U;
    touchpad.scan_time = {0, 65535, 16U, {}};
    touchpad.scan_time_unit_100us = 1U;
    touchpad.enable_multi_packet_frames = 0U;
    touchpad.button_count = 1U;

    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchpad(&touchpad, &spec) == AOAHID_OK);
    const aoa::hid::FieldLayout* count =
        profile_field(spec, aoa::hid::FieldSemantic::contact_count);
    const aoa::hid::FieldLayout* button = profile_field(spec, aoa::hid::FieldSemantic::buttons);
    AOAHID_CHECK(count != nullptr && button != nullptr);

    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_touchpad_button(node, 1U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);

    const auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U);
    if (payloads.size() == 2U && count != nullptr && button != nullptr) {
        AOAHID_CHECK(extract_report_value(payloads[0], *count) == 0U);
        AOAHID_CHECK(extract_report_value(payloads[0], *button) == 1U);
        AOAHID_CHECK(extract_report_value(payloads[1], *count) == 0U);
        AOAHID_CHECK(extract_report_value(payloads[1], *button) == 0U);
    }

    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_controller_close_neutralizes_dpad() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(14U, {5U, 4U});

    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};
    aoahid_device* device = nullptr;
    aoahid_device_options device_options = public_device_options();
    const std::uint32_t drain_timeout_ms = 100U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    const std::array<aoahid_gamepad_axis_option, 2U> axes{
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X", "AXIS_X"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y", "AXIS_Y"}};
    aoahid_gamepad_options controller{};
    controller.struct_size = sizeof(controller);
    controller.axes = axes.data();
    controller.axis_count = axes.size();
    controller.button_count = 5U;
    controller.button_usage_minimum = 1U;

    controller.dpad_representation = AOAHID_DPAD_BUTTONS;
    aoahid_spec* raw_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&controller, &raw_spec) == AOAHID_OK);
    const std::array<const aoa::hid::FieldLayout*, 4U> raw_dpad{
        profile_field(raw_spec, aoa::hid::FieldSemantic::buttons, 5U),
        profile_field(raw_spec, aoa::hid::FieldSemantic::buttons, 6U),
        profile_field(raw_spec, aoa::hid::FieldSemantic::buttons, 7U),
        profile_field(raw_spec, aoa::hid::FieldSemantic::buttons, 8U)};
    AOAHID_CHECK(std::all_of(raw_dpad.begin(), raw_dpad.end(),
                             [](const auto* field) { return field != nullptr; }));
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* raw_node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, raw_spec, &node_options, kControlTimeoutMs, &raw_node) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_gamepad_dpad(raw_node, 1U, 0U, 0U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(raw_node, 100U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(raw_node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U);
    if (payloads.size() == 2U && std::all_of(raw_dpad.begin(), raw_dpad.end(),
                                             [](const auto* field) { return field != nullptr; })) {
        AOAHID_CHECK(extract_report_value(payloads[0], *raw_dpad[0]) == 1U);
        for (const auto* field : raw_dpad) {
            AOAHID_CHECK(extract_report_value(payloads[1], *field) == 0U);
        }
    }
    aoahid_spec_release(raw_spec);

    // The shared neutralization also clears the stored encoded value, but a
    // canonical Hat must continue to serialize its declared Null value.
    controller.dpad_representation = AOAHID_DPAD_HAT;
    controller.hat_logical_minimum = 0;
    controller.hat_logical_maximum = 7;
    controller.hat_bit_width = 4U;
    aoahid_spec* hat_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&controller, &hat_spec) == AOAHID_OK);
    const auto* hat = profile_field(hat_spec, aoa::hid::FieldSemantic::hat);
    AOAHID_CHECK(hat != nullptr);
    aoahid_node* hat_node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, hat_spec, &node_options, kControlTimeoutMs, &hat_node) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_gamepad_dpad(hat_node, 1U, 0U, 0U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(hat_node, 100U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(hat_node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 4U);
    if (payloads.size() == 4U && hat != nullptr) {
        AOAHID_CHECK(extract_report_value(payloads[2], *hat) == 0U);
        AOAHID_CHECK(extract_report_value(payloads[3], *hat) == 15U);
    }
    aoahid_spec_release(hat_spec);

    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

// The drain wait the session's tests give when they close what a helper below opened.
std::uint32_t session_drain_timeout_ms = kDrainTimeoutMs;

void open_public_raw_session(aoahid_context** context, aoahid_device** device, aoahid_node** node,
                             const std::uint32_t close_drain_timeout_ms = 1U) {
    static constexpr std::array<std::uint8_t, 2U> path{5U, 1U};
    const aoahid_fake_libusb_device_config fake{
        2U,      7U, path.data(),       path.size(), 0x18D1U,
        0x2D00U, 2U, "graveyard-phone", "Android",   LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&fake) >= 0);

    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, context) == AOAHID_OK);
    const aoahid_device_info selected{fake.bus,    fake.address,   path.data(),
                                      path.size(), fake.vendor_id, fake.product_id,
                                      0U,          fake.serial,    fake.product};
    aoahid_device_options device_options = public_device_options();
    session_drain_timeout_ms = close_drain_timeout_ms;
    AOAHID_CHECK(aoahid_device_open(*context, &selected, &device_options, device) == AOAHID_OK);

    static constexpr std::array<std::uint8_t, 21U> descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x25U,
        0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report report{{1U, 1U, {}}, 2U};
    const aoahid_raw_options raw_options{sizeof(aoahid_raw_options),
                                         0U,
                                         descriptor.data(),
                                         descriptor.size(),
                                         &report,
                                         1U,
                                         1U,
                                         0U,
                                         0U};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&raw_options, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    AOAHID_CHECK(aoahid_node_open(*device, spec, &node_options, kControlTimeoutMs, node) ==
                 AOAHID_OK);
    // The node's retained reference must keep the spec alive through a late
    // callback even after the caller drops its own reference.
    aoahid_spec_release(spec);
}

void open_public_keyboard_session(aoahid_context** context, aoahid_device** device,
                                  aoahid_node** node, const std::uint32_t close_timeout_ms = 100U) {
    static constexpr std::array<std::uint8_t, 2U> path{6U, 1U};
    const aoahid_fake_libusb_device_config fake{
        3U,      9U, path.data(),      path.size(), 0x18D1U,
        0x2D00U, 2U, "keyboard-phone", "Android",   LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&fake) >= 0);
    aoahid_context_options context_config = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_config, context) == AOAHID_OK);
    const aoahid_device_info selected{fake.bus,    fake.address,   path.data(),
                                      path.size(), fake.vendor_id, fake.product_id,
                                      0U,          fake.serial,    fake.product};
    aoahid_device_options device_config = public_device_options();
    session_drain_timeout_ms = close_timeout_ms;
    AOAHID_CHECK(aoahid_device_open(*context, &selected, &device_config, device) == AOAHID_OK);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    AOAHID_CHECK(aoahid_node_open(*device, spec, &node_options, kControlTimeoutMs, node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);
}

void test_first_report_stall_is_returned_without_retry() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_keyboard_session(&context, &device, &node);

    // A first report that races Android's HID registration and stalls goes
    // out once; the STALL reaches the caller, who decides whether to resend.
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL, 0, 0U);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    for (std::size_t attempt = 0U;
         attempt < 10U && node->transfer_inflight.load(std::memory_order_acquire); ++attempt) {
        AOAHID_CHECK(aoahid_context_poll(context, 1U) == AOAHID_OK);
    }
    AOAHID_CHECK(!node->transfer_inflight.load(std::memory_order_acquire));
    AOAHID_CHECK(controls_for(57U).size() == 1U);

    // The STALL is reported once; the refused state stays pending, so the
    // caller's explicit resend is an ordinary submit of the same report.
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_ERR_STALL);
    AOAHID_CHECK(controls_for(57U).size() == 1U);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    const auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U && payloads[0] == payloads[1]);
    // A timeout may have been delivered, so it is not resent automatically.
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x05U, 1U) == AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_TIMED_OUT, 0, 0U);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    for (std::size_t attempt = 0U;
         attempt < 10U && node->transfer_inflight.load(std::memory_order_acquire); ++attempt) {
        AOAHID_CHECK(aoahid_context_poll(context, 1U) == AOAHID_OK);
    }
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(controls_for(57U).size() == 3U);

    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_error_contract_and_raw_padding() {
    auto* stale_discovery = reinterpret_cast<aoahid_discovery*>(static_cast<std::uintptr_t>(1U));
    AOAHID_CHECK(aoahid_discover(nullptr, 0U, &stale_discovery) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(stale_discovery == nullptr);
    auto* stale_device = reinterpret_cast<aoahid_device*>(static_cast<std::uintptr_t>(1U));
    AOAHID_CHECK(aoahid_device_open(nullptr, nullptr, nullptr, &stale_device) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(stale_device == nullptr);
    AOAHID_CHECK(aoahid_context_poll(nullptr, 1U) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_last_error()->field != nullptr && aoahid_last_error()->reason != nullptr);
    static_cast<void>(aoahid_version());
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);
    AOAHID_CHECK(aoahid_last_error()->field == nullptr);
    AOAHID_CHECK(aoahid_last_error()->transport_status == 0);
    AOAHID_CHECK(aoahid_result_name(999) != nullptr);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_result_name(AOAHID_OK) != nullptr);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);

    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);

    const std::array<std::uint8_t, 2U> bad_padding{1U, 0x80U};
    AOAHID_CHECK(aoahid_raw_submit(node, bad_padding.data(), bad_padding.size(), kSendTimeoutMs) ==
                 AOAHID_ERR_PARAM);
    const aoahid_error_detail* padding_error = aoahid_last_error();
    AOAHID_CHECK(padding_error->field != nullptr && padding_error->reason != nullptr);
    AOAHID_CHECK(padding_error->aoa_request == 57);
    AOAHID_CHECK(padding_error->report_id == 1U);
    AOAHID_CHECK(padding_error->offset == 1U && padding_error->length == 1U);
    AOAHID_CHECK(controls_for(57U).empty());

    const std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_TIMED_OUT, 0, 0U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_ERR_TIMEOUT);
    const aoahid_error_detail* completion_error = aoahid_last_error();
    AOAHID_CHECK(completion_error->code == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(completion_error->transport_status == LIBUSB_TRANSFER_TIMED_OUT);
    AOAHID_CHECK(completion_error->aoa_request == 57);
    AOAHID_CHECK(completion_error->node_id != 0U && completion_error->report_id == 1U);
    AOAHID_CHECK(completion_error->offset == 0U && completion_error->length == 2U);
    AOAHID_CHECK(aoahid_device_latched_error(device) == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_TIMED_OUT);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 57);
    AOAHID_CHECK(aoahid_last_error()->report_id == 1U);

    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_OVERFLOW, 0, 0U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_ERR_OVERFLOW);
    const aoahid_error_detail* overflow_error = aoahid_last_error();
    AOAHID_CHECK(overflow_error->code == AOAHID_ERR_OVERFLOW);
    AOAHID_CHECK(overflow_error->transport_status == LIBUSB_TRANSFER_OVERFLOW);
    AOAHID_CHECK(overflow_error->aoa_request == 57 && overflow_error->report_id == 1U);
    AOAHID_CHECK(overflow_error->length == report.size());
    AOAHID_CHECK(aoahid_device_latched_error(device) == AOAHID_ERR_OVERFLOW);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_OVERFLOW);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_submit_rejection_retains_state_and_close_releases() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_keyboard_session(&context, &device, &node);

    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    aoahid_fake_libusb_queue_submit_result(LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(controls_for(57U).empty());
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 1U);
    if (payloads.size() == 1U) {
        // Usage 0x04 is bitmap bit zero: the first byte after the modifier byte.
        AOAHID_CHECK(payloads[0].size() > 1U && payloads[0][1] == 0x01U);
    }

    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U);
    if (payloads.size() == 2U) {
        AOAHID_CHECK(std::all_of(payloads[1].begin(), payloads[1].end(),
                                 [](const std::uint8_t value) { return value == 0U; }));
    }
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_node_open_host_policy_failure_precedes_request54() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* first_node = nullptr;
    open_public_keyboard_session(&context, &device, &first_node);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const std::size_t registrations_before = controls_for(54U).size();
    const aoahid_node_options reserved_node{sizeof(aoahid_node_options), 0U, 1U, 1U};
    aoahid_node* rejected = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &reserved_node, kControlTimeoutMs, &rejected) ==
                 AOAHID_ERR_PARAM);
    AOAHID_CHECK(rejected == nullptr);
    AOAHID_CHECK(aoahid_last_error()->field != nullptr);
    if (aoahid_last_error()->field != nullptr) {
        AOAHID_CHECK(std::string_view(aoahid_last_error()->field) == "node.reserved_slots");
    }
    AOAHID_CHECK(controls_for(54U).size() == registrations_before);

    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_node_close(first_node, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_node_close_timeout_distinction_and_loss_freeze() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node, 100U);
    const std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_TIMED_OUT, 0, 0U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_TIMED_OUT);
    // The failed close still consumed the Node.
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);

    aoahid_fake_libusb_reset();
    open_public_keyboard_session(&context, &device, &node);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_NO_DEVICE, 0, 0U);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 0U) == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 57);
    AOAHID_CHECK(aoahid_device_latched_error(device) == AOAHID_ERR_NO_DEVICE);
    // The transport latch is independent of the Node's terminal completion.
    // Device teardown must still publish that callback once while consuming
    // the graph.
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 57);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void submit_delayed_public_report(aoahid_node* node);

void test_node_close_retains_terminal_link_loss() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    const std::uint16_t hid_id = aoahid_node_id(node);
    const std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_NO_DEVICE, 0, 0U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);

    // Link loss makes present()==false, but it must not bypass the already
    // published terminal completion when the standalone Node remains owned by
    // the caller.
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_ERR_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_TRANSFER_NO_DEVICE);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 57);
    AOAHID_CHECK(aoahid_last_error()->node_id == hid_id);
    AOAHID_CHECK(aoahid_last_error()->report_id == 1U);
    AOAHID_CHECK(aoahid_last_error()->length == report.size());
    // The failed close still consumed the Node.
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_caller_poll_hot_path_is_lock_and_allocation_free() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_keyboard_session(&context, &device, &node);
    AOAHID_CHECK(context->active_state_mutex == nullptr);
    AOAHID_CHECK(node->active_completion_mutex == nullptr);

    // Prewarm the fake event dispatcher's reusable callback array, then stop
    // recording request 57 payloads so only library allocations are measured.
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    aoahid_fake_libusb_set_control_recording(0);
    const aoahid_fake_libusb_event_stats before = event_stats();
    allocation_probe::begin();
    const aoahid_result key_down = aoahid_keyboard_key(node, 0x04U, 1U);
    const aoahid_result first_submit = aoahid_node_submit(node, kSendTimeoutMs);
    const bool first_completed = !node->transfer_inflight.load(std::memory_order_acquire);
    const aoahid_result key_up = aoahid_keyboard_key(node, 0x04U, 0U);
    const aoahid_result second_submit = aoahid_node_submit(node, kSendTimeoutMs);
    const bool second_completed = !node->transfer_inflight.load(std::memory_order_acquire);
    const std::size_t hot_allocations = allocation_probe::finish();
    aoahid_fake_libusb_set_control_recording(1);

    AOAHID_CHECK(key_down == AOAHID_OK);
    AOAHID_CHECK(first_submit == AOAHID_OK);
    AOAHID_CHECK(first_completed);
    AOAHID_CHECK(key_up == AOAHID_OK);
    AOAHID_CHECK(second_submit == AOAHID_OK);
    AOAHID_CHECK(second_completed);
    const aoahid_fake_libusb_event_stats after = event_stats();
    // Each accepted report performs exactly one async submit followed by one
    // nonblocking event pass and one completion callback. Together with the
    // completed flags sampled before the next state change, these counts prove
    // there is no extra pre-submit event reap in the measured hot path.
    AOAHID_CHECK(after.successful_submits == before.successful_submits + 2U);
    AOAHID_CHECK(after.handle_calls == before.handle_calls + 2U);
    AOAHID_CHECK(after.zero_timeout_calls == before.zero_timeout_calls + 2U);
    AOAHID_CHECK(after.callbacks_dispatched == before.callbacks_dispatched + 2U);
    AOAHID_CHECK(after.waits_started == before.waits_started);
    AOAHID_CHECK(hot_allocations == 0U);

    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_channel_hot_path_is_lock_and_allocation_free() {
    aoahid_fake_libusb_reset();
    const Candidate phone = add_adb_phone();
    aoahid_context_options context_options = public_context_options();
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(phone);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);
    const aoahid_channel_options options = adb_channel_options();
    aoahid_channel* channel = nullptr;
    AOAHID_CHECK(aoahid_channel_open(device, &options, kDrainTimeoutMs, &channel) == AOAHID_OK);
    AOAHID_CHECK(context->active_state_mutex == nullptr);

    const std::array<std::uint8_t, 24> header{'O', 'K', 'A', 'Y'};
    aoahid_fake_libusb_push_bulk_in(0U, kAdbEndpointIn, header.data(), header.size());
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    aoahid_fake_libusb_set_control_recording(0);
    std::array<std::uint8_t, 32> buffer{};
    std::size_t received = 0U;
    std::size_t written = 0U;
    allocation_probe::begin();
    const aoahid_result read =
        aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 0U);
    const aoahid_result write =
        aoahid_channel_write(channel, header.data(), header.size(), &written, 0U);
    const aoahid_result polled = aoahid_context_poll(context, 0U);
    const aoahid_result empty =
        aoahid_channel_read(channel, buffer.data(), buffer.size(), &received, 0U);
    const std::size_t hot_allocations = allocation_probe::finish();
    aoahid_fake_libusb_set_control_recording(1);

    AOAHID_CHECK(read == AOAHID_OK && written == header.size());
    AOAHID_CHECK(write == AOAHID_OK && polled == AOAHID_OK);
    AOAHID_CHECK(empty == AOAHID_ERR_TIMEOUT);
    AOAHID_CHECK(hot_allocations == 0U);

    AOAHID_CHECK(aoahid_channel_close(channel, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_accepted_submit_defers_opportunistic_poll_error() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_keyboard_session(&context, &device, &node);
    // These fixed null selectors are the structural fast-path contract: the
    // submit preflight/error latch and synchronous completion callback use the
    // caller's synchronization domain without acquiring std::mutex.
    AOAHID_CHECK(context->active_state_mutex == nullptr);
    AOAHID_CHECK(node->active_completion_mutex == nullptr);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    aoahid_fake_libusb_queue_event_result(LIBUSB_ERROR_IO);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);

    // The report was accepted, so the submit remains successful. The exact
    // event-handler failure is delivered on the next Context-using call.
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_ERR_IO);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_ERROR_IO);
    AOAHID_CHECK(aoahid_last_error()->field != nullptr);
    if (aoahid_last_error()->field != nullptr) {
        AOAHID_CHECK(std::string_view(aoahid_last_error()->field) == "context.event_pump");
    }
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_context_destroy_aggregates_unregister_error() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    const std::uint16_t hid_id = aoahid_node_id(node);
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);

    // This terminal error consumes Context: teardown completed and the exact
    // request-55 failure is returned instead of being discarded.
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_ERR_STALL);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 55);
    AOAHID_CHECK(aoahid_last_error()->node_id == hid_id);
    // The failed unregister consumed the Node, so teardown does not send request 55 again.
    AOAHID_CHECK(controls_for(55U).size() == 1U);
    aoahid_fake_libusb_reset();
}

void test_context_destroy_never_returns_terminal_pending() {
    aoahid_fake_libusb_reset();
    aoahid_context_options options = public_context_options();
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&options, &context) == AOAHID_OK);
    aoa::detail::DeferredError invalid_terminal{};
    invalid_terminal.transport = aoa::transport::ErrorInfo{AOAHID_CLOSE_PENDING, 0, 0, 0U, 0U, 0U};
    invalid_terminal.field = "context.destroy";
    invalid_terminal.reason = "Injected invalid terminal pending state.";
    invalid_terminal.pending = true;
    {
        const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
        context->deferred_error = invalid_terminal;
    }
    // Context is consumed, but the consuming result can never be the one value
    // that authorizes the blocking wrapper to inspect it again.
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) ==
                 AOAHID_ERR_INTERNAL);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_ERR_INTERNAL);
    AOAHID_CHECK(aoahid_last_error()->field != nullptr);
    if (aoahid_last_error()->field != nullptr) {
        AOAHID_CHECK(std::string_view(aoahid_last_error()->field) == "context.destroy");
    }
    aoahid_fake_libusb_reset();
}

void test_graveyard_publishes_unregister_and_cancel_errors() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    const std::uint16_t unregister_hid_id = aoahid_node_id(node);
    submit_delayed_public_report(node);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_ERR_STALL);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 55);
    AOAHID_CHECK(aoahid_last_error()->node_id == unregister_hid_id);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);

    aoahid_fake_libusb_reset();
    open_public_raw_session(&context, &device, &node);
    const std::uint16_t cancel_hid_id = aoahid_node_id(node);
    const std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,
                                              static_cast<int>(report.size()), 1'000U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_cancel_result(LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_make_pending_transfers_ready();
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 57);
    AOAHID_CHECK(aoahid_last_error()->node_id == cancel_hid_id);
    AOAHID_CHECK(aoahid_last_error()->length == report.size());
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void submit_delayed_public_report(aoahid_node* node) {
    static constexpr std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 2, 1'000U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 1U);
}

void test_node_close_with_report_in_flight_keeps_node() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    submit_delayed_public_report(node);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, 1U) == AOAHID_CLOSE_PENDING);
    AOAHID_CHECK(aoahid_node_id(node) != 0U);
    aoahid_fake_libusb_make_pending_transfers_ready();
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_node_close_failed_unregister_consumes_node() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    const std::uint16_t hid_id = aoahid_node_id(node);
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_ERR_STALL);
    AOAHID_CHECK(aoahid_last_error()->aoa_request == 55);
    AOAHID_CHECK(aoahid_last_error()->node_id == hid_id);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(controls_for(55U).size() == 1U);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void open_public_touch_session(aoahid_context** context, aoahid_device** device, aoahid_node** node,
                               const bool enable_scan_time = false,
                               const bool enable_battery = false) {
    static constexpr std::array<std::uint8_t, 2U> path{5U, 2U};
    const aoahid_fake_libusb_device_config fake{
        2U,      8U, path.data(),         path.size(), 0x18D1U,
        0x2D00U, 2U, "touch-close-phone", "Android",   LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&fake) >= 0);

    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, context) == AOAHID_OK);
    const aoahid_device_info selected{fake.bus,    fake.address,   path.data(),
                                      path.size(), fake.vendor_id, fake.product_id,
                                      0U,          fake.serial,    fake.product};
    aoahid_device_options device_options = public_device_options();
    // A short drain timeout is fine on a lightly loaded CI runner, but the
    // fake backend's async completion still crosses a real OS thread wakeup,
    // and some Windows runners (particularly ARM64) have shown enough
    // scheduling jitter to miss a 50 ms budget. Draining still returns as
    // soon as the pending frame completes, so a larger ceiling only adds
    // margin; it does not slow down the success path.
    session_drain_timeout_ms = 2000U;
    AOAHID_CHECK(aoahid_device_open(*context, &selected, &device_options, device) == AOAHID_OK);

    aoahid_touchscreen_options touch{};
    touch.struct_size = sizeof(touch);
    touch.maximum_contacts = 2U;
    touch.contacts_per_report = 1U;
    touch.contact_id = {0, 15, 4U, {}};
    touch.x = {0, 1000, 16U, {}};
    touch.y = {0, 1000, 16U, {}};
    touch.contact_count = {0, 2, 2U, {}};
    if (enable_scan_time) {
        touch.enable_scan_time = 1U;
        touch.scan_time = {0, 65535, 16U, {}};
        touch.scan_time_unit_100us = 1U;
    }
    touch.enable_multi_packet_frames = 1U;
    if (enable_battery) {
        touch.report_id = {1U, 1U, {0U, 0U, 0U}};
        touch.enable_battery = 1U;
        touch.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
        touch.battery.strength = {1, 100, 8U, {}};
    }
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&touch, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    AOAHID_CHECK(aoahid_node_open(*device, spec, &node_options, kControlTimeoutMs, node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);

    const aoa::detail::TouchContact first{1U, 100, 200, 0, 0, 0, 0};
    const aoa::detail::TouchContact second{2U, 300, 400, 0, 0, 0, 0};
    AOAHID_CHECK(aoahid_touchscreen_contact(*node, first.contact_id, 1U, first.x, first.y,
                                            nullptr) == AOAHID_OK);
    AOAHID_CHECK(aoahid_touchscreen_contact(*node, second.contact_id, 1U, second.x, second.y,
                                            nullptr) == AOAHID_OK);
}

void test_public_touch_scan_time_lifecycle() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_touch_session(&context, &device, &node, true);

    const auto* scan_time = profile_field(node->spec, aoa::hid::FieldSemantic::scan_time);
    AOAHID_CHECK(scan_time != nullptr);
    auto* touch = std::get_if<aoa::detail::TouchState>(&node->state);
    AOAHID_CHECK(touch != nullptr);
    if (scan_time == nullptr || touch == nullptr) {
        AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                     AOAHID_OK);
        AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                     AOAHID_OK);
        aoahid_fake_libusb_reset();
        return;
    }

    // Put the epoch one full 16-bit cycle plus 1000 ticks in the past. The
    // accepted value must therefore prove both nonzero progression and modulo
    // wrap without sleeping or assuming an OS/libusb latency.
    constexpr std::uint64_t kModulus = 65'536U;
    constexpr std::uint64_t kElapsedTicks = kModulus + 1'000U;
    constexpr auto kTick = std::chrono::microseconds{100};
    const auto epoch = std::chrono::steady_clock::now() - kTick * kElapsedTicks;
    touch->scan_epoch = epoch;
    touch->scan_epoch_active = true;

    const auto before_submit = std::chrono::steady_clock::now();
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    const auto after_submit = std::chrono::steady_clock::now();
    auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U);
    if (payloads.size() == 2U) {
        const std::uint64_t first = extract_report_value(payloads[0], *scan_time);
        const std::uint64_t continuation = extract_report_value(payloads[1], *scan_time);
        AOAHID_CHECK(first == continuation);
        AOAHID_CHECK(first != 0U);

        const auto ticks_since_epoch = [epoch](const auto point) {
            return static_cast<std::uint64_t>(
                       std::chrono::duration_cast<std::chrono::microseconds>(point - epoch)
                           .count()) /
                   100U;
        };
        const std::uint64_t first_possible_tick = ticks_since_epoch(before_submit);
        const std::uint64_t last_possible_tick = ticks_since_epoch(after_submit);
        AOAHID_CHECK(first_possible_tick >= kModulus);
        const auto in_modulo_window = [](const std::uint64_t value, const std::uint64_t first_tick,
                                         const std::uint64_t last_tick) {
            if (last_tick - first_tick >= kModulus)
                return true;
            const std::uint64_t first_modulo = first_tick % kModulus;
            const std::uint64_t last_modulo = last_tick % kModulus;
            return first_modulo <= last_modulo ? value >= first_modulo && value <= last_modulo
                                               : value >= first_modulo || value <= last_modulo;
        };
        AOAHID_CHECK(in_modulo_window(first, first_possible_tick, last_possible_tick));
    }

    AOAHID_CHECK(aoahid_touchscreen_contact(node, 1U, 0U, 100, 200, nullptr) == AOAHID_OK);
    AOAHID_CHECK(aoahid_touchscreen_contact(node, 2U, 0U, 300, 400, nullptr) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    AOAHID_CHECK(!touch->scan_epoch_active);
    payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 4U);

    const aoa::detail::TouchContact next{3U, 500, 600, 0, 0, 0, 0};
    AOAHID_CHECK(aoahid_touchscreen_contact(node, next.contact_id, 1U, next.x, next.y, nullptr) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 100U) == AOAHID_OK);
    payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 5U);
    if (payloads.size() == 5U) {
        AOAHID_CHECK(extract_report_value(payloads.back(), *scan_time) == 0U);
    }

    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, session_drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_touch_scan_time_rejected_first_attempt_stays_zero() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(15U, {5U, 5U});

    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected{candidate.bus,
                                      candidate.address,
                                      candidate.port_path.data(),
                                      candidate.port_path.size(),
                                      candidate.vendor_id,
                                      candidate.product_id,
                                      0U,
                                      candidate.serial.c_str(),
                                      candidate.product.c_str()};
    aoahid_device_options device_options = public_device_options();
    device_options.transfer_pool_slots = 1U;
    const std::uint32_t drain_timeout_ms = 100U;
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    static constexpr std::array<std::uint8_t, 21U> raw_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x25U,
        0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const aoahid_raw_report raw_report{{1U, 1U, {}}, 2U};
    aoahid_raw_options raw_options{};
    raw_options.struct_size = sizeof(raw_options);
    raw_options.descriptor = raw_descriptor.data();
    raw_options.descriptor_length = raw_descriptor.size();
    raw_options.reports = &raw_report;
    raw_options.report_count = 1U;
    raw_options.acknowledges_no_android_support = 1U;
    aoahid_spec* raw_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&raw_options, &raw_spec) == AOAHID_OK);

    aoahid_touchscreen_options touchscreen_options{};
    touchscreen_options.struct_size = sizeof(touchscreen_options);
    touchscreen_options.maximum_contacts = 1U;
    touchscreen_options.contacts_per_report = 1U;
    touchscreen_options.contact_id = {0, 15, 4U, {}};
    touchscreen_options.x = {0, 1000, 16U, {}};
    touchscreen_options.y = {0, 1000, 16U, {}};
    touchscreen_options.contact_count = {0, 1, 1U, {}};
    touchscreen_options.enable_scan_time = 1U;
    touchscreen_options.scan_time = {0, 65535, 16U, {}};
    touchscreen_options.scan_time_unit_100us = 1U;
    aoahid_spec* touch_spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&touchscreen_options, &touch_spec) == AOAHID_OK);
    const auto* scan_time = profile_field(touch_spec, aoa::hid::FieldSemantic::scan_time);
    AOAHID_CHECK(scan_time != nullptr);

    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* raw_node = nullptr;
    aoahid_node* touch_node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, raw_spec, &node_options, kControlTimeoutMs, &raw_node) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_node_open(device, touch_spec, &node_options, kControlTimeoutMs,
                                  &touch_node) == AOAHID_OK);
    const aoa::detail::TouchContact contact{1U, 100, 200, 0, 0, 0, 0};
    AOAHID_CHECK(aoahid_touchscreen_contact(touch_node, contact.contact_id, 1U, contact.x,
                                            contact.y, nullptr) == AOAHID_OK);
    auto* touch = std::get_if<aoa::detail::TouchState>(&touch_node->state);
    AOAHID_CHECK(touch != nullptr && !touch->scan_epoch_active && touch->scan_time == 0U);

    static constexpr std::array<std::uint8_t, 2U> raw_payload{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,
                                              static_cast<int>(raw_payload.size()), 1'000U);
    AOAHID_CHECK(aoahid_raw_submit(raw_node, raw_payload.data(), raw_payload.size(),
                                   kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 1U);
    AOAHID_CHECK(aoahid_node_submit(touch_node, kSendTimeoutMs) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(touch != nullptr && !touch->scan_epoch_active && touch->scan_time == 0U);

    aoahid_fake_libusb_make_pending_transfers_ready();
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 0U);

    // A libusb submission rejected synchronously has the same unaccepted-frame
    // semantics as an acquire failure and must roll the provisional epoch back.
    aoahid_fake_libusb_queue_submit_result(LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_node_submit(touch_node, kSendTimeoutMs) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(touch != nullptr && !touch->scan_epoch_active && touch->scan_time == 0U);

    AOAHID_CHECK(aoahid_node_submit_blocking(touch_node, 100U) == AOAHID_OK);
    const auto payloads = control_payloads_for(57U);
    AOAHID_CHECK(payloads.size() == 2U);
    if (payloads.size() == 2U && scan_time != nullptr) {
        AOAHID_CHECK(extract_report_value(payloads.back(), *scan_time) == 0U);
    }

    AOAHID_CHECK(aoahid_node_close(touch_node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(raw_node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_spec_release(touch_spec);
    aoahid_spec_release(raw_spec);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

// The battery report shares the Node with the input report: input goes first,
// one report per submit, and the battery never rides along on close.
void test_battery_reports() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(15U, {5U, 6U});
    aoahid_context* context = nullptr;
    aoahid_context_options context_options = public_context_options();
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(candidate);
    aoahid_device* device = nullptr;
    aoahid_device_options device_options = public_device_options();
    const std::uint32_t drain_timeout_ms = 2000U;
    device_options.validate_reports = 1U;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);

    using Reports = std::vector<std::vector<std::uint8_t>>;
    const auto battery = [](const std::uint8_t value) {
        return std::vector<std::uint8_t>{2U, value};
    };
    std::size_t seen = 0U;
    // The request-57 payloads sent since the last call.
    const auto fresh = [&] {
        Reports all = control_payloads_for(57U);
        Reports added(all.begin() + static_cast<std::ptrdiff_t>(std::min(seen, all.size())),
                      all.end());
        seen = all.size();
        return added;
    };
    const auto is_key_report = [](const std::vector<std::uint8_t>& report) {
        return report.size() == 15U && report[0] == 1U;
    };

    // Nothing was set: nothing is sent.
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    AOAHID_CHECK(fresh().empty());

    // Battery alone.
    AOAHID_CHECK(aoahid_battery_update(node, 80) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    Reports sent = fresh();
    AOAHID_CHECK(sent.size() == 1U && sent[0] == battery(80U));

    // Input first, then the battery, from one blocking submit.
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_battery_update(node, 79) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(sent.size() == 2U && is_key_report(sent[0]) && sent[1] == battery(79U));

    // One report per aoahid_node_submit: input, then battery, then nothing.
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_battery_update(node, 78) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(sent.size() == 1U && is_key_report(sent[0]));
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(sent.size() == 1U && sent[0] == battery(78U));
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(fresh().empty());

    // The same value again is not sent.
    AOAHID_CHECK(aoahid_battery_update(node, 78) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    AOAHID_CHECK(fresh().empty());

    // A STALLed battery report stays pending and goes out on the next submit;
    // no input report is produced by it.
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_STALL, 0, 0U);
    AOAHID_CHECK(aoahid_battery_update(node, 77) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_ERR_STALL);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(!sent.empty() && sent.back() == battery(77U));
    AOAHID_CHECK(std::none_of(sent.begin(), sent.end(), is_key_report));
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    AOAHID_CHECK(fresh().empty());

    // While the battery report is in flight an input update is refused like
    // any other, and accepted once it has completed.
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, 2, 3U);
    AOAHID_CHECK(aoahid_battery_update(node, 76) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x05U, 1U) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(aoahid_battery_update(node, 75) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(node, 0x05U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 500U) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(sent.size() == 2U && sent[0] == battery(76U) && is_key_report(sent[1]));

    // Close releases the held key and drops the unsent battery value.
    AOAHID_CHECK(aoahid_battery_update(node, 74) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    sent = fresh();
    AOAHID_CHECK(sent.size() == 1U && is_key_report(sent[0]) &&
                 std::all_of(sent[0].begin() + 1, sent[0].end(),
                             [](const std::uint8_t byte) { return byte == 0U; }));

    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

// A touch frame of two packets is not split by the battery report.
void test_battery_waits_for_the_touch_frame() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_touch_session(&context, &device, &node, false, true);
    AOAHID_CHECK(aoahid_battery_update(node, 50) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 2000U) == AOAHID_OK);
    const auto sent = control_payloads_for(57U);
    AOAHID_CHECK(sent.size() == 3U);
    if (sent.size() == 3U) {
        AOAHID_CHECK(sent[0][0] == 1U && sent[1][0] == 1U);
        AOAHID_CHECK(sent[2] == (std::vector<std::uint8_t>{2U, 50U}));
    }
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

// The same ordering with completions arriving on the internal event thread.
void test_battery_reports_internal_thread() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(16U, {5U, 7U});
    aoahid_context_options context_options = public_context_options();
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(candidate);
    aoahid_device_options device_options = public_device_options();
    const std::uint32_t drain_timeout_ms = 2000U;
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);

    for (std::int32_t level = 100; level > 80; --level) {
        AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 1U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_battery_update(node, level) == AOAHID_OK);
        AOAHID_CHECK(aoahid_node_submit_blocking(node, 2000U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_keyboard_key(node, 0x04U, 0U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_node_submit_blocking(node, 2000U) == AOAHID_OK);
    }
    const auto sent = control_payloads_for(57U);
    // Each round: key down, battery, key up.
    AOAHID_CHECK(sent.size() == 60U);
    for (std::size_t round = 0U; round < 20U && sent.size() == 60U; ++round) {
        AOAHID_CHECK(sent[round * 3U][0] == 1U && sent[round * 3U + 2U][0] == 1U);
        AOAHID_CHECK(sent[round * 3U + 1U] ==
                     (std::vector<std::uint8_t>{2U, static_cast<std::uint8_t>(100U - round)}));
    }
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, drain_timeout_ms) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_close_drains_touch_frame_before_neutral() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_touch_session(&context, &device, &node);

    // The submit's one nonblocking poll completes packet one, leaving a
    // nonzero packet cursor while no transfer is active.
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, -1, 0U);
    AOAHID_CHECK(aoahid_node_submit(node, kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 0U);
    const aoa::detail::TouchContact blocked_change{1U, 101, 201, 0, 0, 0, 0};
    AOAHID_CHECK(aoahid_touchscreen_contact(node, blocked_change.contact_id, 1U, blocked_change.x,
                                            blocked_change.y, nullptr) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(controls_for(57U).size() == 1U);

    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    // Initial frame: two packets. Neutral lift frame: two packets. Closing may
    // not splice the neutral state into the initial frame's second packet.
    AOAHID_CHECK(controls_for(57U).size() == 4U);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_graveyard_delayed_cancel() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    submit_delayed_public_report(node);

    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_cancel_count() == 1U);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 1U);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_CLOSE_PENDING);

    // poll executes CANCELLED, returns from the callback, and only then reaps
    // the complete device/node/spec/transport graph from the graveyard.
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 0U);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

void test_public_graveyard_blocking_destroy() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    submit_delayed_public_report(node);

    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    // A zero deadline is an unset timeout like any other, and destroys nothing.
    AOAHID_CHECK(aoahid_context_destroy_blocking(context, kControlTimeoutMs,
                                                 session_drain_timeout_ms,
                                                 0U) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(aoahid_context_destroy_blocking(context, kControlTimeoutMs,
                                                 session_drain_timeout_ms, 50U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_fake_libusb_pending_transfer_count() == 0U);
    aoahid_fake_libusb_reset();
}

void test_blocking_destroy_deadline_retains_context() {
    aoahid_fake_libusb_reset();
    aoahid_context* context = nullptr;
    aoahid_device* device = nullptr;
    aoahid_node* node = nullptr;
    open_public_raw_session(&context, &device, &node);
    const std::array<std::uint8_t, 2U> report{1U, 1U};
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED,
                                              static_cast<int>(report.size()), 1'000U);
    AOAHID_CHECK(aoahid_raw_submit(node, report.data(), report.size(), kSendTimeoutMs) ==
                 AOAHID_OK);
    aoahid_fake_libusb_queue_cancel_result(LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);

    AOAHID_CHECK(aoahid_context_destroy_blocking(context, kControlTimeoutMs,
                                                 session_drain_timeout_ms,
                                                 2U) == AOAHID_CLOSE_PENDING);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_CLOSE_PENDING);
    // CLOSE_PENDING retains Context. Make the original accepted transfer terminal,
    // publish the stored cancel failure, and then finish destruction.
    aoahid_fake_libusb_make_pending_transfers_ready();
    AOAHID_CHECK(aoahid_context_poll(context, 0U) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(aoahid_last_error()->transport_status == LIBUSB_ERROR_BUSY);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, session_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

// With every pool slot taken by another Node, submit_blocking in
// internal-thread mode sleeps until a slot is released instead of spinning,
// and sends as soon as one is.
void test_submit_blocking_waits_for_a_free_slot_without_spinning() {
    aoahid_fake_libusb_reset();
    const Candidate candidate = add_candidate(12U, {5U, 2U});
    aoahid_context_options context_options = public_context_options();
    context_options.event_mode = AOAHID_EVENT_INTERNAL_THREAD;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_options, &context) == AOAHID_OK);
    const aoahid_device_info selected = public_info(candidate);
    aoahid_device_options device_options = public_device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &device_options, &device) == AOAHID_OK);

    aoahid_keyboard_options keyboard{};
    keyboard.struct_size = sizeof(keyboard);
    keyboard.usage_minimum = 0x04U;
    keyboard.usage_maximum = 0x65U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    aoahid_node_options node_options{};
    node_options.struct_size = sizeof(node_options);
    aoahid_node* holder = nullptr;
    aoahid_node* waiter = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &holder) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &waiter) ==
                 AOAHID_OK);
    aoahid_spec_release(spec);

    // The holder's report takes the only slot and stays in flight.
    aoahid_fake_libusb_queue_async_completion(LIBUSB_TRANSFER_COMPLETED, -1,
                                              std::numeric_limits<std::uint32_t>::max());
    AOAHID_CHECK(aoahid_keyboard_key(holder, 0x04U, 1U) == AOAHID_OK);
    // The holder's transfer must outlive the waiter's whole wait.
    AOAHID_CHECK(aoahid_node_submit(holder, 5000U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(waiter, 0x05U, 1U) == AOAHID_OK);

    std::atomic<bool> done{false};
    aoahid_result result = AOAHID_ERR_INTERNAL;
    [[maybe_unused]] std::int64_t cpu_ns = -1;
    std::thread thread([&]() {
#if defined(__linux__)
        timespec start{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &start);
#endif
        result = aoahid_node_submit_blocking(waiter, 3000U);
#if defined(__linux__)
        timespec end{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &end);
        cpu_ns = (end.tv_sec - start.tv_sec) * 1000000000LL + (end.tv_nsec - start.tv_nsec);
#endif
        done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    AOAHID_CHECK(!done.load());
    const auto released = std::chrono::steady_clock::now();
    aoahid_fake_libusb_make_pending_transfers_ready();
    while (!done.load() && std::chrono::steady_clock::now() - released < std::chrono::seconds(3))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto waited = std::chrono::steady_clock::now() - released;
    thread.join();
    AOAHID_CHECK(result == AOAHID_OK);
    AOAHID_CHECK(waited < std::chrono::milliseconds(100));
#if defined(__linux__)
    // A spinning wait would burn roughly the whole 200 ms.
    AOAHID_CHECK(cpu_ns >= 0 && cpu_ns < 50000000LL);
#endif
    AOAHID_CHECK(aoahid_keyboard_key(holder, 0x04U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(waiter, 0x05U, 0U) == AOAHID_OK);
    // Both Nodes send a neutral report on close; a 1 ms budget would make this test timing-bound.
    constexpr std::uint32_t close_drain_timeout_ms = 1000U;
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, close_drain_timeout_ms) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, close_drain_timeout_ms) ==
                 AOAHID_OK);
    aoahid_fake_libusb_reset();
}

} // namespace

void test_transport() {
    test_runtime_initialization_options();
    test_registration_reservations_and_async_results();
    test_uncertain_register_attempts_unregister_and_preserves_error();
    test_descriptor_failure_unregisters_and_preserves_error();
    test_error_report_id_is_published_before_user_callback_returns();
    test_current_mode_open_and_explicit_claim();
    test_port_ownership_and_discovery_reuse();
    test_accessory_start_sends_51_52_53_and_closes();
    test_accessory_start_rejects_and_reports_failures();
    test_channel_shares_the_device_handle_internal_thread();
    test_channel_configures_an_unconfigured_device();
    test_channel_request_reads();
    test_hid_adb_and_accessory_bulk_run_together();
    test_channel_caller_poll_and_device_close();
    test_open_sends_no_aoa_request();
    test_required_device_policies_precede_usb_io();
    test_tuning_values_required_and_zero_reservation();
    test_initializers_open_a_working_session();
    test_cancel_and_device_isolation();
    test_blocking_event_wait_is_woken_by_cancel();
    test_public_touchpad_button_only_close();
    test_public_controller_close_neutralizes_dpad();
    test_descriptor_requirements_are_caller_policies();
    test_public_touch_scan_time_lifecycle();
    test_public_touch_scan_time_rejected_first_attempt_stays_zero();
    test_close_drains_touch_frame_before_neutral();
    test_battery_reports();
    test_battery_waits_for_the_touch_frame();
    test_battery_reports_internal_thread();
    test_public_error_contract_and_raw_padding();
    test_first_report_stall_is_returned_without_retry();
    test_submit_blocking_waits_for_a_free_slot_without_spinning();
    test_submit_rejection_retains_state_and_close_releases();
    test_node_open_host_policy_failure_precedes_request54();
    test_node_close_timeout_distinction_and_loss_freeze();
    test_node_close_retains_terminal_link_loss();
    test_node_close_with_report_in_flight_keeps_node();
    test_node_close_failed_unregister_consumes_node();
    test_caller_poll_hot_path_is_lock_and_allocation_free();
    test_channel_hot_path_is_lock_and_allocation_free();
    test_accepted_submit_defers_opportunistic_poll_error();
    test_context_destroy_aggregates_unregister_error();
    test_context_destroy_never_returns_terminal_pending();
    test_graveyard_publishes_unregister_and_cancel_errors();
    test_public_graveyard_delayed_cancel();
    test_public_graveyard_blocking_destroy();
    test_blocking_destroy_deadline_retains_context();
}
