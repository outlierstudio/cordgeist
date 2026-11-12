// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * AOA current-mode control requests, registration, and the allocation-free report
 * send path. The hot path uses a per-device LIFO pool; libusb objects and callbacks
 * remain wholly behind this translation unit.
 */

#include "transport/transport.hpp"
#include "transport/libusb_include.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace {

constexpr std::uint8_t request_type_out_vendor_device = 0x40U;
constexpr std::uint8_t accessory_register_hid = 54U;
constexpr std::uint8_t accessory_unregister_hid = 55U;
constexpr std::uint8_t accessory_set_hid_report_desc = 56U;
constexpr std::uint8_t accessory_send_hid_event = 57U;

bool control_outcome_may_be_accepted(const aoahid_result result) noexcept {
    // A host-side timeout, generic I/O failure, or disconnect does not prove the
    // target missed an OUT control request. A nonzero completion for a request
    // with no data stage is likewise not a usable success result, but the setup
    // may already have taken effect on the target.
    return result == AOAHID_ERR_TIMEOUT || result == AOAHID_ERR_IO ||
           result == AOAHID_ERR_NO_DEVICE || result == AOAHID_ERR_SHORT_TRANSFER;
}

void restore_error(const aoa::transport::ErrorInfo& error) noexcept {
    aoa::transport::record_error(error.result, error.native_status, error.aoa_request, error.hid_id,
                                 error.offset, error.length);
}

struct ControlOutPolicy {
    std::uint32_t timeout_ms{};
    std::uint32_t diagnostic_length{std::numeric_limits<std::uint32_t>::max()};
};

aoahid_result control_out(libusb_device_handle* handle, const std::uint8_t request,
                          const std::uint16_t value, const std::uint16_t index,
                          const std::uint8_t* data, const std::uint16_t length,
                          const ControlOutPolicy policy) noexcept {
    const int status =
        libusb_control_transfer(handle, request_type_out_vendor_device, request, value, index,
                                const_cast<unsigned char*>(data), length, policy.timeout_ms);
    const std::uint16_t hid_id =
        request >= accessory_register_hid && request <= accessory_send_hid_event ? value : 0U;
    const std::uint32_t offset = request == accessory_set_hid_report_desc ? index : 0U;
    aoahid_result result = AOAHID_OK;
    if (status < 0) {
        result = aoa::transport::map_libusb_error(status, false);
    } else if (status != length) {
        result = AOAHID_ERR_SHORT_TRANSFER;
    }
    if (result != AOAHID_OK) {
        aoa::transport::record_error(result, status, request, hid_id, offset,
                                     policy.diagnostic_length ==
                                             std::numeric_limits<std::uint32_t>::max()
                                         ? length
                                         : policy.diagnostic_length);
    }
    return result;
}

void store_le16(std::uint8_t* bytes, const std::uint16_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value & 0xFFU);
    bytes[1] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

} // namespace

namespace aoa::transport {

bool is_cancelled_completion_status(const std::int32_t native_status) noexcept {
    return native_status == static_cast<std::int32_t>(LIBUSB_TRANSFER_CANCELLED);
}

struct Device::Impl {
    enum class SlotState : std::uint8_t { free, prepared, submitted };

    struct Reservation;

    struct Slot {
        Impl* owner{};
        libusb_transfer* transfer{};
        std::vector<std::uint8_t> buffer;
        SlotState state{SlotState::free};
        void* user{};
        Completion completion{};
        std::uint16_t hid_id{};
        std::uint16_t report_id{};
        std::size_t report_length{};
        bool reservation_counted{};
        Reservation* reservation{};
    };

    struct Notice {
        Completion completion{};
        void* user{};
        aoahid_result result{AOAHID_OK};
        std::int32_t native_status{};
    };

    struct Reservation {
        Impl* owner{};
        std::uint16_t hid_id{};
        std::size_t capacity{};
        std::size_t in_use{};
    };

