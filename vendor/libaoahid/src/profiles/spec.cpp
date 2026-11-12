// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * This file validates caller choices and builds immutable descriptors, layouts,
 * and manifests. It performs no USB I/O and stores no device-specific state.
 */

#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace {
using aoa::detail::set_error;
using aoa::hid::DescriptorBuilder;
using aoa::hid::FieldSemantic;

constexpr std::size_t kAoaDescriptorWireMaximum = 65535U;

bool validate_report_id(const aoahid_report_id_option& option, const char* field) noexcept {
    if (const char* reason = aoa::hid::report_id_option_violation(option)) {
        set_error(AOAHID_ERR_PARAM, field, reason);
        return false;
    }
    return true;
}

bool physical_is_zero(const aoahid_physical_properties& physical) noexcept {
    return physical.enabled == 0U && physical.minimum == 0 && physical.maximum == 0 &&
           physical.unit_exponent == 0 && physical.unit == 0U;
}

bool physical_units_match(const aoahid_physical_properties& first,
                          const aoahid_physical_properties& second) noexcept {
    if (first.enabled != second.enabled) {
        return false;
    }
    return first.enabled == 0U ||
           (first.unit == second.unit && first.unit_exponent == second.unit_exponent);
}

bool validate_physical(const aoahid_physical_properties& physical, const char* name) noexcept {
    if (!aoa::detail::valid_boolean(physical.enabled)) {
        set_error(AOAHID_ERR_PARAM, name, "physical.enabled must be exactly zero or one.");
        return false;
    }
    if (physical.enabled == 0U) {
        if (!physical_is_zero(physical)) {
            set_error(AOAHID_ERR_PARAM, name,
                      "Disabled physical properties must be canonical zero.");
            return false;
        }
        return true;
    }
    if (physical.maximum <= physical.minimum || physical.unit_exponent < -8 ||
        physical.unit_exponent > 7 || physical.unit == 0U ||
        !aoa::hid::unit_is_valid(physical.unit)) {
        set_error(AOAHID_ERR_PARAM, name,
                  "Enabled physical properties require ordered extents, exponent -8 through 7, and "
                  "a valid nonzero HID Unit.");
        return false;
    }
    return true;
}

bool validate_field(const aoahid_integer_field& field, const char* name) noexcept {
    if (!validate_physical(field.physical, name)) {
        return false;
    }
    if (field.bit_width == 0U) {
        set_error(AOAHID_ERR_UNSET_FIELD, name, "bit_width is required and cannot be zero.");
        return false;
    }
    if (field.bit_width > 32U ||
        !aoa::hid::value_fits(field.logical_minimum, field.logical_maximum,
                              static_cast<std::uint8_t>(field.bit_width))) {
        set_error(AOAHID_ERR_PARAM, name,
                  "The logical range must be ordered and fit the declared width and signedness.");
        return false;
    }
    return true;
}

bool field_is_zero(const aoahid_integer_field& field) noexcept {
    return field.logical_minimum == 0 && field.logical_maximum == 0 && field.bit_width == 0U &&
           physical_is_zero(field.physical);
}

struct NullEncodingRange {
    std::int32_t minimum;
    std::int32_t maximum;
    std::uint32_t bit_width;
};

bool has_null_encoding(const NullEncodingRange range) noexcept {
    if (range.bit_width == 0U || range.bit_width > 32U) {
        return false;
    }
    std::int64_t representable_minimum = 0;
    std::int64_t representable_maximum = 0;
    if (range.minimum < 0 || range.maximum < 0) {
        representable_minimum = range.bit_width == 32U
                                    ? std::numeric_limits<std::int32_t>::min()
                                    : -(std::int64_t{1} << (range.bit_width - 1U));
        representable_maximum = range.bit_width == 32U
                                    ? std::numeric_limits<std::int32_t>::max()
                                    : (std::int64_t{1} << (range.bit_width - 1U)) - 1;
    } else {
        representable_maximum =
            range.bit_width == 32U
                ? static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())
                : (std::int64_t{1} << range.bit_width) - 1;
    }
    return static_cast<std::int64_t>(range.minimum) > representable_minimum ||
           static_cast<std::int64_t>(range.maximum) < representable_maximum;
}

bool validate_option_boolean(const std::uint32_t value, const char* field) noexcept {
    if (!aoa::detail::valid_boolean(value)) {
        set_error(AOAHID_ERR_PARAM, field, "The flag must be exactly zero or one.");
        return false;
    }
    return true;
}

void pad_report(DescriptorBuilder& builder) {
    const std::size_t padding = (8U - (builder.input_bits() & 7U)) & 7U;
    if (padding != 0U) {
        builder.constant_padding(static_cast<std::uint16_t>(padding));
    }
}

struct GeneratedSpecIdentity {
    aoahid_profile_kind kind;
    aoahid_android_status status;
};

struct BatteryRequest {
    std::uint32_t enabled;
    aoahid_battery_report report;
    aoahid_report_id_option input_report_id;
};

struct BatteryFieldNames {
    const char* enable;
    const char* battery;
    const char* report_id;
    const char* strength;
};

constexpr BatteryFieldNames kKeyboardBatteryNames{"keyboard.enable_battery", "keyboard.battery",
                                                  "keyboard.battery.report_id",
                                                  "keyboard.battery.strength"};
constexpr BatteryFieldNames kMouseBatteryNames{"mouse.enable_battery", "mouse.battery",
                                               "mouse.battery.report_id", "mouse.battery.strength"};
constexpr BatteryFieldNames kToggleBatteryNames{"toggle.enable_battery", "toggle.battery",
                                                "toggle.battery.report_id",
                                                "toggle.battery.strength"};
constexpr BatteryFieldNames kGamepadBatteryNames{"gamepad.enable_battery", "gamepad.battery",
                                                 "gamepad.battery.report_id",
                                                 "gamepad.battery.strength"};
constexpr BatteryFieldNames kTouchscreenBatteryNames{
    "touchscreen.enable_battery", "touchscreen.battery", "touchscreen.battery.report_id",
    "touchscreen.battery.strength"};
constexpr BatteryFieldNames kTouchpadBatteryNames{"touchpad.enable_battery", "touchpad.battery",
                                                  "touchpad.battery.report_id",
                                                  "touchpad.battery.strength"};
constexpr BatteryFieldNames kPenBatteryNames{"pen.enable_battery", "pen.battery",
                                             "pen.battery.report_id", "pen.battery.strength"};
constexpr BatteryFieldNames kDigitizerBatteryNames{"digitizer.enable_battery", "digitizer.battery",
                                                   "digitizer.battery.report_id",
                                                   "digitizer.battery.strength"};

bool validate_battery(const BatteryRequest& request, const BatteryFieldNames& names) noexcept {
    if (!validate_option_boolean(request.enabled, names.enable)) {
        return false;
    }
    const aoahid_battery_report& battery = request.report;
    if (request.enabled == 0U) {
        const bool zero =
            battery.report_id.enabled == 0U && battery.report_id.value == 0U &&
            battery.report_id.reserved8[0] == 0U && battery.report_id.reserved8[1] == 0U &&
            battery.report_id.reserved8[2] == 0U && battery.strength.logical_minimum == 0 &&
            battery.strength.logical_maximum == 0 && battery.strength.bit_width == 0U &&
            physical_is_zero(battery.strength.physical);
        if (!zero) {
            set_error(AOAHID_ERR_PARAM, names.battery,
                      "battery must be all zero while enable_battery is zero.");
        }
        return zero;
    }
    if (!validate_report_id(battery.report_id, names.report_id)) {
        return false;
    }
    if (request.input_report_id.enabled != 1U || battery.report_id.enabled != 1U ||
        battery.report_id.value == request.input_report_id.value) {
        set_error(AOAHID_ERR_PARAM, names.report_id,
                  "A battery report needs the profile's Report ID and its own, with different "
                  "values.");
        return false;
    }
    if (!validate_field(battery.strength, names.strength) || battery.strength.logical_minimum < 0 ||
        battery.strength.logical_maximum <= battery.strength.logical_minimum) {
        set_error(AOAHID_ERR_PARAM, names.strength,
                  "Battery Strength requires an ordered nonnegative range that fits its width.");
        return false;
    }
    return true;
}

