// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Runtime ownership, event pumping, and AOA capability discovery. Discovery
 * deliberately probes every accessible USB device with request 51 and keeps
 * negative probes local to that candidate.
 */

#include "transport/libusb_include.hpp"
#include "transport/transport.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <new>

namespace {

constexpr std::uint8_t request_type_in_vendor_device = 0xC0U;
constexpr std::uint8_t accessory_get_protocol = 51U;
// libusb_get_port_numbers() documents the USB 3.0 topology depth limit as 7.
constexpr std::size_t maximum_port_depth = 7U;

void LIBUSB_CALL discard_libusb_log(libusb_context* context, const libusb_log_level level,
                                    const char* message) noexcept {
    static_cast<void>(context);
    static_cast<void>(level);
    static_cast<void>(message);
}

bool supported_libusb_runtime(const libusb_version* version) noexcept {
    return version != nullptr && version->major == 1U && version->minor == 0U &&
           version->micro >= 30U;
}

std::vector<std::uint8_t> port_path(libusb_device* device) {
    std::array<std::uint8_t, maximum_port_depth> ports{};
    const int count = libusb_get_port_numbers(device, ports.data(), static_cast<int>(ports.size()));
    if (count <= 0) {
        return {};
    }
    return {ports.begin(), ports.begin() + count};
}

std::string read_ascii_string(libusb_device_handle* handle, const std::uint8_t index) {
    if (handle == nullptr || index == 0U) {
        return {};
    }
    std::array<unsigned char, 256U> bytes{};
    const int length = libusb_get_string_descriptor_ascii(handle, index, bytes.data(),
                                                          static_cast<int>(bytes.size()));
    if (length <= 0) {
        return {};
    }
    return {reinterpret_cast<const char*>(bytes.data()), static_cast<std::size_t>(length)};
}

bool candidate_less(const aoa::transport::Candidate& left,
                    const aoa::transport::Candidate& right) noexcept {
    if (left.bus != right.bus) {
        return left.bus < right.bus;
    }
    if (left.port_path != right.port_path) {
        return left.port_path < right.port_path;
    }
    return left.address < right.address;
}

} // namespace

namespace aoa::transport {

namespace {
thread_local ErrorInfo g_transport_error{};
}

ErrorInfo last_error() noexcept { return g_transport_error; }

void reset_error() noexcept { g_transport_error = ErrorInfo{}; }

void record_error(const aoahid_result result, const std::int32_t native_status,
                  const std::int32_t aoa_request, const std::uint16_t hid_id,
                  const std::uint32_t offset, const std::uint32_t length) noexcept {
    g_transport_error = ErrorInfo{result, native_status, aoa_request, hid_id, offset, length};
}

aoahid_result map_libusb_error(const std::int32_t status, const bool probe) noexcept {
    switch (status) {
    case LIBUSB_SUCCESS:
        return AOAHID_OK;
    case LIBUSB_ERROR_INVALID_PARAM:
        return AOAHID_ERR_PARAM;
    case LIBUSB_ERROR_ACCESS:
        return AOAHID_ERR_ACCESS;
    case LIBUSB_ERROR_NO_DEVICE:
        return AOAHID_ERR_NO_DEVICE;
    case LIBUSB_ERROR_NOT_FOUND:
    case LIBUSB_ERROR_NOT_SUPPORTED:
        return AOAHID_ERR_UNSUPPORTED;
    case LIBUSB_ERROR_BUSY:
        return AOAHID_ERR_BUSY;
    case LIBUSB_ERROR_TIMEOUT:
        return AOAHID_ERR_TIMEOUT;
    case LIBUSB_ERROR_OVERFLOW:
        return AOAHID_ERR_OVERFLOW;
    case LIBUSB_ERROR_PIPE:
        return probe ? AOAHID_ERR_NOT_AOA : AOAHID_ERR_STALL;
    case LIBUSB_ERROR_NO_MEM:
        return AOAHID_ERR_INTERNAL;
    case LIBUSB_ERROR_INTERRUPTED:
    case LIBUSB_ERROR_IO:
    case LIBUSB_ERROR_OTHER:
    default:
        return AOAHID_ERR_IO;
    }
}

aoahid_result Runtime::create(const aoahid_event_mode event_mode, Runtime** out) noexcept {
    reset_error();
    if (out == nullptr ||
        (event_mode != AOAHID_EVENT_CALLER_POLL && event_mode != AOAHID_EVENT_INTERNAL_THREAD)) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }
    *out = nullptr;
    auto* runtime = new (std::nothrow) Runtime();
    if (runtime == nullptr) {
        record_error(AOAHID_ERR_INTERNAL);
        return AOAHID_ERR_INTERNAL;
    }
    // Keep both settings context-local. Official libusb 1.0.30 documents that
    // LIBUSB_DEBUG and verbose/debug builds can override the level, and its
    // source sends to the global/system sink before a context callback; this
    // therefore avoids process-global interference but cannot override those
    // externally selected behaviors.
    std::array<libusb_init_option, 2U> init_options{};
    init_options[0].option = LIBUSB_OPTION_LOG_LEVEL;
    init_options[0].value.ival = LIBUSB_LOG_LEVEL_NONE;
    init_options[1].option = LIBUSB_OPTION_LOG_CB;
    init_options[1].value.log_cbval = &discard_libusb_log;
    libusb_context* context = nullptr;
    const int status =
        libusb_init_context(&context, init_options.data(), static_cast<int>(init_options.size()));
    if (status != LIBUSB_SUCCESS) {
        delete runtime;
        const aoahid_result result = map_libusb_error(status, false);
        record_error(result, status);
        return result;
    }
    if (!supported_libusb_runtime(libusb_get_version())) {
        libusb_exit(context);
        delete runtime;
        record_error(AOAHID_ERR_UNSUPPORTED);
        return AOAHID_ERR_UNSUPPORTED;
    }
    runtime->event_mode_ = event_mode;
    runtime->context_ = context;
    *out = runtime;
    return AOAHID_OK;
}