    Port* port{};
    libusb_device_handle* handle{};
    DeviceConfig config{};
    std::vector<Slot> slots;
    std::vector<Slot*> free_stack;
    std::vector<std::unique_ptr<Reservation>> reservations;
    std::size_t reserved_capacity_total{};
    std::size_t unfilled_reserved_total{};
    mutable std::atomic_flag state_lock = ATOMIC_FLAG_INIT;
    std::atomic_flag* active_state_lock{};
    std::size_t outstanding{};
    std::size_t callbacks_active{};
    ErrorInfo latched{};
    bool present{true};
    bool closing{};
    bool claimed{};
    int claimed_interface{-1};

    class StateGuard final {
      public:
        explicit StateGuard(const Impl& owner) noexcept : lock_(owner.active_state_lock) {
            // Caller-poll is a caller-serialized synchronization domain and
            // takes no device-state lock. Internal-thread mode pays this lock
            // only around the pool, inflight accounting, and error latch.
            if (lock_ != nullptr) {
                while (lock_->test_and_set(std::memory_order_acquire)) {
                }
            }
        }

        ~StateGuard() noexcept {
            if (lock_ != nullptr) {
                lock_->clear(std::memory_order_release);
            }
        }

        StateGuard(const StateGuard&) = delete;
        StateGuard& operator=(const StateGuard&) = delete;

      private:
        std::atomic_flag* lock_{};
    };

    void release_reservation_locked(Slot* slot) noexcept {
        Reservation* reservation = slot->reservation;
        if (reservation != nullptr && reservation->in_use > 0U) {
            --reservation->in_use;
            if (slot->reservation_counted) {
                ++unfilled_reserved_total;
            }
        }
        slot->reservation_counted = false;
        slot->reservation = nullptr;
    }

    static void LIBUSB_CALL transfer_callback(libusb_transfer* transfer) noexcept {
        if (transfer == nullptr || transfer->user_data == nullptr) {
            return;
        }
        auto* slot = static_cast<Slot*>(transfer->user_data);
        if (slot->owner != nullptr) {
            slot->owner->transfer_finished(slot);
        }
    }

    static void deliver(const Notice& notice) noexcept {
        if (notice.completion != nullptr) {
            notice.completion(notice.user, notice.result, notice.native_status);
        }
    }

    void deliver_terminal(const Notice& notice) noexcept {
        deliver(notice);
        if (notice.completion != nullptr) {
            const StateGuard guard(*this);
            if (callbacks_active > 0U) {
                --callbacks_active;
            }
        }
    }

    void latch_locked(const Slot* slot, const aoahid_result result,
                      const std::int32_t native_status) noexcept {
        if (result == AOAHID_OK ||
            (closing && native_status == static_cast<std::int32_t>(LIBUSB_TRANSFER_CANCELLED))) {
            return;
        }
        const ErrorInfo error{result,
                              native_status,
                              accessory_send_hid_event,
                              slot == nullptr ? std::uint16_t{0} : slot->hid_id,
                              0U,
                              slot == nullptr ? 0U
                                              : static_cast<std::uint32_t>(slot->report_length),
                              slot == nullptr ? std::uint16_t{0} : slot->report_id};
        if (result == AOAHID_ERR_NO_DEVICE) {
            present = false;
            latched = error;
        } else if (latched.result == AOAHID_OK) {
            latched = error;
        }
    }

    Notice finish_locked(Slot* slot, const aoahid_result result,
                         const std::int32_t native_status) noexcept {
        Notice notice{slot->completion, slot->user, result, native_status};
        if (notice.completion != nullptr) {
            ++callbacks_active;
        }
        latch_locked(slot, result, native_status);
        release_reservation_locked(slot);
        slot->state = SlotState::free;
        slot->user = nullptr;
        slot->completion = nullptr;
        slot->report_id = 0U;
        slot->report_length = 0U;
        slot->reservation = nullptr;
        if (outstanding > 0U) {
            --outstanding;
        }
        free_stack.push_back(slot);
        return notice;
    }

