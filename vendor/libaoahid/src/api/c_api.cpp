// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Stable C ABI orchestration. USB types remain below the transport boundary;
 * immutable specs are shared while all mutable report state stays per node.
 */

#include "api/internal.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <utility>

namespace {
using aoa::detail::set_error;
constexpr std::size_t kMaximumLibusbPortDepth = 7U;
// AOA 1.0 SEND_STRING: at most 256 bytes per string, NUL included.
constexpr std::size_t kMaximumAccessoryStringBytes = 256U;
// A long bounded wait removes the former 100 Hz idle wake cadence while still
// bounding shutdown if a backend cannot honor an event-handler interrupt.
constexpr std::uint32_t kInternalEventPollTimeoutMs = 60'000U;

std::uint16_t report_id(const aoahid_node* node) noexcept {
    return node != nullptr && node->spec != nullptr && node->spec->layout.has_report_id
               ? node->spec->layout.report_id
               : 0U;
}

const char* completion_reason(const aoahid_result result) noexcept {
    switch (result) {
    case AOAHID_ERR_STALL:
        return "The device refused AOA request 57 (STALL); nothing was applied and the state "
               "stays pending for a resend. Right after aoahid_node_open this usually means "
               "Android has not finished registering the HID.";
    case AOAHID_ERR_TIMEOUT:
        return "AOA request 57 timed out; delivery to the accessory cannot be established.";
    case AOAHID_ERR_SHORT_TRANSFER:
        return "AOA request 57 completed with an actual payload length different from the "
               "requested length.";
    case AOAHID_ERR_NO_DEVICE:
        return "The device disappeared before or while AOA request 57 was sent.";
    case AOAHID_ERR_OVERFLOW:
        return "libusb completed AOA request 57 with a transfer-overflow status.";
    case AOAHID_ERR_IO:
        return "The asynchronous AOA request 57 transfer ended with a libusb I/O or cancellation "
               "status.";
    default:
        return "The asynchronous AOA request 57 transfer failed.";
    }
}

aoahid_result consume_completion(aoahid_node* node, const char* field) noexcept {
    const aoahid_result result =
        node->completion_result.exchange(AOAHID_OK, std::memory_order_acq_rel);
    if (result == AOAHID_OK) {
        node->completion_native_status.store(0, std::memory_order_relaxed);
        node->completion_report_length.store(0U, std::memory_order_relaxed);
        node->completion_report_id.store(0U, std::memory_order_relaxed);
        return AOAHID_OK;
    }
    const std::int32_t native_status =
        node->completion_native_status.exchange(0, std::memory_order_relaxed);
    const std::uint32_t length =
        node->completion_report_length.exchange(0U, std::memory_order_relaxed);
    const std::uint16_t completed_report_id =
        node->completion_report_id.exchange(0U, std::memory_order_relaxed);
    set_error(result, field, completion_reason(result), native_status, 57, node->hid_id,
              completed_report_id, 0U, length);
    return result;
}

aoahid_result finish_transport_result(const aoahid_result result, const char* field,
                                      const char* reason, const aoahid_node* node = nullptr,
                                      const std::int32_t fallback_request = 0,
                                      const std::uint32_t fallback_offset = 0U,
                                      const std::uint32_t fallback_length = 0U) noexcept {
    if (result == AOAHID_OK) {
        aoa::detail::clear_error();
        return AOAHID_OK;
    }
    aoa::transport::ErrorInfo transport_error = aoa::transport::last_error();
    if (transport_error.result != result) {
        transport_error = aoa::transport::ErrorInfo{
            result,           0,
            fallback_request, node == nullptr ? std::uint16_t{0} : node->hid_id,
            fallback_offset,  fallback_length,
            report_id(node)};
    }
    const std::uint16_t hid_id = transport_error.hid_id != 0U
                                     ? transport_error.hid_id
                                     : (node == nullptr ? std::uint16_t{0} : node->hid_id);
    const std::uint16_t transport_report_id =
        transport_error.aoa_request == 57 ? transport_error.report_id : report_id(node);
    set_error(result, field, reason, transport_error.native_status,
              transport_error.aoa_request != 0 ? transport_error.aoa_request : fallback_request,
              hid_id, transport_report_id,
              transport_error.offset != 0U ? transport_error.offset : fallback_offset,
              transport_error.length != 0U ? transport_error.length : fallback_length);
    return result;
}

struct DeferredErrorInput {
    aoa::transport::ErrorInfo transport{};
    const char* field{};
    const char* reason{};
    std::uint16_t report_id{};
};

aoa::detail::DeferredError make_deferred_error(DeferredErrorInput input) noexcept {
    aoa::detail::DeferredError error{};
    error.transport = input.transport;
    error.field = input.field;
    error.reason = input.reason;
    error.report_id = input.report_id;
    error.pending = input.transport.result != AOAHID_OK;
    return error;
}

aoa::detail::DeferredError
current_deferred_error(const aoahid_result result, const char* field, const char* reason,
                       const aoahid_node* node = nullptr, const std::int32_t fallback_request = 0,
                       const std::uint32_t fallback_offset = 0U,
                       const std::uint32_t fallback_length = 0U) noexcept {
    const aoahid_error_detail* detail = aoahid_last_error();
    if (detail != nullptr && detail->code == result) {
        return make_deferred_error(DeferredErrorInput{
            aoa::transport::ErrorInfo{result, detail->transport_status, detail->aoa_request,
                                      detail->node_id, detail->offset, detail->length,
                                      detail->report_id},
            detail->field == nullptr ? field : detail->field,
            detail->reason == nullptr ? reason : detail->reason, detail->report_id});
    }

    aoa::transport::ErrorInfo transport_error = aoa::transport::last_error();
    if (transport_error.result != result) {
        transport_error = aoa::transport::ErrorInfo{
            result,           0,
            fallback_request, node == nullptr ? std::uint16_t{0U} : node->hid_id,
            fallback_offset,  fallback_length,
            report_id(node)};
    }
    return make_deferred_error(DeferredErrorInput{
        aoa::transport::ErrorInfo{
            result, transport_error.native_status,
            transport_error.aoa_request != 0 ? transport_error.aoa_request : fallback_request,
            transport_error.hid_id != 0U ? transport_error.hid_id
                                         : (node == nullptr ? std::uint16_t{0U} : node->hid_id),
            transport_error.offset != 0U ? transport_error.offset : fallback_offset,
            transport_error.length != 0U ? transport_error.length : fallback_length,
            transport_error.aoa_request == 57 ? transport_error.report_id : report_id(node)},
        field, reason,
        transport_error.aoa_request == 57 ? transport_error.report_id : report_id(node)});
}

aoahid_result publish_deferred_error(const aoa::detail::DeferredError& error) noexcept {
    if (!error.pending || error.transport.result == AOAHID_OK) {
        aoa::detail::clear_error();
        return AOAHID_OK;
    }
    set_error(error.transport.result, error.field == nullptr ? "context" : error.field,
              error.reason == nullptr ? "A deferred Context operation failed." : error.reason,
              error.transport.native_status, error.transport.aoa_request, error.transport.hid_id,
              error.report_id, error.transport.offset, error.transport.length);
    return error.transport.result;
}

void latch_context_error(aoahid_context* context,
                         const aoa::detail::DeferredError& error) noexcept {
    if (context == nullptr || !error.pending || error.transport.result == AOAHID_OK) {
        return;
    }
    {
        const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
        if (!context->deferred_error.pending) {
            context->deferred_error = error;
        }
    }
    context->graveyard_cv.notify_all();
}

bool peek_context_error(aoahid_context* context, aoa::detail::DeferredError* output) noexcept {
    if (context == nullptr || output == nullptr)
        return false;
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    if (!context->deferred_error.pending)
        return false;
    *output = context->deferred_error;
    return true;
}

bool take_context_error(aoahid_context* context, aoa::detail::DeferredError* output) noexcept {
    if (context == nullptr || output == nullptr)
        return false;
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    if (!context->deferred_error.pending)
        return false;
    *output = context->deferred_error;
    context->deferred_error = aoa::detail::DeferredError{};
    return true;
}

void latch_terminal_event_error(aoahid_context* context,
                                const aoa::detail::DeferredError& error) noexcept {
    if (context == nullptr || !error.pending || error.transport.result == AOAHID_OK)
        return;
    {
        const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
        context->event_thread_error = error;
        if (!context->deferred_error.pending)
            context->deferred_error = error;
    }
    context->event_thread_failed.store(true, std::memory_order_release);
    context->graveyard_cv.notify_all();
}

void request_event_thread_stop(aoahid_context* context) noexcept {
    if (context == nullptr)
        return;
    context->stop_event_thread.store(true, std::memory_order_release);
    if (context->options.event_mode == AOAHID_EVENT_INTERNAL_THREAD &&
        context->runtime != nullptr &&
        !context->event_thread_exited.load(std::memory_order_acquire)) {
        context->runtime->interrupt_event_handler();
    }
    context->graveyard_cv.notify_all();
}

bool peek_terminal_event_error(aoahid_context* context,
                               aoa::detail::DeferredError* output) noexcept {
    if (context == nullptr || output == nullptr ||
        !context->event_thread_failed.load(std::memory_order_acquire)) {
        return false;
    }
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    if (!context->event_thread_error.pending)
        return false;
    *output = context->event_thread_error;
    return true;
}

aoahid_result preflight_context(aoahid_context* context) noexcept {
    aoa::detail::DeferredError error{};
    if (peek_terminal_event_error(context, &error))
        return publish_deferred_error(error);
    return take_context_error(context, &error) ? publish_deferred_error(error) : AOAHID_OK;
}

aoahid_result validate_context_options(const aoahid_context_options* options) noexcept {
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "context_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U ||
        (options->event_mode != AOAHID_EVENT_CALLER_POLL &&
         options->event_mode != AOAHID_EVENT_INTERNAL_THREAD) ||
        options->log_level < AOAHID_LOG_DISABLED || options->log_level > AOAHID_LOG_TRACE ||
        (options->log_level != AOAHID_LOG_DISABLED && options->log_sink == nullptr)) {
        set_error(AOAHID_ERR_UNSET_FIELD, "context_options",
                  "event_mode, log_level, and any enabled log sink must be explicitly valid; "
                  "reserved must be zero.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    return AOAHID_OK;
}

// The tuning values have no default: each is the caller's, and zero means none.
bool required_tuning_nonzero(const aoahid_device_options& options) noexcept {
    return options.descriptor_fragment_bytes != 0U && options.transfer_pool_slots != 0U &&
           options.maximum_report_bytes != 0U;
}

bool required_product_policies_nonzero(const aoahid_device_options& options) noexcept {
    return options.aoa_descriptor_wire_policy_bytes != 0U &&
           options.linux_hid.descriptor_bytes != 0U && options.linux_hid.fields_per_report != 0U &&
           options.linux_hid.global_stack_depth != 0U && options.linux_hid.usages != 0U &&
           options.linux_hid.report_data_bits != 0U && options.linux_hid.report_size_bits != 0U &&
           options.target_ep0_data_policy_bytes != 0U &&
           options.host_control_buffer_policy_bytes != 0U;
}

aoahid_result prepare_device_options(const aoahid_device_options* options,
                                     aoahid_device_options* effective) noexcept {
    if (options == nullptr || !aoa::detail::valid_struct(
                                  options, options->struct_size,
                                  static_cast<std::uint32_t>(sizeof(*options)), "device_options")) {
        return AOAHID_ERR_PARAM;
    }
    *effective = *options;
    if (effective->reserved != 0U || !required_tuning_nonzero(*effective) ||
        !required_product_policies_nonzero(*effective) ||
        !aoa::detail::valid_boolean(effective->validate_reports) ||
        (effective->interface_claim_policy != AOAHID_INTERFACE_CLAIM_NONE &&
         effective->interface_claim_policy != AOAHID_INTERFACE_CLAIM_EXPLICIT) ||
        (effective->interface_claim_policy == AOAHID_INTERFACE_CLAIM_NONE &&
         effective->interface_number != -1) ||
        (effective->interface_claim_policy == AOAHID_INTERFACE_CLAIM_EXPLICIT &&
         (effective->interface_number < 0 || effective->interface_number > 255))) {
        set_error(AOAHID_ERR_UNSET_FIELD, "device_options",
                  "Every tuning value and product policy must be nonzero, and every flag and "
                  "claim choice valid.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (effective->descriptor_fragment_bytes > std::numeric_limits<std::uint16_t>::max() ||
        effective->maximum_report_bytes > std::numeric_limits<std::uint16_t>::max() ||
        effective->aoa_descriptor_wire_policy_bytes > std::numeric_limits<std::uint16_t>::max() ||
        effective->descriptor_fragment_bytes > effective->target_ep0_data_policy_bytes ||
        effective->descriptor_fragment_bytes > effective->host_control_buffer_policy_bytes ||
        effective->maximum_report_bytes > effective->target_ep0_data_policy_bytes ||
        effective->maximum_report_bytes > effective->host_control_buffer_policy_bytes) {
        set_error(AOAHID_ERR_OVERFLOW, "device.size_policies",
                  "A caller policy is inconsistent with the AOA 16-bit wire fields or another "
                  "explicitly selected policy.");
        return AOAHID_ERR_OVERFLOW;
    }
    return AOAHID_OK;
}

aoa::transport::Candidate copy_candidate(const aoahid_device_info& info) {
    aoa::transport::Candidate candidate{};
    candidate.bus = info.bus_number;
    candidate.address = info.device_address;
    if (info.port_path != nullptr && info.port_path_length != 0U) {
        candidate.port_path.assign(info.port_path, info.port_path + info.port_path_length);
    }
    candidate.vendor_id = info.vendor_id;
    candidate.product_id = info.product_id;
    candidate.protocol_version = info.protocol_version;
    if (info.serial != nullptr)
        candidate.serial = info.serial;
    if (info.product != nullptr)
        candidate.product = info.product;
    return candidate;
}

aoa::transport::DeviceConfig copy_config(const aoahid_device_options& options,
                                         const aoahid_event_mode event_mode) noexcept {
    return aoa::transport::DeviceConfig{event_mode,
                                        options.descriptor_fragment_bytes,
                                        options.transfer_pool_slots,
                                        options.maximum_report_bytes,
                                        options.interface_claim_policy,
                                        options.interface_number};
}

// Every wait is the caller's, per call, and has no default: zero is an error.
bool timeout_is_set(const std::uint32_t value, const char* field) noexcept {
    if (value != 0U) {
        return true;
    }
    set_error(AOAHID_ERR_UNSET_FIELD, field, "This timeout must be set; it has no default.");
    return false;
}

void refresh_discovery_views(aoahid_discovery* discovery) noexcept {
    for (auto& entry : discovery->entries) {
        entry.public_info = aoahid_device_info{entry.candidate.bus,
                                               entry.candidate.address,
                                               entry.candidate.port_path.data(),
                                               entry.candidate.port_path.size(),
                                               entry.candidate.vendor_id,
                                               entry.candidate.product_id,
                                               entry.candidate.protocol_version,
                                               entry.candidate.serial.c_str(),
                                               entry.candidate.product.c_str()};
    }
}

aoahid_result initialize_node_state(aoahid_node* node) {
    switch (node->spec->kind) {
    case AOAHID_PROFILE_KEYBOARD: {
        const auto* spec = std::get_if<aoa::detail::KeyboardConfig>(&node->spec->config);
        aoa::detail::KeyboardState value{};
        const std::size_t key_count =
            static_cast<std::size_t>(spec->options.usage_maximum - spec->options.usage_minimum) +
            1U;
        value.key_transitions.resize(key_count);
        value.pressed_bitmap.resize(key_count);
        node->state = std::move(value);
        break;
    }
    case AOAHID_PROFILE_MOUSE: {
        const auto* spec = std::get_if<aoa::detail::MouseConfig>(&node->spec->config);
        aoa::detail::MouseState value{};
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case AOAHID_PROFILE_TOGGLE: {
        const auto* spec = std::get_if<aoa::detail::ConsumerConfig>(&node->spec->config);
        aoa::detail::ConsumerState value{};
        value.pressed.resize(spec->allowed_usages.size());
        value.transitions.resize(spec->allowed_usages.size());
        node->state = std::move(value);
        break;
    }
    case AOAHID_PROFILE_GAMEPAD: {
        const auto* spec = std::get_if<aoa::detail::GamepadConfig>(&node->spec->config);
        aoa::detail::GamepadState value{};
        value.axes.reserve(spec->axes.size());
        for (const auto& axis : spec->axes)
            value.axes.push_back(axis.axis.neutral_value);
        value.buttons.resize(spec->options.button_count);
        value.button_transitions.resize(spec->options.button_count);
        node->state = std::move(value);
        break;
    }
    case AOAHID_PROFILE_TOUCHSCREEN:
    case AOAHID_PROFILE_TOUCHPAD: {
        const auto* spec = std::get_if<aoa::detail::TouchConfig>(&node->spec->config);
        aoa::detail::TouchState value{};
        value.buttons.resize(spec->fields.button_count);
        value.button_transitions.resize(spec->fields.button_count);
        node->state = std::move(value);
        break;
    }
    case AOAHID_PROFILE_PEN:
    case AOAHID_PROFILE_DIGITIZER:
        node->state = aoa::detail::PenState{};
        break;
    case AOAHID_PROFILE_RAW:
        node->state = aoa::detail::RawState{};
        node->dirty = false;
        break;
    default:
        set_error(AOAHID_ERR_INTERNAL, "spec.kind",
                  "A validated ProfileSpec contained an unknown profile kind.");
        return AOAHID_ERR_INTERNAL;
    }
    return AOAHID_OK;
}

bool has_non_neutral_state(const aoahid_node* node) noexcept {
    if (node == nullptr || node->spec == nullptr) {
        return false;
    }
    if (const auto* keyboard = std::get_if<aoa::detail::KeyboardState>(&node->state)) {
        return keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    }
    if (const auto* mouse = std::get_if<aoa::detail::MouseState>(&node->state)) {
        return std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* consumer = std::get_if<aoa::detail::ConsumerState>(&node->state)) {
        return std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* gamepad = std::get_if<aoa::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<aoa::detail::GamepadConfig>(&node->spec->config);
        if (spec == nullptr)
            return false;
        for (std::size_t index = 0U; index < gamepad->axes.size(); ++index) {
            if (gamepad->axes[index] != spec->axes[index].axis.neutral_value)
                return true;
        }
        return gamepad->hat_has_direction ||
               std::any_of(gamepad->buttons.begin(), gamepad->buttons.end(),
                           [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* touch = std::get_if<aoa::detail::TouchState>(&node->state)) {
        const bool active_contact =
            std::any_of(touch->contacts.begin(), touch->contacts.end(), [](const auto& contact) {
                return contact.phase == aoa::detail::ContactPhase::down;
            });
        return active_contact || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    }
    if (const auto* pen = std::get_if<aoa::detail::PenState>(&node->state)) {
        return pen->tool_switch_departure || pen->desired.in_range != 0U ||
               pen->desired.tip != 0U || pen->desired.barrel_buttons != 0U;
    }
    return false;
}

// Each helper is deliberately noinline and touches exactly one NodeState
// alternative. This is a one-time close-path operation, never the per-report
// send path, so the call overhead is immaterial; keeping each alternative's
// vector access in its own non-inlined function also keeps GCC 14's
// -Wstringop-overflow analysis from unifying value ranges across unrelated
// sibling if/else-if arms of one large function, which produced a false
// positive at -O3 on musl. MSVC does not
// recognize the gnu:: attribute namespace and, under /WX, turns its own
// warning about that into a hard error, so this is spelled per compiler
// rather than as a portable [[gnu::noinline]] attribute.
#if defined(_MSC_VER) && !defined(__clang__)
#define AOAHID_NOINLINE __declspec(noinline)
#else
#define AOAHID_NOINLINE __attribute__((noinline))
#endif

AOAHID_NOINLINE bool neutralize_keyboard(aoa::detail::KeyboardState* keyboard) noexcept {
    const bool had_non_neutral_state =
        keyboard->modifiers != 0U || keyboard->pressed_bitmap_count != 0U;
    std::fill(keyboard->pressed_bitmap.begin(), keyboard->pressed_bitmap.end(), std::uint8_t{0});
    keyboard->pressed_bitmap_count = 0U;
    keyboard->modifiers = 0U;
    return had_non_neutral_state;
}

AOAHID_NOINLINE bool neutralize_mouse(aoa::detail::MouseState* mouse) noexcept {
    const bool had_non_neutral_state =
        std::any_of(mouse->buttons.begin(), mouse->buttons.end(),
                    [](const std::uint8_t value) { return value != 0U; });
    std::fill(mouse->buttons.begin(), mouse->buttons.end(), std::uint8_t{0});
    mouse->dx = mouse->dy = mouse->wheel = mouse->pan = 0;
    return had_non_neutral_state;
}

AOAHID_NOINLINE bool neutralize_gamepad(aoa::detail::GamepadState* gamepad,
                                        const aoa::detail::GamepadConfig* spec) noexcept {
    bool had_non_neutral_state = false;
    for (std::size_t index = 0U; index < gamepad->axes.size(); ++index) {
        had_non_neutral_state =
            had_non_neutral_state || gamepad->axes[index] != spec->axes[index].axis.neutral_value;
        gamepad->axes[index] = spec->axes[index].axis.neutral_value;
    }
    had_non_neutral_state = had_non_neutral_state || gamepad->hat_has_direction ||
                            std::any_of(gamepad->buttons.begin(), gamepad->buttons.end(),
                                        [](const std::uint8_t value) { return value != 0U; });
    std::fill(gamepad->buttons.begin(), gamepad->buttons.end(), std::uint8_t{0});
    gamepad->hat_has_direction = false;
    gamepad->hat = 0;
    return had_non_neutral_state;
}

AOAHID_NOINLINE bool neutralize_touch(aoa::detail::TouchState* touch) noexcept {
    bool had_non_neutral_state = false;
    for (auto& contact : touch->contacts) {
        had_non_neutral_state =
            had_non_neutral_state || contact.phase != aoa::detail::ContactPhase::none;
        if (contact.phase == aoa::detail::ContactPhase::down) {
            contact.phase = aoa::detail::ContactPhase::up;
        }
    }
    had_non_neutral_state =
        had_non_neutral_state || std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                             [](const std::uint8_t value) { return value != 0U; });
    std::fill(touch->buttons.begin(), touch->buttons.end(), std::uint8_t{0});
    touch->packet_cursor = 0U;
    return had_non_neutral_state;
}

AOAHID_NOINLINE bool neutralize_pen(aoa::detail::PenState* pen) noexcept {
    const bool had_non_neutral_state = pen->desired.in_range != 0U || pen->desired.tip != 0U ||
                                       pen->desired.barrel_buttons != 0U ||
                                       pen->emitted.in_range != 0U || pen->emitted.tip != 0U ||
                                       pen->emitted.barrel_buttons != 0U;
    pen->desired.in_range = 0U;
    pen->desired.tip = 0U;
    pen->desired.barrel_buttons = 0U;
    pen->desired.pressure = 0;
    pen->tool_switch_departure = false;
    return had_non_neutral_state;
}
#undef AOAHID_NOINLINE

void make_neutral(aoahid_node* node) noexcept {
    bool had_non_neutral_state = false;
    if (auto* keyboard = std::get_if<aoa::detail::KeyboardState>(&node->state)) {
        had_non_neutral_state = neutralize_keyboard(keyboard);
    } else if (auto* mouse = std::get_if<aoa::detail::MouseState>(&node->state)) {
        had_non_neutral_state = neutralize_mouse(mouse);
    } else if (auto* consumer = std::get_if<aoa::detail::ConsumerState>(&node->state)) {
        had_non_neutral_state = std::any_of(consumer->pressed.begin(), consumer->pressed.end(),
                                            [](const std::uint8_t value) { return value != 0U; });
        std::fill(consumer->pressed.begin(), consumer->pressed.end(), std::uint8_t{0});
    } else if (auto* gamepad = std::get_if<aoa::detail::GamepadState>(&node->state)) {
        const auto* spec = std::get_if<aoa::detail::GamepadConfig>(&node->spec->config);
        had_non_neutral_state = neutralize_gamepad(gamepad, spec);
    } else if (auto* touch = std::get_if<aoa::detail::TouchState>(&node->state)) {
        had_non_neutral_state = neutralize_touch(touch);
    } else if (auto* pen = std::get_if<aoa::detail::PenState>(&node->state)) {
        had_non_neutral_state = neutralize_pen(pen);
    }
    node->dirty = (had_non_neutral_state || node->emitted_non_neutral) &&
                  node->spec->kind != AOAHID_PROFILE_RAW;
}

// drop_battery is the close path: a battery level has no neutral value, so an
// unsent one is discarded instead of being sent on the way out.
aoahid_result poll_until_idle(aoahid_node* node, const std::uint32_t timeout_ms,
                              const bool drop_battery = false) {
    if (timeout_ms == 0U) {
        set_error(AOAHID_ERR_UNSET_FIELD, "timeout_ms", "A nonzero timeout is required.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    const aoahid_result preflight = preflight_context(node->device->context);
    if (preflight != AOAHID_OK)
        return preflight;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        // Set when every pool slot is taken by other Nodes: this Node has no
        // transfer of its own to wait for, so it waits for any slot instead.
        bool pool_busy = false;
        std::uint64_t slot_generation = 0U;
        if (!node->transfer_inflight.load(std::memory_order_acquire)) {
            const aoahid_result completion = consume_completion(node, "node.completion");
            if (completion != AOAHID_OK)
                return completion;
            if (drop_battery) {
                node->battery_dirty = false;
            }
            if (!node->dirty && !node->battery_dirty)
                return AOAHID_OK;
            slot_generation = node->device->slot_generation.load();
            // The blocking timeout also bounds each report it sends: what is left of it.
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            const std::uint32_t send_timeout_ms = static_cast<std::uint32_t>(std::clamp<long long>(
                left, 1LL, static_cast<long long>(std::numeric_limits<std::uint32_t>::max())));
            const aoahid_result submit = aoahid_node_submit(node, send_timeout_ms);
            if (submit != AOAHID_OK && submit != AOAHID_ERR_BUSY)
                return submit;
            pool_busy = submit == AOAHID_ERR_BUSY &&
                        !node->transfer_inflight.load(std::memory_order_acquire);
        }
        if (node->device->context->options.event_mode == AOAHID_EVENT_CALLER_POLL) {
            const aoahid_result poll = node->device->context->runtime->poll(1U);
            if (poll != AOAHID_OK && poll != AOAHID_ERR_TIMEOUT) {
                return finish_transport_result(poll, "context.poll",
                                               "libusb event handling failed.", node);
            }
        } else if (pool_busy) {
            // Woken by the next completion on this Device (see
            // transfer_complete()); a bump before this wait makes it return
            // at once, so no release is missed. The quantum only rechecks
            // Context errors.
            aoahid_device* device = node->device;
            std::unique_lock<std::mutex> lock(device->slot_mutex);
            device->slot_waiters.fetch_add(1U);
            const auto quantum = std::chrono::steady_clock::now() + std::chrono::milliseconds(10U);
            static_cast<void>(device->slot_cv.wait_until(
                lock, std::min(deadline, quantum), [device, slot_generation]() noexcept {
                    return device->slot_generation.load() != slot_generation;
                }));
            device->slot_waiters.fetch_sub(1U);
            lock.unlock();
            const aoahid_result event_error = preflight_context(node->device->context);
            if (event_error != AOAHID_OK)
                return event_error;
        } else {
            std::unique_lock<std::mutex> lock(node->completion_mutex);
            node->completion_waiters.fetch_add(1U, std::memory_order_acq_rel);
            const auto quantum = std::chrono::steady_clock::now() + std::chrono::milliseconds(10U);
            static_cast<void>(node->completion_cv.wait_until(
                lock, std::min(deadline, quantum), [node]() noexcept {
                    return !node->transfer_inflight.load(std::memory_order_acquire);
                }));
            node->completion_waiters.fetch_sub(1U, std::memory_order_acq_rel);
            // Do not nest the Context mutex below the per-Node completion
            // mutex. The event thread publishes Context errors independently.
            lock.unlock();
            const aoahid_result event_error = preflight_context(node->device->context);
            if (event_error != AOAHID_OK)
                return event_error;
        }
    } while (std::chrono::steady_clock::now() < deadline);
    set_error(AOAHID_ERR_TIMEOUT, "node.submit_blocking",
              "The report sequence did not reach a terminal callback before the deadline.", 0, 57,
              node->hid_id, report_id(node), 0U,
              static_cast<std::uint32_t>(node->spec->layout.wire_bytes));
    return AOAHID_ERR_TIMEOUT;
}

void remember_close_error(aoahid_device* device, const aoa::detail::DeferredError& error) noexcept {
    if (device == nullptr || !error.pending || error.transport.result == AOAHID_OK ||
        device->close_error.pending) {
        return;
    }
    device->close_error = error;
    device->close_result = error.transport.result;
}

void remember_current_close_error(aoahid_device* device, const aoahid_result result,
                                  const char* field, const char* reason,
                                  const aoahid_node* node = nullptr,
                                  const std::int32_t fallback_request = 0,
                                  const std::uint32_t fallback_offset = 0U,
                                  const std::uint32_t fallback_length = 0U) noexcept {
    if (result == AOAHID_OK || result == AOAHID_CLOSE_PENDING)
        return;
    remember_close_error(device,
                         current_deferred_error(result, field, reason, node, fallback_request,
                                                fallback_offset, fallback_length));
}

std::uint32_t
remaining_milliseconds(const std::chrono::steady_clock::time_point deadline) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
        return 0U;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (remaining <= 0) {
        return 1U;
    }
    return remaining > static_cast<std::chrono::milliseconds::rep>(
                           std::numeric_limits<std::uint32_t>::max())
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(remaining);
}

void release_node_storage(aoahid_node* node) noexcept {
    if (node == nullptr) {
        return;
    }
    // Only internal-thread mode can race Node teardown against its callback.
    // Caller-poll callbacks execute inside the caller's synchronization domain,
    // so its null active mutex deliberately keeps teardown lock-free too.
    { const aoa::detail::ModeMutexGuard callback_exit(node->active_completion_mutex); }
    node->closed = true;
    static_cast<void>(aoa::detail::release_spec(node->spec));
    node->spec = nullptr;
    node->device = nullptr;
    delete node;
}

bool touch_frame_in_progress(const aoahid_node* node) noexcept {
    const auto* touch =
        node == nullptr ? nullptr : std::get_if<aoa::detail::TouchState>(&node->state);
    return touch != nullptr && touch->packet_cursor != 0U;
}

// The Node stays live only while a transfer of it is in flight, because the transfer's callback
// still points at it: that case returns AOAHID_CLOSE_PENDING. Every other result means the Node is
// gone. A step that fails does not stop the ones after it, and the first failure is returned.
aoahid_result close_live_node_until(aoahid_node* node,
                                    const std::chrono::steady_clock::time_point deadline,
                                    const std::uint32_t control_timeout_ms) {
    aoahid_device* device = node->device;
    aoa::detail::DeferredError failure{};
    // Every step records its own error detail, which is kept for the first failure.
    const auto keep_first = [&](const aoahid_result result) {
        if (result != AOAHID_OK && !failure.pending)
            failure = current_deferred_error(result, "node.close", "A close step failed.", node);
    };
    const auto still_in_flight = [&]() {
        if (!node->transfer_inflight.load(std::memory_order_acquire))
            return false;
        set_error(AOAHID_CLOSE_PENDING, "node.close",
                  "A report transfer of the Node was still in flight at the close deadline.", 0, 57,
                  node->hid_id, report_id(node), 0U,
                  static_cast<std::uint32_t>(node->spec->layout.wire_bytes));
        return true;
    };

    // A touch frame that has started must be finished before the neutral frame replaces it.
    if (node->transfer_inflight.load(std::memory_order_acquire) || touch_frame_in_progress(node)) {
        const std::uint32_t remaining = remaining_milliseconds(deadline);
        if (remaining != 0U)
            keep_first(poll_until_idle(node, remaining, true));
    }
    if (still_in_flight())
        return AOAHID_CLOSE_PENDING;
    keep_first(consume_completion(node, "node.completion"));
    node->battery_dirty = false;

    if (device->transport->present()) {
        make_neutral(node);
        if (node->dirty) {
            const std::uint32_t remaining = remaining_milliseconds(deadline);
            if (remaining == 0U) {
                set_error(AOAHID_ERR_TIMEOUT, "node.close",
                          "No time was left to send the neutral report.");
                keep_first(AOAHID_ERR_TIMEOUT);
            } else {
                keep_first(poll_until_idle(node, remaining, true));
            }
            if (still_in_flight())
                return AOAHID_CLOSE_PENDING;
        }
        const aoahid_result unregister =
            device->transport->unregister_hid(node->hid_id, control_timeout_ms);
        if (unregister != AOAHID_ERR_NO_DEVICE) {
            keep_first(finish_transport_result(unregister, "node.unregister",
                                               "AOA request 55 failed while closing the Node.",
                                               node, 55));
        }
    }
    if (device->transport->clear_reservation(node->reservation) != AOAHID_OK) {
        set_error(AOAHID_CLOSE_PENDING, "node.reserved_slots",
                  "The Node's reserved transfer slot is still in use.", 0, 57, node->hid_id);
        return AOAHID_CLOSE_PENDING;
    }

    auto& nodes = device->nodes;
    nodes.erase(std::remove(nodes.begin(), nodes.end(), node), nodes.end());
    aoa::detail::log_event(device->context, AOAHID_LOG_INFO, "node_close", device, node,
                           aoa::detail::LogEventStatus{failure.transport.result, 0, 55});
    release_node_storage(node);
    return publish_deferred_error(failure);
}

void remember_terminal_completion(aoahid_device* device, aoahid_node* node) noexcept {
    if (device == nullptr || node == nullptr)
        return;
    const aoahid_result result =
        node->completion_result.exchange(AOAHID_OK, std::memory_order_acq_rel);
    const std::int32_t native_status =
        node->completion_native_status.exchange(0, std::memory_order_relaxed);
    const std::uint32_t length =
        node->completion_report_length.exchange(0U, std::memory_order_relaxed);
    const std::uint16_t completed_report_id =
        node->completion_report_id.exchange(0U, std::memory_order_relaxed);
    if (result == AOAHID_OK || aoa::transport::is_cancelled_completion_status(native_status)) {
        return;
    }
    remember_close_error(
        device, make_deferred_error(DeferredErrorInput{
                    aoa::transport::ErrorInfo{result, native_status, 57, node->hid_id, 0U, length},
                    "node.completion", completion_reason(result), completed_report_id}));
}

// A Device graph may be freed only after its own transfers and every child
// Channel's Bulk transfers have delivered their callbacks.
bool graph_drained(const aoahid_device* device) noexcept {
    if (device == nullptr || device->transport == nullptr || !device->transport->drained()) {
        return false;
    }
    return std::all_of(device->channels.begin(), device->channels.end(),
                       [](const aoahid_channel* channel) { return channel->transport->drained(); });
}

// libusb's WinUSB backend sends a device-recipient control transfer through a
// WinUSB interface it claims for that transfer and releases on completion. On
// a phone that interface is usually the ADB one a Channel wants, and the
// auto-release would undo a Channel claim made meanwhile (or a Channel release
// would abort the transfer). So a Channel claims and releases its interface
// only while no HID report transfer is in flight.
bool wait_for_report_drain(aoahid_device* device,
                           const std::chrono::steady_clock::time_point deadline,
                           aoahid_result* poll_error) noexcept {
    *poll_error = AOAHID_OK;
    while (!device->transport->drained()) {
        if (remaining_milliseconds(deadline) == 0U) {
            return false;
        }
        if (device->context->options.event_mode == AOAHID_EVENT_CALLER_POLL) {
            const aoahid_result poll = device->context->runtime->poll(1U);
            if (poll != AOAHID_OK) {
                *poll_error = poll;
                return false;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

void release_channel_storage(aoahid_channel* channel) noexcept {
    delete channel->transport;
    channel->transport = nullptr;
    channel->device = nullptr;
    delete channel;
}

aoa::detail::DeferredError destroy_drained_device(aoahid_device* device) noexcept {
    if (!graph_drained(device)) {
        return make_deferred_error(
            DeferredErrorInput{aoa::transport::ErrorInfo{AOAHID_CLOSE_PENDING, 0, 0, 0U, 0U, 0U},
                               "device.close", "Terminal transfer callbacks have not drained."});
    }

    while (!device->nodes.empty()) {
        aoahid_node* node = device->nodes.back();
        device->nodes.pop_back();
        node->closed = true;
        remember_terminal_completion(device, node);
        if (device->transport->present()) {
            const aoahid_result unregister =
                device->transport->unregister_hid(node->hid_id, device->close_control_timeout_ms);
            if (unregister != AOAHID_OK && unregister != AOAHID_ERR_NO_DEVICE) {
                remember_current_close_error(device, unregister, "node.unregister",
                                             "AOA request 55 failed during Device teardown.", node,
                                             55);
            }
        }
        const aoahid_result clear_reservation =
            device->transport->clear_reservation(node->reservation);
        remember_current_close_error(
            device, clear_reservation, "node.reserved_slots",
            "A transfer-pool reservation could not be released during Device teardown.", node);
        release_node_storage(node);
    }

    for (aoahid_channel* channel : device->channels) {
        release_channel_storage(channel);
    }
    device->channels.clear();

    const aoa::detail::DeferredError outcome = device->close_error;
    const aoahid_result result = outcome.pending ? outcome.transport.result : AOAHID_OK;
    aoa::detail::log_event(device->context,
                           result == AOAHID_OK ? AOAHID_LOG_INFO : AOAHID_LOG_ERROR, "device_close",
                           device, nullptr, aoa::detail::LogEventStatus{result});
    delete device->transport;
    device->transport = nullptr;
    device->context = nullptr;
    delete device;
    return outcome;
}

void park_in_graveyard(aoahid_context* context, aoahid_device* device) noexcept {
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    context->graveyard.push_back(device);
    context->graveyard_size.store(context->graveyard.size(), std::memory_order_release);
}

void reap_graveyard(aoahid_context* context) noexcept {
    if (context == nullptr || context->graveyard_size.load(std::memory_order_acquire) == 0U) {
        return;
    }
    for (;;) {
        aoahid_device* ready = nullptr;
        {
            const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
            const auto found =
                std::find_if(context->graveyard.begin(), context->graveyard.end(),
                             [](const aoahid_device* device) { return graph_drained(device); });
            if (found == context->graveyard.end()) {
                return;
            }
            ready = *found;
            context->graveyard.erase(found);
            context->graveyard_size.store(context->graveyard.size(), std::memory_order_release);
        }
        const aoa::detail::DeferredError outcome = destroy_drained_device(ready);
        latch_context_error(context, outcome);
        context->graveyard_cv.notify_all();
    }
}

bool graveyard_empty(aoahid_context* context) noexcept {
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    return context->graveyard.empty();
}

// A graveyard entry is parked only by an explicit close, so the fast path in
// reap_graveyard must observe the same emptiness the mutex-held view reports.
static_assert(sizeof(std::atomic<std::size_t>) >= sizeof(std::size_t),
              "graveyard_size must hold a container size");

aoahid_device* last_active_device(aoahid_context* context) noexcept {
    const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
    return context->devices.empty() ? nullptr : context->devices.back();
}

std::size_t find_or_create_hid_id_domain(aoahid_context* context,
                                         const aoa::transport::Candidate& candidate) {
    const bool unidentified = candidate.port_path.empty();
    const auto found = std::find_if(context->hid_id_domains.begin(), context->hid_id_domains.end(),
                                    [&](const aoa::detail::HidIdDomain& domain) {
                                        if (unidentified) {
                                            return domain.unidentified;
                                        }
                                        return !domain.unidentified &&
                                               domain.bus == candidate.bus &&
                                               domain.port_path == candidate.port_path;
                                    });
    if (found != context->hid_id_domains.end()) {
        return static_cast<std::size_t>(found - context->hid_id_domains.begin());
    }
    aoa::detail::HidIdDomain domain{};
    domain.bus = candidate.bus;
    domain.port_path = candidate.port_path;
    domain.unidentified = unidentified;
    context->hid_id_domains.push_back(std::move(domain));
    return context->hid_id_domains.size() - 1U;
}

aoahid_result allocate_hid_id(aoahid_device* device, std::uint16_t* hid_id) noexcept {
    if (device == nullptr || device->context == nullptr || hid_id == nullptr) {
        set_error(AOAHID_ERR_PARAM, "device.hid_id",
                  "A live device and HID ID output pointer are required.");
        return AOAHID_ERR_PARAM;
    }
    const aoa::detail::ModeMutexGuard guard(device->context->active_state_mutex);
    if (device->hid_id_domain_index >= device->context->hid_id_domains.size()) {
        set_error(AOAHID_ERR_INTERNAL, "device.hid_id",
                  "The device has no valid physical-identity allocation domain.");
        return AOAHID_ERR_INTERNAL;
    }
    auto& domain = device->context->hid_id_domains[device->hid_id_domain_index];
    if (domain.next_hid_id > std::numeric_limits<std::uint16_t>::max()) {
        set_error(AOAHID_ERR_OVERFLOW, "device.hid_id",
                  "The monotonic AOA HID ID space for this physical-identity domain is exhausted.");
        return AOAHID_ERR_OVERFLOW;
    }
    *hid_id = static_cast<std::uint16_t>(domain.next_hid_id++);
    return AOAHID_OK;
}

aoahid_result check_reservation(const aoahid_device* device,
                                const aoahid_node_options& options) noexcept {
    const bool no_reservation = options.has_reserved_slots == 0U && options.reserved_slots == 0U;
    if (options.reserved != 0U || (!no_reservation && options.has_reserved_slots != 1U) ||
        options.reserved_slots > 1U) {
        set_error(AOAHID_ERR_UNSET_FIELD, "node.reserved_slots",
                  "Reserve nothing with 0/0, or one slot with has_reserved_slots 1 and "
                  "reserved_slots 0 or 1.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    std::uint64_t reserved = options.reserved_slots;
    for (const auto* sibling : device->nodes)
        reserved += sibling->reserved_slots;
    if (reserved >= device->config.pool_slots) {
        set_error(AOAHID_ERR_PARAM, "node.reserved_slots",
                  "The reservations of all Nodes must leave at least one shared pool slot.");
        return AOAHID_ERR_PARAM;
    }
    return AOAHID_OK;
}

std::uint32_t saturate_u32(const std::uint64_t value) noexcept {
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

// Every limit is checked before request 54, so a Spec the target would refuse never registers.
// A refused limit reports the Device's value in offset and the Spec's in length.
aoahid_result check_spec_fits_device(const aoahid_device* device,
                                     const aoahid_spec* spec) noexcept {
    if (spec->descriptor.empty() || spec->descriptor.size() > device->aoa_descriptor_policy_bytes) {
        set_error(AOAHID_ERR_OVERFLOW, "spec.descriptor.aoa_wire_policy",
                  "The descriptor exceeds aoa_descriptor_wire_policy_bytes.");
        return AOAHID_ERR_OVERFLOW;
    }
    struct Limit {
        const char* field;
        const char* reason;
        std::uint64_t allowed;
        std::uint64_t needed;
    };
    const aoahid_linux_hid_policy& linux_hid = device->linux_hid;
    const aoa::hid::DescriptorRequirements& needs = spec->requirements;
    const std::array<Limit, 6> limits{{
        {"spec.descriptor.linux_policy", "The descriptor exceeds linux_hid.descriptor_bytes.",
         linux_hid.descriptor_bytes, spec->descriptor.size()},
        {"spec.requirements.maximum_fields_per_report",
         "A report has more fields than linux_hid.fields_per_report.", linux_hid.fields_per_report,
         needs.maximum_fields_per_report},
        {"spec.requirements.maximum_global_stack_depth",
         "The descriptor pushes deeper than linux_hid.global_stack_depth.",
         linux_hid.global_stack_depth, needs.maximum_global_stack_depth},
        {"spec.requirements.maximum_report_size_bits",
         "A Report Size is larger than linux_hid.report_size_bits.", linux_hid.report_size_bits,
         needs.maximum_report_size_bits},
        {"spec.requirements.maximum_usages",
         "The descriptor has more Usages than linux_hid.usages.", linux_hid.usages,
         needs.maximum_usages},
        {"spec.requirements.maximum_report_data_bits",
         "A report is larger than linux_hid.report_data_bits.", linux_hid.report_data_bits,
         needs.maximum_report_data_bits},
    }};
    for (const Limit& limit : limits) {
        if (limit.needed > limit.allowed) {
            set_error(AOAHID_ERR_OVERFLOW, limit.field, limit.reason, 0, 0, 0, 0,
                      saturate_u32(limit.allowed), saturate_u32(limit.needed));
            return AOAHID_ERR_OVERFLOW;
        }
    }
    for (const auto& report : spec->report_capabilities) {
        if (report.wire_length > device->config.maximum_report_bytes ||
            report.wire_length > device->target_ep0_policy_bytes ||
            report.wire_length > device->host_control_policy_bytes) {
            set_error(AOAHID_ERR_OVERFLOW, "spec.report",
                      "A report is larger than maximum_report_bytes, "
                      "target_ep0_data_policy_bytes or host_control_buffer_policy_bytes.");
            return AOAHID_ERR_OVERFLOW;
        }
    }
    return AOAHID_OK;
}
} // namespace

#if defined(AOAHID_CHECK_SERIALIZATION)
namespace aoa::detail {
void serialization_violation(const char* api) noexcept {
    // Not through the log sink: it belongs to the caller and may itself be mid-call.
    std::fprintf(stderr,
                 "libaoahid: %s was called while another thread is inside a call on the same "
                 "Context; the caller must serialize them.\n",
                 api);
    std::fflush(stderr);
    std::abort();
}
} // namespace aoa::detail
#endif

#define AOAHID_C_RESULT_CATCH(api_name)                                                            \
    catch (const std::bad_alloc&) {                                                                \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
        return AOAHID_ERR_INTERNAL;                                                                \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
        return AOAHID_ERR_INTERNAL;                                                                \
    }

#define AOAHID_C_VALUE_CATCH(api_name, fallback)                                                   \
    catch (const std::bad_alloc&) {                                                                \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
        return fallback;                                                                           \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
        return fallback;                                                                           \
    }

#define AOAHID_C_VOID_CATCH(api_name)                                                              \
    catch (const std::bad_alloc&) {                                                                \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "Memory allocation failed inside the public C ABI call.");                       \
    }                                                                                              \
    catch (...) {                                                                                  \
        set_error(AOAHID_ERR_INTERNAL, api_name,                                                   \
                  "An internal C++ operation failed inside the public C ABI call.");               \
    }

extern "C" {

aoahid_result AOAHID_CALL aoahid_context_create(const aoahid_context_options* options,
                                                aoahid_context** out_context) try {
    aoa::detail::clear_error();
    if (out_context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_context", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_context = nullptr;
    const aoahid_result validation = validate_context_options(options);
    if (validation != AOAHID_OK)
        return validation;
    auto* context = new (std::nothrow) aoahid_context();
    if (context == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "context", "Memory allocation for the Context failed.");
        return AOAHID_ERR_INTERNAL;
    }
    context->options = *options;
    context->active_state_mutex =
        options->event_mode == AOAHID_EVENT_INTERNAL_THREAD ? &context->mutex : nullptr;
    const aoahid_result result =
        aoa::transport::Runtime::create(options->event_mode, &context->runtime);
    if (result != AOAHID_OK) {
        delete context;
        return finish_transport_result(result, "context.runtime",
                                       "libusb runtime initialization failed.");
    }
    if (options->event_mode == AOAHID_EVENT_INTERNAL_THREAD) {
        try {
            context->event_thread = std::thread([context]() noexcept {
                try {
                    while (!context->stop_event_thread.load(std::memory_order_acquire)) {
                        const aoahid_result poll_result =
                            context->runtime->poll(kInternalEventPollTimeoutMs);
                        if (poll_result != AOAHID_OK) {
                            const aoa::transport::ErrorInfo transport_error =
                                aoa::transport::last_error();
                            latch_context_error(
                                context,
                                make_deferred_error(DeferredErrorInput{
                                    aoa::transport::ErrorInfo{poll_result,
                                                              transport_error.result == poll_result
                                                                  ? transport_error.native_status
                                                                  : 0,
                                                              transport_error.result == poll_result
                                                                  ? transport_error.aoa_request
                                                                  : 0,
                                                              transport_error.result == poll_result
                                                                  ? transport_error.hid_id
                                                                  : std::uint16_t{0U},
                                                              transport_error.result == poll_result
                                                                  ? transport_error.offset
                                                                  : 0U,
                                                              transport_error.result == poll_result
                                                                  ? transport_error.length
                                                                  : 0U},
                                    "context.event_pump",
                                    "The internal libusb event pump failed; the first failure was "
                                    "retained for an application thread."}));
                        }
                        // poll() has returned from every callback it dispatched, so a
                        // drained graveyard graph can now be reclaimed safely.
                        reap_graveyard(context);
                        if (poll_result != AOAHID_OK) {
                            // A backend that fails immediately must not turn the
                            // internal event thread into a busy loop. This wait is
                            // confined to the failure path and remains interruptible.
                            std::unique_lock<std::mutex> lock(context->mutex);
                            static_cast<void>(context->graveyard_cv.wait_for(
                                lock, std::chrono::milliseconds(10U), [context]() noexcept {
                                    return context->stop_event_thread.load(
                                        std::memory_order_acquire);
                                }));
                        }
                    }
                } catch (...) {
                    latch_terminal_event_error(
                        context,
                        make_deferred_error(DeferredErrorInput{
                            aoa::transport::ErrorInfo{AOAHID_ERR_INTERNAL, 0, 0, 0U, 0U, 0U},
                            "context.event_pump",
                            "An internal exception stopped the Context event thread."}));
                }
                context->event_thread_exited.store(true, std::memory_order_release);
                context->graveyard_cv.notify_all();
            });
        } catch (...) {
            delete context->runtime;
            context->runtime = nullptr;
            delete context;
            throw;
        }
    }
    *out_context = context;
    aoa::detail::emit_log(context, AOAHID_LOG_INFO, "event=context_create result=AOAHID_OK");
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("context.create")

aoahid_result AOAHID_CALL aoahid_context_poll(aoahid_context* context,
                                              const std::uint32_t timeout_ms) try {
    AOAHID_SERIALIZED(context, "aoahid_context_poll");
    aoa::detail::clear_error();
    if (context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "context", "The Context handle is null.");
        return AOAHID_ERR_PARAM;
    }
    if (context->options.event_mode != AOAHID_EVENT_CALLER_POLL) {
        set_error(AOAHID_ERR_UNSUPPORTED, "context.event_mode",
                  "Caller polling is available only in caller-poll mode.");
        return AOAHID_ERR_UNSUPPORTED;
    }
    const aoahid_result deferred = preflight_context(context);
    if (deferred != AOAHID_OK)
        return deferred;
    const aoahid_result result = context->runtime->poll(timeout_ms);
    // libusb cancellation is asynchronous. A graveyard device can be released
    // only after poll has returned from every terminal callback.
    reap_graveyard(context);
    if (result != AOAHID_OK) {
        return finish_transport_result(result, "context.poll", "libusb event handling failed.");
    }
    return preflight_context(context);
}
AOAHID_C_RESULT_CATCH("context.poll")

aoahid_result AOAHID_CALL aoahid_discover(aoahid_context* context,
                                          const std::uint32_t control_timeout_ms,
                                          aoahid_discovery** out_discovery) try {
    AOAHID_SERIALIZED(context, "aoahid_discover");
    aoa::detail::clear_error();
    if (out_discovery != nullptr)
        *out_discovery = nullptr;
    if (context == nullptr || out_discovery == nullptr) {
        set_error(AOAHID_ERR_PARAM, "discover", "A Context and an output pointer are required.");
        return AOAHID_ERR_PARAM;
    }
    if (!timeout_is_set(control_timeout_ms, "control_timeout_ms"))
        return AOAHID_ERR_UNSET_FIELD;
    const aoahid_result deferred = preflight_context(context);
    if (deferred != AOAHID_OK)
        return deferred;
    auto discovery = std::unique_ptr<aoahid_discovery>(new (std::nothrow) aoahid_discovery());
    if (discovery == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "discovery",
                  "Memory allocation for the discovery result failed.");
        return AOAHID_ERR_INTERNAL;
    }
    std::vector<aoa::transport::Candidate> candidates;
    const aoahid_result result = context->runtime->discover(control_timeout_ms, &candidates);
    if (result != AOAHID_OK) {
        return finish_transport_result(result, "discovery", "USB enumeration failed.", nullptr, 51,
                                       0U, 2U);
    }
    discovery->entries.reserve(candidates.size());
    for (auto& candidate : candidates) {
        aoahid_discovery_entry entry{};
        entry.candidate = std::move(candidate);
        discovery->entries.push_back(std::move(entry));
    }
    refresh_discovery_views(discovery.get());
    *out_discovery = discovery.release();
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("discover")

std::size_t AOAHID_CALL aoahid_discovery_count(const aoahid_discovery* discovery) try {
    aoa::detail::clear_error();
    if (discovery == nullptr) {
        set_error(AOAHID_ERR_PARAM, "discovery",
                  "The discovery handle is null; zero is the failure sentinel.");
        return 0U;
    }
    return discovery->entries.size();
}
AOAHID_C_VALUE_CATCH("discovery.count", 0U)

const aoahid_device_info* AOAHID_CALL aoahid_discovery_get(const aoahid_discovery* discovery,
                                                           const std::size_t index) try {
    aoa::detail::clear_error();
    if (discovery == nullptr || index >= discovery->entries.size()) {
        set_error(AOAHID_ERR_PARAM, discovery == nullptr ? "discovery" : "discovery.index",
                  discovery == nullptr ? "The discovery handle is null."
                                       : "The discovery index is outside the owned result list.",
                  0, 0, 0, 0,
                  static_cast<std::uint32_t>(
                      std::min<std::size_t>(index, std::numeric_limits<std::uint32_t>::max())),
                  static_cast<std::uint32_t>(
                      std::min<std::size_t>(discovery == nullptr ? 0U : discovery->entries.size(),
                                            std::numeric_limits<std::uint32_t>::max())));
        return nullptr;
    }
    return &discovery->entries[index].public_info;
}
AOAHID_C_VALUE_CATCH("discovery.get", nullptr)

void AOAHID_CALL aoahid_discovery_destroy(aoahid_discovery* discovery) try {
    aoa::detail::clear_error();
    if (discovery == nullptr) {
        set_error(AOAHID_ERR_PARAM, "discovery", "The discovery handle is null.");
        return;
    }
    delete discovery;
}
AOAHID_C_VOID_CATCH("discovery.destroy")

aoahid_result AOAHID_CALL aoahid_accessory_start(aoahid_context* context,
                                                 const aoahid_device_info* selected,
                                                 const aoahid_accessory_options* options,
                                                 const std::uint32_t control_timeout_ms) try {
    AOAHID_SERIALIZED(context, "aoahid_accessory_start");
    aoa::detail::clear_error();
    if (!timeout_is_set(control_timeout_ms, "control_timeout_ms"))
        return AOAHID_ERR_UNSET_FIELD;
    if (context == nullptr || selected == nullptr) {
        set_error(AOAHID_ERR_PARAM, "accessory.start", "Context and selection are required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result deferred = preflight_context(context);
    if (deferred != AOAHID_OK)
        return deferred;
    if ((selected->port_path_length != 0U && selected->port_path == nullptr) ||
        selected->port_path_length > kMaximumLibusbPortDepth) {
        set_error(selected->port_path_length > kMaximumLibusbPortDepth ? AOAHID_ERR_OVERFLOW
                                                                       : AOAHID_ERR_PARAM,
                  "device.port_path",
                  "A nonempty libusb physical path needs a pointer and cannot exceed seven ports.");
        return selected->port_path_length > kMaximumLibusbPortDepth ? AOAHID_ERR_OVERFLOW
                                                                    : AOAHID_ERR_PARAM;
    }
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "accessory_options")) {
        return AOAHID_ERR_PARAM;
    }
    const aoahid_aoa_strings& strings = options->strings;
    if (options->reserved != 0U || strings.manufacturer == nullptr ||
        strings.manufacturer[0] == '\0' || strings.model == nullptr || strings.model[0] == '\0') {
        set_error(AOAHID_ERR_UNSET_FIELD, "accessory_options",
                  "A nonempty manufacturer and model are required, and reserved must be zero.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    const std::array<const char*, 6U> values{strings.manufacturer, strings.model,
                                             strings.description,  strings.version,
                                             strings.uri,          strings.serial};
    // AOA 1.0 limits each request-52 string to 256 bytes including its NUL;
    // check all of them before any request is sent.
    for (std::size_t index = 0U; index < values.size(); ++index) {
        if (values[index] != nullptr &&
            std::char_traits<char>::length(values[index]) + 1U > kMaximumAccessoryStringBytes) {
            set_error(AOAHID_ERR_OVERFLOW, "accessory_options.strings",
                      "An AOA identification string exceeds 256 bytes including its NUL.", 0, 52,
                      0U, 0U, static_cast<std::uint32_t>(index));
            return AOAHID_ERR_OVERFLOW;
        }
    }
    const aoahid_result result = context->runtime->start_accessory(
        copy_candidate(*selected), values.data(), control_timeout_ms);
    if (result != AOAHID_OK) {
        return finish_transport_result(result, "accessory.start",
                                       "An AOA accessory-mode start request failed.");
    }
    aoa::detail::log_event(context, AOAHID_LOG_INFO, "accessory_start", nullptr, nullptr,
                           aoa::detail::LogEventStatus{AOAHID_OK, 0, 53});
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("accessory.start")

aoahid_result AOAHID_CALL aoahid_device_open(aoahid_context* context,
                                             const aoahid_device_info* selected,
                                             const aoahid_device_options* options,
                                             aoahid_device** out_device) try {
    AOAHID_SERIALIZED(context, "aoahid_device_open");
    aoa::detail::clear_error();
    if (out_device != nullptr)
        *out_device = nullptr;
    if (context == nullptr || selected == nullptr || out_device == nullptr) {
        set_error(AOAHID_ERR_PARAM, "device.open", "Context, selection, and output are required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result deferred = preflight_context(context);
    if (deferred != AOAHID_OK)
        return deferred;
    if ((selected->port_path_length != 0U && selected->port_path == nullptr) ||
        selected->port_path_length > kMaximumLibusbPortDepth) {
        set_error(selected->port_path_length > kMaximumLibusbPortDepth ? AOAHID_ERR_OVERFLOW
                                                                       : AOAHID_ERR_PARAM,
                  "device.port_path",
                  "A nonempty libusb physical path needs a pointer and cannot exceed seven ports.");
        return selected->port_path_length > kMaximumLibusbPortDepth ? AOAHID_ERR_OVERFLOW
                                                                    : AOAHID_ERR_PARAM;
    }
    aoahid_device_options effective_options{};
    const aoahid_result validation = prepare_device_options(options, &effective_options);
    if (validation != AOAHID_OK)
        return validation;
    auto device = std::unique_ptr<aoahid_device>(new (std::nothrow) aoahid_device());
    if (device == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "device", "Memory allocation for the Device failed.");
        return AOAHID_ERR_INTERNAL;
    }
    try {
        device->context = context;
        device->config = copy_config(effective_options, context->options.event_mode);
        device->aoa_descriptor_policy_bytes = effective_options.aoa_descriptor_wire_policy_bytes;
        device->linux_hid = effective_options.linux_hid;
        device->target_ep0_policy_bytes = effective_options.target_ep0_data_policy_bytes;
        device->host_control_policy_bytes = effective_options.host_control_buffer_policy_bytes;
        device->validate_reports = effective_options.validate_reports == 1U;
        const auto candidate = copy_candidate(*selected);
        device->bus = candidate.bus;
        device->port_path = candidate.port_path;
        device->vendor_id = candidate.vendor_id;
        device->product_id = candidate.product_id;
        const aoahid_result result = aoa::transport::Device::open(
            context->runtime, candidate, device->config, &device->transport);
        if (result != AOAHID_OK) {
            return finish_transport_result(result, "device.open",
                                           "The selected USB/AOA device could not be opened.");
        }
        {
            const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
            // Every live Device can move from devices to graveyard without an
            // allocation. This keeps close teardown recoverable even under OOM.
            const std::size_t live_capacity =
                context->devices.size() + context->graveyard.size() + 1U;
            context->devices.reserve(live_capacity);
            context->graveyard.reserve(live_capacity);
            device->hid_id_domain_index = find_or_create_hid_id_domain(context, candidate);
            context->devices.push_back(device.get());
        }
    } catch (...) {
        if (device->transport != nullptr) {
            delete device->transport;
            device->transport = nullptr;
        }
        throw;
    }
    *out_device = device.release();
    aoa::detail::log_event(context, AOAHID_LOG_INFO, "device_open", *out_device, nullptr);
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("device.open")

aoahid_result AOAHID_CALL aoahid_node_open(aoahid_device* device, aoahid_spec* spec,
                                           const aoahid_node_options* options,
                                           const std::uint32_t control_timeout_ms,
                                           aoahid_node** out_node) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(device), "aoahid_node_open");
    aoa::detail::clear_error();
    if (out_node == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_node", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_node = nullptr;
    if (!timeout_is_set(control_timeout_ms, "control_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (device == nullptr || device->transport == nullptr || spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "node.open", "A live Device and immutable Spec are required.");
        return AOAHID_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_PARAM, "device",
                  "The Device handle was consumed by close and cannot open a Node.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result deferred = preflight_context(device->context);
    if (deferred != AOAHID_OK)
        return deferred;
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)), "node_options")) {
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result reservation = check_reservation(device, *options);
    if (reservation != AOAHID_OK)
        return reservation;
    const aoahid_result fits = check_spec_fits_device(device, spec);
    if (fits != AOAHID_OK)
        return fits;
    auto node = std::unique_ptr<aoahid_node>(new (std::nothrow) aoahid_node());
    if (node == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "node", "Memory allocation for the Node failed.");
        return AOAHID_ERR_INTERNAL;
    }
    bool retained = false;
    bool reservation_installed = false;
    try {
        node->device = device;
        node->spec = spec;
        node->active_completion_mutex =
            device->context->options.event_mode == AOAHID_EVENT_INTERNAL_THREAD
                ? &node->completion_mutex
                : nullptr;
        const aoahid_result allocated = allocate_hid_id(device, &node->hid_id);
        if (allocated != AOAHID_OK) {
            return allocated;
        }
        node->reserved_slots = options->reserved_slots;
        const aoahid_result initialized = initialize_node_state(node.get());
        if (initialized != AOAHID_OK) {
            return initialized;
        }
        // Complete every potentially allocating host-side operation before
        // request 54. After Android accepts a registration, publishing the
        // Node into this pre-reserved vector cannot allocate or orphan it.
        device->nodes.reserve(device->nodes.size() + 1U);
        if (!aoa::detail::retain_spec(spec)) {
            set_error(AOAHID_ERR_OVERFLOW, "spec.references",
                      "The Spec reference count cannot be incremented for this Node.");
            return AOAHID_ERR_OVERFLOW;
        }
        retained = true;
        const aoahid_result reserved_slots = device->transport->set_reservation(
            node->hid_id, node->reserved_slots, &node->reservation);
        if (reserved_slots != AOAHID_OK) {
            static_cast<void>(finish_transport_result(
                reserved_slots, "node.reserved_slots",
                "The requested transfer-pool reservation could not be installed.", node.get()));
            static_cast<void>(aoa::detail::release_spec(spec));
            retained = false;
            return reserved_slots;
        }
        reservation_installed = true;
    } catch (...) {
        if (reservation_installed) {
            static_cast<void>(device->transport->clear_reservation(node->reservation));
        }
        if (retained) {
            static_cast<void>(aoa::detail::release_spec(spec));
        }
        throw;
    }

    const aoahid_result registered = device->transport->register_hid(
        node->hid_id, spec->descriptor.data(), spec->descriptor.size(), control_timeout_ms);
    if (registered != AOAHID_OK) {
        aoa::detail::log_event(
            device->context, AOAHID_LOG_ERROR, "node_register", device, node.get(),
            aoa::detail::LogEventStatus{registered, 0, 54, 0U,
                                        static_cast<std::uint32_t>(spec->descriptor.size())});
        static_cast<void>(finish_transport_result(
            registered, "node.register", "AOA HID registration or descriptor transfer failed.",
            node.get(), 54, 0U, static_cast<std::uint32_t>(spec->descriptor.size())));
        static_cast<void>(device->transport->clear_reservation(node->reservation));
        static_cast<void>(aoa::detail::release_spec(spec));
        return registered;
    }

    // Capacity was established before request 54 and the element is a raw
    // pointer, so this publication performs neither allocation nor user code.
    device->nodes.push_back(node.get());
    *out_node = node.release();
    aoa::detail::log_event(
        device->context, AOAHID_LOG_INFO, "node_register", device, *out_node,
        aoa::detail::LogEventStatus{AOAHID_OK, 0, 54, 0U,
                                    static_cast<std::uint32_t>(spec->descriptor.size())});
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("node.open")

std::uint16_t AOAHID_CALL aoahid_node_id(const aoahid_node* node) try {
    aoa::detail::clear_error();
    if (node == nullptr || node->closed) {
        set_error(AOAHID_ERR_PARAM, "node",
                  "The Node handle is null or closed; zero is the failure sentinel.");
        return 0U;
    }
    return node->hid_id;
}
AOAHID_C_VALUE_CATCH("node.id", 0U)

aoahid_result AOAHID_CALL aoahid_node_manifest(const aoahid_node* node,
                                               aoahid_capability_manifest* manifest) try {
    aoa::detail::clear_error();
    if (node == nullptr || node->closed || node->spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "node.manifest", "A live node is required.");
        return AOAHID_ERR_PARAM;
    }
    return aoahid_spec_manifest(node->spec, manifest);
}
AOAHID_C_RESULT_CATCH("node.manifest")

aoahid_result AOAHID_CALL aoahid_node_submit(aoahid_node* node,
                                             const std::uint32_t send_timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(node), "aoahid_node_submit");
    aoa::detail::clear_error();
    if (!timeout_is_set(send_timeout_ms, "send_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "node.submit",
                  "A live Node attached to a Device and Spec is required.");
        return AOAHID_ERR_PARAM;
    }
    if (node->spec->kind == AOAHID_PROFILE_RAW) {
        set_error(AOAHID_ERR_UNSUPPORTED, "node.submit",
                  "Raw nodes require aoahid_raw_submit() with an explicit report buffer.");
        return AOAHID_ERR_UNSUPPORTED;
    }
    const aoahid_result deferred = preflight_context(node->device->context);
    if (deferred != AOAHID_OK)
        return deferred;
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(AOAHID_ERR_BUSY, "node.submit", "The node already has one report in flight.");
        return AOAHID_ERR_BUSY;
    }
    const aoahid_result completion = consume_completion(node, "node.completion");
    if (completion != AOAHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    // Input always goes first; the battery report is sent only by a submit
    // that has no input report to send.
    const bool battery_report = !node->dirty && node->battery_dirty;
    if (!node->dirty && !battery_report) {
        node->transfer_inflight.store(false, std::memory_order_release);
        aoa::detail::clear_error();
        return AOAHID_OK;
    }
    const aoa::hid::ReportLayout& layout =
        battery_report ? node->spec->battery_layout : node->spec->layout;
    aoa::transport::PreparedTransfer prepared{};
    const aoahid_result acquired =
        node->device->transport->acquire(node->hid_id, node->reservation, layout.wire_bytes, node,
                                         &aoa::detail::transfer_complete, &prepared);
    if (acquired != AOAHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return finish_transport_result(acquired, "node.submit",
                                       "No transfer slot could be acquired for AOA request 57.",
                                       node, 57, 0U, static_cast<std::uint32_t>(layout.wire_bytes));
    }
    auto* scan_touch =
        battery_report ? nullptr : std::get_if<aoa::detail::TouchState>(&node->state);
    const auto* scan_config = std::get_if<aoa::detail::TouchConfig>(&node->spec->config);
    const bool update_scan_time = scan_touch != nullptr && scan_touch->packet_cursor == 0U &&
                                  scan_config != nullptr &&
                                  scan_config->fields.enable_scan_time == 1U;
    const std::uint32_t previous_scan_time =
        update_scan_time ? scan_touch->scan_time : std::uint32_t{0};
    const auto previous_scan_epoch =
        update_scan_time ? scan_touch->scan_epoch : std::chrono::steady_clock::time_point{};
    const bool previous_scan_epoch_active = update_scan_time && scan_touch->scan_epoch_active;
    const auto rollback_scan_time = [&]() noexcept {
        if (update_scan_time) {
            scan_touch->scan_time = previous_scan_time;
            scan_touch->scan_epoch = previous_scan_epoch;
            scan_touch->scan_epoch_active = previous_scan_epoch_active;
        }
    };
    if (update_scan_time) {
        const auto now = std::chrono::steady_clock::now();
        if (!scan_touch->scan_epoch_active) {
            scan_touch->scan_epoch = now;
            scan_touch->scan_epoch_active = true;
            scan_touch->scan_time = 0U;
        } else {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::microseconds>(now - scan_touch->scan_epoch)
                    .count();
            const std::uint64_t ticks =
                elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed) / 100U;
            const std::uint64_t modulus =
                static_cast<std::uint64_t>(scan_config->fields.scan_time.logical_maximum) + 1U;
            scan_touch->scan_time = static_cast<std::uint32_t>(ticks % modulus);
        }
    }
    std::size_t report_length = 0U;
    const aoahid_result serialized =
        battery_report ? aoa::detail::serialize_battery(node, prepared.payload, prepared.capacity,
                                                        &report_length)
                       : aoa::detail::serialize_node(node, prepared.payload, prepared.capacity,
                                                     &report_length);
    if (serialized != AOAHID_OK) {
        node->device->transport->abandon(prepared);
        rollback_scan_time();
        node->transfer_inflight.store(false, std::memory_order_release);
        return serialized;
    }
    if (node->device->validate_reports) {
        const aoahid_result validated = aoa::detail::validate_serialized_report(
            node->spec, layout, prepared.payload, report_length);
        if (validated != AOAHID_OK) {
            node->device->transport->abandon(prepared);
            rollback_scan_time();
            node->transfer_inflight.store(false, std::memory_order_release);
            return validated;
        }
    }
    node->submitted_battery = battery_report;
    node->submitted_non_neutral = !battery_report && has_non_neutral_state(node);
    node->submitted_report_length = report_length;
    node->submitted_report_id = battery_report ? layout.report_id : report_id(node);
    const aoahid_result result = node->device->transport->submit(
        prepared, report_length, node->submitted_report_id, send_timeout_ms);
    if (result != AOAHID_OK) {
        // Submit-time failures invoke the completion path synchronously. The
        // return value already reports that failure, so do not report it twice.
        rollback_scan_time();
        const aoahid_result completion_failure = consume_completion(node, "node.submit");
        if (completion_failure == AOAHID_OK) {
            return finish_transport_result(
                result, "node.submit",
                "libusb rejected AOA request 57 before asynchronous completion.", node, 57, 0U,
                static_cast<std::uint32_t>(report_length));
        }
        return result;
    }
    if (result == AOAHID_OK &&
        node->device->context->options.event_mode == AOAHID_EVENT_CALLER_POLL &&
        node->transfer_inflight.load(std::memory_order_acquire)) {
        const aoahid_result poll_result = node->device->context->runtime->poll(0U);
        if (poll_result != AOAHID_OK) {
            latch_context_error(
                node->device->context,
                current_deferred_error(poll_result, "context.event_pump",
                                       "The opportunistic caller-poll event handler failed after "
                                       "accepting the report."));
        }
    }
    aoa::detail::clear_error();
    return result;
}
AOAHID_C_RESULT_CATCH("node.submit")

aoahid_result AOAHID_CALL aoahid_node_submit_blocking(aoahid_node* node,
                                                      const std::uint32_t timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(node), "aoahid_node_submit_blocking");
    aoa::detail::clear_error();
    if (node == nullptr || node->closed || node->device == nullptr || node->spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "node.submit_blocking",
                  "A live Node with an attached Device and Spec is required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result result = poll_until_idle(node, timeout_ms);
    return aoa::detail::finish_result(result, "node.submit_blocking",
                                      "The blocking report sequence failed.");
}
AOAHID_C_RESULT_CATCH("node.submit_blocking")

aoahid_result AOAHID_CALL aoahid_raw_submit(aoahid_node* node, const std::uint8_t* report,
                                            const std::size_t length,
                                            const std::uint32_t send_timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(node), "aoahid_raw_submit");
    aoa::detail::clear_error();
    if (!timeout_is_set(send_timeout_ms, "send_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->transport == nullptr || node->spec == nullptr ||
        node->spec->kind != AOAHID_PROFILE_RAW || report == nullptr) {
        set_error(AOAHID_ERR_PARAM, "raw.submit", "A live idle raw node and report are required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result deferred = preflight_context(node->device->context);
    if (deferred != AOAHID_OK)
        return deferred;
    if (node->transfer_inflight.exchange(true, std::memory_order_acq_rel)) {
        set_error(AOAHID_ERR_BUSY, "raw.submit", "The raw node already has one report in flight.");
        return AOAHID_ERR_BUSY;
    }
    const aoahid_result completion = consume_completion(node, "raw.completion");
    if (completion != AOAHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return completion;
    }
    const auto* raw = std::get_if<aoa::detail::RawConfig>(&node->spec->config);
    const auto accepted =
        std::find_if(raw->reports.begin(), raw->reports.end(), [=](const auto& item) {
            return item.wire_length == length &&
                   ((item.report_id.enabled == 0U) ||
                    (length != 0U && report[0] == item.report_id.value));
        });
    if (accepted == raw->reports.end()) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(AOAHID_ERR_PARAM, "raw.report",
                  "The report ID or exact wire length is not accepted by the raw spec.");
        return AOAHID_ERR_PARAM;
    }
    const std::size_t accepted_index = static_cast<std::size_t>(accepted - raw->reports.begin());
    const std::uint8_t valid_mask = raw->trailing_valid_masks[accepted_index];
    if ((report[length - 1U] & static_cast<std::uint8_t>(~valid_mask)) != 0U) {
        node->transfer_inflight.store(false, std::memory_order_release);
        set_error(AOAHID_ERR_PARAM, "raw.report.trailing_padding",
                  "Unused trailing Input-report bits must be zero.", 0, 57, node->hid_id,
                  accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U,
                  static_cast<std::uint32_t>(length - 1U), 1U);
        return AOAHID_ERR_PARAM;
    }
    aoa::transport::PreparedTransfer prepared{};
    aoahid_result result = node->device->transport->acquire(
        node->hid_id, node->reservation, length, node, &aoa::detail::transfer_complete, &prepared);
    if (result != AOAHID_OK) {
        node->transfer_inflight.store(false, std::memory_order_release);
        return finish_transport_result(result, "raw.submit",
                                       "No transfer slot could be acquired for raw AOA request 57.",
                                       node, 57, 0U,
                                       static_cast<std::uint32_t>(std::min<std::size_t>(
                                           length, std::numeric_limits<std::uint32_t>::max())));
    }
    std::memcpy(prepared.payload, report, length);
    node->dirty = true;
    node->submitted_non_neutral = false;
    node->submitted_report_length = length;
    node->submitted_report_id = accepted->report_id.enabled == 1U ? accepted->report_id.value : 0U;
    result = node->device->transport->submit(prepared, length, node->submitted_report_id,
                                             send_timeout_ms);
    if (result != AOAHID_OK) {
        const aoahid_result completion_failure = consume_completion(node, "raw.submit");
        if (completion_failure == AOAHID_OK) {
            return finish_transport_result(
                result, "raw.submit",
                "libusb rejected raw AOA request 57 before asynchronous completion.", node, 57, 0U,
                static_cast<std::uint32_t>(
                    std::min<std::size_t>(length, std::numeric_limits<std::uint32_t>::max())));
        }
        return result;
    }
    aoa::detail::clear_error();
    return result;
}
AOAHID_C_RESULT_CATCH("raw.submit")

aoahid_result AOAHID_CALL aoahid_node_close(aoahid_node* node,
                                            const std::uint32_t control_timeout_ms,
                                            const std::uint32_t drain_timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(node), "aoahid_node_close");
    aoa::detail::clear_error();
    if (!timeout_is_set(control_timeout_ms, "control_timeout_ms") ||
        !timeout_is_set(drain_timeout_ms, "drain_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (node == nullptr || node->closed || node->device == nullptr ||
        node->device->transport == nullptr ||
        node->device->closing.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_PARAM, "node.close",
                  "The Node handle is null, closed, detached, or owned by a closing Device.");
        return AOAHID_ERR_PARAM;
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);
    return close_live_node_until(node, deadline, control_timeout_ms);
}
AOAHID_C_RESULT_CATCH("node.close")

static aoahid_result device_close_impl(aoahid_device* device, bool* consumed,
                                       // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                                       const std::uint32_t control_timeout_ms,
                                       const std::uint32_t drain_timeout_ms) try {
    aoa::detail::clear_error();
    if (consumed == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "device.close",
                  "The internal Device ownership result pointer is null.");
        return AOAHID_ERR_INTERNAL;
    }
    *consumed = false;
    if (device == nullptr || device->transport == nullptr || device->context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "device.close",
                  "A live Device attached to its Context and transport is required.");
        return AOAHID_ERR_PARAM;
    }
    aoahid_context* context = device->context;
    // Device open reserves both Context vectors for every live graph. Moving
    // this pointer from devices to graveyard therefore cannot allocate after
    // the closing flag consumes the public handle.
    {
        const aoa::detail::ModeMutexGuard guard(context->active_state_mutex);
        bool expected = false;
        if (!device->closing.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            set_error(AOAHID_ERR_PARAM, "device.close",
                      "The Device handle was already consumed by an earlier close call.");
            return AOAHID_ERR_PARAM;
        }
        auto& devices = context->devices;
        devices.erase(std::remove(devices.begin(), devices.end(), device), devices.end());
        *consumed = true;
        // The Context graveyard may finish this teardown after the call returns.
        device->close_control_timeout_ms = control_timeout_ms;
        device->close_drain_timeout_ms = drain_timeout_ms;
    }

    try {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(device->close_drain_timeout_ms);
        while (!device->nodes.empty()) {
            aoahid_node* closing_node = device->nodes.back();
            const aoahid_result result =
                close_live_node_until(closing_node, deadline, control_timeout_ms);
            // The pending Node and the ones not closed yet go to the graveyard with the Device.
            if (result == AOAHID_CLOSE_PENDING)
                break;
            remember_current_close_error(device, result, "node.close",
                                         "A Node close step failed during Device teardown.");
        }
    } catch (const std::bad_alloc&) {
        remember_close_error(
            device,
            make_deferred_error(DeferredErrorInput{
                aoa::transport::ErrorInfo{AOAHID_ERR_INTERNAL, 0, 0, 0U, 0U, 0U}, "device.close",
                "Memory allocation failed after Device ownership was consumed; teardown "
                "continued."}));
    } catch (...) {
        remember_close_error(
            device,
            make_deferred_error(DeferredErrorInput{
                aoa::transport::ErrorInfo{AOAHID_ERR_INTERNAL, 0, 0, 0U, 0U, 0U}, "device.close",
                "An internal operation failed after Device ownership was consumed; teardown "
                "continued."}));
    }

    // Channels share the Device's handle; cancel them with its transfers.
    for (aoahid_channel* channel : device->channels) {
        channel->transport->lose();
    }
    const aoahid_result cancel = device->transport->cancel_all();
    if (cancel != AOAHID_OK && cancel != AOAHID_ERR_NO_DEVICE) {
        static_cast<void>(finish_transport_result(cancel, "device.cancel",
                                                  "libusb transfer cancellation failed."));
        remember_current_close_error(device, cancel, "device.cancel",
                                     "libusb transfer cancellation failed.", nullptr, 57);
    }
    for (aoahid_node* node : device->nodes) {
        node->closed = true;
    }
    if (!device->channels.empty()) {
        // Read-ahead IN transfers are always in flight; give their
        // cancellation the close budget (pumping in caller-poll mode) before
        // falling back to the graveyard. A Device without Channels skips this.
        const auto channel_deadline = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(device->close_drain_timeout_ms);
        while (!graph_drained(device) && remaining_milliseconds(channel_deadline) != 0U) {
            if (context->options.event_mode == AOAHID_EVENT_CALLER_POLL) {
                const aoahid_result poll = context->runtime->poll(1U);
                if (poll != AOAHID_OK) {
                    static_cast<void>(finish_transport_result(poll, "context.poll",
                                                              "libusb event handling failed."));
                    remember_current_close_error(device, poll, "context.poll",
                                                 "libusb event handling failed while Channel "
                                                 "transfers drained during Device teardown.");
                    break;
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    if (graph_drained(device)) {
        const aoa::detail::DeferredError outcome = destroy_drained_device(device);
        return publish_deferred_error(outcome);
    }

    // The Context finishes the teardown once the callbacks have run (nodes and retained Specs are
    // their user_data); aoahid_context_poll and aoahid_context_destroy report its errors.
    aoa::detail::log_event(context, AOAHID_LOG_INFO, "device_close_pending", device, nullptr,
                           aoa::detail::LogEventStatus{AOAHID_CLOSE_PENDING});
    aoa::detail::clear_error();
    // In internal-thread mode the event thread may reap the Device as soon as it is published.
    park_in_graveyard(context, device);
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("device.close")

aoahid_result AOAHID_CALL aoahid_device_close(aoahid_device* device,
                                              const std::uint32_t control_timeout_ms,
                                              const std::uint32_t drain_timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(device), "aoahid_device_close");
    aoa::detail::clear_error();
    if (device != nullptr && (!timeout_is_set(control_timeout_ms, "control_timeout_ms") ||
                              !timeout_is_set(drain_timeout_ms, "drain_timeout_ms"))) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    bool consumed = false;
    return device_close_impl(device, &consumed, control_timeout_ms, drain_timeout_ms);
}
AOAHID_C_RESULT_CATCH("device.close")

aoahid_result AOAHID_CALL aoahid_device_latched_error(aoahid_device* device) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(device), "aoahid_device_latched_error");
    aoa::detail::clear_error();
    if (device == nullptr || device->closing.load(std::memory_order_acquire) ||
        device->transport == nullptr) {
        set_error(AOAHID_ERR_PARAM, "device.latched_error",
                  "A live, open Device handle is required.");
        return AOAHID_ERR_PARAM;
    }
    aoa::transport::ErrorInfo error{};
    const aoahid_result result = device->transport->latched_error(&error);
    if (result == AOAHID_OK) {
        return preflight_context(device->context);
    }
    set_error(result, "device.latched_error",
              error.aoa_request == 57 ? completion_reason(result)
                                      : "The Device retained a prior transport failure.",
              error.native_status, error.aoa_request, error.hid_id, error.report_id, error.offset,
              error.length);
    return result;
}
AOAHID_C_RESULT_CATCH("device.latched_error")

aoahid_result AOAHID_CALL aoahid_channel_open(aoahid_device* device,
                                              const aoahid_channel_options* options,
                                              const std::uint32_t drain_timeout_ms,
                                              aoahid_channel** out_channel) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(device), "aoahid_channel_open");
    aoa::detail::clear_error();
    if (out_channel == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_channel", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_channel = nullptr;
    if (!timeout_is_set(drain_timeout_ms, "drain_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (device == nullptr || device->transport == nullptr || device->context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "channel.open", "A live Device is required.");
        return AOAHID_ERR_PARAM;
    }
    if (device->closing.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_PARAM, "device",
                  "The Device handle was consumed by close and cannot open a Channel.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result deferred = preflight_context(device->context);
    if (deferred != AOAHID_OK)
        return deferred;
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "channel_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U || options->reserved8 != 0U ||
        !aoa::detail::valid_boolean(options->zero_length_termination) ||
        (options->read_mode != AOAHID_CHANNEL_READ_STREAM &&
         options->read_mode != AOAHID_CHANNEL_READ_REQUEST) ||
        options->out_transfers == 0U || options->transfer_bytes == 0U ||
        (options->read_mode == AOAHID_CHANNEL_READ_STREAM && options->in_transfers == 0U)) {
        set_error(AOAHID_ERR_UNSET_FIELD, "channel_options",
                  "Reserved fields must be zero, zero_length_termination exactly zero or one, "
                  "read_mode STREAM or REQUEST, and out_transfers, transfer_bytes and (for "
                  "STREAM) in_transfers nonzero.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    aoa::transport::ChannelConfig config{};
    config.interface_class = options->interface_class;
    config.interface_subclass = options->interface_subclass;
    config.interface_protocol = options->interface_protocol;
    config.in_transfers = options->in_transfers;
    config.out_transfers = options->out_transfers;
    config.transfer_bytes = options->transfer_bytes;
    config.zero_length_termination = options->zero_length_termination == 1U;
    config.request_reads = options->read_mode == AOAHID_CHANNEL_READ_REQUEST;
    config.pump = device->context->options.event_mode == AOAHID_EVENT_CALLER_POLL
                      ? device->context->runtime
                      : nullptr;

    auto channel = std::unique_ptr<aoahid_channel>(new (std::nothrow) aoahid_channel());
    if (channel == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "channel", "Memory allocation for the Channel failed.");
        return AOAHID_ERR_INTERNAL;
    }
    // Reserve first so publishing an opened Channel cannot fail.
    device->channels.reserve(device->channels.size() + 1U);
    aoahid_result poll = AOAHID_OK;
    if (!wait_for_report_drain(
            device, std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms),
            &poll)) {
        if (poll != AOAHID_OK) {
            return finish_transport_result(poll, "context.poll", "libusb event handling failed.");
        }
        set_error(AOAHID_ERR_TIMEOUT, "channel.open",
                  "In-flight report transfers did not complete within the close drain budget.");
        return AOAHID_ERR_TIMEOUT;
    }
    const aoahid_result opened =
        aoa::transport::Channel::open(device->transport->port(), config, &channel->transport);
    if (opened != AOAHID_OK) {
        return finish_transport_result(
            opened, "channel.open",
            opened == AOAHID_ERR_UNSUPPORTED
                ? "No interface has the requested class triple and a Bulk IN/OUT pair."
                : "The Bulk interface could not be read, claimed, or started.");
    }
    channel->device = device;
    device->channels.push_back(channel.get());
    *out_channel = channel.release();
    aoa::detail::log_event(device->context, AOAHID_LOG_INFO, "channel_open", device, nullptr);
    aoa::detail::clear_error();
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("channel.open")

aoahid_result AOAHID_CALL aoahid_channel_close(aoahid_channel* channel,
                                               const std::uint32_t drain_timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context(channel), "aoahid_channel_close");
    aoa::detail::clear_error();
    if (channel != nullptr && !timeout_is_set(drain_timeout_ms, "drain_timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (channel == nullptr || channel->device == nullptr || channel->transport == nullptr ||
        channel->device->closing.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_PARAM, "channel.close",
                  "The Channel handle is null, detached, or owned by a closing Device.");
        return AOAHID_ERR_PARAM;
    }
    aoahid_device* device = channel->device;
    channel->transport->lose();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);
    while (!channel->transport->drained()) {
        const std::uint32_t remaining = remaining_milliseconds(deadline);
        if (remaining == 0U) {
            set_error(AOAHID_CLOSE_PENDING, "channel.close",
                      "Cancelled Bulk transfers did not complete within the close drain budget; "
                      "the lost Channel remains caller-owned.");
            return AOAHID_CLOSE_PENDING;
        }
        if (device->context->options.event_mode == AOAHID_EVENT_CALLER_POLL) {
            if (device->context->runtime->poll(1U) != AOAHID_OK) {
                set_error(AOAHID_CLOSE_PENDING, "channel.close",
                          "libusb event handling failed before the cancelled Bulk transfers "
                          "completed; the Channel remains caller-owned.",
                          aoa::transport::last_error().native_status);
                return AOAHID_CLOSE_PENDING;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    // A report still in flight at the deadline does not keep the Channel open.
    aoahid_result poll = AOAHID_OK;
    static_cast<void>(wait_for_report_drain(device, deadline, &poll));
    auto& channels = device->channels;
    channels.erase(std::remove(channels.begin(), channels.end(), channel), channels.end());
    release_channel_storage(channel);
    aoa::detail::log_event(device->context, AOAHID_LOG_INFO, "channel_close", device, nullptr);
    return finish_transport_result(poll, "context.poll", "libusb event handling failed.");
}
AOAHID_C_RESULT_CATCH("channel.close")

aoahid_result AOAHID_CALL aoahid_channel_write(aoahid_channel* channel, const std::uint8_t* data,
                                               const std::size_t length, std::size_t* out_written,
                                               const std::uint32_t timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context_caller_poll(channel),
                      "aoahid_channel_write");
    aoa::detail::clear_error();
    if (channel == nullptr || channel->transport == nullptr) {
        if (out_written != nullptr)
            *out_written = 0U;
        set_error(AOAHID_ERR_PARAM, "channel.write", "A live Channel is required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result result = channel->transport->write(data, length, out_written, timeout_ms);
    if (result != AOAHID_OK) {
        return finish_transport_result(
            result, "channel.write",
            result == AOAHID_ERR_TIMEOUT ? "Every OUT transfer stayed in flight until the timeout."
                                         : "The Bulk OUT path failed or the Channel is lost.");
    }
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("channel.write")

aoahid_result AOAHID_CALL aoahid_channel_read(aoahid_channel* channel, std::uint8_t* buffer,
                                              const std::size_t capacity, std::size_t* out_received,
                                              const std::uint32_t timeout_ms) try {
    AOAHID_SERIALIZED(aoa::detail::serialization_context_caller_poll(channel),
                      "aoahid_channel_read");
    aoa::detail::clear_error();
    if (channel == nullptr || channel->transport == nullptr) {
        if (out_received != nullptr)
            *out_received = 0U;
        set_error(AOAHID_ERR_PARAM, "channel.read", "A live Channel is required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result result =
        channel->transport->read(buffer, capacity, out_received, timeout_ms);
    if (result != AOAHID_OK) {
        return finish_transport_result(result, "channel.read",
                                       result == AOAHID_ERR_TIMEOUT
                                           ? "No Bulk IN data arrived before the timeout."
                                           : "The Bulk IN path failed or the Channel is lost.");
    }
    return AOAHID_OK;
}
AOAHID_C_RESULT_CATCH("channel.read")

static aoahid_result context_destroy_impl(aoahid_context* context, bool* context_consumed,
                                          const std::uint32_t control_timeout_ms,
                                          const std::uint32_t drain_timeout_ms) {
    *context_consumed = false;
    aoa::detail::clear_error();
    if (context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "context.destroy", "The Context handle is null.");
        return AOAHID_ERR_PARAM;
    }
    aoa::detail::DeferredError close_error{};
    for (;;) {
        aoahid_device* device = last_active_device(context);
        if (device == nullptr) {
            break;
        }
        bool device_consumed = false;
        const aoahid_result close =
            device_close_impl(device, &device_consumed, control_timeout_ms, drain_timeout_ms);
        if (!device_consumed) {
            const aoahid_result retained_result = close == AOAHID_OK ? AOAHID_ERR_INTERNAL : close;
            const aoa::detail::DeferredError error = current_deferred_error(
                retained_result, "device.close",
                "A Device teardown operation failed before ownership transfer.");
            // No Device pointer access is permitted after device_close_impl:
            // only its explicit ownership result decides whether this Context
            // remains retryable.
            latch_context_error(context, error);
            set_error(AOAHID_CLOSE_PENDING, "context.destroy",
                      "A Device could not yet transfer ownership to Context teardown; the Context "
                      "remains caller-owned.",
                      error.transport.native_status, error.transport.aoa_request,
                      error.transport.hid_id, error.report_id, error.transport.offset,
                      error.transport.length);
            return AOAHID_CLOSE_PENDING;
        }
        if (close != AOAHID_OK && close != AOAHID_CLOSE_PENDING) {
            const aoa::detail::DeferredError error = current_deferred_error(
                close, "device.close",
                "A Device teardown operation failed while destroying its Context.");
            if (!close_error.pending)
                close_error = error;
        }
    }
    latch_context_error(context, close_error);

    reap_graveyard(context);
    if (!graveyard_empty(context)) {
        aoa::detail::DeferredError prior{};
        static_cast<void>(peek_context_error(context, &prior));
        set_error(AOAHID_CLOSE_PENDING, "context.destroy",
                  "The Context graveyard still owns Device graphs with pending callbacks.",
                  prior.transport.native_status, prior.transport.aoa_request,
                  prior.transport.hid_id, prior.report_id, prior.transport.offset,
                  prior.transport.length);
        return AOAHID_CLOSE_PENDING;
    }

    request_event_thread_stop(context);
    if (context->event_thread.joinable())
        context->event_thread.join();
    aoa::detail::DeferredError terminal{};
    // A stopped event pump is the terminal cause that made caller takeover
    // necessary. Prefer it over an older recoverable poll diagnostic while
    // still clearing ordinary deferred storage before Context destruction.
    if (peek_terminal_event_error(context, &terminal)) {
        aoa::detail::DeferredError discarded{};
        static_cast<void>(take_context_error(context, &discarded));
    } else {
        static_cast<void>(take_context_error(context, &terminal));
    }
    if (terminal.pending && terminal.transport.result == AOAHID_CLOSE_PENDING) {
        terminal = make_deferred_error(DeferredErrorInput{
            aoa::transport::ErrorInfo{AOAHID_ERR_INTERNAL, terminal.transport.native_status,
                                      terminal.transport.aoa_request, terminal.transport.hid_id,
                                      terminal.transport.offset, terminal.transport.length},
            "context.destroy",
            "Context teardown reached a terminal state with an invalid CLOSE_PENDING diagnostic.",
            terminal.report_id});
    }
    // Publish and localize the terminal result before deleting Context. The
    // blocking wrapper may inspect Context only when this value is precisely
    // CLOSE_PENDING, which is excluded above for every consuming path.
    const aoahid_result terminal_result = publish_deferred_error(terminal);
    aoa::detail::emit_log(
        context, terminal_result == AOAHID_OK ? AOAHID_LOG_INFO : AOAHID_LOG_ERROR,
        terminal_result == AOAHID_OK ? "event=context_destroy result=AOAHID_OK"
                                     : "event=context_destroy result=deferred_error");
    *context_consumed = true;
    delete context->runtime;
    delete context;
    return terminal_result;
}

aoahid_result AOAHID_CALL aoahid_context_destroy(aoahid_context* context,
                                                 const std::uint32_t control_timeout_ms,
                                                 const std::uint32_t drain_timeout_ms) try {
    AOAHID_SERIALIZED_PROBE(context, "aoahid_context_destroy");
    aoa::detail::clear_error();
    if (context != nullptr && (!timeout_is_set(control_timeout_ms, "control_timeout_ms") ||
                               !timeout_is_set(drain_timeout_ms, "drain_timeout_ms"))) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    bool consumed = false;
    return context_destroy_impl(context, &consumed, control_timeout_ms, drain_timeout_ms);
}
AOAHID_C_RESULT_CATCH("context.destroy")

aoahid_result AOAHID_CALL aoahid_context_destroy_blocking(
    aoahid_context* context, const std::uint32_t control_timeout_ms,
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    const std::uint32_t drain_timeout_ms, const std::uint32_t timeout_ms) try {
    AOAHID_SERIALIZED_PROBE(context, "aoahid_context_destroy_blocking");
    aoa::detail::clear_error();
    if (context == nullptr) {
        set_error(AOAHID_ERR_PARAM, "context.destroy", "The Context handle is null.");
        return AOAHID_ERR_PARAM;
    }
    if (!timeout_is_set(control_timeout_ms, "control_timeout_ms") ||
        !timeout_is_set(drain_timeout_ms, "drain_timeout_ms") ||
        !timeout_is_set(timeout_ms, "timeout_ms")) {
        return AOAHID_ERR_UNSET_FIELD;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    aoa::detail::DeferredError observed{};
    do {
        bool consumed = false;
        const aoahid_result result =
            context_destroy_impl(context, &consumed, control_timeout_ms, drain_timeout_ms);
        if (consumed) {
            return observed.pending ? publish_deferred_error(observed) : result;
        }
        if (result != AOAHID_CLOSE_PENDING) {
            return observed.pending ? publish_deferred_error(observed) : result;
        }
        if (context->options.event_mode == AOAHID_EVENT_CALLER_POLL) {
            const aoahid_result poll = aoahid_context_poll(context, 1U);
            if (poll != AOAHID_OK && !observed.pending) {
                observed = current_deferred_error(
                    poll, "context.poll",
                    "libusb event handling failed while draining Context teardown.");
            }
        } else if (context->event_thread_failed.load(std::memory_order_acquire)) {
            // A terminal event-thread failure leaves no pump to deliver cancel
            // callbacks. Stop and join that thread before the caller takes over;
            // the normal internal-thread path never polls here, so two threads
            // cannot enter libusb event handling concurrently.
            if (context->event_thread.joinable()) {
                if (!context->event_thread_exited.load(std::memory_order_acquire)) {
                    request_event_thread_stop(context);
                }
                if (context->event_thread.get_id() != std::this_thread::get_id()) {
                    context->event_thread.join();
                }
            }
            const aoahid_result poll = context->runtime->poll(1U);
            reap_graveyard(context);
            if (poll != AOAHID_OK) {
                latch_context_error(
                    context, current_deferred_error(poll, "context.poll",
                                                    "Caller fallback event handling failed after "
                                                    "the internal event thread terminated."));
                const auto quantum =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(10U);
                std::unique_lock<std::mutex> lock(context->mutex);
                static_cast<void>(
                    context->graveyard_cv.wait_until(lock, std::min(deadline, quantum)));
            }
        } else {
            std::unique_lock<std::mutex> lock(context->mutex);
            const auto quantum = std::chrono::steady_clock::now() + std::chrono::milliseconds(10U);
            static_cast<void>(context->graveyard_cv.wait_until(lock, std::min(deadline, quantum)));
        }
    } while (std::chrono::steady_clock::now() < deadline);
    aoa::detail::DeferredError deferred{};
    if (observed.pending) {
        latch_context_error(context, observed);
        deferred = observed;
    } else {
        static_cast<void>(peek_context_error(context, &deferred));
    }
    set_error(AOAHID_CLOSE_PENDING, "context.destroy",
              "Teardown did not drain before the deadline; the Context remains caller-owned.",
              deferred.transport.native_status, deferred.transport.aoa_request,
              deferred.transport.hid_id, deferred.report_id, deferred.transport.offset,
              deferred.transport.length);
    return AOAHID_CLOSE_PENDING;
}
AOAHID_C_RESULT_CATCH("context.destroy_blocking")

} // extern "C"

#undef AOAHID_C_VOID_CATCH
#undef AOAHID_C_VALUE_CATCH
#undef AOAHID_C_RESULT_CATCH
