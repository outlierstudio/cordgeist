// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Bulk IN/OUT channel on a shared Port. Buffers and transfers are allocated at
 * open; read/write/callback never allocate. The callback publishes a slot index
 * into an SPSC ring and wakes a waiter; it takes the waiter mutex only when a
 * thread is actually blocked, the same pattern Node completion uses.
 */

#include "transport/libusb_include.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <new>
#include <vector>

namespace {

constexpr std::uint32_t no_slot = std::numeric_limits<std::uint32_t>::max();

class IndexRing final {
  public:
    void init(const std::size_t capacity) {
        std::size_t size = 1U;
        while (size < capacity) {
            size <<= 1U;
        }
        slots_.assign(size, 0U);
        mask_ = size - 1U;
    }

    bool push(const std::uint32_t value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_.load(std::memory_order_acquire) == slots_.size()) {
            return false;
        }
        slots_[tail & mask_] = value;
        // seq_cst pairs with Waiter: see notify().
        tail_.store(tail + 1U, std::memory_order_seq_cst);
        return true;
    }

    bool pop(std::uint32_t* value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_.load(std::memory_order_acquire)) {
            return false;
        }
        *value = slots_[head & mask_];
        head_.store(head + 1U, std::memory_order_release);
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_seq_cst);
    }

  private:
    std::vector<std::uint32_t> slots_;
    std::size_t mask_{};
    std::atomic<std::size_t> head_{0U};
    std::atomic<std::size_t> tail_{0U};
};

class Waiter final {
  public:
    void notify() noexcept {
        // The publisher's seq_cst store and this seq_cst load pair with the
        // waiter's seq_cst fetch_add and predicate load: in the single total
        // order, either the waiter sees the published state or this side sees
        // the waiter and locks to wake it. No wake-up can be lost.
        if (waiters_.load(std::memory_order_seq_cst) != 0U) {
            const std::lock_guard<std::mutex> guard(mutex_);
            cv_.notify_all();
        }
    }

    template <typename Predicate>
    void wait_until(const std::chrono::steady_clock::time_point deadline, Predicate ready) {
        waiters_.fetch_add(1U, std::memory_order_seq_cst);
        {
            std::unique_lock<std::mutex> lock(mutex_);
            static_cast<void>(cv_.wait_until(lock, deadline, ready));
        }
        waiters_.fetch_sub(1U, std::memory_order_relaxed);
    }

  private:
    std::atomic<std::uint32_t> waiters_{0U};
    std::mutex mutex_;
    std::condition_variable cv_;
};