    static aoahid_result completion_result(const libusb_transfer* transfer) noexcept {
        switch (transfer->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return transfer->actual_length ==
                           static_cast<int>(static_cast<Slot*>(transfer->user_data)->report_length)
                       ? AOAHID_OK
                       : AOAHID_ERR_SHORT_TRANSFER;
        case LIBUSB_TRANSFER_TIMED_OUT:
            return AOAHID_ERR_TIMEOUT;
        case LIBUSB_TRANSFER_STALL:
            return AOAHID_ERR_STALL;
        case LIBUSB_TRANSFER_NO_DEVICE:
            return AOAHID_ERR_NO_DEVICE;
        case LIBUSB_TRANSFER_OVERFLOW:
            return AOAHID_ERR_OVERFLOW;
        case LIBUSB_TRANSFER_CANCELLED:
        case LIBUSB_TRANSFER_ERROR:
        default:
            return AOAHID_ERR_IO;
        }
    }

    void transfer_finished(Slot* slot) noexcept {
        Notice notice{};
        bool notify = false;
        {
            const StateGuard guard(*this);
            if (slot->state != SlotState::submitted) {
                return;
            }
            const aoahid_result result = completion_result(slot->transfer);
            notice = finish_locked(slot, result, static_cast<std::int32_t>(slot->transfer->status));
            notify = true;
        }
        if (notify) {
            deliver_terminal(notice);
        }
    }

    aoahid_result submit_prepared(const PreparedTransfer prepared, const std::size_t report_length,
                                  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                  const std::uint16_t report_id,
                                  const std::uint32_t send_timeout_ms) noexcept {
        auto* slot = static_cast<Slot*>(prepared.slot);
        if (slot == nullptr || slot->owner != this) {
            return AOAHID_ERR_PARAM;
        }

        Notice notice{};
        bool notify = false;
        aoahid_result result = AOAHID_OK;
        {
            const StateGuard guard(*this);
            if (slot->state == SlotState::prepared) {
                slot->report_id = report_id;
            }
            if (slot->state != SlotState::prepared ||
                prepared.payload != slot->buffer.data() + LIBUSB_CONTROL_SETUP_SIZE ||
                report_length != slot->report_length || report_length == 0U ||
                report_length > config.maximum_report_bytes ||
                report_id > std::numeric_limits<std::uint8_t>::max()) {
                if (slot->state == SlotState::prepared) {
                    notice = finish_locked(slot, AOAHID_ERR_PARAM, LIBUSB_ERROR_INVALID_PARAM);
                    notify = true;
                }
                result = AOAHID_ERR_PARAM;
            } else if (closing || !present) {
                result = present ? AOAHID_ERR_IO : AOAHID_ERR_NO_DEVICE;
                const std::int32_t native_status =
                    closing ? static_cast<std::int32_t>(LIBUSB_TRANSFER_CANCELLED)
                            : static_cast<std::int32_t>(LIBUSB_ERROR_NO_DEVICE);
                notice = finish_locked(slot, result, native_status);
                notify = true;
            } else {
                // acquire() already wrote this slot's HID ID and wLength.
                libusb_fill_control_transfer(slot->transfer, handle, slot->buffer.data(),
                                             transfer_callback, slot, send_timeout_ms);
                slot->state = SlotState::submitted;
                const int submit_status = libusb_submit_transfer(slot->transfer);
                if (submit_status != LIBUSB_SUCCESS) {
                    result = map_libusb_error(submit_status, false);
                    notice = finish_locked(slot, result, submit_status);
                    notify = true;
                }
            }
        }
        if (notify) {
            deliver_terminal(notice);
        }
        return result;
    }

