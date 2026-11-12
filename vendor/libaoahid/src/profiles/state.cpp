// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Profile state machines and bit-exact serializers. Mutators do no I/O and
 * allocate no memory after node construction. A node permits one report in
 * flight, which makes completion accounting deterministic for relative axes.
 */

#include "api/internal.hpp"
#include "hid/usages.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <new>

namespace {
using aoa::detail::ContactPhase;
using aoa::detail::set_error;
using aoa::hid::FieldLayout;
using aoa::hid::FieldSemantic;

aoahid_result link_available(const aoahid_node* node, const char* field) noexcept {
    if (node == nullptr) {
        set_error(AOAHID_ERR_PARAM, field, "The node is not attached to a live device.");
        return AOAHID_ERR_PARAM;
    }
    // Pure state-machine tests construct the private node representation
    // without a transport. Every public Node is attached by aoahid_node_open().
    if (node->device == nullptr) {
        return AOAHID_OK;
    }
    if (node->device->transport == nullptr) {
        set_error(AOAHID_ERR_PARAM, field, "The node is not attached to a live device.");
        return AOAHID_ERR_PARAM;
    }
    if (node->device->transport->present()) {
        return AOAHID_OK;
    }
    aoa::transport::ErrorInfo error{};
    static_cast<void>(node->device->transport->latched_error(&error));
    const std::uint16_t failed_report_id =
        error.aoa_request == 57 && error.hid_id == node->hid_id
            ? error.report_id
            : (node->spec != nullptr && node->spec->layout.has_report_id
                   ? node->spec->layout.report_id
                   : 0U);
    set_error(AOAHID_ERR_NO_DEVICE, field, "The device is gone; its node state is frozen.",
              error.native_status, error.aoa_request, node->hid_id, failed_report_id, error.offset,
              error.length);
    return AOAHID_ERR_NO_DEVICE;
}

aoahid_result usable(const aoahid_node* node, const aoahid_profile_kind kind,
                     const char* field) noexcept {
    if (node == nullptr || node->closed || node->spec == nullptr || node->spec->kind != kind) {
        set_error(AOAHID_ERR_PARAM, field, "The operation does not match the node profile.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result link = link_available(node, field);
    if (link != AOAHID_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_BUSY, field, "The node has one report in flight.");
        return AOAHID_ERR_BUSY;
    }
    return AOAHID_OK;
}

bool in_range(const std::int64_t value, const aoahid_integer_field& field) noexcept {
    return value >= field.logical_minimum && value <= field.logical_maximum;
}

bool add_checked(std::int64_t* target, const std::int32_t value) noexcept {
    if ((value > 0 && *target > std::numeric_limits<std::int64_t>::max() - value) ||
        (value < 0 && *target < std::numeric_limits<std::int64_t>::min() - value)) {
        return false;
    }
    *target += value;
    return true;
}

bool store_bits(std::uint8_t* report, const std::size_t capacity, const FieldLayout& field,
                const std::int64_t value, const bool allow_null = false) noexcept {
    if (report == nullptr || field.bit_width == 0U || field.bit_width > 32U ||
        field.bit_offset + field.bit_width > capacity * 8U ||
        (allow_null && !field.has_null_state)) {
        return false;
    }
    if (!allow_null && (value < field.logical_minimum || value > field.logical_maximum)) {
        return false;
    }
    std::uint64_t encoded =
        static_cast<std::uint64_t>(value) & ((std::uint64_t{1} << field.bit_width) - 1U);
    std::size_t position = field.bit_offset;
    std::uint32_t remaining = field.bit_width;
    // A field spans at most four bytes, so it is written a byte at a time.
    while (remaining != 0U) {
        const auto shift = static_cast<std::uint32_t>(position & 7U);
        const std::uint32_t taken = std::min(remaining, 8U - shift);
        const std::uint32_t byte_mask = ((1U << taken) - 1U) << shift;
        std::uint8_t& target = report[position / 8U];
        target =
            static_cast<std::uint8_t>((static_cast<std::uint32_t>(target) & ~byte_mask) |
                                      ((static_cast<std::uint32_t>(encoded) << shift) & byte_mask));
        encoded >>= taken;
        position += taken;
        remaining -= taken;
    }
    return true;
}

std::int64_t hat_null(const FieldLayout& field) noexcept {
    if (field.is_signed) {
        const std::int64_t minimum = field.bit_width == 32U
                                         ? std::numeric_limits<std::int32_t>::min()
                                         : -(std::int64_t{1} << (field.bit_width - 1U));
        if (minimum < field.logical_minimum) {
            return minimum;
        }
        return field.bit_width == 32U ? std::numeric_limits<std::int32_t>::max()
                                      : (std::int64_t{1} << (field.bit_width - 1U)) - 1;
    }
    const std::int64_t maximum =
        field.bit_width == 32U
            ? static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())
            : (std::int64_t{1} << field.bit_width) - 1;
    if (0 < field.logical_minimum) {
        return 0;
    }
    return maximum;
}

template <typename T> const T* config(const aoahid_node* node) noexcept {
    return std::get_if<T>(&node->spec->config);
}

template <typename T> T* state(aoahid_node* node) noexcept { return std::get_if<T>(&node->state); }

aoahid_result pending_transition(const char* field) noexcept {
    set_error(AOAHID_ERR_BUSY, field,
              "An opposite lifecycle transition is pending its first accepted report.");
    return AOAHID_ERR_BUSY;
}

bool transition_storage_matches(const std::vector<std::uint8_t>& values,
                                const std::vector<std::uint8_t>& transitions,
                                const char* field) noexcept {
    if (values.size() == transitions.size()) {
        return true;
    }
    set_error(AOAHID_ERR_INTERNAL, field,
              "The preallocated lifecycle-transition state does not match the profile layout.");
    return false;
}

std::uint32_t pen_barrel_changes(const aoahid_pen_sample& before,
                                 const aoahid_pen_sample& after) noexcept {
    return before.barrel_buttons ^ after.barrel_buttons;
}

bool pen_transition_conflicts(const aoa::detail::PenState& pen,
                              const aoahid_pen_sample& next) noexcept {
    const bool changes_lifecycle =
        pen.desired.in_range != next.in_range || pen.desired.tip != next.tip ||
        pen.desired.eraser != next.eraser || pen_barrel_changes(pen.desired, next) != 0U;
    const bool lifecycle_pending = pen.in_range_transition_pending || pen.tip_transition_pending ||
                                   pen.eraser_transition_pending || pen.barrel_transitions != 0U;
    return changes_lifecycle && lifecycle_pending;
}

void record_pen_transitions(aoa::detail::PenState* pen, const aoahid_pen_sample& next) noexcept {
    pen->in_range_transition_pending =
        pen->in_range_transition_pending || pen->desired.in_range != next.in_range;
    pen->tip_transition_pending = pen->tip_transition_pending || pen->desired.tip != next.tip;
    pen->eraser_transition_pending =
        pen->eraser_transition_pending || pen->desired.eraser != next.eraser;
    pen->barrel_transitions |= pen_barrel_changes(pen->desired, next);
}

void consume_lifecycle_transitions(aoahid_node* node) noexcept {
    if (auto* keyboard = state<aoa::detail::KeyboardState>(node); keyboard != nullptr) {
        std::fill(keyboard->key_transitions.begin(), keyboard->key_transitions.end(),
                  std::uint8_t{0});
        keyboard->modifier_transitions = 0U;
    } else if (auto* mouse = state<aoa::detail::MouseState>(node); mouse != nullptr) {
        std::fill(mouse->button_transitions.begin(), mouse->button_transitions.end(),
                  std::uint8_t{0});
    } else if (auto* consumer = state<aoa::detail::ConsumerState>(node); consumer != nullptr) {
        std::fill(consumer->transitions.begin(), consumer->transitions.end(), std::uint8_t{0});
    } else if (auto* gamepad = state<aoa::detail::GamepadState>(node); gamepad != nullptr) {
        std::fill(gamepad->button_transitions.begin(), gamepad->button_transitions.end(),
                  std::uint8_t{0});
        gamepad->hat_transition_pending = false;
    } else if (auto* touch = state<aoa::detail::TouchState>(node); touch != nullptr) {
        for (auto& contact : touch->contacts) {
            contact.lifecycle_transition_pending = false;
        }
        std::fill(touch->button_transitions.begin(), touch->button_transitions.end(),
                  std::uint8_t{0});
    } else if (auto* pen = state<aoa::detail::PenState>(node); pen != nullptr) {
        pen->barrel_transitions = 0U;
        pen->in_range_transition_pending = false;
        pen->tip_transition_pending = false;
        pen->eraser_transition_pending = false;
    }
}

std::size_t collect_touch(const aoa::detail::TouchState& touch,
                          std::array<const aoa::detail::ContactState*, 16>* active) noexcept {
    std::size_t count = 0U;
    for (const auto& contact : touch.contacts) {
        if (contact.phase != ContactPhase::none) {
            (*active)[count++] = &contact;
        }
    }
    return count;
}

bool validate_contact(const aoa::detail::TouchConfig& config_value,
                      const aoa::detail::TouchContact& contact, const char* operation) noexcept {
    const auto& options = config_value.fields;
    if (contact.contact_id > static_cast<std::uint32_t>(options.contact_id.logical_maximum) ||
        static_cast<std::int64_t>(contact.contact_id) < options.contact_id.logical_minimum ||
        !in_range(contact.x, options.x) || !in_range(contact.y, options.y) ||
        (options.enable_pressure == 1U && !in_range(contact.pressure, options.pressure)) ||
        (options.enable_width == 1U && !in_range(contact.width, options.width)) ||
        (options.enable_height == 1U && !in_range(contact.height, options.height)) ||
        // Linux hid-multitouch.c treats Azimuth samples as [0, Logical
        // Maximum): the maximum is the full-turn endpoint, not a valid sample,
        // even though HID logical extents are otherwise inclusive.
        (options.enable_azimuth == 1U &&
         (contact.azimuth < 0 || contact.azimuth >= options.azimuth.logical_maximum)) ||
        (options.enable_pressure == 0U && contact.pressure != 0) ||
        (options.enable_width == 0U && contact.width != 0) ||
        (options.enable_height == 0U && contact.height != 0) ||
        (options.enable_azimuth == 0U && contact.azimuth != 0)) {
        set_error(AOAHID_ERR_PARAM, operation,
                  "A contact value is outside its declared logical range.");
        return false;
    }
    return true;
}

template <typename Function>
aoahid_result abi_state_result([[maybe_unused]] aoahid_node* node, const char* field,
                               Function&& function) noexcept {
    try {
        AOAHID_SERIALIZED(aoa::detail::serialization_context(node), field);
        return function();
    } catch (const std::bad_alloc&) {
        set_error(AOAHID_ERR_INTERNAL, field,
                  "Memory allocation failed inside the C ABI boundary.");
        return AOAHID_ERR_INTERNAL;
    } catch (...) {
        set_error(AOAHID_ERR_INTERNAL, field,
                  "A C++ exception was contained at the C ABI boundary.");
        return AOAHID_ERR_INTERNAL;
    }
}
} // namespace