aoahid_result status_result(const libusb_transfer_status status) noexcept {
    switch (status) {
    case LIBUSB_TRANSFER_COMPLETED:
        return AOAHID_OK;
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

std::chrono::steady_clock::time_point deadline_after(const std::uint32_t timeout_ms) noexcept {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
}

std::uint32_t remaining_ms(const std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    return remaining <= 0 ? 0U : static_cast<std::uint32_t>(remaining);
}

} // namespace

namespace aoa::transport {

struct Channel::Impl {
    struct Slot {
        Impl* owner{};
        libusb_transfer* transfer{};
        std::vector<std::uint8_t> buffer;
        std::uint32_t index{};
        bool in{};
    };

    Port* port{};
    libusb_device_handle* handle{};
    int interface_number{-1};
    bool claimed{};
    unsigned char endpoint_in{};
    unsigned char endpoint_out{};
    std::uint32_t max_packet{};
    std::uint32_t transfer_bytes{};
    bool zero_length_termination{};
    bool request_reads{};
    // Caller-poll mode: waits drive libusb events on the calling thread.
    Runtime* pump{};
    std::vector<Slot> in_slots;
    std::vector<Slot> out_slots;
    IndexRing ready_in;
    IndexRing free_out;
    Waiter in_waiter;
    Waiter out_waiter;
    std::atomic<std::size_t> in_flight{0U};
    std::atomic<bool> lost{false};
    std::atomic<aoahid_result> write_error{AOAHID_OK};
    // Reader-owned.
    std::uint32_t current{no_slot};
    std::size_t offset{};
    // Request mode: the single IN transfer is submitted and not yet consumed.
    bool request_pending{};
    // Writer-owned: a slot whose submit failed is reused before the ring.
    std::uint32_t spare{no_slot};

    void mark_lost() noexcept {
        lost.store(true, std::memory_order_seq_cst);
        in_waiter.notify();
        out_waiter.notify();
    }

    static void LIBUSB_CALL callback(libusb_transfer* transfer) noexcept {
        auto* slot = static_cast<Slot*>(transfer->user_data);
        Impl* owner = slot->owner;
        if (slot->in) {
            static_cast<void>(owner->ready_in.push(slot->index));
            owner->in_waiter.notify();
        } else {
            const bool complete = transfer->status == LIBUSB_TRANSFER_COMPLETED &&
                                  transfer->actual_length == transfer->length;
            if (!complete) {
                const aoahid_result error = transfer->status == LIBUSB_TRANSFER_COMPLETED
                                                ? AOAHID_ERR_SHORT_TRANSFER
                                                : status_result(transfer->status);
                aoahid_result expected = AOAHID_OK;
                static_cast<void>(owner->write_error.compare_exchange_strong(
                    expected, error, std::memory_order_acq_rel));
                if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
                    owner->lost.store(true, std::memory_order_seq_cst);
                    owner->in_waiter.notify();
                }
            }
            static_cast<void>(owner->free_out.push(slot->index));
            owner->out_waiter.notify();
        }
        // Last access: a closer may free the Channel once this reaches zero.
        owner->in_flight.fetch_sub(1U, std::memory_order_acq_rel);
    }

    aoahid_result submit(Slot& slot, const int length, const unsigned char endpoint) noexcept {
        libusb_fill_bulk_transfer(slot.transfer, handle, endpoint, slot.buffer.data(), length,
                                  callback, &slot, 0U);
        in_flight.fetch_add(1U, std::memory_order_acq_rel);
        const int status = libusb_submit_transfer(slot.transfer);
        if (status != LIBUSB_SUCCESS) {
            in_flight.fetch_sub(1U, std::memory_order_acq_rel);
            return map_libusb_error(status, false);
        }
        // lose() may have run between the caller's lost check and this submit.
        if (lost.load(std::memory_order_acquire)) {
            static_cast<void>(libusb_cancel_transfer(slot.transfer));
        }
        return AOAHID_OK;
    }

    aoahid_result submit_in(Slot& slot, const std::uint32_t length) noexcept {
        if (lost.load(std::memory_order_acquire)) {
            return AOAHID_ERR_NO_DEVICE;
        }
        const aoahid_result result = submit(slot, static_cast<int>(length), endpoint_in);
        if (result != AOAHID_OK) {
            mark_lost();
        }
        return result;
    }

    void release_resources() noexcept {
        for (auto* slots : {&in_slots, &out_slots}) {
            for (Slot& slot : *slots) {
                if (slot.transfer != nullptr) {
                    libusb_free_transfer(slot.transfer);
                    slot.transfer = nullptr;
                }
            }
        }
        if (claimed) {
            port->release_interface(interface_number);
        }
        port->release();
    }
};

namespace {

struct BulkInterface {
    int number{-1};
    unsigned char endpoint_in{};
    unsigned char endpoint_out{};
    std::uint32_t max_packet{};
};

int find_bulk_interface(libusb_device_handle* handle, const ChannelConfig& config,
                        BulkInterface* out) {
    libusb_config_descriptor* descriptor = nullptr;
    const int status = libusb_get_active_config_descriptor(libusb_get_device(handle), &descriptor);
    if (status != LIBUSB_SUCCESS) {
        return status;
    }
    for (int index = 0; index < descriptor->bNumInterfaces && out->number < 0; ++index) {
        const libusb_interface& candidate = descriptor->interface[index];
        if (candidate.num_altsetting < 1) {
            continue;
        }
        const libusb_interface_descriptor& setting = candidate.altsetting[0];
        if (setting.bInterfaceClass != config.interface_class ||
            setting.bInterfaceSubClass != config.interface_subclass ||
            setting.bInterfaceProtocol != config.interface_protocol) {
            continue;
        }
        BulkInterface found{};
        for (int endpoint = 0; endpoint < setting.bNumEndpoints; ++endpoint) {
            const libusb_endpoint_descriptor& value = setting.endpoint[endpoint];
            if ((value.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
                LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK) {
                continue;
            }
            // wMaxPacketSize bits 10..0 hold the packet size.
            const std::uint32_t packet = value.wMaxPacketSize & 0x07FFU;
            if ((value.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0U) {
                if (found.endpoint_in == 0U) {
                    found.endpoint_in = value.bEndpointAddress;
                    found.max_packet = std::max(found.max_packet, packet);
                }
            } else if (found.endpoint_out == 0U) {
                found.endpoint_out = value.bEndpointAddress;
                found.max_packet = std::max(found.max_packet, packet);
            }
        }
        if (found.endpoint_in != 0U && found.endpoint_out != 0U && found.max_packet != 0U) {
            found.number = setting.bInterfaceNumber;
            *out = found;
        }
    }
    libusb_free_config_descriptor(descriptor);
    return out->number < 0 ? LIBUSB_ERROR_NOT_FOUND : LIBUSB_SUCCESS;
}

} // namespace

aoahid_result Channel::open(Port* port, const ChannelConfig& config, Channel** out) {
    reset_error();
    *out = nullptr;
    // REQUEST reads keep no IN transfers queued, so in_transfers is not used there.
    if (port == nullptr || (!config.request_reads && config.in_transfers == 0U) ||
        config.out_transfers == 0U || config.transfer_bytes == 0U) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    auto* handle = static_cast<libusb_device_handle*>(port->native_handle());
    // AOA 1.0 has the host select configuration 1 before using the accessory
    // endpoints. Only an unconfigured device is configured: libusb documents a
    // SET_CONFIGURATION of the active value as a lightweight device reset.
    int configuration = 0;
    int configured = libusb_get_configuration(handle, &configuration);
    if (configured == LIBUSB_SUCCESS && configuration == 0) {
        configured = libusb_set_configuration(handle, 1);
    }
    if (configured != LIBUSB_SUCCESS) {
        const aoahid_result result = map_libusb_error(configured, false);
        record_error(result, configured);
        return result;
    }
    BulkInterface bulk{};
    const int found = find_bulk_interface(handle, config, &bulk);
    if (found != LIBUSB_SUCCESS) {
        const aoahid_result result = found == LIBUSB_ERROR_NOT_FOUND
                                         ? AOAHID_ERR_UNSUPPORTED
                                         : map_libusb_error(found, false);
        record_error(result, found);
        return result;
    }
    const std::uint64_t rounded =
        (static_cast<std::uint64_t>(config.transfer_bytes) + bulk.max_packet - 1U) /
        bulk.max_packet * bulk.max_packet;
    if (rounded > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        record_error(AOAHID_ERR_OVERFLOW);
        return AOAHID_ERR_OVERFLOW;
    }

    const int claim_status = port->claim_interface(bulk.number);
    if (claim_status != LIBUSB_SUCCESS) {
        const aoahid_result result = map_libusb_error(claim_status, false);
        record_error(result, claim_status);
        return result;
    }
    port->retain();

    auto* channel = new (std::nothrow) Channel();
    auto* impl = new (std::nothrow) Impl();
    if (channel == nullptr || impl == nullptr) {
        delete channel;
        delete impl;
        port->release_interface(bulk.number);
        port->release();
        record_error(AOAHID_ERR_INTERNAL, LIBUSB_ERROR_NO_MEM);
        return AOAHID_ERR_INTERNAL;
    }
    impl->port = port;
    impl->handle = handle;
    impl->interface_number = bulk.number;
    impl->claimed = true;
    impl->endpoint_in = bulk.endpoint_in;
    impl->endpoint_out = bulk.endpoint_out;
    impl->max_packet = bulk.max_packet;
    impl->transfer_bytes = static_cast<std::uint32_t>(rounded);
    impl->zero_length_termination = config.zero_length_termination;
    impl->request_reads = config.request_reads;
    impl->pump = config.pump;
    channel->impl_ = impl;
    try {
        const std::uint32_t in_count = config.request_reads ? 1U : config.in_transfers;
        impl->in_slots.resize(in_count);
        impl->out_slots.resize(config.out_transfers);
        impl->ready_in.init(in_count);
        impl->free_out.init(config.out_transfers);
        std::uint32_t index = 0U;
        for (auto* slots : {&impl->in_slots, &impl->out_slots}) {
            index = 0U;
            for (Impl::Slot& slot : *slots) {
                slot.owner = impl;
                slot.index = index++;
                slot.in = slots == &impl->in_slots;
                slot.buffer.resize(impl->transfer_bytes);
                slot.transfer = libusb_alloc_transfer(0);
                if (slot.transfer == nullptr) {
                    delete channel;
                    record_error(AOAHID_ERR_INTERNAL, LIBUSB_ERROR_NO_MEM);
                    return AOAHID_ERR_INTERNAL;
                }
            }
        }
    } catch (...) {
        delete channel;
        throw;
    }
    for (const Impl::Slot& slot : impl->out_slots) {
        static_cast<void>(impl->free_out.push(slot.index));
    }
    // Request mode submits nothing until the first read sizes the transfer.
    for (Impl::Slot& slot : impl->in_slots) {
        if (impl->request_reads) {
            break;
        }
        const aoahid_result submitted = impl->submit_in(slot, impl->transfer_bytes);
        if (submitted != AOAHID_OK) {
            if (impl->in_flight.load(std::memory_order_acquire) == 0U) {
                // Nothing is in flight yet, so the Channel can be freed now.
                delete channel;
                record_error(submitted);
                return submitted;
            }
            // Earlier transfers are in flight; the lost Channel reports this
            // failure through read and must be closed by the caller.
            impl->mark_lost();
            channel->lose();
            break;
        }
    }
    *out = channel;
    return AOAHID_OK;
}

Channel::~Channel() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    if (!drained()) {
        // A late callback would touch freed memory; leak instead, as Device does.
        impl_ = nullptr;
        return;
    }
    impl_->release_resources();
    delete impl_;
    impl_ = nullptr;
}

void Channel::lose() noexcept {
    if (impl_ == nullptr) {
        return;
    }
    impl_->mark_lost();
    for (auto* slots : {&impl_->in_slots, &impl_->out_slots}) {
        for (Impl::Slot& slot : *slots) {
            if (slot.transfer != nullptr) {
                // NOT_FOUND for an idle transfer is expected and harmless.
                static_cast<void>(libusb_cancel_transfer(slot.transfer));
            }
        }
    }
}

bool Channel::drained() const noexcept {
    return impl_ == nullptr || impl_->in_flight.load(std::memory_order_acquire) == 0U;
}

aoahid_result Channel::read(std::uint8_t* buffer, const std::size_t capacity, std::size_t* received,
                            const std::uint32_t timeout_ms) noexcept {
    reset_error();
    if (received != nullptr) {
        *received = 0U;
    }
    if (impl_ == nullptr || buffer == nullptr || received == nullptr || capacity == 0U) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    Impl& impl = *impl_;
    const auto deadline = deadline_after(timeout_ms);
    for (;;) {
        if (impl.current != no_slot) {
            Impl::Slot& slot = impl.in_slots[impl.current];
            const auto available =
                static_cast<std::size_t>(slot.transfer->actual_length) - impl.offset;
            const std::size_t copied = std::min(available, capacity);
            std::memcpy(buffer, slot.buffer.data() + impl.offset, copied);
            impl.offset += copied;
            *received = copied;
            if (copied == available) {
                impl.current = no_slot;
                if (!impl.request_reads) {
                    static_cast<void>(impl.submit_in(slot, impl.transfer_bytes));
                }
            }
            return AOAHID_OK;
        }
        if (impl.request_reads && !impl.request_pending) {
            // Rounding to whole packets keeps a full last packet from
            // overflowing; transfer_bytes is already a packet multiple.
            const std::size_t wanted = std::min<std::size_t>(capacity, impl.transfer_bytes);
            const auto length = static_cast<std::uint32_t>((wanted + impl.max_packet - 1U) /
                                                           impl.max_packet * impl.max_packet);
            const aoahid_result submitted = impl.submit_in(impl.in_slots[0], length);
            if (submitted != AOAHID_OK) {
                record_error(submitted);
                return submitted;
            }
            impl.request_pending = true;
        }
        std::uint32_t index = 0U;
        if (impl.ready_in.pop(&index)) {
            Impl::Slot& slot = impl.in_slots[index];
            impl.request_pending = false;
            const libusb_transfer_status status = slot.transfer->status;
            if (status == LIBUSB_TRANSFER_COMPLETED) {
                if (slot.transfer->actual_length > 0) {
                    impl.current = index;
                    impl.offset = 0U;
                } else if (!impl.request_reads) {
                    // A zero-length packet carries no data; request mode
                    // resubmits at the top of the loop.
                    static_cast<void>(impl.submit_in(slot, impl.transfer_bytes));
                }
                continue;
            }
            const bool already_lost = impl.lost.load(std::memory_order_acquire);
            if (!already_lost) {
                lose();
            }
            // A cancellation caused by lose() is loss, not a transfer error.
            const aoahid_result result =
                already_lost ? AOAHID_ERR_NO_DEVICE : status_result(status);
            record_error(result, static_cast<std::int32_t>(status));
            return result;
        }
        if (impl.lost.load(std::memory_order_acquire)) {
            record_error(AOAHID_ERR_NO_DEVICE);
            return AOAHID_ERR_NO_DEVICE;
        }
        if (timeout_ms == 0U || std::chrono::steady_clock::now() >= deadline) {
            record_error(AOAHID_ERR_TIMEOUT);
            return AOAHID_ERR_TIMEOUT;
        }
        if (impl.pump != nullptr) {
            const aoahid_result pumped = impl.pump->poll(remaining_ms(deadline));
            if (pumped != AOAHID_OK) {
                return pumped;
            }
            continue;
        }
        impl.in_waiter.wait_until(deadline, [&impl]() noexcept {
            return !impl.ready_in.empty() || impl.lost.load(std::memory_order_seq_cst);
        });
    }
}

aoahid_result Channel::write(const std::uint8_t* data, const std::size_t length,
                             std::size_t* written, const std::uint32_t timeout_ms) noexcept {
    reset_error();
    if (written != nullptr) {
        *written = 0U;
    }
    if (impl_ == nullptr || written == nullptr || (data == nullptr && length != 0U)) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    Impl& impl = *impl_;
    if (impl.lost.load(std::memory_order_acquire)) {
        record_error(AOAHID_ERR_NO_DEVICE);
        return AOAHID_ERR_NO_DEVICE;
    }
    const aoahid_result earlier = impl.write_error.exchange(AOAHID_OK, std::memory_order_acq_rel);
    if (earlier != AOAHID_OK) {
        record_error(earlier);
        return earlier;
    }
    const auto deadline = deadline_after(timeout_ms);
    bool zero_length_packet =
        impl.zero_length_termination && length != 0U && length % impl.max_packet == 0U;
    std::size_t offset = 0U;
    while (offset < length || zero_length_packet) {
        std::uint32_t index = impl.spare;
        impl.spare = no_slot;
        while (index == no_slot && !impl.free_out.pop(&index)) {
            index = no_slot;
            if (impl.lost.load(std::memory_order_acquire)) {
                record_error(AOAHID_ERR_NO_DEVICE);
                return AOAHID_ERR_NO_DEVICE;
            }
            if (timeout_ms == 0U || std::chrono::steady_clock::now() >= deadline) {
                record_error(AOAHID_ERR_TIMEOUT);
                return AOAHID_ERR_TIMEOUT;
            }
            if (impl.pump != nullptr) {
                const aoahid_result pumped = impl.pump->poll(remaining_ms(deadline));
                if (pumped != AOAHID_OK) {
                    return pumped;
                }
                continue;
            }
            impl.out_waiter.wait_until(deadline, [&impl]() noexcept {
                return !impl.free_out.empty() || impl.lost.load(std::memory_order_seq_cst);
            });
        }
        Impl::Slot& slot = impl.out_slots[index];
        const std::size_t chunk = std::min<std::size_t>(impl.transfer_bytes, length - offset);
        if (chunk != 0U) {
            std::memcpy(slot.buffer.data(), data + offset, chunk);
        }
        const aoahid_result submitted =
            impl.lost.load(std::memory_order_acquire)
                ? AOAHID_ERR_NO_DEVICE
                : impl.submit(slot, static_cast<int>(chunk), impl.endpoint_out);
        if (submitted != AOAHID_OK) {
            impl.spare = index;
            if (submitted == AOAHID_ERR_NO_DEVICE) {
                impl.mark_lost();
            }
            record_error(submitted);
            return submitted;
        }
        if (chunk == 0U) {
            zero_length_packet = false;
        }
        offset += chunk;
        *written = offset;
    }
    return AOAHID_OK;
}

} // namespace aoa::transport