Runtime::~Runtime() noexcept {
    if (context_ != nullptr) {
        libusb_exit(static_cast<libusb_context*>(context_));
        context_ = nullptr;
    }
}

void* Runtime::native_context() const noexcept { return context_; }

void Runtime::interrupt_event_handler() noexcept {
    if (context_ != nullptr) {
        libusb_interrupt_event_handler(static_cast<libusb_context*>(context_));
    }
}

aoahid_result Runtime::poll(const std::uint32_t timeout_ms) noexcept {
    reset_error();
    if (context_ == nullptr) {
        record_error(AOAHID_ERR_PARAM);
        return AOAHID_ERR_PARAM;
    }

    const std::uint64_t wait_us = static_cast<std::uint64_t>(timeout_ms) * 1000U;
    timeval timeout{};
    timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(wait_us / 1'000'000U);
    timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(wait_us % 1'000'000U);
    const int status = libusb_handle_events_timeout_completed(
        static_cast<libusb_context*>(context_), &timeout, nullptr);
    if (status == LIBUSB_SUCCESS || status == LIBUSB_ERROR_INTERRUPTED) {
        return AOAHID_OK;
    }
    const aoahid_result result = map_libusb_error(status, false);
    record_error(result, status);
    return result;
}

aoahid_result Runtime::discover(const std::uint32_t timeout_ms, std::vector<Candidate>* out) {
    reset_error();
    if (context_ == nullptr || out == nullptr || timeout_ms == 0U) {
        record_error(AOAHID_ERR_PARAM, 0, accessory_get_protocol, 0U, 0U, 2U);
        return AOAHID_ERR_PARAM;
    }
    out->clear();

    libusb_device** devices = nullptr;
    const auto count = libusb_get_device_list(static_cast<libusb_context*>(context_), &devices);
    if (count < 0) {
        const std::int32_t status = static_cast<std::int32_t>(count);
        const aoahid_result result = map_libusb_error(status, false);
        record_error(result, status);
        return result;
    }

    try {
        for (std::size_t index = 0U; index < static_cast<std::size_t>(count); ++index) {
            libusb_device* device = devices[index];
            libusb_device_descriptor descriptor{};
            if (device == nullptr ||
                libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
                continue;
            }

            // An open Device already owns this device: probe through its handle
            // so the process never holds two handles to one device (WinUSB
            // rejects a second open of a composite device).
            Port* owned =
                retain_port(libusb_get_bus_number(device), libusb_get_device_address(device));
            libusb_device_handle* handle =
                owned == nullptr ? nullptr
                                 : static_cast<libusb_device_handle*>(owned->native_handle());
            if (owned == nullptr &&
                (libusb_open(device, &handle) != LIBUSB_SUCCESS || handle == nullptr)) {
                continue;
            }
            const auto close_handle = [owned, handle]() noexcept {
                if (owned != nullptr) {
                    owned->release();
                } else {
                    libusb_close(handle);
                }
            };

            try {
                std::array<unsigned char, 2U> version_bytes{};
                const int transferred = libusb_control_transfer(
                    handle, request_type_in_vendor_device, accessory_get_protocol, 0U, 0U,
                    version_bytes.data(), static_cast<std::uint16_t>(version_bytes.size()),
                    timeout_ms);
                if (transferred == static_cast<int>(version_bytes.size())) {
                    const std::uint16_t protocol =
                        static_cast<std::uint16_t>(version_bytes[0]) |
                        static_cast<std::uint16_t>(static_cast<std::uint16_t>(version_bytes[1])
                                                   << 8U);
                    if (protocol != 0U) {
                        Candidate candidate{};
                        candidate.bus = libusb_get_bus_number(device);
                        candidate.address = libusb_get_device_address(device);
                        candidate.port_path = port_path(device);
                        candidate.vendor_id = descriptor.idVendor;
                        candidate.product_id = descriptor.idProduct;
                        candidate.protocol_version = protocol;
                        candidate.serial = read_ascii_string(handle, descriptor.iSerialNumber);
                        candidate.product = read_ascii_string(handle, descriptor.iProduct);
                        out->push_back(std::move(candidate));
                    }
                }
            } catch (...) {
                close_handle();
                throw;
            }
            close_handle();
        }
    } catch (...) {
        libusb_free_device_list(devices, 1);
        throw;
    }

    libusb_free_device_list(devices, 1);
    std::sort(out->begin(), out->end(), candidate_less);
    return AOAHID_OK;
}

} // namespace aoa::transport