namespace aoa::detail {

aoahid_result validate_serialized_report(const aoahid_spec* spec, const std::uint8_t* report,
                                         const std::size_t report_length) noexcept {
    if (spec == nullptr) {
        set_error(AOAHID_ERR_INTERNAL, "report.length",
                  "The generated report does not match its immutable wire length.");
        return AOAHID_ERR_INTERNAL;
    }
    return validate_serialized_report(spec, spec->layout, report, report_length);
}

aoahid_result validate_serialized_report(const aoahid_spec* spec,
                                         const aoa::hid::ReportLayout& layout,
                                         const std::uint8_t* report,
                                         const std::size_t report_length) noexcept {
    if (spec == nullptr || report == nullptr || report_length != layout.wire_bytes ||
        report_length == 0U) {
        set_error(AOAHID_ERR_INTERNAL, "report.length",
                  "The generated report does not match its immutable wire length.");
        return AOAHID_ERR_INTERNAL;
    }
    if (layout.has_report_id && report[0] != layout.report_id) {
        set_error(AOAHID_ERR_INTERNAL, "report.report_id",
                  "The generated report prefix does not match its immutable Report ID.", 0, 57, 0,
                  layout.report_id, 0, static_cast<std::uint32_t>(report_length));
        return AOAHID_ERR_INTERNAL;
    }
    for (const FieldLayout& field : layout.fields) {
        if (field.bit_width == 0U || field.bit_width > 32U ||
            field.bit_offset > report_length * 8U ||
            field.bit_width > report_length * 8U - field.bit_offset) {
            set_error(AOAHID_ERR_INTERNAL, "report.layout",
                      "An immutable field lies outside the generated report.");
            return AOAHID_ERR_INTERNAL;
        }
        std::uint64_t encoded = 0U;
        std::size_t position = field.bit_offset;
        for (std::uint32_t done = 0U; done < field.bit_width;) {
            const auto shift = static_cast<std::uint32_t>(position & 7U);
            const std::uint32_t taken = std::min(field.bit_width - done, 8U - shift);
            encoded |= static_cast<std::uint64_t>(
                           (static_cast<std::uint32_t>(report[position / 8U]) >> shift) &
                           ((1U << taken) - 1U))
                       << done;
            position += taken;
            done += taken;
        }
        std::int64_t value = static_cast<std::int64_t>(encoded);
        if (field.is_signed && (encoded & (std::uint64_t{1} << (field.bit_width - 1U))) != 0U) {
            value = static_cast<std::int64_t>(encoded | (~std::uint64_t{0} << field.bit_width));
        }
        const bool null_semantic = field.has_null_state && value == hat_null(field);
        const bool in_declared_range =
            value >= field.logical_minimum && value <= field.logical_maximum;
        if ((!in_declared_range && !null_semantic) ||
            (field.semantic == FieldSemantic::constant_padding && value != 0)) {
            set_error(
                AOAHID_ERR_INTERNAL, "report.field",
                "A generated field is outside its immutable logical range or padding is nonzero.");
            return AOAHID_ERR_INTERNAL;
        }
    }
    return AOAHID_OK;
}

aoahid_result serialize_battery(const aoahid_node* node, std::uint8_t* report,
                                const std::size_t capacity, std::size_t* report_length) noexcept {
    if (node == nullptr || node->spec == nullptr || !node->spec->has_battery || report == nullptr ||
        report_length == nullptr || capacity < node->spec->battery_layout.wire_bytes) {
        set_error(AOAHID_ERR_PARAM, "report", "The serializer buffer is missing or too small.");
        return AOAHID_ERR_PARAM;
    }
    const auto& layout = node->spec->battery_layout;
    std::fill_n(report, layout.wire_bytes, std::uint8_t{0});
    report[0] = layout.report_id;
    if (!store_bits(report, capacity, layout.fields.front(), node->battery.strength, false)) {
        set_error(AOAHID_ERR_INTERNAL, "report.field",
                  "The battery level does not fit its immutable field.");
        return AOAHID_ERR_INTERNAL;
    }
    *report_length = layout.wire_bytes;
    return AOAHID_OK;
}

aoahid_result serialize_node(aoahid_node* node, std::uint8_t* report, const std::size_t capacity,
                             std::size_t* report_length) noexcept {
    if (node == nullptr || node->spec == nullptr || report == nullptr || report_length == nullptr ||
        capacity < node->spec->layout.wire_bytes) {
        set_error(AOAHID_ERR_PARAM, "report", "The serializer buffer is missing or too small.");
        return AOAHID_ERR_PARAM;
    }
    const auto& layout = node->spec->layout;
    std::fill_n(report, layout.wire_bytes, std::uint8_t{0});
    if (layout.has_report_id) {
        report[0] = layout.report_id;
    }

    const KeyboardState* keyboard = state<KeyboardState>(node);
    const MouseState* mouse = state<MouseState>(node);
    const ConsumerState* consumer = state<ConsumerState>(node);
    const GamepadState* gamepad = state<GamepadState>(node);
    const TouchState* touch = state<TouchState>(node);
    const PenState* pen = state<PenState>(node);
    const KeyboardConfig* keyboard_config = config<KeyboardConfig>(node);
    const GamepadConfig* gamepad_config = config<GamepadConfig>(node);
    const TouchConfig* touch_config = config<TouchConfig>(node);
    std::array<const ContactState*, 16> active{};
    const std::size_t active_count = touch == nullptr ? 0U : collect_touch(*touch, &active);
    const std::size_t touch_start = touch == nullptr ? 0U : touch->packet_cursor;
    bool ok = true;

    for (const FieldLayout& field : layout.fields) {
        std::int64_t value = 0;
        bool allow_null = false;
        switch (field.semantic) {
        case FieldSemantic::modifier:
            value = keyboard == nullptr ? 0 : (keyboard->modifiers >> field.instance) & 1U;
            break;
        case FieldSemantic::key_bitmap:
            if (keyboard != nullptr && keyboard_config != nullptr) {
                value = field.instance < keyboard->pressed_bitmap.size()
                            ? keyboard->pressed_bitmap[field.instance]
                            : 0;
            }
            break;
        case FieldSemantic::buttons:
            if (mouse != nullptr && field.instance < mouse->buttons.size()) {
                value = mouse->buttons[field.instance];
            } else if (gamepad != nullptr && field.instance < gamepad->buttons.size()) {
                value = gamepad->buttons[field.instance];
            } else if (gamepad != nullptr && gamepad_config != nullptr &&
                       gamepad_config->options.dpad_representation == AOAHID_DPAD_BUTTONS &&
                       field.instance >= gamepad_config->options.button_count &&
                       field.instance < gamepad_config->options.button_count + 4U) {
                value =
                    (gamepad->hat >> (field.instance - gamepad_config->options.button_count)) & 1;
            } else if (touch != nullptr && field.instance < touch->buttons.size()) {
                value = touch->buttons[field.instance];
            } else if (pen != nullptr) {
                value = pen->tool_switch_departure || pen->desired.in_range == 0U
                            ? 0
                            : (pen->desired.barrel_buttons >> field.instance) & 1U;
            }
            break;
        case FieldSemantic::x:
        case FieldSemantic::y:
        case FieldSemantic::pressure:
        case FieldSemantic::width:
        case FieldSemantic::height:
        case FieldSemantic::azimuth: {
            if (mouse != nullptr) {
                const std::int64_t pending =
                    field.semantic == FieldSemantic::x ? mouse->dx : mouse->dy;
                value = std::clamp(pending, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                if (field.semantic == FieldSemantic::x) {
                    node->submitted_dx = value;
                } else {
                    node->submitted_dy = value;
                }
            } else if (touch != nullptr && touch_config != nullptr) {
                const std::size_t contact_index = touch_start + field.instance;
                if (contact_index < active_count) {
                    const auto& sample = active[contact_index]->sample;
                    if (field.semantic == FieldSemantic::x)
                        value = sample.x;
                    if (field.semantic == FieldSemantic::y)
                        value = sample.y;
                    if (field.semantic == FieldSemantic::pressure) {
                        value = active[contact_index]->phase == ContactPhase::down
                                    ? std::max(sample.pressure, 1)
                                    : 0;
                    }
                    if (field.semantic == FieldSemantic::width)
                        value = sample.width;
                    if (field.semantic == FieldSemantic::height)
                        value = sample.height;
                    if (field.semantic == FieldSemantic::azimuth)
                        value = sample.azimuth;
                }
            } else if (pen != nullptr) {
                const aoahid_pen_sample& sample =
                    pen->tool_switch_departure ? pen->emitted : pen->desired;
                if (field.semantic == FieldSemantic::x)
                    value = sample.x;
                if (field.semantic == FieldSemantic::y)
                    value = sample.y;
                if (field.semantic == FieldSemantic::pressure) {
                    value = pen->tool_switch_departure || sample.tip == 0U
                                ? 0
                                : std::max(sample.pressure, 1);
                }
            }
            break;
        }
        case FieldSemantic::wheel:
            if (mouse != nullptr) {
                value = std::clamp(mouse->wheel, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                node->submitted_wheel = value;
            }
            break;
        case FieldSemantic::pan:
            if (mouse != nullptr) {
                value = std::clamp(mouse->pan, static_cast<std::int64_t>(field.logical_minimum),
                                   static_cast<std::int64_t>(field.logical_maximum));
                node->submitted_pan = value;
            }
            break;
        case FieldSemantic::consumer_usage:
            value = consumer != nullptr && field.instance < consumer->pressed.size()
                        ? consumer->pressed[field.instance]
                        : 0;
            break;
        case FieldSemantic::gamepad_axis:
            value = gamepad != nullptr && field.instance < gamepad->axes.size()
                        ? gamepad->axes[field.instance]
                        : 0;
            break;
        case FieldSemantic::hat:
            if (gamepad != nullptr && gamepad->hat_has_direction) {
                value = gamepad->hat;
            } else {
                value = hat_null(field);
                allow_null = true;
            }
            break;
        case FieldSemantic::tip:
            if (touch != nullptr) {
                const std::size_t contact_index = touch_start + field.instance;
                value = contact_index < active_count &&
                                active[contact_index]->phase == ContactPhase::down
                            ? 1
                            : 0;
            } else if (pen != nullptr) {
                value = pen->tool_switch_departure ? 0 : pen->desired.tip;
            }
            break;
        case FieldSemantic::in_range:
            value = pen == nullptr || pen->tool_switch_departure ? 0 : pen->desired.in_range;
            break;
        case FieldSemantic::eraser:
            value = pen == nullptr || pen->tool_switch_departure || pen->desired.in_range == 0U
                        ? 0
                        : pen->desired.eraser;
            break;
        case FieldSemantic::contact_id: {
            const std::size_t contact_index = touch_start + field.instance;
            value = touch != nullptr && contact_index < active_count
                        ? active[contact_index]->sample.contact_id
                        : 0;
            break;
        }
        case FieldSemantic::scan_time:
            value = touch == nullptr ? 0 : touch->scan_time;
            break;
        case FieldSemantic::contact_count:
            value =
                touch == nullptr || touch_start != 0U ? 0 : static_cast<std::int64_t>(active_count);
            break;
        case FieldSemantic::tilt_x:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.tilt_x : pen->desired.tilt_x);
            break;
        case FieldSemantic::tilt_y:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.tilt_y : pen->desired.tilt_y);
            break;
        case FieldSemantic::twist:
            value = pen == nullptr
                        ? 0
                        : (pen->tool_switch_departure ? pen->emitted.twist : pen->desired.twist);
            break;
        case FieldSemantic::battery_strength:
            ok = false;
            break;
        case FieldSemantic::constant_padding:
            continue;
        }
        ok = ok && store_bits(report, capacity, field, value, allow_null);
    }
    if (!ok) {
        set_error(AOAHID_ERR_INTERNAL, "report.layout",
                  "A state value could not be serialized into its immutable layout.");
        return AOAHID_ERR_INTERNAL;
    }
    if (touch != nullptr && touch_config != nullptr) {
        node->submitted_touch_count =
            std::min<std::size_t>(touch_config->fields.contacts_per_report,
                                  active_count > touch_start ? active_count - touch_start : 0U);
        node->submitted_touch_final_packet =
            touch_start + node->submitted_touch_count >= active_count;
    }
    *report_length = layout.wire_bytes;
    return AOAHID_OK;
}

void transfer_complete(void* user, const aoahid_result result,
                       const std::int32_t native_status) noexcept {
    auto* node = static_cast<aoahid_node*>(user);
    if (node == nullptr) {
        return;
    }
    // libusb_submit_transfer() returns a negative error when it did not accept
    // the transfer. In that case no completion will follow, and the synthetic
    // completion used to recover the pool must not consume caller state.
    const bool accepted_by_libusb = native_status >= 0;
    // A STALL is the target refusing request 57 (f_accessory stalls when the
    // HID ID is not registered yet), so nothing was applied. Like a submit
    // rejection it keeps the state pending for the caller to send again; the
    // submit call that reports the STALL sends nothing itself. Timeouts and
    // cancellations stay consumed because delivery is unknown.
    const bool may_have_applied = accepted_by_libusb && result != AOAHID_ERR_STALL;
    // The battery report never touches input state. Like input, a refused
    // transfer stays pending and an uncertain one is not sent again.
    const bool battery_report = node->submitted_battery;
    node->submitted_battery = false;
    bool lifecycle_report_complete = true;
    if (battery_report) {
        node->battery_dirty = !may_have_applied;
    } else if (!may_have_applied) {
        node->dirty = true;
    } else if (auto* mouse = state<MouseState>(node); mouse != nullptr) {
        // A timed-out/cancelled control transfer may have reached the device.
        // Consuming the submitted relative component prevents an unsafe retry
        // from applying the same movement twice.
        mouse->dx -= node->submitted_dx;
        mouse->dy -= node->submitted_dy;
        mouse->wheel -= node->submitted_wheel;
        mouse->pan -= node->submitted_pan;
        node->dirty = result == AOAHID_OK &&
                      (mouse->dx != 0 || mouse->dy != 0 || mouse->wheel != 0 || mouse->pan != 0);
    } else if (result == AOAHID_OK) {
        if (auto* touch = state<TouchState>(node); touch != nullptr) {
            if (node->submitted_touch_final_packet) {
                for (auto& contact : touch->contacts) {
                    if (contact.phase == ContactPhase::up) {
                        contact.phase = ContactPhase::none;
                    }
                }
                touch->packet_cursor = 0U;
                node->dirty = false;
                const bool still_active =
                    std::any_of(touch->contacts.begin(), touch->contacts.end(),
                                [](const ContactState& contact) {
                                    return contact.phase == ContactPhase::down;
                                }) ||
                    std::any_of(touch->buttons.begin(), touch->buttons.end(),
                                [](const std::uint8_t value) { return value != 0U; });
                if (!still_active) {
                    // HUT 1.7 section 16.5 defines the base as the first frame
                    // after inactivity, so the next activity starts at zero.
                    touch->scan_epoch_active = false;
                }
            } else {
                touch->packet_cursor += node->submitted_touch_count;
                node->dirty = true;
                lifecycle_report_complete = false;
            }
        } else if (auto* pen = state<PenState>(node); pen != nullptr) {
            if (pen->tool_switch_departure) {
                pen->emitted.in_range = 0U;
                pen->emitted.tip = 0U;
                pen->tool_switch_departure = false;
                node->dirty = true;
                lifecycle_report_complete = false;
            } else {
                pen->emitted = pen->desired;
                node->dirty = false;
            }
        } else {
            node->dirty = false;
        }
    } else {
        // libusb cannot establish whether a timed-out or cancelled control
        // transfer reached the device. Retaining desired state is useful for
        // the next explicit mutation, but an automatic retry could duplicate
        // a key, selector, contact frame, or raw event.
        if (auto* touch = state<TouchState>(node); touch != nullptr) {
            touch->packet_cursor = 0U;
        }
        node->dirty = false;
    }
    if (!battery_report && may_have_applied && (result != AOAHID_OK || lifecycle_report_complete)) {
        // An accepted terminal transfer may have reached the target even when
        // completion is uncertain. Consume its edge baseline so a later
        // opposite transition can release any state the target may hold.
        consume_lifecycle_transitions(node);
    }
    if (!battery_report && may_have_applied) {
        if (result == AOAHID_OK) {
            node->emitted_non_neutral = node->submitted_non_neutral;
        } else {
            // A terminal callback cannot establish whether the target applied
            // an OUT transfer. Preserve any possible held state so close emits
            // a conservative neutral report if the link still exists.
            node->emitted_non_neutral = node->emitted_non_neutral || node->submitted_non_neutral;
        }
    }
    node->submitted_dx = 0;
    node->submitted_dy = 0;
    node->submitted_wheel = 0;
    node->submitted_pan = 0;
    node->submitted_non_neutral = false;
    // A callback may run on the Context's internal event thread. Publish every
    // diagnostic component with the completion result so the application
    // caller can reconstruct its own thread-local error record later.
    node->completion_native_status.store(native_status, std::memory_order_relaxed);
    node->completion_report_length.store(static_cast<std::uint32_t>(node->submitted_report_length),
                                         std::memory_order_relaxed);
    node->completion_report_id.store(node->submitted_report_id, std::memory_order_relaxed);
    node->submitted_report_length = 0U;
    node->submitted_report_id = 0U;
    node->completion_result.store(result, std::memory_order_release);
    // The transport freed this report's pool slot before calling back.
    if (aoahid_device* device = node->device; device != nullptr) {
        device->slot_generation.fetch_add(1U);
        if (device->slot_waiters.load() != 0U) {
            // A waiter checks the generation under this mutex; taking it here
            // means the notify cannot land between its check and its wait.
            const std::lock_guard<std::mutex> lock(device->slot_mutex);
            device->slot_cv.notify_all();
        }
    }
    // Internal-thread mode publishes while holding the same mutex every
    // Node-destruction path uses as a callback-exit barrier. Caller-poll mode
    // stores a null active mutex: its callback runs synchronously inside the
    // caller's synchronization domain and acquires no lock. A closer may
    // observe transfer_inflight=false immediately in internal-thread mode, but
    // it cannot destroy the Node until this guard has released. Do not access
    // node after this block.
    {
        const ModeMutexGuard guard(node->active_completion_mutex);
        node->transfer_inflight.store(false, std::memory_order_release);
        if (node->completion_waiters.load(std::memory_order_acquire) != 0U) {
            node->completion_cv.notify_all();
        }
    }
}

} // namespace aoa::detail