    void observe_control_result(const aoahid_result result) noexcept {
        if (result != AOAHID_ERR_NO_DEVICE) {
            return;
        }
        const StateGuard guard(*this);
        present = false;
        latched = last_error();
        if (latched.result != AOAHID_ERR_NO_DEVICE) {
            latched = ErrorInfo{AOAHID_ERR_NO_DEVICE, 0, 0, 0U, 0U, 0U};
        }
    }
};

aoahid_result Device::open(Runtime* runtime, const Candidate& candidate, const DeviceConfig& config,
                           Device** out) {
    reset_error();
    if (out == nullptr) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    *out = nullptr;
    if (runtime == nullptr || runtime->native_context() == nullptr ||
        config.event_mode != runtime->event_mode_ ||
        (config.event_mode != AOAHID_EVENT_CALLER_POLL &&
         config.event_mode != AOAHID_EVENT_INTERNAL_THREAD) ||
        config.descriptor_fragment_bytes == 0U ||
        config.descriptor_fragment_bytes > std::numeric_limits<std::uint16_t>::max() ||
        config.pool_slots == 0U || config.maximum_report_bytes == 0U ||
        config.maximum_report_bytes > std::numeric_limits<std::uint16_t>::max() ||
        (config.claim_policy != AOAHID_INTERFACE_CLAIM_NONE &&
         config.claim_policy != AOAHID_INTERFACE_CLAIM_EXPLICIT) ||
        (config.claim_policy == AOAHID_INTERFACE_CLAIM_NONE && config.interface_number != -1) ||
        (config.claim_policy == AOAHID_INTERFACE_CLAIM_EXPLICIT &&
         (config.interface_number < 0 || config.interface_number > 255))) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }

    Port* port = nullptr;
    const aoahid_result port_result = runtime->acquire_port(candidate, &port);
    if (port_result != AOAHID_OK) {
        return port_result;
    }
    auto* handle = static_cast<libusb_device_handle*>(port->native_handle());

    bool claimed = false;
    int claimed_interface = -1;
    if (config.claim_policy == AOAHID_INTERFACE_CLAIM_EXPLICIT) {
        claimed_interface = config.interface_number;
        const int claim_status = port->claim_interface(claimed_interface);
        if (claim_status != LIBUSB_SUCCESS) {
            port->release();
            const aoahid_result claim_result = map_libusb_error(claim_status, false);
            record_error(claim_result, claim_status);
            return claim_result;
        }
        claimed = true;
    }

    auto* device = new (std::nothrow) Device();
    auto* impl = new (std::nothrow) Impl();
    if (device == nullptr || impl == nullptr) {
        delete device;
        delete impl;
        if (claimed) {
            port->release_interface(claimed_interface);
        }
        port->release();
        record_error(AOAHID_ERR_INTERNAL);
        return AOAHID_ERR_INTERNAL;
    }

    impl->port = port;
    impl->handle = handle;
    impl->config = config;
    impl->active_state_lock =
        config.event_mode == AOAHID_EVENT_INTERNAL_THREAD ? &impl->state_lock : nullptr;
    impl->claimed = claimed;
    impl->claimed_interface = claimed_interface;
    // Establish ownership before any throwing STL operation. If a resize,
    // reserve, or Runtime registry growth throws, Device's destructor releases
    // the partially built pool, claimed interface, and libusb handle before the
    // exception reaches the public C boundary.
    device->impl_ = impl;
    try {
        impl->slots.resize(config.pool_slots);
        impl->free_stack.reserve(config.pool_slots);
        impl->reservations.reserve(config.pool_slots);
        for (Impl::Slot& slot : impl->slots) {
            slot.owner = impl;
            slot.buffer.resize(static_cast<std::size_t>(LIBUSB_CONTROL_SETUP_SIZE) +
                               config.maximum_report_bytes);
            slot.transfer = libusb_alloc_transfer(0);
            if (slot.transfer == nullptr) {
                delete device;
                record_error(AOAHID_ERR_INTERNAL, LIBUSB_ERROR_NO_MEM);
                return AOAHID_ERR_INTERNAL;
            }
            libusb_fill_control_setup(slot.buffer.data(), request_type_out_vendor_device,
                                      accessory_send_hid_event, 0U, 0U, 0U);
            impl->free_stack.push_back(&slot);
        }
    } catch (...) {
        delete device;
        throw;
    }
    *out = device;
    return AOAHID_OK;
}

Device::~Device() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    Impl* impl = impl_;
    if (!drained()) {
        static_cast<void>(cancel_all());
        // The public lifecycle retains a non-drained Device in its graveyard.
        // If that contract is violated, leak the complete callback-owned state
        // rather than manufacturing a use-after-free in a late libusb callback.
        impl_ = nullptr;
        return;
    }
    if (impl->claimed) {
        impl->port->release_interface(impl->claimed_interface);
    }
    impl->port->release();
    for (Impl::Slot& slot : impl->slots) {
        if (slot.transfer != nullptr) {
            libusb_free_transfer(slot.transfer);
        }
        slot.transfer = nullptr;
    }
    delete impl;
    impl_ = nullptr;
}