template <typename Emit>
aoahid_result build_generated_spec(const GeneratedSpecIdentity identity,
                                   aoa::detail::SpecConfig config, const BatteryRequest& battery,
                                   Emit emit, aoahid_spec** out_spec) {
    if (out_spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_spec", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_spec = nullptr;
    auto spec = std::make_unique<aoahid_spec>();
    const aoa::hid::BatteryField battery_field{
        battery.report.report_id.value, battery.report.strength.logical_minimum,
        battery.report.strength.logical_maximum,
        static_cast<std::uint8_t>(battery.report.strength.bit_width),
        battery.report.strength.physical};
    const bool has_battery = battery.enabled == 1U;
    DescriptorBuilder counting_builder(nullptr, kAoaDescriptorWireMaximum, &spec->layout);
    if (has_battery) {
        counting_builder.set_battery(battery_field, &spec->battery_layout);
    }
    const bool counted = emit(counting_builder);
    counting_builder.finish();
    if (!counted || !counting_builder.good()) {
        set_error(AOAHID_ERR_OVERFLOW, "spec.descriptor",
                  "Descriptor generation failed or exceeded the AOA wire-length field.");
        return AOAHID_ERR_OVERFLOW;
    }
    spec->requirements = counting_builder.requirements();
    std::vector<std::uint8_t> construction(counting_builder.descriptor_size());
    DescriptorBuilder emitting_builder(construction.data(), construction.size(), nullptr);
    if (has_battery) {
        emitting_builder.set_battery(battery_field, nullptr);
    }
    const bool emitted = emit(emitting_builder);
    emitting_builder.finish();
    if (!emitted || !emitting_builder.good() ||
        emitting_builder.descriptor_size() != construction.size() ||
        emitting_builder.requirements().maximum_fields_per_report !=
            spec->requirements.maximum_fields_per_report ||
        emitting_builder.requirements().maximum_global_stack_depth !=
            spec->requirements.maximum_global_stack_depth ||
        emitting_builder.requirements().maximum_report_size_bits !=
            spec->requirements.maximum_report_size_bits ||
        emitting_builder.requirements().maximum_usages != spec->requirements.maximum_usages ||
        emitting_builder.requirements().maximum_report_data_bits !=
            spec->requirements.maximum_report_data_bits) {
        set_error(AOAHID_ERR_INTERNAL, "spec.descriptor",
                  "The descriptor counting and emission passes produced different results.");
        return AOAHID_ERR_INTERNAL;
    }
    const aoa::hid::ValidationIssue validation =
        aoa::hid::validate_generated(construction.data(), construction.size(), spec->layout,
                                     has_battery ? &spec->battery_layout : nullptr);
    if (validation.result != AOAHID_OK) {
        set_error(validation.result, validation.field, validation.reason, 0, 0, 0, 0,
                  static_cast<std::uint32_t>(validation.offset), 0);
        return validation.result;
    }
    spec->kind = identity.kind;
    spec->android_status = identity.status;
    spec->descriptor = std::move(construction);
    spec->config = std::move(config);
    spec->report_capabilities.push_back(aoahid_report_capability{
        {spec->layout.has_report_id ? 1U : 0U, spec->layout.report_id, {0U, 0U, 0U}},
        static_cast<std::uint32_t>(spec->layout.wire_bytes)});
    if (has_battery) {
        spec->has_battery = true;
        spec->battery = battery.report;
        spec->report_capabilities.push_back(
            aoahid_report_capability{{1U, spec->battery_layout.report_id, {0U, 0U, 0U}},
                                     static_cast<std::uint32_t>(spec->battery_layout.wire_bytes)});
    }
    *out_spec = spec.release();
    aoa::detail::clear_error();
    return AOAHID_OK;
}

struct UsageSemanticFlags {
    bool accepted;
    bool relative;
    bool no_preferred;
};

UsageSemanticFlags usage_semantic_flags(const aoahid_usage_semantic semantic) noexcept {
    switch (semantic) {
    case AOAHID_USAGE_SELECTOR_BITMAP:
    case AOAHID_USAGE_MOMENTARY:
    case AOAHID_USAGE_RETRIGGER:
        return UsageSemanticFlags{true, false, false};
    case AOAHID_USAGE_ON_OFF_TOGGLE:
    case AOAHID_USAGE_ONE_SHOT:
        return UsageSemanticFlags{true, true, false};
    case AOAHID_USAGE_ON_OFF_MAINTAINED:
        return UsageSemanticFlags{true, false, true};
    case AOAHID_USAGE_ON_OFF_PAIR:
    case AOAHID_USAGE_LINEAR:
    case AOAHID_USAGE_DYNAMIC_VALUE:
    case AOAHID_USAGE_NAMED_ARRAY:
    default:
        return UsageSemanticFlags{false, false, false};
    }
}

bool controller_axis_usage(const aoahid_axis_role role, std::uint16_t* usage_page,
                           std::uint16_t* usage) noexcept {
    if (usage_page == nullptr || usage == nullptr) {
        return false;
    }
    *usage_page = aoa::hid::usage::page_generic_desktop;
    switch (role) {
    case AOAHID_AXIS_X:
        *usage = aoa::hid::usage::x;
        return true;
    case AOAHID_AXIS_Y:
        *usage = aoa::hid::usage::y;
        return true;
    case AOAHID_AXIS_Z:
        *usage = aoa::hid::usage::z;
        return true;
    case AOAHID_AXIS_RX:
        *usage = aoa::hid::usage::rx;
        return true;
    case AOAHID_AXIS_RY:
        *usage = aoa::hid::usage::ry;
        return true;
    case AOAHID_AXIS_RZ:
        *usage = aoa::hid::usage::rz;
        return true;
    case AOAHID_AXIS_SLIDER:
        *usage = aoa::hid::usage::slider;
        return true;
    case AOAHID_AXIS_DIAL:
        *usage = aoa::hid::usage::dial;
        return true;
    case AOAHID_AXIS_WHEEL:
        *usage = aoa::hid::usage::wheel;
        return true;
    case AOAHID_AXIS_SIMULATION_RUDDER:
        *usage_page = aoa::hid::usage::page_simulation;
        *usage = aoa::hid::usage::simulation_rudder;
        return true;
    case AOAHID_AXIS_SIMULATION_THROTTLE:
        *usage_page = aoa::hid::usage::page_simulation;
        *usage = aoa::hid::usage::simulation_throttle;
        return true;
    case AOAHID_AXIS_SIMULATION_ACCELERATOR:
        *usage_page = aoa::hid::usage::page_simulation;
        *usage = aoa::hid::usage::simulation_accelerator;
        return true;
    case AOAHID_AXIS_SIMULATION_BRAKE:
        *usage_page = aoa::hid::usage::page_simulation;
        *usage = aoa::hid::usage::simulation_brake;
        return true;
    case AOAHID_AXIS_SIMULATION_STEERING:
        *usage_page = aoa::hid::usage::page_simulation;
        *usage = aoa::hid::usage::simulation_steering;
        return true;
    default:
        return false;
    }
}

bool controller_axis_domain_matches_usage(const aoahid_gamepad_axis_option& axis) noexcept {
    const auto& value = axis.value;
    switch (axis.role) {
    case AOAHID_AXIS_SIMULATION_ACCELERATOR:
    case AOAHID_AXIS_SIMULATION_BRAKE:
    case AOAHID_AXIS_SIMULATION_THROTTLE:
        return value.logical_minimum == 0 && value.logical_maximum > 0 && axis.neutral_value == 0;
    case AOAHID_AXIS_SIMULATION_STEERING:
    case AOAHID_AXIS_SIMULATION_RUDDER:
        return value.logical_minimum < 0 && value.logical_maximum > 0 && axis.neutral_value == 0;
    default:
        return true;
    }
}

bool emit_usage_controls(DescriptorBuilder& builder, const aoahid_toggle_options& options) {
    if (!builder.begin_application(options.application_page, options.application_usage) ||
        !builder.set_report_id(options.report_id.enabled == 1U, options.report_id.value)) {
        return false;
    }
    for (std::size_t index = 0U; index < options.allowed_usage_count; ++index) {
        const UsageSemanticFlags flags = usage_semantic_flags(options.usage_semantics[index]);
        if (!flags.accepted ||
            !builder.variable(options.field_page, options.allowed_usages[index], 0, 1, 1U,
                              flags.relative, false, FieldSemantic::consumer_usage,
                              static_cast<std::uint16_t>(index), flags.no_preferred)) {
            return false;
        }
    }
    pad_report(builder);
    return builder.end_collection();
}

aoahid_result create_usage_control_spec(const aoahid_toggle_options* options,
                                        aoahid_spec** out_spec) {
    if (options == nullptr || !aoa::detail::valid_struct(
                                  options, options == nullptr ? 0U : options->struct_size,
                                  static_cast<std::uint32_t>(sizeof(*options)), "toggle_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U || options->reserved16 != 0U ||
        !validate_report_id(options->report_id, "toggle.report_id")) {
        set_error(AOAHID_ERR_PARAM, "toggle.reserved",
                  "reserved fields must be zero and the Report ID declaration must be valid.");
        return AOAHID_ERR_PARAM;
    }
    const bool is_camera = options->field_page == aoa::hid::usage::page_camera_control;
    if (options->application_page == 0U || options->application_usage == 0U ||
        options->field_page == 0U) {
        set_error(AOAHID_ERR_UNSET_FIELD, "toggle.application",
                  "application_page, application_usage, and field_page are required.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (options->allowed_usages == nullptr || options->allowed_usage_count == 0U) {
        set_error(AOAHID_ERR_UNSET_FIELD, "toggle.allowed_usages",
                  "At least one caller-selected Usage is required.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (options->usage_semantics == nullptr) {
        set_error(AOAHID_ERR_UNSET_FIELD, "toggle.usage_semantics",
                  "Every allow-listed Usage needs an explicit HUT semantic.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (options->expected_linux_event_types == nullptr ||
        options->expected_linux_codes == nullptr) {
        set_error(AOAHID_ERR_UNSET_FIELD, "toggle.expected_linux_events",
                  "Every allow-listed Usage needs explicit target-event evidence.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (options->allowed_usage_count > std::numeric_limits<std::uint16_t>::max()) {
        set_error(AOAHID_ERR_OVERFLOW, "toggle.allowed_usage_count",
                  "The generated layout cannot index this many one-bit controls.");
        return AOAHID_ERR_OVERFLOW;
    }
    for (std::size_t index = 0; index < options->allowed_usage_count; ++index) {
        if (options->allowed_usages[index] == 0U ||
            options->expected_linux_event_types[index] == nullptr ||
            options->expected_linux_codes[index] == nullptr ||
            options->expected_linux_event_types[index][0] == '\0' ||
            options->expected_linux_codes[index][0] == '\0') {
            set_error(AOAHID_ERR_PARAM, "toggle.allowed_usages",
                      "Every nonzero Usage requires explicit expected Linux event type and code "
                      "evidence.");
            return AOAHID_ERR_PARAM;
        }
        if (std::find(options->allowed_usages, options->allowed_usages + index,
                      options->allowed_usages[index]) != options->allowed_usages + index) {
            set_error(AOAHID_ERR_PARAM, "toggle.allowed_usages",
                      "Each Usage may appear only once in the allow-list.");
            return AOAHID_ERR_PARAM;
        }
        if (options->application_page == options->field_page &&
            options->allowed_usages[index] == options->application_usage) {
            set_error(AOAHID_ERR_PARAM, "toggle.allowed_usages",
                      "The Application Collection Usage is not an Input control Usage.");
            return AOAHID_ERR_PARAM;
        }
        const aoahid_usage_semantic semantic = options->usage_semantics[index];
        if (!usage_semantic_flags(semantic).accepted) {
            if (semantic == AOAHID_USAGE_ON_OFF_PAIR || semantic == AOAHID_USAGE_LINEAR ||
                semantic == AOAHID_USAGE_DYNAMIC_VALUE || semantic == AOAHID_USAGE_NAMED_ARRAY) {
                set_error(AOAHID_ERR_UNSUPPORTED, "toggle.usage_semantics",
                          "This HUT semantic requires a value, direction, or collection API; "
                          "it cannot be represented by press/release.");
                return AOAHID_ERR_UNSUPPORTED;
            }
            set_error(AOAHID_ERR_PARAM, "toggle.usage_semantics",
                      "Every Usage must select one defined aoahid_usage_semantic value.");
            return AOAHID_ERR_PARAM;
        }
        if (is_camera && options->allowed_usages[index] != aoa::hid::usage::camera_auto_focus &&
            options->allowed_usages[index] != aoa::hid::usage::camera_shutter) {
            set_error(AOAHID_ERR_PARAM, "toggle.allowed_usages",
                      "HUT 1.7 Camera Control defines Auto-focus 0x20 and Shutter "
                      "0x21; no other Usage exists on that page.");
            return AOAHID_ERR_PARAM;
        }
        if (is_camera && semantic != AOAHID_USAGE_ONE_SHOT) {
            set_error(AOAHID_ERR_UNSUPPORTED, "toggle.usage_semantics",
                      "HUT 1.7 Table 35.1 defines Auto-focus and Shutter as One Shot Controls.");
            return AOAHID_ERR_UNSUPPORTED;
        }
    }
    aoa::detail::ConsumerConfig config{};
    config.options = *options;
    config.allowed_usages.assign(options->allowed_usages,
                                 options->allowed_usages + options->allowed_usage_count);
    config.usage_semantics.assign(options->usage_semantics,
                                  options->usage_semantics + options->allowed_usage_count);
    for (std::size_t index = 0U; index < options->allowed_usage_count; ++index) {
        config.expected_linux_event_types.emplace_back(options->expected_linux_event_types[index]);
        config.expected_linux_codes.emplace_back(options->expected_linux_codes[index]);
    }
    config.options.allowed_usages = nullptr;
    config.options.allowed_usage_count = config.allowed_usages.size();
    config.options.usage_semantics = nullptr;
    config.options.expected_linux_event_types = nullptr;
    config.options.expected_linux_codes = nullptr;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kToggleBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{AOAHID_PROFILE_TOGGLE, AOAHID_ANDROID_CONDITIONAL}, std::move(config),
        battery, [=](DescriptorBuilder& builder) { return emit_usage_controls(builder, *options); },
        out_spec);
}

template <typename Function>
aoahid_result abi_result(const char* field, Function&& function) noexcept {
    try {
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

template <typename Function>
aoahid_result abi_spec_factory(aoahid_spec** out_spec, const char* field,
                               Function&& function) noexcept {
    aoa::detail::clear_error();
    if (out_spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_spec", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_spec = nullptr;
    return abi_result(field, std::forward<Function>(function));
}

template <typename Function> void abi_void(const char* field, Function&& function) noexcept {
    try {
        function();
    } catch (const std::bad_alloc&) {
        set_error(AOAHID_ERR_INTERNAL, field,
                  "Memory allocation failed inside the C ABI boundary.");
    } catch (...) {
        set_error(AOAHID_ERR_INTERNAL, field,
                  "A C++ exception was contained at the C ABI boundary.");
    }
}

/* aoahid_touchscreen_options and aoahid_touchpad_options share every field below
 * verbatim (same names, same order); only button_count, present solely on
 * aoahid_touchpad_options, is excluded and set separately by each caller. One
 * templated copy keeps the two option structs' shared shape edited in exactly
 * one place instead of two independently hand-maintained functions. A
 * template cannot itself carry C language linkage, so it is defined here,
 * before the extern "C" block below, rather than beside its two callers. */
template <typename Options>
void touch_fields_from(const Options& options, aoa::detail::TouchFields* out) noexcept {
    out->report_id = options.report_id;
    out->maximum_contacts = options.maximum_contacts;
    out->contacts_per_report = options.contacts_per_report;
    out->contact_id = options.contact_id;
    out->x = options.x;
    out->y = options.y;
    out->contact_count = options.contact_count;
    out->enable_pressure = options.enable_pressure;
    out->pressure = options.pressure;
    out->enable_width = options.enable_width;
    out->width = options.width;
    out->enable_height = options.enable_height;
    out->height = options.height;
    out->enable_azimuth = options.enable_azimuth;
    out->azimuth = options.azimuth;
    out->enable_scan_time = options.enable_scan_time;
    out->scan_time = options.scan_time;
    out->scan_time_unit_100us = options.scan_time_unit_100us;
    out->enable_contact_count_maximum_feature_declaration =
        options.enable_contact_count_maximum_feature_declaration;
    out->enable_multi_packet_frames = options.enable_multi_packet_frames;
}
} // namespace

namespace aoa::detail {

bool retain_spec(aoahid_spec* spec) noexcept {
    if (spec == nullptr) {
        return false;
    }
    std::uint32_t references = spec->references.load(std::memory_order_relaxed);
    while (references != std::numeric_limits<std::uint32_t>::max()) {
        if (spec->references.compare_exchange_weak(references, references + 1U,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

bool release_spec(aoahid_spec* spec) noexcept {
    if (spec == nullptr) {
        return false;
    }
    std::uint32_t references = spec->references.load(std::memory_order_acquire);
    while (references != 0U) {
        if (spec->references.compare_exchange_weak(references, references - 1U,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            if (references == 1U) {
                delete spec;
            }
            return true;
        }
    }
    return false;
}

} // namespace aoa::detail

extern "C" {

static aoahid_result aoahid_spec_create_keyboard_impl(const aoahid_keyboard_options* options,
                                                      aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "keyboard_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U || !validate_report_id(options->report_id, "keyboard.report_id")) {
        set_error(AOAHID_ERR_PARAM, "keyboard.reserved", "reserved must be zero.");
        return AOAHID_ERR_PARAM;
    }
    if (options->usage_minimum < 0x04U || options->usage_maximum < options->usage_minimum) {
        set_error(AOAHID_ERR_PARAM, "keyboard.usage_range",
                  "The declared non-modifier Usage range must be ordered and exclude the reserved "
                  "keyboard error selectors 0x00 through 0x03.");
        return AOAHID_ERR_PARAM;
    }
    if (options->usage_minimum <= aoa::hid::usage::keyboard_right_gui &&
        options->usage_maximum >= aoa::hid::usage::keyboard_left_control) {
        set_error(AOAHID_ERR_PARAM, "keyboard.usage_range",
                  "The non-modifier bitmap cannot duplicate modifier Usages 0xE0 through 0xE7.");
        return AOAHID_ERR_PARAM;
    }

    aoa::detail::KeyboardConfig config{};
    config.options = *options;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kKeyboardBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{AOAHID_PROFILE_KEYBOARD, AOAHID_ANDROID_CONDITIONAL}, config, battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(aoa::hid::usage::page_generic_desktop,
                                           aoa::hid::usage::keyboard) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.variable_range(
                    aoa::hid::usage::page_keyboard, aoa::hid::usage::keyboard_left_control,
                    aoa::hid::usage::keyboard_right_gui, 1U, FieldSemantic::modifier) ||
                !builder.variable_range(aoa::hid::usage::page_keyboard, options->usage_minimum,
                                        options->usage_maximum, 1U, FieldSemantic::key_bitmap)) {
                return false;
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

static aoahid_result aoahid_spec_create_mouse_impl(const aoahid_mouse_options* options,
                                                   aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)), "mouse_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U || !validate_report_id(options->report_id, "mouse.report_id") ||
        !validate_field(options->x, "mouse.x") || !validate_field(options->y, "mouse.y") ||
        options->x.logical_minimum >= 0 || options->x.logical_maximum <= 0 ||
        options->y.logical_minimum >= 0 || options->y.logical_maximum <= 0 ||
        options->button_count == 0U || options->button_count > 65535U ||
        !validate_option_boolean(options->enable_wheel, "mouse.enable_wheel") ||
        !validate_option_boolean(options->enable_pan, "mouse.enable_pan")) {
        set_error(AOAHID_ERR_PARAM, "mouse_options",
                  "The mouse options require valid relative X/Y fields, at least one button, valid "
                  "flags, and zero reserved data.");
        return AOAHID_ERR_PARAM;
    }
    if ((options->enable_wheel == 1U && !validate_field(options->wheel, "mouse.wheel")) ||
        (options->enable_pan == 1U && !validate_field(options->pan, "mouse.pan"))) {
        return AOAHID_ERR_PARAM;
    }
    if ((options->enable_wheel == 0U && !field_is_zero(options->wheel)) ||
        (options->enable_pan == 0U && !field_is_zero(options->pan))) {
        set_error(AOAHID_ERR_PARAM, "mouse.disabled_fields",
                  "Disabled wheel and pan declarations must be canonical zero.");
        return AOAHID_ERR_PARAM;
    }
    if ((options->enable_wheel == 1U &&
         (options->wheel.logical_minimum >= 0 || options->wheel.logical_maximum <= 0)) ||
        (options->enable_pan == 1U &&
         (options->pan.logical_minimum >= 0 || options->pan.logical_maximum <= 0))) {
        set_error(AOAHID_ERR_PARAM, "mouse.scroll_range",
                  "Portable relative wheel and pan ranges must represent negative, zero, and "
                  "positive deltas.");
        return AOAHID_ERR_PARAM;
    }
    aoa::detail::MouseConfig config{};
    config.options = *options;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kMouseBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{AOAHID_PROFILE_MOUSE, AOAHID_ANDROID_PORTABLE_CANDIDATE}, config,
        battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(aoa::hid::usage::page_generic_desktop,
                                           aoa::hid::usage::mouse) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.begin_physical(aoa::hid::usage::page_generic_desktop,
                                        aoa::hid::usage::pointer) ||
                !builder.variable_range(aoa::hid::usage::page_button, 1U,
                                        static_cast<std::uint16_t>(options->button_count), 1U,
                                        FieldSemantic::buttons)) {
                return false;
            }
            pad_report(builder);
            if (!builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::x,
                                  options->x.logical_minimum, options->x.logical_maximum,
                                  static_cast<std::uint8_t>(options->x.bit_width), true, false,
                                  FieldSemantic::x, 0U, false, &options->x.physical) ||
                !builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::y,
                                  options->y.logical_minimum, options->y.logical_maximum,
                                  static_cast<std::uint8_t>(options->y.bit_width), true, false,
                                  FieldSemantic::y, 0U, false, &options->y.physical)) {
                return false;
            }
            // X and Y widths are caller-declared (1-32 bits, see LIMITS.md);
            // realign to a byte boundary before Wheel/Pan so a wide,
            // misaligned field can never cross a fifth byte (HID 1.11 8.4).
            pad_report(builder);
            if (options->enable_wheel == 1U &&
                !builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::wheel,
                                  options->wheel.logical_minimum, options->wheel.logical_maximum,
                                  static_cast<std::uint8_t>(options->wheel.bit_width), true, false,
                                  FieldSemantic::wheel, 0U, false, &options->wheel.physical)) {
                return false;
            }
            pad_report(builder);
            if (options->enable_pan == 1U &&
                !builder.variable(aoa::hid::usage::page_consumer, aoa::hid::usage::ac_pan,
                                  options->pan.logical_minimum, options->pan.logical_maximum,
                                  static_cast<std::uint8_t>(options->pan.bit_width), true, false,
                                  FieldSemantic::pan, 0U, false, &options->pan.physical)) {
                return false;
            }
            pad_report(builder);
            return builder.end_collection() && builder.end_collection();
        },
        out_spec);
}

static aoahid_result aoahid_spec_create_toggle_impl(const aoahid_toggle_options* options,
                                                    aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    return create_usage_control_spec(options, out_spec);
}

constexpr aoahid_physical_properties kAndroidHatDegrees{1U, 0, 315, 0, 0x14U};

static aoahid_result create_controller_spec(const aoahid_gamepad_options* options,
                                            aoahid_spec** out_spec) {
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "gamepad_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U || !validate_report_id(options->report_id, "gamepad.report_id") ||
        options->axes == nullptr || options->axis_count < 2U || options->button_count == 0U ||
        options->button_count > 65535U || options->button_usage_minimum == 0U ||
        (options->dpad_representation != AOAHID_DPAD_NONE &&
         options->dpad_representation != AOAHID_DPAD_HAT &&
         options->dpad_representation != AOAHID_DPAD_BUTTONS)) {
        set_error(AOAHID_ERR_PARAM, "gamepad",
                  "A gamepad requires explicit axes, at least one button, and valid flags.");
        return AOAHID_ERR_PARAM;
    }
    if (options->axis_count > std::numeric_limits<std::uint16_t>::max()) {
        set_error(AOAHID_ERR_OVERFLOW, "gamepad.axis_count",
                  "The generated layout cannot index more than 65535 controller axes.");
        return AOAHID_ERR_OVERFLOW;
    }
    if (static_cast<std::uint32_t>(options->button_usage_minimum) + options->button_count - 1U >
        std::numeric_limits<std::uint16_t>::max()) {
        set_error(AOAHID_ERR_OVERFLOW, "gamepad.button_usage_minimum",
                  "The declared button Usage range exceeds the 16-bit Usage value space.");
        return AOAHID_ERR_OVERFLOW;
    }
    if (options->dpad_representation == AOAHID_DPAD_BUTTONS &&
        options->button_count >
            static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) - 3U) {
        set_error(AOAHID_ERR_OVERFLOW, "gamepad.button_count",
                  "Four D-pad fields must fit the layout's 16-bit field-instance space.");
        return AOAHID_ERR_OVERFLOW;
    }
    bool has_x = false;
    bool has_y = false;
    aoa::detail::GamepadConfig config{};
    config.options = *options;
    for (std::size_t index = 0; index < options->axis_count; ++index) {
        std::uint16_t expected_page = 0U;
        std::uint16_t expected_usage = 0U;
        if (!controller_axis_usage(options->axes[index].role, &expected_page, &expected_usage) ||
            options->axes[index].usage_page != expected_page ||
            options->axes[index].usage != expected_usage) {
            set_error(AOAHID_ERR_PARAM, "gamepad.axes.role",
                      "Each axis role must match its audited HUT 1.7 Usage Page and Usage.");
            return AOAHID_ERR_PARAM;
        }
        for (std::size_t previous = 0U; previous < index; ++previous) {
            if (options->axes[previous].role == options->axes[index].role) {
                set_error(AOAHID_ERR_PARAM, "gamepad.axes.role",
                          "Each controller axis role may be declared only once.");
                return AOAHID_ERR_PARAM;
            }
        }
        if (!validate_field(options->axes[index].value, "gamepad.axes.value") ||
            options->axes[index].neutral_value < options->axes[index].value.logical_minimum ||
            options->axes[index].neutral_value > options->axes[index].value.logical_maximum ||
            options->axes[index].usage_page == 0U || options->axes[index].usage == 0U ||
            options->axes[index].expected_linux_code == nullptr ||
            options->axes[index].expected_android_axis == nullptr ||
            options->axes[index].expected_linux_code[0] == '\0' ||
            options->axes[index].expected_android_axis[0] == '\0') {
            set_error(
                AOAHID_ERR_UNSET_FIELD, "gamepad.axes",
                "Every axis requires Usage, range, width, Linux code, and Android axis evidence.");
            return AOAHID_ERR_UNSET_FIELD;
        }
        if (!controller_axis_domain_matches_usage(options->axes[index])) {
            set_error(AOAHID_ERR_PARAM, "gamepad.axes.value",
                      "Simulation Accelerator, Brake, and Throttle require a zero-to-positive "
                      "range with zero neutral; Steering and Rudder require a signed range "
                      "around zero with zero neutral.");
            return AOAHID_ERR_PARAM;
        }
        has_x = has_x || options->axes[index].role == AOAHID_AXIS_X;
        has_y = has_y || options->axes[index].role == AOAHID_AXIS_Y;
        aoa::detail::GamepadAxisConfig axis{};
        axis.axis = options->axes[index];
        axis.expected_linux_code = options->axes[index].expected_linux_code;
        axis.expected_android_axis = options->axes[index].expected_android_axis;
        axis.axis.expected_linux_code = nullptr;
        axis.axis.expected_android_axis = nullptr;
        config.axes.push_back(std::move(axis));
    }
    if (!has_x || !has_y) {
        set_error(AOAHID_ERR_PARAM, "gamepad.axes",
                  "The portable candidate requires explicitly declared X and Y axes.");
        return AOAHID_ERR_PARAM;
    }
    if (options->dpad_representation == AOAHID_DPAD_HAT) {
        if (options->hat_logical_minimum != 0 || options->hat_logical_maximum != 7 ||
            options->hat_bit_width != 4U ||
            !has_null_encoding(NullEncodingRange{options->hat_logical_minimum,
                                                 options->hat_logical_maximum,
                                                 options->hat_bit_width})) {
            set_error(AOAHID_ERR_PARAM, "gamepad.hat",
                      "The Android portable-candidate Hat must explicitly select logical 0..7 "
                      "in four bits and leave an encoding for Null State.");
            return AOAHID_ERR_PARAM;
        }
    } else if (options->hat_logical_minimum != 0 || options->hat_logical_maximum != 0 ||
               options->hat_bit_width != 0U) {
        set_error(AOAHID_ERR_PARAM, "gamepad.hat",
                  "Hat range fields must be canonical zero unless the D-pad is a Hat Switch.");
        return AOAHID_ERR_PARAM;
    }
    config.options.axes = nullptr;
    // Android 17 CDD §7.2.6.1 lists A/B/X/Y as Button usages 1, 2, 4,
    // and 5 and defines its directional mapping through the canonical Hat.
    // A contiguous Button range starting at 1 therefore needs at least five
    // entries before this descriptor shape is a portable candidate.
    const bool has_android_abxy =
        options->button_usage_minimum == 1U && options->button_count >= 5U;
    const aoahid_android_status android_status =
        options->dpad_representation != AOAHID_DPAD_HAT || !has_android_abxy
            ? AOAHID_ANDROID_CONDITIONAL
            : AOAHID_ANDROID_PORTABLE_CANDIDATE;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery, kGamepadBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{AOAHID_PROFILE_GAMEPAD, android_status}, std::move(config), battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(aoa::hid::usage::page_generic_desktop,
                                           aoa::hid::usage::gamepad) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.variable_range(aoa::hid::usage::page_button, options->button_usage_minimum,
                                        static_cast<std::uint16_t>(options->button_usage_minimum +
                                                                   options->button_count - 1U),
                                        1U, FieldSemantic::buttons)) {
                return false;
            }
            pad_report(builder);
            if (options->dpad_representation == AOAHID_DPAD_HAT &&
                !builder.variable(aoa::hid::usage::page_generic_desktop,
                                  aoa::hid::usage::hat_switch, options->hat_logical_minimum,
                                  options->hat_logical_maximum,
                                  static_cast<std::uint8_t>(options->hat_bit_width), false, true,
                                  FieldSemantic::hat, 0U, false, &kAndroidHatDegrees)) {
                return false;
            }
            if (options->dpad_representation == AOAHID_DPAD_BUTTONS) {
                constexpr std::uint16_t usages[] = {
                    aoa::hid::usage::dpad_up, aoa::hid::usage::dpad_down,
                    aoa::hid::usage::dpad_right, aoa::hid::usage::dpad_left};
                for (std::size_t index = 0U; index < 4U; ++index) {
                    if (!builder.variable(aoa::hid::usage::page_generic_desktop, usages[index], 0,
                                          1, 1U, false, false, FieldSemantic::buttons,
                                          static_cast<std::uint16_t>(options->button_count + index),
                                          true)) {
                        return false;
                    }
                }
            }
            // The Hat Switch and the raw D-pad OOC bits above are 4 bits wide
            // and are not byte-aligned on their own; without realigning here,
            // an axis width in HUT 1.7's full 1-32 bit range (see LIMITS.md)
            // could start at a nonzero bit offset and cross a fifth byte,
            // which HID 1.11 section 8.4 forbids for a single field.
            pad_report(builder);
            for (std::size_t index = 0; index < options->axis_count; ++index) {
                const aoahid_gamepad_axis_option& axis = options->axes[index];
                if (!builder.variable(
                        axis.usage_page, axis.usage, axis.value.logical_minimum,
                        axis.value.logical_maximum, static_cast<std::uint8_t>(axis.value.bit_width),
                        false, false, FieldSemantic::gamepad_axis,
                        static_cast<std::uint16_t>(index), false, &axis.value.physical)) {
                    return false;
                }
                // Each axis has its own caller-declared width (1-32 bits, see LIMITS.md). A
                // width that is not a multiple of eight leaves the next axis at a nonzero bit
                // offset; realigning after each axis keeps that drift from ever compounding into
                // a fifth-byte span two or more axes in, regardless of the declared width.
                pad_report(builder);
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

static aoahid_result aoahid_spec_create_gamepad_impl(const aoahid_gamepad_options* options,
                                                     aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    return create_controller_spec(options, out_spec);
}

// set_error() keeps the field-name pointer without copying it (see
// error_detail.cpp), so every name is a string literal.
struct TouchFieldNames {
    const char* report_id;
    const char* x;
    const char* y;
    const char* enable_pressure;
    const char* enable_width;
    const char* enable_height;
    const char* enable_azimuth;
    const char* enable_scan_time;
    const char* scan_time_unit_100us;
    const char* enable_contact_count_maximum_feature_declaration;
    const char* enable_multi_packet_frames;
    const char* fields;
    const char* contact_id;
    const char* contact_count;
    const char* contact_fields;
    const char* contacts_per_report;
    const char* pressure;
    const char* disabled_fields;
    const char* azimuth;
    const char* width;
    const char* height;
    const char* inactive_geometry;
    const char* geometry_units;
    const char* scan_time;
    const char* button_count;
};

constexpr TouchFieldNames kTouchscreenFieldNames{
    "touchscreen.report_id",
    "touchscreen.x",
    "touchscreen.y",
    "touchscreen.enable_pressure",
    "touchscreen.enable_width",
    "touchscreen.enable_height",
    "touchscreen.enable_azimuth",
    "touchscreen.enable_scan_time",
    "touchscreen.scan_time_unit_100us",
    "touchscreen.enable_contact_count_maximum_feature_declaration",
    "touchscreen.enable_multi_packet_frames",
    "touchscreen.fields",
    "touchscreen.contact_id",
    "touchscreen.contact_count",
    "touchscreen.contact_fields",
    "touchscreen.contacts_per_report",
    "touchscreen.pressure",
    "touchscreen.disabled_fields",
    "touchscreen.azimuth",
    "touchscreen.width",
    "touchscreen.height",
    "touchscreen.inactive_geometry",
    "touchscreen.geometry_units",
    "touchscreen.scan_time",
    "touchscreen.button_count",
};

constexpr TouchFieldNames kTouchpadFieldNames{
    "touchpad.report_id",
    "touchpad.x",
    "touchpad.y",
    "touchpad.enable_pressure",
    "touchpad.enable_width",
    "touchpad.enable_height",
    "touchpad.enable_azimuth",
    "touchpad.enable_scan_time",
    "touchpad.scan_time_unit_100us",
    "touchpad.enable_contact_count_maximum_feature_declaration",
    "touchpad.enable_multi_packet_frames",
    "touchpad.fields",
    "touchpad.contact_id",
    "touchpad.contact_count",
    "touchpad.contact_fields",
    "touchpad.contacts_per_report",
    "touchpad.pressure",
    "touchpad.disabled_fields",
    "touchpad.azimuth",
    "touchpad.width",
    "touchpad.height",
    "touchpad.inactive_geometry",
    "touchpad.geometry_units",
    "touchpad.scan_time",
    "touchpad.button_count",
};

// profile_kind, application_usage, and android_status are only ever passed
// as fixed literals from this function's two call sites (the Touchscreen and
// Touchpad wrappers), never from caller-supplied runtime values, so a
// transposition would be a compile-time-local mistake caught immediately by
// every existing test rather than a live bug.
static aoahid_result
create_touch_spec_from_fields(const aoa::detail::TouchFields& fields, const TouchFieldNames& names,
                              // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
                              const aoahid_profile_kind profile_kind,
                              const std::uint16_t application_usage,
                              const aoahid_android_status android_status,
                              const BatteryRequest& battery, aoahid_spec** out_spec) {
    if (!validate_report_id(fields.report_id, names.report_id) ||
        !validate_field(fields.x, names.x) || !validate_field(fields.y, names.y) ||
        fields.x.logical_minimum > 0 || fields.x.logical_maximum < 0 ||
        fields.y.logical_minimum > 0 || fields.y.logical_maximum < 0 ||
        fields.maximum_contacts == 0U || fields.maximum_contacts > 16U ||
        fields.contacts_per_report == 0U || fields.contacts_per_report > fields.maximum_contacts ||
        !validate_option_boolean(fields.enable_pressure, names.enable_pressure) ||
        !validate_option_boolean(fields.enable_width, names.enable_width) ||
        !validate_option_boolean(fields.enable_height, names.enable_height) ||
        !validate_option_boolean(fields.enable_azimuth, names.enable_azimuth) ||
        !validate_option_boolean(fields.enable_scan_time, names.enable_scan_time) ||
        !validate_option_boolean(fields.scan_time_unit_100us, names.scan_time_unit_100us) ||
        !validate_option_boolean(fields.enable_contact_count_maximum_feature_declaration,
                                 names.enable_contact_count_maximum_feature_declaration) ||
        !validate_option_boolean(fields.enable_multi_packet_frames,
                                 names.enable_multi_packet_frames)) {
        set_error(AOAHID_ERR_PARAM, names.fields,
                  "The touch options contain an invalid required field, range, count, or flag.");
        return AOAHID_ERR_PARAM;
    }
    if (!validate_field(fields.contact_id, names.contact_id) ||
        !validate_field(fields.contact_count, names.contact_count)) {
        set_error(AOAHID_ERR_PARAM, names.contact_fields,
                  "Fixed-slot Multi-Touch requires explicit Contact Identifier and Contact Count "
                  "fields.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.contact_count.logical_minimum > 0 ||
        fields.contact_count.logical_maximum < static_cast<std::int32_t>(fields.maximum_contacts)) {
        set_error(AOAHID_ERR_PARAM, names.contact_count,
                  "Contact Count must represent zero through maximum_contacts even though "
                  "fixed-slot output never emits an isolated zero frame.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.contact_id.logical_minimum > 0 || fields.contact_id.logical_maximum < 0 ||
        static_cast<std::uint64_t>(fields.contact_id.logical_maximum) + 1U <
            fields.maximum_contacts) {
        set_error(AOAHID_ERR_PARAM, names.contact_id,
                  "The Contact Identifier range must represent zero for inactive slots and at "
                  "least maximum_contacts distinct nonnegative API IDs.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_multi_packet_frames == 0U &&
        fields.contacts_per_report != fields.maximum_contacts) {
        set_error(AOAHID_ERR_PARAM, names.contacts_per_report,
                  "Without multi-packet frames every declared contact must fit one report.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_pressure == 1U &&
        (!validate_field(fields.pressure, names.pressure) || fields.pressure.logical_minimum != 0 ||
         fields.pressure.logical_maximum < 1)) {
        set_error(AOAHID_ERR_PARAM, names.pressure,
                  "Enabled touch pressure must explicitly represent zero and at least one positive "
                  "contact value.");
        return AOAHID_ERR_PARAM;
    }
    if ((fields.enable_pressure == 0U && !field_is_zero(fields.pressure)) ||
        (fields.enable_width == 0U && !field_is_zero(fields.width)) ||
        (fields.enable_height == 0U && !field_is_zero(fields.height)) ||
        (fields.enable_azimuth == 0U && !field_is_zero(fields.azimuth)) ||
        (fields.enable_scan_time == 0U &&
         (!field_is_zero(fields.scan_time) || fields.scan_time_unit_100us != 0U))) {
        set_error(
            AOAHID_ERR_PARAM, names.disabled_fields,
            "Disabled pressure, geometry, and Scan Time declarations must be canonical zero.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_azimuth == 1U &&
        (!validate_field(fields.azimuth, names.azimuth) || fields.azimuth.logical_minimum != 0 ||
         fields.azimuth.logical_maximum <= 0)) {
        set_error(AOAHID_ERR_PARAM, names.azimuth,
                  "Enabled Azimuth requires logical minimum zero and a positive full-turn logical "
                  "maximum.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_width == 1U && !validate_field(fields.width, names.width)) {
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_height == 1U && !validate_field(fields.height, names.height)) {
        return AOAHID_ERR_PARAM;
    }
    if ((fields.enable_width == 1U &&
         (fields.width.logical_minimum > 0 || fields.width.logical_maximum < 0)) ||
        (fields.enable_height == 1U &&
         (fields.height.logical_minimum > 0 || fields.height.logical_maximum < 0))) {
        set_error(AOAHID_ERR_PARAM, names.inactive_geometry,
                  "Enabled width and height fields must represent zero for inactive fixed slots.");
        return AOAHID_ERR_PARAM;
    }
    if ((fields.enable_width == 1U &&
         !physical_units_match(fields.width.physical, fields.x.physical)) ||
        (fields.enable_height == 1U &&
         !physical_units_match(fields.height.physical, fields.y.physical))) {
        set_error(AOAHID_ERR_PARAM, names.geometry_units,
                  "HUT Width must use the same HID Unit and Unit Exponent as X, and Height must "
                  "use the same Unit and Unit Exponent as Y.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.enable_scan_time == 1U &&
        (!validate_field(fields.scan_time, names.scan_time) || fields.scan_time_unit_100us != 1U ||
         fields.scan_time.logical_minimum != 0 || fields.scan_time.logical_maximum < 1 ||
         !physical_is_zero(fields.scan_time.physical))) {
        set_error(
            AOAHID_ERR_PARAM, names.scan_time,
            "The portable Linux profile requires an explicit 100-microsecond Scan Time counter.");
        return AOAHID_ERR_PARAM;
    }
    if (fields.button_count > 65535U) {
        set_error(AOAHID_ERR_OVERFLOW, names.button_count,
                  "The Button Usage range cannot exceed the 16-bit Usage value space.");
        return AOAHID_ERR_OVERFLOW;
    }
    aoa::detail::TouchConfig config{};
    config.fields = fields;
    if (!validate_battery(battery, profile_kind == AOAHID_PROFILE_TOUCHSCREEN
                                       ? kTouchscreenBatteryNames
                                       : kTouchpadBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{profile_kind, android_status}, config, battery,
        [=](DescriptorBuilder& builder) {
            if (!builder.begin_application(aoa::hid::usage::page_digitizers, application_usage) ||
                !builder.set_report_id(fields.report_id.enabled == 1U, fields.report_id.value)) {
                return false;
            }
            if (fields.enable_contact_count_maximum_feature_declaration == 1U &&
                !builder.feature_static_value(
                    aoa::hid::usage::page_digitizers, aoa::hid::usage::contact_count_maximum, 0,
                    static_cast<std::int32_t>(fields.maximum_contacts),
                    static_cast<std::uint8_t>(fields.contact_count.bit_width))) {
                return false;
            }
            for (std::uint32_t contact = 0U; contact < fields.contacts_per_report; ++contact) {
                if (!builder.begin_logical(aoa::hid::usage::page_digitizers,
                                           aoa::hid::usage::finger) ||
                    !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::tip_switch,
                                      0, 1, 1U, false, false, FieldSemantic::tip,
                                      static_cast<std::uint16_t>(contact))) {
                    return false;
                }
                pad_report(builder);
                if (!builder.variable(
                        aoa::hid::usage::page_digitizers, aoa::hid::usage::contact_identifier,
                        fields.contact_id.logical_minimum, fields.contact_id.logical_maximum,
                        static_cast<std::uint8_t>(fields.contact_id.bit_width), false, false,
                        FieldSemantic::contact_id, static_cast<std::uint16_t>(contact), false,
                        &fields.contact_id.physical) ||
                    !builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::x,
                                      fields.x.logical_minimum, fields.x.logical_maximum,
                                      static_cast<std::uint8_t>(fields.x.bit_width), false, false,
                                      FieldSemantic::x, static_cast<std::uint16_t>(contact), false,
                                      &fields.x.physical) ||
                    !builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::y,
                                      fields.y.logical_minimum, fields.y.logical_maximum,
                                      static_cast<std::uint8_t>(fields.y.bit_width), false, false,
                                      FieldSemantic::y, static_cast<std::uint16_t>(contact), false,
                                      &fields.y.physical)) {
                    return false;
                }
                // Contact Identifier, X, Y, Pressure, Width, Height, and
                // Azimuth are each an independently caller-declared field
                // (see LIMITS.md); realigning after Y and between the
                // optional fields keeps any one of them from starting at a
                // nonzero bit offset and crossing a fifth byte (HID 1.11 8.4).
                pad_report(builder);
                if (fields.enable_pressure == 1U &&
                    !builder.variable(
                        aoa::hid::usage::page_digitizers, aoa::hid::usage::tip_pressure,
                        fields.pressure.logical_minimum, fields.pressure.logical_maximum,
                        static_cast<std::uint8_t>(fields.pressure.bit_width), false, false,
                        FieldSemantic::pressure, static_cast<std::uint16_t>(contact), false,
                        &fields.pressure.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_width == 1U &&
                    !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::width,
                                      fields.width.logical_minimum, fields.width.logical_maximum,
                                      static_cast<std::uint8_t>(fields.width.bit_width), false,
                                      false, FieldSemantic::width,
                                      static_cast<std::uint16_t>(contact), false,
                                      &fields.width.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_height == 1U &&
                    !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::height,
                                      fields.height.logical_minimum, fields.height.logical_maximum,
                                      static_cast<std::uint8_t>(fields.height.bit_width), false,
                                      false, FieldSemantic::height,
                                      static_cast<std::uint16_t>(contact), false,
                                      &fields.height.physical)) {
                    return false;
                }
                pad_report(builder);
                if (fields.enable_azimuth == 1U &&
                    !builder.variable(
                        aoa::hid::usage::page_digitizers, aoa::hid::usage::azimuth,
                        fields.azimuth.logical_minimum, fields.azimuth.logical_maximum,
                        static_cast<std::uint8_t>(fields.azimuth.bit_width), false, false,
                        FieldSemantic::azimuth, static_cast<std::uint16_t>(contact), false,
                        &fields.azimuth.physical)) {
                    return false;
                }
                if (!builder.end_collection()) {
                    return false;
                }
            }
            // Scan Time and Contact Count are each an independently
            // caller-declared 1-32 bit field (see LIMITS.md), following the
            // last enabled per-contact field above; realigning between them
            // keeps any one of them from starting at a nonzero bit offset
            // and crossing a fifth byte (HID 1.11 8.4).
            pad_report(builder);
            if (fields.enable_scan_time == 1U &&
                !builder.variable(
                    aoa::hid::usage::page_digitizers, aoa::hid::usage::scan_time,
                    fields.scan_time.logical_minimum, fields.scan_time.logical_maximum,
                    static_cast<std::uint8_t>(fields.scan_time.bit_width), false, false,
                    FieldSemantic::scan_time, 0U, false, &fields.scan_time.physical)) {
                return false;
            }
            pad_report(builder);
            if (!builder.variable(
                    aoa::hid::usage::page_digitizers, aoa::hid::usage::contact_count,
                    fields.contact_count.logical_minimum, fields.contact_count.logical_maximum,
                    static_cast<std::uint8_t>(fields.contact_count.bit_width), false, false,
                    FieldSemantic::contact_count, 0U, false, &fields.contact_count.physical)) {
                return false;
            }
            if (fields.button_count > 0U) {
                if (!builder.variable_range(aoa::hid::usage::page_button, 1U,
                                            static_cast<std::uint16_t>(fields.button_count), 1U,
                                            FieldSemantic::buttons)) {
                    return false;
                }
            }
            pad_report(builder);
            return builder.end_collection();
        },
        out_spec);
}

static aoahid_result aoahid_spec_create_touchscreen_impl(const aoahid_touchscreen_options* options,
                                                         aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "touchscreen_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U) {
        set_error(AOAHID_ERR_PARAM, "touchscreen_options",
                  "The touch options contain an invalid required field, range, count, or flag.");
        return AOAHID_ERR_PARAM;
    }
    aoa::detail::TouchFields fields{};
    touch_fields_from(*options, &fields);
    fields.button_count = 0U;
    return create_touch_spec_from_fields(
        fields, kTouchscreenFieldNames, AOAHID_PROFILE_TOUCHSCREEN, aoa::hid::usage::touch_screen,
        AOAHID_ANDROID_PORTABLE_CANDIDATE,
        BatteryRequest{options->enable_battery, options->battery, options->report_id}, out_spec);
}

static aoahid_result aoahid_spec_create_touchpad_impl(const aoahid_touchpad_options* options,
                                                      aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "touchpad_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (options->reserved != 0U) {
        set_error(AOAHID_ERR_PARAM, "touchpad_options",
                  "The touchpad options contain an invalid required field, range, count, or flag.");
        return AOAHID_ERR_PARAM;
    }
    aoa::detail::TouchFields fields{};
    touch_fields_from(*options, &fields);
    fields.button_count = options->button_count;
    return create_touch_spec_from_fields(
        fields, kTouchpadFieldNames, AOAHID_PROFILE_TOUCHPAD, aoa::hid::usage::touch_pad,
        AOAHID_ANDROID_CONDITIONAL,
        BatteryRequest{options->enable_battery, options->battery, options->report_id}, out_spec);
}

// Pen and Digitizer share their validation; each reports its own field names.
struct StylusFieldNames {
    const char* barrel_usages;
    const char* disabled_fields;
    const char* enable_eraser;
    const char* enable_hover;
    const char* enable_pressure;
    const char* enable_tilt;
    const char* enable_twist_target_specific;
    const char* fields;
    const char* neutral_fields;
    const char* pressure;
    const char* report_id;
    const char* tilt_x;
    const char* tilt_y;
    const char* twist;
    const char* x;
    const char* y;
    const char* options;
};

constexpr StylusFieldNames kPenFieldNames{
    "pen.barrel_usages",
    "pen.disabled_fields",
    "pen.enable_eraser",
    "pen.enable_hover",
    "pen.enable_pressure",
    "pen.enable_tilt",
    "pen.enable_twist_target_specific",
    "pen.fields",
    "pen.neutral_fields",
    "pen.pressure",
    "pen.report_id",
    "pen.tilt_x",
    "pen.tilt_y",
    "pen.twist",
    "pen.x",
    "pen.y",
    "pen_options",
};

constexpr StylusFieldNames kDigitizerFieldNames{
    "digitizer.barrel_usages",
    "digitizer.disabled_fields",
    "digitizer.enable_eraser",
    "digitizer.enable_hover",
    "digitizer.enable_pressure",
    "digitizer.enable_tilt",
    "digitizer.enable_twist_target_specific",
    "digitizer.fields",
    "digitizer.neutral_fields",
    "digitizer.pressure",
    "digitizer.report_id",
    "digitizer.tilt_x",
    "digitizer.tilt_y",
    "digitizer.twist",
    "digitizer.x",
    "digitizer.y",
    "digitizer_options",
};

// Pen and Digitizer share one report shape; only the Application Usage and
// the Android classification differ.
static aoahid_result create_stylus_spec(const aoahid_pen_options* options,
                                        const aoahid_profile_kind kind, aoahid_spec** out_spec) {
    const StylusFieldNames& names =
        kind == AOAHID_PROFILE_PEN ? kPenFieldNames : kDigitizerFieldNames;
    if (options->reserved != 0U || !validate_report_id(options->report_id, names.report_id) ||
        !validate_field(options->x, names.x) || !validate_field(options->y, names.y) ||
        options->x.logical_minimum > 0 || options->x.logical_maximum < 0 ||
        options->y.logical_minimum > 0 || options->y.logical_maximum < 0 ||
        !validate_option_boolean(options->enable_pressure, names.enable_pressure) ||
        !validate_option_boolean(options->enable_tilt, names.enable_tilt) ||
        !validate_option_boolean(options->enable_twist_target_specific,
                                 names.enable_twist_target_specific) ||
        !validate_option_boolean(options->enable_eraser, names.enable_eraser) ||
        !validate_option_boolean(options->enable_hover, names.enable_hover) ||
        options->barrel_usage_count > 32U ||
        (options->barrel_usage_count != 0U && options->barrel_usages == nullptr)) {
        return aoa::detail::finish_result(
            AOAHID_ERR_PARAM, names.options,
            "An enabled pen field has an invalid range or bit width.");
    }
    if ((options->enable_pressure == 1U &&
         (!validate_field(options->pressure, names.pressure) ||
          options->pressure.logical_minimum != 0 || options->pressure.logical_maximum < 1)) ||
        (options->enable_tilt == 1U && (!validate_field(options->tilt_x, names.tilt_x) ||
                                        !validate_field(options->tilt_y, names.tilt_y))) ||
        (options->enable_twist_target_specific == 1U &&
         !validate_field(options->twist, names.twist))) {
        return aoa::detail::finish_result(
            AOAHID_ERR_PARAM, names.fields,
            "An enabled pen field has an invalid range or bit width.");
    }
    if ((options->enable_pressure == 0U && !field_is_zero(options->pressure)) ||
        (options->enable_tilt == 0U &&
         (!field_is_zero(options->tilt_x) || !field_is_zero(options->tilt_y))) ||
        (options->enable_twist_target_specific == 0U && !field_is_zero(options->twist)) ||
        (options->barrel_usage_count == 0U && options->barrel_usages != nullptr)) {
        set_error(AOAHID_ERR_PARAM, names.disabled_fields,
                  "Disabled optional pen declarations and absent barrel storage must be canonical "
                  "zero/null.");
        return AOAHID_ERR_PARAM;
    }
    if ((options->enable_tilt == 1U &&
         (options->tilt_x.logical_minimum > 0 || options->tilt_x.logical_maximum < 0 ||
          options->tilt_y.logical_minimum > 0 || options->tilt_y.logical_maximum < 0)) ||
        (options->enable_twist_target_specific == 1U &&
         (options->twist.logical_minimum > 0 || options->twist.logical_maximum < 0))) {
        set_error(
            AOAHID_ERR_PARAM, names.neutral_fields,
            "Enabled tilt and twist fields must represent zero for hover and departure reports.");
        return AOAHID_ERR_PARAM;
    }
    aoa::detail::PenConfig config{};
    config.options = *options;
    if (options->barrel_usage_count != 0U) {
        config.barrel_usages.assign(options->barrel_usages,
                                    options->barrel_usages + options->barrel_usage_count);
    }
    for (std::size_t index = 0U; index < config.barrel_usages.size(); ++index) {
        if ((config.barrel_usages[index] != aoa::hid::usage::barrel_switch &&
             config.barrel_usages[index] != aoa::hid::usage::secondary_barrel_switch) ||
            std::find(config.barrel_usages.begin(),
                      config.barrel_usages.begin() + static_cast<std::ptrdiff_t>(index),
                      config.barrel_usages[index]) !=
                config.barrel_usages.begin() + static_cast<std::ptrdiff_t>(index)) {
            set_error(AOAHID_ERR_PARAM, names.barrel_usages,
                      "Each barrel control must be a unique HUT 1.7 Barrel Switch (0x44) or "
                      "Secondary Barrel Switch (0x5A).");
            return AOAHID_ERR_PARAM;
        }
    }
    config.options.barrel_usages = nullptr;
    const BatteryRequest battery{options->enable_battery, options->battery, options->report_id};
    if (!validate_battery(battery,
                          kind == AOAHID_PROFILE_PEN ? kPenBatteryNames : kDigitizerBatteryNames)) {
        return AOAHID_ERR_PARAM;
    }
    return build_generated_spec(
        GeneratedSpecIdentity{kind, kind == AOAHID_PROFILE_PEN ? AOAHID_ANDROID_PORTABLE_CANDIDATE
                                                               : AOAHID_ANDROID_CONDITIONAL},
        config, battery,
        [=](DescriptorBuilder& builder) {
            const std::uint16_t application =
                kind == AOAHID_PROFILE_PEN ? aoa::hid::usage::pen : aoa::hid::usage::digitizer;
            if (!builder.begin_application(aoa::hid::usage::page_digitizers, application) ||
                !builder.set_report_id(options->report_id.enabled == 1U,
                                       options->report_id.value) ||
                !builder.begin_physical(aoa::hid::usage::page_digitizers,
                                        aoa::hid::usage::stylus)) {
                return false;
            }
            // Android common processes Variable fields in descriptor order. In
            // the audited legacy input path, In Range chooses PEN or RUBBER
            // from the Invert state already seen in that report; the Android
            // 17 priority table likewise places Invert before Tip and In Range.
            if (options->enable_eraser == 1U &&
                !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::invert, 0, 1,
                                  1U, false, false, FieldSemantic::eraser, 0U)) {
                return false;
            }
            if (!builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::tip_switch, 0,
                                  1, 1U, false, false, FieldSemantic::tip, 0U) ||
                !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::in_range, 0, 1,
                                  1U, false, false, FieldSemantic::in_range, 0U)) {
                return false;
            }
            for (std::size_t index = 0U; index < options->barrel_usage_count; ++index) {
                if (!builder.variable(aoa::hid::usage::page_digitizers,
                                      options->barrel_usages[index], 0, 1, 1U, false, false,
                                      FieldSemantic::buttons, static_cast<std::uint16_t>(index))) {
                    return false;
                }
            }
            pad_report(builder);
            if (!builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::x,
                                  options->x.logical_minimum, options->x.logical_maximum,
                                  static_cast<std::uint8_t>(options->x.bit_width), false, false,
                                  FieldSemantic::x, 0U, false, &options->x.physical) ||
                !builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::y,
                                  options->y.logical_minimum, options->y.logical_maximum,
                                  static_cast<std::uint8_t>(options->y.bit_width), false, false,
                                  FieldSemantic::y, 0U, false, &options->y.physical)) {
                return false;
            }
            // Each of X, Y, Pressure, Tilt X/Y, and Twist below is an
            // independently caller-declared 1-32 bit field (see LIMITS.md);
            // realigning between them keeps any one of them from starting at
            // a nonzero bit offset and crossing a fifth byte (HID 1.11 8.4).
            pad_report(builder);
            if (options->enable_pressure == 1U &&
                !builder.variable(
                    aoa::hid::usage::page_digitizers, aoa::hid::usage::tip_pressure,
                    options->pressure.logical_minimum, options->pressure.logical_maximum,
                    static_cast<std::uint8_t>(options->pressure.bit_width), false, false,
                    FieldSemantic::pressure, 0U, false, &options->pressure.physical)) {
                return false;
            }
            pad_report(builder);
            if (options->enable_tilt == 1U &&
                (!builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::x_tilt,
                                   options->tilt_x.logical_minimum, options->tilt_x.logical_maximum,
                                   static_cast<std::uint8_t>(options->tilt_x.bit_width), false,
                                   false, FieldSemantic::tilt_x, 0U, false,
                                   &options->tilt_x.physical) ||
                 !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::y_tilt,
                                   options->tilt_y.logical_minimum, options->tilt_y.logical_maximum,
                                   static_cast<std::uint8_t>(options->tilt_y.bit_width), false,
                                   false, FieldSemantic::tilt_y, 0U, false,
                                   &options->tilt_y.physical))) {
                return false;
            }
            pad_report(builder);
            if (options->enable_twist_target_specific == 1U &&
                !builder.variable(aoa::hid::usage::page_digitizers, aoa::hid::usage::twist,
                                  options->twist.logical_minimum, options->twist.logical_maximum,
                                  static_cast<std::uint8_t>(options->twist.bit_width), false, false,
                                  FieldSemantic::twist, 0U, false, &options->twist.physical)) {
                return false;
            }
            pad_report(builder);
            return builder.end_collection() && builder.end_collection();
        },
        out_spec);
}