extern "C" {

// The usage/down order is the fixed, already-shipped aoahid_keyboard_key(node, usage,
// down) ABI order and cannot be changed to avoid this warning.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static aoahid_result keyboard_key_impl(aoahid_node* node, const std::uint16_t usage,
                                       const std::uint32_t down) {
    aoa::detail::clear_error();
    if (!aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "keyboard.key", "down must be exactly zero or one.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result check = usable(node, AOAHID_PROFILE_KEYBOARD, "keyboard.key");
    if (check != AOAHID_OK)
        return check;
    const bool press = down == 1U;
    auto* keyboard = state<aoa::detail::KeyboardState>(node);
    const auto* spec = config<aoa::detail::KeyboardConfig>(node);
    bool changed = false;
    if (usage >= aoa::hid::usage::keyboard_left_control &&
        usage <= aoa::hid::usage::keyboard_right_gui) {
        const auto mask =
            static_cast<std::uint8_t>(1U << (usage - aoa::hid::usage::keyboard_left_control));
        changed = ((keyboard->modifiers & mask) != 0U) != press;
        if (changed && (keyboard->modifier_transitions & mask) != 0U) {
            return pending_transition("keyboard.key");
        }
        if (changed) {
            keyboard->modifiers = press ? static_cast<std::uint8_t>(keyboard->modifiers | mask)
                                        : static_cast<std::uint8_t>(keyboard->modifiers & ~mask);
            keyboard->modifier_transitions =
                static_cast<std::uint8_t>(keyboard->modifier_transitions | mask);
        }
    } else {
        if (usage < spec->options.usage_minimum || usage > spec->options.usage_maximum) {
            set_error(AOAHID_ERR_PARAM, "keyboard.usage",
                      "The Usage is outside the caller-declared key range.");
            return AOAHID_ERR_PARAM;
        }
        const std::size_t index = usage - spec->options.usage_minimum;
        if (index >= keyboard->key_transitions.size()) {
            set_error(AOAHID_ERR_INTERNAL, "keyboard.key",
                      "The preallocated keyboard state does not match the declared Usage range.");
            return AOAHID_ERR_INTERNAL;
        }
        if (index >= keyboard->pressed_bitmap.size()) {
            set_error(AOAHID_ERR_INTERNAL, "keyboard.key",
                      "The preallocated bitmap does not match the declared Usage range.");
            return AOAHID_ERR_INTERNAL;
        }
        changed = (keyboard->pressed_bitmap[index] != 0U) != press;
        if (changed && keyboard->key_transitions[index] != 0U) {
            return pending_transition("keyboard.key");
        }
        if (changed) {
            if (press) {
                if (keyboard->pressed_bitmap_count >= keyboard->pressed_bitmap.size()) {
                    set_error(AOAHID_ERR_INTERNAL, "keyboard.key",
                              "The bitmap key count is inconsistent with its storage.");
                    return AOAHID_ERR_INTERNAL;
                }
                keyboard->pressed_bitmap[index] = 1U;
                ++keyboard->pressed_bitmap_count;
            } else {
                if (keyboard->pressed_bitmap_count == 0U) {
                    set_error(AOAHID_ERR_INTERNAL, "keyboard.key",
                              "The bitmap key count is inconsistent with its storage.");
                    return AOAHID_ERR_INTERNAL;
                }
                keyboard->pressed_bitmap[index] = 0U;
                --keyboard->pressed_bitmap_count;
            }
            keyboard->key_transitions[index] = 1U;
        }
    }
    node->dirty = node->dirty || changed;
    aoa::detail::clear_error();
    return AOAHID_OK;
}

static aoahid_result aoahid_mouse_move_impl(aoahid_node* node, const std::int32_t dx,
                                            const std::int32_t dy) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_MOUSE, "mouse.move");
    if (check != AOAHID_OK)
        return check;
    auto* mouse = state<aoa::detail::MouseState>(node);
    std::int64_t next_dx = mouse->dx;
    std::int64_t next_dy = mouse->dy;
    if (!add_checked(&next_dx, dx) || !add_checked(&next_dy, dy)) {
        set_error(AOAHID_ERR_OVERFLOW, "mouse.delta",
                  "The pending relative movement overflowed int64_t.");
        return AOAHID_ERR_OVERFLOW;
    }
    mouse->dx = next_dx;
    mouse->dy = next_dy;
    node->dirty = node->dirty || dx != 0 || dy != 0;
    return AOAHID_OK;
}