aoahid_result Device::register_hid(const std::uint16_t hid_id, const std::uint8_t* descriptor,
                                   const std::size_t descriptor_length,
                                   const std::uint32_t control_timeout_ms) noexcept {
    reset_error();
    if (impl_ == nullptr || descriptor == nullptr || hid_id == 0U || descriptor_length == 0U ||
        descriptor_length > std::numeric_limits<std::uint16_t>::max()) {
        const aoahid_result error = descriptor_length > std::numeric_limits<std::uint16_t>::max()
                                        ? AOAHID_ERR_OVERFLOW
                                        : AOAHID_ERR_PARAM;
        record_error(error, 0, accessory_register_hid, hid_id, 0U,
                     static_cast<std::uint32_t>(std::min<std::size_t>(
                         descriptor_length, std::numeric_limits<std::uint32_t>::max())));
        return error;
    }
    {
        const Impl::StateGuard guard(*impl_);
        if (!impl_->present) {
            record_error(AOAHID_ERR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, accessory_register_hid,
                         hid_id, 0U, static_cast<std::uint32_t>(descriptor_length));
            return AOAHID_ERR_NO_DEVICE;
        }
        if (impl_->closing) {
            record_error(AOAHID_ERR_IO, LIBUSB_TRANSFER_CANCELLED, accessory_register_hid, hid_id,
                         0U, static_cast<std::uint32_t>(descriptor_length));
            return AOAHID_ERR_IO;
        }
    }

    aoahid_result result = control_out(
        impl_->handle, accessory_register_hid, hid_id,
        static_cast<std::uint16_t>(descriptor_length), nullptr, 0U,
        ControlOutPolicy{control_timeout_ms, static_cast<std::uint32_t>(descriptor_length)});
    impl_->observe_control_result(result);
    if (result != AOAHID_OK) {
        const ErrorInfo failure = last_error();
        if (result != AOAHID_ERR_NO_DEVICE && control_outcome_may_be_accepted(result)) {
            // Request 54 may have inserted an incomplete entry in the gadget's
            // new_hid_list even when the host cannot establish completion. A
            // best-effort request 55 prevents an unreachable registration from
            // surviving for the rest of this USB connection.
            const aoahid_result cleanup =
                control_out(impl_->handle, accessory_unregister_hid, hid_id, 0U, nullptr, 0U,
                            ControlOutPolicy{control_timeout_ms});
            impl_->observe_control_result(cleanup);
        }
        restore_error(failure);
        return result;
    }

    // Fragment size is caller policy (64 bytes is conservative), not an AOA
    // protocol constant. Offsets nevertheless must be exact and ascending.
    std::size_t offset = 0U;
    while (offset < descriptor_length) {
        const std::size_t fragment_length = std::min<std::size_t>(
            impl_->config.descriptor_fragment_bytes, descriptor_length - offset);
        result = control_out(impl_->handle, accessory_set_hid_report_desc, hid_id,
                             static_cast<std::uint16_t>(offset), descriptor + offset,
                             static_cast<std::uint16_t>(fragment_length),
                             ControlOutPolicy{control_timeout_ms});
        impl_->observe_control_result(result);
        if (result != AOAHID_OK) {
            ErrorInfo failure = last_error();
            if (result != AOAHID_ERR_NO_DEVICE) {
                const aoahid_result cleanup =
                    control_out(impl_->handle, accessory_unregister_hid, hid_id, 0U, nullptr, 0U,
                                ControlOutPolicy{control_timeout_ms});
                impl_->observe_control_result(cleanup);
            }
            record_error(failure.result, failure.native_status, failure.aoa_request, failure.hid_id,
                         failure.offset, failure.length);
            return failure.result;
        }
        offset += fragment_length;
    }
    return AOAHID_OK;
}