static aoahid_result aoahid_spec_create_pen_impl(const aoahid_pen_options* options,
                                                 aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)), "pen_options")) {
        return aoa::detail::finish_result(AOAHID_ERR_PARAM, "pen_options",
                                          "The pen options contain an invalid required field.");
    }
    return create_stylus_spec(options, AOAHID_PROFILE_PEN, out_spec);
}

static aoahid_result aoahid_spec_create_digitizer_impl(const aoahid_digitizer_options* options,
                                                       aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)),
                                   "digitizer_options")) {
        return aoa::detail::finish_result(
            AOAHID_ERR_PARAM, "digitizer_options",
            "The digitizer options contain an invalid required field.");
    }
    aoahid_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.reserved = options->reserved;
    pen.report_id = options->report_id;
    pen.x = options->x;
    pen.y = options->y;
    pen.enable_pressure = options->enable_pressure;
    pen.pressure = options->pressure;
    pen.enable_tilt = options->enable_tilt;
    pen.tilt_x = options->tilt_x;
    pen.tilt_y = options->tilt_y;
    pen.enable_twist_target_specific = options->enable_twist_target_specific;
    pen.twist = options->twist;
    pen.barrel_usages = options->barrel_usages;
    pen.barrel_usage_count = options->barrel_usage_count;
    pen.enable_eraser = options->enable_eraser;
    pen.enable_hover = options->enable_hover;
    pen.enable_battery = options->enable_battery;
    pen.battery = options->battery;
    return create_stylus_spec(&pen, AOAHID_PROFILE_DIGITIZER, out_spec);
}