static aoahid_result aoahid_mouse_scroll_impl(aoahid_node* node, const std::int32_t wheel,
                                              const std::int32_t pan) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_MOUSE, "mouse.scroll");
    if (check != AOAHID_OK)
        return check;
    auto* mouse = state<aoa::detail::MouseState>(node);
    const auto* spec = config<aoa::detail::MouseConfig>(node);
    if ((wheel != 0 && spec->options.enable_wheel != 1U) ||
        (pan != 0 && spec->options.enable_pan != 1U)) {
        set_error(AOAHID_ERR_UNSUPPORTED, "mouse.scroll",
                  "The requested scroll axis is absent from the spec.");
        return AOAHID_ERR_UNSUPPORTED;
    }
    std::int64_t next_wheel = mouse->wheel;
    std::int64_t next_pan = mouse->pan;
    if (!add_checked(&next_wheel, wheel) || !add_checked(&next_pan, pan)) {
        set_error(AOAHID_ERR_OVERFLOW, "mouse.scroll",
                  "The pending relative scroll overflowed int64_t.");
        return AOAHID_ERR_OVERFLOW;
    }
    mouse->wheel = next_wheel;
    mouse->pan = next_pan;
    node->dirty = node->dirty || wheel != 0 || pan != 0;
    return AOAHID_OK;
}