aoahid_result Device::unregister_hid(const std::uint16_t hid_id,
                                     const std::uint32_t control_timeout_ms) noexcept {
    reset_error();
    if (impl_ == nullptr || hid_id == 0U) {
        record_error(AOAHID_ERR_PARAM, 0, accessory_unregister_hid, hid_id);
        return AOAHID_ERR_PARAM;
    }
    {
        const Impl::StateGuard guard(*impl_);
        if (!impl_->present) {
            record_error(AOAHID_ERR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, accessory_unregister_hid,
                         hid_id);
            return AOAHID_ERR_NO_DEVICE;
        }
    }
    const aoahid_result result = control_out(impl_->handle, accessory_unregister_hid, hid_id, 0U,
                                             nullptr, 0U, ControlOutPolicy{control_timeout_ms});
    impl_->observe_control_result(result);
    return result;
}

aoahid_result Device::set_reservation(const std::uint16_t hid_id, const std::size_t count,
                                      ReservationToken* out) {
    reset_error();
    if (out == nullptr) {
        record_error(AOAHID_ERR_PARAM, 0, 0, hid_id);
        return AOAHID_ERR_PARAM;
    }
    *out = ReservationToken{};
    if (impl_ == nullptr || hid_id == 0U) {
        record_error(AOAHID_ERR_PARAM, 0, 0, hid_id);
        return AOAHID_ERR_PARAM;
    }
    auto reservation = std::unique_ptr<Impl::Reservation>(new (std::nothrow) Impl::Reservation());
    if (reservation == nullptr) {
        record_error(AOAHID_ERR_INTERNAL, LIBUSB_ERROR_NO_MEM, 0, hid_id);
        return AOAHID_ERR_INTERNAL;
    }
    const Impl::StateGuard guard(*impl_);
    if (!impl_->present) {
        record_error(AOAHID_ERR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, 0, hid_id);
        return AOAHID_ERR_NO_DEVICE;
    }
    if (impl_->closing) {
        record_error(AOAHID_ERR_IO, LIBUSB_TRANSFER_CANCELLED, 0, hid_id);
        return AOAHID_ERR_IO;
    }

    const bool duplicate =
        std::any_of(impl_->reservations.begin(), impl_->reservations.end(),
                    [hid_id](const std::unique_ptr<Impl::Reservation>& existing) {
                        return existing != nullptr && existing->hid_id == hid_id;
                    });
    if (duplicate ||
        (count > 0U && (count >= impl_->slots.size() ||
                        impl_->reserved_capacity_total >= impl_->slots.size() - count))) {
        record_error(AOAHID_ERR_PARAM, 0, 0, hid_id, 0U,
                     static_cast<std::uint32_t>(
                         std::min<std::size_t>(count, std::numeric_limits<std::uint32_t>::max())));
        return AOAHID_ERR_PARAM;
    }
    reservation->owner = impl_;
    reservation->hid_id = hid_id;
    reservation->capacity = count;
    Impl::Reservation* token = reservation.get();
    impl_->reservations.push_back(std::move(reservation));
    impl_->reserved_capacity_total += count;
    impl_->unfilled_reserved_total += count;
    out->value = token;
    return AOAHID_OK;
}

aoahid_result Device::clear_reservation(const ReservationToken token) noexcept {
    reset_error();
    auto* reservation = static_cast<Impl::Reservation*>(token.value);
    if (impl_ == nullptr || reservation == nullptr) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    const Impl::StateGuard guard(*impl_);
    const auto found =
        std::find_if(impl_->reservations.begin(), impl_->reservations.end(),
                     [reservation](const std::unique_ptr<Impl::Reservation>& candidate) {
                         return candidate.get() == reservation;
                     });
    if (found == impl_->reservations.end()) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    reservation = found->get();
    if (reservation->in_use != 0U) {
        record_error(AOAHID_ERR_BUSY, 0, 0, reservation->hid_id);
        return AOAHID_ERR_BUSY;
    }
    impl_->reserved_capacity_total -= reservation->capacity;
    impl_->unfilled_reserved_total -= reservation->capacity;
    impl_->reservations.erase(found);
    return AOAHID_OK;
}

