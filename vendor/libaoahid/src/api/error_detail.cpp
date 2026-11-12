// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * This file owns thread-local diagnostics and stable result names. It performs
 * no allocation, logging, synchronization, or USB operation.
 */

#include "api/internal.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace {
thread_local aoahid_error_detail g_error{AOAHID_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};

class LogLine final {
  public:
    void append(const std::string_view text) noexcept {
        for (const char value : text) {
            if (size_ + 1U >= bytes_.size())
                break;
            bytes_[size_++] = value;
        }
        bytes_[size_] = '\0';
    }

    template <typename Integer>
    void append_integer(const Integer value, const int base = 10) noexcept {
        if (size_ + 1U >= bytes_.size())
            return;
        const auto converted =
            std::to_chars(bytes_.data() + size_, bytes_.data() + bytes_.size() - 1U, value, base);
        if (converted.ec == std::errc{}) {
            size_ = static_cast<std::size_t>(converted.ptr - bytes_.data());
            bytes_[size_] = '\0';
        }
    }

    const char* data() const noexcept { return bytes_.data(); }

  private:
    std::array<char, 512U> bytes_{};
    std::size_t size_{};
};

void append_key(LogLine& line, const std::string_view key) noexcept {
    line.append(" ");
    line.append(key);
    line.append("=");
}
} // namespace