static aoahid_result aoahid_mouse_button_impl(aoahid_node* node, const std::uint32_t button,
                                              const std::uint32_t down) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_MOUSE, "mouse.button");
    if (check != AOAHID_OK)
        return check;
    auto* mouse = state<aoa::detail::MouseState>(node);
    if (button == 0U || button > mouse->buttons.size() || !aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "mouse.button",
                  "Button is one-based and down must be zero or one.");
        return AOAHID_ERR_PARAM;
    }
    if (!transition_storage_matches(mouse->buttons, mouse->button_transitions, "mouse.button")) {
        return AOAHID_ERR_INTERNAL;
    }
    const std::size_t index = button - 1U;
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = mouse->buttons[index] != value;
    if (changed && mouse->button_transitions[index] != 0U) {
        return pending_transition("mouse.button");
    }
    if (changed) {
        mouse->buttons[index] = value;
        mouse->button_transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

// The usage/down order is the fixed, already-shipped aoahid_toggle_usage(node,
// usage, down) ABI order and cannot be changed to avoid this warning.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static aoahid_result aoahid_toggle_impl(aoahid_node* node, const std::uint16_t usage,
                                        const std::uint32_t down) {
    aoa::detail::clear_error();
    if (!aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "toggle", "down must be exactly zero or one.");
        return AOAHID_ERR_PARAM;
    }
    if (node == nullptr || node->spec == nullptr || node->closed ||
        node->spec->kind != AOAHID_PROFILE_TOGGLE) {
        set_error(AOAHID_ERR_PARAM, "toggle", "The operation requires a live Toggle node.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result link = link_available(node, "toggle");
    if (link != AOAHID_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_BUSY, "toggle", "The node has one report in flight.");
        return AOAHID_ERR_BUSY;
    }
    auto* consumer = state<aoa::detail::ConsumerState>(node);
    const auto* spec = config<aoa::detail::ConsumerConfig>(node);
    const auto found = std::find(spec->allowed_usages.begin(), spec->allowed_usages.end(), usage);
    if (found == spec->allowed_usages.end()) {
        set_error(AOAHID_ERR_PARAM, "toggle.usage", "The Usage is not in the caller allow-list.");
        return AOAHID_ERR_PARAM;
    }
    if (!transition_storage_matches(consumer->pressed, consumer->transitions, "toggle")) {
        return AOAHID_ERR_INTERNAL;
    }
    const auto index = static_cast<std::size_t>(found - spec->allowed_usages.begin());
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = consumer->pressed[index] != value;
    if (changed && consumer->transitions[index] != 0U) {
        return pending_transition("toggle");
    }
    if (changed) {
        consumer->pressed[index] = value;
        consumer->transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

static aoahid_result aoahid_gamepad_button_impl(aoahid_node* node, const std::uint32_t button,
                                                const std::uint32_t down) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_GAMEPAD, "gamepad.button");
    if (check != AOAHID_OK)
        return check;
    auto* gamepad = state<aoa::detail::GamepadState>(node);
    if (button == 0U || button > gamepad->buttons.size() || !aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "gamepad.button",
                  "Button is one-based and down must be zero or one.");
        return AOAHID_ERR_PARAM;
    }
    if (!transition_storage_matches(gamepad->buttons, gamepad->button_transitions,
                                    "gamepad.button")) {
        return AOAHID_ERR_INTERNAL;
    }
    const std::size_t index = button - 1U;
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = gamepad->buttons[index] != value;
    if (changed && gamepad->button_transitions[index] != 0U) {
        return pending_transition("gamepad.button");
    }
    if (changed) {
        gamepad->buttons[index] = value;
        gamepad->button_transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

static aoahid_result aoahid_gamepad_axis_impl(aoahid_node* node, const std::size_t axis_index,
                                              const std::int32_t value) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_GAMEPAD, "gamepad.axis");
    if (check != AOAHID_OK)
        return check;
    auto* gamepad = state<aoa::detail::GamepadState>(node);
    const auto* spec = config<aoa::detail::GamepadConfig>(node);
    if (axis_index >= gamepad->axes.size() || !in_range(value, spec->axes[axis_index].axis.value)) {
        set_error(AOAHID_ERR_PARAM, "gamepad.axis", "The axis index or value is outside the spec.");
        return AOAHID_ERR_PARAM;
    }
    const bool changed = gamepad->axes[axis_index] != value;
    gamepad->axes[axis_index] = value;
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