aoahid_result Device::acquire(const std::uint16_t hid_id, const ReservationToken reservation,
                              const std::size_t report_length, void* user,
                              const Completion completion, PreparedTransfer* out) noexcept {
    reset_error();
    if (out == nullptr) {
        record_error(AOAHID_ERR_PARAM, 0, accessory_send_hid_event, hid_id);
        return AOAHID_ERR_PARAM;
    }
    *out = PreparedTransfer{};
    if (impl_ == nullptr || hid_id == 0U || report_length == 0U ||
        report_length > impl_->config.maximum_report_bytes || completion == nullptr) {
        const aoahid_result result = report_length > std::numeric_limits<std::uint16_t>::max()
                                         ? AOAHID_ERR_OVERFLOW
                                         : AOAHID_ERR_PARAM;
        record_error(result, 0, accessory_send_hid_event, hid_id, 0U,
                     static_cast<std::uint32_t>(std::min<std::size_t>(
                         report_length, std::numeric_limits<std::uint32_t>::max())));
        return result;
    }

    const Impl::StateGuard guard(*impl_);
    auto* own_reservation = static_cast<Impl::Reservation*>(reservation.value);
    if (own_reservation != nullptr &&
        (own_reservation->owner != impl_ || own_reservation->hid_id != hid_id)) {
        record_error(AOAHID_ERR_PARAM, 0, accessory_send_hid_event, hid_id, 0U,
                     static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_PARAM;
    }
    if (!impl_->present) {
        record_error(AOAHID_ERR_NO_DEVICE, LIBUSB_ERROR_NO_DEVICE, accessory_send_hid_event, hid_id,
                     0U, static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_NO_DEVICE;
    }
    if (impl_->closing) {
        record_error(AOAHID_ERR_IO, LIBUSB_TRANSFER_CANCELLED, accessory_send_hid_event, hid_id, 0U,
                     static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_IO;
    }
    if (impl_->free_stack.empty()) {
        record_error(AOAHID_ERR_BUSY, 0, accessory_send_hid_event, hid_id, 0U,
                     static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_BUSY;
    }

    const bool filling_own_reservation =
        own_reservation != nullptr && own_reservation->in_use < own_reservation->capacity;
    if (!filling_own_reservation && impl_->free_stack.size() <= impl_->unfilled_reserved_total) {
        record_error(AOAHID_ERR_BUSY, 0, accessory_send_hid_event, hid_id, 0U,
                     static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_BUSY;
    }

    Impl::Slot* slot = impl_->free_stack.back();
    impl_->free_stack.pop_back();
    slot->state = Impl::SlotState::prepared;
    slot->user = user;
    slot->completion = completion;
    slot->hid_id = hid_id;
    slot->report_id = 0U;
    slot->report_length = report_length;
    slot->reservation = own_reservation;
    slot->reservation_counted = filling_own_reservation;
    if (own_reservation != nullptr) {
        ++own_reservation->in_use;
        if (filling_own_reservation) {
            --impl_->unfilled_reserved_total;
        }
    }
    ++impl_->outstanding;

    // The three invariant setup fields were initialized with the pool. HID ID
    // and wLength vary for every acquisition because this pool is shared by all
    // nodes on the device.
    store_le16(slot->buffer.data() + 2U, hid_id);
    store_le16(slot->buffer.data() + 6U, static_cast<std::uint16_t>(report_length));
    out->slot = slot;
    out->payload = slot->buffer.data() + LIBUSB_CONTROL_SETUP_SIZE;
    out->capacity = impl_->config.maximum_report_bytes;
    return AOAHID_OK;
}

aoahid_result Device::submit(const PreparedTransfer prepared, const std::size_t report_length,
                             const std::uint16_t report_id,
                             const std::uint32_t send_timeout_ms) noexcept {
    reset_error();
    const aoahid_result result =
        impl_ == nullptr
            ? AOAHID_ERR_PARAM
            : impl_->submit_prepared(prepared, report_length, report_id, send_timeout_ms);
    if (result != AOAHID_OK && last_error().result == AOAHID_OK) {
        record_error(result, 0, accessory_send_hid_event, 0U, 0U,
                     static_cast<std::uint32_t>(std::min<std::size_t>(
                         report_length, std::numeric_limits<std::uint32_t>::max())));
    }
    return result;
}

void Device::abandon(const PreparedTransfer prepared) noexcept {
    if (impl_ == nullptr) {
        return;
    }
    auto* slot = static_cast<Impl::Slot*>(prepared.slot);
    if (slot == nullptr || slot->owner != impl_) {
        return;
    }
    const Impl::StateGuard guard(*impl_);
    if (slot->state != Impl::SlotState::prepared) {
        return;
    }
    impl_->release_reservation_locked(slot);
    slot->state = Impl::SlotState::free;
    slot->user = nullptr;
    slot->completion = nullptr;
    slot->report_id = 0U;
    slot->report_length = 0U;
    if (impl_->outstanding > 0U) {
        --impl_->outstanding;
    }
    impl_->free_stack.push_back(slot);
}

aoahid_result Device::cancel_all() noexcept {
    reset_error();
    if (impl_ == nullptr) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    {
        const Impl::StateGuard guard(*impl_);
        impl_->closing = true;
    }

    aoahid_result result = AOAHID_OK;
    // Slots and transfers have stable addresses for the Device lifetime. Walk
    // them directly so this destructor-reachable path performs no allocation.
    // A submitted callback may win the race before cancel; libusb documents
    // NOT_FOUND for a transfer that is no longer in flight, which is benign.
    for (Impl::Slot& slot : impl_->slots) {
        Impl::Notice notice{};
        libusb_transfer* transfer = nullptr;
        std::uint16_t cancelled_hid_id = 0U;
        std::uint32_t cancelled_length = 0U;
        {
            const Impl::StateGuard guard(*impl_);
            if (slot.state == Impl::SlotState::submitted) {
                transfer = slot.transfer;
                cancelled_hid_id = slot.hid_id;
                cancelled_length = static_cast<std::uint32_t>(std::min<std::size_t>(
                    slot.report_length, std::numeric_limits<std::uint32_t>::max()));
            } else if (slot.state == Impl::SlotState::prepared) {
                notice = impl_->finish_locked(&slot, AOAHID_ERR_IO, LIBUSB_TRANSFER_CANCELLED);
            }
        }
        if (notice.completion != nullptr) {
            impl_->deliver_terminal(notice);
        }
        if (transfer != nullptr) {
            const int status = libusb_cancel_transfer(transfer);
            if (status != LIBUSB_SUCCESS && status != LIBUSB_ERROR_NOT_FOUND &&
                result == AOAHID_OK) {
                result = map_libusb_error(status, false);
                record_error(result, status, accessory_send_hid_event, cancelled_hid_id, 0U,
                             cancelled_length);
            }
        }
    }
    return result;
}

bool Device::drained() const noexcept {
    if (impl_ == nullptr) {
        return true;
    }
    const Impl::StateGuard guard(*impl_);
    return impl_->outstanding == 0U && impl_->callbacks_active == 0U;
}

bool Device::present() const noexcept {
    if (impl_ == nullptr) {
        return false;
    }
    const Impl::StateGuard guard(*impl_);
    return impl_->present;
}

aoahid_result Device::latched_error(ErrorInfo* detail) noexcept {
    if (impl_ == nullptr) {
        if (detail != nullptr) {
            *detail = ErrorInfo{AOAHID_ERR_PARAM, 0, 0, 0U, 0U, 0U};
        }
        return AOAHID_ERR_PARAM;
    }
    const Impl::StateGuard guard(*impl_);
    const ErrorInfo error = impl_->latched;
    const aoahid_result result = error.result;
    if (detail != nullptr) {
        *detail = error;
    }
    if (result != AOAHID_ERR_NO_DEVICE) {
        impl_->latched = ErrorInfo{};
    }
    return result;
}

Port* Device::port() const noexcept { return impl_ == nullptr ? nullptr : impl_->port; }

} // namespace aoa::transport