namespace aoa::detail {

void set_error(const aoahid_result result, const char* field, const char* reason,
               const std::int32_t native_status, const std::int32_t aoa_request,
               const std::uint16_t hid_id, const std::uint16_t report_id,
               const std::uint32_t offset, const std::uint32_t length) noexcept {
    g_error = aoahid_error_detail{
        result,        field,       reason == nullptr ? "An unspecified error occurred." : reason,
        native_status, aoa_request, hid_id,
        report_id,     offset,      length};
}

void clear_error() noexcept {
    g_error = aoahid_error_detail{AOAHID_OK, nullptr, "No error.", 0, 0, 0, 0, 0, 0};
}

aoahid_result finish_result(const aoahid_result result, const char* field, const char* reason,
                            const std::int32_t native_status, const std::int32_t aoa_request,
                            const std::uint16_t hid_id, const std::uint16_t report_id,
                            const std::uint32_t offset, const std::uint32_t length) noexcept {
    if (result == AOAHID_OK) {
        clear_error();
    } else if (g_error.code != result) {
        set_error(result, field, reason, native_status, aoa_request, hid_id, report_id, offset,
                  length);
    }
    return result;
}

const char* result_name(const aoahid_result result) noexcept {
    switch (result) {
    case AOAHID_OK:
        return "AOAHID_OK";
    case AOAHID_ERR_PARAM:
        return "AOAHID_ERR_PARAM";
    case AOAHID_ERR_UNSET_FIELD:
        return "AOAHID_ERR_UNSET_FIELD";
    case AOAHID_ERR_UNSUPPORTED:
        return "AOAHID_ERR_UNSUPPORTED";
    case AOAHID_ERR_NOT_AOA:
        return "AOAHID_ERR_NOT_AOA";
    case AOAHID_ERR_ACCESS:
        return "AOAHID_ERR_ACCESS";
    case AOAHID_ERR_BUSY:
        return "AOAHID_ERR_BUSY";
    case AOAHID_ERR_NO_DEVICE:
        return "AOAHID_ERR_NO_DEVICE";
    case AOAHID_ERR_STALL:
        return "AOAHID_ERR_STALL";
    case AOAHID_ERR_TIMEOUT:
        return "AOAHID_ERR_TIMEOUT";
    case AOAHID_ERR_SHORT_TRANSFER:
        return "AOAHID_ERR_SHORT_TRANSFER";
    case AOAHID_ERR_DESCRIPTOR_REJECTED:
        return "AOAHID_ERR_DESCRIPTOR_REJECTED";
    case AOAHID_ERR_IO:
        return "AOAHID_ERR_IO";
    case AOAHID_ERR_OVERFLOW:
        return "AOAHID_ERR_OVERFLOW";
    case AOAHID_CLOSE_PENDING:
        return "AOAHID_CLOSE_PENDING";
    case AOAHID_ERR_INTERNAL:
        return "AOAHID_ERR_INTERNAL";
    default:
        return nullptr;
    }
}

void emit_log(const aoahid_context* context, const aoahid_log_level level,
              const char* message) noexcept {
    if (context == nullptr || message == nullptr || level < AOAHID_LOG_ERROR ||
        context->options.log_level == AOAHID_LOG_DISABLED || context->options.log_level < level ||
        context->options.log_sink == nullptr) {
        return;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try {
        context->options.log_sink(context->options.log_user, level, message);
    } catch (...) {
        // A sink is foreign callback code. It cannot report a failure through
        // this void callback and must never unwind through libaoahid or a
        // libusb callback frame. Logging is therefore best-effort.
        return;
    }
#else
    context->options.log_sink(context->options.log_user, level, message);
#endif
}

void log_event(const aoahid_context* context, const aoahid_log_level level, const char* event,
               const aoahid_device* device, const aoahid_node* node,
               const LogEventStatus status) noexcept {
    if (context == nullptr || event == nullptr ||
        context->options.log_level == AOAHID_LOG_DISABLED || context->options.log_level < level ||
        context->options.log_sink == nullptr) {
        return;
    }
    LogLine line{};
    line.append("event=");
    line.append(event);
    if (device != nullptr) {
        append_key(line, "bus");
        line.append_integer(device->bus);
        append_key(line, "port");
        if (device->port_path.empty()) {
            line.append("unknown");
        } else {
            for (std::size_t index = 0U; index < device->port_path.size(); ++index) {
                if (index != 0U)
                    line.append(".");
                line.append_integer(device->port_path[index]);
            }
        }
        append_key(line, "vid");
        line.append("0x");
        line.append_integer(device->vendor_id, 16);
        append_key(line, "pid");
        line.append("0x");
        line.append_integer(device->product_id, 16);
    }
    if (node != nullptr) {
        append_key(line, "node_id");
        line.append_integer(node->hid_id);
        if (node->spec != nullptr && node->spec->layout.has_report_id) {
            append_key(line, "report_id");
            line.append_integer(node->spec->layout.report_id);
        }
    }
    if (status.aoa_request != 0) {
        append_key(line, "request");
        line.append_integer(status.aoa_request);
    }
    if (status.offset != 0U) {
        append_key(line, "offset");
        line.append_integer(status.offset);
    }
    if (status.length != 0U) {
        append_key(line, "length");
        line.append_integer(status.length);
    }
    append_key(line, "result");
    const char* name = result_name(status.result);
    line.append(name == nullptr ? "AOAHID_RESULT_UNKNOWN" : name);
    if (status.native_status != 0) {
        append_key(line, "transport_status");
        line.append_integer(status.native_status);
    }
    emit_log(context, level, line.data());
}

bool valid_boolean(const std::uint32_t value) noexcept { return value == 0U || value == 1U; }

bool valid_struct(const void* value, const std::uint32_t actual_size,
                  const std::uint32_t expected_size, const char* field) noexcept {
    if (value == nullptr) {
        set_error(AOAHID_ERR_PARAM, field, "The required structure pointer is null.");
        return false;
    }
    if (actual_size != expected_size) {
        set_error(AOAHID_ERR_PARAM, field,
                  "struct_size must equal the size of the ABI structure used by this release.");
        return false;
    }
    return true;
}

} // namespace aoa::detail

extern "C" {

const aoahid_error_detail* AOAHID_CALL aoahid_last_error(void) try {
    return &g_error;
} catch (...) {
    aoa::detail::set_error(AOAHID_ERR_INTERNAL, "last_error",
                           "An internal C++ operation failed inside the public C ABI call.");
    return &g_error;
}

const char* AOAHID_CALL aoahid_result_name(const aoahid_result result) try {
    const char* name = aoa::detail::result_name(result);
    if (name == nullptr) {
        aoa::detail::set_error(AOAHID_ERR_PARAM, "result",
                               "The result value is outside the public aoahid_result enum.");
        return "AOAHID_RESULT_UNKNOWN";
    }
    aoa::detail::clear_error();
    return name;
} catch (...) {
    aoa::detail::set_error(AOAHID_ERR_INTERNAL, "result.name",
                           "An internal C++ operation failed inside the public C ABI call.");
    return "AOAHID_ERR_INTERNAL";
}

std::uint32_t AOAHID_CALL aoahid_version(void) try {
    aoa::detail::clear_error();
    return (AOAHID_VERSION_MAJOR << 24U) | (AOAHID_VERSION_MINOR << 16U) | AOAHID_VERSION_PATCH;
} catch (...) {
    aoa::detail::set_error(AOAHID_ERR_INTERNAL, "version",
                           "An internal C++ operation failed inside the public C ABI call.");
    return 0U;
}

} // extern "C"