static aoahid_result gamepad_dpad_impl(aoahid_node* node, const std::uint32_t up,
                                       const std::uint32_t down, const std::uint32_t right,
                                       const std::uint32_t left) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_GAMEPAD, "gamepad.dpad");
    if (check != AOAHID_OK)
        return check;
    const auto* spec = config<aoa::detail::GamepadConfig>(node);
    if ((spec->options.dpad_representation != AOAHID_DPAD_BUTTONS &&
         spec->options.dpad_representation != AOAHID_DPAD_HAT) ||
        !aoa::detail::valid_boolean(up) || !aoa::detail::valid_boolean(down) ||
        !aoa::detail::valid_boolean(right) || !aoa::detail::valid_boolean(left)) {
        set_error(AOAHID_ERR_PARAM, "gamepad.dpad",
                  "The spec must select a D-pad representation and every direction must be zero "
                  "or one.");
        return AOAHID_ERR_PARAM;
    }
    const bool any_direction = (up | down | right | left) != 0U;
    std::int32_t encoded =
        static_cast<std::int32_t>(up | (down << 1U) | (right << 2U) | (left << 3U));
    if (spec->options.dpad_representation == AOAHID_DPAD_HAT &&
        ((up == 1U && down == 1U) || (right == 1U && left == 1U))) {
        set_error(AOAHID_ERR_PARAM, "gamepad.dpad",
                  "A Hat Switch cannot encode opposite directions simultaneously.");
        return AOAHID_ERR_PARAM;
    }
    if (spec->options.dpad_representation == AOAHID_DPAD_HAT && encoded != 0) {
        if (up == 1U) {
            encoded = right == 1U ? 1 : (left == 1U ? 7 : 0);
        } else if (down == 1U) {
            encoded = right == 1U ? 3 : (left == 1U ? 5 : 4);
        } else {
            encoded = right == 1U ? 2 : 6;
        }
    }
    auto* gamepad = state<aoa::detail::GamepadState>(node);
    const bool next_has_direction = any_direction;
    const bool changed =
        gamepad->hat_has_direction != next_has_direction || gamepad->hat != encoded;
    if (changed && gamepad->hat_transition_pending) {
        return pending_transition("gamepad.dpad");
    }
    if (changed) {
        gamepad->hat_has_direction = next_has_direction;
        gamepad->hat = encoded;
        gamepad->hat_transition_pending = true;
    }
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

// The optional fields of aoahid_touchscreen_extra and aoahid_touchpad_extra, which have the
// same layout, so the two public entry points share one implementation.
struct TouchExtra {
    std::int32_t pressure;
    std::int32_t width;
    std::int32_t height;
    std::int32_t azimuth;
};

