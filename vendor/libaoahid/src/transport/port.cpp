// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Port: sole owner of one physical device's libusb handle and interface
 * claims, with a Runtime-level registry, plus the explicit accessory-mode
 * start. Only open/close paths touch it; the report send path uses the
 * borrowed handle directly.
 */

#include "transport/libusb_include.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>

namespace {

// libusb_get_port_numbers() documents the USB 3.0 topology depth limit as 7.
constexpr std::size_t maximum_port_depth = 7U;
constexpr std::uint8_t request_type_out_vendor_device = 0x40U;
constexpr std::uint8_t request_type_in_vendor_device = 0xC0U;
constexpr std::uint8_t accessory_get_protocol = 51U;
constexpr std::uint8_t accessory_send_string = 52U;
constexpr std::uint8_t accessory_start = 53U;
// AOA 1.0/2.0: Google VID 0x18D1 with PID 0x2D00-0x2D05 is accessory mode.
constexpr std::uint16_t accessory_vendor_id = 0x18D1U;
constexpr std::uint16_t accessory_product_first = 0x2D00U;
constexpr std::uint16_t accessory_product_last = 0x2D05U;
constexpr std::size_t accessory_string_count = 6U;

bool port_path_matches(libusb_device* device, const std::vector<std::uint8_t>& expected) noexcept {
    if (device == nullptr || expected.empty() || expected.size() > maximum_port_depth) {
        return false;
    }
    std::array<std::uint8_t, maximum_port_depth> ports{};
    const int count = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    return count == static_cast<int>(expected.size()) &&
           std::equal(expected.begin(), expected.end(), ports.begin());
}

bool original_identity_matches(libusb_device* device, const libusb_device_descriptor& descriptor,
                               const aoa::transport::Candidate& candidate) {
    if (libusb_get_bus_number(device) != candidate.bus ||
        descriptor.idVendor != candidate.vendor_id ||
        descriptor.idProduct != candidate.product_id) {
        return false;
    }
    if (!candidate.port_path.empty()) {
        return port_path_matches(device, candidate.port_path);
    }
    return libusb_get_device_address(device) == candidate.address;
}

int open_original(libusb_context* context, const aoa::transport::Candidate& candidate,
                  libusb_device_handle** out, std::uint8_t* address) {
    *out = nullptr;
    libusb_device** devices = nullptr;
    const auto count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        return static_cast<int>(count);
    }

    int result = LIBUSB_ERROR_NO_DEVICE;
    for (std::size_t index = 0U; index < static_cast<std::size_t>(count); ++index) {
        libusb_device* device = devices[index];
        libusb_device_descriptor descriptor{};
        if (device == nullptr ||
            libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS ||
            !original_identity_matches(device, descriptor, candidate)) {
            continue;
        }
        *address = libusb_get_device_address(device);
        result = libusb_open(device, out);
        break;
    }
    libusb_free_device_list(devices, 1);
    return result;
}

// One AOA control request as the diagnostic record reports it.
struct ControlRequest {
    std::uint8_t request{};
    std::uint32_t offset{};
    std::uint32_t length{};
};

aoahid_result control_result(const int status, const ControlRequest sent) noexcept {
    aoahid_result result = AOAHID_OK;
    if (status < 0) {
        result = aoa::transport::map_libusb_error(status, sent.request == accessory_get_protocol);
    } else if (static_cast<std::uint32_t>(status) != sent.length) {
        result = AOAHID_ERR_SHORT_TRANSFER;
    }
    if (result != AOAHID_OK) {
        aoa::transport::record_error(result, status, sent.request, 0U, sent.offset, sent.length);
    }
    return result;
}

} // namespace