static aoahid_result aoahid_spec_create_raw_impl(const aoahid_raw_options* options,
                                                 aoahid_spec** out_spec) {
    aoa::detail::clear_error();
    if (options == nullptr ||
        !aoa::detail::valid_struct(options, options == nullptr ? 0U : options->struct_size,
                                   static_cast<std::uint32_t>(sizeof(*options)), "raw_options")) {
        return AOAHID_ERR_PARAM;
    }
    if (out_spec == nullptr) {
        set_error(AOAHID_ERR_PARAM, "out_spec", "The output pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *out_spec = nullptr;
    if (options->reserved != 0U || options->acknowledges_no_android_support != 1U ||
        options->requires_output != 0U || options->requires_feature_response != 0U) {
        set_error(AOAHID_ERR_UNSUPPORTED, "raw.acknowledgements",
                  "Raw input requires explicit no-support acknowledgement and no Output or "
                  "Feature-response dependency.");
        return AOAHID_ERR_UNSUPPORTED;
    }
    // validate_raw permits exactly the 256 possible Report IDs. Bound every
    // count-derived allocation before using the caller's count so an invalid
    // raw table cannot turn a deterministic validation error into allocation
    // pressure or std::length_error.
    if (options->reports == nullptr || options->report_count == 0U) {
        set_error(AOAHID_ERR_UNSET_FIELD, "raw.reports",
                  "Raw descriptors require an explicit accepted-report table.");
        return AOAHID_ERR_UNSET_FIELD;
    }
    if (options->report_count > 256U) {
        set_error(AOAHID_ERR_OVERFLOW, "raw.reports",
                  "The accepted-report table exceeds the 256 possible Report IDs.");
        return AOAHID_ERR_OVERFLOW;
    }
    std::vector<std::uint8_t> trailing_valid_masks(options->report_count, std::uint8_t{0xFFU});
    aoa::hid::DescriptorRequirements requirements{};
    const aoa::hid::ValidationIssue validation =
        aoa::hid::validate_raw(options->descriptor, options->descriptor_length, options->reports,
                               options->report_count, trailing_valid_masks.data(), &requirements);
    if (validation.result != AOAHID_OK) {
        set_error(validation.result, validation.field, validation.reason, 0, 0, 0, 0,
                  static_cast<std::uint32_t>(validation.offset), 0);
        return validation.result;
    }
    auto spec = std::make_unique<aoahid_spec>();
    spec->kind = AOAHID_PROFILE_RAW;
    spec->android_status = AOAHID_ANDROID_UNKNOWN;
    spec->requirements = requirements;
    spec->descriptor.assign(options->descriptor, options->descriptor + options->descriptor_length);
    aoa::detail::RawConfig config{};
    config.reports.assign(options->reports, options->reports + options->report_count);
    config.trailing_valid_masks = std::move(trailing_valid_masks);
    for (const aoahid_raw_report& report : config.reports) {
        spec->report_capabilities.push_back(
            aoahid_report_capability{report.report_id, report.wire_length});
    }
    spec->config = std::move(config);
    *out_spec = spec.release();
    aoa::detail::clear_error();
    return AOAHID_OK;
}

static void aoahid_spec_retain_impl(aoahid_spec* spec) {
    aoa::detail::clear_error();
    if (!aoa::detail::retain_spec(spec)) {
        set_error(spec == nullptr ? AOAHID_ERR_PARAM : AOAHID_ERR_OVERFLOW, "spec.retain",
                  spec == nullptr ? "The spec handle is null."
                                  : "The spec reference count cannot be incremented further.");
    }
}

static void aoahid_spec_release_impl(aoahid_spec* spec) {
    aoa::detail::clear_error();
    if (!aoa::detail::release_spec(spec)) {
        set_error(AOAHID_ERR_PARAM, "spec.release",
                  "The spec handle is null or has no owned reference to release.");
    }
}

static aoahid_result aoahid_spec_descriptor_impl(const aoahid_spec* spec,
                                                 const std::uint8_t** bytes, std::size_t* length) {
    aoa::detail::clear_error();
    if (bytes != nullptr) {
        *bytes = nullptr;
    }
    if (length != nullptr) {
        *length = 0U;
    }
    if (spec == nullptr || bytes == nullptr || length == nullptr) {
        set_error(AOAHID_ERR_PARAM, "spec", "A required pointer is null.");
        return AOAHID_ERR_PARAM;
    }
    *bytes = spec->descriptor.data();
    *length = spec->descriptor.size();
    aoa::detail::clear_error();
    return AOAHID_OK;
}

static aoahid_result aoahid_spec_manifest_impl(const aoahid_spec* spec,
                                               aoahid_capability_manifest* manifest) {
    aoa::detail::clear_error();
    if (spec == nullptr || manifest == nullptr || manifest->struct_size != sizeof(*manifest) ||
        manifest->reserved != 0U) {
        set_error(AOAHID_ERR_PARAM, "manifest",
                  "The manifest pointer, struct_size, or reserved field is invalid.");
        return AOAHID_ERR_PARAM;
    }
    manifest->input_supported = 1U;
    manifest->output_supported = 0U;
    manifest->feature_transport_supported = 0U;
    manifest->android_status = spec->android_status;
    manifest->profile_kind = spec->kind;
    manifest->reports = spec->report_capabilities.data();
    manifest->report_count = spec->report_capabilities.size();
    manifest->descriptor_length = spec->descriptor.size();
    aoa::detail::clear_error();
    return AOAHID_OK;
}

aoahid_result AOAHID_CALL aoahid_spec_create_keyboard(const aoahid_keyboard_options* options,
                                                      aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_keyboard",
                            [&] { return aoahid_spec_create_keyboard_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_mouse(const aoahid_mouse_options* options,
                                                   aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_mouse",
                            [&] { return aoahid_spec_create_mouse_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_toggle(const aoahid_toggle_options* options,
                                                    aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_toggle",
                            [&] { return aoahid_spec_create_toggle_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_gamepad(const aoahid_gamepad_options* options,
                                                     aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_gamepad",
                            [&] { return aoahid_spec_create_gamepad_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_touchscreen(const aoahid_touchscreen_options* options,
                                                         aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_touchscreen",
                            [&] { return aoahid_spec_create_touchscreen_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_touchpad(const aoahid_touchpad_options* options,
                                                      aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_touchpad",
                            [&] { return aoahid_spec_create_touchpad_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_pen(const aoahid_pen_options* options,
                                                 aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_pen",
                            [&] { return aoahid_spec_create_pen_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_digitizer(const aoahid_digitizer_options* options,
                                                       aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_digitizer",
                            [&] { return aoahid_spec_create_digitizer_impl(options, out_spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_create_raw(const aoahid_raw_options* options,
                                                 aoahid_spec** out_spec) {
    return abi_spec_factory(out_spec, "spec.create_raw",
                            [&] { return aoahid_spec_create_raw_impl(options, out_spec); });
}

void AOAHID_CALL aoahid_spec_retain(aoahid_spec* spec) {
    abi_void("spec.retain", [&] { aoahid_spec_retain_impl(spec); });
}

void AOAHID_CALL aoahid_spec_release(aoahid_spec* spec) {
    abi_void("spec.release", [&] { aoahid_spec_release_impl(spec); });
}

aoahid_result AOAHID_CALL aoahid_spec_descriptor(const aoahid_spec* spec,
                                                 const std::uint8_t** bytes, std::size_t* length) {
    return abi_result("spec.descriptor",
                      [&] { return aoahid_spec_descriptor_impl(spec, bytes, length); });
}

aoahid_result AOAHID_CALL aoahid_spec_manifest(const aoahid_spec* spec,
                                               aoahid_capability_manifest* manifest) {
    return abi_result("spec.manifest", [&] { return aoahid_spec_manifest_impl(spec, manifest); });
}

} // extern "C"