// The contact_id/down/x/y order is the fixed aoahid_touchscreen_contact and
// aoahid_touchpad_contact ABI order and cannot be changed to avoid this warning.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
static aoahid_result touch_contact_impl(aoahid_node* node, const aoahid_profile_kind kind,
                                        const std::uint32_t contact_id, const std::uint32_t down,
                                        const std::int32_t x, const std::int32_t y,
                                        const TouchExtra* extra) {
    aoa::detail::clear_error();
    const bool right_kind = node != nullptr && node->spec != nullptr && node->spec->kind == kind;
    if (!right_kind || node->closed || !aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "touch", "A live touch node and boolean down are required.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result link = link_available(node, "touch");
    if (link != AOAHID_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_BUSY, "touch", "A touch report is in flight.");
        return AOAHID_ERR_BUSY;
    }
    const auto* spec = config<aoa::detail::TouchConfig>(node);
    auto* touch = state<aoa::detail::TouchState>(node);
    if (touch->packet_cursor != 0U) {
        set_error(AOAHID_ERR_BUSY, "touch",
                  "The current multi-packet frame must finish before state changes.");
        return AOAHID_ERR_BUSY;
    }
    auto active =
        std::find_if(touch->contacts.begin(), touch->contacts.end(), [=](const auto& entry) {
            return entry.phase == ContactPhase::down && entry.sample.contact_id == contact_id;
        });
    if (down == 0U) {
        if (active == touch->contacts.end()) {
            set_error(AOAHID_ERR_PARAM,
                      kind == AOAHID_PROFILE_TOUCHSCREEN ? "touchscreen.contact_id"
                                                         : "touchpad.contact_id",
                      "Lift requires an active Contact ID.");
            return AOAHID_ERR_PARAM;
        }
        if (!in_range(x, spec->fields.x) || !in_range(y, spec->fields.y)) {
            set_error(AOAHID_ERR_PARAM, "touch", "The final coordinates are outside the spec.");
            return AOAHID_ERR_PARAM;
        }
        if (active->lifecycle_transition_pending) {
            return pending_transition("touch");
        }
        active->phase = ContactPhase::up;
        active->sample.x = x;
        active->sample.y = y;
        active->sample.pressure = 0;
        active->lifecycle_transition_pending = true;
        touch->packet_cursor = 0U;
        node->dirty = true;
        return AOAHID_OK;
    }
    aoa::detail::TouchContact contact{};
    contact.contact_id = contact_id;
    contact.x = x;
    contact.y = y;
    if (extra != nullptr) {
        contact.pressure = extra->pressure;
        contact.width = extra->width;
        contact.height = extra->height;
        contact.azimuth = extra->azimuth;
    }
    if (!validate_contact(*spec, contact, "touch"))
        return AOAHID_ERR_PARAM;
    if (active != touch->contacts.end()) {
        // The Contact ID is already down, so this call moves it.
        const bool changed =
            active->sample.x != contact.x || active->sample.y != contact.y ||
            active->sample.pressure != contact.pressure || active->sample.width != contact.width ||
            active->sample.height != contact.height || active->sample.azimuth != contact.azimuth;
        active->sample = contact;
        touch->packet_cursor = 0U;
        node->dirty = node->dirty || changed;
        return AOAHID_OK;
    }
    for (const auto& entry : touch->contacts) {
        if (entry.phase != ContactPhase::none && entry.sample.contact_id == contact_id) {
            set_error(AOAHID_ERR_BUSY,
                      kind == AOAHID_PROFILE_TOUCHSCREEN ? "touchscreen.contact_id"
                                                         : "touchpad.contact_id",
                      "The Contact ID is already active.");
            return AOAHID_ERR_BUSY;
        }
    }
    auto found = std::find_if(touch->contacts.begin(), touch->contacts.end(),
                              [](const auto& entry) { return entry.phase == ContactPhase::none; });
    if (found == touch->contacts.end() ||
        std::count_if(touch->contacts.begin(), touch->contacts.end(), [](const auto& entry) {
            return entry.phase != ContactPhase::none;
        }) >= static_cast<std::ptrdiff_t>(spec->fields.maximum_contacts)) {
        set_error(AOAHID_ERR_OVERFLOW,
                  kind == AOAHID_PROFILE_TOUCHSCREEN ? "touchscreen.maximum_contacts"
                                                     : "touchpad.maximum_contacts",
                  "No declared contact slot remains.");
        return AOAHID_ERR_OVERFLOW;
    }
    found->phase = ContactPhase::down;
    found->sample = contact;
    found->lifecycle_transition_pending = true;
    touch->packet_cursor = 0U;
    node->dirty = true;
    return AOAHID_OK;
}