namespace aoa::transport {

aoahid_result Runtime::acquire_port(const Candidate& candidate, Port** out) {
    *out = nullptr;
    auto* context = static_cast<libusb_context*>(context_);
    if (context == nullptr) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    auto* port = new (std::nothrow) Port();
    if (port == nullptr) {
        record_error(AOAHID_ERR_INTERNAL, LIBUSB_ERROR_NO_MEM);
        return AOAHID_ERR_INTERNAL;
    }
    libusb_device_handle* handle = nullptr;
    std::uint8_t address = 0U;
    const int status = open_original(context, candidate, &handle, &address);
    if (status != LIBUSB_SUCCESS || handle == nullptr) {
        delete port;
        const aoahid_result result = map_libusb_error(status, false);
        record_error(result, status);
        return result;
    }
    port->runtime_ = this;
    port->handle_ = handle;
    port->bus_ = candidate.bus;
    port->address_ = address;
    std::unique_lock<std::mutex> guard(ports_mutex_, std::defer_lock);
    if (event_mode_ == AOAHID_EVENT_INTERNAL_THREAD)
        guard.lock();
    try {
        port->claims_.reserve(1U);
        ports_.push_back(port);
    } catch (...) {
        libusb_close(handle);
        delete port;
        throw;
    }
    port->references_.store(1U, std::memory_order_relaxed);
    *out = port;
    return AOAHID_OK;
}

Port* Runtime::retain_port(const std::uint8_t bus, const std::uint8_t address) noexcept {
    std::unique_lock<std::mutex> guard(ports_mutex_, std::defer_lock);
    if (event_mode_ == AOAHID_EVENT_INTERNAL_THREAD)
        guard.lock();
    for (Port* port : ports_) {
        if (port->bus_ == bus && port->address_ == address && port->try_retain()) {
            return port;
        }
    }
    return nullptr;
}

void Runtime::unregister_port(Port* port) noexcept {
    std::unique_lock<std::mutex> guard(ports_mutex_, std::defer_lock);
    if (event_mode_ == AOAHID_EVENT_INTERNAL_THREAD)
        guard.lock();
    const auto found = std::find(ports_.begin(), ports_.end(), port);
    if (found != ports_.end()) {
        ports_.erase(found);
    }
}

aoahid_result Runtime::start_accessory(const Candidate& candidate, const char* const strings[6],
                                       const std::uint32_t control_timeout_ms) {
    reset_error();
    if (candidate.vendor_id == accessory_vendor_id &&
        candidate.product_id >= accessory_product_first &&
        candidate.product_id <= accessory_product_last) {
        return AOAHID_OK;
    }
    // An open Device on this device lends its handle; a second open would
    // fail on WinUSB.
    Port* port = retain_port(candidate.bus, candidate.address);
    if (port == nullptr) {
        const aoahid_result opened = acquire_port(candidate, &port);
        if (opened != AOAHID_OK || port == nullptr) {
            return opened;
        }
    }
    auto* handle = static_cast<libusb_device_handle*>(port->native_handle());

    std::array<unsigned char, 2U> version{};
    int status = libusb_control_transfer(
        handle, request_type_in_vendor_device, accessory_get_protocol, 0U, 0U, version.data(),
        static_cast<std::uint16_t>(version.size()), control_timeout_ms);
    aoahid_result result = control_result(status, ControlRequest{accessory_get_protocol, 0U, 2U});
    if (result == AOAHID_ERR_SHORT_TRANSFER ||
        (result == AOAHID_OK && version[0] == 0U && version[1] == 0U)) {
        // AOA defines request 51 as a two-byte, nonzero protocol version.
        record_error(AOAHID_ERR_NOT_AOA, status, accessory_get_protocol, 0U, 0U, 2U);
        result = AOAHID_ERR_NOT_AOA;
    }
    for (std::size_t index = 0U; result == AOAHID_OK && index < accessory_string_count; ++index) {
        if (strings[index] == nullptr) {
            continue;
        }
        // AOA 1.0 sends each string with its terminating NUL.
        const std::size_t length = std::strlen(strings[index]) + 1U;
        if (length > std::numeric_limits<std::uint16_t>::max()) {
            record_error(AOAHID_ERR_OVERFLOW, 0, accessory_send_string, 0U,
                         static_cast<std::uint32_t>(index),
                         std::numeric_limits<std::uint32_t>::max());
            result = AOAHID_ERR_OVERFLOW;
            break;
        }
        status = libusb_control_transfer(
            handle, request_type_out_vendor_device, accessory_send_string, 0U,
            static_cast<std::uint16_t>(index),
            reinterpret_cast<unsigned char*>(const_cast<char*>(strings[index])),
            static_cast<std::uint16_t>(length), control_timeout_ms);
        result = control_result(status, ControlRequest{accessory_send_string,
                                                       static_cast<std::uint32_t>(index),
                                                       static_cast<std::uint32_t>(length)});
    }
    if (result == AOAHID_OK) {
        status = libusb_control_transfer(handle, request_type_out_vendor_device, accessory_start,
                                         0U, 0U, nullptr, 0U, control_timeout_ms);
        result = control_result(status, ControlRequest{accessory_start, 0U, 0U});
    }
    const ErrorInfo error = last_error();
    port->release();
    if (result != AOAHID_OK) {
        record_error(error.result, error.native_status, error.aoa_request, error.hid_id,
                     error.offset, error.length);
    }
    return result;
}

bool Port::try_retain() noexcept {
    // A Port whose count already reached zero is being torn down by its last
    // releaser; resurrecting it would close the handle under the new owner.
    std::size_t current = references_.load(std::memory_order_relaxed);
    while (current != 0U) {
        if (references_.compare_exchange_weak(current, current + 1U, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Port::retain() noexcept { references_.fetch_add(1U, std::memory_order_relaxed); }

void Port::release() noexcept {
    if (references_.fetch_sub(1U, std::memory_order_acq_rel) != 1U) {
        return;
    }
    // Unregister before closing so discovery cannot retain a Port whose
    // handle is about to close.
    if (runtime_ != nullptr) {
        runtime_->unregister_port(this);
    }
    auto* handle = static_cast<libusb_device_handle*>(handle_);
    for (const Claim& claim : claims_) {
        static_cast<void>(libusb_release_interface(handle, claim.interface_number));
    }
    libusb_close(handle);
    delete this;
}

void* Port::native_handle() const noexcept { return handle_; }

std::size_t Port::references() const noexcept {
    return references_.load(std::memory_order_acquire);
}

std::int32_t Port::claim_interface(const std::int32_t interface_number) noexcept {
    const std::lock_guard<std::mutex> guard(claims_mutex_);
    const auto found =
        std::find_if(claims_.begin(), claims_.end(), [interface_number](const Claim& claim) {
            return claim.interface_number == interface_number;
        });
    if (found != claims_.end()) {
        ++found->count;
        return LIBUSB_SUCCESS;
    }
    const int status =
        libusb_claim_interface(static_cast<libusb_device_handle*>(handle_), interface_number);
    if (status != LIBUSB_SUCCESS) {
        return status;
    }
    try {
        claims_.push_back(Claim{interface_number, 1U});
    } catch (...) {
        static_cast<void>(libusb_release_interface(static_cast<libusb_device_handle*>(handle_),
                                                   interface_number));
        return LIBUSB_ERROR_NO_MEM;
    }
    return LIBUSB_SUCCESS;
}

void Port::release_interface(const std::int32_t interface_number) noexcept {
    const std::lock_guard<std::mutex> guard(claims_mutex_);
    const auto found =
        std::find_if(claims_.begin(), claims_.end(), [interface_number](const Claim& claim) {
            return claim.interface_number == interface_number;
        });
    if (found == claims_.end() || --found->count != 0U) {
        return;
    }
    claims_.erase(found);
    static_cast<void>(
        libusb_release_interface(static_cast<libusb_device_handle*>(handle_), interface_number));
}

} // namespace aoa::transport