static aoahid_result aoahid_touchpad_button_impl(aoahid_node* node, const std::uint32_t button,
                                                 const std::uint32_t down) {
    aoa::detail::clear_error();
    const aoahid_result check = usable(node, AOAHID_PROFILE_TOUCHPAD, "touchpad.button");
    if (check != AOAHID_OK)
        return check;
    auto* touch = state<aoa::detail::TouchState>(node);
    if (touch->packet_cursor != 0U) {
        set_error(AOAHID_ERR_BUSY, "touchpad.button",
                  "The current multi-packet frame must finish before state changes.");
        return AOAHID_ERR_BUSY;
    }
    if (button == 0U || button > touch->buttons.size() || !aoa::detail::valid_boolean(down)) {
        set_error(AOAHID_ERR_PARAM, "touchpad.button",
                  "Button is one-based and down must be zero or one.");
        return AOAHID_ERR_PARAM;
    }
    if (!transition_storage_matches(touch->buttons, touch->button_transitions, "touchpad.button")) {
        return AOAHID_ERR_INTERNAL;
    }
    const std::size_t index = button - 1U;
    const auto value = static_cast<std::uint8_t>(down);
    const bool changed = touch->buttons[index] != value;
    if (changed && touch->button_transitions[index] != 0U) {
        return pending_transition("touchpad.button");
    }
    if (changed) {
        touch->buttons[index] = value;
        touch->button_transitions[index] = 1U;
    }
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

static aoahid_result stylus_update_impl(aoahid_node* node, const aoahid_profile_kind kind,
                                        const aoahid_pen_sample* sample) {
    aoa::detail::clear_error();
    const aoahid_result check =
        usable(node, kind, kind == AOAHID_PROFILE_PEN ? "pen.update" : "digitizer.update");
    if (check != AOAHID_OK)
        return check;
    if (sample == nullptr) {
        set_error(AOAHID_ERR_PARAM, kind == AOAHID_PROFILE_PEN ? "pen.sample" : "digitizer.sample",
                  "The pen sample pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    const auto* spec = config<aoa::detail::PenConfig>(node);
    const auto& options = spec->options;
    const std::uint32_t barrel_mask =
        options.barrel_usage_count == 32U
            ? std::numeric_limits<std::uint32_t>::max()
            : (options.barrel_usage_count == 0U ? 0U : (1U << options.barrel_usage_count) - 1U);
    if (!aoa::detail::valid_boolean(sample->in_range) || !aoa::detail::valid_boolean(sample->tip) ||
        !aoa::detail::valid_boolean(sample->eraser) ||
        (!options.enable_hover && sample->in_range != sample->tip) ||
        (sample->tip == 1U && sample->in_range == 0U) ||
        (options.enable_eraser == 0U && sample->eraser != 0U) ||
        (sample->barrel_buttons & ~barrel_mask) != 0U || !in_range(sample->x, options.x) ||
        !in_range(sample->y, options.y) ||
        (options.enable_pressure == 0U && sample->pressure != 0) ||
        (options.enable_tilt == 0U && (sample->tilt_x != 0 || sample->tilt_y != 0)) ||
        (options.enable_twist_target_specific == 0U && sample->twist != 0) ||
        (options.enable_pressure == 1U && !in_range(sample->pressure, options.pressure)) ||
        (options.enable_tilt == 1U && (!in_range(sample->tilt_x, options.tilt_x) ||
                                       !in_range(sample->tilt_y, options.tilt_y))) ||
        (options.enable_twist_target_specific == 1U && !in_range(sample->twist, options.twist))) {
        set_error(AOAHID_ERR_PARAM, kind == AOAHID_PROFILE_PEN ? "pen.sample" : "digitizer.sample",
                  "A flag, button, or value is outside the immutable pen spec.");
        return AOAHID_ERR_PARAM;
    }
    auto* pen = state<aoa::detail::PenState>(node);
    const bool changed =
        pen->desired.in_range != sample->in_range || pen->desired.tip != sample->tip ||
        pen->desired.eraser != sample->eraser ||
        pen->desired.barrel_buttons != sample->barrel_buttons || pen->desired.x != sample->x ||
        pen->desired.y != sample->y || pen->desired.pressure != sample->pressure ||
        pen->desired.tilt_x != sample->tilt_x || pen->desired.tilt_y != sample->tilt_y ||
        pen->desired.twist != sample->twist;
    if (pen_transition_conflicts(*pen, *sample)) {
        return pending_transition(kind == AOAHID_PROFILE_PEN ? "pen.update" : "digitizer.update");
    }
    record_pen_transitions(pen, *sample);
    pen->desired = *sample;
    pen->tool_switch_departure =
        pen->emitted.in_range == 1U && pen->emitted.eraser != pen->desired.eraser;
    node->dirty = node->dirty || changed;
    return AOAHID_OK;
}

static aoahid_result aoahid_battery_update_impl(aoahid_node* node, const std::int32_t strength) {
    aoa::detail::clear_error();
    if (node == nullptr || node->closed || node->spec == nullptr || !node->spec->has_battery) {
        set_error(AOAHID_ERR_PARAM, "battery.update", "The node has no battery report.");
        return AOAHID_ERR_PARAM;
    }
    const aoahid_result link = link_available(node, "battery.update");
    if (link != AOAHID_OK)
        return link;
    if (node->transfer_inflight.load(std::memory_order_acquire)) {
        set_error(AOAHID_ERR_BUSY, "battery.update", "The node has one report in flight.");
        return AOAHID_ERR_BUSY;
    }
    if (!in_range(strength, node->spec->battery.strength)) {
        set_error(AOAHID_ERR_PARAM, "battery.strength", "The value is outside the declared range.");
        return AOAHID_ERR_PARAM;
    }
    // The first value is always a change, even when it equals the zero the
    // state starts with.
    const bool changed = !node->battery.has_value || node->battery.strength != strength;
    node->battery.has_value = true;
    node->battery.strength = strength;
    node->battery_dirty = node->battery_dirty || changed;
    return AOAHID_OK;
}

aoahid_result AOAHID_CALL aoahid_keyboard_key(aoahid_node* node, const std::uint16_t usage,
                                              const std::uint32_t down) {
    return abi_state_result(node, "keyboard.key",
                            [&] { return keyboard_key_impl(node, usage, down); });
}

aoahid_result AOAHID_CALL aoahid_mouse_move(aoahid_node* node, const std::int32_t dx,
                                            const std::int32_t dy) {
    return abi_state_result(node, "mouse.move",
                            [&] { return aoahid_mouse_move_impl(node, dx, dy); });
}

aoahid_result AOAHID_CALL aoahid_mouse_scroll(aoahid_node* node, const std::int32_t wheel,
                                              const std::int32_t pan) {
    return abi_state_result(node, "mouse.scroll",
                            [&] { return aoahid_mouse_scroll_impl(node, wheel, pan); });
}

aoahid_result AOAHID_CALL aoahid_mouse_button(aoahid_node* node, const std::uint32_t button,
                                              const std::uint32_t down) {
    return abi_state_result(node, "mouse.button",
                            [&] { return aoahid_mouse_button_impl(node, button, down); });
}

aoahid_result AOAHID_CALL aoahid_toggle_usage(aoahid_node* node, const std::uint16_t usage,
                                              const std::uint32_t down) {
    return abi_state_result(node, "toggle", [&] { return aoahid_toggle_impl(node, usage, down); });
}

aoahid_result AOAHID_CALL aoahid_gamepad_button(aoahid_node* node, const std::uint32_t button,
                                                const std::uint32_t down) {
    return abi_state_result(node, "gamepad.button",
                            [&] { return aoahid_gamepad_button_impl(node, button, down); });
}

aoahid_result AOAHID_CALL aoahid_gamepad_axis(aoahid_node* node, const std::size_t axis_index,
                                              const std::int32_t value) {
    return abi_state_result(node, "gamepad.axis",
                            [&] { return aoahid_gamepad_axis_impl(node, axis_index, value); });
}

aoahid_result AOAHID_CALL aoahid_gamepad_dpad(aoahid_node* node, const std::uint32_t up,
                                              const std::uint32_t down, const std::uint32_t right,
                                              const std::uint32_t left) {
    return abi_state_result(node, "gamepad.dpad",
                            [&] { return gamepad_dpad_impl(node, up, down, right, left); });
}

aoahid_result AOAHID_CALL aoahid_touchscreen_contact(aoahid_node* node,
                                                     const std::uint32_t contact_id,
                                                     const std::uint32_t down, const std::int32_t x,
                                                     const std::int32_t y,
                                                     const aoahid_touchscreen_extra* extra) {
    return abi_state_result(node, "touchscreen.contact", [&] {
        const TouchExtra values = extra == nullptr ? TouchExtra{}
                                                   : TouchExtra{extra->pressure, extra->width,
                                                                extra->height, extra->azimuth};
        return touch_contact_impl(node, AOAHID_PROFILE_TOUCHSCREEN, contact_id, down, x, y,
                                  extra == nullptr ? nullptr : &values);
    });
}

aoahid_result AOAHID_CALL aoahid_touchpad_contact(aoahid_node* node, const std::uint32_t contact_id,
                                                  const std::uint32_t down, const std::int32_t x,
                                                  const std::int32_t y,
                                                  const aoahid_touchpad_extra* extra) {
    return abi_state_result(node, "touchpad.contact", [&] {
        const TouchExtra values = extra == nullptr ? TouchExtra{}
                                                   : TouchExtra{extra->pressure, extra->width,
                                                                extra->height, extra->azimuth};
        return touch_contact_impl(node, AOAHID_PROFILE_TOUCHPAD, contact_id, down, x, y,
                                  extra == nullptr ? nullptr : &values);
    });
}

aoahid_result AOAHID_CALL aoahid_touchpad_button(aoahid_node* node, const std::uint32_t button,
                                                 const std::uint32_t down) {
    return abi_state_result(node, "touchpad.button",
                            [&] { return aoahid_touchpad_button_impl(node, button, down); });
}

aoahid_result AOAHID_CALL aoahid_pen_update(aoahid_node* node, const aoahid_pen_sample* sample) {
    return abi_state_result(node, "pen.update",
                            [&] { return stylus_update_impl(node, AOAHID_PROFILE_PEN, sample); });
}

aoahid_result AOAHID_CALL aoahid_digitizer_update(aoahid_node* node,
                                                  const aoahid_digitizer_sample* sample) {
    return abi_state_result(node, "digitizer.update", [&] {
        const aoahid_pen_sample values =
            sample == nullptr
                ? aoahid_pen_sample{}
                : aoahid_pen_sample{sample->in_range,       sample->tip,    sample->eraser,
                                    sample->barrel_buttons, sample->x,      sample->y,
                                    sample->pressure,       sample->tilt_x, sample->tilt_y,
                                    sample->twist};
        return stylus_update_impl(node, AOAHID_PROFILE_DIGITIZER,
                                  sample == nullptr ? nullptr : &values);
    });
}

aoahid_result AOAHID_CALL aoahid_battery_update(aoahid_node* node, const std::int32_t strength) {
    return abi_state_result(node, "battery.update",
                            [&] { return aoahid_battery_update_impl(node, strength); });
}

} // extern "C"
